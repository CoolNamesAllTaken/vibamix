#ifndef VIBAMIX_GATEWAY_STATUS_H
#define VIBAMIX_GATEWAY_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Mesh-gateway status overlay. While a phone/laptop is connected to this badge
 * and broadcasting mesh commands to the fleet, the badge still applies each
 * command (it's a normal node) but also shows what it relayed: a thin banner
 * composited onto the rendered content (see gateway_status_overlay), and a line
 * on the Connected screen for non-visual commands.
 */

enum gateway_cmd {
	GW_CMD_NAME,
	GW_CMD_FUNFACT,
	GW_CMD_LED,
	GW_CMD_IMAGE,
	GW_CMD_SCREEN,
	GW_CMD_DISPLAY,
	GW_CMD_ATTENDEE,
	GW_CMD_FRAMELED,
};

void gateway_status_set_active(bool on);   /* connected gateway: enable the overlay */
bool gateway_status_active(void);
void gateway_status_note(int cmd);          /* record a relayed command (+count) */
uint32_t gateway_status_count(void);
const char *gateway_status_label(void);     /* label of the last relayed command */
void gateway_status_set_keepalive(bool alive, bool blink);

/* No-op unless active and at least one command has been relayed; otherwise draws
 * the banner into the framebuffer `fb` (call as the last build step before the
 * panel push so it shares the single refresh). */
void gateway_status_overlay(uint8_t *fb);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_GATEWAY_STATUS_H */
