#include "factory_id.h"

#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>

struct __packed factory_rec {
	uint32_t magic;
	uint16_t id;
	uint16_t id_chk;
};

/* Cache: -1 = not yet read, 0 = blank/invalid, else the assigned id. */
static int s_cached = -1;

uint16_t factory_id_get(void)
{
	if (s_cached >= 0) {
		return (uint16_t)s_cached;
	}
	s_cached = 0; /* default: blank/invalid -> caller falls back */

	const struct flash_area *fa;
	if (flash_area_open(FIXED_PARTITION_ID(factory_partition), &fa) != 0) {
		printk("factory_id: flash_area_open failed\n");
		return 0;
	}

	struct factory_rec rec;
	int rc = flash_area_read(fa, 0, &rec, sizeof(rec));
	flash_area_close(fa);
	if (rc != 0) {
		printk("factory_id: read failed (%d)\n", rc);
		return 0;
	}

	if (rec.magic == FACTORY_ID_MAGIC &&
	    rec.id >= 1 && rec.id <= 0x7FFF &&
	    rec.id_chk == (uint16_t)~rec.id) {
		s_cached = rec.id;
		printk("factory_id: assigned id 0x%04x\n", rec.id);
	} else {
		printk("factory_id: blank/invalid, using fallback\n");
	}

	return (uint16_t)s_cached;
}
