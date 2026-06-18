#include "GUI.h"
#include "Display_EPD_W21_spi.h"
#include "GUI_Paint.h"
#include "dither.h"
#include "fonts.h"
#include "gateway_status.h"
#include <string.h>
#include <zephyr/sys/printk.h>

// With ROTATE_270 the drawing surface is EPD_HEIGHT wide x EPD_WIDTH tall.
static constexpr int kCanvasW = EPD_HEIGHT; // 264
static constexpr int kCanvasH = EPD_WIDTH;  // 176

// Render stored grayscale image slots in real 4-level gray (SSD1680 custom waveform)
// instead of dithering to 1bpp. Set to 0 to fall back to the dithered B/W path.
#ifndef VIBAMIX_EPD_4GRAY
#define VIBAMIX_EPD_4GRAY 1
#endif

// Pre-condition the panel before each 4-gray render so the "from white" gray waveform
// starts from a uniform state (kills the structured "tiny X" grain). 0=none,
// 1=single white flush, 2=white->black->white wipe. 2 is cleanest; drop to 1 if the
// extra flashing/time isn't worth it.
#ifndef VIBAMIX_EPD_4GRAY_CLEAR
#define VIBAMIX_EPD_4GRAY_CLEAR 2
#endif

void GUI::init()
{
    EPD_GPIO_Init();
    EPD_HW_Init();
}

void GUI::wake()
{
    // The panel was put in deep sleep; re-init before driving it again.
    EPD_HW_Init();
}

void GUI::show_hello_world()
{
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    Paint_DrawString_P(10, 10, "Hello World", &PoppinsMd20, WHITE, BLACK);
    gateway_status_overlay(m_image);  // mesh-gateway banner (no-op unless relaying)
    EPD_Display(m_image);  // EPD_Display already triggers a full refresh internally
    printk("ePaper hello world displayed\n");
}

void GUI::show_text(const char *name, const char *body)
{
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    Paint_DrawString_P(8, 10, name ? name : "", &PoppinsSB24, WHITE, BLACK);

    // Word-wrap the fun fact across the canvas width in Poppins Medium. The font
    // is proportional, so wrap by measuring candidate lines, not a char count.
    if (body && body[0]) {
        const int kLineH = PoppinsMd16.Height + 2;
        const int maxW   = kCanvasW - 16;
        const int x = 8;
        int y = 56;
        char line[64];
        int n = 0;

        const char *p = body;
        for (;;) {
            while (*p == ' ') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }

            // Next word is [p, q).
            const char *q = p;
            while (*q != '\0' && *q != ' ') {
                ++q;
            }
            const int wlen = (int)(q - p);

            // Build "<line> <word>" and see if it still fits the canvas width.
            char cand[64];
            int cn = 0;
            if (n > 0) {
                memcpy(cand, line, n);
                cn = n;
                cand[cn++] = ' ';
            }
            int copy = wlen;
            if (cn + copy > (int)sizeof(cand) - 1) {
                copy = (int)sizeof(cand) - 1 - cn;
            }
            memcpy(cand + cn, p, copy);
            cn += copy;
            cand[cn] = '\0';

            if (n > 0 && Paint_StringWidth_P(cand, &PoppinsMd16) > maxW) {
                // Doesn't fit: flush the current line, start a new one with the word.
                Paint_DrawString_P(x, y, line, &PoppinsMd16, WHITE, BLACK);
                y += kLineH;
                if (y > kCanvasH - kLineH) {
                    n = 0;
                    break;
                }
                n = (copy < (int)sizeof(line) - 1) ? copy : (int)sizeof(line) - 1;
                memcpy(line, p, n);
                line[n] = '\0';
            } else {
                // Fits (or the word is alone on the line): accept the candidate.
                memcpy(line, cand, cn + 1);
                n = cn;
            }
            p = q;
        }
        if (n > 0 && y <= kCanvasH - kLineH) {
            Paint_DrawString_P(x, y, line, &PoppinsMd16, WHITE, BLACK);
        }
    }

    gateway_status_overlay(m_image);  // mesh-gateway banner (no-op unless relaying)
    EPD_Display(m_image);  // EPD_Display already triggers a full refresh internally
    printk("ePaper identity displayed\n");
}

void GUI::render_image(const uint8_t *buf, size_t len)
{
    // image_xfer reassembles into m_image, so buf == m_image; copy only if not.
    if (buf != m_image && len <= sizeof(m_image)) {
        memcpy(m_image, buf, len);
    }
    gateway_status_overlay(m_image);  // mesh-gateway banner (no-op unless relaying)
    EPD_Display(m_image);  // EPD_Display already triggers a full refresh internally
    printk("ePaper image displayed\n");
}

void GUI::render_gray2(const uint8_t *src, uint16_t w, uint16_t h)
{
#if VIBAMIX_EPD_4GRAY
    // Real 4-level gray: load the gray waveform LUT, then write the two bitplanes.
    // m_image is reused for both planes in sequence (no second framebuffer). The
    // gateway banner is skipped here — it's a 1-bit overlay that has no meaning in
    // the dual-plane gray RAM, and image pushes are full-screen by design.
    EPD_Condition_4Gray(VIBAMIX_EPD_4GRAY_CLEAR);  // uniform start state -> less grain
    EPD_Init_4Gray();
    gray2_to_plane(src, w, h, m_image, 0);  // MSB plane -> RAM 0x26
    EPD_WritePlane(0x26, m_image);
    gray2_to_plane(src, w, h, m_image, 1);  // LSB plane -> RAM 0x24
    EPD_WritePlane(0x24, m_image);
    EPD_Update_4Gray();
    EPD_Restore_BW();  // gray waveform is scoped to this render; leave the panel in B/W mode
    printk("ePaper 4-gray image displayed\n");
#else
    dither_2bit_to_fb(src, w, h, m_image);
    gateway_status_overlay(m_image);  // mesh-gateway banner (no-op unless relaying)
    EPD_Display(m_image);  // EPD_Display already triggers a full refresh internally
    printk("ePaper grayscale image displayed\n");
#endif
}

void GUI::set_base_map()
{
    EPD_SetBaseMap(m_image);
    printk("ePaper base map set\n");
}

void GUI::refresh_partial()
{
    EPD_Display_Partial(m_image);
}

void GUI::sleep()
{
    EPD_DeepSleep();
}
