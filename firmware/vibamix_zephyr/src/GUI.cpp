#include "GUI.h"
#include "Display_EPD_W21_spi.h"
#include "GUI_Paint.h"
#include "fonts.h"
#include <stdio.h>
#include <zephyr/sys/printk.h>

void GUI::init()
{
    EPD_GPIO_Init();
    EPD_HW_Init();
}

void GUI::wake()
{
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

void GUI::show_als_readings(uint32_t lux, uint32_t gain, uint16_t ch0_raw, uint16_t ch1_raw, bool valid, const char *diag)
{
    char buf[32];
    Paint_NewImage(m_image, EPD_WIDTH, EPD_HEIGHT, ROTATE_270, WHITE);
    Paint_Clear(WHITE);
    if (valid) {
        snprintf(buf, sizeof(buf), "Lux: %u", lux);
        Paint_DrawString_EN(10, 5, buf, &Font20, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "Gain: %uX", gain);
        Paint_DrawString_EN(10, 32, buf, &Font16, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "CH0: %u", ch0_raw);
        Paint_DrawString_EN(10, 54, buf, &Font16, WHITE, BLACK);
        snprintf(buf, sizeof(buf), "CH1: %u", ch1_raw);
        Paint_DrawString_EN(10, 76, buf, &Font16, WHITE, BLACK);
    } else {
        Paint_DrawString_EN(10, 10, "ALS FAIL:", &Font20, WHITE, BLACK);
        Paint_DrawString_EN(10, 40, diag, &Font16, WHITE, BLACK);
    }
    EPD_Display(m_image);
    EPD_Update();
}

void GUI::sleep()
{
    EPD_DeepSleep();
}
