#ifndef VIBAMIX_IDENTITY_H
#define VIBAMIX_IDENTITY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The badge's stable identity, derived once from the FICR device id. Both the
 * mesh unicast address and the human-readable config code come from the same
 * source so the printed/QR code matches the badge's mesh address.
 */

/* Non-zero 15-bit mesh unicast address (0x0001..0x7FFF). */
uint16_t app_identity_addr(void);

/* 4 hex chars + NUL, e.g. "1A2F" — the code shown on the config screen / QR. */
void app_identity_code(char out[5]);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_IDENTITY_H */
