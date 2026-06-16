#include "badge_store.h"

#include <errno.h>
#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

#define SLOT_SIZE 0x4000u            /* 16 KB per slot (matches the DTS partition) */
#define HDR_SIZE  16u
#define IMG_MAGIC 0x474D4956u        /* 'VIMG' */

/* Exactly 16 bytes, RRAM write-line aligned. Header is written last so a torn
 * write leaves an invalid magic rather than a half-image marked valid. */
struct __packed img_hdr {
	uint32_t magic;
	uint8_t  fmt;
	uint8_t  rsvd;
	uint16_t w;
	uint16_t h;
	uint16_t len;
	uint32_t crc;
};
BUILD_ASSERT(sizeof(struct img_hdr) == HDR_SIZE, "img_hdr must be 16 bytes");

static const struct flash_area *s_fa;

int badge_store_init(void)
{
	int rc = flash_area_open(FIXED_PARTITION_ID(images_partition), &s_fa);
	if (rc) {
		printk("badge_store: flash_area_open failed (%d)\n", rc);
		s_fa = NULL;
	}
	return rc;
}

static bool read_hdr(uint8_t slot, struct img_hdr *hdr)
{
	if (!s_fa || slot >= BADGE_IMAGE_SLOTS) {
		return false;
	}
	if (flash_area_read(s_fa, (off_t)slot * SLOT_SIZE, hdr, sizeof(*hdr)) != 0) {
		return false;
	}
	return hdr->magic == IMG_MAGIC;
}

int badge_store_image_write(uint8_t slot, uint8_t fmt, const uint8_t *data, size_t len,
			    uint16_t w, uint16_t h, uint32_t crc)
{
	if (!s_fa || slot >= BADGE_IMAGE_SLOTS) {
		return -EINVAL;
	}
	if (len == 0 || (len % 16) != 0 || HDR_SIZE + len > SLOT_SIZE) {
		return -EINVAL;
	}

	const off_t base = (off_t)slot * SLOT_SIZE;
	int rc = flash_area_erase(s_fa, base, SLOT_SIZE);
	if (rc) {
		printk("badge_store: erase slot %u failed (%d)\n", slot, rc);
		return rc;
	}
	rc = flash_area_write(s_fa, base + HDR_SIZE, data, len);
	if (rc) {
		printk("badge_store: write payload slot %u failed (%d)\n", slot, rc);
		return rc;
	}

	struct img_hdr hdr = {
		.magic = IMG_MAGIC,
		.fmt = fmt,
		.rsvd = 0,
		.w = w,
		.h = h,
		.len = (uint16_t)len,
		.crc = crc,
	};
	rc = flash_area_write(s_fa, base, &hdr, sizeof(hdr));
	if (rc) {
		printk("badge_store: write header slot %u failed (%d)\n", slot, rc);
		return rc;
	}
	printk("badge_store: slot %u written (fmt %u, %u B)\n", slot, fmt, (unsigned)len);
	return 0;
}

int badge_store_image_read(uint8_t slot, uint8_t *buf, size_t cap, uint8_t *fmt,
			   size_t *out_len, uint16_t *w, uint16_t *h)
{
	struct img_hdr hdr;

	if (!read_hdr(slot, &hdr)) {
		return -ENOENT;
	}
	if (hdr.len > cap) {
		return -ENOMEM;
	}
	int rc = flash_area_read(s_fa, (off_t)slot * SLOT_SIZE + HDR_SIZE, buf, hdr.len);
	if (rc) {
		return rc;
	}
	if (fmt) {
		*fmt = hdr.fmt;
	}
	if (out_len) {
		*out_len = hdr.len;
	}
	if (w) {
		*w = hdr.w;
	}
	if (h) {
		*h = hdr.h;
	}
	return 0;
}

bool badge_store_image_present(uint8_t slot)
{
	struct img_hdr hdr;

	return read_hdr(slot, &hdr);
}
