#include "GUI.h"
#include "Display_EPD_W21_spi.h"
#include "GUI_Paint.h"
#include "fonts.h"
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
    Paint_DrawString_EN(10, 10, "Hello World", &Font20, WHITE, BLACK);
    EPD_Display(m_image);
    EPD_Update();
    printk("ePaper hello world displayed\n");
}

void GUI::show_text(const char *name, const char *fun_fact)
{
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    Paint_DrawString_EN(8, 10, name ? name : "", &Font24, WHITE, BLACK);

    // Word-wrap the fun fact across the canvas width in Font16.
    if (fun_fact && fun_fact[0]) {
        const int kCharW = Font16.Width;       // px per glyph
        const int kLineH = Font16.Height + 2;
        const int max_chars = (kCanvasW - 16) / kCharW;
        int x = 8, y = 56;
        char line[48];
        int n = 0;

        for (const char *p = fun_fact;; ++p) {
            bool brk = (*p == '\0');
            if (*p == ' ' || brk) {
                line[n] = '\0';
                if (n > 0) {
                    Paint_DrawString_EN(x, y, line, &Font16, WHITE, BLACK);
                    y += kLineH;
                    n = 0;
                }
                if (brk || y > kCanvasH - kLineH) {
                    break;
                }
                continue;
            }
            if (n < max_chars && n < (int)sizeof(line) - 1) {
                line[n++] = *p;
            }
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

void GUI::sleep()
{
    EPD_DeepSleep();
}
