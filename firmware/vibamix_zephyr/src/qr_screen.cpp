#include "qr_screen.h"

#include "Display_EPD_W21.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <zephyr/sys/printk.h>

// With ROTATE_270 the drawing surface is EPD_HEIGHT wide x EPD_WIDTH tall.
static constexpr int kCanvasW = EPD_HEIGHT; // 264
static constexpr int kCanvasH = EPD_WIDTH;  // 176

// Pin the QR version so the encode buffers stay tiny (our URL fits well within
// v5). NEVER use qrcodegen_BUFFER_LEN_MAX (v40 = ~3.9 KB) — it would blow the stack.
static constexpr int kQrMaxVersion = 5;
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];
static uint8_t s_qr_tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(kQrMaxVersion)];

// Solid black rectangle [x0,x1]x[y0,y1] inclusive, via exact pixel writes
// (Paint_DrawRectangle's fill drops the bottom row — see the QR fix history).
static void fill_rect(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            Paint_SetPixel(x, y, BLACK);
        }
    }
}

// Draws a small battery icon (outline + nub + fill proportional to pct) with its
// top-left at (x, y). Body is 30x14. Returns the x just past the icon.
static int draw_battery_icon(int x, int y, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int bw = 30, bh = 14;
    // Outline (1px border).
    Paint_DrawRectangle(x, y, x + bw, y + bh, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    // Positive terminal nub on the right.
    fill_rect(x + bw + 1, y + 4, x + bw + 3, y + bh - 4);
    // Fill proportional to charge, inset 2px from the border.
    const int inner_w = bw - 4;
    const int fill_w = inner_w * pct / 100;
    if (fill_w > 0) {
        fill_rect(x + 2, y + 2, x + 2 + fill_w - 1, y + bh - 2);
    }
    return x + bw + 4;
}

void qr_screen_draw(GUI &gui, const char *code, const char *url,
                    int batt_mv, int batt_pct, int remaining_sec)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    const bool ok = qrcodegen_encodeText(url, s_qr_tmp, s_qr, qrcodegen_Ecc_MEDIUM,
                                         qrcodegen_VERSION_MIN, kQrMaxVersion,
                                         qrcodegen_Mask_AUTO, true);
    int qr_extent = 0;
    if (ok) {
        const int n = qrcodegen_getSize(s_qr);
        const int quiet = 4;                 // standard QR quiet zone (modules)
        const int cells = n + 2 * quiet;
        int scale = (kCanvasH - 8) / cells;  // fit within the 176 px axis
        if (scale < 2) {
            scale = 2;
        }
        const int ox = 4, oy = 4;            // top-left origin (incl. quiet zone)
        for (int yy = 0; yy < n; yy++) {
            for (int xx = 0; xx < n; xx++) {
                if (!qrcodegen_getModule(s_qr, xx, yy)) {
                    continue;
                }
                const int px = ox + (xx + quiet) * scale;
                const int py = oy + (yy + quiet) * scale;
                fill_rect(px, py, px + scale - 1, py + scale - 1);
            }
        }
        qr_extent = ox + cells * scale;
    } else {
        printk("qr: encode failed for url len %u\n",
               (unsigned)(url ? __builtin_strlen(url) : 0));
        Paint_DrawString_EN(8, 8, "QR ERROR", &Font24, WHITE, BLACK);
    }

    // Right column.
    const int tx = (qr_extent > 0 ? qr_extent + 10 : 8);
    Paint_DrawString_EN(tx, 4,  code, &Font24, WHITE, BLACK);
    Paint_DrawString_EN(tx, 34, "Scan to set up", &Font12, WHITE, BLACK);

    // Battery: icon + "X.XXV  NN%".
    const int by = 64;
    char batt[20];
    if (batt_mv >= 0) {
        draw_battery_icon(tx, by, batt_pct);
        snprintf(batt, sizeof(batt), "%d.%02dV  %d%%",
                 batt_mv / 1000, (batt_mv % 1000) / 10, batt_pct);
    } else {
        draw_battery_icon(tx, by, 0);
        snprintf(batt, sizeof(batt), "-- V");
    }
    Paint_DrawString_EN(tx, by + 22, batt, &Font12, WHITE, BLACK);

    // Countdown to end of config mode.
    if (remaining_sec < 0) {
        remaining_sec = 0;
    }
    char cd[24];
    snprintf(cd, sizeof(cd), "Ends in %d:%02d", remaining_sec / 60, remaining_sec % 60);
    Paint_DrawString_EN(tx, 120, cd, &Font16, WHITE, BLACK);
}
