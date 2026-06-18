#include "qr_screen.h"

#include "Display_EPD_W21.h"
#include "GUI_Paint.h"
#include "badge_store.h"
#include "fonts.h"
#include "gateway_status.h"
#include "qrcodegen.h"

#include <math.h>
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

// Draw `text` word-wrapped to `maxW` pixels starting at (x, y), in proportional
// font `font`. The font is proportional, so wrap by measuring candidate lines
// (mirrors the loop in GUI::show_text). Returns the y just below the last line.
static int draw_wrapped(int x, int y, int maxW, const char *text, pFONT *font)
{
    if (!text || !text[0]) {
        return y;
    }
    const int line_h = font->Height + 2;
    char line[64];
    int n = 0;

    const char *p = text;
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

        // Build "<line> <word>" and see if it still fits within maxW.
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

        if (n > 0 && Paint_StringWidth_P(cand, font) > maxW) {
            // Doesn't fit: flush the current line, start a new one with the word.
            Paint_DrawString_P(x, y, line, font, WHITE, BLACK);
            y += line_h;
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
    if (n > 0) {
        Paint_DrawString_P(x, y, line, font, WHITE, BLACK);
        y += line_h;
    }
    return y;
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

    // Right column (fixed x so it doesn't shift with QR size): a wrapped prompt
    // and the badge id.
    const int rx = kQrBox + 8;
    const int rmaxW = kCanvasW - kMargin - rx;
    int ry = draw_wrapped(rx, body_y, rmaxW,
                          "Scan with the vibamix app to set up your badge",
                          &PoppinsMd16);
    char idline[24];
    snprintf(idline, sizeof(idline), "Badge ID: %s", code ? code : "");
    Paint_DrawString_P(rx, ry + 8, idline, &PoppinsMd16, WHITE, BLACK);
}

// Blit a 2-bit grayscale source (4 px/byte, MSB-first, row-major, level 0=black..
// 3=white — the dither.c / image_xfer authoring format) into the rect [rx,ry,rw,rh],
// scaled to fit while preserving aspect and centered. Nearest-neighbor sample,
// thresholded to 1bpp (levels 0/1 -> black).
static void draw_gray2_fit(const uint8_t *src, int sw, int sh,
                           int rx, int ry, int rw, int rh)
{
    if (!src || sw <= 0 || sh <= 0 || rw <= 0 || rh <= 0) {
        return;
    }
    int dw = rw;
    int dh = sh * rw / sw;
    if (dh > rh) {
        dh = rh;
        dw = sw * rh / sh;
    }
    if (dw <= 0 || dh <= 0) {
        return;
    }
    const int ox = rx + (rw - dw) / 2;
    const int oy = ry + (rh - dh) / 2;
    for (int dy = 0; dy < dh; dy++) {
        const int sy = dy * sh / dh;
        for (int dx = 0; dx < dw; dx++) {
            const int sx = dx * sw / dw;
            const uint32_t pi = (uint32_t)sy * sw + sx;
            const uint8_t shift = (uint8_t)((3 - (pi & 3)) * 2);
            const uint8_t val = (src[pi >> 2] >> shift) & 0x3;
            if (val < 2) {
                Paint_SetPixel(ox + dx, oy + dy, BLACK);
            }
        }
    }
}

// Height of the bottom name/ID banner on the identity frame, and the thin
// countdown fill pinned to the very bottom edge (drawn within the banner).
// 32 (not 30) so the banner is exactly 4 panel byte-columns (canvas y 144..175 ->
// cols 18..21) — lets the gray-identity path region-refresh it on whole-byte bounds.
static constexpr int kIdBannerH = 32;
static constexpr int kIdFillH   = 4;

// Bottom banner: a solid black strip across the screen bottom with `left` (the
// attendee name, or the event name when an event mesh is connected) and the
// attendee ID `right` in white.
static void draw_identity_banner(const char *left, const char *right)
{
    const int by0 = kCanvasH - kIdBannerH;

    fill_rect(0, by0, kCanvasW - 1, kCanvasH - 1);   // black strip
    Paint_DrawString_P(kMargin, by0 + 4, (left && left[0]) ? left : "vibamix",
                       &PoppinsMd20, BLACK, WHITE);   // BG black, FG white
    if (right && right[0]) {
        const int tw = Paint_StringWidth_P(right, &PoppinsMd16);
        Paint_DrawString_P(kCanvasW - kMargin - tw, by0 + 7, right,
                           &PoppinsMd16, BLACK, WHITE);
    }
}

// Thin countdown fill pinned to the very bottom edge of the banner: a white bar
// (legible over the black banner) whose width shrinks from full toward empty as
// remaining/total falls. Mirrors event_status_overlay's fill math.
static void draw_identity_countdown(int remaining_sec, int total_sec)
{
    if (total_sec <= 0) {
        total_sec = 1;
    }
    if (remaining_sec < 0) {
        remaining_sec = 0;
    } else if (remaining_sec > total_sec) {
        remaining_sec = total_sec;
    }
    const int fy0 = kCanvasH - kIdFillH;
    const int fill_w = (int)((int64_t)remaining_sec * kCanvasW / total_sec);
    for (int y = fy0; y < kCanvasH; y++) {
        for (int x = 0; x < kCanvasW; x++) {
            Paint_SetPixel(x, y, x < fill_w ? WHITE : BLACK);
        }
    }
}

// Identity body: the optional identity image fills the area above the bottom
// name/ID banner; without one the main area is left blank (white). The banner is
// always drawn at the bottom.
static void draw_identity_body(const char *name, const char *table,
                               const uint8_t *img, uint8_t img_fmt,
                               uint16_t img_w, uint16_t img_h, int top_y)
{
    if (img && img_fmt == BADGE_FMT_GRAY2 && img_w > 0 && img_h > 0) {
        const int by0 = kCanvasH - kIdBannerH;
        draw_gray2_fit(img, img_w, img_h, kMargin, top_y,
                       kCanvasW - 2 * kMargin, by0 - top_y - 2);
    }
    draw_identity_banner(name, table);
}

void identity_screen_draw(GUI &gui, const char *name, const char *table,
                          const uint8_t *img, uint8_t img_fmt,
                          uint16_t img_w, uint16_t img_h)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    draw_identity_body(name, table, img, img_fmt, img_w, img_h, kMargin);
    // Keep the "Connected" strip when this frame is force-displayed over GATT
    // (no-op when not connected).
    gateway_status_overlay(fb);
}

void identity_banner_over(GUI &gui, const char *name, const char *table)
{
    uint8_t *fb = gui.framebuffer();

    // Bind Paint to the already-filled framebuffer (a full-screen B/W identity image
    // blitted in) WITHOUT clearing, then draw the opaque banner over its bottom.
    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    draw_identity_banner(name, table);
    gateway_status_overlay(fb);   // "Connected" strip when force-displayed (else no-op)
}

void identity_countdown_overlay(GUI &gui, const char *name, const char *table,
                                const char *event_name, int remaining_sec,
                                int total_sec)
{
    uint8_t *fb = gui.framebuffer();

    // Re-bind Paint to the already-built identity frame without clearing it, then
    // repaint only the bottom banner + countdown fill. When an event mesh is
    // connected the banner's left text is the event name; otherwise the attendee
    // name. Idempotent, so repeated partial refreshes don't smear.
    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    const char *left = (event_name && event_name[0]) ? event_name : name;
    draw_identity_banner(left, table);
    draw_identity_countdown(remaining_sec, total_sec);
}

// The "asleep" indicator (top-right): a crescent moon with three ascending z's to
// its left. Drawn into the currently-bound Paint framebuffer. White halos/backgrounds
// keep it legible over a blank corner or a dark image.
static void draw_sleep_indicator(void)
{
    // Three ascending z's (small mono font, white box behind each for legibility).
    Paint_DrawString_EN(202, 26, "z", &Font12, WHITE, BLACK);
    Paint_DrawString_EN(212, 17, "z", &Font12, WHITE, BLACK);
    Paint_DrawString_EN(222, 8,  "z", &Font12, WHITE, BLACK);

    // Crescent moon: white halo, black disc, then a white disc offset up-right to
    // carve the crescent opening (same circle-carve trick as draw_bt_badge).
    const int mx = kCanvasW - 16, my = 20, r = 11;
    Paint_DrawCircle(mx, my, r + 2, WHITE, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    Paint_DrawCircle(mx, my, r, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    Paint_DrawCircle(mx + 5, my - 4, r, WHITE, DRAW_FILL_FULL, DOT_PIXEL_1X1);
}

void identity_sleep_overlay(GUI &gui)
{
    uint8_t *fb = gui.framebuffer();

    // Re-bind Paint to the already-built identity frame without clearing it, then
    // composite the asleep indicator into the top-right corner.
    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    draw_sleep_indicator();
}

// Set the 2-bit grayscale level (0..3) of source pixel (x,y) in a w-wide image.
static inline void gray2_set_level(uint8_t *src, uint16_t w, int x, int y, uint8_t level)
{
    const uint32_t pi = (uint32_t)y * w + x;
    const uint8_t  shift = (uint8_t)((3 - (pi & 3)) * 2);
    src[pi >> 2] = (uint8_t)((src[pi >> 2] & ~(0x3 << shift)) | (level << shift));
}

// Bake the bottom name/ID banner (opaque) and, if sleeping, the asleep moon (black
// overlay) into a 2-bit grayscale source, so a single 4-gray render shows the
// full-screen image with the banner/moon over it (a 1-bit partial overlay can't run
// over a 4-gray base — it would re-drive every mid-gray pixel). `scratch` is a 1-bit
// work framebuffer (the GUI framebuffer); it is overwritten. Scratch pixels are read
// via the ROTATE_270 mapping that matches gray2_to_plane / Paint.
void identity_bake_overlays(uint8_t *src, uint16_t w, uint16_t h, uint8_t *scratch,
                            const char *name, const char *table, bool sleeping)
{
    if (!src || w == 0 || h == 0) {
        return;
    }
    const int cw = (w < kCanvasW) ? (int)w : kCanvasW;
    const int ch = (h < kCanvasH) ? (int)h : kCanvasH;
    const int wbyte = EPD_WIDTH / 8;   // 22 bytes per panel row

    // Draw the 1-bit banner (+ moon) into the scratch framebuffer.
    Paint_NewImage(scratch, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    draw_identity_banner(name, table);
    if (sleeping) {
        draw_sleep_indicator();
    }

    // Opaque banner: every pixel in the bottom strip -> white(3)/black(0).
    for (int y = kCanvasH - kIdBannerH; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            const int idx = (kCanvasW - 1 - x) * wbyte + (y >> 3);
            const bool white = scratch[idx] & (0x80 >> (y & 7));
            gray2_set_level(src, w, x, y, white ? 3 : 0);
        }
    }

    // Asleep moon: black-only overlay (leave the gray image where the glyph isn't
    // black, so we don't stamp a white box over the corner).
    if (sleeping) {
        for (int y = 0; y < 44 && y < ch; y++) {
            for (int x = 198; x < cw; x++) {
                const int idx = (kCanvasW - 1 - x) * wbyte + (y >> 3);
                if (scratch[idx] & (0x80 >> (y & 7))) {
                    continue;   // white -> keep the image
                }
                gray2_set_level(src, w, x, y, 0);
            }
        }
    }
}

void identity_status_screen_draw(GUI &gui, const char *name, const char *table,
                                 int batt_mv, int batt_pct,
                                 int remaining_sec, int total_sec)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    draw_status_bar(batt_mv, batt_pct, remaining_sec, total_sec);
    draw_identity_body(name, table, nullptr, 0, 0, 0, 42);
}

// A clean "settings" gear centered at (cx, cy), rasterized in polar form so the
// teeth are crisp radial spokes (not square blobs): the body is a solid ring out
// to r_root, then n_teeth evenly spaced teeth extend out to r_tip, with a punched
// white center hole. Runs over a small box once per refresh, so the per-pixel
// trig is cheap.
static void draw_gear(int cx, int cy, int r_tip, int hole_r)
{
    constexpr float kPi = 3.14159265f;
    constexpr int   n_teeth = 8;
    constexpr float duty = 0.5f;          // solid fraction of each tooth pitch at the root
    constexpr float kTaper = 0.06f;       // how much the tooth narrows from root to tip
    const float r_root = r_tip * 0.74f;

    for (int y = cy - r_tip; y <= cy + r_tip; y++) {
        for (int x = cx - r_tip; x <= cx + r_tip; x++) {
            const float dx = (float)(x - cx);
            const float dy = (float)(y - cy);
            const float dist = sqrtf(dx * dx + dy * dy);
            if (dist > (float)r_tip || dist <= (float)hole_r) {
                continue;                 // outside the tip / inside the hole
            }
            bool solid;
            if (dist <= r_root) {
                solid = true;             // body ring
            } else {
                float a = atan2f(dy, dx) / (2.0f * kPi);   // -0.5 .. 0.5
                if (a < 0.0f) {
                    a += 1.0f;
                }
                float frac = a * n_teeth;
                frac -= (float)(int)frac; // position within this tooth pitch [0,1)
                // Taper the tooth: shrink its angular half-width with radius so the
                // side walls converge inward (a real gear tooth) instead of being
                // radial/constant-width (which flares the tooth wider toward the rim
                // and meets the tip at an obtuse corner). Centered at frac = duty/2.
                const float t = (dist - r_root) / (float)(r_tip - r_root); // 0..1
                const float half = (duty * 0.5f) - kTaper * t;
                solid = fabsf(frac - duty * 0.5f) < half;
            }
            if (solid) {
                Paint_SetPixel(x, y, BLACK);
            }
        }
    }
}

// The Bluetooth rune in `color`, centered at (cx, cy): a vertical spine, top/
// bottom strokes to the right peaks, and the two long crossing diagonals — the
// bind rune that reads as the Bluetooth glyph. `half_h` is half the rune height.
static void draw_bt_glyph(int cx, int cy, int half_h, int color)
{
    const int d = half_h / 2;            // half width
    const int q = half_h / 2;            // peak y-offset from center
    const int ax = cx, ay0 = cy - half_h, ay1 = cy + half_h;   // spine ends
    const int prx = cx + d;              // right peaks x
    const int plx = cx - d;              // left ends x
    const int py0 = cy - q, py1 = cy + q;

    Paint_DrawLine(ax, ay0, ax, ay1, color, LINE_STYLE_SOLID, DOT_PIXEL_1X1); // spine
    Paint_DrawLine(ax, ay0, prx, py0, color, LINE_STYLE_SOLID, DOT_PIXEL_1X1); // top->UR
    Paint_DrawLine(ax, ay1, prx, py1, color, LINE_STYLE_SOLID, DOT_PIXEL_1X1); // bot->LR
    Paint_DrawLine(prx, py0, plx, py1, color, LINE_STYLE_SOLID, DOT_PIXEL_1X1); // UR->LL
    Paint_DrawLine(prx, py1, plx, py0, color, LINE_STYLE_SOLID, DOT_PIXEL_1X1); // LR->UL
}

// Small Bluetooth "connected" badge: a filled black disc with the rune in white,
// tucked over the gear's lower-right to read as config + connected. A white halo
// ring separates the disc from the (also black) gear body it sits on.
static void draw_bt_badge(int cx, int cy, int r)
{
    Paint_DrawCircle(cx, cy, r + 2, WHITE, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    Paint_DrawCircle(cx, cy, r, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    draw_bt_glyph(cx, cy, r * 3 / 5, WHITE);
}

void config_screen_connected(GUI &gui, const char *name, const char *table,
                             int batt_mv, int batt_pct, bool app_alive, bool blink)
{
    uint8_t *fb = gui.framebuffer();

    Paint_NewImage(fb, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);

    // Top status bar: battery (left), keepalive pulse dot (right), divider.
    char pc[20];
    const int icon_end = draw_battery_icon(kMargin, 4, batt_mv >= 0 ? batt_pct : 0);
    if (batt_mv >= 0) {
        snprintf(pc, sizeof(pc), "%d%% (%d.%02dV)", batt_pct,
                 batt_mv / 1000, (batt_mv % 1000) / 10);
    } else {
        snprintf(pc, sizeof(pc), "-- %%");
    }
    Paint_DrawString_P(icon_end + 3, 1, pc, &PoppinsMd16, WHITE, BLACK);

    // Keepalive dot (top-right): pulses solid/hollow each second while the app's
    // keepalive writes keep arriving; stays hollow (no pulse) if stale.
    const int dx1 = kCanvasW - kMargin;
    const int dx0 = dx1 - 8;
    if (app_alive && blink) {
        fill_rect(dx0, 5, dx1, 13);
    } else {
        draw_rect(dx0, 5, dx1, 13);
    }

    fill_rect(kMargin, 24, kCanvasW - kMargin, 24);

    // Left: large config gear with a Bluetooth "connected" badge at its corner.
    const int gx = 52, gy = 90, gr = 34;
    draw_gear(gx, gy, gr, gr / 3);
    draw_bt_badge(gx + 24, gy + 24, 14);

    // Right column: heading + name + what-to-do / how-to-exit instructions.
    const int rx = 104;
    const int rmaxW = kCanvasW - kMargin - rx;
    Paint_DrawString_P(rx, 32, "Connected", &PoppinsSB24, WHITE, BLACK);
    int ry = 70;
    if (name && name[0]) {
        Paint_DrawString_P(rx, ry, name, &PoppinsMd20, WHITE, BLACK);
        ry += PoppinsMd20.Height + 6;
    }
    if (gateway_status_count() > 0) {
        // Acting as a mesh gateway: show the last command relayed to the fleet.
        char bc[48];
        snprintf(bc, sizeof(bc), "Broadcast: %s  x%u",
                 gateway_status_label(), (unsigned)gateway_status_count());
        draw_wrapped(rx, ry, rmaxW, bc, &PoppinsMd16);
    } else {
        draw_wrapped(rx, ry, rmaxW,
                     "Edit your name, facts & image from the vibamix app.",
                     &PoppinsMd16);
    }

    // Bottom bar: an inverted strip with the exit instruction, so it's unmissable.
    const int bar_h = 24;
    const int by0 = kCanvasH - bar_h;
    fill_rect(0, by0, kCanvasW - 1, kCanvasH - 1);
    const char *exit_lbl = "Press the button to exit";
    const int ew = Paint_StringWidth_P(exit_lbl, &PoppinsMd16);
    Paint_DrawString_P((kCanvasW - ew) / 2, by0 + 4, exit_lbl,
                       &PoppinsMd16, BLACK, WHITE);
}
