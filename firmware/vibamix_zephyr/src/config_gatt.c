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

#define VBX_UUID_SVC     BT_UUID_128_ENCODE(0xf0de0001, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMG     BT_UUID_128_ENCODE(0xf0de0002, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_NAME    BT_UUID_128_ENCODE(0xf0de0003, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_SCREEN  BT_UUID_128_ENCODE(0xf0de0004, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMGSLOT BT_UUID_128_ENCODE(0xf0de0005, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_DISPLAY BT_UUID_128_ENCODE(0xf0de0006, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_ATTND   BT_UUID_128_ENCODE(0xf0de0007, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_FRMLED  BT_UUID_128_ENCODE(0xf0de0008, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_OTA     BT_UUID_128_ENCODE(0xf0de0009, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_OTAST   BT_UUID_128_ENCODE(0xf0de000A, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_KEEPAL  BT_UUID_128_ENCODE(0xf0de000B, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_SNAP    BT_UUID_128_ENCODE(0xf0de000C, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_SCRRD   BT_UUID_128_ENCODE(0xf0de000D, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMGRD   BT_UUID_128_ENCODE(0xf0de000E, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)

static const struct bt_uuid_128 vbx_svc_uuid     = BT_UUID_INIT_128(VBX_UUID_SVC);
static const struct bt_uuid_128 vbx_img_uuid     = BT_UUID_INIT_128(VBX_UUID_IMG);
static const struct bt_uuid_128 vbx_name_uuid    = BT_UUID_INIT_128(VBX_UUID_NAME);
static const struct bt_uuid_128 vbx_screen_uuid  = BT_UUID_INIT_128(VBX_UUID_SCREEN);
static const struct bt_uuid_128 vbx_imgslot_uuid = BT_UUID_INIT_128(VBX_UUID_IMGSLOT);
static const struct bt_uuid_128 vbx_display_uuid = BT_UUID_INIT_128(VBX_UUID_DISPLAY);
static const struct bt_uuid_128 vbx_attnd_uuid   = BT_UUID_INIT_128(VBX_UUID_ATTND);
static const struct bt_uuid_128 vbx_frmled_uuid  = BT_UUID_INIT_128(VBX_UUID_FRMLED);
static const struct bt_uuid_128 vbx_ota_uuid     = BT_UUID_INIT_128(VBX_UUID_OTA);
static const struct bt_uuid_128 vbx_otast_uuid   = BT_UUID_INIT_128(VBX_UUID_OTAST);
static const struct bt_uuid_128 vbx_keepal_uuid  = BT_UUID_INIT_128(VBX_UUID_KEEPAL);
static const struct bt_uuid_128 vbx_snap_uuid    = BT_UUID_INIT_128(VBX_UUID_SNAP);
static const struct bt_uuid_128 vbx_scrrd_uuid   = BT_UUID_INIT_128(VBX_UUID_SCRRD);
static const struct bt_uuid_128 vbx_imgrd_uuid   = BT_UUID_INIT_128(VBX_UUID_IMGRD);

/* Chunk framing op bytes, shared by the image/image-slot/screen characteristics. */
#define OP_START 0x01
#define OP_DATA  0x02
#define OP_END   0x03

static const struct config_gatt_callbacks *s_cb;

/* Reassembly for a screen pushed over GATT (reliable, acked writes). */
static struct {
	bool    active;
	uint8_t idx;
	size_t  hlen;
	size_t  blen;
	char    header[APP_CFG_HEADER_MAX];
	char    body[APP_CFG_BODY_MAX];
} s_scr;

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

/* f0de0002 — legacy 1bpp image: render only, no storage slot. */
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

/* f0de0005 — image slot: stores into one of 4 slots, 1bpp or 2-bit grayscale.
 * START = op,slot,format,le16 size,le16 w,le16 h ; DATA/END as the image char. */
static ssize_t imgslot_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
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
		if (len < 9) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_start(p[1] /*slot*/, p[2] /*format*/, sys_get_le16(p + 3),
				 sys_get_le16(p + 5), sys_get_le16(p + 7));
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

/* f0de0004 — text screen: START = op,idx,hlen,header ; DATA = op,le16 off,body ;
 * END commits via on_screen. */
static ssize_t screen_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
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
	case OP_START: {
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint8_t idx = p[1];
		size_t  hlen = p[2];

		if (idx >= APP_CFG_SCREEN_COUNT) {
			s_scr.active = false;
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		size_t avail = len - 3;
		if (hlen > avail) {
			hlen = avail;
		}
		if (hlen > sizeof(s_scr.header) - 1) {
			hlen = sizeof(s_scr.header) - 1;
		}
		memcpy(s_scr.header, p + 3, hlen);
		s_scr.header[hlen] = '\0';
		s_scr.hlen = hlen;
		s_scr.idx = idx;
		s_scr.blen = 0;
		s_scr.active = true;
		break;
	}
	case OP_DATA: {
		if (!s_scr.active || len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		uint16_t off = sys_get_le16(p + 1);
		size_t   n = len - 3;

		if ((size_t)off + n > sizeof(s_scr.body) - 1) {
			s_scr.active = false;
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
		memcpy(s_scr.body + off, p + 3, n);
		if ((size_t)off + n > s_scr.blen) {
			s_scr.blen = (size_t)off + n;
		}
		break;
	}
	case OP_END:
		if (s_scr.active) {
			s_scr.body[s_scr.blen] = '\0';
			if (s_cb && s_cb->on_screen) {
				s_cb->on_screen(s_scr.idx, s_scr.header, s_scr.hlen,
						s_scr.body, s_scr.blen);
			}
			s_scr.active = false;
		}
		break;
	default:
		return BT_GATT_ERR(BT_ATT_ERR_NOT_SUPPORTED);
	}

	note_activity();
	return len;
}

/* f0de0006 — display a stored screen: kind, idx. */
static ssize_t display_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (s_cb && s_cb->on_display) {
		s_cb->on_display(p[0], p[1]);
	}
	note_activity();
	return len;
}

static ssize_t name_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (s_cb && s_cb->on_name) {
		s_cb->on_name((const char *)buf, len);
	}
	note_activity();
	return len;
}

/* f0de0007 — attendee/table ID (UTF-8 string, <=10 chars). */
static ssize_t attendee_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (s_cb && s_cb->on_attendee) {
		s_cb->on_attendee((const char *)buf, len);
	}
	note_activity();
	return len;
}

/* f0de0008 — per-frame LED: kind, idx, anim, r, g, b. */
static ssize_t frmled_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	const uint8_t *p = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 6) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (s_cb && s_cb->on_frame_led) {
		s_cb->on_frame_led(p[0], p[1], p[2], p[3], p[4], p[5]);
	}
	note_activity();
	return len;
}

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

/* ---- Read-back: let the host fetch what's currently stored on the badge. ----
 * Zephyr's bt_gatt_attr_read + the host's read-blob handle values larger than the
 * MTU automatically, so each read serves a full serialized value. */

/* Append a length-prefixed (u8 len) string; returns bytes written. */
static size_t put_str(uint8_t *p, const char *s, size_t maxlen)
{
	size_t n = strnlen(s, maxlen);

	p[0] = (uint8_t)n;
	memcpy(p + 1, s, n);
	return 1 + n;
}

/* f0de000C — config snapshot (read-only). One read returns the overview:
 *   name, fun_fact, attendee     (each: u8 len + bytes)
 *   has_color, r, g, b
 *   has_disp, disp_kind, disp_idx
 *   screen_count(u8), per screen: present(u8), hlen(u8), header[hlen]   (titles only)
 *   slot_count(u8),   per slot:   present(u8)
 * Per-screen bodies and image pixels are fetched on demand (f0de000D / f0de000E). */
static uint8_t s_snap[1200];

static ssize_t snap_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	const struct app_config *cfg = app_config_get();
	uint8_t *p = s_snap;

	p += put_str(p, cfg->name, APP_CFG_NAME_MAX - 1);
	p += put_str(p, cfg->fun_fact, APP_CFG_FACT_MAX - 1);
	p += put_str(p, cfg->attendee_id, APP_CFG_ATTENDEE_MAX - 1);
	*p++ = cfg->has_color ? 1 : 0;
	*p++ = cfg->r;
	*p++ = cfg->g;
	*p++ = cfg->b;
	*p++ = cfg->has_disp ? 1 : 0;
	*p++ = cfg->disp_kind;
	*p++ = cfg->disp_idx;

	*p++ = APP_CFG_SCREEN_COUNT;
	for (int i = 0; i < APP_CFG_SCREEN_COUNT; i++) {
		const struct badge_screen *s = &cfg->screens[i];
		size_t hn = s->present ? strnlen(s->header, APP_CFG_HEADER_MAX - 1) : 0;

		*p++ = s->present ? 1 : 0;
		*p++ = (uint8_t)hn;
		memcpy(p, s->header, hn);
		p += hn;
	}

	*p++ = APP_CFG_IMAGE_SLOTS;
	for (int i = 0; i < APP_CFG_IMAGE_SLOTS; i++) {
		*p++ = badge_store_image_present(i) ? 1 : 0;
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, s_snap, p - s_snap);
}

/* f0de000D — text screen (write index to select, then read). Read returns
 *   present(u8); if present: hlen(u8), header[hlen], le16 blen, body[blen]. */
static uint8_t s_scr_sel;
static uint8_t s_scrrd[3 + APP_CFG_HEADER_MAX + APP_CFG_BODY_MAX];

static ssize_t scrrd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	s_scr_sel = ((const uint8_t *)buf)[0];
	return len;
}

static ssize_t scrrd_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	const struct badge_screen *s =
		(s_scr_sel < APP_CFG_SCREEN_COUNT) ? app_config_get_screen(s_scr_sel) : NULL;
	uint8_t *p = s_scrrd;

	if (s == NULL) {
		*p++ = 0; /* not present */
	} else {
		size_t hn = strnlen(s->header, APP_CFG_HEADER_MAX - 1);
		size_t bn = strnlen(s->body, APP_CFG_BODY_MAX - 1);

		*p++ = 1;
		*p++ = (uint8_t)hn;
		memcpy(p, s->header, hn);
		p += hn;
		sys_put_le16((uint16_t)bn, p);
		p += 2;
		memcpy(p, s->body, bn);
		p += bn;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, s_scrrd, p - s_scrrd);
}

/* f0de000E — image pixels (write slot to select, then read). The select-write
 * reads the slot from flash once into the buffer (avoids re-reading per blob
 * chunk); read returns present(u8), fmt(u8), le16 w, le16 h, le16 len, pixels[len]. */
static uint8_t s_imgrd[8 + BADGE_IMG_GRAY2_BYTES];
static size_t  s_imgrd_len = 8;

static ssize_t imgrd_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	uint8_t  slot = ((const uint8_t *)buf)[0];
	uint8_t  fmt = 0;
	size_t   ilen = 0;
	uint16_t w = 0, h = 0;
	int rc = (slot < BADGE_IMAGE_SLOTS)
			 ? badge_store_image_read(slot, s_imgrd + 8, BADGE_IMG_GRAY2_BYTES,
						  &fmt, &ilen, &w, &h)
			 : -1;

	if (rc == 0) {
		s_imgrd[0] = 1;
		s_imgrd[1] = fmt;
		sys_put_le16(w, s_imgrd + 2);
		sys_put_le16(h, s_imgrd + 4);
		sys_put_le16((uint16_t)ilen, s_imgrd + 6);
		s_imgrd_len = 8 + ilen;
	} else {
		memset(s_imgrd, 0, 8);
		s_imgrd_len = 8;
	}
	return len;
}

static ssize_t imgrd_read(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, s_imgrd, s_imgrd_len);
}

BT_GATT_SERVICE_DEFINE(vbx_cfg_svc,
	BT_GATT_PRIMARY_SERVICE(&vbx_svc_uuid),
	BT_GATT_CHARACTERISTIC(&vbx_img_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, img_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_name_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, name_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_screen_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, screen_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_imgslot_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, imgslot_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_display_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, display_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_attnd_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, attendee_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_frmled_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, frmled_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_ota_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, ota_write_char, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_otast_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, otast_read, NULL, NULL),
	/* Read-back: config snapshot, and select-then-read for screen text / image pixels. */
	BT_GATT_CHARACTERISTIC(&vbx_snap_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, snap_read, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_scrrd_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, scrrd_read, scrrd_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_imgrd_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, imgrd_read, imgrd_write, NULL),
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
