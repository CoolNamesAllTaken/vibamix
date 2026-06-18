#ifndef VBX_BL_STATE_H
#define VBX_BL_STATE_H

/*
 * Bootloader state: per-slot metadata for the custom direct-XIP A/B scheme,
 * stored as a 32-byte append log in the `bl_state` flash partition. Shared by
 * the bootloader (read + attempt counting) and the app (confirm + pending).
 *
 * The latest valid record (highest seq, good crc8) is authoritative. A torn
 * write fails crc8 and is ignored, so the previous record survives.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BL_SLOT_A      0
#define BL_SLOT_B      1
#define BL_NUM_SLOTS   2
#define BL_MAX_ATTEMPTS 3 /* trial boots allowed before an unconfirmed image is dropped */

struct __packed bl_slot_meta {
	uint32_t image_len;  /* image size (locates the trailer); 0 = none */
	uint32_t version;
	uint8_t  valid;      /* 1 = an OTA wrote+CRC-verified this slot */
	uint8_t  confirmed;  /* 1 = the app booted healthily from this slot */
	uint8_t  attempts;   /* trial boots since last confirm */
	uint8_t  stale;      /* 1 = superseded by a newer install (still bootable, deprioritized) */
};

struct __packed bl_state_rec {
	uint32_t magic;
	uint16_t seq;
	uint8_t  crc8;
	uint8_t  rsvd;
	struct bl_slot_meta slot[BL_NUM_SLOTS];
};

/* Read the latest valid record into *out. Returns 0, or -1 if none/blank. */
int bl_state_read(struct bl_state_rec *out);

/* Append a record (magic/seq/crc8 are filled in here). Returns 0 on success. */
int bl_state_append(struct bl_state_rec *rec);

/* App helpers (read-modify-append). */
int bl_state_set_pending(int slot, uint32_t version, uint32_t image_len);
int bl_state_confirm(int slot);
int bl_state_inc_attempts(int slot);

#ifdef __cplusplus
}
#endif

#endif /* VBX_BL_STATE_H */
