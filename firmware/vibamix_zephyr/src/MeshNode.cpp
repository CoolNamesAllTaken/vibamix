#include "MeshNode.h"

#include "ConfigMode.h"
#include "app_config.h"
#include "badge_store.h"
#include "identity.h"
#include "image_xfer.h"
#include "mesh_keys.h"
#include "mesh_model.h"
#include "qr_screen.h"

#include <stdio.h>
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

static void cb_set_name(const char *name, size_t len)
{
    if (s_self) { s_self->on_set_name(name, len); }
}

static void cb_set_fun_fact(const char *fact, size_t len)
{
    if (s_self) { s_self->on_set_fun_fact(fact, len); }
}

static void cb_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_self) { s_self->on_set_led_color(r, g, b); }
}

static void cb_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                     uint16_t w, uint16_t h)
{
    if (s_self) { s_self->on_image(slot, fmt, buf, len, w, h); }
}

static void cb_heartbeat(void)
{
    config_mode_on_heartbeat();   // keep an awake badge awake
}

static void cb_set_screen(uint8_t idx, const char *hdr, size_t hlen,
                          const char *body, size_t blen)
{
    if (s_self) { s_self->on_set_screen(idx, hdr, hlen, body, blen); }
    config_mode_on_heartbeat();
}

static void cb_display(uint8_t kind, uint8_t idx)
{
    if (s_self) { s_self->on_display_screen(kind, idx); }
    config_mode_on_content();     // a command screen took over; keep awake
}

static void cb_set_attendee(const char *id, size_t len)
{
    if (s_self) { s_self->on_set_attendee_id(id, len); }
    config_mode_on_heartbeat();
}

static void cb_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                             uint8_t r, uint8_t g, uint8_t b)
{
    if (s_self) { s_self->on_set_frame_led(kind, idx, anim, r, g, b); }
    config_mode_on_heartbeat();
}

} // extern "C"

static const struct mesh_config_handlers s_handlers = {
    .set_name       = cb_set_name,
    .set_fun_fact   = cb_set_fun_fact,
    .set_led_color  = cb_set_led_color,
    .heartbeat      = cb_heartbeat,
    .set_screen     = cb_set_screen,
    .display_screen = cb_display,
    .set_attendee   = cb_set_attendee,
    .set_frame_led  = cb_set_frame_led,
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

    // Advertise the proxy/provisioning GATT service for phone ingress.
    bt_mesh_prov_enable(BT_MESH_PROV_GATT);

    return 0;
}

void MeshNode::apply_persisted_config()
{
    const struct app_config *cfg = app_config_get();

    if (cfg->has_color && m_leds) {
        m_leds->set_color(cfg->r, cfg->g, cfg->b);
    }
    // If a frame is the resting content and it has a per-frame LED, that wins.
    if (cfg->has_disp) {
        apply_frame_led(cfg->disp_kind, cfg->disp_idx);
    }

    // Single boot-display authority. The panel was left awake by GUI::init().
    if (!m_gui) {
        return;
    }
    if (cfg->has_custom_image) {
        // Leave the user's bistable image untouched; just release the panel.
        m_gui->sleep();
    } else if (cfg->has_name) {
        redraw_identity();
    } else {
        // First boot, nothing configured yet.
        m_gui->show_hello_world();
        m_gui->sleep();
    }
}

void MeshNode::redraw_identity()
{
    if (!m_gui) {
        return;
    }
    const struct app_config *cfg = app_config_get();
    m_gui->wake();
    identity_screen_draw(*m_gui, cfg->name, cfg->has_attendee ? cfg->attendee_id : "",
                         cfg->fun_fact);
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
    redraw_identity();
}

void MeshNode::on_set_fun_fact(const char *fact, size_t len)
{
    app_config_set_fun_fact(fact, len);
    redraw_identity();
}

void MeshNode::on_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    app_config_set_color(r, g, b);
    if (m_leds) {
        m_leds->set_color(r, g, b);
    }
}

void MeshNode::on_set_attendee_id(const char *id, size_t len)
{
    app_config_set_attendee_id(id, len);
    redraw_identity();   // table ID lives on the identity screen
}

void MeshNode::on_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                                uint8_t r, uint8_t g, uint8_t b)
{
    app_config_set_frame_led(kind, idx, anim, r, g, b);

    // If this is the frame currently on screen, apply it live.
    const struct app_config *cfg = app_config_get();
    if (cfg->has_disp && cfg->disp_kind == kind && cfg->disp_idx == idx) {
        apply_frame_led(kind, idx);
    }
}

void MeshNode::on_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                        uint16_t w, uint16_t h)
{
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
