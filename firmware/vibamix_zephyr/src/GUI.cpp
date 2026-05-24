#include "GUI.h"
#include "Display_EPD_W21_spi.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include <zephyr/sys/printk.h>

void GUI::init()
{
    EPD_GPIO_Init();
    EPD_HW_Init();
}

void GUI::show_hello_world()
{
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(10, 10, "Hello World", &Font20, WHITE, BLACK);
    EPD_Display(m_image);
    EPD_Update();
    printk("ePaper hello world displayed\n");
}

void GUI::sleep()
{
    EPD_DeepSleep();
}
