#ifndef VBX_IMG_H
#define VBX_IMG_H

/*
 * Vibamix app image trailer for the custom direct-XIP bootloader.
 *
 * Built images get a 32-byte trailer appended after the raw image (by
 * scripts/vbx_trailer.py at build time, or written by the OTA receiver). The
 * bootloader finds the trailer at slot_base + image_len (image_len comes from
 * bl_state), checks the magic, and CRC-verifies the image before jumping.
 *
 * RRAM is memory-mapped (XIP) at the slot's flash offset, so both the trailer
 * and the image bytes can be read through plain pointers.
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/crc.h>

#define VBX_IMG_MAGIC        0x58474D49u /* 'IMGX' */
#define VBX_IMG_TRAILER_SIZE 32u

struct __packed vbx_img_trailer {
	uint32_t magic;       /* VBX_IMG_MAGIC */
	uint32_t image_len;   /* bytes of image preceding this trailer */
	uint32_t crc32;       /* crc32_ieee over the first image_len bytes */
	uint32_t version;     /* monotonic build/seq number */
	uint16_t hdr_version; /* trailer format version (1) */
	uint16_t reserved;
	uint8_t  pad[12];     /* -> 32 bytes, 16-aligned */
};

BUILD_ASSERT(sizeof(struct vbx_img_trailer) == VBX_IMG_TRAILER_SIZE,
	     "vbx_img_trailer must be 32 bytes");

/* Verify a memory-mapped image at `base` whose trailer is at base+image_len.
 * Returns true (and fills *out_version) on magic + length + CRC match. */
static inline bool vbx_img_verify(const uint8_t *base, uint32_t image_len,
				  uint32_t *out_version)
{
	if (image_len == 0 || image_len == 0xFFFFFFFFu) {
		return false;
	}
	const struct vbx_img_trailer *t =
		(const struct vbx_img_trailer *)(base + image_len);
	if (t->magic != VBX_IMG_MAGIC || t->image_len != image_len) {
		return false;
	}
	if (crc32_ieee(base, image_len) != t->crc32) {
		return false;
	}
	if (out_version) {
		*out_version = t->version;
	}
	return true;
}

#endif /* VBX_IMG_H */
