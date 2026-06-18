#ifndef VIBAMIX_APP_CONFIG_H
#define VIBAMIX_APP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent badge config (configurable over GATT only — the mesh surface is
 * broadcast/ephemeral), stored in the "vibamix" settings subtree on the RRAM
 * storage partition (ZMS backend). Three frame kinds:
 *   - the identity frame: attendee name + ID string + optional identity image,
 *     with its own LED color/animation shown while it is active;
 *   - 20 text frames: title + full-screen body, each with an LED color/animation;
 *   - 4 image frames: a stored full-screen image, each with an LED color/animation.
 *
 * Note: pushed ePaper images are NOT persisted here — the panel is bistable and
 * retains its last image across power loss, and on reboot a badge with a stored
 * name re-renders its identity frame.
 */

#define APP_CFG_NAME_MAX 32
/* Attendee/table ID: a short human string (e.g. a table number), up to 10 chars. */
#define APP_CFG_ATTENDEE_MAX 11

/* Stored text screens: a header + a full-screen body, addressed by index. */
#define APP_CFG_SCREEN_COUNT 20
#define APP_CFG_HEADER_MAX   48
#define APP_CFG_BODY_MAX     1024
/* Image frames (must match BADGE_IMAGE_SLOTS in badge_store.h). */
#define APP_CFG_IMAGE_SLOTS  4

/* Frame kinds (see app_config_set_display, frame LED, mesh/GATT display). */
#define APP_DISP_KIND_TEXT     0
#define APP_DISP_KIND_IMAGE    1
#define APP_DISP_KIND_IDENTITY 2

/* Per-frame LED config: an animation style (see LedPattern codes) + a color. */
struct frame_led {
	uint8_t anim;
	uint8_t r, g, b;
};

struct badge_screen {
	char header[APP_CFG_HEADER_MAX];
	char body[APP_CFG_BODY_MAX];
	bool present;
	struct frame_led led;   /* LED animation shown while this frame is displayed */
};

struct app_config {
	/* Identity frame. */
	char    name[APP_CFG_NAME_MAX];
	char    attendee_id[APP_CFG_ATTENDEE_MAX];
	struct frame_led identity_led;   /* LED shown while the identity frame is active */
	bool    has_name;
	bool    has_attendee;
	/* A user-drawn full-screen image is currently shown. Persisted so the boot
	 * identity redraw is skipped and the bistable image is preserved. Set when an
	 * image is rendered; cleared when name text replaces the screen. */
	bool    has_custom_image;

	struct badge_screen screens[APP_CFG_SCREEN_COUNT];
	struct frame_led     image_led[APP_CFG_IMAGE_SLOTS];

	/* Last screen commanded to display (kind + index), persisted. */
	uint8_t disp_kind;
	uint8_t disp_idx;
	bool    has_disp;
};

/* Register the settings handler. Call before settings_load(). */
int app_config_init(void);

const struct app_config *app_config_get(void);

void app_config_set_name(const char *s, size_t len);
void app_config_set_attendee_id(const char *s, size_t len);
void app_config_set_has_image(bool has_image);

/* Text screens. set persists header + body for slot `idx`. get returns NULL if
 * the slot is empty. */
void app_config_set_screen(uint8_t idx, const char *hdr, size_t hlen,
			   const char *body, size_t blen);
const struct badge_screen *app_config_get_screen(uint8_t idx);

/* Per-frame LED animation + color, keyed by (kind, idx) — kind is
 * APP_DISP_KIND_TEXT (0..SCREEN_COUNT-1) or APP_DISP_KIND_IMAGE (0..IMAGE_SLOTS-1).
 * get returns false if kind/idx is out of range; *out is the stored config. */
void app_config_set_frame_led(uint8_t kind, uint8_t idx, uint8_t anim,
			      uint8_t r, uint8_t g, uint8_t b);
bool app_config_get_frame_led(uint8_t kind, uint8_t idx, struct frame_led *out);

/* Remember which screen is currently displayed. */
void app_config_set_display(uint8_t kind, uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_APP_CONFIG_H */
