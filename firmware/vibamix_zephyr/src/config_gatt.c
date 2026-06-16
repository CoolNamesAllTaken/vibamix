#include "config_gatt.h"
#include "image_xfer.h"

#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk.h>

#define VBX_UUID_SVC  BT_UUID_128_ENCODE(0xf0de0001, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_IMG  BT_UUID_128_ENCODE(0xf0de0002, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)
#define VBX_UUID_NAME BT_UUID_128_ENCODE(0xf0de0003, 0x4b1c, 0x4e2a, 0x9a11, 0xa1b2c3d4e5f6)

static const struct bt_uuid_128 vbx_svc_uuid  = BT_UUID_INIT_128(VBX_UUID_SVC);
static const struct bt_uuid_128 vbx_img_uuid  = BT_UUID_INIT_128(VBX_UUID_IMG);
static const struct bt_uuid_128 vbx_name_uuid = BT_UUID_INIT_128(VBX_UUID_NAME);

/* Image-upload framing (matches the mesh vendor-model opcodes). */
#define OP_IMG_START 0x01 /* le16 size, le16 w, le16 h */
#define OP_IMG_DATA  0x02 /* le16 offset, bytes[]      */
#define OP_IMG_END   0x03 /* le32 crc32-ieee           */

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
	case OP_IMG_START:
		if (len < 7) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_start(sys_get_le16(p + 1), sys_get_le16(p + 3), sys_get_le16(p + 5));
		break;
	case OP_IMG_DATA:
		if (len < 3) {
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
		image_xfer_data(sys_get_le16(p + 1), p + 3, len - 3);
		break;
	case OP_IMG_END:
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

BT_GATT_SERVICE_DEFINE(vbx_cfg_svc,
	BT_GATT_PRIMARY_SERVICE(&vbx_svc_uuid),
	BT_GATT_CHARACTERISTIC(&vbx_img_uuid.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, img_write, NULL),
	BT_GATT_CHARACTERISTIC(&vbx_name_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE, NULL, name_write, NULL),
);
