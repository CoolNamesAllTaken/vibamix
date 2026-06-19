#include "mesh_model.h"
#include "app_config.h"
#include "badge_store.h"
#include "image_xfer.h"
#include "mesh_keys.h"

#include <errno.h>
#include <string.h>
#include <zephyr/bluetooth/mesh.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

/* Company ID for the vendor model. Nordic's CID for the prototype; swap for your
 * own registered Company ID before shipping. */
#define VIBAMIX_CID      CONFIG_BT_COMPANY_ID
#define VIBAMIX_MODEL_ID 0x0001

/* Vendor opcodes (3-byte: 0x_C0_<b0>_<cid>). Broadcast surface. Most are ephemeral
 * (draw/override live state, store nothing); DISPLAY is the exception — it carries
 * no content, just tells every badge to show a frame it already has stored.
 * Opcodes 0x01/0x02/0x0B/0x0C (set name/fun-fact/attendee/frame-LED) were removed —
 * that stored config is now GATT-only. */
#define OP_SET_LED       BT_MESH_MODEL_OP_3(0x03, VIBAMIX_CID) /* anim,r,g,b (live, not stored) */
#define OP_IMG_START     BT_MESH_MODEL_OP_3(0x04, VIBAMIX_CID) /* le16 size,w,h */
#define OP_IMG_DATA      BT_MESH_MODEL_OP_3(0x05, VIBAMIX_CID) /* le16 off, bytes */
#define OP_IMG_END       BT_MESH_MODEL_OP_3(0x06, VIBAMIX_CID) /* le32 crc */
#define OP_HEARTBEAT     BT_MESH_MODEL_OP_3(0x07, VIBAMIX_CID) /* empty */
#define OP_SHOW_TEXT_HDR BT_MESH_MODEL_OP_3(0x08, VIBAMIX_CID) /* title (draw, not stored) */
#define OP_SHOW_TEXT_BODY BT_MESH_MODEL_OP_3(0x09, VIBAMIX_CID) /* seq,last, body */
#define OP_DISPLAY       BT_MESH_MODEL_OP_3(0x0A, VIBAMIX_CID) /* kind,idx (show a stored frame) */

static const struct mesh_config_handlers *s_cfg;

/* Reassembly for a text frame drawn over mesh: title arrives in OP_SHOW_TEXT_HDR,
 * body in OP_SHOW_TEXT_BODY chunks (seq-ordered, terminated by last=1). The
 * completed frame is drawn straight to the panel — never stored. Best-effort: a
 * dropped/out-of-order chunk aborts the update (GATT is the reliable path). */
static struct {
	bool    active;
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

static int handle_set_led(const struct bt_mesh_model *model,
			  struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t anim = net_buf_simple_pull_u8(buf);
	uint8_t r = net_buf_simple_pull_u8(buf);
	uint8_t g = net_buf_simple_pull_u8(buf);
	uint8_t b = net_buf_simple_pull_u8(buf);

	if (s_cfg && s_cfg->show_led) {
		s_cfg->show_led(anim, r, g, b);
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
		/* Optional event name in the payload (capped; keeps the beat
		 * unsegmented). The handler copies it, so a buf pointer is fine. */
		size_t len = buf->len;

		if (len > 8) {
			len = 8;
		}
		s_cfg->heartbeat((const char *)buf->data, len);
	}
	return 0;
}

static int handle_show_text_hdr(const struct bt_mesh_model *model,
				struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	size_t hlen = buf->len;

	if (hlen > sizeof(s_scr.header) - 1) {
		hlen = sizeof(s_scr.header) - 1;
	}
	memcpy(s_scr.header, buf->data, hlen);
	s_scr.header[hlen] = '\0';
	s_scr.hlen = hlen;
	s_scr.blen = 0;
	s_scr.next_seq = 0;
	s_scr.active = true;
	return 0;
}

static int handle_show_text_body(const struct bt_mesh_model *model,
				 struct bt_mesh_msg_ctx *ctx, struct net_buf_simple *buf)
{
	uint8_t seq = net_buf_simple_pull_u8(buf);
	uint8_t last = net_buf_simple_pull_u8(buf);

	/* Abort on any mismatch — best-effort over the unacked group address. */
	if (!s_scr.active || seq != s_scr.next_seq) {
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
		if (s_cfg && s_cfg->show_text) {
			s_cfg->show_text(s_scr.header, s_scr.hlen,
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
	{ OP_SET_LED,        BT_MESH_LEN_EXACT(4), handle_set_led },
	{ OP_IMG_START,      BT_MESH_LEN_EXACT(6), handle_img_start },
	{ OP_IMG_DATA,       BT_MESH_LEN_MIN(3),   handle_img_data },
	{ OP_IMG_END,        BT_MESH_LEN_EXACT(4), handle_img_end },
	{ OP_HEARTBEAT,      BT_MESH_LEN_MIN(0),   handle_heartbeat },
	{ OP_SHOW_TEXT_HDR,  BT_MESH_LEN_MIN(0),   handle_show_text_hdr },
	{ OP_SHOW_TEXT_BODY, BT_MESH_LEN_MIN(2),   handle_show_text_body },
	{ OP_DISPLAY,        BT_MESH_LEN_EXACT(2), handle_display },
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

/* Bound app key + group, remembered so the gateway path can originate to them. */
static uint16_t s_net_idx;
static uint16_t s_app_idx;
static uint16_t s_group_addr;

void mesh_model_bind_and_subscribe(uint16_t app_idx, uint16_t group_addr)
{
	/* The keys[]/groups[] arrays are allocated by BT_MESH_MODEL_VND_CB; writing
	 * entry 0 here is exactly what a Config Server would do on a bind/subscribe.
	 * Sized by CONFIG_BT_MESH_MODEL_KEY_COUNT / _GROUP_COUNT in prj.conf. */
	vnd_models[0].keys[0] = app_idx;
	vnd_models[0].groups[0] = group_addr;
	s_net_idx = VIBAMIX_NET_IDX;
	s_app_idx = app_idx;
	s_group_addr = group_addr;
	printk("mesh: bound app_idx %u, subscribed group 0x%04x\n", app_idx, group_addr);
}

int mesh_model_send_to_group(const uint8_t *access, size_t len)
{
	/* `access` is a complete vendor-model access payload (opcode + params). Wrap it
	 * and publish to the "all badges" group with the app key. The mesh stack handles
	 * app + network encryption and (if needed) segmentation. */
	NET_BUF_SIMPLE_DEFINE(buf, BT_MESH_TX_SDU_MAX);
	struct bt_mesh_msg_ctx ctx = {
		.net_idx = s_net_idx,
		.app_idx = s_app_idx,
		.addr = s_group_addr,
		.send_ttl = BT_MESH_TTL_DEFAULT,
	};

	if (len > net_buf_simple_tailroom(&buf)) {
		return -EMSGSIZE;
	}
	net_buf_simple_add_mem(&buf, access, len);
	return bt_mesh_model_send(&vnd_models[0], &ctx, &buf, NULL, NULL);
}
