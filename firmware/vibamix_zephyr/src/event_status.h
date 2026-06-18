#ifndef VIBAMIX_EVENT_STATUS_H
#define VIBAMIX_EVENT_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event-heartbeat latch for "mesh mode" (the normal awake path, not the
 * button-wake config GATT flow). The event controller floods a 1 Hz mesh
 * heartbeat (vendor opcode 0x07) carrying a short event name; while those beats
 * arrive a badge stays awake and the identity frame's bottom banner shows the
 * event name (the countdown bar itself is drawn by qr_screen's
 * identity_countdown_overlay). This module just latches the last-seen event name
 * and hands the "heartbeat received" signal from the BT RX thread to the main
 * awake loop.
 */

/* Max event-name bytes carried in an (unsegmented) heartbeat. Keeping the mesh
 * access payload <= 11 bytes (3-byte opcode + name + 4-byte TransMIC <= 15)
 * means each beat is a single flood — see docs/ble-config-api.md and mesh.py. */
#define EVENT_NAME_MAX 8

/* BT RX thread: latch the (optional) event name and flag a heartbeat. `len` may
 * be 0 (nameless beat); the previously latched name is kept in that case. */
void event_status_note_heartbeat(const char *name, size_t len);

/* Main thread: returns true once per heartbeat (clears the pending flag). */
bool event_status_consume_heartbeat(void);

/* Latest latched event name (empty string until the first named beat). */
const char *event_status_name(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_EVENT_STATUS_H */
