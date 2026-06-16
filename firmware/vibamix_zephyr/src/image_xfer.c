#include "image_xfer.h"

#include <string.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

static uint8_t  *s_buf;
static size_t    s_cap;
static image_complete_cb s_cb;

/* Current transfer state. */
static bool      s_active;
static uint16_t  s_size;     /* expected image byte count */
static uint16_t  s_width;
static uint16_t  s_height;
static size_t    s_received; /* bytes written so far */

void image_xfer_init(uint8_t *buf, size_t cap)
{
	s_buf = buf;
	s_cap = cap;
}

void image_xfer_set_complete_cb(image_complete_cb cb)
{
	s_cb = cb;
}

void image_xfer_start(uint16_t size, uint16_t width, uint16_t height)
{
	if (!s_buf || size == 0 || size > s_cap) {
		printk("img: START rejected (size=%u cap=%u)\n", size, (unsigned)s_cap);
		s_active = false;
		return;
	}

	s_size = size;
	s_width = width;
	s_height = height;
	s_received = 0;
	s_active = true;
	memset(s_buf, 0, s_cap);
	printk("img: START size=%u %ux%u\n", size, width, height);
}

void image_xfer_data(uint16_t offset, const uint8_t *data, uint16_t len)
{
	if (!s_active) {
		return;
	}
	if ((size_t)offset + len > s_size) {
		printk("img: DATA out of range (off=%u len=%u size=%u)\n",
		       offset, len, s_size);
		s_active = false;
		return;
	}

	memcpy(s_buf + offset, data, len);
	s_received += len;
}

void image_xfer_end(uint32_t crc)
{
	if (!s_active) {
		printk("img: END with no active transfer\n");
		return;
	}
	s_active = false;

	if (s_received < s_size) {
		/* Best-effort over a flooded mesh: a group transfer is unacked,
		 * so a dropped segment leaves a hole. Reject rather than render
		 * a corrupt image. A future revision can report missing offsets
		 * back to the controller for retransmit. */
		printk("img: END incomplete (%u/%u bytes)\n",
		       (unsigned)s_received, s_size);
		return;
	}

	uint32_t calc = crc32_ieee(s_buf, s_size);
	if (calc != crc) {
		printk("img: END crc mismatch (got 0x%08x want 0x%08x)\n", calc, crc);
		return;
	}

	printk("img: END ok, rendering\n");
	if (s_cb) {
		s_cb(s_buf, s_size, s_width, s_height);
	}
}
