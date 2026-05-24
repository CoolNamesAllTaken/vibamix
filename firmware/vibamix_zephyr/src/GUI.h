#ifndef VIBAMIX_GUI_H
#define VIBAMIX_GUI_H

#include <stdint.h>
#include "Display_EPD_W21.h"

class GUI {
public:
    void init();
    void show_hello_world();
    void sleep();

private:
    uint8_t m_image[EPD_WIDTH * EPD_HEIGHT / 8];
};

#endif /* VIBAMIX_GUI_H */
