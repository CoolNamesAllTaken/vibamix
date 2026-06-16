#ifndef VIBAMIX_CONFIG_GATT_H
#define VIBAMIX_CONFIG_GATT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Custom GATT "badge config" service for direct phone->badge configuration over
 * Web Bluetooth (NOT over mesh). A connected browser writes:
 *   - Image char (chunked START/DATA/END) -> drives image_xfer into the framebuffer.
 *   - Name char  -> set attendee name.
 * The service is always registered (BT_GATT_SERVICE_DEFINE); it is only useful
 * while connectable advertising runs, which the badge does in config mode.
 *
 * 128-bit UUIDs (base f0de00xx-4b1c-4e2a-9a11-a1b2c3d4e5f6):
 *   service f0de0001, image f0de0002, name f0de0003.
 */

struct config_gatt_callbacks {
	void (*on_name)(const char *s, size_t len); /* set name + redraw identity */
	void (*on_activity)(void);                   /* reset the config-window timer */
};

/* Register the callbacks invoked from GATT write handlers. Pointer must outlive
 * the connection (static lifetime). */
void config_gatt_set_callbacks(const struct config_gatt_callbacks *cb);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_CONFIG_GATT_H */
