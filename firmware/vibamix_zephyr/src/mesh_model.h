#ifndef VIBAMIX_MESH_MODEL_H
#define VIBAMIX_MESH_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/mesh.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Vibamix vendor model: the badge's BROADCAST command surface over Bluetooth
 * Mesh. Mesh commands affect every badge at once and are ephemeral — they draw
 * or override live state and are never stored. Per-badge persistent config
 * (name, ID, frames, per-frame LEDs, ...) is GATT-only (see config_gatt.h).
 *
 * The composition data, element/model tables and opcode handlers live here in C
 * because the BT_MESH_MODEL_* macros use C99 compound literals (not valid C++).
 * Commands are forwarded to the C++ app through the handler struct below; image
 * opcodes are forwarded to image_xfer.c (render-only, no storage slot).
 */

struct mesh_config_handlers {
	/* Event heartbeat — keep an awake badge awake (resets the config window).
	 * Carries an optional short event name (<= 8 bytes; len 0 = nameless beat). */
	void (*heartbeat)(const char *name, size_t len);
	/* Override the live LED animation + color on every badge (not stored). */
	void (*show_led)(uint8_t anim, uint8_t r, uint8_t g, uint8_t b);
	/* Draw a text frame (title + body) straight to the panel (not stored),
	 * reassembled from mesh chunks. */
	void (*show_text)(const char *title, size_t tlen,
			  const char *body, size_t blen);
};

/* Register the C++ side callbacks. Pass a pointer with static lifetime. */
void mesh_model_set_config_handlers(const struct mesh_config_handlers *h);

/* Composition data for bt_mesh_init(). */
const struct bt_mesh_comp *mesh_model_comp(void);

/* Bind the app key and subscribe the "all badges" group on the vendor model,
 * standing in for what a Configuration Client would normally do. */
void mesh_model_bind_and_subscribe(uint16_t app_idx, uint16_t group_addr);

/* Originate a vendor-model message on the mesh: `access` is a ready vendor-model
 * access payload (3-byte opcode + params), sent to the bound "all badges" group.
 * Used by the config-mode gateway path (badgectl writes the payload over GATT and
 * the connected badge re-broadcasts it). Returns 0 on success. */
int mesh_model_send_to_group(const uint8_t *access, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_MESH_MODEL_H */
