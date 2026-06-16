#include "mesh_model.h"
#include "app_config.h"
#include "badge_store.h"
#include "image_xfer.h"

#include <string.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* Company ID for the vendor model. Nordic's CID for the prototype; swap for your
 * own registered Company ID before shipping. */
#define VIBAMIX_CID      CONFIG_BT_COMPANY_ID
#define VIBAMIX_MODEL_ID 0x0001

/* Vendor opcodes (3-byte: 0x_C0_<b0>_<cid>). */
#define OP_SET_NAME      BT_MESH_MODEL_OP_3(0x01, VIBAMIX_CID) /* UTF-8 string */
#define OP_SET_FUN_FACT  BT_MESH_MODEL_OP_3(0x02, VIBAMIX_CID) /* UTF-8 string */
#define OP_SET_LED_COLOR BT_MESH_MODEL_OP_3(0x03, VIBAMIX_CID) /* r,g,b */
#define OP_IMG_START     BT_MESH_MODEL_OP_3(0x04, VIBAMIX_CID) /* le16 size,w,h */
#define OP_IMG_DATA      BT_MESH_MODEL_OP_3(0x05, VIBAMIX_CID) /* le16 off, bytes */
#define OP_IMG_END       BT_MESH_MODEL_OP_3(0x06, VIBAMIX_CID) /* le32 crc */
#define OP_HEARTBEAT     BT_MESH_MODEL_OP_3(0x07, VIBAMIX_CID) /* empty */
#define OP_SCREEN_HDR    BT_MESH_MODEL_OP_3(0x08, VIBAMIX_CID) /* u8 idx, header */
#define OP_SCREEN_BODY   BT_MESH_MODEL_OP_3(0x09, VIBAMIX_CID) /* u8 idx,seq,last, body */
#define OP_DISPLAY       BT_MESH_MODEL_OP_3(0x0A, VIBAMIX_CID) /* u8 kind, idx */

static const struct mesh_config_handlers *s_cfg;

/* Reassembly for a text screen pushed over mesh: header arrives in OP_SCREEN_HDR,
 * body in OP_SCREEN_BODY chunks (seq-ordered, terminated by last=1). Best-effort:
 * a dropped/out-of-order chunk aborts the update (GATT is the reliable path). */
static struct {
	bool    active;
	uint8_t idx;
	uint8_t next_seq;
	size_t  hlen;
	size_t  blen;
	char    header[APP_CFG_HEADER_MAX];
	char    body[APP_CFG_BODY_MAX];
} s_scr;

void mesh_model_set_config_handlers(const struct mesh_config_handlers *h)
{
	s_cfg = h;
}

static int handle_set_name(const struct bt_mesh_model *model,
			   struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	if (s_cfg && s_cfg->set_name) {
		s_cfg->set_name((const char *)buf->data, buf->len);
	}
	return 0;
}

static int handle_set_fun_fact(const struct bt_mesh_model *model,
			       struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	if (s_cfg && s_cfg->set_fun_fact) {
		s_cfg->set_fun_fact((const char *)buf->data, buf->len);
	}
	return 0;
}

static int handle_set_led_color(const struct bt_mesh_model *model,
				struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t r = net_buf_simple_pull_u8(buf);
	uint8_t g = net_buf_simple_pull_u8(buf);
	uint8_t b = net_buf_simple_pull_u8(buf);

	if (s_cfg && s_cfg->set_led_color) {
		s_cfg->set_led_color(r, g, b);
	}
	return 0;
}

static int handle_img_start(const struct bt_mesh_model *model,
			    struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint16_t size = net_buf_simple_pull_le16(buf);
	uint16_t w = net_buf_simple_pull_le16(buf);
	uint16_t h = net_buf_simple_pull_le16(buf);

	/* Legacy 1bpp push straight to the panel (no storage slot). */
	image_xfer_start(BADGE_SLOT_NONE, BADGE_FMT_BW, size, w, h);
	return 0;
}

static int handle_img_data(const struct bt_mesh_model *model,
			   struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint16_t offset = net_buf_simple_pull_le16(buf);

	image_xfer_data(offset, buf->data, buf->len);
	return 0;
}

static int handle_img_end(const struct bt_mesh_model *model,
			  struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint32_t crc = net_buf_simple_pull_le32(buf);

	image_xfer_end(crc);
	return 0;
}

static int handle_heartbeat(const struct bt_mesh_model *model,
			    struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	if (s_cfg && s_cfg->heartbeat) {
		s_cfg->heartbeat();
	}
	return 0;
}

static int handle_screen_hdr(const struct bt_mesh_model *model,
			     struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t idx = net_buf_simple_pull_u8(buf);
	size_t  hlen = buf->len;

	if (idx >= APP_CFG_SCREEN_COUNT) {
		s_scr.active = false;
		return 0;
	}
	if (hlen > sizeof(s_scr.header) - 1) {
		hlen = sizeof(s_scr.header) - 1;
	}
	memcpy(s_scr.header, buf->data, hlen);
	s_scr.header[hlen] = '\0';
	s_scr.hlen = hlen;
	s_scr.idx = idx;
	s_scr.blen = 0;
	s_scr.next_seq = 0;
	s_scr.active = true;
	return 0;
}

static int handle_screen_body(const struct bt_mesh_model *model,
			      struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t idx = net_buf_simple_pull_u8(buf);
	uint8_t seq = net_buf_simple_pull_u8(buf);
	uint8_t last = net_buf_simple_pull_u8(buf);

	/* Abort on any mismatch — best-effort over the unacked group address. */
	if (!s_scr.active || idx != s_scr.idx || seq != s_scr.next_seq) {
		s_scr.active = false;
		return 0;
	}

	size_t n = buf->len;
	if (s_scr.blen + n > sizeof(s_scr.body) - 1) {
		n = sizeof(s_scr.body) - 1 - s_scr.blen;
	}
	memcpy(s_scr.body + s_scr.blen, buf->data, n);
	s_scr.blen += n;
	s_scr.next_seq++;

	if (last) {
		s_scr.body[s_scr.blen] = '\0';
		if (s_cfg && s_cfg->set_screen) {
			s_cfg->set_screen(s_scr.idx, s_scr.header, s_scr.hlen,
					  s_scr.body, s_scr.blen);
		}
		s_scr.active = false;
	}
	return 0;
}

static int handle_display(const struct bt_mesh_model *model,
			  struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t kind = net_buf_simple_pull_u8(buf);
	uint8_t idx = net_buf_simple_pull_u8(buf);

	if (s_cfg && s_cfg->display_screen) {
		s_cfg->display_screen(kind, idx);
	}
	return 0;
}

static const struct bt_mesh_model_op vibamix_op[] = {
	{ OP_SET_NAME,      BT_MESH_LEN_MIN(0),   handle_set_name },
	{ OP_SET_FUN_FACT,  BT_MESH_LEN_MIN(0),   handle_set_fun_fact },
	{ OP_SET_LED_COLOR, BT_MESH_LEN_EXACT(3), handle_set_led_color },
	{ OP_IMG_START,     BT_MESH_LEN_EXACT(6), handle_img_start },
	{ OP_IMG_DATA,      BT_MESH_LEN_MIN(3),   handle_img_data },
	{ OP_IMG_END,       BT_MESH_LEN_EXACT(4), handle_img_end },
	{ OP_HEARTBEAT,     BT_MESH_LEN_MIN(0),   handle_heartbeat },
	{ OP_SCREEN_HDR,    BT_MESH_LEN_MIN(1),   handle_screen_hdr },
	{ OP_SCREEN_BODY,   BT_MESH_LEN_MIN(3),   handle_screen_body },
	{ OP_DISPLAY,       BT_MESH_LEN_EXACT(2), handle_display },
	BT_MESH_MODEL_OP_END,
};

/* Foundation models (Config + Health server are required on the primary element). */
static struct bt_mesh_health_srv health_srv;
BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static struct bt_mesh_model sig_models[] = {
	BT_MESH_MODEL_CFG_SRV,
	BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
};

static struct bt_mesh_model vnd_models[] = {
	BT_MESH_MODEL_VND_CB(VIBAMIX_CID, VIBAMIX_MODEL_ID, vibamix_op, NULL, NULL, NULL),
};

static const struct bt_mesh_elem elements[] = {
	BT_MESH_ELEM(0, sig_models, vnd_models),
};

static const struct bt_mesh_comp comp = {
	.cid = VIBAMIX_CID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

const struct bt_mesh_comp *mesh_model_comp(void)
{
	return &comp;
}

void mesh_model_bind_and_subscribe(uint16_t app_idx, uint16_t group_addr)
{
	/* The keys[]/groups[] arrays are allocated by BT_MESH_MODEL_VND_CB; writing
	 * entry 0 here is exactly what a Config Server would do on a bind/subscribe.
	 * Sized by CONFIG_BT_MESH_MODEL_KEY_COUNT / _GROUP_COUNT in prj.conf. */
	vnd_models[0].keys[0] = app_idx;
	vnd_models[0].groups[0] = group_addr;
	printk("mesh: bound app_idx %u, subscribed group 0x%04x\n", app_idx, group_addr);
}
