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
 * heartbeat (vendor opcode 0x07); while those beats arrive a badge stays awake
 * (each beat resets the awake-window deadline). This module just hands the
 * "heartbeat received" signal from the BT RX thread to the main awake loop. The
 * heartbeat's optional event-name payload is accepted but no longer displayed.
 */

/* BT RX thread: flag a heartbeat. `name`/`len` (the optional event name) are
 * ignored — kept in the signature so the mesh handler doesn't need to change. */
void event_status_note_heartbeat(const char *name, size_t len);

/* Main thread: returns true once per heartbeat (clears the pending flag). */
bool event_status_consume_heartbeat(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_EVENT_STATUS_H */
