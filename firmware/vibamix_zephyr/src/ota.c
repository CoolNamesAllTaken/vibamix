#include "ota.h"
#include "slots.h"

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include "bl_state.h"
#include "vbx_img.h"

/*
 * Direct-XIP OTA receiver. The host streams the already-trailered image built
 * for the *inactive* slot (slotA.bin / slotB.bin = raw image + 32-byte trailer).
 * We write it raw into the inactive slot via flash_area, CRC-verify it in place
 * (RRAM is memory-mapped), mark the slot pending in bl_state, and reboot. The
 * bootloader picks it up, re-verifies, and boots it; the active slot is never
 * touched, so a power fail mid-OTA simply leaves the running image intact.
 */

/* nRF54L15 RRAM write-block is 16 bytes; flash_area_write must be aligned. */
#define WB 16u

static const struct flash_area *s_fa;
static bool     s_active;
static uint32_t s_total;     /* image + trailer (what the host sends) */
static uint32_t s_written;   /* bytes accepted from the host */
static uint32_t s_flushed;   /* bytes actually committed to flash */
static uint8_t  s_acc[WB];   /* partial write-block accumulator */
static size_t   s_acc_len;

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("ota: rebooting to apply update\n");
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(s_reboot_work, reboot_work_fn);

static void ota_abort(void)
{
	s_active = false;
	if (s_fa) {
		flash_area_close(s_fa);
		s_fa = NULL;
	}
}

int ota_begin(uint32_t total_size)
{
	ota_abort();

	int err = flash_area_open(OTHER_SLOT_FA_ID, &s_fa);
	if (err) {
		printk("ota: flash_area_open failed (%d)\n", err);
		return err;
	}
	if (total_size == 0 || total_size > s_fa->fa_size) {
		printk("ota: bad size %u (slot %u)\n",
		       (unsigned)total_size, (unsigned)s_fa->fa_size);
		ota_abort();
		return -EINVAL;
	}

	/* Erase enough erase-blocks to hold the image. */
	size_t erase = ROUND_UP(total_size, 4096u);
	err = flash_area_erase(s_fa, 0, erase);
	if (err) {
		printk("ota: erase failed (%d)\n", err);
		ota_abort();
		return err;
	}

	s_total = total_size;
	s_written = 0;
	s_flushed = 0;
	s_acc_len = 0;
	s_active = true;
	printk("ota: begin, %u bytes -> slot %u\n",
	       (unsigned)total_size, (unsigned)OTHER_SLOT);
	return 0;
}

/* Commit as many whole write-blocks as possible from the accumulator + data. */
static int flush_aligned(const uint8_t *data, size_t len)
{
	/* Top up the accumulator first. */
	while (s_acc_len < WB && len > 0) {
		s_acc[s_acc_len++] = *data++;
		len--;
	}
	if (s_acc_len == WB) {
		int err = flash_area_write(s_fa, s_flushed, s_acc, WB);
		if (err) {
			return err;
		}
		s_flushed += WB;
		s_acc_len = 0;
	}

	/* Bulk-write the aligned middle directly from the caller's buffer. */
	size_t bulk = len & ~(size_t)(WB - 1);
	if (bulk) {
		int err = flash_area_write(s_fa, s_flushed, data, bulk);
		if (err) {
			return err;
		}
		s_flushed += bulk;
		data += bulk;
		len -= bulk;
	}

	/* Stash the remainder for the next call. */
	if (len) {
		memcpy(s_acc, data, len);
		s_acc_len = len;
	}
	return 0;
}

int ota_write(uint32_t offset, const uint8_t *data, uint16_t len)
{
	if (!s_active) {
		return -EINVAL;
	}
	if (offset != s_written) {
		printk("ota: offset gap (got %u, expected %u)\n",
		       (unsigned)offset, (unsigned)s_written);
		ota_abort();
		return -EINVAL;
	}
	if (s_written + len > s_total) {
		printk("ota: overflow (%u + %u > %u)\n",
		       (unsigned)s_written, (unsigned)len, (unsigned)s_total);
		ota_abort();
		return -EINVAL;
	}

	int err = flush_aligned(data, len);
	if (err) {
		printk("ota: flash write failed (%d)\n", err);
		ota_abort();
		return err;
	}
	s_written += len;
	return 0;
}

int ota_finish(uint32_t crc)
{
	if (!s_active) {
		return -EINVAL;
	}

	if (s_written != s_total) {
		printk("ota: size mismatch (%u/%u)\n",
		       (unsigned)s_written, (unsigned)s_total);
		ota_abort();
		return -EINVAL;
	}

	/* Flush a final partial block, zero-padded. The trailer tool pads the
	 * image to 16 bytes so a correctly-built image leaves nothing here. */
	if (s_acc_len) {
		memset(s_acc + s_acc_len, 0, WB - s_acc_len);
		int err = flash_area_write(s_fa, s_flushed, s_acc, WB);
		if (err) {
			printk("ota: tail write failed (%d)\n", err);
			ota_abort();
			return err;
		}
		s_flushed += WB;
		s_acc_len = 0;
	}

	/* Verify the image in place (RRAM is memory-mapped at the slot offset). */
	uint32_t image_len = s_total - VBX_IMG_TRAILER_SIZE;
	const uint8_t *base = (const uint8_t *)s_fa->fa_off;
	uint32_t version = 0;

	if (!vbx_img_verify(base, image_len, &version)) {
		printk("ota: image verify failed\n");
		ota_abort();
		return -EINVAL;
	}
	/* Cross-check the host-supplied CRC against the trailer's. */
	const struct vbx_img_trailer *t =
		(const struct vbx_img_trailer *)(base + image_len);
	if (t->crc32 != crc) {
		printk("ota: host crc mismatch (got 0x%08x trailer 0x%08x)\n",
		       crc, t->crc32);
		ota_abort();
		return -EINVAL;
	}

	int err = bl_state_set_pending(OTHER_SLOT, version, image_len);
	if (err) {
		printk("ota: bl_state_set_pending failed (%d)\n", err);
		ota_abort();
		return err;
	}

	flash_area_close(s_fa);
	s_fa = NULL;
	s_active = false;

	printk("ota: slot %u pending (v%u, %u bytes); rebooting shortly\n",
	       (unsigned)OTHER_SLOT, (unsigned)version, (unsigned)image_len);
	k_work_schedule(&s_reboot_work, K_MSEC(1200));
	return 0;
}

void ota_confirm_on_boot(void)
{
	int err = bl_state_confirm(MY_SLOT);
	if (err) {
		printk("ota: confirm slot %u failed (%d)\n", (unsigned)MY_SLOT, err);
	}
}

uint8_t ota_inactive_slot(void)
{
	return OTHER_SLOT;
}
