#include "ConfigMode.h"

#include "MeshNode.h"
#include "app_config.h"
#include "battery.h"
#include "config_gatt.h"
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

// Base URL the QR encodes; the page reads `id`. Set this to your GitHub Pages URL.
// Keep it short so the QR stays a low version (see qr_screen.cpp).
#define VIBAMIX_CONFIG_URL_BASE "https://example.github.io/vibamix/?id="

// How long to stay awake with no activity before powering off.
static constexpr int64_t kConfigWindowMs = 180 * 1000;

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

// Dedicated fast connectable advertiser, run only during config mode so a laptop
// finds the badge instantly. The mesh proxy's own advertising is slow (~1.9 s
// Network-ID) with the name only in the scan response; this advert puts the
// config service UUID + name in the primary AD at ~100 ms. Extended advertising
// (a second adv set; CONFIG_BT_EXT_ADV_MAX_ADV_SET=2) — required to fit a 128-bit
// UUID + name, and to coexist with the mesh stack's own extended advertising.
static struct bt_le_ext_adv *s_adv;
static volatile bool         s_adv_on;
static bool                  s_adv_give_up;

static const struct bt_le_adv_param k_fast_adv_param = {
    .id = BT_ID_DEFAULT,
    .sid = 1,
    .options = BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_EXT_ADV,
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
            const char *name = bt_get_name();
            struct bt_data ad[] = {
                BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
                BT_DATA_BYTES(BT_DATA_UUID128_ALL,
                    BT_UUID_128_ENCODE(0xf0de0001, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)),
                BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
            };
            if (bt_le_ext_adv_set_data(s_adv, ad, ARRAY_SIZE(ad), NULL, 0) != 0) {
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

void config_mode_on_button(void)
{
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
    .on_ota_start = cb_ota_start,
    .on_ota_end   = cb_ota_end,
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
    // Store + render via the mesh path, then stop the countdown from repainting.
    if (m_mesh) {
        m_mesh->on_image(slot, fmt, buf, len, w, h);
    }
    s_content_shown = true;
    note_activity();
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
    if (m_mesh) {
        m_mesh->on_display_screen(kind, idx);
    }
    s_content_shown = true;
    note_activity();
}

void ConfigMode::on_ota_start(uint32_t total)
{
    // Take over the panel so the countdown stops repainting, and warn the user.
    s_content_shown = true;
    note_activity();
    if (m_gui) {
        m_gui->wake();
        m_gui->show_text("Updating firmware", "Do not power off");
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

void ConfigMode::run(GUI *gui, MeshNode *mesh, const struct gpio_dt_spec *btn)
{
    m_gui = gui;
    m_mesh = mesh;
    s_self = this;
    s_content_shown = false;
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
    snprintf(url, sizeof(url), VIBAMIX_CONFIG_URL_BASE "%s", code);

    const int total_sec = (int)(kConfigWindowMs / 1000);

    // Initial full draw + establish the partial-refresh baseline. The panel stays
    // awake (no deep sleep) so the per-second countdown can use partial refresh.
    gui->wake();
    qr_screen_draw(*gui, code, url, batt_mv, batt_pct, total_sec, total_sec);
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
    enum { PHASE_QR, PHASE_CONNECTED, PHASE_IDENTITY } phase = PHASE_QR;
    int last_shown = -1;
    int tick = 0;

    for (;;) {
        // Consume a mesh heartbeat/content event (set on the BT thread) here so
        // the 64-bit s_last_activity is only ever written from the main thread.
        if (atomic_cas(&s_heartbeat_pending, 1, 0)) {
            note_activity();
        }

        const bool connected = atomic_get(&s_conn_count) > 0;

        // Keep the fast advert running whenever idle (it stops on each connection),
        // so re-scans after a disconnect stay fast. No-op once already advertising.
        if (!connected) {
            start_fast_adv();
        }

        if (connected && phase != PHASE_CONNECTED) {
            phase = PHASE_CONNECTED;
            s_content_shown = false;
            gui->wake();
            config_screen_connected(*gui, cfg->name,
                                    cfg->has_attendee ? cfg->attendee_id : "",
                                    batt_mv, batt_pct);
            gui->set_base_map();
            last_shown = -1;
        } else if (!connected && phase == PHASE_CONNECTED) {
            // Just disconnected: show the identity screen + a fresh countdown.
            phase = PHASE_IDENTITY;
            s_content_shown = false;
            note_activity();
            last_shown = -1;
        }

        if (s_exit) {
            break;
        }
        if (!connected && (k_uptime_get() - s_last_activity) >= kConfigWindowMs) {
            break;
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
                if (phase == PHASE_QR) {
                    qr_screen_draw(*gui, code, url, batt_mv, batt_pct, remaining, total_sec);
                } else {
                    identity_status_screen_draw(*gui, cfg->name,
                                                cfg->has_attendee ? cfg->attendee_id : "",
                                                cfg->fun_fact, batt_mv, batt_pct,
                                                remaining, total_sec);
                }
                if (++tick % kFullRefreshEvery == 0) {
                    gui->set_base_map();   // periodic full refresh to clear ghosting
                } else {
                    gui->refresh_partial();
                }
            }
        }
        k_sleep(K_MSEC(250));
    }
    printk("config: window closing (%s)\n", s_exit ? "button" : "timeout");

    stop_fast_adv();

    // Rest on a clean identity screen (no status bar) unless a specific frame or
    // OTA screen is being shown — the bistable panel keeps whatever we leave.
    if (!s_content_shown) {
        gui->wake();
        identity_screen_draw(*gui, cfg->name, cfg->has_attendee ? cfg->attendee_id : "",
                             cfg->fun_fact);
        gui->set_base_map();
    }
    gui->sleep();

    // Disconnect cleanly so the peer sees a teardown, not a supervision timeout.
    if (s_conn) {
        bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        for (int i = 0; i < 30 && s_conn; i++) {
            k_sleep(K_MSEC(10));
        }
    }
}
