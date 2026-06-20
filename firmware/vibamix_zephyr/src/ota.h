#ifndef VIBAMIX_OTA_H
#define VIBAMIX_OTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE OTA receiver for the custom direct-XIP A/B scheme (no MCUboot). Streams a
 * new app image — built for the *inactive* slot, with a CRC trailer — into that
 * slot via raw flash_area writes, verifies it, marks it pending in bl_state, and
 * reboots. The bootloader then CRC-verifies + boots it; if it fails to confirm,
 * the watchdog reset makes the bootloader revert to the previous slot.
 *
 * The host must send the image for whichever slot is currently inactive (it reads
 * the OTA-status GATT characteristic to learn which). See docs/ble-config-api.md.
 */

/* Start a transfer of `total_size` bytes (image + 32-byte trailer). Returns 0. */
int ota_begin(uint32_t total_size);

/* Append a chunk; `offset` must equal the bytes received so far (ordered). */
int ota_write(uint32_t offset, const uint8_t *data, uint16_t len);

/* Finish: verify the trailer (magic + CRC) against the written slot and the
 * host-supplied `crc`, mark the slot pending in bl_state, and schedule a reboot.
 * Returns 0 if the image was accepted and the reboot is pending. */
int ota_finish(uint32_t crc);

/* Call once early at boot: confirm THIS slot in bl_state (a valid boot signal),
 * so the bootloader keeps it instead of reverting. */
void ota_confirm_on_boot(void);

/* The slot the host should target (the inactive one): 0 = A, 1 = B. */
uint8_t ota_inactive_slot(void);

/* Transfer progress (for a UI progress bar): bytes accepted so far, and the
 * total announced at ota_begin(). Both 0 when no transfer is active. */
uint32_t ota_written(void);
uint32_t ota_total(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_OTA_H */
