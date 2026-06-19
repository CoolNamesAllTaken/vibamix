#ifndef VBX_BL_DIAG_H
#define VBX_BL_DIAG_H

/*
 * Bootloader on-ePaper diagnostics. The board has no usable serial, so on a
 * trial boot / revert / halt we render the boot decision + per-slot state to the
 * panel via the minimal driver (epd_min). main.c gathers the facts (live CRC,
 * version, bl_state fields) into bl_diag and calls bl_diag_show().
 */

#include <stdbool.h>
#include <stdint.h>

#define BL_DIAG_NUM_SLOTS 2

struct bl_diag_slot {
	bool     valid;     /* bl_state says this slot was OTA-verified */
	bool     crc_ok;    /* live trailer CRC verified this boot */
	bool     confirmed; /* app confirmed a healthy boot */
	uint8_t  attempts;  /* trial boots since last confirm */
	uint32_t version;
	uint32_t image_len;
};

struct bl_diag {
	bool                have_state; /* a valid bl_state record was found */
	uint16_t            seq;        /* bl_state record seq (if have_state) */
	int                 chosen;     /* slot index booted this time, or -1 (halt) */
	bool                trial;      /* chosen slot is an unconfirmed trial */
	const char         *reason;     /* short label, e.g. "TRIAL" / "REVERT" / "HALT" */
	struct bl_diag_slot slot[BL_DIAG_NUM_SLOTS];
};

/* Init the panel (lazily) and full-refresh the diagnostic screen. Best-effort:
 * a missing/stuck panel is silently ignored so it never blocks booting. */
void bl_diag_show(const struct bl_diag *d);

#endif /* VBX_BL_DIAG_H */
