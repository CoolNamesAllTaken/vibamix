#ifndef VIBAMIX_CONFIG_MODE_H
#define VIBAMIX_CONFIG_MODE_H

#include <stddef.h>
#include "GUI.h"

class MeshNode;
struct gpio_dt_spec;

/*
 * Config mode: entered when the badge is woken by the button (see main.cpp).
 * Renders the unique ID + QR screen, lets a phone connect over the custom GATT
 * config service and upload a drawing / set a name, and stays awake until a
 * timeout of inactivity or a second button press. main() then powers off.
 */
class ConfigMode {
public:
    // Blocks until the config window closes. BLE/mesh must already be up. `btn`
    // is the user button, polled so the entering/waking press doesn't immediately
    // count as the exit press.
    void run(GUI *gui, MeshNode *mesh, const struct gpio_dt_spec *btn);

    // Called from GATT/connection events to keep the window open.
    void note_activity();
    // Called from the button ISR (via config_mode_on_button) to exit.
    void request_exit();
    // GATT name-write handler bridge.
    void on_name(const char *s, size_t len);
    // Image-upload completion: render it (it takes over the screen) and stop the
    // countdown from repainting over it.
    void on_content_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                          uint16_t w, uint16_t h);
    // GATT screen/display handler bridges.
    void on_screen(uint8_t idx, const char *hdr, size_t hlen,
                   const char *body, size_t blen);
    void on_display(uint8_t kind, uint8_t idx);
    // OTA: show a status screen while the firmware image streams in / applies.
    void on_ota_start(uint32_t total);
    void on_ota_end(bool ok);
    // Config-command bridges for the attendee/table ID and per-frame LED.
    void on_attendee(const char *s, size_t len);
    void on_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                      uint8_t r, uint8_t g, uint8_t b);
    // Gateway bridge: re-originate a vendor-model access payload onto the mesh.
    void on_mesh_tx(const uint8_t *access, size_t len);

private:
    GUI      *m_gui{nullptr};
    MeshNode *m_mesh{nullptr};
};

#ifdef __cplusplus
extern "C" {
#endif

// Hook for the user-button ISR to request exiting config mode.
void config_mode_on_button(void);
// Mesh-RX hooks (run on the BT thread): keep an awake badge awake. `on_content`
// also marks the screen as taken over so the countdown stops repainting.
void config_mode_on_heartbeat(void);
void config_mode_on_content(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_CONFIG_MODE_H */
