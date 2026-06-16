/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <cmsis_core.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>

#include "ConfigMode.h"
#include "GUI.h"
#include "MeshNode.h"
#include "LEDStrip.h"

// How long the LEDs run after a normal boot before the device deep-sleeps. A
// button press during this window jumps straight into config mode.
static constexpr uint32_t kAwakeMs = 5000;

// Ignore button edges closer together than this (contact debounce).
static constexpr uint32_t kBtnDebounceMs = 80;

static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

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

// True if an SWD debugger has enabled halting debug (C_DEBUGEN). We skip System
// OFF in that case: a powered-down core can't be cleanly reset/halted, which
// makes "reset device" in the debugger slow and unpredictable.
static inline bool debugger_attached(void)
{
	return (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) != 0;
}

int main(void)
{
	printk("Starting vibamix\n");

	bool config_mode = woke_from_button();

	// Bring the LED strip up first so the boot animation is independent of the
	// ePaper/mesh. A strip failure is non-fatal — we still deep-sleep below.
	const bool leds_ok = (s_leds.init() == 0);

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
	}

	// Bring up the mesh node (BT + mesh stack, self-provision, GATT proxy, and
	// the per-device name used for Web Bluetooth discovery). Non-fatal.
	s_mesh.init(&s_gui, &s_leds);

	if (!config_mode)
	{
		// Normal boot: draw the boot screen (custom image as-is, else identity,
		// else "Hello World"), then run an awake window. A button press during
		// the window promotes us straight into config mode.
		s_mesh.apply_persisted_config();

		int64_t deadline = k_uptime_get() + kAwakeMs;
		while (k_uptime_get() < deadline && !s_btn_event)
		{
			if (leds_ok)
			{
				s_leds.render();
			}
			k_sleep(K_MSEC(LEDStrip::kFrameMs));
		}
		if (leds_ok)
		{
			s_leds.off();   // blank the chain + cut the LED power gate
		}
		if (s_btn_event)
		{
			config_mode = true;
		}
	}

	if (config_mode)
	{
		// Woken by button (or promoted above): show the ID + QR and stay awake so
		// a phone can connect over the custom GATT service and upload a badge.
		printk("Entering config mode\n");
		if (leds_ok)
		{
			s_leds.off();   // keep it dark during config; battery-friendly
		}
		s_config.run(&s_gui, &s_mesh, &user_btn);
	}

	// Under a debugger, stay awake instead of entering System OFF: a powered-down
	// core can't be cleanly reset/halted, so "reset device" would be slow and
	// unpredictable. Standalone, the button enters config mode by waking from
	// System OFF; with no System OFF here, poll for the press ourselves so the
	// button still works (otherwise it's dead once we drop into the idle thread).
	if (debugger_attached())
	{
		printk("Debugger attached - staying awake (skipping System OFF); press button for config mode\n");
		for (;;)
		{
			wait_button_release();   // ignore a still-held entering/exit press
			s_btn_event = false;
			while (!s_btn_event)
			{
				k_sleep(K_MSEC(50));
			}
			if (leds_ok)
			{
				s_leds.off();
			}
			s_config.run(&s_gui, &s_mesh, &user_btn);
		}
	}

	// Enter deep sleep (System OFF). The ePaper is bistable and keeps its image.
	// Wait for the button to be released first so we don't sleep with the line
	// already active (which would immediately re-wake), then arm it as the wake
	// source. Wake by reset, or by pressing the user button.
	printk("Entering deep sleep\n");
	if (btn_ready)
	{
		wait_button_release();
		gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_LEVEL_ACTIVE);
	}
	sys_poweroff();

	return 0;
}
