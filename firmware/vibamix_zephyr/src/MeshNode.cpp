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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

// Singleton so the file-scope C trampolines can reach the instance.
static MeshNode *s_self;

// Reassembly/staging buffer for one image at a time (2-bit gray = 11616 B, 1-bit
// = 5808 B). image_xfer reassembles into this; display reads slots into it too.
static uint8_t s_img_stage[12288];

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
    if (s_self) { s_self->on_show_text(title, tlen, body, blen); }
    config_mode_on_content();     // a text frame took over the panel; keep awake
}

} // extern "C"

static const struct mesh_config_handlers s_handlers = {
    .heartbeat = cb_heartbeat,
    .show_led  = cb_show_led,
    .show_text = cb_show_text,
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

    if (!bt_mesh_is_provisioned()) {
        static const uint8_t net_key[16] = VIBAMIX_NET_KEY;
        static const uint8_t dev_key[16] = VIBAMIX_DEV_KEY;
        static const uint8_t app_key[16] = VIBAMIX_APP_KEY;

        err = bt_mesh_provision(net_key, VIBAMIX_NET_IDX, 0 /*flags*/, 0 /*iv*/,
                                addr, dev_key);
        if (err) {
            printk("Self-provision failed (err %d)\n", err);
            return err;
        }
        bt_mesh_app_key_add(VIBAMIX_APP_IDX, VIBAMIX_NET_IDX, app_key);
        printk("Self-provisioned at unicast 0x%04x\n", addr);
    } else {
        printk("Already provisioned (restored from settings)\n");
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
    const struct app_config *cfg = app_config_get();

    // The identity frame is home: apply its LED (animation + color) on boot.
    apply_frame_led(APP_DISP_KIND_IDENTITY, 0);

    // Single boot-display authority. The panel was left awake by GUI::init().
    if (!m_gui) {
        return;
    }
    if (cfg->has_name) {
        redraw_identity();
    } else {
        // First boot, nothing configured yet.
        m_gui->show_hello_world();
        m_gui->sleep();
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
    m_identity_gray = use_gray;

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
        // countdown (m_identity_gray stays false).
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

void MeshNode::on_show_text(const char *title, size_t tlen, const char *body, size_t blen)
{
    ARG_UNUSED(tlen);
    ARG_UNUSED(blen);
    if (!m_gui) {
        return;
    }
    // Mesh broadcast: draw the text frame straight to the panel. Not stored, so a
    // reboot returns to the identity frame. Mirror on_display_screen's gateway
    // handling so a connected gateway overlays its relay banner and stays awake.
    if (gateway_status_active()) {
        gateway_status_note(GW_CMD_SCREEN);
    }
    m_gui->wake();
    m_gui->show_text(title, body);
    m_gui->sleep();
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
            printk("display: image slot %u empty\n", idx);
            return;
        }
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
