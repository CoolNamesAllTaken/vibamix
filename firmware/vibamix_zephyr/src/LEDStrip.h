#pragma once

#include <stdint.h>

class LEDStrip {
public:
    bool init();
    void set(uint8_t r, uint8_t g, uint8_t b);
    void off();
};
