/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <cmsis_core.h>
#include <hal/nrf_wdt.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>

#include "ConfigMode.h"
#include "GUI.h"
#include "MeshNode.h"
#include "LEDStrip.h"
#include "ambient_light_sensor.h"
#include "app_config.h"
#include "event_status.h"
#include "qr_screen.h"
#include "ota.h"

// How long the badge stays awake after a normal boot before deep-sleeping. The
// identity frame's bottom countdown bar counts down this window; a button press
// during it jumps straight into config mode. Each event heartbeat (when an event
// mesh is connected) resets the deadline to now + this window.
static constexpr uint32_t kAwakeMs = 60 * 1000;
static constexpr int      kAwakeTotalSec = kAwakeMs / 1000;

// Full refresh every N countdown-bar repaints to clear partial-refresh ghosting.
static constexpr int kBarFullRefreshEvery = 30;

// Ignore button edges closer together than this (contact debounce).
static constexpr uint32_t kBtnDebounceMs = 80;

// Hold the button this long (continuously) to force the badge into deep sleep from
// any awake state; polled at this cadence by s_longpress_timer.
static constexpr uint32_t kForceSleepHoldMs = 5000;
static constexpr uint32_t kLongPressPollMs  = 100;

// When a debugger has enabled halting debug, a true System OFF can't be cleanly
// reset/halted over SWD, making "reset device" slow/unpredictable. Set this to 1
// to keep the badge awake (fake sleep) under a debugger instead of powering off.
// Default 0: always enter real System OFF.
#ifndef VIBAMIX_FAKE_SLEEP_UNDER_DEBUGGER
#define VIBAMIX_FAKE_SLEEP_UNDER_DEBUGGER 0
#endif

static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

// Antenna RF switch (U10, FM8625H): rf_sw_pwr powers the switch (VDD), rf_sw_ctl
// selects the port. HIGH -> RF2 -> ANT2 (external antenna). See board DTS.
static const struct gpio_dt_spec rf_sw_pwr = GPIO_DT_SPEC_GET(DT_NODELABEL(rf_sw_pwr), gpios);
static const struct gpio_dt_spec rf_sw_ctl = GPIO_DT_SPEC_GET(DT_NODELABEL(rf_sw_ctl), gpios);

static struct gpio_callback btn_cb_data;

// Set (debounced) by the button ISR; consumed by the awake-window loop below.
static volatile bool s_btn_event;

// Static globals keep the 5808-byte image buffer in .bss, not the main stack.
static GUI       s_gui;
static MeshNode  s_mesh;
static LEDStrip  s_leds;
static ConfigMode s_config;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	static uint32_t last_ms;
	uint32_t now = k_uptime_get_32();

	if (now - last_ms < kBtnDebounceMs) {
		return;   // bounce
	}
	last_ms = now;

	printk("User button pressed\n");
	s_btn_event = true;        // wake/enter signal for the awake window
	config_mode_on_button();   // exit signal if already in config mode
}

// Set once the user has held the button for kForceSleepHoldMs; the awake-window and
// config-mode loops poll this and bail out to enter_deep_sleep(). Reset on the next
// boot (a System OFF wake is a full reboot).
static atomic_t s_force_sleep = ATOMIC_INIT(0);

extern "C" bool app_force_sleep_requested(void)
{
	return atomic_get(&s_force_sleep) != 0;
}

// Set by the SET_CONFIG_MODE mesh opcode (on the BT thread) to request entering
// config mode; consumed by run_awake_window() on the main thread, so it only takes
// effect while the badge is awake. Mirrors the s_btn_event "enter config" signal.
static atomic_t s_enter_config = ATOMIC_INIT(0);

extern "C" void app_request_config_mode(void)
{
	atomic_set(&s_enter_config, 1);
}

// Poll the button level; once it has been held continuously for kForceSleepHoldMs,
// latch the force-sleep flag, light the user LED as a "hold registered" cue (it's
// turned off again by gpio_lowpower_for_sleep() when the badge actually powers down),
// and nudge config mode to exit. ISR context: only register reads/writes + an atomic.
static void longpress_poll_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	static uint32_t held_ms;

	if (gpio_is_ready_dt(&user_btn) && gpio_pin_get_dt(&user_btn) == 1) {
		held_ms += kLongPressPollMs;
		if (held_ms >= kForceSleepHoldMs && !atomic_get(&s_force_sleep)) {
			printk("Button held %u ms - forcing deep sleep\n", held_ms);
			atomic_set(&s_force_sleep, 1);
			if (gpio_is_ready_dt(&user_led)) {
				gpio_pin_set_dt(&user_led, 1);   // active-low -> LED on
			}
			config_mode_on_button();             // break ConfigMode's loop if active
		}
	} else {
		held_ms = 0;   // released (or a bounce) -> restart the hold count
	}
}
static K_TIMER_DEFINE(s_longpress_timer, longpress_poll_fn, NULL);

// Block until the button is released (logical inactive) or a timeout, so we
// never arm an edge/level interrupt — or enter System OFF — with it still held.
static void wait_button_release(void)
{
	for (int i = 0; i < 500 && gpio_pin_get_dt(&user_btn) == 1; i++) {
		k_sleep(K_MSEC(10));
	}
}

// True if we woke from System OFF via the armed user button, vs a cold boot
// (battery insert = POR, debugger/reset = PIN). On nRF54L the System-OFF GPIO
// wake is reported as RESET_LOW_POWER_WAKE.
static bool woke_from_button(void)
{
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) != 0) {
		return false;
	}
	hwinfo_clear_reset_cause();   // RESETREAS is sticky; clear so next boot is clean
	return (cause & RESET_LOW_POWER_WAKE) != 0;
}

#if VIBAMIX_FAKE_SLEEP_UNDER_DEBUGGER
// True if an SWD debugger has enabled halting debug (C_DEBUGEN). We skip System
// OFF in that case: a powered-down core can't be cleanly reset/halted, which
// makes "reset device" in the debugger slow and unpredictable.
static inline bool debugger_attached(void)
{
	return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0;
}
#endif

// The bootloader arms wdt31 (only) when chain-loading an unconfirmed trial image
// and leaves it running across the jump. We feed it directly via the HAL (the
// Zephyr driver can't feed a channel it didn't install) once a healthy boot is
// confirmed; before that point the dog runs unfed, so an image that hangs during
// early bring-up is reset and rolled back by the bootloader.
static NRF_WDT_Type *const s_wdt = (NRF_WDT_Type *)DT_REG_ADDR(DT_NODELABEL(wdt31));

static void wdt_feed_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	nrf_wdt_reload_request_set(s_wdt, NRF_WDT_RR0);
}
static K_TIMER_DEFINE(s_wdt_timer, wdt_feed_fn, NULL);

// Start keeping the watchdog fed for the rest of this awake session. It pauses
// itself in System OFF (WDT_OPT_PAUSE_IN_SLEEP), so deep sleep is safe.
static void wdt_keepalive_start(void)
{
	nrf_wdt_reload_request_set(s_wdt, NRF_WDT_RR0);
	k_timer_start(&s_wdt_timer, K_SECONDS(2), K_SECONDS(2));
}

// Float app-owned, non-wake GPIOs to high impedance before System OFF. nRF54L15 does not
// reliably hold a *driven* output through System OFF, so rather than fight that we
// disconnect these pins and let the board's pull resistors settle each net into its safe
// (off) state — e.g. the external pull-up on the WS2812 power-bus PMOS gate (P1.07, cut by
// LEDStrip::off()) keeps the strip's VDD disabled. High-Z also draws no current itself.
// main() re-drives all of these on the next wake.
//
// The user button (P0.00) is NOT touched here — enter_deep_sleep() arms it as the
// wake source afterwards, so this must run first. The ePaper control pins
// (CS/DC/RST/BUSY) are deliberately left as the EPD driver left them: the panel is
// bistable and already in deep sleep, and GUI::init() reconfigures them at boot;
// disturbing them risks the retained image.
static void gpio_lowpower_for_sleep(void)
{
	// Antenna RF switch (U10) control + VDD, and the user status LED: disconnect so the
	// board pulls hold them off. (The WS2812 power-bus gate P1.07 is floated by
	// LEDStrip::off(), which ran first in enter_deep_sleep().)
	if (gpio_is_ready_dt(&rf_sw_ctl))
	{
		gpio_pin_configure_dt(&rf_sw_ctl, GPIO_DISCONNECTED);
	}
	if (gpio_is_ready_dt(&rf_sw_pwr))
	{
		gpio_pin_configure_dt(&rf_sw_pwr, GPIO_DISCONNECTED);
	}
	if (gpio_is_ready_dt(&user_led))
	{
		gpio_pin_configure_dt(&user_led, GPIO_DISCONNECTED);
	}
}

// The single deep-sleep (System OFF) path: blank the LEDs, rest the panel on the
// "asleep" identity frame, power down the non-wake GPIOs, arm the user button as the
// wake source, then power off. Every non-debugger exit funnels through here so this
// cleanup can never be skipped.
static void enter_deep_sleep(void)
{
	printk("Entering deep sleep\n");

	// Stop the long-press poll timer so it isn't reading the button between the
	// System-OFF wake arming and sys_poweroff() below.
	k_timer_stop(&s_longpress_timer);

	// 1. LEDs off, unconditionally — cuts the WS2812 power gate even if the strip
	//    failed to init (that path leaves the gate driven active otherwise).
	s_leds.off();

	// 2. Rest the bistable panel on the identity frame with the asleep indicator.
	s_mesh.redraw_identity(/*sleeping=*/true);

	// 3. Power down the antenna switch + other non-wake GPIOs.
	gpio_lowpower_for_sleep();

	// 4. Arm the user button as the System OFF wake source — last, so the GPIO
	//    reconfig above can't disturb its SENSE. Wait for release first so a still-
	//    held button doesn't immediately re-wake.
	if (gpio_is_ready_dt(&user_btn))
	{
		wait_button_release();
		gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_LEVEL_ACTIVE);
	}

	// 5. Deep sleep. The ePaper keeps its image with no power. System OFF powers the
	//    radio + peripherals down in hardware (no bt_disable() needed). Wake by a
	//    button press (RESET_LOW_POWER_WAKE) or a reset — a full reboot that re-runs
	//    main()/MeshNode::init() to bring BT/mesh back up.
	sys_poweroff();
}

// Identity-frame awake window: rest on the identity frame and tick a thin bottom
// countdown bar down to deep sleep. Each 1 Hz event-mesh heartbeat resets the
// deadline (and switches the bar's left text to the event name), so the badge stays
// awake for the duration of an event ("mesh mode"). Returns true if the user button
// was pressed (caller re-enters config mode), false on timeout (caller deep-sleeps).
// The caller must have established the identity frame as the panel base first (via
// apply_persisted_config / redraw_identity).
static bool run_awake_window(void)
{
	// A 5 s hold already latched force-sleep (e.g. while exiting config mode) — go
	// straight to deep sleep without opening the window or waiting on the held button.
	if (atomic_get(&s_force_sleep)) {
		return false;
	}

	// Drop a still-held / already-latched press (e.g. the one that just exited config
	// mode) so the window doesn't close the instant it opens.
	wait_button_release();
	s_btn_event = false;
	atomic_clear(&s_enter_config);   // drop any stale mesh enter-config request

	const struct app_config *cfg = app_config_get();
	const char *id_name  = cfg->name;
	const char *id_table = cfg->has_attendee ? cfg->attendee_id : "";

	int64_t now = k_uptime_get();
	int64_t deadline = now + kAwakeMs;
	int     bar_tick = 0;
	int     last_remaining = -1;
	int64_t last_bar_ms = 0;
	bool    force_rebase = false;   // a deferred render replaced the panel frame

	// The countdown bar overlays whatever frame is on the panel — identity, or a text
	// or image frame pushed mid-window — so every non-config frame auto-sleeps on the
	// deadline unless event heartbeats keep it awake. A full-screen 4-gray frame can't
	// be partial-refreshed whole-frame (that would flatten the gray), so over a gray
	// frame the bar holds static (no live tick) and the window runs purely on the
	// deadline; 1-bit frames tick the opaque bottom banner via a region refresh.
	// Re-read after each content render below, since the frame (and its gray-ness) changes.
	bool gray = s_mesh.current_frame_is_gray();

	// The name/ID banner is drawn only on the identity frame; text/image frames get the
	// bare countdown fill so their content isn't covered. Re-read after a content render.
	bool on_identity = s_mesh.current_frame_is_identity();

	// Take ownership of the panel: while this loop runs, mesh content commands
	// (arriving on the BT thread) must NOT render the ePaper themselves — that races
	// this thread's countdown refresh and wedges the panel, after which the button is
	// never serviced. Defer them to consume_pending_render() below. Set before the
	// first wake() so there's no window where the main loop drives the panel undeferred.
	s_mesh.set_defer_renders(true);

	// Wake the panel for the partial-refresh countdown ticks (the identity redraw
	// left it asleep after its full-refresh baseline draw).
	s_gui.wake();

	while ((now = k_uptime_get()) < deadline && !s_btn_event &&
	       !atomic_get(&s_force_sleep) && !atomic_get(&s_enter_config))
	{
		if (event_status_consume_heartbeat())
		{
			deadline = now + kAwakeMs;
		}

		// Render any mesh content staged on the BT thread, on THIS thread, so the
		// panel stays single-threaded. The render left a new frame on the panel and
		// the panel asleep, so re-wake for the partial ticks and re-base the bar
		// (set_base_map) on the next tick rather than partial-refreshing over a stale base.
		if (s_mesh.consume_pending_render())
		{
			s_gui.wake();
			gray = s_mesh.current_frame_is_gray();   // the new frame may be 1-bit or 4-gray
			on_identity = s_mesh.current_frame_is_identity();
			force_rebase = true;
			last_remaining = -1;
			last_bar_ms = 0;
		}

		// (LEDs animate on their own thread; see LEDStrip::init.)

		// A full-screen 4-gray frame is static — a 1-bit partial can't run over it, so
		// there's no live bar to tick (heartbeats/button/timeout below still drive the
		// window). A 1-bit frame repaints its countdown bar ~1 Hz (flash-free partial
		// refresh) — with the name/ID banner only on the identity frame (on_identity).
		if (!gray && now - last_bar_ms >= 1000)
		{
			last_bar_ms = now;
			int remaining = (int)((deadline - now + 999) / 1000);
			if (remaining != last_remaining)
			{
				last_remaining = remaining;
				identity_countdown_overlay(
					s_gui, id_name, id_table, on_identity,
					remaining, kAwakeTotalSec);
				if (force_rebase)
				{
					force_rebase = false;
					s_gui.set_base_map();
				}
				else if (++bar_tick % kBarFullRefreshEvery == 0)
				{
					s_gui.set_base_map();
				}
				else
				{
					s_gui.refresh_partial();
				}
			}
		}

		k_sleep(K_MSEC(LEDStrip::kFrameMs));
	}

	// Leaving the window (button or timeout): stop deferring and drop any work staged
	// after the last drain, so a stale mesh frame can't render once config mode or
	// deep-sleep owns the panel.
	s_mesh.set_defer_renders(false);

	// A forced sleep (5 s hold) must go to deep sleep, not be treated as a press that
	// re-enters config mode.
	if (atomic_get(&s_force_sleep)) {
		return false;
	}
	// A SET_CONFIG_MODE(on) mesh opcode re-enters config mode, same as a button press.
	if (atomic_get(&s_enter_config)) {
		atomic_clear(&s_enter_config);
		return true;
	}
	return s_btn_event;
}

int main(void)
{
	printk("Starting vibamix\n");

	// Select the external antenna before any radio activity: power the RF switch
	// (U10) and drive RF_SW_CTL HIGH (-> RF2 -> ANT2, the external antenna).
	// Reconfigured on every wake since System OFF resets GPIO state.
	if (gpio_is_ready_dt(&rf_sw_pwr))
	{
		gpio_pin_configure_dt(&rf_sw_pwr, GPIO_OUTPUT_ACTIVE);
	}
	if (gpio_is_ready_dt(&rf_sw_ctl))
	{
		gpio_pin_configure_dt(&rf_sw_ctl, GPIO_OUTPUT_ACTIVE);
	}

	// Always wake into the identity frame, whether from a cold boot or a System OFF
	// button wake. A button press during the awake window then promotes into config
	// mode (run_awake_window() returns true). Still clear the sticky reset cause so
	// the next boot reads clean.
	(void)woke_from_button();
	bool config_mode = false;

	// Bring the LED strip up first so the boot animation is independent of the
	// ePaper/mesh. A strip failure is non-fatal — we still deep-sleep below.
	const bool leds_ok = (s_leds.init() == 0);

	// Start the ambient-light-sensor monitor AFTER the LED strip powers its rail:
	// the LTR-329 shares that gate, so the monitor must not poll I2C before it's up
	// (and stays quiet whenever the rail is off). It reads continuously while the
	// LEDs are lit and pulse-samples in config mode (see ambient_light_sensor.cpp).
	ambient_light_sensor_start(&s_leds);

	s_gui.init();

	if (gpio_is_ready_dt(&user_led))
	{
		gpio_pin_configure_dt(&user_led, GPIO_OUTPUT_INACTIVE);
	}

	const bool btn_ready = gpio_is_ready_dt(&user_btn);
	if (btn_ready)
	{
		gpio_pin_configure_dt(&user_btn, GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_EDGE_TO_ACTIVE);
		gpio_init_callback(&btn_cb_data, button_pressed, BIT(user_btn.pin));
		gpio_add_callback(user_btn.port, &btn_cb_data);

		// Watch for a 5 s hold to force deep sleep from any awake state.
		k_timer_start(&s_longpress_timer, K_MSEC(kLongPressPollMs),
			      K_MSEC(kLongPressPollMs));
	}

	// Bring up the mesh node (BT + mesh stack, self-provision, GATT proxy, and
	// the per-device name used for Web Bluetooth discovery). Non-fatal.
	s_mesh.init(&s_gui, &s_leds);

	// Reaching here means BLE/mesh came up, so a freshly-OTA'd image is healthy:
	// confirm THIS slot in bl_state so the bootloader keeps it (idempotent no-op
	// on a normal boot). An image that crashes/hangs before this point fails to
	// confirm — the bootloader's attempt counter + watchdog then roll it back.
	ota_confirm_on_boot();

	// Now that we've confirmed, keep the watchdog fed (it was running unfed up to
	// this point to catch a hung bring-up). No-op if the bootloader didn't arm it.
	wdt_keepalive_start();

	if (!config_mode)
	{
		// Normal boot: rest on the identity frame (the home screen; "Hello World"
		// only on first boot with no name set). The config<->countdown loop below
		// then runs the awake window on top of it.
		s_mesh.apply_persisted_config();   // renders + set_base_map() + panel sleep
	}

	// Config <-> identity-countdown loop. Pressing the button in config mode drops
	// back to the identity frame and counts down to sleep (staying awake while an
	// event mesh heartbeats); pressing it during the countdown promotes back into
	// config mode. Only a countdown that times out with no event reaches deep sleep.
	for (;;)
	{
		if (config_mode)
		{
			// Woken by button (or promoted from the countdown): show the ID + QR and
			// stay awake so a phone can connect over GATT and upload a badge.
			printk("Entering config mode\n");
			if (leds_ok)
			{
				s_leds.off();   // keep it dark during config; battery-friendly
			}
			// LEDs (and the shared light-sensor rail) are dark now — have the ALS
			// briefly pulse the rail for config-screen lux instead of polling a dead bus.
			ambient_light_sensor_set_config_mode(true);
			s_config.run(&s_gui, &s_mesh, &user_btn);
			// Stop pulsing and wait for the rail to be released before we re-take it.
			ambient_light_sensor_set_config_mode(false);
			config_mode = false;

			// A 5 s hold ended config mode -> go straight to deep sleep, skipping the
			// identity restore + countdown.
			if (atomic_get(&s_force_sleep)) {
				break;
			}

			// Config left the QR / a pushed image on the panel — restore the identity
			// frame (and its LED) so the countdown window rests on it.
			if (leds_ok)
			{
				s_leds.power_on();
			}
			s_mesh.apply_persisted_config();
		}

		// Identity-frame countdown to sleep (heartbeat-aware "mesh mode").
		if (run_awake_window())
		{
			config_mode = true;   // button pressed during the countdown -> config
			continue;
		}

#if VIBAMIX_FAKE_SLEEP_UNDER_DEBUGGER
		// Timed out with no event. Under a debugger we can't cleanly enter System OFF
		// (a powered-down core can't be reset/halted), so wait for the next button
		// press to re-enter config instead of sleeping.
		if (debugger_attached())
		{
			printk("Debugger attached - skipping System OFF; resting asleep, press button for config mode\n");
			// Can't truly System OFF under a debugger, so simulate sleep: blank the LED
			// strip and rest the bistable panel on the asleep identity frame. Otherwise the
			// awake-window animation keeps running and it never looks asleep.
			s_leds.off();
			s_mesh.redraw_identity(/*sleeping=*/true);
			// Mirror real sleep: go radio-quiet while idling so this simulated sleep
			// matches the production power profile (no scanner/relay/beacon spikes).
			s_mesh.radio_suspend();
			wait_button_release();
			s_btn_event = false;
			while (!s_btn_event)
			{
				k_sleep(K_MSEC(50));
			}
			// Exit from (fake) deep sleep: bring BT/mesh back up before config mode,
			// which needs the BLE advertiser + GATT service.
			s_mesh.radio_resume();
			config_mode = true;
			continue;
		}
#endif

		break;
	}

	// Centralized deep-sleep: LEDs off, asleep frame, GPIO power-down, button armed
	// as the wake source, then System OFF.
	enter_deep_sleep();

	return 0;
}
