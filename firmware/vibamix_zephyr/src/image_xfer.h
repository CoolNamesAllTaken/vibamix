#ifndef VIBAMIX_IMAGE_XFER_H
#define VIBAMIX_IMAGE_XFER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reassembles an ePaper framebuffer pushed over the mesh in chunks
 * (IMG_START -> N x IMG_DATA -> IMG_END). Chunks are written straight into the
 * display framebuffer supplied by image_xfer_init(), so there is no second
 * 5.8 KB buffer. On a verified IMG_END the completion callback fires.
 */

/* Called once the full image is received and its CRC matches. `buf` is the
 * buffer passed to image_xfer_init(). `slot`/`fmt` are echoed from START — used
 * by the GATT grayscale path to pick a storage slot + format (BADGE_FMT_*); the
 * legacy 1bpp framebuffer path ignores them. */
typedef void (*image_complete_cb)(uint8_t slot, uint8_t fmt, const uint8_t *buf,
				  size_t len, uint16_t width, uint16_t height);

/* Bind the destination buffer (framebuffer or grayscale staging) and capacity. */
void image_xfer_init(uint8_t *buf, size_t cap);

void image_xfer_set_complete_cb(image_complete_cb cb);

/* Opcode handlers — called from the mesh vendor model and the GATT service.
 * `slot`/`fmt` are carried through to the completion callback. */
void image_xfer_start(uint8_t slot, uint8_t fmt, uint16_t size,
		      uint16_t width, uint16_t height);
void image_xfer_data(uint16_t offset, const uint8_t *data, uint16_t len);
void image_xfer_end(uint32_t crc);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_IMAGE_XFER_H */
