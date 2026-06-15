/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>

#include "GUI.h"
#include "MeshNode.h"
#include "LEDStrip.h"

// How long the LEDs run after boot before the device deep-sleeps.
static constexpr uint32_t kAwakeMs = 5000;

static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

static struct gpio_callback btn_cb_data;

// Static globals keep the 5808-byte image buffer in .bss, not the main stack.
static GUI      s_gui;
static MeshNode s_mesh;
static LEDStrip s_leds;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	printk("User button pressed\n");
}

int main(void)
{
	printk("Starting vibamix\n");

	// Bring the LED strip up first so the boot animation is independent of the
	// ePaper/mesh. A strip failure is non-fatal — we still deep-sleep below.
	const bool leds_ok = (s_leds.init() == 0);

	s_gui.init();
	s_gui.show_hello_world();
	s_gui.sleep();

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

	// Bring up the mesh node (BT + mesh stack, self-provision, GATT proxy).
	// Non-fatal: even if it fails we still run the LED animation and deep-sleep.
	s_mesh.init(&s_gui, &s_leds);

	// Apply any persisted identity/color (switches the strip to a solid color
	// if one was saved; otherwise it shows the scrolling wheel + blob).
	s_mesh.apply_persisted_config();

	// Run the LEDs for kAwakeMs, then power everything down.
	if (leds_ok)
	{
		s_leds.play_for(kAwakeMs);
		s_leds.off();          // blank the chain + cut the LED power gate
	}
	else
	{
		k_sleep(K_MSEC(kAwakeMs));
	}

	// Enter deep sleep (System OFF). The ePaper is bistable and keeps its image.
	// Wake by reset, or by pressing the user button (armed as a wake source).
	printk("Entering deep sleep\n");
	if (btn_ready)
	{
		gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_LEVEL_ACTIVE);
	}
	sys_poweroff();

	return 0;
}
