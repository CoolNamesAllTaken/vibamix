#ifndef VIBAMIX_GUI_H
#define VIBAMIX_GUI_H

#include <stddef.h>
#include <stdint.h>
#include "Display_EPD_W21.h"

class GUI {
public:
    void init();

    // Re-initialize the panel after a deep sleep, before redrawing.
    void wake();

    void show_hello_world();

    // Draw the badge identity screen: name (large) + fun fact (wrapped).
    void show_text(const char *name, const char *fun_fact);

    // Blit a full 1bpp framebuffer to the panel. `buf` is normally framebuffer()
    // itself (image_xfer reassembles straight into it), so no copy is needed.
    void render_image(const uint8_t *buf, size_t len);

    void show_als_readings(uint32_t lux, uint32_t gain, uint16_t ch0_raw, uint16_t ch1_raw, bool valid, const char *diag);

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
    uint8_t m_image[EPD_WIDTH * EPD_HEIGHT / 8];
};

#endif /* VIBAMIX_GUI_H */
