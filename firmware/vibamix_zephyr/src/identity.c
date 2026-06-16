#include "identity.h"

#include <stdio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/byteorder.h>

uint16_t app_identity_addr(void)
{
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
