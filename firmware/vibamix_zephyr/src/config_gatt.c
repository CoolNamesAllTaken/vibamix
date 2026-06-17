#include "config_gatt.h"
#include "app_config.h"
#include "badge_store.h"
#include "image_xfer.h"
#include "ota.h"

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

static const struct bt_uuid_128 vbx_svc_uuid     = BT_UUID_INIT_128(VBX_UUID_SVC);
static const struct bt_uuid_128 vbx_img_uuid     = BT_UUID_INIT_128(VBX_UUID_IMG);
static const struct bt_uuid_128 vbx_name_uuid    = BT_UUID_INIT_128(VBX_UUID_NAME);
static const struct bt_uuid_128 vbx_screen_uuid  = BT_UUID_INIT_128(VBX_UUID_SCREEN);
static const struct bt_uuid_128 vbx_imgslot_uuid = BT_UUID_INIT_128(VBX_UUID_IMGSLOT);
static const struct bt_uuid_128 vbx_display_uuid = BT_UUID_INIT_128(VBX_UUID_DISPLAY);
static const struct bt_uuid_128 vbx_attnd_uuid   = BT_UUID_INIT_128(VBX_UUID_ATTND);
static const struct bt_uuid_128 vbx_frmled_uuid  = BT_UUID_INIT_128(VBX_UUID_FRMLED);
static const struct bt_uuid_128 vbx_ota_uuid     = BT_UUID_INIT_128(VBX_UUID_OTA);

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

/* f0de0009 — OTA firmware update. Streams a signed MCUboot image into slot1.
 * START = op,le32 total_size ; DATA = op,le32 offset,bytes ; END = op,le32 crc32.
 * (u32 fields — the image is ~360 KB, well beyond the u16 used elsewhere.) */
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
);
