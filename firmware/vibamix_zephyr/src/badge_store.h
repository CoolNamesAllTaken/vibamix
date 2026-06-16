#ifndef VIBAMIX_BADGE_STORE_H
#define VIBAMIX_BADGE_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent full-screen image slots, stored raw in the `images` flash partition
 * (one 16 KB slot per index, via <zephyr/storage/flash_map.h>). Each slot holds
 * either a 2-bit grayscale image or a 1-bit B/W image (per-slot toggle); the
 * format is recorded in a 16-byte slot header so the display path knows whether
 * to dither (gray) or blit directly (B/W).
 *
 * Image payloads are panel-sized (176x264): 1-bit = 5808 B, 2-bit = 11616 B.
 */

#define BADGE_IMAGE_SLOTS 4
#define BADGE_SLOT_NONE   0xFF /* render-only, do not persist to a slot */

#define BADGE_FMT_BW    1 /* 1-bit B/W, panel-native (5808 B) — display by direct blit */
#define BADGE_FMT_GRAY2 2 /* 2-bit grayscale (11616 B) — display by on-device dither */

#define BADGE_IMG_BW_BYTES    5808  /* 176*264/8 */
#define BADGE_IMG_GRAY2_BYTES 11616 /* 176*264*2/8 */

/* Open the images flash area. Returns 0 on success. */
int badge_store_init(void);

/* Erase slot and write a new image (payload first, header last). `len` must be a
 * multiple of 16 (both payload sizes are). Returns 0 on success. */
int badge_store_image_write(uint8_t slot, uint8_t fmt, const uint8_t *data, size_t len,
			    uint16_t w, uint16_t h, uint32_t crc);

/* Read a stored image into `buf` (>= len). Outputs are optional (may be NULL).
 * Returns 0 on success, -ENOENT if the slot is empty/invalid. */
int badge_store_image_read(uint8_t slot, uint8_t *buf, size_t cap, uint8_t *fmt,
			   size_t *out_len, uint16_t *w, uint16_t *h);

/* True if the slot holds a valid image (header magic matches). */
bool badge_store_image_present(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_BADGE_STORE_H */
