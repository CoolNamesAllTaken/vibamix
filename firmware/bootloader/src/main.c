/*
 * Vibamix first-stage bootloader — custom direct-XIP A/B with CRC + watchdog
 * auto-revert (no MCUboot).
 *
 * Boot decision:
 *   1. Debugger attached  -> jump straight to slot A (the dev/flash-debug path;
 *      the debugger `load`s an unsigned ELF into slot A and we run it as-is).
 *   2. Else pick, from bl_state, the highest-version slot that is `valid`, whose
 *      CRC trailer verifies, and that is `confirmed` OR still has a trial attempt
 *      left. A trial boot bumps its attempt count *before* jumping, so a failed
 *      boot (watchdog/crash reset) is excluded next time -> revert to the other
 *      confirmed slot.
 *   3. If bl_state has nothing usable (e.g. first boot after an SWD flash of slot
 *      A, before the app has confirmed), fall back to scanning slot A for a valid
 *      trailer and booting it as a trial.
 *
 * RRAM is memory-mapped (XIP) at the flash offset, so slot images + trailers are
 * read through plain pointers; only bl_state needs the flash driver.
 */

#include <cmsis_core.h>
#include <hal/nrf_gpio.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "bl_state.h"
#include "diag.h"
#include "vbx_img.h"

/* TEMP boot-trace: blink the user LED (P2.00, active-low) N times with a raw busy
 * loop — no console / clock / kernel needed. Localizes a battery-only (no-debugger)
 * boot hang in the bootloader. Remove once found. */
#define BL_BOOT_TRACE 1
#if BL_BOOT_TRACE
static void bl_blink(int n)
{
	nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(2, 0));
	for (int i = 0; i < n; i++) {
		nrf_gpio_pin_write(NRF_GPIO_PIN_MAP(2, 0), 0);   /* LED on */
		for (volatile uint32_t d = 0; d < 4000000U; d++) { __asm__ volatile("nop"); }
		nrf_gpio_pin_write(NRF_GPIO_PIN_MAP(2, 0), 1);   /* LED off */
		for (volatile uint32_t d = 0; d < 4000000U; d++) { __asm__ volatile("nop"); }
	}
	for (volatile uint32_t d = 0; d < 12000000U; d++) { __asm__ volatile("nop"); }
}
static int bl_early_blink(void)
{
	bl_blink(1);   /* B1: bootloader C runtime alive (PRE_KERNEL_1) */
	return 0;
}
SYS_INIT(bl_early_blink, PRE_KERNEL_1, 0);
#else
#define bl_blink(n) ((void)0)
#endif

#define SLOT_A_OFF  DT_REG_ADDR(DT_NODELABEL(slot_a_partition))
#define SLOT_A_SIZE DT_REG_SIZE(DT_NODELABEL(slot_a_partition))
#define SLOT_B_OFF  DT_REG_ADDR(DT_NODELABEL(slot_b_partition))
#define SLOT_B_SIZE DT_REG_SIZE(DT_NODELABEL(slot_b_partition))

struct arm_vector_table {
	uint32_t msp;
	uint32_t reset;
};

static uint32_t slot_off(int slot)
{
	return slot == BL_SLOT_B ? SLOT_B_OFF : SLOT_A_OFF;
}

static uint32_t slot_size(int slot)
{
	return slot == BL_SLOT_B ? SLOT_B_SIZE : SLOT_A_SIZE;
}

static const uint8_t *slot_base(int slot)
{
	/* RRAM is mapped at 0x0, so the flash offset is the XIP address. */
	return (const uint8_t *)(uintptr_t)slot_off(slot);
}

static inline bool debugger_attached(void)
{
	return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0;
}

/* Scan a slot for a self-consistent trailer (magic + image_len == offset + CRC).
 * Used only when bl_state can't tell us image_len. Returns image_len or 0. */
static uint32_t scan_trailer(int slot, uint32_t *out_version)
{
	const uint8_t *base = slot_base(slot);
	uint32_t max = slot_size(slot);

	/* Trailers are 16-byte aligned and at least 16 bytes of image precede them. */
	for (uint32_t len = 16; len + VBX_IMG_TRAILER_SIZE <= max; len += 16) {
		const struct vbx_img_trailer *t =
			(const struct vbx_img_trailer *)(base + len);
		if (t->magic != VBX_IMG_MAGIC || t->image_len != len) {
			continue;
		}
		if (vbx_img_verify(base, len, out_version)) {
			return len;
		}
	}
	return 0;
}

static void boot_slot(int slot) __attribute__((noreturn));
static void boot_slot(int slot)
{
	/* Static so it doesn't live on the stack we are about to abandon. */
	static struct arm_vector_table *vt;

	vt = (struct arm_vector_table *)slot_base(slot);

	printk("bl: booting slot %c (msp=0x%08x reset=0x%08x)\n",
	       slot == BL_SLOT_B ? 'B' : 'A', vt->msp, vt->reset);

	if (IS_ENABLED(CONFIG_SYSTEM_TIMER_HAS_DISABLE_SUPPORT)) {
		sys_clock_disable();
	}

	/* Disable + acknowledge all interrupts (mirror MCUboot cleanup_arm_interrupts). */
	__ISB();
	__disable_irq();
	for (uint8_t i = 0; i < ARRAY_SIZE(NVIC->ICER); i++) {
		NVIC->ICER[i] = 0xFFFFFFFF;
	}
	for (uint8_t i = 0; i < ARRAY_SIZE(NVIC->ICPR); i++) {
		NVIC->ICPR[i] = 0xFFFFFFFF;
	}

	/* Clear the MPU so the app configures it from scratch. */
#if defined(CONFIG_CPU_HAS_ARM_MPU)
	{
		uint32_t n = (MPU->TYPE & MPU_TYPE_DREGION_Msk) >> MPU_TYPE_DREGION_Pos;

		for (uint32_t i = 0; i < n; i++) {
			ARM_MPU_ClrRegion(i);
		}
	}
#endif

	SCB->VTOR = (uint32_t)(uintptr_t)vt;
	__DSB();
	__set_MSP(vt->msp);
	__set_CONTROL(0x00); /* the app reconfigures the core on its own */
	__ISB();

	((void (*)(void))vt->reset)();

	CODE_UNREACHABLE;
}

/* Arm wdt31 so a hung trial boot resets the SoC (-> revert). It keeps running
 * across the jump; the app feeds it after a healthy boot, and it pauses itself
 * in System OFF (WDT_OPT_PAUSE_IN_SLEEP). */
/* Gather the boot facts (live CRC, version, bl_state fields) for the on-ePaper
 * diagnostic screen. Live-verifies each slot's trailer so the panel shows the
 * real CRC pass/fail, independent of what bl_state claims. */
static void build_diag(struct bl_diag *d, const struct bl_state_rec *rec, bool have,
		       int choice, bool trial, const char *reason)
{
	memset(d, 0, sizeof(*d));
	d->have_state = have;
	d->seq = have ? rec->seq : 0;
	d->chosen = choice;
	d->trial = trial;
	d->reason = reason;

	for (int s = 0; s < BL_NUM_SLOTS; s++) {
		const struct bl_slot_meta *m = &rec->slot[s];
		uint32_t ver = 0;
		bool crc = false;

		if (have && m->image_len >= VBX_IMG_TRAILER_SIZE) {
			crc = vbx_img_verify(slot_base(s), m->image_len, &ver);
		}
		d->slot[s].valid = have && m->valid;
		d->slot[s].crc_ok = crc;
		d->slot[s].confirmed = have && m->confirmed;
		d->slot[s].attempts = have ? m->attempts : 0;
		d->slot[s].version = crc ? ver : (have ? m->version : 0);
		d->slot[s].image_len = have ? m->image_len : 0;
	}
}

static void arm_watchdog(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
	struct wdt_timeout_cfg cfg = {
		/* Generous: a healthy image confirms in a few seconds, but worst-case
		 * boot-to-confirm (ePaper HW init + bt_enable + mesh/settings) can exceed
		 * 8 s, which used to burn the trial and (with no revert target) brick. */
		.window = { .min = 0U, .max = 30000U },
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		printk("bl: watchdog not ready; booting without rollback timer\n");
		return;
	}
	if (wdt_install_timeout(wdt, &cfg) < 0) {
		printk("bl: wdt_install_timeout failed\n");
		return;
	}
	if (wdt_setup(wdt, WDT_OPT_PAUSE_IN_SLEEP | WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		printk("bl: wdt_setup failed\n");
	}
}

int main(void)
{
	bl_blink(2);   /* B2: bootloader main() reached (console init survived) */
	printk("\nvbx bootloader\n");

	if (debugger_attached()) {
		printk("bl: debugger attached -> slot A (dev path)\n");
		boot_slot(BL_SLOT_A);
	}

	struct bl_state_rec rec;
	bool have = (bl_state_read(&rec) == 0);

	int choice = -1;
	bool choice_trial = false;

	if (have) {
		/* Prefer the not-stale slot (the most recently installed image); a stale
		 * slot stays bootable as a revert/fallback target. Iterating low->high and
		 * only switching to a later slot when it is fresh and the incumbent is
		 * stale yields: prefer not-stale, and on a tie (both stale or both fresh)
		 * keep the lower index -> slot 0 (A). Selection is version-independent so
		 * equal-version A/B bundles install correctly. */
		for (int s = 0; s < BL_NUM_SLOTS; s++) {
			const struct bl_slot_meta *m = &rec.slot[s];
			uint32_t ver = 0;

			if (!m->valid) {
				continue;
			}
			if (!vbx_img_verify(slot_base(s), m->image_len, &ver)) {
				printk("bl: slot %d CRC failed\n", s);
				continue;
			}
			bool bootable = m->confirmed || (m->attempts < BL_MAX_ATTEMPTS);

			if (!bootable) {
				printk("bl: slot %d unconfirmed + out of attempts\n", s);
				continue;
			}
			if (choice < 0 || (!m->stale && rec.slot[choice].stale)) {
				choice = s;
				choice_trial = !m->confirmed;
			}
		}
	}

	/* Only scan as a fallback when bl_state has never recorded slot A (a fresh
	 * SWD flash). Once seeded below, a slot-A image that keeps failing to confirm
	 * is left to the normal attempt logic (it trials once, then we halt) rather
	 * than being re-seeded into an endless retry. */
	if (choice < 0 && (!have || !rec.slot[BL_SLOT_A].valid)) {
		/* First boot after an SWD flash. Find + run slot A by scanning for its
		 * trailer, recording it as a trial. */
		uint32_t ver = 0;
		uint32_t len = scan_trailer(BL_SLOT_A, &ver);

		if (len) {
			printk("bl: no state; slot A trailer found (v%u, %u B), trial boot\n",
			       (unsigned)ver, (unsigned)len);
			bl_state_set_pending(BL_SLOT_A, ver, len);
			choice = BL_SLOT_A;
			choice_trial = true;
		}
	}

	if (choice < 0) {
		/* Last resort before bricking: scan BOTH slots for any CRC-valid image
		 * and trial-boot the highest-version one. This rescues the common OTA
		 * case (good new image, no trailered revert target) — better a slow
		 * retry loop than a dead board. */
		int ls = -1;
		uint32_t ls_ver = 0, ls_len = 0;

		for (int s = 0; s < BL_NUM_SLOTS; s++) {
			uint32_t ver = 0;
			uint32_t len = scan_trailer(s, &ver);

			if (len && (ls < 0 || ver >= ls_ver)) {
				ls = s;
				ls_ver = ver;
				ls_len = len;
			}
		}
		if (ls >= 0) {
			printk("bl: last-resort trial boot of slot %d (v%u, %u B)\n",
			       ls, (unsigned)ls_ver, (unsigned)ls_len);
			bl_state_set_pending(ls, ls_ver, ls_len);
			choice = ls;
			choice_trial = true;
		}
	}

	/* Re-read so the diagnostic screen reflects any set_pending above. */
	have = (bl_state_read(&rec) == 0);

	if (choice < 0) {
		struct bl_diag d;

		build_diag(&d, &rec, have, -1, false, "HALT NO BOOTABLE IMG");
		bl_diag_show(&d);
		printk("bl: no bootable image; halting\n");
		for (;;) {
			k_msleep(1000);
		}
	}

	/* A revert = we picked a confirmed slot while the other slot is a valid but
	 * unconfirmed image (a failed trial we just rolled back from). Draw the diag
	 * on trials, reverts, and halts — but not on a plain healthy boot, so normal
	 * boots aren't slowed by the ~2-4 s ePaper refresh. */
	int other = choice ^ 1;
	bool revert = !choice_trial && have && rec.slot[other].valid &&
		      !rec.slot[other].confirmed;

	if (choice_trial || revert) {
		static char reason[24];
		struct bl_diag d;

		if (choice_trial) {
			strcpy(reason, "TRIAL BOOT");
		} else {
			strcpy(reason, "REVERT TO ");
			size_t l = strlen(reason);

			reason[l] = (char)('A' + choice);
			reason[l + 1] = '\0';
		}
		build_diag(&d, &rec, have, choice, choice_trial, reason);
		/* Before arm_watchdog(): the refresh must not eat the trial's budget. */
		bl_diag_show(&d);
	}

	if (choice_trial) {
		/* Count this trial *before* jumping so a reset-without-confirm reverts. */
		bl_state_inc_attempts(choice);
		arm_watchdog();
	}

	bl_blink(3);   /* B3: decision made, about to chain-load the app slot */
	boot_slot(choice);
}
