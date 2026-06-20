#include "config_gatt.h"
#include "app_config.h"
#include "badge_store.h"
#include "image_xfer.h"
#include "ota.h"
#include "slots.h"

#include "bl_state.h"

#include <string.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

/*
 * Custom GATT "badge config" service for direct phone->badge configuration (NOT
 * over mesh). Consolidated into one read/write characteristic per FRAME TYPE —
 * identity (f0de0003), text frame (f0de0004), image frame (f0de0005) — plus a
 * quick-render image, OTA, and keepalive. Each frame characteristic carries the
 * whole frame, supports both write (configure) and read (load current), and folds
 * in a "display now" op. Writes are tagged by a leading op byte; a host SELECTs a
 * frame (which serializes it) then reads it back.
 *
 * 128-bit UUIDs base: f0de00xx-4b1c-4e2a-9a11-a1b2c3d4e5f6.
 */

#define VBX_UUID_SVC    BT_UUID_128_ENCODE(0xf0de0001, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMG    BT_UUID_128_ENCODE(0xf0de0002, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IDENT  BT_UUID_128_ENCODE(0xf0de0003, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_TEXT   BT_UUID_128_ENCODE(0xf0de0004, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMGF   BT_UUID_128_ENCODE(0xf0de0005, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_OTA    BT_UUID_128_ENCODE(0xf0de0009, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_OTAST  BT_UUID_128_ENCODE(0xf0de000A, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_KEEPAL BT_UUID_128_ENCODE(0xf0de000B, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_MESHTX BT_UUID_128_ENCODE(0xf0de000C, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)

static const struct bt_uuid_128 vbx_svc_uuid    = BT_UUID_INIT_128(VBX_UUID_SVC);
static const struct bt_uuid_128 vbx_img_uuid    = BT_UUID_INIT_128(VBX_UUID_IMG);
static const struct bt_uuid_128 vbx_ident_uuid  = BT_UUID_INIT_128(VBX_UUID_IDENT);
static const struct bt_uuid_128 vbx_text_uuid   = BT_UUID_INIT_128(VBX_UUID_TEXT);
static const struct bt_uuid_128 vbx_imgf_uuid   = BT_UUID_INIT_128(VBX_UUID_IMGF);
static const struct bt_uuid_128 vbx_ota_uuid    = BT_UUID_INIT_128(VBX_UUID_OTA);
static const struct bt_uuid_128 vbx_otast_uuid  = BT_UUID_INIT_128(VBX_UUID_OTAST);
static const struct bt_uuid_128 vbx_keepal_uuid = BT_UUID_INIT_128(VBX_UUID_KEEPAL);
static const struct bt_uuid_128 vbx_meshtx_uuid = BT_UUID_INIT_128(VBX_UUID_MESHTX);

/* Op bytes — first byte of a write. */
#define OP_START   0x01   /* begin a chunked payload (image pixels / text body) */
#define OP_DATA    0x02   /* a chunk */
#define OP_END     0x03   /* commit */
#define OP_META    0x10   /* identity: name + ID + LED in one small write */
#define OP_DISPLAY 0x20   /* show this frame on the panel now */
#define OP_SELECT  0x21   /* serialize this frame so the next read returns it */
#define OP_READ_AT 0x22   /* set the read window base (le16 offset) into s_rd */

static const struct config_gatt_callbacks *s_cb;

void config_gatt_set_callbacks(const struct config_gatt_callbacks *cb)
{
	s_cb = cb;
}

static void note_activity(void)
{
	if (s_cb && s_cb->on_activity) {
		s_cb->on_activity();
	}
}

/* Append a length-prefixed (u8 len) string; returns bytes written. */
static size_t put_str(uint8_t *p, const char *s, size_t maxlen)
{
	size_t n = strnlen(s, maxlen);

	p[0] = (uint8_t)n;
	memcpy(p + 1, s, n);
	return 1 + n;
}

/* One shared read buffer. Identity / text / image-frame reads never overlap (the
 * host SELECTs then reads one frame at a time), and the image payloads are large,
 * so a single buffer serves all three. Serialized on each OP_SELECT write; the
 * read handler returns the cached bytes (Zephyr's blob-read serves values larger
 * than the MTU across multiple offset reads, so the buffer must stay stable). */
/* Sized for the largest serialization: identity = name + ID + LED + image header
 * + gray2 pixels (~11.6 KB). 64 B of headroom covers all the metadata fields. */
static uint8_t s_rd[64 + BADGE_IMG_GRAY2_BYTES];
static size_t  s_rd_len;
/* App-level read window base. macOS/CoreBluetooth caps a single characteristic read
 * at 512 B, so the host pulls s_rd in <=MTU pieces: it writes OP_READ_AT [le16 off]
 * to move this base, then reads. OP_SELECT resets it to 0. */
static uint16_t s_rd_off;

static ssize_t frame_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	size_t base = (s_rd_off < s_rd_len) ? s_rd_off : s_rd_len;

	return bt_gatt_attr_read(conn, attr, buf, len, offset, s_rd + base, s_rd_len - base);
}

/* Append "anim, r, g, b" for frame (kind, idx). */
static uint8_t *put_frame_led(uint8_t *q, uint8_t kind, uint8_t idx)
{
	struct frame_led led = { 0 };

	app_config_get_frame_led(kind, idx, &led);
	*q++ = led.anim;
	*q++ = led.r;
	*q++ = led.g;
	*q++ = led.b;
	return q;
}

/* Append "present, fmt, le16 w, le16 h, le16 len [, pixels]" for a stored image. */
static uint8_t *put_image(uint8_t *q, uint8_t slot, bool want_pixels)
{
	uint8_t  fmt = 0;
	size_t   ilen = 0;
	uint16_t w = 0, h = 0;
	bool present = (badge_store_image_info(slot, &fmt, &ilen, &w, &h) == 0);

	*q++ = present ? 1 : 0;
	*q++ = fmt;
	sys_put_le16(w, q); q += 2;
	sys_put_le16(h, q); q += 2;
	sys_put_le16((uint16_t)ilen, q); q += 2;
	if (present && want_pixels) {
		badge_store_image_read(slot, q, BADGE_IMG_GRAY2_BYTES, NULL, NULL, NULL, NULL);
		q += ilen;
	}
	return q;
}

/* ====================== quick-render image (f0de0002) ====================== */
/* render only, no storage slot — draw a 1bpp image straight to the panel. */
static ssize_t img_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OP_START:
		if (len < 7) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_start(BADGE_SLOT_NONE, BADGE_FMT_BW, sys_get_le16(p + 1),
				 sys_get_le16(p + 3), sys_get_le16(p + 5));
		break;
	case OP_DATA:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_data(sys_get_le16(p + 1), p + 3, len - 3);
		break;
	case OP_END:
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_end(sys_get_le32(p + 1));
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* ====================== identity frame (f0de0003) ====================== */
/* Write ops:
 *   META    [0x10, namelen, name, idlen, id, anim, r, g, b]
 *   IMG     START [0x01, fmt, le16 size, le16 w, le16 h] / DATA / END (gray2 image)
 *   DISPLAY [0x20]
 *   SELECT  [0x21, want_pixels]  (then read)
 * Read returns: namelen,name, idlen,id, anim,r,g,b, present,fmt,le16 w,le16 h,
 *   le16 len, [pixels if want_pixels]. */
static ssize_t identity_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OP_META: {
		size_t o = 1;

		if (o + 1 > len) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint8_t nl = p[o++];
		if (o + nl + 1 > len) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		const char *name = (const char *)(p + o);
		o += nl;
		uint8_t il = p[o++];
		if (o + il + 4 > len) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		const char *id = (const char *)(p + o);
		o += il;
		uint8_t anim = p[o], r = p[o + 1], g = p[o + 2], b = p[o + 3];

		if (s_cb && s_cb->on_name) {
			s_cb->on_name(name, nl);
		}
		if (s_cb && s_cb->on_attendee) {
			s_cb->on_attendee(id, il);
		}
		if (s_cb && s_cb->on_frame_led) {
			s_cb->on_frame_led(APP_DISP_KIND_IDENTITY, 0, anim, r, g, b);
		}
		break;
	}
	case OP_START:
		if (len < 8) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_start(BADGE_SLOT_IDENTITY, p[1], sys_get_le16(p + 2),
				 sys_get_le16(p + 4), sys_get_le16(p + 6));
		break;
	case OP_DATA:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_data(sys_get_le16(p + 1), p + 3, len - 3);
		break;
	case OP_END:
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_end(sys_get_le32(p + 1));
		break;
	case OP_DISPLAY:
		if (s_cb && s_cb->on_display) {
			s_cb->on_display(APP_DISP_KIND_IDENTITY, 0);
		}
		break;
	case OP_SELECT: {
		bool want_pixels = (len >= 2 && p[1]);
		const struct app_config *cfg = app_config_get();
		uint8_t *q = s_rd;

		q += put_str(q, cfg->name, APP_CFG_NAME_MAX - 1);
		q += put_str(q, cfg->attendee_id, APP_CFG_ATTENDEE_MAX - 1);
		q = put_frame_led(q, APP_DISP_KIND_IDENTITY, 0);
		q = put_image(q, BADGE_SLOT_IDENTITY, want_pixels);
		s_rd_len = q - s_rd;
		s_rd_off = 0;
		break;
	}
	case OP_READ_AT:
		s_rd_off = (len >= 3) ? sys_get_le16(p + 1) : 0;
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* ====================== text frame (f0de0004) ====================== */
/* Reassembly for a text frame pushed over GATT (reliable, acked writes). */
static struct {
	bool    active;
	uint8_t idx;
	uint8_t anim, r, g, b;
	size_t  hlen;
	size_t  blen;
	char    header[APP_CFG_HEADER_MAX];
	char    body[APP_CFG_BODY_MAX];
} s_txt;

/* Write ops:
 *   START   [0x01, idx, anim, r, g, b, hlen, header]
 *   DATA    [0x02, le16 off, body]
 *   END     [0x03]
 *   DISPLAY [0x20, idx]
 *   SELECT  [0x21, idx]  (then read)
 * Read returns: present, idx, anim,r,g,b, hlen, header, le16 blen, body. */
static ssize_t text_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OP_META:   /* LED-only update: [op, idx, anim, r, g, b] (no body resend) */
		if (len < 6) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (p[1] < APP_CFG_SCREEN_COUNT && s_cb && s_cb->on_frame_led) {
			s_cb->on_frame_led(APP_DISP_KIND_TEXT, p[1], p[2], p[3], p[4], p[5]);
		}
		break;
	case OP_START: {
		if (len < 7) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint8_t idx = p[1];

		if (idx >= APP_CFG_SCREEN_COUNT) {
			s_txt.active = false;
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		size_t hlen = p[6];
		size_t avail = len - 7;

		if (hlen > avail) {
			hlen = avail;
		}
		if (hlen > sizeof(s_txt.header) - 1) {
			hlen = sizeof(s_txt.header) - 1;
		}
		memcpy(s_txt.header, p + 7, hlen);
		s_txt.header[hlen] = '\0';
		s_txt.hlen = hlen;
		s_txt.idx = idx;
		s_txt.anim = p[2];
		s_txt.r = p[3];
		s_txt.g = p[4];
		s_txt.b = p[5];
		s_txt.blen = 0;
		s_txt.active = true;
		break;
	}
	case OP_DATA: {
		if (!s_txt.active || len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint16_t off = sys_get_le16(p + 1);
		size_t   n = len - 3;

		if ((size_t)off + n > sizeof(s_txt.body) - 1) {
			s_txt.active = false;
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		memcpy(s_txt.body + off, p + 3, n);
		if ((size_t)off + n > s_txt.blen) {
			s_txt.blen = (size_t)off + n;
		}
		break;
	}
	case OP_END:
		if (s_txt.active) {
			s_txt.body[s_txt.blen] = '\0';
			if (s_cb && s_cb->on_frame_led) {
				s_cb->on_frame_led(APP_DISP_KIND_TEXT, s_txt.idx,
						   s_txt.anim, s_txt.r, s_txt.g, s_txt.b);
			}
			if (s_cb && s_cb->on_screen) {
				s_cb->on_screen(s_txt.idx, s_txt.header, s_txt.hlen,
						s_txt.body, s_txt.blen);
			}
			s_txt.active = false;
		}
		break;
	case OP_DISPLAY:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (s_cb && s_cb->on_display) {
			s_cb->on_display(APP_DISP_KIND_TEXT, p[1]);
		}
		break;
	case OP_SELECT: {
		uint8_t idx = (len >= 2) ? p[1] : 0;
		const struct badge_screen *s =
			(idx < APP_CFG_SCREEN_COUNT) ? app_config_get_screen(idx) : NULL;
		uint8_t *q = s_rd;

		*q++ = s ? 1 : 0;
		*q++ = idx;
		q = put_frame_led(q, APP_DISP_KIND_TEXT, idx);
		if (s) {
			size_t hn = strnlen(s->header, APP_CFG_HEADER_MAX - 1);
			size_t bn = strnlen(s->body, APP_CFG_BODY_MAX - 1);

			*q++ = (uint8_t)hn;
			memcpy(q, s->header, hn);
			q += hn;
			sys_put_le16((uint16_t)bn, q);
			q += 2;
			memcpy(q, s->body, bn);
			q += bn;
		} else {
			*q++ = 0;
			sys_put_le16(0, q);
			q += 2;
		}
		s_rd_len = q - s_rd;
		s_rd_off = 0;
		break;
	}
	case OP_READ_AT:
		s_rd_off = (len >= 3) ? sys_get_le16(p + 1) : 0;
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* ====================== image frame (f0de0005) ====================== */
/* Write ops:
 *   START   [0x01, slot, fmt, anim, r, g, b, le16 size, le16 w, le16 h] / DATA / END
 *   DISPLAY [0x20, slot]
 *   SELECT  [0x21, slot, want_pixels]  (then read)
 * Read returns: present, slot, anim,r,g,b, fmt, le16 w, le16 h, le16 len,
 *   [pixels if want_pixels]. */
static ssize_t imgframe_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OP_META:   /* LED-only update: [op, slot, anim, r, g, b] (no image resend) */
		if (len < 6) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (p[1] < BADGE_IMAGE_SLOTS && s_cb && s_cb->on_frame_led) {
			s_cb->on_frame_led(APP_DISP_KIND_IMAGE, p[1], p[2], p[3], p[4], p[5]);
		}
		break;
	case OP_START: {
		if (len < 13) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint8_t slot = p[1];

		if (slot >= BADGE_IMAGE_SLOTS) {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		if (s_cb && s_cb->on_frame_led) {
			s_cb->on_frame_led(APP_DISP_KIND_IMAGE, slot, p[3], p[4], p[5], p[6]);
		}
		image_xfer_start(slot, p[2] /*fmt*/, sys_get_le16(p + 7),
				 sys_get_le16(p + 9), sys_get_le16(p + 11));
		break;
	}
	case OP_DATA:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_data(sys_get_le16(p + 1), p + 3, len - 3);
		break;
	case OP_END:
		if (len < 5) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_end(sys_get_le32(p + 1));
		break;
	case OP_DISPLAY:
		if (len < 2) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		if (s_cb && s_cb->on_display) {
			s_cb->on_display(APP_DISP_KIND_IMAGE, p[1]);
		}
		break;
	case OP_SELECT: {
		uint8_t slot = (len >= 2) ? p[1] : 0;
		bool want_pixels = (len >= 3 && p[2]);
		uint8_t *q = s_rd;

		*q++ = badge_store_image_present(slot) ? 1 : 0;
		*q++ = slot;
		q = put_frame_led(q, APP_DISP_KIND_IMAGE, slot);
		q = put_image(q, slot, want_pixels);
		s_rd_len = q - s_rd;
		s_rd_off = 0;
		break;
	}
	case OP_READ_AT:
		s_rd_off = (len >= 3) ? sys_get_le16(p + 1) : 0;
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* ====================== OTA (f0de0009 / f0de000A) ====================== */
/* f0de0009 — OTA firmware update. Streams the trailered image built for the
 * *inactive* direct-XIP slot (raw image + 32-byte CRC trailer; see vbx_img.h)
 * into that slot. Read f0de000A first to learn which slot to send.
 * START = op,le32 total_size ; DATA = op,le32 offset,bytes ; END = op,le32 crc32.
 * total_size includes the trailer; crc32 is the trailer's crc32 (over the image
 * bytes only). (u32 fields — the image is ~360 KB, beyond the u16 used elsewhere.) */
static ssize_t ota_write_char(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 5) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	switch (p[0]) {
	case OP_START: {
		uint32_t total = sys_get_le32(p + 1);

		if (ota_begin(total) != 0) {
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}
		if (s_cb && s_cb->on_ota_start) {
			s_cb->on_ota_start(total);
		}
		break;
	}
	case OP_DATA:
		if (ota_write(sys_get_le32(p + 1), p + 5, len - 5) != 0) {
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}
		if (s_cb && s_cb->on_ota_progress) {
			s_cb->on_ota_progress(ota_written(), ota_total());
		}
		break;
	case OP_END: {
		int err = ota_finish(sys_get_le32(p + 1));

		if (s_cb && s_cb->on_ota_end) {
			s_cb->on_ota_end(err == 0);
		}
		if (err) {
			return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
		}
		break;
	}
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* f0de000A — OTA status (read-only). Lets the host learn which slot to target:
 * [0]=active_slot, [1]=inactive_slot, [2..5]=le32 active image version (0 if the
 * running image was flashed over SWD rather than OTA'd, so bl_state has no record). */
static ssize_t otast_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	uint8_t st[6];
	struct bl_state_rec rec;
	uint32_t version = 0;

	if (bl_state_read(&rec) == 0) {
		version = rec.slot[MY_SLOT].version;
	}
	st[0] = MY_SLOT;
	st[1] = OTHER_SLOT;
	sys_put_le32(version, st + 2);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, st, sizeof(st));
}

/* ====================== mesh TX gateway (f0de000C) ====================== */
/* The value is a complete vendor-model access payload (3-byte opcode + params).
 * This config-mode badge re-originates it onto the mesh "all badges" group, so an
 * app can broadcast to the fleet without the SIG mesh GATT proxy. One write = one
 * mesh message; chunked broadcasts (image/text) are sent as successive writes. */
static ssize_t meshtx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 3) {   /* at least a 3-byte vendor opcode */
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (s_cb && s_cb->on_mesh_tx) {
		s_cb->on_mesh_tx((const uint8_t *)buf, len);
	}
	note_activity();
	return len;
}

/* ====================== keepalive (f0de000B) ====================== */
/* f0de000B — per-connection keepalive. The laptop writes once a second (app->badge
 * liveness); the badge notifies once a second (badge->app liveness). Payload is a
 * 1-byte counter, ignored except as a ping. Must be the LAST characteristic so the
 * notify helper can find its value attr as attrs[attr_count - 2] (CCC is last). */
static ssize_t keepalive_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (s_cb && s_cb->on_keepalive) {
		s_cb->on_keepalive(len ? ((const uint8_t *)buf)[0] : 0);
	}
	note_activity();
	return len;
}

BT_GATT_SERVICE_DEFINE(vbx_cfg_svc,
	BT_GATT_PRIMARY_SERVICE(&vbx_svc_uuid),
	/* Quick-render image: draw now, not stored. */
	BT_GATT_CHARACTERISTIC(&vbx_img_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, img_write, NULL),
	/* One read/write characteristic per frame type (write tagged by op byte;
	 * SELECT then read to load the current frame; DISPLAY to show it now). */
	BT_GATT_CHARACTERISTIC(&vbx_ident_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, frame_read, identity_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_text_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, frame_read, text_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_imgf_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, frame_read, imgframe_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_ota_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, ota_write_char, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_otast_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, otast_read, NULL, NULL),
	/* Mesh-TX gateway: re-originate a vendor-model access payload onto the mesh. */
	BT_GATT_CHARACTERISTIC(&vbx_meshtx_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, meshtx_write, NULL),
	/* Keepalive — keep these two LAST (see keepalive_write / config_gatt_keepalive_notify). */
	BT_GATT_CHARACTERISTIC(&vbx_keepal_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, keepalive_write, NULL),
	BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Notify the keepalive characteristic (badge -> app liveness). NULL conn = all
 * subscribers. The value attr is second-to-last (CCC is last). */
int config_gatt_keepalive_notify(uint8_t code)
{
	const struct bt_gatt_attr *val = &vbx_cfg_svc.attrs[vbx_cfg_svc.attr_count - 2];

	return bt_gatt_notify(NULL, val, &code, sizeof(code));
}
