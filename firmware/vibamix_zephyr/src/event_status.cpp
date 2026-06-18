#include "event_status.h"

#include <string.h>
#include <zephyr/sys/atomic.h>

// Latched on the BT RX thread, read on the main thread. The name is display-only,
// so (like gateway_status) we don't lock it; a torn read just blips one frame.
static char    s_name[EVENT_NAME_MAX + 1];
static atomic_t s_hb_pending;

void event_status_note_heartbeat(const char *name, size_t len)
{
	if (name && len > 0) {
		if (len > EVENT_NAME_MAX) {
			len = EVENT_NAME_MAX;
		}
		memcpy(s_name, name, len);
		s_name[len] = '\0';
	}
	atomic_set(&s_hb_pending, 1);
}

bool event_status_consume_heartbeat(void)
{
	return atomic_cas(&s_hb_pending, 1, 0);
}

const char *event_status_name(void)
{
	return s_name;
}
