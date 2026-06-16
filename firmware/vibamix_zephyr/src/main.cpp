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
#include "ambient_light_sensor.h"


static const struct gpio_dt_spec led_enable = GPIO_DT_SPEC_GET(DT_NODELABEL(led_enable), gpios);
static const struct gpio_dt_spec user_led   = GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn   = GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

static struct gpio_callback btn_cb_data;

// Static globals keep the 5808-byte image buffer in .bss, not the main stack.
static GUI      s_gui;
static BLERadio s_ble;
static LEDStrip s_leds;

K_SEM_DEFINE(s_btn_sem, 0, 1);

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&s_btn_sem);
}

static void led_boot_sequence(void)
{
	static const struct { uint8_t r, g, b; } k_colors[] = {
		{255,   0,   0},  // red
		{  0, 255,   0},  // green
		{  0,   0, 255},  // blue
		{255, 255, 255},  // white
	};
	for (size_t i = 0; i < ARRAY_SIZE(k_colors); i++) {
		s_leds.set(k_colors[i].r, k_colors[i].g, k_colors[i].b);
		k_sleep(K_MSEC(500));
	}
	s_leds.off();
}

static void update_display(void)
{
	struct als_reading r = ambient_light_sensor_get();
	s_gui.wake();
	s_gui.show_als_readings(r.lux, r.gain, r.ch0_raw, r.ch1_raw, r.valid, r.diag);
	s_gui.sleep();
}

int main(void)
{
	printk("Starting vibamix\n");

	gpio_pin_configure_dt(&led_enable, GPIO_OUTPUT_ACTIVE);

	s_leds.init();
	ambient_light_sensor_start();  // start early — 2s boot sequence gives ALS time to get first reading
	led_boot_sequence();

	s_gui.init();
	update_display();              // show ALS reading immediately on boot

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

	for (;;)
	{
		k_sem_take(&s_btn_sem, K_FOREVER);
		update_display();
	}
}
