#ifndef VIBAMIX_APP_CONFIG_H
#define VIBAMIX_APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent badge identity (attendee name, fun fact, LED color), stored in the
 * "vibamix" settings subtree on the RRAM storage partition (ZMS backend).
 *
 * Note: pushed ePaper images are NOT persisted here — the panel is bistable and
 * retains its last image across power loss, and on reboot a badge with a stored
 * name re-renders its name/fact identity screen.
 */

#define APP_CFG_NAME_MAX 32
#define APP_CFG_FACT_MAX 96

struct app_config {
	char    name[APP_CFG_NAME_MAX];
	char    fun_fact[APP_CFG_FACT_MAX];
	uint8_t r, g, b;
	bool    has_color;
	bool    has_name;
	/* A user-drawn full-screen image is currently shown. Persisted so the boot
	 * identity redraw is skipped and the bistable image is preserved. Set when an
	 * image is rendered; cleared when name/fact text replaces the screen. */
	bool    has_custom_image;
};

/* Register the settings handler. Call before settings_load(). */
int app_config_init(void);

const struct app_config *app_config_get(void);

void app_config_set_name(const char *s, size_t len);
void app_config_set_fun_fact(const char *s, size_t len);
void app_config_set_color(uint8_t r, uint8_t g, uint8_t b);
void app_config_set_has_image(bool has_image);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_APP_CONFIG_H */
