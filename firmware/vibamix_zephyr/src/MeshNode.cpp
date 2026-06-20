#include "MeshNode.h"

#include "ConfigMode.h"
#include "app_config.h"
#include "badge_store.h"
#include "event_status.h"
#include "gateway_status.h"
#include "identity.h"
#include "image_xfer.h"
#include "mesh_keys.h"
#include "mesh_model.h"
#include "qr_screen.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

// Singleton so the file-scope C trampolines can reach the instance.
static MeshNode *s_self;

// Reassembly/staging buffer for one image at a time (2-bit gray = 11616 B, 1-bit
// = 5808 B). image_xfer reassembles into this; display reads slots into it too.
static uint8_t s_img_stage[12288];

// ---- deferred content rendering (cold-boot "mesh mode" awake window) ----
//
// During run_awake_window() the MAIN thread owns the ePaper (countdown bar). Mesh
// content commands (show-text / image / display-screen) arrive on the BT RX thread;
// rendering them there races the main thread on the shared panel + BUSY line and
// wedges it (Epaper_READBUSY then spins, so the main loop never re-checks the button
// and a press is ignored until reboot). So while deferral is on, the handlers only
// stage the work + set a flag; run_awake_window() drains it via
// consume_pending_render() on the main thread, keeping the panel single-threaded.
// This mirrors ConfigMode's identical BT-thread-must-not-render deferral.
static atomic_t s_defer_renders;
static atomic_t s_text_pending;
static atomic_t s_image_pending;
static atomic_t s_display_pending;

// Staged show-text (copied out of the mesh reassembly buffer, which is reused).
static char s_ptext_hdr[APP_CFG_HEADER_MAX];
static char s_ptext_body[APP_CFG_BODY_MAX];

// Staged image: a ping-pong of two render buffers (as ConfigMode). on_image's source
// is the live image_xfer reassembly buffer, which a next transfer overwrites, so the
// pixels must be copied out; the BT thread fills whichever buffer the main thread
// isn't rendering. Not reusing s_img_stage avoids clobbering an unrendered frame.
struct pending_img {
    uint8_t  slot, fmt;
    uint16_t w, h;
    uint32_t len;
};
static uint8_t            s_mesh_render_buf[2][BADGE_IMG_GRAY2_BYTES];
static struct pending_img s_pimg[2];
static volatile int8_t    s_next_buf;            // BT: buffer to fill next
static volatile int8_t    s_rendering_buf = -1;  // main: buffer in use (-1 = idle)
static volatile int8_t    s_pending_buf;         // buffer holding the pending image

// Staged display-screen: kind/idx only — on_display_screen reads stored content from
// flash/app_config on the main thread, so no pixel copy is needed.
static uint8_t s_pd_kind, s_pd_idx;

// Per-device provisioning UUID, derived from the FICR device id.
static uint8_t s_dev_uuid[16];
static const struct bt_mesh_prov prov = {
    .uuid = s_dev_uuid,
};

extern "C" {

static void cb_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                     uint16_t w, uint16_t h)
{
    if (s_self) { s_self->on_image(slot, fmt, buf, len, w, h); }
}

static void cb_heartbeat(const char *name, size_t len)
{
    event_status_note_heartbeat(name, len);   // latch event name; wake mesh-mode loop
    config_mode_on_heartbeat();               // keep a config-mode badge awake too
}

static void cb_show_led(uint8_t anim, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_self) { s_self->on_show_led(anim, r, g, b); }
    config_mode_on_heartbeat();   // LEDs only — keep awake, no screen takeover
}

static void cb_show_text(const char *title, size_t tlen, const char *body, size_t blen)
{
    // on_show_text decides whether to render (and signal config_mode_on_content); a
    // gateway badge notes the relay but stays on its gateway screen.
    if (s_self) { s_self->on_show_text(title, tlen, body, blen); }
}

static void cb_display_screen(uint8_t kind, uint8_t idx)
{
    // Mesh broadcast "show stored frame N". A gateway badge receives its own
    // re-originated broadcast back via mesh loopback — it must NOT render it (it
    // stays on the Mesh Gateway screen) and just notes the relay, exactly like
    // cb_show_text. Remote badges render the stored frame (the shared handler also
    // persists the displayed-frame selection) and stay awake.
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_DISPLAY);
        return;
    }
    if (s_self) { s_self->on_display_screen(kind, idx); }
    config_mode_on_content();
}

static void cb_set_brightness(uint8_t level)
{
    if (s_self) { s_self->on_set_brightness(level); }
    config_mode_on_heartbeat();   // LEDs only — keep awake, no screen takeover
}

static void cb_set_config_mode(uint8_t on)
{
    if (s_self) { s_self->on_set_config_mode(on); }
}

static void cb_release_brightness(void)
{
    if (s_self) { s_self->on_release_brightness(); }
    config_mode_on_heartbeat();   // LEDs only — keep awake, no screen takeover
}

} // extern "C"

static const struct mesh_config_handlers s_handlers = {
    .heartbeat = cb_heartbeat,
    .show_led  = cb_show_led,
    .show_text = cb_show_text,
    .display_screen = cb_display_screen,
    .set_brightness = cb_set_brightness,
    .release_brightness = cb_release_brightness,
    .set_config_mode = cb_set_config_mode,
};

int MeshNode::init(GUI *gui, LEDStrip *leds)
{
    m_gui = gui;
    m_leds = leds;
    s_self = this;

    mesh_model_set_config_handlers(&s_handlers);
    image_xfer_init(s_img_stage, sizeof(s_img_stage));
    image_xfer_set_complete_cb(cb_image);

    badge_store_init();  // open the raw images flash partition
    app_config_init();   // register the settings handler before settings_load()

    int err = bt_enable(nullptr);
    if (err) {
        printk("Bluetooth init failed (err %d)\n", err);
        return err;
    }

    err = bt_mesh_init(&prov, mesh_model_comp());
    if (err) {
        printk("Mesh init failed (err %d)\n", err);
        return err;
    }

    // Restores persisted app config and (with CONFIG_BT_SETTINGS) mesh seq/RPL.
    settings_load();

    // Per-device GAP name "vibamix-XXXX" so a phone (Web Bluetooth) can pick this
    // exact badge out of the chooser. The mesh proxy advertising carries it in its
    // scan response (CONFIG_BT_MESH_PROXY_USE_DEVICE_NAME).
    char code[5];
    app_identity_code(code);
    char name[16];
    snprintf(name, sizeof(name), "vibamix-%s", code);
    bt_set_name(name);

    // Deterministic identity: unicast address from the FICR device id (the
    // provisioning UUID also uses that id).
    hwinfo_get_device_id(s_dev_uuid, sizeof(s_dev_uuid));
    uint16_t addr = app_identity_addr();

    static const uint8_t app_key[16] = VIBAMIX_APP_KEY;

    if (!bt_mesh_is_provisioned()) {
        static const uint8_t net_key[16] = VIBAMIX_NET_KEY;
        static const uint8_t dev_key[16] = VIBAMIX_DEV_KEY;

        err = bt_mesh_provision(net_key, VIBAMIX_NET_IDX, 0 /*flags*/, 0 /*iv*/,
                                addr, dev_key);
        if (err) {
            printk("Self-provision failed (err %d)\n", err);
            return err;
        }
        printk("Self-provisioned at unicast 0x%04x\n", addr);
    } else {
        printk("Already provisioned (restored from settings)\n");
    }

    // Ensure the shared app key is bound — every boot, not just on first
    // provision. A node can be "provisioned" in persisted settings yet be
    // missing the AppKey record (e.g. the original add failed, or settings were
    // partially written), which leaves the gateway TX path failing forever with
    // "Unknown AppKey 0x000" (-EINVAL). bt_mesh_app_key_add() is idempotent: it
    // returns STATUS_IDX_ALREADY_STORED for a matching key.
    if (!bt_mesh_app_key_exists(VIBAMIX_APP_IDX)) {
        uint8_t st = bt_mesh_app_key_add(VIBAMIX_APP_IDX, VIBAMIX_NET_IDX, app_key);
        if (st) {
            printk("App key add failed (status 0x%02x)\n", st);
        } else {
            printk("App key 0x%03x added\n", VIBAMIX_APP_IDX);
        }
    }

    // Stand in for the Config Client: bind the app key and join the group.
    mesh_model_bind_and_subscribe(VIBAMIX_APP_IDX, VIBAMIX_GROUP_ADDR);

    // No SIG GATT proxy / PB-GATT: the badge is connectable only in config mode
    // (button). Mesh messages travel over the advertising bearer; a config-mode
    // badge gateways injects via mesh_model_send_to_group() (see on_mesh_tx).

    return 0;
}

void MeshNode::apply_persisted_config()
{
    // The identity frame is home: apply its LED (animation + color) on boot.
    apply_frame_led(APP_DISP_KIND_IDENTITY, 0);

    // Single boot-display authority. The panel was left awake by GUI::init().
    if (!m_gui) {
        return;
    }
    // Even with nothing configured yet, rest on a clean identity frame (cleared
    // white background + banner, which falls back to "vibamix" when the name is
    // empty) rather than a "Hello World" placeholder.
    redraw_identity();
}

void MeshNode::radio_suspend()
{
    // Park the mesh network first (stop scanning / relay / Secure Network Beacon TX),
    // then disable the controller so MPSL releases the radio + clocks. Without this the
    // radio keeps cycling and sys_poweroff() can't reach true System OFF.
    int err = bt_mesh_suspend();
    if (err && err != -EALREADY) {
        printk("bt_mesh_suspend failed (%d)\n", err);
    }
    err = bt_disable();
    if (err) {
        printk("bt_disable failed (%d)\n", err);
    }
}

void MeshNode::radio_resume()
{
    // Mirror init()'s order: bring the controller back up, then resume mesh activity.
    int err = bt_enable(nullptr);
    if (err) {
        printk("bt_enable (resume) failed (%d)\n", err);
        return;
    }
    err = bt_mesh_resume();
    if (err && err != -EALREADY) {
        printk("bt_mesh_resume failed (%d)\n", err);
    }
}

void MeshNode::redraw_identity(bool sleeping)
{
    if (!m_gui) {
        return;
    }
    const struct app_config *cfg = app_config_get();

    // Optional identity image (gray2) drawn below the name/ID banner.
    const uint8_t *img = nullptr;
    uint8_t  fmt = 0;
    size_t   len = 0;
    uint16_t w = 0, h = 0;
    if (badge_store_image_read(BADGE_SLOT_IDENTITY, s_img_stage, sizeof(s_img_stage),
                               &fmt, &len, &w, &h) == 0) {
        img = s_img_stage;
    }

    const char *table = cfg->has_attendee ? cfg->attendee_id : "";

#if VIBAMIX_EPD_4GRAY
    const bool use_gray = (img && fmt == BADGE_FMT_GRAY2 && w > 0 && h > 0);
#else
    const bool use_gray = false;
#endif
    m_current_gray = use_gray;
    m_current_is_identity = true;

    m_gui->wake();
    if (use_gray) {
        // Full-screen 4-gray identity: bake the opaque banner (+ asleep moon) into the
        // gray2 source, then render it in one 4-gray pass. (A 1-bit overlay/partial
        // can't run over a 4-gray base — it re-drives every mid-gray pixel — so the
        // banner is composited into the image, and the awake window holds it static.)
        identity_bake_overlays(s_img_stage, w, h, m_gui->framebuffer(),
                               cfg->name, table, sleeping);
        m_gui->render_identity_gray(s_img_stage, w, h);
    } else if (img && fmt == BADGE_FMT_BW && len == m_gui->framebuffer_size()) {
        // Full-screen B/W identity image + banner. A B/W image is already in panel-
        // framebuffer layout, so blit it straight in (like render_image), then draw the
        // banner over its bottom. 1-bit throughout -> the awake window keeps the live
        // countdown (m_current_gray stays false).
        memcpy(m_gui->framebuffer(), img, len);
        identity_banner_over(*m_gui, cfg->name, table);
        if (sleeping) {
            identity_sleep_overlay(*m_gui);
        }
        m_gui->set_base_map();
    } else {
        identity_screen_draw(*m_gui, cfg->name, table, img, fmt, w, h);
        if (sleeping) {
            identity_sleep_overlay(*m_gui);   // moon + z z z, so the panel rests "asleep"
        }
        m_gui->set_base_map();   // push the framebuffer to the panel (full refresh)
    }
    m_gui->sleep();
}

void MeshNode::apply_frame_led(uint8_t kind, uint8_t idx)
{
    struct frame_led led;

    // anim 0 (Off) means "no per-frame override" — leave the default LED.
    if (m_leds && app_config_get_frame_led(kind, idx, &led) &&
        led.anim != (uint8_t)LedPattern::Off) {
        m_leds->set_anim(led_pattern_from_code(led.anim), led.r, led.g, led.b);
    }
}

void MeshNode::on_set_name(const char *name, size_t len)
{
    app_config_set_name(name, len);
    // While a phone is connected, just store — don't churn the panel (the Connected
    // screen / shown frame stays). The next resting redraw picks up the new name.
    if (gateway_status_active()) {
        return;
    }
    redraw_identity();
}

void MeshNode::on_show_led(uint8_t anim, uint8_t r, uint8_t g, uint8_t b)
{
    // Mesh broadcast: override the live LEDs on every badge. Not stored — the
    // next frame display (or a reboot) restores that frame's own LED config.
    if (m_leds) {
        m_leds->set_anim(led_pattern_from_code(anim), r, g, b);
    }
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_LED);
    }
}

void MeshNode::on_set_brightness(uint8_t level)
{
    // Mesh broadcast: pin LED brightness on every badge for the current LED state,
    // overriding the ALS auto-adjust. Cleared by the next LED state change.
    if (m_leds) {
        m_leds->set_brightness_override(level);
    }
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_BRIGHTNESS);
    }
}

void MeshNode::on_release_brightness()
{
    // Mesh broadcast: hand LED brightness back to the ALS auto-adjust on every badge
    // without disturbing the current animation/color.
    if (m_leds) {
        m_leds->clear_brightness_override();
    }
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_BRIGHTNESS);
    }
}

void MeshNode::on_set_config_mode(uint8_t on)
{
    // A gateway badge re-originates the broadcast to the fleet and receives it back
    // via mesh loopback. Don't act on it locally — that would kick the operator out
    // of (or churn) their own config-mode session. Just note the relay for stats.
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_CONFIG);
        return;
    }
    if (on) {
        // Best-effort: only takes effect while the badge is awake (run_awake_window
        // consumes this). A System OFF badge's radio is off and never sees it.
        app_request_config_mode();
    } else {
        // No-op unless currently in config mode (reuses the button-exit path).
        config_mode_request_exit();
    }
}

void MeshNode::on_show_text(const char *title, size_t tlen, const char *body, size_t blen)
{
    ARG_UNUSED(tlen);
    ARG_UNUSED(blen);
    if (!m_gui) {
        return;
    }
    // Mesh broadcast (always a fleet "show text" — not stored). A gateway badge only
    // notes the relay for its stats and stays on the Mesh Gateway screen; it does not
    // render the broadcast it's sending to the fleet.
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_SCREEN);
        return;
    }
    if (atomic_get(&s_defer_renders)) {
        // Awake window: stage for the main thread (see deferral note above).
        strncpy(s_ptext_hdr, title ? title : "", sizeof(s_ptext_hdr) - 1);
        s_ptext_hdr[sizeof(s_ptext_hdr) - 1] = '\0';
        strncpy(s_ptext_body, body ? body : "", sizeof(s_ptext_body) - 1);
        s_ptext_body[sizeof(s_ptext_body) - 1] = '\0';
        atomic_set(&s_text_pending, 1);
        return;
    }
    do_show_text(title, body);
}

void MeshNode::do_show_text(const char *title, const char *body)
{
    m_current_gray = false;   // text is 1-bit; awake window can tick a live bar over it
    m_current_is_identity = false;
    m_gui->wake();
    m_gui->show_text(title, body);
    m_gui->sleep();
    config_mode_on_content();   // content took over the panel; keep awake (no-op outside config)
}

void MeshNode::on_mesh_tx(const uint8_t *access, size_t len)
{
    // Config-mode gateway: forward the app's vendor-model access payload onto the
    // mesh group. Best-effort (ENOBUFS under heavy chunking is acceptable).
    int err = mesh_model_send_to_group(access, len);
    if (err) {
        printk("mesh tx failed (%d)\n", err);
    }
}

void MeshNode::on_set_attendee_id(const char *id, size_t len)
{
    app_config_set_attendee_id(id, len);
    if (gateway_status_active()) {
        return;   // connected: store only, don't churn the panel
    }
    redraw_identity();   // table ID lives on the identity frame
}

void MeshNode::on_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                                uint8_t r, uint8_t g, uint8_t b)
{
    app_config_set_frame_led(kind, idx, anim, r, g, b);

    // If this is the frame currently on screen, apply it live. The identity frame
    // is the default, so treat "no explicit display yet" as showing identity.
    const struct app_config *cfg = app_config_get();
    bool shown;
    if (kind == APP_DISP_KIND_IDENTITY) {
        shown = !cfg->has_disp || cfg->disp_kind == APP_DISP_KIND_IDENTITY;
    } else {
        shown = cfg->has_disp && cfg->disp_kind == kind && cfg->disp_idx == idx;
    }
    if (shown) {
        apply_frame_led(kind, idx);
    }
}

void MeshNode::on_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                        uint16_t w, uint16_t h)
{
    if (atomic_get(&s_defer_renders)) {
        // Awake window: copy the pixels into the idle ping-pong buffer + stage for
        // the main thread (the source is the live reassembly buffer; see above).
        if (len > BADGE_IMG_GRAY2_BYTES) {
            return;   // larger than a panel frame; shouldn't happen, drop it
        }
        int idx = s_next_buf;
        if (idx == s_rendering_buf) {
            idx ^= 1;
        }
        memcpy(s_mesh_render_buf[idx], buf, len);
        s_pimg[idx].slot = slot;
        s_pimg[idx].fmt  = fmt;
        s_pimg[idx].w    = w;
        s_pimg[idx].h    = h;
        s_pimg[idx].len  = (uint32_t)len;
        s_pending_buf = idx;
        s_next_buf    = idx ^ 1;
        atomic_set(&s_image_pending, 1);
        return;
    }
    do_image(slot, fmt, buf, len, w, h);
}

void MeshNode::do_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                        uint16_t w, uint16_t h)
{
    // Identity image: store it, then recompose the identity frame (banner + image)
    // rather than blitting it full-screen.
    if (slot == BADGE_SLOT_IDENTITY) {
        uint32_t crc = crc32_ieee(buf, len);
        badge_store_image_write(BADGE_SLOT_IDENTITY, fmt, buf, len, w, h, crc);
        redraw_identity();
        apply_frame_led(APP_DISP_KIND_IDENTITY, 0);
        return;
    }
    if (slot < BADGE_IMAGE_SLOTS) {
        uint32_t crc = crc32_ieee(buf, len);
        badge_store_image_write(slot, fmt, buf, len, w, h, crc);
        app_config_set_display(APP_DISP_KIND_IMAGE, slot);
    }
    m_current_gray = (fmt == BADGE_FMT_GRAY2);
    m_current_is_identity = false;
    if (m_gui) {
        m_gui->wake();
        if (fmt == BADGE_FMT_GRAY2) {
            m_gui->render_gray2(buf, w, h);
        } else {
            m_gui->render_image(buf, len);
        }
        m_gui->sleep();
    }
    app_config_set_has_image(true);
    if (slot < BADGE_IMAGE_SLOTS) {
        apply_frame_led(APP_DISP_KIND_IMAGE, slot);
    }
}

void MeshNode::on_set_screen(uint8_t idx, const char *hdr, size_t hlen,
                             const char *body, size_t blen)
{
    app_config_set_screen(idx, hdr, hlen, body, blen);
}

void MeshNode::on_display_screen(uint8_t kind, uint8_t idx)
{
    if (!m_gui) {
        return;
    }

    // Force-display over GATT: take over the panel (stop the connected-screen
    // repaint) and keep the "Connected" overlay on the shown frame. No gateway
    // "Broadcast" note — this is a direct command, not a relayed mesh broadcast.
    if (gateway_status_active()) {
        config_mode_on_content();
    }

    if (atomic_get(&s_defer_renders)) {
        // Awake window: stage for the main thread (stored content read there).
        s_pd_kind = kind;
        s_pd_idx  = idx;
        atomic_set(&s_display_pending, 1);
        return;
    }
    do_display_screen(kind, idx);
}

void MeshNode::do_display_screen(uint8_t kind, uint8_t idx)
{
    if (kind == APP_DISP_KIND_IDENTITY) {
        redraw_identity();
        app_config_set_display(APP_DISP_KIND_IDENTITY, 0);
        apply_frame_led(APP_DISP_KIND_IDENTITY, 0);
        return;
    }

    if (kind == APP_DISP_KIND_TEXT) {
        const struct badge_screen *scr = app_config_get_screen(idx);
        if (!scr) {
            printk("display: text screen %u empty\n", idx);
            return;
        }
        m_current_gray = false;   // text is 1-bit; awake window ticks a live bar over it
        m_current_is_identity = false;
        m_gui->wake();
        m_gui->show_text(scr->header, scr->body);
        m_gui->sleep();
        app_config_set_display(kind, idx);
        app_config_set_has_image(false);
        apply_frame_led(kind, idx);
    } else if (kind == APP_DISP_KIND_IMAGE) {
        uint8_t fmt;
        size_t len;
        uint16_t w, h;

        if (badge_store_image_read(idx, s_img_stage, sizeof(s_img_stage),
                                   &fmt, &len, &w, &h) != 0) {
            // Empty slot: clear the bistable panel to blank white rather than
            // leaving the previously-shown image frozen on it.
            printk("display: image slot %u empty - clearing panel\n", idx);
            m_current_gray = false;   // blank is 1-bit
            m_current_is_identity = false;
            m_gui->wake();
            m_gui->show_blank();
            m_gui->sleep();
            app_config_set_has_image(false);
            return;
        }
        m_current_gray = (fmt == BADGE_FMT_GRAY2);
        m_current_is_identity = false;
        m_gui->wake();
        if (fmt == BADGE_FMT_GRAY2) {
            m_gui->render_gray2(s_img_stage, w, h);
        } else {
            m_gui->render_image(s_img_stage, len);
        }
        m_gui->sleep();
        app_config_set_display(kind, idx);
        app_config_set_has_image(true);
        apply_frame_led(kind, idx);
    }
}

void MeshNode::set_defer_renders(bool on)
{
    atomic_set(&s_defer_renders, on ? 1 : 0);
    if (!on) {
        // Leaving the awake window: drop any work staged after the last drain so a
        // stale frame can't render later (e.g. once config mode owns the panel).
        atomic_clear(&s_text_pending);
        atomic_clear(&s_image_pending);
        atomic_clear(&s_display_pending);
    }
}

bool MeshNode::consume_pending_render()
{
    bool rendered = false;
    if (atomic_cas(&s_text_pending, 1, 0)) {
        do_show_text(s_ptext_hdr, s_ptext_body);
        rendered = true;
    }
    if (atomic_cas(&s_image_pending, 1, 0)) {
        int idx = s_pending_buf;
        s_rendering_buf = idx;   // tells the BT thread to fill the other buffer
        const struct pending_img p = s_pimg[idx];
        do_image(p.slot, p.fmt, s_mesh_render_buf[idx], p.len, p.w, p.h);
        s_rendering_buf = -1;
        rendered = true;
    }
    if (atomic_cas(&s_display_pending, 1, 0)) {
        do_display_screen(s_pd_kind, s_pd_idx);
        rendered = true;
    }
    return rendered;
}
