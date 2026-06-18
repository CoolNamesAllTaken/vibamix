#include "dither.h"

#include <string.h>

#define FB_BYTES   5808 /* 176*264/8 */
#define CANVAS_W   264  /* ROTATE_270 landscape width  */
#define CANVAS_H   176  /* ROTATE_270 landscape height */
#define WIDTH_BYTE 22   /* bytes per panel row (176/8) */

/* Standard 4x4 Bayer threshold matrix (values 0..15). */
static const uint8_t bayer4[4][4] = {
	{ 0,  8,  2, 10 },
	{ 12, 4, 14,  6 },
	{ 3, 11,  1,  9 },
	{ 15, 7, 13,  5 },
};

void dither_2bit_to_fb(const uint8_t *src, uint16_t w, uint16_t h, uint8_t *fb)
{
	memset(fb, 0xFF, FB_BYTES); /* white background; black pixels clear bits */

	if (w > CANVAS_W) {
		w = CANVAS_W;
	}
	if (h > CANVAS_H) {
		h = CANVAS_H;
	}

	for (uint16_t dy = 0; dy < h; dy++) {
		for (uint16_t dx = 0; dx < w; dx++) {
			uint32_t pi = (uint32_t)dy * w + dx;
			uint8_t  shift = (uint8_t)((3 - (pi & 3)) * 2);
			uint8_t  val = (uint8_t)((src[pi >> 2] >> shift) & 0x3);

			/* level 0..3 -> intensity 0/85/170/255; white if it beats the
			 * ordered threshold. level 0 is always black, level 3 always white. */
			int intensity = val * 85;
			int thr = bayer4[dy & 3][dx & 3] * 16; /* 0..240 */
			if (intensity > thr) {
				continue; /* white — bit already set */
			}

			int idx = (CANVAS_W - 1 - dx) * WIDTH_BYTE + (dy >> 3);
			fb[idx] &= (uint8_t)~(0x80 >> (dy & 7));
		}
	}
}

void gray2_to_plane(const uint8_t *src, uint16_t w, uint16_t h, uint8_t *plane, int which)
{
	/* RAM bit 1 = waveform "condition false"; we clear bits where it's true. This is
	 * the inverse polarity of the B/W framebuffer, matching the reference's ~out_byte. */
	memset(plane, 0xFF, FB_BYTES);

	if (w > CANVAS_W) {
		w = CANVAS_W;
	}
	if (h > CANVAS_H) {
		h = CANVAS_H;
	}

	for (uint16_t dy = 0; dy < h; dy++) {
		for (uint16_t dx = 0; dx < w; dx++) {
			uint32_t pi = (uint32_t)dy * w + dx;
			uint8_t  shift = (uint8_t)((3 - (pi & 3)) * 2);
			uint8_t  val = (uint8_t)((src[pi >> 2] >> shift) & 0x3);

			/* which==0 -> MSB plane (RAM 0x26); which==1 -> LSB plane (RAM 0x24).
			 * level 0=black .. 3=white: white clears both planes, black clears
			 * neither, the two mids clear exactly one (4 LUT waveforms -> 4 grays). */
			int cond = which ? (val & 1) : ((val >> 1) & 1);
			if (cond) {
				int idx = (CANVAS_W - 1 - dx) * WIDTH_BYTE + (dy >> 3);
				plane[idx] &= (uint8_t)~(0x80 >> (dy & 7));
			}
		}
	}
}
