#include "LEDStrip.h"

#include <zephyr/drivers/led_strip.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_strip, LOG_LEVEL_INF);

#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

static const struct device *s_dev = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb s_pixels[STRIP_NUM_PIXELS];

bool LEDStrip::init() {
    if (!device_is_ready(s_dev)) {
        LOG_ERR("LED strip device not ready");
        return false;
    }
    off();
    return true;
}

void LEDStrip::set(uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        s_pixels[i].r = r;
        s_pixels[i].g = g;
        s_pixels[i].b = b;
    }
    led_strip_update_rgb(s_dev, s_pixels, STRIP_NUM_PIXELS);
}

void LEDStrip::off() {
    set(0, 0, 0);
}
