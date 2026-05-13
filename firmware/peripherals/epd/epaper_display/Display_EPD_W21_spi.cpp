#include "Display_EPD_W21_spi.h"

#if DT_NODE_EXISTS(DT_NODELABEL(epaper))

#define EPD_NODE DT_NODELABEL(epaper)

const struct gpio_dt_spec epd_dc_gpio   = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
const struct gpio_dt_spec epd_rst_gpio  = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
const struct gpio_dt_spec epd_busy_gpio = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

static const struct spi_dt_spec epd_spi = SPI_DT_SPEC_GET(
	EPD_NODE, SPI_OP_MODE_MASTER | SPI_WORD_SET(8));

void EPD_GPIO_Init(void)
{
	gpio_pin_configure_dt(&epd_dc_gpio,   GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&epd_rst_gpio,  GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&epd_busy_gpio, GPIO_INPUT);
}

void SPI_Write(unsigned char value)
{
	const struct spi_buf tx = { .buf = &value, .len = 1 };
	const struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
	spi_write_dt(&epd_spi, &tx_set);
}

void EPD_W21_WriteCMD(unsigned char command)
{
	EPD_W21_DC_0;
	SPI_Write(command);
}

void EPD_W21_WriteDATA(unsigned char data)
{
	EPD_W21_DC_1;
	SPI_Write(data);
}

#else /* !DT_NODE_EXISTS(DT_NODELABEL(epaper)) */

/* Stub implementations for build targets without EPD hardware (e.g. DEVELOP_IN_NRF52833) */
const struct gpio_dt_spec epd_dc_gpio   = {};
const struct gpio_dt_spec epd_rst_gpio  = {};
const struct gpio_dt_spec epd_busy_gpio = {};

void EPD_GPIO_Init(void) {}
void SPI_Write(unsigned char value) { (void)value; }
void EPD_W21_WriteCMD(unsigned char command) { (void)command; }
void EPD_W21_WriteDATA(unsigned char data) { (void)data; }

#endif /* DT_NODE_EXISTS(DT_NODELABEL(epaper)) */

/***********************************************************
                end file
***********************************************************/
