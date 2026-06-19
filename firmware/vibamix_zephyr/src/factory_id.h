#ifndef VIBAMIX_FACTORY_ID_H
#define VIBAMIX_FACTORY_ID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-unit factory record, assigned and written once at manufacturing time by
 * firmware/flashtool/ into the `factory` flash partition, and read-only at
 * runtime. It carries the badge's unique 15-bit id so the mesh unicast address,
 * the config code, and the "vibamix-XXXX" GAP name don't collide across a large
 * fleet (a random device-id slice collides at ~75% for 300 badges).
 *
 * On-flash layout (little-endian) at the start of the `factory` partition. The
 * flashtool writes exactly this blob; keep it in sync.
 *   off 0  u32  magic   = FACTORY_ID_MAGIC ('VBXF')
 *   off 4  u16  id       (1..0x7FFF)
 *   off 6  u16  id_chk   = (uint16_t)~id   (cheap torn-write / blank check)
 */
#define FACTORY_ID_MAGIC 0x46584256u /* 'V','B','X','F' little-endian */

/*
 * The assigned unique id, or 0 if the partition is blank / invalid (e.g. a dev
 * unit flashed with `west flash` and never run through the flashtool). Callers
 * fall back to the device-id-derived address in that case. Cached after the
 * first read.
 */
uint16_t factory_id_get(void);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_FACTORY_ID_H */
