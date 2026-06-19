#include "event_status.h"

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

// Set on the BT RX thread, consumed on the main thread.
static atomic_t s_hb_pending;

void event_status_note_heartbeat(const char *name, size_t len)
{
	// The event name (if any) is no longer displayed — a heartbeat only keeps the
	// badge awake (resets the awake-window deadline). Ignore the name payload.
	ARG_UNUSED(name);
	ARG_UNUSED(len);
	atomic_set(&s_hb_pending, 1);
}

bool event_status_consume_heartbeat(void)
{
	return atomic_cas(&s_hb_pending, 1, 0);
}
