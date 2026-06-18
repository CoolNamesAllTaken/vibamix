#ifndef VIBAMIX_LED_STRIP_H
#define VIBAMIX_LED_STRIP_H

#include <zephyr/drivers/led_strip.h>
#include <zephyr/devicetree.h>
#include <stdint.h>

#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

// Selectable LED behaviors. Add a value here + a render_*() method + a switch
// case in render() to introduce a new pattern; mesh/app code commands it via
// set_pattern() / set_anim(). The numeric values are the on-wire animation code
// (per-frame LED config + SET_FRAME_LED) — keep them stable.
enum class LedPattern : uint8_t {
    Off     = 0,  // strip blanked
    Solid   = 1,  // single brightness-scaled color (see set_color)
    Rainbow = 2,  // rotating dim rainbow (color ignored)
    Wheel   = 3,  // scrolling RGB wheel with a soft blob bouncing (color ignored)
    Breathe = 4,  // chosen color eased up/down in brightness (~2.5 s)
    Comet   = 5,  // chosen-color dot runs across with a fading tail, wrapping
    Sparkle = 6,  // chosen-color twinkles over a dim floor
};

// Clamp an on-wire animation code to a valid LedPattern (unknown -> Solid).
LedPattern led_pattern_from_code(uint8_t code);

class LEDStrip {
public:
    // Frame period of the animation loop (~25 fps). Exposed so the caller that
    // drives render() ticks at the same cadence the animations assume.
    static constexpr uint32_t kFrameMs = 40;

    // Enables the strip's power gate, checks the device is ready, and starts the
    // dedicated render thread (steady ~25 fps, independent of the ePaper/main loop).
    int init();

    // Select the active pattern drawn by render(). This is the command hook
    // for future patterns (e.g. from a mesh opcode).
    void set_pattern(LedPattern pattern);
    LedPattern pattern() const { return m_pattern; }

    // Set a solid color (brightness-scaled) and switch to the Solid pattern,
    // e.g. from the SET_LED_COLOR command.
    void set_color(uint8_t r, uint8_t g, uint8_t b);

    // Select an animation AND its base color together (does not force Solid).
    // Used to apply a frame's per-frame LED config when it's displayed.
    void set_anim(LedPattern pattern, uint8_t r, uint8_t g, uint8_t b);

    // Re-assert the WS2812 power gate (PMOS on) after a prior off(), e.g. when
    // config mode wants to light a frame's LED again. init() also does this. The
    // render thread resumes driving the strip while powered.
    void power_on();

    // Blank the chain and cut the WS2812 power gate (PMOS off), e.g. before
    // deep sleep so the strip draws no current. The render thread idles while off.
    void off();

private:
    // Render thread: draws the active pattern every kFrameMs while powered.
    static void thread_entry(void *a, void *b, void *c);
    void render_loop();
    // Draw one frame of the active pattern and advance its animation clock.
    void render();
    void render_off();
    void render_solid();
    void render_rainbow();
    void render_wheel();
    void render_breathe();
    void render_comet();
    void render_sparkle();
    void commit();
    uint32_t rand_next();

    struct led_rgb m_pixels[STRIP_NUM_PIXELS];
    struct led_rgb m_color{};
    LedPattern     m_pattern{LedPattern::Wheel};
    uint32_t       m_tick{0};   // free-running animation frame counter
    uint8_t        m_level[STRIP_NUM_PIXELS]{};  // per-pixel fade state (sparkle)
    uint32_t       m_rng{0x1234abcdu};           // xorshift PRNG state (sparkle)
    volatile bool  m_powered{false};             // strip gate on -> render thread commits
};

#endif /* VIBAMIX_LED_STRIP_H */
