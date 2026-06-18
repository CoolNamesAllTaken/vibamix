#ifndef VIBAMIX_GUI_H
#define VIBAMIX_GUI_H

#include <stddef.h>
#include <stdint.h>
#include "Display_EPD_W21.h"

// Render stored grayscale image slots (and the identity image) in real 4-level gray
// (SSD1680 custom waveform) instead of dithering to 1bpp. Set to 0 for the dithered
// B/W path. Declared here so non-GUI code can pick the gray identity render path.
#ifndef VIBAMIX_EPD_4GRAY
#define VIBAMIX_EPD_4GRAY 1
#endif

class GUI {
public:
    void init();
    void show_hello_world();

    // Re-initialize the panel after a deep sleep, before redrawing.
    void wake();

    // Draw the badge identity screen: name (large) + fun fact (wrapped).
    void show_text(const char *name, const char *body);

    // Blit a full 1bpp framebuffer to the panel. `buf` is normally framebuffer()
    // itself (image_xfer reassembles straight into it), so no copy is needed.
    void render_image(const uint8_t *buf, size_t len);

    // Dither a 2-bit grayscale image (landscape-packed, w<=264 h<=176) into the
    // framebuffer and display it.
    void render_gray2(const uint8_t *src, uint16_t w, uint16_t h);

    // Render a 2-bit grayscale image FULL-SCREEN (4-gray) as the identity base. The
    // banner is baked into the source before this (see identity_bake_overlays) — a
    // 1-bit overlay can't run over a 4-gray base.
    void render_identity_gray(const uint8_t *src, uint16_t w, uint16_t h);

    void sleep();

    // Full refresh that also sets the partial-refresh baseline (both RAM banks),
    // for screens that will then be updated with refresh_partial(). Renders
    // whatever is currently in framebuffer().
    void set_base_map();

    // Fast, flash-free differential update of the current framebuffer. Requires a
    // prior set_base_map(). Use for the live config-screen countdown.
    void refresh_partial();

    // The display framebuffer, shared with image_xfer for in-place reassembly.
    uint8_t *framebuffer() { return m_image; }
    size_t   framebuffer_size() const { return sizeof(m_image); }

private:
    void render_gray_full(const uint8_t *src, uint16_t w, uint16_t h);

    uint8_t m_image[EPD_WIDTH * EPD_HEIGHT / 8];
};

#endif /* VIBAMIX_GUI_H */
