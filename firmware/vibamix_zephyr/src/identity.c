#include "identity.h"
#include "factory_id.h"

#include <stdio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>

uint16_t app_identity_addr(void)
{
	/* Prefer the unique id assigned at manufacturing (collision-free up to
	 * 32767 units). Fall back to a 15-bit slice of the FICR device id for dev
	 * units that were never run through the flashtool — that slice collides at
	 * ~75% for a few hundred badges, so production units must carry a factory
	 * id. */
	uint16_t id = factory_id_get();
	if (id != 0) {
		return id;
	}

	uint8_t uuid[16];

	hwinfo_get_device_id(uuid, sizeof(uuid));
	uint16_t addr = sys_get_le16(uuid) & 0x7FFF;
	if (addr == 0) {
		addr = 1; /* unicast addresses must be non-zero */
	}
	return addr;
}

void app_identity_code(char out[5])
{
	snprintf(out, 5, "%04X", app_identity_addr());
}
