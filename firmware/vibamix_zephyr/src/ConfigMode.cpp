#include "ConfigMode.h"

#include "MeshNode.h"
#include "battery.h"
#include "config_gatt.h"
#include "identity.h"
#include "image_xfer.h"
#include "qr_screen.h"

#include <stdio.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

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

// Set from the mesh-RX (BT) thread; consumed on the main thread so only the main
// thread ever writes the 64-bit s_last_activity (no torn write on the 32-bit M33).
static atomic_t s_heartbeat_pending;

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
    .on_name     = cb_name,
    .on_activity = cb_activity,
    .on_screen   = cb_screen,
    .on_display  = cb_display,
};

static void conn_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        return;
    }
    if (!s_conn) {
        s_conn = bt_conn_ref(conn);
    }
    if (s_self) { s_self->note_activity(); }
    printk("config: peer connected\n");
}

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
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
    qr_screen_draw(*gui, code, url, batt_mv, batt_pct, total_sec);
    gui->set_base_map();

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

    // Stay awake until inactivity timeout or a second button press. Each second,
    // repaint the countdown with a fast partial refresh (a full refresh every
    // kFullRefreshEvery ticks clears ghosting). Activity (connect / GATT writes /
    // disconnect) calls note_activity() and resets the window. Once an image/name
    // takes over the screen, stop repainting it.
    int last_shown = total_sec;
    int tick = 0;
    while (!s_exit && (k_uptime_get() - s_last_activity) < kConfigWindowMs) {
        // Consume a mesh heartbeat/content event (set on the BT thread) here so
        // the 64-bit s_last_activity is only ever written from the main thread.
        if (atomic_cas(&s_heartbeat_pending, 1, 0)) {
            note_activity();
        }
        if (!s_content_shown) {
            int remaining = (int)((kConfigWindowMs - (k_uptime_get() - s_last_activity)) / 1000);
            if (remaining < 0) {
                remaining = 0;
            }
            if (remaining != last_shown) {
                last_shown = remaining;
                qr_screen_draw(*gui, code, url, batt_mv, batt_pct, remaining);
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

    gui->sleep();

    // Disconnect cleanly so the peer sees a teardown, not a supervision timeout.
    if (s_conn) {
        bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        for (int i = 0; i < 30 && s_conn; i++) {
            k_sleep(K_MSEC(10));
        }
    }
}
