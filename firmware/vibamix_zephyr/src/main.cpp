/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "GUI.h"
#include "MeshNode.h"
#include "LEDStrip.h"

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

	// Bring the LED strip up FIRST, before the GUI/mesh, so the startup rainbow
	// is a reliable "I'm alive" signal that doesn't depend on the ePaper refresh
	// or the BT/mesh stack succeeding. A strip failure is non-fatal.
	const bool leds_ok = (s_leds.init() == 0);
	if (leds_ok)
	{
		// Brief startup animation as an early "alive" signal before GUI/mesh;
		// the main loop below carries the same scrolling wheel + blob on
		// continuously until a mesh command sets a solid color.
		s_leds.play_for(3000);
	}

	s_gui.init();
	s_gui.show_hello_world();
	s_gui.sleep();

	if (!gpio_is_ready_dt(&user_led))
	{
		printk("LED device not ready\n");
		return 0;
	}
	gpio_pin_configure_dt(&user_led, GPIO_OUTPUT_INACTIVE);

	if (!gpio_is_ready_dt(&user_btn))
	{
		printk("Button device not ready\n");
		return 0;
	}
	gpio_pin_configure_dt(&user_btn, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_EDGE_TO_ACTIVE);
	gpio_init_callback(&btn_cb_data, button_pressed, BIT(user_btn.pin));
	gpio_add_callback(user_btn.port, &btn_cb_data);

	// Bring up the mesh node (BT + mesh stack, self-provision, GATT proxy).
	if (s_mesh.init(&s_gui, &s_leds) != 0)
	{
		return 0;
	}

	// Apply any persisted identity/color now that LEDs are up (switches the
	// strip to a solid color if one was saved; otherwise it stays Off).
	s_mesh.apply_persisted_config();

	if (!leds_ok)
	{
		// No LED strip — keep the rest of the firmware running.
		for (;;)
		{
			k_sleep(K_FOREVER);
		}
	}

	for (;;)
	{
		s_leds.render();
		k_sleep(K_MSEC(LEDStrip::kFrameMs));
	}
}
