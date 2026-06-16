#include "MeshNode.h"

#include "app_config.h"
#include "identity.h"
#include "image_xfer.h"
#include "mesh_keys.h"
#include "mesh_model.h"

#include <stdio.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

// Singleton so the file-scope C trampolines can reach the instance.
static MeshNode *s_self;

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

static void cb_image(const uint8_t *buf, size_t len, uint16_t w, uint16_t h)
{
    if (s_self) { s_self->on_image(buf, len, w, h); }
}

} // extern "C"

static const struct mesh_config_handlers s_handlers = {
    .set_name      = cb_set_name,
    .set_fun_fact  = cb_set_fun_fact,
    .set_led_color = cb_set_led_color,
};

int MeshNode::init(GUI *gui, LEDStrip *leds)
{
    m_gui = gui;
    m_leds = leds;
    s_self = this;

    mesh_model_set_config_handlers(&s_handlers);
    image_xfer_init(gui->framebuffer(), gui->framebuffer_size());
    image_xfer_set_complete_cb(cb_image);

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
    m_gui->show_text(cfg->name, cfg->fun_fact);
    m_gui->sleep();
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

void MeshNode::on_image(const uint8_t *buf, size_t len, uint16_t w, uint16_t h)
{
    if (m_gui) {
        m_gui->wake();
        m_gui->render_image(buf, len);
        m_gui->sleep();
    }
    app_config_set_has_image(true);
}
