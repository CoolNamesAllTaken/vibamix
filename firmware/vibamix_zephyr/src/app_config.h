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

/* Stored text screens: a header + a full-screen body, addressed by index. */
#define APP_CFG_SCREEN_COUNT 20
#define APP_CFG_HEADER_MAX   48
#define APP_CFG_BODY_MAX     1024

/* "Displayed screen" kinds (see app_config_set_display / DISPLAY_SCREEN). */
#define APP_DISP_KIND_TEXT  0
#define APP_DISP_KIND_IMAGE 1

struct badge_screen {
	char header[APP_CFG_HEADER_MAX];
	char body[APP_CFG_BODY_MAX];
	bool present;
};

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

	struct badge_screen screens[APP_CFG_SCREEN_COUNT];

	/* Last screen commanded to display (kind + index), persisted. */
	uint8_t disp_kind;
	uint8_t disp_idx;
	bool    has_disp;
};

/* Register the settings handler. Call before settings_load(). */
int app_config_init(void);

const struct app_config *app_config_get(void);

void app_config_set_name(const char *s, size_t len);
void app_config_set_fun_fact(const char *s, size_t len);
void app_config_set_color(uint8_t r, uint8_t g, uint8_t b);
void app_config_set_has_image(bool has_image);

/* Text screens. set persists header + body for slot `idx`. get returns NULL if
 * the slot is empty. */
void app_config_set_screen(uint8_t idx, const char *hdr, size_t hlen,
			   const char *body, size_t blen);
const struct badge_screen *app_config_get_screen(uint8_t idx);

/* Remember which screen is currently displayed. */
void app_config_set_display(uint8_t kind, uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_APP_CONFIG_H */
