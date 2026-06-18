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

    // The LED strip this node drives (config mode borrows it to light per-frame
    // LEDs while a content frame is on the panel).
    LEDStrip *leds() const { return m_leds; }

    // Redraw the clean identity frame (name/ID + optional image), full refresh.
    // Used to rest the bistable panel on a bar-free frame before sleep. When
    // `sleeping` is set, a small "asleep" indicator (moon + z z z) is composited
    // into the corner before the refresh.
    void redraw_identity(bool sleeping = false);

    // True if the last redraw_identity() rendered a full-screen 4-gray image (so the
    // awake-window countdown must use the banner-region refresh, not a whole-frame
    // partial which would flatten the gray). Set by redraw_identity().
    bool identity_is_gray() const { return m_identity_gray; }

    // Called by file-scope C trampolines; not for external use.
    //
    // Mesh (broadcast, ephemeral): override the live LEDs / draw a text frame
    // straight to the panel — neither is stored.
    void on_show_led(uint8_t anim, uint8_t r, uint8_t g, uint8_t b);
    void on_show_text(const char *title, size_t tlen, const char *body, size_t blen);

    // Config-mode gateway: re-originate a vendor-model access payload (sent by a
    // GATT-connected app) onto the mesh "all badges" group.
    void on_mesh_tx(const uint8_t *access, size_t len);

    // GATT (individual, persistent) config — reused by the config-mode GATT path.
    void on_set_name(const char *name, size_t len);
    // image_xfer completion: store to `slot` (0xFF = render-only) + render.
    void on_image(uint8_t slot, uint8_t fmt, const uint8_t *buf, size_t len,
                  uint16_t w, uint16_t h);
    // Store a text screen (header + body) at `idx`.
    void on_set_screen(uint8_t idx, const char *hdr, size_t hlen,
                       const char *body, size_t blen);
    // Render a stored frame: kind text(0) / image(1) / identity(2).
    void on_display_screen(uint8_t kind, uint8_t idx);
    // Set the attendee/table ID (shown on the identity frame).
    void on_set_attendee_id(const char *id, size_t len);
    // Set a frame's LED animation + color; applies live if it's the shown frame.
    void on_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
                          uint8_t r, uint8_t g, uint8_t b);

private:
    void apply_frame_led(uint8_t kind, uint8_t idx);

    GUI      *m_gui{nullptr};
    LEDStrip *m_leds{nullptr};
    bool      m_identity_gray{false};
};

#endif /* VIBAMIX_MESH_NODE_H */
