#include "GUI.h"
#include "Display_EPD_W21_spi.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/printk.h>

// With ROTATE_270 the drawing surface is EPD_HEIGHT wide x EPD_WIDTH tall.
static constexpr int kCanvasW = EPD_HEIGHT; // 264
static constexpr int kCanvasH = EPD_WIDTH;  // 176

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
    EPD_Display(m_image);
    EPD_Update();
    printk("ePaper hello world displayed\n");
}

void GUI::show_text(const char *name, const char *fun_fact)
{
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    Paint_DrawString_P(8, 10, name ? name : "", &PoppinsSB24, WHITE, BLACK);

    // Word-wrap the fun fact across the canvas width in Poppins Medium. The font
    // is proportional, so wrap by measuring candidate lines, not a char count.
    if (fun_fact && fun_fact[0]) {
        const int kLineH = PoppinsMd16.Height + 2;
        const int maxW   = kCanvasW - 16;
        const int x = 8;
        int y = 56;
        char line[64];
        int n = 0;

        const char *p = fun_fact;
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

    EPD_Display(m_image);
    EPD_Update();
    printk("ePaper identity displayed\n");
}

void GUI::render_image(const uint8_t *buf, size_t len)
{
    // image_xfer reassembles into m_image, so buf == m_image; copy only if not.
    if (buf != m_image && len <= sizeof(m_image)) {
        memcpy(m_image, buf, len);
    }
    EPD_Display(m_image);
    EPD_Update();
    printk("ePaper image displayed\n");
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

void GUI::show_als_readings(uint32_t lux, uint32_t gain, uint16_t ch0_raw, uint16_t ch1_raw, bool valid, const char *diag)
{
    char buf[32];
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    if (valid) {
        snprintf(buf, sizeof(buf), "Lux: %u", lux);
        Paint_DrawString_EN(10, 5, buf, &Font20, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "Gain: %uX", gain);
        Paint_DrawString_EN(10, 32, buf, &Font16, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "CH0: %u", ch0_raw);
        Paint_DrawString_EN(10, 54, buf, &Font16, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "CH1: %u", ch1_raw);
        Paint_DrawString_EN(10, 76, buf, &Font16, WHITE, BLACK);
    } else {
        Paint_DrawString_EN(10, 10, "ALS FAIL:", &Font20, WHITE, BLACK);
        Paint_DrawString_EN(10, 40, diag, &Font16, WHITE, BLACK);
    }
    EPD_Display(m_image);
    EPD_Update();
}

void GUI::sleep()
{
    EPD_DeepSleep();
}
