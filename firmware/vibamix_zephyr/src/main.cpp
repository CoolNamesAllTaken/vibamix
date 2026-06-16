/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>

#include "BLERadio.h"
#include "GUI.h"
#include "LEDStrip.h"
#include "ambient_light_sensor.h"

static const struct gpio_dt_spec led_enable =
    GPIO_DT_SPEC_GET(DT_NODELABEL(led_enable), gpios);
static const struct gpio_dt_spec user_led =
    GPIO_DT_SPEC_GET(DT_ALIAS(user_led), gpios);
static const struct gpio_dt_spec user_btn =
    GPIO_DT_SPEC_GET(DT_ALIAS(user_button), gpios);

// Static globals keep the 5808-byte image buffer in .bss, not the main stack.
static GUI s_gui;
static BLERadio s_ble;
static LEDStrip s_leds;

static void led_boot_sequence(void) {
  static const struct {
    uint8_t r, g, b;
  } k_colors[] = {
      {255, 0, 0},     // red
      {0, 255, 0},     // green
      {0, 0, 255},     // blue
      {255, 255, 255}, // white
  };
  for (size_t i = 0; i < ARRAY_SIZE(k_colors); i++) {
    s_leds.set(k_colors[i].r, k_colors[i].g, k_colors[i].b);
    k_sleep(K_MSEC(500));
  }
  //   s_leds.off();
}

static void update_display(void) {
  struct als_reading r = ambient_light_sensor_get();
  s_gui.wake();
  s_gui.show_als_readings(r.lux, r.gain, r.ch0_raw, r.ch1_raw, r.valid, r.diag);
  s_gui.sleep();
}

int main(void) {
  printk("Starting vibamix\n");

  // Power on LED strip + ALS sensor (shared enable, active-low gate).
  gpio_pin_configure_dt(&led_enable, GPIO_OUTPUT_ACTIVE);

  // Start ALS early so the sensor has the full 2-second LED animation to warm
  // up and produce its first reading before we update the display.
  s_leds.init();
  ambient_light_sensor_start();
  led_boot_sequence(); // 4 × 500 ms = 2 s; ALS needs ~610 ms for first sample

  // Cut LED/ALS power — ePaper is bistable and holds the image without power.
  gpio_pin_configure_dt(&led_enable, GPIO_OUTPUT_INACTIVE);

  s_gui.init();
  update_display();

  if (gpio_is_ready_dt(&user_led)) {
    gpio_pin_configure_dt(&user_led, GPIO_OUTPUT_INACTIVE);
  }

  s_ble.init(&user_led);
  s_ble.start_advertising();

  // Arm the button as a level-active wake source, then enter System OFF.
  // The nRF54L15 will restart from main() when the button is pressed.
  if (gpio_is_ready_dt(&user_btn)) {
    gpio_pin_configure_dt(&user_btn, GPIO_INPUT);
    // Wait for any held press to release so we don't immediately re-wake.
    while (gpio_pin_get_dt(&user_btn) == 1) {
      k_sleep(K_MSEC(10));
    }
    gpio_pin_interrupt_configure_dt(&user_btn, GPIO_INT_LEVEL_ACTIVE);
  }

  printk("Entering deep sleep\n");
  sys_poweroff();

  return 0;
}
