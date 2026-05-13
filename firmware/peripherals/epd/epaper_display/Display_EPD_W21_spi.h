#ifndef _DISPLAY_EPD_W21_SPI_
#define _DISPLAY_EPD_W21_SPI_

#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>

extern const struct gpio_dt_spec epd_dc_gpio;
extern const struct gpio_dt_spec epd_rst_gpio;
extern const struct gpio_dt_spec epd_busy_gpio;

// CS managed by Zephyr SPI driver via cs-gpios in DTS
#define EPD_W21_CS_0         ((void)0)
#define EPD_W21_CS_1         ((void)0)

// DC: GPIO_ACTIVE_LOW in DTS → logical 1 = physical LOW = command mode
#define EPD_W21_DC_0         gpio_pin_set_dt(&epd_dc_gpio, 1)
#define EPD_W21_DC_1         gpio_pin_set_dt(&epd_dc_gpio, 0)

// RST: GPIO_ACTIVE_LOW in DTS → logical 1 = physical LOW = reset asserted
#define EPD_W21_RST_0        gpio_pin_set_dt(&epd_rst_gpio, 1)
#define EPD_W21_RST_1        gpio_pin_set_dt(&epd_rst_gpio, 0)

// MOSI/CLK managed by Zephyr SPIM — bit-bang macros unused
#define EPD_W21_MOSI_0       ((void)0)
#define EPD_W21_MOSI_1       ((void)0)
#define EPD_W21_CLK_0        ((void)0)
#define EPD_W21_CLK_1        ((void)0)

// BUSY: GPIO_ACTIVE_HIGH in DTS → returns 1 when busy
#define isEPD_W21_BUSY       gpio_pin_get_dt(&epd_busy_gpio)

void SPI_Write(unsigned char value);
void EPD_W21_WriteDATA(unsigned char data);
void EPD_W21_WriteCMD(unsigned char command);
void EPD_GPIO_Init(void);

#endif  //#ifndef _DISPLAY_EPD_W21_SPI_

/***********************************************************
                end file
***********************************************************/
