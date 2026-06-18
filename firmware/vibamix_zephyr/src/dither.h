#ifndef VIBAMIX_DITHER_H
#define VIBAMIX_DITHER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dither a 2-bit grayscale image to the 1bpp panel framebuffer (5808 bytes,
 * 176x264) using an ordered 4x4 Bayer matrix. The source is authored on the
 * ROTATE_270 landscape canvas (w up to 264, h up to 176), packed 4 pixels/byte,
 * MSB-first, row-major (pixel_index = y*w + x; level 0=black .. 3=white).
 * `fb` must point at a 5808-byte buffer; it is fully written (white background
 * then black pixels cleared) to match the panel bit layout (bit 1 = white).
 */
void dither_2bit_to_fb(const uint8_t *src, uint16_t w, uint16_t h, uint8_t *fb);

/*
 * Build one 1bpp bitplane (5808 bytes) for real 4-level grayscale display. Same
 * source format and panel orientation as dither_2bit_to_fb, but instead of
 * dithering, emit one bit of each 2-bit pixel: `which`=0 -> MSB plane (RAM 0x26),
 * `which`=1 -> LSB plane (RAM 0x24). The two planes drive the SSD1680 4-gray
 * waveform LUT (see EPD_Init_4Gray / EPD_WritePlane). `plane` is fully written.
 */
void gray2_to_plane(const uint8_t *src, uint16_t w, uint16_t h, uint8_t *plane, int which);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_DITHER_H */
