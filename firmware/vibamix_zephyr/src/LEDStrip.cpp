#include "LEDStrip.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <math.h>

// ~25% brightness cap — easy on the eyes and lower current across 4 LEDs.
static constexpr uint8_t kBrightness = 64;

// Rainbow pattern: hue degrees rotated per frame.
static constexpr uint16_t kRotationStep = 4;

// Wheel pattern tuning.
static constexpr uint16_t kHueStep    = 3;     // hue deg scrolled per frame (~4.8 s/cycle)
static constexpr uint16_t kHueSpread  = 60;    // hue deg between adjacent LEDs
static constexpr uint32_t kBlobPeriod = 100;   // frames for one L->R->L bounce (~4 s)
static constexpr float    kBlobWidth  = 2.0f;  // blob falloff radius, in LED units
static constexpr float    kBlobFloor  = 0.10f; // faint floor so the wheel stays visible

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

void LEDStrip::set_pattern(LedPattern pattern)
{
    m_pattern = pattern;
}

void LEDStrip::set_color(uint8_t r, uint8_t g, uint8_t b)
{
    // Scale to the same brightness cap the animations use (~25%) to keep current
    // and glare down across the 4-LED chain.
    m_color.r = (uint16_t)r * kBrightness / 255;
    m_color.g = (uint16_t)g * kBrightness / 255;
    m_color.b = (uint16_t)b * kBrightness / 255;
    m_pattern = LedPattern::Solid;
}

void LEDStrip::render()
{
    switch (m_pattern) {
    case LedPattern::Solid:   render_solid();   break;
    case LedPattern::Rainbow: render_rainbow(); break;
    case LedPattern::Wheel:   render_wheel();   break;
    case LedPattern::Off:
    default:                  render_off();     break;
    }
    m_tick++;
}

void LEDStrip::play_for(uint32_t duration_ms)
{
    const int64_t deadline = k_uptime_get() + duration_ms;
    while (k_uptime_get() < deadline) {
        render();
        k_sleep(K_MSEC(kFrameMs));
    }
}

void LEDStrip::render_off()
{
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        m_pixels[i] = led_rgb{};
    }
    commit();
}

void LEDStrip::render_solid()
{
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        m_pixels[i] = m_color;
    }
    commit();
}

void LEDStrip::render_rainbow()
{
    const uint16_t phase = (uint16_t)((m_tick * kRotationStep) % 360);
    constexpr uint16_t spread = 360 / STRIP_NUM_PIXELS;
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint16_t hue = (phase + i * spread) % 360;
        m_pixels[i] = hsv_to_rgb(hue, kBrightness);
    }
    commit();
}

void LEDStrip::render_wheel()
{
    // Continuously scrolling RGB wheel across the strip.
    const uint16_t hue_base = (uint16_t)((m_tick * kHueStep) % 360);

    // Soft blob whose center bounces left<->right as a triangle wave: the
    // position sweeps 0 -> (N-1) -> 0 once per kBlobPeriod frames.
    const uint32_t c    = m_tick % kBlobPeriod;
    const uint32_t half = kBlobPeriod / 2;
    const float u = (c < half) ? (float)c / (float)half
                               : (float)(kBlobPeriod - c) / (float)half;   // 0->1->0
    const float blob_pos = u * (float)(STRIP_NUM_PIXELS - 1);

    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        const uint16_t hue = (uint16_t)((hue_base + i * kHueSpread) % 360);

        // Linear falloff from the blob center, with a faint floor so the
        // scrolling wheel stays visible where the blob isn't.
        float factor = 1.0f - fabsf((float)i - blob_pos) / kBlobWidth;
        if (factor < kBlobFloor) {
            factor = kBlobFloor;
        }
        const uint8_t val = (uint8_t)(kBrightness * factor);
        m_pixels[i] = hsv_to_rgb(hue, val);
    }
    commit();
}

void LEDStrip::commit()
{
    led_strip_update_rgb(s_strip, m_pixels, STRIP_NUM_PIXELS);
}
