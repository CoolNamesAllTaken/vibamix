#ifndef VBX_BL_EPD_MIN_H
#define VBX_BL_EPD_MIN_H

/*
 * Minimal, self-contained GDEY027T91 (SSD1680) driver for the bootloader.
 *
 * The app has a full C++ ePaper + GUI_Paint stack, but that won't fit in the
 * 48 KB bootloader_partition. This is a stripped C driver: GPIO + SPI from the
 * same board DT nodes (epaper / spi00), HW init, and a single full-refresh of a
 * 5808-byte 1bpp framebuffer. Used only to draw a boot diagnostic screen.
 */

#include <stdint.h>

#define EPD_MIN_FB_SIZE 5808 /* 176x264 1bpp: 264 rows * 22 bytes */

/* Reset + init the panel. Returns 0 on success, <0 if SPI/GPIO not ready or the
 * BUSY line never clears (stuck panel). */
int epd_min_init(void);

/* Full-refresh the panel with fb[EPD_MIN_FB_SIZE] (bit 1 = white). Returns 0, or
 * <0 if BUSY times out. Call epd_min_init() first. */
int epd_min_show(const uint8_t *fb);

#endif /* VBX_BL_EPD_MIN_H */
