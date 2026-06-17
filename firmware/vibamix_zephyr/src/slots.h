#ifndef VIBAMIX_SLOTS_H
#define VIBAMIX_SLOTS_H

/*
 * Which direct-XIP slot THIS app image was linked for, derived at compile time
 * from the code-partition load offset. The slot-A and slot-B builds are
 * identical source; only CONFIG_FLASH_LOAD_OFFSET differs (see scripts/build_slots.sh).
 */

#include <zephyr/devicetree.h>
#include <zephyr/storage/flash_map.h>
#include "bl_state.h"

#define SLOT_A_OFF DT_REG_ADDR(DT_NODELABEL(slot_a_partition))
#define SLOT_B_OFF DT_REG_ADDR(DT_NODELABEL(slot_b_partition))

#if CONFIG_FLASH_LOAD_OFFSET == SLOT_A_OFF
#define MY_SLOT          BL_SLOT_A
#define OTHER_SLOT       BL_SLOT_B
#define OTHER_SLOT_FA_ID FIXED_PARTITION_ID(slot_b_partition)
#elif CONFIG_FLASH_LOAD_OFFSET == SLOT_B_OFF
#define MY_SLOT          BL_SLOT_B
#define OTHER_SLOT       BL_SLOT_A
#define OTHER_SLOT_FA_ID FIXED_PARTITION_ID(slot_a_partition)
#else
#error "App is not linked for slot A or slot B (check zephyr,code-partition)"
#endif

#endif /* VIBAMIX_SLOTS_H */
