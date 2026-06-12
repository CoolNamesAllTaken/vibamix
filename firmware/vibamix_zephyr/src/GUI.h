#ifndef VIBAMIX_GUI_H
#define VIBAMIX_GUI_H

#include <stdint.h>
#include "Display_EPD_W21.h"

class GUI {
public:
    void init();
    void wake();
    void show_hello_world();
    void show_als_readings(uint32_t lux, uint32_t gain, uint16_t ch0_raw, uint16_t ch1_raw, bool valid, const char *diag);
    void sleep();

private:
    uint8_t m_image[EPD_WIDTH * EPD_HEIGHT / 8];
};

#endif /* VIBAMIX_GUI_H */
