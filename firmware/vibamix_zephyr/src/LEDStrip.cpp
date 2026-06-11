#include "LEDStrip.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <errno.h>

// ~25% brightness cap — easy on the eyes and lower current across 4 LEDs.
static constexpr uint8_t kBrightness = 64;

static const struct device *const s_strip = DEVICE_DT_GET(STRIP_NODE);

// WS2812 VDD power gate (P1.07, active-low PMOS); driving it active powers the chain.
static const struct gpio_dt_spec s_power = GPIO_DT_SPEC_GET(DT_NODELABEL(led_enable), gpios);

// Full-saturation HSV->RGB. hue in [0,360), val is the peak channel value.
static struct led_rgb hsv_to_rgb(uint16_t hue, uint8_t val)
{
    const uint8_t region = hue / 60;          // 0..5
    const uint8_t rem    = (hue % 60) * 255 / 60;  // position within region, 0..255
    const uint8_t p = 0;
    const uint8_t q = (uint16_t)val * (255 - rem) / 255;
    const uint8_t t = (uint16_t)val * rem / 255;

    struct led_rgb c{};
    switch (region) {
    case 0:  c.r = val; c.g = t;   c.b = p;   break;
    case 1:  c.r = q;   c.g = val; c.b = p;   break;
    case 2:  c.r = p;   c.g = val; c.b = t;   break;
    case 3:  c.r = p;   c.g = q;   c.b = val; break;
    case 4:  c.r = t;   c.g = p;   c.b = val; break;
    default: c.r = val; c.g = p;   c.b = q;   break;
    }
    return c;
}

int LEDStrip::init()
{
    if (!gpio_is_ready_dt(&s_power)) {
        printk("LED power gate not ready\n");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&s_power, GPIO_OUTPUT_ACTIVE);

    if (!device_is_ready(s_strip)) {
        printk("LED strip device not ready\n");
        return -ENODEV;
    }
    printk("LED strip ready (%d pixels)\n", STRIP_NUM_PIXELS);
    return 0;
}

void LEDStrip::render(uint32_t phase)
{
    constexpr uint16_t spread = 360 / STRIP_NUM_PIXELS;
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint16_t hue = (phase + i * spread) % 360;
        m_pixels[i] = hsv_to_rgb(hue, kBrightness);
    }
    led_strip_update_rgb(s_strip, m_pixels, STRIP_NUM_PIXELS);
}
