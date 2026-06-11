#ifndef VIBAMIX_LED_STRIP_H
#define VIBAMIX_LED_STRIP_H

#include <zephyr/drivers/led_strip.h>
#include <zephyr/devicetree.h>
#include <stdint.h>

#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

class LEDStrip {
public:
    // Enables the strip's power gate and checks the device is ready.
    int init();

    // Renders a rainbow across the chain, rotated by `phase` (hue degrees).
    void render(uint32_t phase);

private:
    struct led_rgb m_pixels[STRIP_NUM_PIXELS];
};

#endif /* VIBAMIX_LED_STRIP_H */
