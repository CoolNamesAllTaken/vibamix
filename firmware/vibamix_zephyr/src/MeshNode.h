#ifndef VIBAMIX_MESH_NODE_H
#define VIBAMIX_MESH_NODE_H

#include "GUI.h"
#include "LEDStrip.h"

/*
 * Bluetooth Mesh node for a vibamix badge. Replaces the old plain-BLE BLERadio.
 *
 * init() brings up Bluetooth + the mesh stack, loads persisted settings,
 * deterministically self-provisions the badge (baked keys, unicast address
 * derived from the FICR device id), binds the app key, subscribes the
 * "all badges" group, and enables the GATT proxy so a phone can connect to the
 * nearest badge and inject messages that flood across the mesh.
 *
 * Incoming vendor-model commands are dispatched (via file-scope C trampolines)
 * to the GUI and LED strip, and persisted through app_config.
 */
class MeshNode {
public:
    int init(GUI *gui, LEDStrip *leds);

    // Apply whatever config was restored from settings to the display + LEDs.
    void apply_persisted_config();

    // Called by file-scope C trampolines; not for external use.
    void on_set_name(const char *name, size_t len);
    void on_set_fun_fact(const char *fact, size_t len);
    void on_set_led_color(uint8_t r, uint8_t g, uint8_t b);
    // image_xfer completion: store to `slot` (0xFF = render-only) + render.
    void on_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                  uint16_t w, uint16_t h);
    // Store a text screen (header + body) at `idx`.
    void on_set_screen(uint8_t idx, const char *hdr, size_t hlen,
                       const char *body, size_t blen);
    // Render a stored screen: kind 0 = text screen, 1 = image slot.
    void on_display_screen(uint8_t kind, uint8_t idx);

private:
    void redraw_identity();

    GUI      *m_gui{nullptr};
    LEDStrip *m_leds{nullptr};
};

#endif /* VIBAMIX_MESH_NODE_H */
