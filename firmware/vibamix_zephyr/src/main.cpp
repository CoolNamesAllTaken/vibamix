/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "GUI.h"
#include "BLERadio.h"
#include "LEDStrip.h"

static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn = GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

static struct gpio_callback btn_cb_data;

// Static globals keep the 5808-byte image buffer in .bss, not the main stack.
static GUI      s_gui;
static BLERadio s_ble;
static LEDStrip s_leds;

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	printk("User button pressed\n");
}

int main(void)
{
	printk("Starting vibamix\n");

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

	if (s_ble.init(&user_led) != 0)
	{
		return 0;
	}
	s_ble.start_advertising();

	if (s_leds.init() != 0)
	{
		// No LED strip — keep the rest of the firmware running.
		for (;;)
		{
			k_sleep(K_FOREVER);
		}
	}

	uint32_t phase = 0;
	for (;;)
	{
		s_leds.render(phase);
		phase = (phase + 4) % 360;    // rotation speed
		k_sleep(K_MSEC(40));          // ~25 fps
	}
}
