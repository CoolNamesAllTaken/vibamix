#ifndef VIBAMIX_CONFIG_GATT_H
#define VIBAMIX_CONFIG_GATT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Custom GATT "badge config" service for direct phone->badge configuration over
 * Web Bluetooth (NOT over mesh). A connected browser writes:
 *   - Image char (f0de0002, START/DATA/END) -> 1bpp render-only into the framebuffer.
 *   - Name char (f0de0003) -> set attendee name.
 *   - Screen char (f0de0004, START/DATA/END) -> store text screen idx (header+body).
 *   - Image-slot char (f0de0005, START/DATA/END) -> store image slot (1bpp or 2-bit gray).
 *   - Display char (f0de0006) -> show a stored screen (kind+idx).
 * The service is always registered (BT_GATT_SERVICE_DEFINE); it is only useful
 * while connectable advertising runs, which the badge does in config mode.
 *
 * 128-bit UUIDs base: f0de00xx-4b1c-4e2a-9a11-a1b2c3d4e5f6.
 */

struct config_gatt_callbacks {
	void (*on_name)(const char *s, size_t len); /* set name + redraw identity */
	void (*on_activity)(void);                   /* reset the config-window timer */
	void (*on_screen)(uint8_t idx, const char *hdr, size_t hlen,
			  const char *body, size_t blen);
	void (*on_display)(uint8_t kind, uint8_t idx);
	void (*on_attendee)(const char *s, size_t len); /* set attendee/table ID */
	void (*on_frame_led)(uint8_t kind, uint8_t idx, uint8_t anim,
			     uint8_t r, uint8_t g, uint8_t b);
	/* Gateway: re-originate a vendor-model access payload onto the mesh group. */
	void (*on_mesh_tx)(const uint8_t *access, size_t len);
	void (*on_ota_start)(uint32_t total);  /* OTA began — show "updating" screen */
	void (*on_ota_end)(bool ok);           /* OTA finished — show "rebooting"/failed */
	void (*on_keepalive)(uint8_t code);    /* 1 Hz keepalive write from the laptop */
};

/* Register the callbacks invoked from GATT write handlers. Pointer must outlive
 * the connection (static lifetime). */
void config_gatt_set_callbacks(const struct config_gatt_callbacks *cb);

/* Send a keepalive notification to subscribed centrals (badge -> app liveness). */
int config_gatt_keepalive_notify(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_CONFIG_GATT_H */
