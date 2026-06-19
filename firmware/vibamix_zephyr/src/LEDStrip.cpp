#include "LEDStrip.h"
#include "ambient_light_sensor.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <errno.h>
#include <math.h>

// ALS auto-brightness mapping. Lux is gain-corrected illuminance: below kLuxDim
// the room is dark -> floor brightness; at/above kLuxBright -> the cap; linear in
// between. Higher ambient light -> brighter LEDs (so they stay visible), darker
// room -> gentler glow. The applied brightness eases toward the target by
// kBrightSlewStep per ~40 ms frame, so it never flickers or steps abruptly.
static constexpr uint32_t kLuxDim        = 10;    // <=10 lux: dim room / night -> floor
static constexpr uint32_t kLuxBright     = 400;   // >=400 lux: bright indoor -> cap
static constexpr uint8_t  kBrightMin     = 12;    // gentle floor in the dark (~5%)
static constexpr uint8_t  kBrightSlewStep = 2;    // max brightness change per frame
static constexpr uint32_t kAlsPollFrames = 25;    // refresh target ~1 Hz (25 frames @ 40 ms)

// Map gain-corrected lux to a target applied brightness in [kBrightMin, kBrightnessCap].
static uint8_t lux_to_brightness(uint32_t lux)
{
    if (lux <= kLuxDim) {
        return kBrightMin;
    }
    if (lux >= kLuxBright) {
        return LEDStrip::kBrightnessCap;
    }
    const uint32_t span_lux = kLuxBright - kLuxDim;
    const uint32_t span_bri = LEDStrip::kBrightnessCap - kBrightMin;
    return (uint8_t)(kBrightMin + (lux - kLuxDim) * span_bri / span_lux);
}

// Rainbow pattern: hue degrees rotated per frame.
static constexpr uint16_t kRotationStep = 4;

// Wheel pattern tuning.
static constexpr uint16_t kHueStep    = 3;     // hue deg scrolled per frame (~4.8 s/cycle)
static constexpr uint16_t kHueSpread  = 60;    // hue deg between adjacent LEDs
static constexpr uint32_t kBlobPeriod = 100;   // frames for one L->R->L bounce (~4 s)
static constexpr float    kBlobWidth  = 2.0f;  // blob falloff radius, in LED units
static constexpr float    kBlobFloor  = 0.10f; // faint floor so the wheel stays visible

// Breathe: triangle-wave brightness period (frames). ~64*40ms ≈ 2.6 s.
static constexpr uint32_t kBreathePeriod = 64;
static constexpr uint8_t  kBreatheFloor  = 28;   // dimmest point of the pulse
// Comet: a fractional head sweeps the ring continuously; brightness falls off with
// distance behind the head (a short tail), so motion is smooth, not per-LED hops.
static constexpr float kCometFramesPerLoop = 60.0f;  // frames for one full sweep (~2.4 s)
static constexpr float kCometTail = 1.8f;            // tail length, in LED units
// Sparkle: per-frame decay (gentler = smoother fade), dim floor, ~1/N spawn chance.
static constexpr uint8_t  kSparkDecay = 10;
static constexpr uint8_t  kSparkFloor = 12;

// Render thread: draws the active pattern at kFrameMs independent of the main /
// ePaper loop, so a (blocking) panel refresh can't stall the animation.
K_THREAD_STACK_DEFINE(s_led_stack, 768);
static struct k_thread s_led_thread;

static const struct device *const s_strip = DEVICE_DT_GET(STRIP_NODE);

// Scale a color by lvl/255 (lvl folds in both the per-pattern level and the
// current applied brightness).
static struct led_rgb scale_rgb(struct led_rgb c, uint8_t lvl)
{
    struct led_rgb o{};
    o.r = (uint16_t)c.r * lvl / 255;
    o.g = (uint16_t)c.g * lvl / 255;
    o.b = (uint16_t)c.b * lvl / 255;
    return o;
}

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

    // Drive rendering from a dedicated, steady-cadence thread so the ePaper's
    // (blocking) refreshes on the main thread don't freeze the animation.
    m_powered = true;
    m_render_enabled = true;
    k_thread_create(&s_led_thread, s_led_stack, K_THREAD_STACK_SIZEOF(s_led_stack),
                    LEDStrip::thread_entry, this, NULL, NULL,
                    K_PRIO_PREEMPT(10), 0, K_NO_WAIT);
    k_thread_name_set(&s_led_thread, "led");

    printk("LED strip ready (%d pixels)\n", STRIP_NUM_PIXELS);
    return 0;
}

void LEDStrip::thread_entry(void *a, void *b, void *c)
{
    ARG_UNUSED(b);
    ARG_UNUSED(c);
    static_cast<LEDStrip *>(a)->render_loop();
}

void LEDStrip::render_loop()
{
    for (;;) {
        if (m_powered && m_render_enabled) {
            update_brightness();
            render();
        }
        k_sleep(K_MSEC(kFrameMs));
    }
}

void LEDStrip::update_brightness()
{
    // Refresh the target ~1 Hz from the latest ALS reading. On an invalid reading
    // (sensor absent / error / pre-first-sample) hold the previous target — which
    // defaults to the cap — so the strip never goes dark on a sensor fault.
    if (m_tick % kAlsPollFrames == 0) {
        struct als_reading r = ambient_light_sensor_get();
        if (r.valid) {
            m_bright_target = lux_to_brightness(r.lux);
        }
    }

    // Slew one step toward the target so brightness eases instead of jumping.
    if (m_brightness < m_bright_target) {
        const uint8_t d = m_bright_target - m_brightness;
        m_brightness += (d < kBrightSlewStep) ? d : kBrightSlewStep;
    } else if (m_brightness > m_bright_target) {
        const uint8_t d = m_brightness - m_bright_target;
        m_brightness -= (d < kBrightSlewStep) ? d : kBrightSlewStep;
    }
}

void LEDStrip::set_pattern(LedPattern pattern)
{
    m_pattern = pattern;
}

void LEDStrip::set_color(uint8_t r, uint8_t g, uint8_t b)
{
    set_anim(LedPattern::Solid, r, g, b);
}

void LEDStrip::set_anim(LedPattern pattern, uint8_t r, uint8_t g, uint8_t b)
{
    // Store the RAW color; the current brightness (capped, ALS-driven) is applied
    // at render time so a later brightness change affects an already-set color.
    m_color.r = r;
    m_color.g = g;
    m_color.b = b;
    m_pattern = pattern;
}

uint32_t LEDStrip::rand_next()
{
    uint32_t x = m_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_rng = x ? x : 0x1234abcdu;
    return m_rng;
}

LedPattern led_pattern_from_code(uint8_t code)
{
    switch (code) {
    case (uint8_t)LedPattern::Off:     return LedPattern::Off;
    case (uint8_t)LedPattern::Solid:   return LedPattern::Solid;
    case (uint8_t)LedPattern::Rainbow: return LedPattern::Rainbow;
    case (uint8_t)LedPattern::Wheel:   return LedPattern::Wheel;
    case (uint8_t)LedPattern::Breathe: return LedPattern::Breathe;
    case (uint8_t)LedPattern::Comet:   return LedPattern::Comet;
    case (uint8_t)LedPattern::Sparkle: return LedPattern::Sparkle;
    default:                           return LedPattern::Solid;
    }
}

void LEDStrip::render()
{
    switch (m_pattern) {
    case LedPattern::Solid:   render_solid();   break;
    case LedPattern::Rainbow: render_rainbow(); break;
    case LedPattern::Wheel:   render_wheel();   break;
    case LedPattern::Breathe: render_breathe(); break;
    case LedPattern::Comet:   render_comet();   break;
    case LedPattern::Sparkle: render_sparkle(); break;
    case LedPattern::Off:
    default:                  render_off();     break;
    }
    m_tick++;
}

void LEDStrip::power_on()
{
    // Re-enable the active-low PMOS gate so the WS2812 VDD rail is restored after
    // a prior off() (which cut it). The render thread resumes lighting the chain.
    gpio_pin_configure_dt(&s_power, GPIO_OUTPUT_ACTIVE);
    m_powered = true;
    m_render_enabled = true;
}

void LEDStrip::power_on_dark()
{
    // Power the rail (which also feeds the shared LTR-329 light sensor) but keep the
    // chain dark for an ambient-light pulse in config mode. Keep the render thread
    // DISABLED (m_render_enabled stays false) so it can't commit to the WS2812 SPI bus
    // while this caller (the ALS thread) does — only one committer at a time. Clock
    // black once here to close the power-up flash window.
    m_render_enabled = false;
    m_pattern = LedPattern::Off;
    gpio_pin_configure_dt(&s_power, GPIO_OUTPUT_ACTIVE);
    m_powered = true;
    if (device_is_ready(s_strip)) {
        render_off();   // commit black now (render thread is disabled, so no SPI race)
    }
}

void LEDStrip::off()
{
    // Stop the render thread committing, push black out (while still powered), then
    // float the power gate to high impedance so the WS2812 VDD rail collapses and the
    // chain draws no current. The PMOS gate has an external pull-up, so disconnecting the
    // pin (rather than actively driving it) pulls the gate high = PMOS off, and that holds
    // through System OFF where a driven output isn't reliably retained. Safe to call even
    // if init() failed (e.g. the strip device never came up): skip the black-out commit in
    // that case, but ALWAYS float the gate — init() drives it active before the strip-ready
    // check, so this is the one operation that must run to leave the rail off before sleep.
    m_powered = false;
    m_render_enabled = false;
    m_pattern = LedPattern::Off;
    if (device_is_ready(s_strip)) {
        render_off();
    }
    gpio_pin_configure_dt(&s_power, GPIO_DISCONNECTED);   // high-Z; external pull-up -> PMOS off
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
    const struct led_rgb px = scale_rgb(m_color, m_brightness);
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        m_pixels[i] = px;
    }
    commit();
}

void LEDStrip::render_rainbow()
{
    const uint16_t phase = (uint16_t)((m_tick * kRotationStep) % 360);
    constexpr uint16_t spread = 360 / STRIP_NUM_PIXELS;
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint16_t hue = (phase + i * spread) % 360;
        m_pixels[i] = hsv_to_rgb(hue, m_brightness);
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
        const uint8_t val = (uint8_t)(m_brightness * factor);
        m_pixels[i] = hsv_to_rgb(hue, val);
    }
    commit();
}

void LEDStrip::render_breathe()
{
    // Triangle-wave brightness from kBreatheFloor..255 over kBreathePeriod frames.
    const uint32_t c    = m_tick % kBreathePeriod;
    const uint32_t half = kBreathePeriod / 2;
    const uint32_t tri  = (c < half) ? c : (kBreathePeriod - c);  // 0..half
    const uint8_t lvl = (uint8_t)(kBreatheFloor +
                                  (uint32_t)(255 - kBreatheFloor) * tri / half);
    const uint8_t b8 = (uint8_t)((uint16_t)lvl * m_brightness / 255);
    const struct led_rgb px = scale_rgb(m_color, b8);
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        m_pixels[i] = px;
    }
    commit();
}

void LEDStrip::render_comet()
{
    // A fractional head sweeps the ring continuously; each LED lights by how far it
    // sits *behind* the head (wrapped), so the comet glides with a fading tail
    // instead of hopping LED-to-LED.
    const float speed = (float)STRIP_NUM_PIXELS / kCometFramesPerLoop;
    const float head = fmodf((float)m_tick * speed, (float)STRIP_NUM_PIXELS);
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        float behind = head - (float)i;
        if (behind < 0.0f) {
            behind += (float)STRIP_NUM_PIXELS;   // wrap: distance behind the head
        }
        float factor = 1.0f - behind / kCometTail;
        if (factor < 0.0f) {
            factor = 0.0f;
        }
        m_pixels[i] = scale_rgb(m_color, (uint8_t)(factor * (float)m_brightness));
    }
    commit();
}

void LEDStrip::render_sparkle()
{
    // Decay each pixel, occasionally relight a random one to full, keep a dim floor.
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        m_level[i] = (m_level[i] > kSparkDecay) ? (uint8_t)(m_level[i] - kSparkDecay) : 0;
    }
    if ((rand_next() & 0x7) == 0) {
        m_level[rand_next() % STRIP_NUM_PIXELS] = 255;
    }
    for (size_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        const uint8_t raw_lvl = m_level[i] > kSparkFloor ? m_level[i] : kSparkFloor;
        const uint8_t lvl = (uint8_t)((uint16_t)raw_lvl * m_brightness / 255);
        m_pixels[i] = scale_rgb(m_color, lvl);
    }
    commit();
}

void LEDStrip::commit()
{
    led_strip_update_rgb(s_strip, m_pixels, STRIP_NUM_PIXELS);
}
