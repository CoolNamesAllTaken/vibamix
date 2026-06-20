#include "ConfigMode.h"

#include "MeshNode.h"
#include "app_config.h"
#include "badge_store.h"
#include "ambient_light_sensor.h"
#include "battery.h"
#include "config_gatt.h"
#include "gateway_status.h"
#include "identity.h"
#include "image_xfer.h"
#include "qr_screen.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

// Prefix of the deep link the QR encodes; the badge code is appended to form
// "vibamix://connect?name=vibamix-XXXX", which the native vibamix app opens and
// uses to connect to this badge by its GAP name. Keep it short so the QR stays a
// low version (see qr_screen.cpp).
#define VIBAMIX_CONFIG_QR_PREFIX "vibamix://connect?name=vibamix-"

// How long to stay awake with no activity before powering off.
static constexpr int64_t kConfigWindowMs = 600 * 1000;

// How often to do a full refresh (clears partial-refresh ghosting), in countdown ticks.
static constexpr int kFullRefreshEvery = 30;

static ConfigMode      *s_self;
static volatile int64_t s_last_activity;
static volatile bool    s_exit;
static volatile bool    s_content_shown; // an image/name took over the screen
static struct bt_conn  *s_conn; // most-recent connection, for a clean teardown
static atomic_t         s_conn_count; // active connections (BT thread inc/dec)

// Set from the mesh-RX (BT) thread; consumed on the main thread so only the main
// thread ever writes the 64-bit s_last_activity (no torn write on the 32-bit M33).
static atomic_t s_heartbeat_pending;

// Per-connection keepalive: the laptop writes f0de000B once a second (app->badge
// liveness). cb_keepalive (BT thread) just sets the flag; the main loop records the
// time (so the 64-bit timestamp is only written on the main thread).
static atomic_t s_keepalive_pending;
static int64_t  s_last_keepalive_rx;

// Deferred content render. The GATT/mesh completion callbacks run on the BT RX
// thread; doing the flash write + multi-second ePaper full refresh there blocks the
// link and the central drops the connection. So the callbacks just record the work
// (atomically) and the main loop in run() performs it on the main thread.
//
// Images are copied into a ping-pong of two render buffers: the END write is acked
// immediately, so a *next* upload can begin (overwriting the staging buffer) and
// even complete while the previous image is still rendering. The BT thread fills
// whichever buffer the main thread isn't currently rendering, so there's no shared
// mutable state between them during the (slow) render.
struct pending_img {
    uint8_t  slot, fmt;
    uint16_t w, h;
    uint32_t len;
};
static uint8_t            s_render_buf[2][BADGE_IMG_GRAY2_BYTES];
static struct pending_img s_pimg[2];
static volatile int8_t    s_next_buf;          // BT: buffer to fill next
static volatile int8_t    s_rendering_buf = -1; // main: buffer in use (-1 = idle)
static volatile int8_t    s_pending_buf;        // buffer holding the pending image
static atomic_t           s_image_pending;
static atomic_t           s_display_pending;
static uint8_t            s_pd_kind, s_pd_idx;

// Deferred OTA progress. The f0de0009 DATA writes land on the BT thread once per
// chunk; cb_ota_progress just records the running byte counts (32-bit atomics are
// torn-free on the M33) and sets a flag, and the main loop redraws the progress bar
// — only when the integer percentage changes, so the slow ePaper refresh runs at
// most ~100 times across the whole transfer regardless of chunk count.
static atomic_t           s_ota_progress_pending;
static atomic_t           s_ota_written;
static atomic_t           s_ota_total;

// Dedicated fast connectable advertiser, run only during config mode so a laptop
// or phone finds the badge instantly. The mesh proxy's own advertising is slow
// (~1.9 s Network-ID) and carries the name only in its scan response; this advert
// puts the config service UUID in the primary AD + the name in the scan response.
//
// LEGACY advertising (not extended): macOS/CoreBluetooth (and iOS/Web Bluetooth)
// connect unreliably to connectable *extended* advertisers — the connection
// handshake times out and can wedge the host controller. A legacy connectable +
// scannable advertiser connects reliably. The 128-bit UUID (18 B) + flags (3 B)
// fit the 31-byte legacy AD; the name rides in the scan response. Coexists with
// the mesh stack's adv sets (CONFIG_BT_EXT_ADV_MAX_ADV_SET=5 leaves room).
static struct bt_le_ext_adv *s_adv;
static volatile bool         s_adv_on;
static bool                  s_adv_give_up;

static const struct bt_le_adv_param k_fast_adv_param = {
    .id = BT_ID_DEFAULT,
    .sid = 1,  // ignored for legacy advertising
    .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_SCANNABLE,
    .interval_min = 0x00A0,  // 100 ms
    .interval_max = 0x00F0,  // 150 ms
};

static void start_fast_adv(void)
{
    if (!s_adv && !s_adv_give_up) {
        if (bt_le_ext_adv_create(&k_fast_adv_param, NULL, &s_adv) != 0) {
            printk("config: fast adv create failed\n");
            s_adv_give_up = true;
        } else {
            // Build the scan-response name locally from the config code instead of
            // bt_get_name(): it is guaranteed short and null-terminated, so the legacy
            // scan response always fits. bt_get_name() has been observed returning an
            // over-long (unterminated) buffer here, which made bt_le_ext_adv_set_data
            // fail with "adv or scan rsp data too large" and left the badge silent.
            char code[5];
            app_identity_code(code);
            char advname[20];
            int nlen = snprintf(advname, sizeof(advname), "vibamix-%s", code);
            if (nlen < 0) {
                nlen = 0;
            } else if (nlen > (int)sizeof(advname) - 1) {
                nlen = sizeof(advname) - 1;   // snprintf truncated — clamp to what fits
            }
            struct bt_data ad[] = {
                BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
                BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                    BT_UUID_128_ENCODE(0xf0de0001, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)),
            };
            struct bt_data sd[] = {
                BT_DATA(BT_DATA_NAME_COMPLETE, advname, nlen),
            };
            if (bt_le_ext_adv_set_data(s_adv, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd)) != 0) {
                printk("config: fast adv set_data failed\n");
            }
        }
    }
    if (s_adv && !s_adv_on &&
        bt_le_ext_adv_start(s_adv, BT_LE_EXT_ADV_START_DEFAULT) == 0) {
        s_adv_on = true;
    }
}

static void stop_fast_adv(void)
{
    if (s_adv) {
        bt_le_ext_adv_stop(s_adv);
        bt_le_ext_adv_delete(s_adv);
        s_adv = nullptr;
    }
    s_adv_on = false;
}

extern "C" {

static void cb_name(const char *s, size_t len)
{
    if (s_self) { s_self->on_name(s, len); }
}

static void cb_activity(void)
{
    if (s_self) { s_self->note_activity(); }
}

static void cb_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                     uint16_t w, uint16_t h)
{
    if (s_self) { s_self->on_content_image(slot, fmt, buf, len, w, h); }
}

static void cb_screen(uint8_t idx, const char *hdr, size_t hlen,
                      const char *body, size_t blen)
{
    if (s_self) { s_self->on_screen(idx, hdr, hlen, body, blen); }
}

static void cb_display(uint8_t kind, uint8_t idx)
{
    if (s_self) { s_self->on_display(kind, idx); }
}

static void cb_ota_start(uint32_t total)
{
    if (s_self) { s_self->on_ota_start(total); }
}

static void cb_ota_progress(uint32_t written, uint32_t total)
{
    // BT thread: just record the counts; the main loop redraws the bar.
    atomic_set(&s_ota_written, (atomic_val_t)written);
    atomic_set(&s_ota_total, (atomic_val_t)total);
    atomic_set(&s_ota_progress_pending, 1);
}

static void cb_ota_end(bool ok)
{
    if (s_self) { s_self->on_ota_end(ok); }
}

static void cb_attendee(const char *s, size_t len)
{
    if (s_self) { s_self->on_attendee(s, len); }
}

static void cb_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                         uint8_t r, uint8_t g, uint8_t b)
{
    if (s_self) { s_self->on_frame_led(kind, idx, anim, r, g, b); }
}

static void cb_mesh_tx(const uint8_t *access, size_t len)
{
    if (s_self) { s_self->on_mesh_tx(access, len); }
}

static void cb_keepalive(uint8_t code)
{
    ARG_UNUSED(code);
    atomic_set(&s_keepalive_pending, 1);
}

void config_mode_on_button(void)
{
    if (s_self) { s_self->request_exit(); }
}

void config_mode_request_exit(void)
{
    // Same effect as the button-exit path; no-op when config mode isn't running.
    if (s_self) { s_self->request_exit(); }
}

void config_mode_on_heartbeat(void)
{
    atomic_set(&s_heartbeat_pending, 1);
}

void config_mode_on_content(void)
{
    atomic_set(&s_heartbeat_pending, 1);
    s_content_shown = true;
}

} // extern "C"

static const struct config_gatt_callbacks s_gatt_cb = {
    .on_name      = cb_name,
    .on_activity  = cb_activity,
    .on_screen    = cb_screen,
    .on_display   = cb_display,
    .on_attendee  = cb_attendee,
    .on_frame_led = cb_frame_led,
    .on_mesh_tx   = cb_mesh_tx,
    .on_ota_start = cb_ota_start,
    .on_ota_progress = cb_ota_progress,
    .on_ota_end   = cb_ota_end,
    .on_keepalive = cb_keepalive,
};

static void conn_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        return;
    }
    atomic_inc(&s_conn_count);
    if (!s_conn) {
        s_conn = bt_conn_ref(conn);
    }
    s_adv_on = false;  // the controller stops our adv set on connection
    if (s_self) { s_self->note_activity(); }
    printk("config: peer connected\n");
}

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    atomic_dec(&s_conn_count);
    if (s_conn == conn) {
        bt_conn_unref(s_conn);
        s_conn = nullptr;
    }
    if (s_self) { s_self->note_activity(); }
    printk("config: peer disconnected (0x%02x)\n", reason);
}

BT_CONN_CB_DEFINE(config_conn_cb) = {
    .connected    = conn_connected,
    .disconnected = conn_disconnected,
};

void ConfigMode::note_activity()
{
    s_last_activity = k_uptime_get();
}

void ConfigMode::request_exit()
{
    s_exit = true;
}

void ConfigMode::on_name(const char *s, size_t len)
{
    // Reuse the mesh path: persist the name + redraw the identity screen, which
    // takes over the panel (stop the countdown from repainting over it).
    if (m_mesh) {
        m_mesh->on_set_name(s, len);
    }
    s_content_shown = true;
}

void ConfigMode::on_content_image(uint8_t slot, uint8_t fmt, const uint8_t *buf,
                                  size_t len, uint16_t w, uint16_t h)
{
    // A render-only image broadcast to the whole fleet (slot NONE): if this badge is a
    // mesh gateway, just note the relay for its stats and stay on the Mesh Gateway
    // screen — don't take over the panel with the content it's broadcasting. (Stored
    // slots 0-3 / identity are direct per-badge config and still render below.)
    if (gateway_status_active() && slot == BADGE_SLOT_NONE) {
        gateway_status_note(GW_CMD_IMAGE);
        return;
    }

    // Runs on the BT RX thread: record the work and let run() do the store+render
    // on the main thread (rendering here would block the link and drop the
    // connection). Copy into the buffer the main thread isn't rendering, so a
    // fast-acked next upload can't clobber an in-flight render.
    int idx = s_next_buf;
    if (idx == s_rendering_buf) {
        idx ^= 1;
    }
    if (len > sizeof(s_render_buf[idx])) {
        return;
    }
    memcpy(s_render_buf[idx], buf, len);
    s_pimg[idx].slot = slot;
    s_pimg[idx].fmt = fmt;
    s_pimg[idx].w = w;
    s_pimg[idx].h = h;
    s_pimg[idx].len = (uint32_t)len;
    s_pending_buf = idx;
    s_next_buf = idx ^ 1;
    s_content_shown = true;
    atomic_set(&s_image_pending, 1);
}

void ConfigMode::on_screen(uint8_t idx, const char *hdr, size_t hlen,
                           const char *body, size_t blen)
{
    if (m_mesh) {
        m_mesh->on_set_screen(idx, hdr, hlen, body, blen);
    }
    note_activity();
}

void ConfigMode::on_display(uint8_t kind, uint8_t idx)
{
    // Defer the render to the main loop (see on_content_image) — it reads stored
    // content from flash/settings, so no buffer copy is needed.
    s_pd_kind = kind;
    s_pd_idx = idx;
    s_content_shown = true;
    atomic_set(&s_display_pending, 1);
}

void ConfigMode::on_ota_start(uint32_t total)
{
    ARG_UNUSED(total);
    // Take over the panel so the countdown stops repainting, and warn the user.
    // Paint the 0% progress frame and establish it as the partial-refresh base; the
    // main loop then advances the bar in place via refresh_partial as chunks arrive.
    s_content_shown = true;
    note_activity();
    if (m_gui) {
        m_gui->wake();
        config_screen_ota(*m_gui, 0);
        m_gui->set_base_map();
    }
}

void ConfigMode::on_ota_end(bool ok)
{
    note_activity();
    if (m_gui) {
        m_gui->wake();
        m_gui->show_text(ok ? "Rebooting..." : "Update failed",
                         ok ? "New firmware staged" : "Old firmware kept");
    }
}

void ConfigMode::on_attendee(const char *s, size_t len)
{
    if (m_mesh) {
        m_mesh->on_set_attendee_id(s, len);
    }
    note_activity();
}

void ConfigMode::on_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                              uint8_t r, uint8_t g, uint8_t b)
{
    if (m_mesh) {
        m_mesh->on_set_frame_led(kind, idx, anim, r, g, b);
    }
    note_activity();
}

void ConfigMode::on_mesh_tx(const uint8_t *access, size_t len)
{
    // This badge is the gateway: re-originate the app's broadcast onto the mesh.
    if (m_mesh) {
        m_mesh->on_mesh_tx(access, len);
    }
    note_activity();
}

void ConfigMode::run(GUI *gui, MeshNode *mesh, const struct gpio_dt_spec *btn)
{
    m_gui = gui;
    m_mesh = mesh;
    s_self = this;
    s_content_shown = false;
    atomic_clear(&s_image_pending);
    atomic_clear(&s_display_pending);
    note_activity();

    config_gatt_set_callbacks(&s_gatt_cb);
    image_xfer_set_complete_cb(cb_image);   // take over the image render in config mode

    // Read the battery once, gating the divider on only for the sample.
    battery_init();
    const int batt_mv = battery_read_mv();
    const int batt_pct = (batt_mv >= 0) ? battery_percent(batt_mv) : 0;

    char code[5];
    app_identity_code(code);
    char url[80];
    snprintf(url, sizeof(url), VIBAMIX_CONFIG_QR_PREFIX "%s", code);

    const int total_sec = (int)(kConfigWindowMs / 1000);

    // Initial full draw + establish the partial-refresh baseline. The panel stays
    // awake (no deep sleep) so the per-second countdown can use partial refresh.
    gui->wake();
    {
        struct als_reading als = ambient_light_sensor_get();
        const int lux = als.valid ? (int)als.lux : -1;
        qr_screen_draw(*gui, code, url, batt_mv, batt_pct, total_sec, total_sec, lux);
    }
    gui->set_base_map();

    // Bring up the fast connectable advertiser so a laptop finds us quickly.
    s_adv_give_up = false;
    start_fast_adv();

    // Consume the press that woke/entered us: wait for the button to be released,
    // then clear the exit flag so the window doesn't close immediately. Only a
    // genuine new press after this point exits.
    if (btn) {
        for (int i = 0; i < 500 && gpio_pin_get_dt(btn) == 1; i++) {
            k_sleep(K_MSEC(10));
        }
    }
    s_exit = false;

    printk("config: window open, code %s, url %s\n", code, url);

    // Three phases: waiting (QR + countdown) -> connected (no timeout, "Connected"
    // screen) -> after a disconnect, identity screen + a fresh time-to-sleep
    // countdown (reset by the event-mesh heartbeat). A button press, or the window
    // elapsing while not connected, ends config mode.
    const struct app_config *cfg = app_config_get();
    enum { PHASE_QR, PHASE_CONNECTED } phase = PHASE_QR;
    bool s_disconnected_exit = false;   // ended because the peer dropped the GATT link
    int last_shown = -1;
    int tick = 0;
    int64_t last_ka_sent = 0;   // last 1 Hz keepalive notify (main thread)
    uint8_t ka_seq = 0;
    bool    ka_blink = false;
    int     ka_tick = 0;
    bool    gw_shown = false;   // last connected-screen variant (false=Connected, true=Mesh Gateway)
    int     ota_pct_shown = -1; // last OTA progress percentage painted (-1 = none yet)
    int     ota_tick = 0;       // partial-refresh counter for the OTA bar
    // True once the currently-shown content frame has been (re)established as the
    // partial-refresh base while the panel is awake — so the keepalive dot can be
    // partial-refreshed over it. Invalidated whenever a content frame is rendered
    // (those paths sleep the panel, leaving the base stale).
    bool    content_based = false;
    bool    prev_content_shown = false;

    // Per-frame LEDs: light the strip only while a content frame is on the panel
    // (mirror s_content_shown). The strip was powered down before config mode, so
    // we re-power on the rising edge and cut it again on the falling edge. The
    // frame's pattern/color is already set by apply_frame_led in the deferred
    // on_display_screen / on_image paths.
    LEDStrip *leds = mesh->leds();
    bool      led_on = false;

    for (;;) {
        // Consume a mesh heartbeat/content event (set on the BT thread) here so
        // the 64-bit s_last_activity is only ever written from the main thread.
        if (atomic_cas(&s_heartbeat_pending, 1, 0)) {
            note_activity();
        }
        // Same for the inbound keepalive write (app -> badge liveness).
        if (atomic_cas(&s_keepalive_pending, 1, 0)) {
            s_last_keepalive_rx = k_uptime_get();
            note_activity();
        }

        // Render deferred content on the main thread — never on the BT thread,
        // where the multi-second ePaper refresh stalls and drops the BLE link.
        if (atomic_cas(&s_image_pending, 1, 0)) {
            int idx = s_pending_buf;
            s_rendering_buf = idx;   // tells the BT thread to use the other buffer
            const struct pending_img p = s_pimg[idx];
            mesh->on_image(p.slot, p.fmt, s_render_buf[idx], p.len, p.w, p.h);
            s_rendering_buf = -1;
            content_based = false;   // new frame on the panel; re-base before partial refresh
            note_activity();
        }
        if (atomic_cas(&s_display_pending, 1, 0)) {
            mesh->on_display_screen(s_pd_kind, s_pd_idx);
            content_based = false;   // new frame on the panel; re-base before partial refresh
            note_activity();
        }

        // Advance the OTA progress bar (deferred from the BT thread). Redraw only on
        // an integer-percent change so the ePaper refresh runs at most ~100 times for
        // the whole image; a periodic full refresh clears partial-refresh ghosting.
        if (atomic_cas(&s_ota_progress_pending, 1, 0)) {
            uint32_t w = (uint32_t)atomic_get(&s_ota_written);
            uint32_t t = (uint32_t)atomic_get(&s_ota_total);
            int pct = t ? (int)((uint64_t)w * 100 / t) : 0;
            if (pct != ota_pct_shown) {
                ota_pct_shown = pct;
                config_screen_ota(*gui, pct);
                if (++ota_tick % kFullRefreshEvery == 0) {
                    gui->set_base_map();
                } else {
                    gui->refresh_partial();
                }
            }
            note_activity();
        }

        // A content frame just took over the panel (e.g. a name push, which sets the
        // flag on the BT thread without queuing a deferred render): re-base it before
        // the keepalive dot can be partial-refreshed over it.
        if (s_content_shown && !prev_content_shown) {
            content_based = false;
        }
        prev_content_shown = s_content_shown;

        const bool connected = atomic_get(&s_conn_count) > 0;

        // Keep the fast advert running whenever idle (it stops on each connection),
        // so re-scans after a disconnect stay fast. No-op once already advertising.
        if (!connected) {
            start_fast_adv();
        }

        if (connected && phase != PHASE_CONNECTED) {
            phase = PHASE_CONNECTED;
            s_content_shown = false;
            s_last_keepalive_rx = k_uptime_get();  // assume alive at connect
            last_ka_sent = 0;                      // notify immediately
            ka_blink = false;
            gateway_status_set_active(true);       // relayed mesh commands now overlay
            gw_shown = false;                      // count==0 at connect -> Connected screen
            gui->wake();
            {
                struct als_reading als = ambient_light_sensor_get();
                const int lux = als.valid ? (int)als.lux : -1;
                config_screen_connected(*gui, cfg->name,
                                        cfg->has_attendee ? cfg->attendee_id : "",
                                        batt_mv, batt_pct, true, false, lux);
            }
            gui->set_base_map();
            last_shown = -1;
        } else if (!connected && phase == PHASE_CONNECTED) {
            // Computer disconnected: fully leave config mode so main() restores the
            // home identity frame and runs the heartbeat-aware mesh-mode countdown
            // (run_awake_window). The post-loop cleanup below tears everything down.
            gateway_status_set_active(false);
            s_disconnected_exit = true;
            break;
        }

        if (s_exit) {
            break;
        }
        // A 5 s button hold forces sleep regardless of connection state; bail so
        // main() routes straight to deep sleep. (The hold's initial press may have
        // had its s_exit cleared by the entry wait-for-release, so check directly.)
        if (app_force_sleep_requested()) {
            break;
        }
        if (!connected && (k_uptime_get() - s_last_activity) >= kConfigWindowMs) {
            break;
        }

        // While connected: 1 Hz bidirectional keepalive. The notify must keep
        // running even while a forced frame is shown (s_content_shown), so the link
        // stays up; only the Connected-screen repaint is gated (so it doesn't
        // overpaint the shown frame).
        if (connected && phase == PHASE_CONNECTED) {
            int64_t now = k_uptime_get();
            if (now - last_ka_sent >= 1000) {
                last_ka_sent = now;
                config_gatt_keepalive_notify(ka_seq++);
                ka_blink = !ka_blink;
                bool app_alive = (now - s_last_keepalive_rx) < 3000;
                gateway_status_set_keepalive(app_alive, ka_blink);  // feed the banner dot
                if (!s_content_shown) {
                    // Once this badge has relayed >=1 command to the fleet it's a mesh
                    // gateway -> dedicated screen; otherwise the plain Connected screen.
                    const bool gw = gateway_status_count() > 0;
                    struct als_reading als = ambient_light_sensor_get();
                    const int lux = als.valid ? (int)als.lux : -1;
                    if (gw) {
                        config_screen_gateway(*gui, batt_mv, batt_pct, app_alive, ka_blink, lux);
                    } else {
                        config_screen_connected(*gui, cfg->name,
                                                cfg->has_attendee ? cfg->attendee_id : "",
                                                batt_mv, batt_pct, app_alive, ka_blink, lux);
                    }
                    if (gw != gw_shown) {
                        // Layout changed (Connected <-> Gateway): full refresh so the
                        // old layout doesn't ghost through the partial.
                        gw_shown = gw;
                        ka_tick = 0;
                        gui->set_base_map();
                    } else if (++ka_tick % kFullRefreshEvery == 0) {
                        gui->set_base_map();
                    } else {
                        gui->refresh_partial();
                    }
                } else if (!content_based) {
                    // A content frame owns the panel and the render left it asleep
                    // with a stale partial base. Re-establish it once (wake + full
                    // refresh) with the gateway banner composited on top, so the
                    // keepalive dot can then be partial-refreshed over the frame.
                    gui->wake();
                    gateway_status_overlay(gui->framebuffer());
                    gui->set_base_map();
                    content_based = true;
                    ka_tick = 0;
                } else {
                    // Keep the keepalive dot blinking over the shown frame: re-composite
                    // the banner + flash-free partial refresh (periodic full refresh
                    // clears ghosting).
                    gateway_status_overlay(gui->framebuffer());
                    if (++ka_tick % kFullRefreshEvery == 0) {
                        gui->set_base_map();
                    } else {
                        gui->refresh_partial();
                    }
                }
            }
        }

        // Per-second repaint for the QR and identity phases (not while connected,
        // and not once a frame/OTA screen explicitly took over the panel).
        if (!connected && !s_content_shown) {
            int remaining = (int)((kConfigWindowMs - (k_uptime_get() - s_last_activity)) / 1000);
            if (remaining < 0) {
                remaining = 0;
            }
            if (remaining != last_shown) {
                last_shown = remaining;
                // Only the QR phase repaints here — a disconnect exits the loop, so
                // phase is always PHASE_QR while disconnected.
                struct als_reading als = ambient_light_sensor_get();
                const int lux = als.valid ? (int)als.lux : -1;
                qr_screen_draw(*gui, code, url, batt_mv, batt_pct, remaining, total_sec, lux);
                if (++tick % kFullRefreshEvery == 0) {
                    gui->set_base_map();   // periodic full refresh to clear ghosting
                } else {
                    gui->refresh_partial();
                }
            }
        }
        // Gate the per-frame LED strip on while a content frame is shown (the LED
        // render thread animates it; see LEDStrip::init).
        if (leds) {
            if (s_content_shown && !led_on) {
                leds->power_on();
                led_on = true;
            } else if (!s_content_shown && led_on) {
                leds->off();
                led_on = false;
            }
        }

        k_sleep(K_MSEC(100));
    }
    printk("config: window closing (%s)\n",
           s_exit ? "button" : s_disconnected_exit ? "disconnect" : "timeout");

    if (leds) {
        leds->off();   // strip dark for the identity rest + System OFF
    }
    gateway_status_set_active(false);
    stop_fast_adv();

    // The caller (main) funnels into enter_deep_sleep() after run() returns, which
    // rests the panel on the clean identity frame (with the asleep indicator) — so
    // we don't redraw it here. A successful OTA reboots ~1.2 s after its END write,
    // preempting that and keeping the "Rebooting…" screen.

    // Disconnect cleanly so the peer sees a teardown, not a supervision timeout.
    if (s_conn) {
        bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        for (int i = 0; i < 30 && s_conn; i++) {
            k_sleep(K_MSEC(10));
        }
    }
}
