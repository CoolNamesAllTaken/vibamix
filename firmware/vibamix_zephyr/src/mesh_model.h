#ifndef VIBAMIX_MESH_MODEL_H
#define VIBAMIX_MESH_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Vibamix vendor model: the badge's command surface over Bluetooth Mesh.
 *
 * The composition data, element/model tables and opcode handlers live here in C
 * because the BT_MESH_MODEL_* macros use C99 compound literals (not valid C++).
 * Config opcodes are forwarded to the C++ app through the handler struct below;
 * image opcodes are forwarded to image_xfer.c.
 */

struct mesh_config_handlers {
	void (*set_name)(const char *name, size_t len);
	void (*set_fun_fact)(const char *fact, size_t len);
	void (*set_led_color)(uint8_t r, uint8_t g, uint8_t b);
	/* Event heartbeat — keep an awake badge awake (resets the config window). */
	void (*heartbeat)(void);
	/* Store a text screen (header + body), reassembled from mesh chunks. */
	void (*set_screen)(uint8_t idx, const char *hdr, size_t hlen,
			   const char *body, size_t blen);
	/* Display a stored screen: kind 0 = text screen, 1 = image slot. */
	void (*display_screen)(uint8_t kind, uint8_t idx);
	/* Set the attendee/table ID string. */
	void (*set_attendee)(const char *id, size_t len);
	/* Set a frame's LED animation + color: kind/idx as display_screen. */
	void (*set_frame_led)(uint8_t kind, uint8_t idx, uint8_t anim,
			      uint8_t r, uint8_t g, uint8_t b);
};

/* Register the C++ side callbacks. Pass a pointer with static lifetime. */
void mesh_model_set_config_handlers(const struct mesh_config_handlers *h);

/* Composition data for bt_mesh_init(). */
const struct bt_mesh_comp *mesh_model_comp(void);

/* Bind the app key and subscribe the "all badges" group on the vendor model,
 * standing in for what a Configuration Client would normally do. */
void mesh_model_bind_and_subscribe(uint16_t app_idx, uint16_t group_addr);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_MESH_MODEL_H */
