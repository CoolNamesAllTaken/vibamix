#ifndef VIBAMIX_OTA_H
#define VIBAMIX_OTA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE OTA receiver: streams a signed MCUboot image into the secondary slot
 * (slot1) via Zephyr's flash_img API, then asks MCUboot to swap on reboot.
 * Driven by the OTA GATT characteristic (see config_gatt.c). MCUboot performs
 * the cryptographic verification on swap; the app-level CRC here is a fast
 * pre-reboot sanity check.
 *
 * Swap-with-revert: the freshly-swapped image must confirm itself on boot
 * (ota_confirm_on_boot) or MCUboot reverts to the previous image.
 */

/* Start a transfer of `total_size` bytes. Returns 0 on success. */
int ota_begin(uint32_t total_size);

/* Append a chunk; `offset` must equal the bytes received so far (ordered). */
int ota_write(uint32_t offset, const uint8_t *data, uint16_t len);

/* Finish: flush, verify size + CRC-32/IEEE, request a swap, and schedule a
 * reboot (~1.2 s later so the GATT response goes out first). Returns 0 if the
 * image was accepted and the reboot is pending. */
int ota_finish(uint32_t crc);

/* Call once early at boot: confirms the running image so MCUboot keeps it
 * (no-op if already confirmed). Reaching this point means the image booted far
 * enough to be trustworthy; otherwise MCUboot reverts on the next reset. */
void ota_confirm_on_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_OTA_H */
