#include "qr_screen.h"

#include "Display_EPD_W21.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/sys/printk.h>

// With ROTATE_270 the drawing surface is EPD_HEIGHT wide x EPD_WIDTH tall.
static constexpr int kCanvasW = EPD_HEIGHT; // 264
static constexpr int kCanvasH = EPD_WIDTH;  // 176

static constexpr int kMargin = 6;
static constexpr int kQrBox  = 120;  // left column width reserved for the QR

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

// 1px outline [x0,x1]x[y0,y1] via exact pixel writes. Paint_DrawRectangle's
// outline goes through Paint_DrawPoint, which draws DOT_PIXEL_1X1 at (X-1,Y-1) —
// so it lands 1px up-left of our fills and the fill looks offset. Draw the border
// with the same Paint_SetPixel convention as fill_rect so the two line up.
static void draw_rect(int x0, int y0, int x1, int y1)
{
    fill_rect(x0, y0, x1, y0);   // top
    fill_rect(x0, y1, x1, y1);   // bottom
    fill_rect(x0, y0, x0, y1);   // left
    fill_rect(x1, y0, x1, y1);   // right
}

// Small battery icon (outline + nub + fill proportional to pct), top-left at
// (x, y). Body is 20x10. Returns the x just past the icon (incl. nub).
static int draw_battery_icon(int x, int y, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    const int bw = 20, bh = 10;
    draw_rect(x, y, x + bw, y + bh);
    fill_rect(x + bw + 1, y + 3, x + bw + 2, y + bh - 3);   // + terminal nub
    const int inner_w = bw - 4;
    const int fill_w = inner_w * pct / 100;
    if (fill_w > 0) {
        fill_rect(x + 2, y + 2, x + 2 + fill_w - 1, y + bh - 2);
    }
    return x + bw + 4;
}

// Top status bar: battery icon + percent (left), countdown M:SS (right), then a
// progress bar that shrinks from full toward empty as remaining/total falls.
static void draw_status_bar(int batt_mv, int batt_pct, int remaining_sec, int total_sec)
{
    char buf[20];

    // Battery (left): "NN% (X.XXV)".
    const int icon_end = draw_battery_icon(kMargin, 4, batt_mv >= 0 ? batt_pct : 0);
    if (batt_mv >= 0) {
        snprintf(buf, sizeof(buf), "%d%% (%d.%02dV)", batt_pct,
                 batt_mv / 1000, (batt_mv % 1000) / 10);
    } else {
        snprintf(buf, sizeof(buf), "-- %%");
    }
    Paint_DrawString_P(icon_end + 3, 1, buf, &PoppinsMd16, WHITE, BLACK);

    // Countdown (right-aligned).
    if (remaining_sec < 0) {
        remaining_sec = 0;
    }
    snprintf(buf, sizeof(buf), "%d:%02d", remaining_sec / 60, remaining_sec % 60);
    const int cw = Paint_StringWidth_P(buf, &PoppinsMd16);
    Paint_DrawString_P(kCanvasW - kMargin - cw, 1, buf, &PoppinsMd16, WHITE, BLACK);

    // Progress bar.
    const int y0 = 21, y1 = 27;
    draw_rect(kMargin, y0, kCanvasW - kMargin, y1);
    if (total_sec <= 0) {
        total_sec = 1;
    }
    int rem = remaining_sec > total_sec ? total_sec : remaining_sec;
    const int inner_w = (kCanvasW - 2 * kMargin) - 3;
    const int fw = inner_w * rem / total_sec;
    if (fw > 0) {
        fill_rect(kMargin + 2, y0 + 2, kMargin + 2 + fw - 1, y1 - 2);
    }

    // Divider under the header.
    fill_rect(kMargin, 33, kCanvasW - kMargin, 33);
}

void qr_screen_draw(GUI &gui, const char *code, const char *url,
                    int batt_mv, int batt_pct, int remaining_sec, int total_sec)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    draw_status_bar(batt_mv, batt_pct, remaining_sec, total_sec);

    // Body: QR (left, in a fixed-width box so the right column stays put), and
    // the heading / prompt / code (right).
    const int body_y = 40;
    const bool ok = qrcodegen_encodeText(url, s_qr_tmp, s_qr, qrcodegen_Ecc_MEDIUM,
                                         qrcodegen_VERSION_MIN, kQrMaxVersion,
                                         qrcodegen_Mask_AUTO, true);
    if (ok) {
        const int n = qrcodegen_getSize(s_qr);
        const int quiet = 4;                 // standard QR quiet zone (modules)
        const int cells = n + 2 * quiet;
        const int avail_w = kQrBox - kMargin;
        const int avail_h = kCanvasH - body_y - 2;
        int scale = (avail_w < avail_h ? avail_w : avail_h) / cells;
        if (scale < 2) {
            scale = 2;
        }
        const int ox = kMargin, oy = body_y;
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
    } else {
        printk("qr: encode failed for url len %u\n",
               (unsigned)(url ? __builtin_strlen(url) : 0));
        Paint_DrawString_P(kMargin, body_y, "QR ERROR", &PoppinsMd20, WHITE, BLACK);
    }

    // Right column (fixed x so it doesn't shift with QR size).
    const int rx = kQrBox + 8;
    Paint_DrawString_P(rx, body_y,      "Set up",     &PoppinsMd20, WHITE, BLACK);
    Paint_DrawString_P(rx, body_y + 22, "your badge", &PoppinsMd20, WHITE, BLACK);
    Paint_DrawString_P(rx, body_y + 52, "Scan to set up", &PoppinsMd16, WHITE, BLACK);
    Paint_DrawString_P(rx, body_y + 80, "CODE", &PoppinsMd16, WHITE, BLACK);
    Paint_DrawString_P(rx, body_y + 96, code, &PoppinsSB24, WHITE, BLACK);
}

// Word-wrap `s` in proportional `font` within `max_w`, starting at (x, y).
// Returns the y just past the last line.
static int draw_wrapped(int x, int y, int max_w, const char *s, pFONT *font, int line_h)
{
    if (!s || !s[0]) {
        return y;
    }
    char line[64];
    size_t ll = 0;
    line[0] = '\0';
    const char *p = s;

    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *w0 = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t wn = (size_t)(p - w0);
        if (wn == 0) {
            break;
        }
        if (wn > sizeof(line) - 1) {
            wn = sizeof(line) - 1;   // clamp a pathological word
        }

        // Build "line + ' ' + word" in a candidate, bounded by memcpy.
        char cand[64];
        size_t cl = 0;
        if (ll) {
            memcpy(cand, line, ll);
            cl = ll;
            if (cl < sizeof(cand) - 1) {
                cand[cl++] = ' ';
            }
        }
        if (cl + wn > sizeof(cand) - 1) {
            wn = sizeof(cand) - 1 - cl;
        }
        memcpy(cand + cl, w0, wn);
        cl += wn;
        cand[cl] = '\0';

        if (ll == 0 || Paint_StringWidth_P(cand, font) <= max_w) {
            memcpy(line, cand, cl + 1);
            ll = cl;
        } else {
            Paint_DrawString_P(x, y, line, font, WHITE, BLACK);
            y += line_h;
            size_t n = (size_t)(p - w0);
            if (n > sizeof(line) - 1) {
                n = sizeof(line) - 1;
            }
            memcpy(line, w0, n);
            line[n] = '\0';
            ll = n;
        }
    }
    if (ll) {
        Paint_DrawString_P(x, y, line, font, WHITE, BLACK);
        y += line_h;
    }
    return y;
}

// Identity body: name (large), "Table <id>", then the wrapped fun fact.
static void draw_identity_body(const char *name, const char *table,
                               const char *fun_fact, int top_y)
{
    int y = top_y;
    Paint_DrawString_P(kMargin, y, (name && name[0]) ? name : "vibamix",
                       &PoppinsSB24, WHITE, BLACK);
    y += PoppinsSB24.Height + 6;

    if (table && table[0]) {
        char tb[24];
        snprintf(tb, sizeof(tb), "Table %s", table);
        Paint_DrawString_P(kMargin, y, tb, &PoppinsMd20, WHITE, BLACK);
        y += PoppinsMd20.Height + 6;
    }
    if (fun_fact && fun_fact[0]) {
        y += 4;
        draw_wrapped(kMargin, y, kCanvasW - 2 * kMargin, fun_fact, &PoppinsMd16,
                     PoppinsMd16.Height + 3);
    }
}

void identity_screen_draw(GUI &gui, const char *name, const char *table,
                          const char *fun_fact)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    draw_identity_body(name, table, fun_fact, 12);
}

void identity_status_screen_draw(GUI &gui, const char *name, const char *table,
                                 const char *fun_fact, int batt_mv, int batt_pct,
                                 int remaining_sec, int total_sec)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    draw_status_bar(batt_mv, batt_pct, remaining_sec, total_sec);
    draw_identity_body(name, table, fun_fact, 42);
}

void config_screen_connected(GUI &gui, const char *name, const char *table,
                             int batt_mv, int batt_pct)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    // Status bar: battery (left) + "Connected" (right), divider — no countdown.
    char pc[20];
    const int icon_end = draw_battery_icon(kMargin, 4, batt_mv >= 0 ? batt_pct : 0);
    if (batt_mv >= 0) {
        snprintf(pc, sizeof(pc), "%d%% (%d.%02dV)", batt_pct,
                 batt_mv / 1000, (batt_mv % 1000) / 10);
    } else {
        snprintf(pc, sizeof(pc), "-- %%");
    }
    Paint_DrawString_P(icon_end + 3, 1, pc, &PoppinsMd16, WHITE, BLACK);
    const char *lbl = "Connected";
    const int lw = Paint_StringWidth_P(lbl, &PoppinsMd16);
    Paint_DrawString_P(kCanvasW - kMargin - lw, 1, lbl, &PoppinsMd16, WHITE, BLACK);
    fill_rect(kMargin, 24, kCanvasW - kMargin, 24);

    // Body.
    Paint_DrawString_P(kMargin, 48, "Connected", &PoppinsSB24, WHITE, BLACK);
    Paint_DrawString_P(kMargin, 80, "Editing your badge...",
                       &PoppinsMd16, WHITE, BLACK);
    if (name && name[0]) {
        Paint_DrawString_P(kMargin, 110, name, &PoppinsMd20, WHITE, BLACK);
    }
    if (table && table[0]) {
        char tb[24];
        snprintf(tb, sizeof(tb), "Table %s", table);
        Paint_DrawString_P(kMargin, 138, tb, &PoppinsMd16, WHITE, BLACK);
    }
}
