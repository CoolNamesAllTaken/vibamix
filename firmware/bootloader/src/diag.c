/*
 * Bootloader ePaper diagnostics — see diag.h.
 *
 * Renders the boot decision + per-slot bl_state into a 1bpp framebuffer with a
 * tiny built-in 5x7 font, then full-refreshes the panel via epd_min. Self
 * contained C (no GUI_Paint, no flash/vbx deps) so it fits the 48 KB bootloader.
 */

#include "diag.h"
#include "epd_min.h"

#include <stdint.h>
#include <string.h>
#include <zephyr/sys/printk.h>

/* Logical drawing space: x across the 176-px axis, y down the 264-px axis. The
 * panel framebuffer is written linearly to RAM 0x24; a pixel (x,y) lands at
 * byte (263-y)*22 + (x>>3), bit 0x80>>(x&7), where bit 1 = white. */
#define EPD_W 176
#define EPD_H 264

static uint8_t fb[EPD_MIN_FB_SIZE];

static void set_black(int x, int y)
{
	if ((unsigned)x >= EPD_W || (unsigned)y >= EPD_H) {
		return;
	}
	unsigned idx = (unsigned)(EPD_H - 1 - y) * 22u + ((unsigned)x >> 3);
	fb[idx] &= ~(0x80u >> (x & 7)); /* clear bit -> black */
}

/* 5x7 font, column-major, bit0 = top row. Covers the glyphs the diagnostics use;
 * unknown chars render blank. Values are the classic 5x7 console font. */
struct glyph { char c; uint8_t col[5]; };

static const struct glyph FONT[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
	{'!', {0x00, 0x00, 0x5F, 0x00, 0x00}},
	{'#', {0x14, 0x7F, 0x14, 0x7F, 0x14}},
	{'%', {0x23, 0x13, 0x08, 0x64, 0x62}},
	{'(', {0x00, 0x1C, 0x22, 0x41, 0x00}},
	{')', {0x00, 0x41, 0x22, 0x1C, 0x00}},
	{'+', {0x08, 0x08, 0x3E, 0x08, 0x08}},
	{'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
	{'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
	{'/', {0x20, 0x10, 0x08, 0x04, 0x02}},
	{'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
	{'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
	{'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
	{'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
	{'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
	{'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
	{'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
	{'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
	{'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
	{'9', {0x06, 0x49, 0x49, 0x29, 0x1E}},
	{':', {0x00, 0x36, 0x36, 0x00, 0x00}},
	{'=', {0x14, 0x14, 0x14, 0x14, 0x14}},
	{'>', {0x00, 0x41, 0x22, 0x14, 0x08}},
	{'?', {0x02, 0x01, 0x51, 0x09, 0x06}},
	{'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
	{'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
	{'C', {0x3E, 0x41, 0x41, 0x41, 0x22}},
	{'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
	{'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
	{'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
	{'G', {0x3E, 0x41, 0x49, 0x49, 0x7A}},
	{'H', {0x7F, 0x08, 0x08, 0x08, 0x7F}},
	{'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
	{'J', {0x20, 0x40, 0x41, 0x3F, 0x01}},
	{'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
	{'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
	{'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
	{'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
	{'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
	{'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
	{'Q', {0x3E, 0x41, 0x51, 0x21, 0x5E}},
	{'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
	{'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
	{'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
	{'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
	{'V', {0x1F, 0x20, 0x40, 0x20, 0x1F}},
	{'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
	{'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
	{'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
	{'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
};

static const uint8_t *glyph_for(char c)
{
	if (c >= 'a' && c <= 'z') {
		c -= 32; /* fold lowercase to the uppercase glyphs */
	}
	for (size_t i = 0; i < sizeof(FONT) / sizeof(FONT[0]); i++) {
		if (FONT[i].c == c) {
			return FONT[i].col;
		}
	}
	return FONT[0].col; /* space */
}

#define CHAR_W 6 /* 5 px glyph + 1 px gap */
#define CHAR_H 8 /* 7 px glyph + 1 px gap */

static void draw_char(int x, int y, char c)
{
	const uint8_t *g = glyph_for(c);

	for (int col = 0; col < 5; col++) {
		uint8_t bits = g[col];
		for (int row = 0; row < 7; row++) {
			if (bits & (1u << row)) {
				set_black(x + col, y + row);
			}
		}
	}
}

static void draw_str(int x, int y, const char *s)
{
	for (; *s; s++) {
		if (x + CHAR_W > EPD_W) {
			break; /* clip at right edge */
		}
		draw_char(x, y, *s);
		x += CHAR_W;
	}
}

/* Minimal unsigned-decimal formatter (no stdio in this tiny image). */
static void u32_to_dec(uint32_t v, char *out)
{
	char tmp[10];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v && n < (int)sizeof(tmp));

	int i = 0;
	while (n > 0) {
		out[i++] = tmp[--n];
	}
	out[i] = '\0';
}

/* Build "<label><uint>" into buf. */
static void draw_kv_u32(int x, int y, const char *label, uint32_t v)
{
	char num[12];

	u32_to_dec(v, num);
	draw_str(x, y, label);
	draw_str(x + (int)strlen(label) * CHAR_W, y, num);
}

void bl_diag_show(const struct bl_diag *d)
{
	if (epd_min_init() < 0) {
		printk("bl: diag: panel init failed\n");
		return;
	}

	memset(fb, 0xFF, sizeof(fb)); /* white background */

	int y = 2;

	draw_str(2, y, "VBX BOOTLOADER");
	y += CHAR_H;
	draw_str(2, y, d->reason ? d->reason : "");
	y += CHAR_H + 2;

	if (d->chosen >= 0) {
		char line[24];

		strcpy(line, d->trial ? "TRIAL SLOT " : "BOOT SLOT ");
		size_t l = strlen(line);
		line[l++] = (char)('A' + d->chosen);
		line[l] = '\0';
		draw_str(2, y, line);
	} else {
		draw_str(2, y, "HALT NO IMAGE");
	}
	y += CHAR_H;

	if (d->have_state) {
		draw_kv_u32(2, y, "STATE SEQ ", d->seq);
	} else {
		draw_str(2, y, "STATE NONE");
	}
	y += CHAR_H + 2;

	for (int s = 0; s < BL_DIAG_NUM_SLOTS; s++) {
		const struct bl_diag_slot *m = &d->slot[s];
		char hdr[8];

		strcpy(hdr, "SLOT ");
		hdr[5] = (char)('A' + s);
		hdr[6] = '\0';
		draw_str(2, y, hdr);
		if (s == d->chosen) {
			draw_str(2 + (int)strlen(hdr) * CHAR_W, y, ">"); /* booted slot */
		}
		y += CHAR_H;

		draw_str(8, y, m->valid ? "VALID" : "NOVALID");
		draw_str(8 + 8 * CHAR_W, y, m->crc_ok ? "CRC OK" : "CRC FAIL");
		y += CHAR_H;

		draw_kv_u32(8, y, "VER ", m->version);
		y += CHAR_H;

		draw_kv_u32(8, y, "LEN ", m->image_len);
		y += CHAR_H;

		draw_kv_u32(8, y, "ATT ", m->attempts);
		draw_str(8 + 8 * CHAR_W, y, m->confirmed ? "CONF" : "UNCONF");
		y += CHAR_H + 2;
	}

	epd_min_show(fb);
}
