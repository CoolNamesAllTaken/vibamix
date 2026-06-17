#include "ota.h"

#include <errno.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/reboot.h>

static struct flash_img_context s_ctx;
static bool     s_active;
static uint32_t s_total;
static uint32_t s_crc;      /* running crc32_ieee over received bytes */
static size_t   s_received;

static void reboot_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	printk("ota: rebooting to apply update\n");
	sys_reboot(SYS_REBOOT_COLD);
}
static K_WORK_DELAYABLE_DEFINE(s_reboot_work, reboot_work_fn);

int ota_begin(uint32_t total_size)
{
	int err = flash_img_init(&s_ctx);   /* targets the secondary slot (slot1) */
	if (err) {
		printk("ota: flash_img_init failed (%d)\n", err);
		s_active = false;
		return err;
	}
	s_total = total_size;
	s_crc = 0;
	s_received = 0;
	s_active = true;
	printk("ota: begin, %u bytes\n", (unsigned)total_size);
	return 0;
}

int ota_write(uint32_t offset, const uint8_t *data, uint16_t len)
{
	if (!s_active) {
		return -EINVAL;
	}
	if (offset != s_received) {
		printk("ota: offset gap (got %u, expected %u)\n",
		       (unsigned)offset, (unsigned)s_received);
		s_active = false;
		return -EINVAL;
	}
	int err = flash_img_buffered_write(&s_ctx, data, len, false);
	if (err) {
		printk("ota: flash write failed (%d)\n", err);
		s_active = false;
		return err;
	}
	s_crc = crc32_ieee_update(s_crc, data, len);
	s_received += len;
	return 0;
}

int ota_finish(uint32_t crc)
{
	if (!s_active) {
		return -EINVAL;
	}
	s_active = false;

	/* Flush any bytes still buffered in flash_img. */
	int err = flash_img_buffered_write(&s_ctx, NULL, 0, true);
	if (err) {
		printk("ota: flush failed (%d)\n", err);
		return err;
	}
	if (s_received != s_total) {
		printk("ota: size mismatch (%u/%u)\n", (unsigned)s_received, (unsigned)s_total);
		return -EINVAL;
	}
	if (s_crc != crc) {
		printk("ota: crc mismatch (got 0x%08x want 0x%08x)\n", s_crc, crc);
		return -EINVAL;
	}

	err = boot_request_upgrade(BOOT_UPGRADE_TEST);   /* swap, then revert if not confirmed */
	if (err) {
		printk("ota: boot_request_upgrade failed (%d)\n", err);
		return err;
	}
	printk("ota: image staged (%u bytes); rebooting shortly\n", (unsigned)s_received);
	k_work_schedule(&s_reboot_work, K_MSEC(1200));
	return 0;
}

void ota_confirm_on_boot(void)
{
	if (!boot_is_img_confirmed()) {
		int err = boot_write_img_confirmed();
		printk("ota: confirmed running image (%d)\n", err);
	}
}
