#include "bl_state.h"

#include <string.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#define BL_STATE_MAGIC 0x53584256u /* 'VBXS' */
#define REC_SIZE       ((off_t)sizeof(struct bl_state_rec))
#define FA_ID          FIXED_PARTITION_ID(bl_state_partition)

BUILD_ASSERT(sizeof(struct bl_state_rec) == 32, "bl_state_rec must be 32 bytes");

/* crc8 over the record with the crc8 field zeroed. */
static uint8_t rec_crc(const struct bl_state_rec *r)
{
	struct bl_state_rec t = *r;

	t.crc8 = 0;
	return crc8_ccitt(0, (const uint8_t *)&t, sizeof(t));
}

static bool rec_valid(const struct bl_state_rec *r)
{
	return r->magic == BL_STATE_MAGIC && rec_crc(r) == r->crc8;
}

int bl_state_read(struct bl_state_rec *out)
{
	const struct flash_area *fa;

	if (flash_area_open(FA_ID, &fa) != 0) {
		return -1;
	}

	bool found = false;
	uint16_t best = 0;
	struct bl_state_rec r;

	for (off_t off = 0; off + REC_SIZE <= (off_t)fa->fa_size; off += REC_SIZE) {
		if (flash_area_read(fa, off, &r, sizeof(r)) != 0) {
			break;
		}
		if (!rec_valid(&r)) {
			continue;
		}
		if (!found || r.seq >= best) { /* seq is monotonic; writes are rare (no wrap in practice) */
			best = r.seq;
			*out = r;
			found = true;
		}
	}

	flash_area_close(fa);
	return found ? 0 : -1;
}

int bl_state_append(struct bl_state_rec *rec)
{
	const struct flash_area *fa;

	if (flash_area_open(FA_ID, &fa) != 0) {
		return -1;
	}

	int rc = -1;
	bool have = false;
	uint16_t latest = 0;
	off_t free_off = -1;
	struct bl_state_rec r;

	for (off_t off = 0; off + REC_SIZE <= (off_t)fa->fa_size; off += REC_SIZE) {
		if (flash_area_read(fa, off, &r, sizeof(r)) != 0) {
			goto out;
		}
		if (rec_valid(&r)) {
			if (!have || r.seq >= latest) {
				latest = r.seq;
			}
			have = true;
		} else if (free_off < 0) {
			free_off = off;
		}
	}

	rec->magic = BL_STATE_MAGIC;
	rec->seq = have ? (uint16_t)(latest + 1) : 1;
	rec->rsvd = 0;
	rec->crc8 = rec_crc(rec);

	if (free_off < 0) {
		/* Log full: erase and restart (current snapshot is in `rec`). */
		if (flash_area_erase(fa, 0, fa->fa_size) != 0) {
			goto out;
		}
		free_off = 0;
	}

	rc = flash_area_write(fa, free_off, rec, sizeof(*rec));
out:
	flash_area_close(fa);
	return rc;
}

static int rmw(int slot, void (*mut)(struct bl_slot_meta *))
{
	struct bl_state_rec r;

	if (slot < 0 || slot >= BL_NUM_SLOTS) {
		return -1;
	}
	if (bl_state_read(&r) != 0) {
		memset(&r, 0, sizeof(r));
	}
	mut(&r.slot[slot]);
	return bl_state_append(&r);
}

static int g_version;
static uint32_t g_image_len;

static void mut_pending(struct bl_slot_meta *m)
{
	m->image_len = g_image_len;
	m->version = g_version;
	m->valid = 1;
	m->confirmed = 0;
	m->attempts = 0;
}

static void mut_confirm(struct bl_slot_meta *m)
{
	m->valid = 1;
	m->confirmed = 1;
	m->attempts = 0;
}

static void mut_attempt(struct bl_slot_meta *m)
{
	m->attempts++;
}

int bl_state_set_pending(int slot, uint32_t version, uint32_t image_len)
{
	g_version = version;
	g_image_len = image_len;
	return rmw(slot, mut_pending);
}

int bl_state_confirm(int slot)
{
	struct bl_state_rec r;

	if (slot < 0 || slot >= BL_NUM_SLOTS) {
		return -1;
	}
	/* Idempotent: skip the write (and flash wear) if already confirmed. */
	if (bl_state_read(&r) == 0 && r.slot[slot].confirmed && r.slot[slot].attempts == 0) {
		return 0;
	}
	return rmw(slot, mut_confirm);
}

int bl_state_inc_attempts(int slot)
{
	return rmw(slot, mut_attempt);
}
