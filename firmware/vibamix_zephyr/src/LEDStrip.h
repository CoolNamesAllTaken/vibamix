#ifndef VIBAMIX_LED_STRIP_H
#define VIBAMIX_LED_STRIP_H

#include <zephyr/drivers/led_strip.h>
#include <zephyr/devicetree.h>
#include <stdint.h>

#define STRIP_NODE       DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

// Selectable LED behaviors. Add a value here + a render_*() method + a switch
// case in render() to introduce a new pattern; mesh/app code commands it via
// set_pattern().
enum class LedPattern : uint8_t {
    Off,        // strip blanked
    Solid,      // single brightness-scaled color (see set_color)
    Rainbow,    // rotating dim rainbow
    Wheel,      // scrolling RGB wheel with a soft blob bouncing left<->right
};

class LEDStrip {
public:
    // Frame period of the animation loop (~25 fps). Exposed so the caller that
    // drives render() ticks at the same cadence the animations assume.
    static constexpr uint32_t kFrameMs = 40;

    // Enables the strip's power gate and checks the device is ready.
    int init();

    // Select the active pattern drawn by render(). This is the command hook
    // for future patterns (e.g. from a mesh opcode).
    void set_pattern(LedPattern pattern);
    LedPattern pattern() const { return m_pattern; }

    // Set a solid color (brightness-scaled) and switch to the Solid pattern,
    // e.g. from a mesh command.
    void set_color(uint8_t r, uint8_t g, uint8_t b);

    // Draw one frame of the active pattern and advance its animation clock.
    void render();

    // Blocking: render the active pattern for `duration_ms` as a boot "alive"
    // signal before the GUI/mesh come up. Shares the animation clock with
    // render(), so the main loop continues the same animation seamlessly.
    void play_for(uint32_t duration_ms);

private:
    void render_off();
    void render_solid();
    void render_rainbow();
    void render_wheel();
    void commit();

    struct led_rgb m_pixels[STRIP_NUM_PIXELS];
    struct led_rgb m_color{};
    LedPattern     m_pattern{LedPattern::Wheel};
    uint32_t       m_tick{0};   // free-running animation frame counter
};

#endif /* VIBAMIX_LED_STRIP_H */
