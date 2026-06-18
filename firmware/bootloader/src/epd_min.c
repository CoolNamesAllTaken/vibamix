/*
 * Minimal GDEY027T91 (SSD1680) driver for the bootloader — see epd_min.h.
 *
 * Ported from the app's peripherals/epd/.../Display_EPD_W21.cpp, keeping only the
 * HW init (SWRESET) and the full-refresh path (RAM 0x24 + update 0x22/0xF7/0x20).
 * GPIO polarity matches the board DTS (dc/reset GPIO_ACTIVE_LOW, busy ACTIVE_HIGH);
 * CS is driven by the Zephyr SPI controller via cs-gpios.
 */

#include "epd_min.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>

#define EPD_NODE DT_NODELABEL(epaper)

#if DT_NODE_EXISTS(EPD_NODE)

static const struct gpio_dt_spec dc_gpio   = GPIO_DT_SPEC_GET(EPD_NODE, dc_gpios);
static const struct gpio_dt_spec rst_gpio  = GPIO_DT_SPEC_GET(EPD_NODE, reset_gpios);
static const struct gpio_dt_spec busy_gpio = GPIO_DT_SPEC_GET(EPD_NODE, busy_gpios);

static const struct spi_dt_spec epd_spi =
	SPI_DT_SPEC_GET(EPD_NODE, SPI_OP_MODE_MASTER | SPI_WORD_SET(8), 0);

/* BUSY is ACTIVE_HIGH (1 = busy). Bound the wait so a dead/absent panel can't
 * wedge the bootloader; ~5 s covers a full refresh (typ ~2-4 s). */
#define BUSY_TIMEOUT_MS 6000

static int wait_busy(void)
{
	for (int waited = 0; waited < BUSY_TIMEOUT_MS; waited += 5) {
		if (gpio_pin_get_dt(&busy_gpio) == 0) {
			return 0;
		}
		k_msleep(5);
	}
	return -1; /* stuck busy */
}

static void write_cmd(uint8_t cmd)
{
	gpio_pin_set_dt(&dc_gpio, 1); /* DC active-low -> logical 1 = command */
	const struct spi_buf b = { .buf = &cmd, .len = 1 };
	const struct spi_buf_set set = { .buffers = &b, .count = 1 };
	spi_write_dt(&epd_spi, &set);
}

static void write_data(const uint8_t *data, size_t len)
{
	gpio_pin_set_dt(&dc_gpio, 0); /* logical 0 = data */
	const struct spi_buf b = { .buf = (void *)data, .len = len };
	const struct spi_buf_set set = { .buffers = &b, .count = 1 };
	spi_write_dt(&epd_spi, &set);
}

static void write_data1(uint8_t d)
{
	write_data(&d, 1);
}

int epd_min_init(void)
{
	if (!spi_is_ready_dt(&epd_spi) || !gpio_is_ready_dt(&dc_gpio) ||
	    !gpio_is_ready_dt(&rst_gpio) || !gpio_is_ready_dt(&busy_gpio)) {
		return -1;
	}

	gpio_pin_configure_dt(&dc_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rst_gpio, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&busy_gpio, GPIO_INPUT);

	/* Hardware reset (RST active-low: logical 1 = asserted). */
	gpio_pin_set_dt(&rst_gpio, 1);
	k_msleep(10);
	gpio_pin_set_dt(&rst_gpio, 0);
	k_msleep(10);

	if (wait_busy() < 0) {
		return -1;
	}
	write_cmd(0x12); /* SWRESET */
	if (wait_busy() < 0) {
		return -1;
	}
	return 0;
}

int epd_min_show(const uint8_t *fb)
{
	write_cmd(0x24); /* write B/W RAM (bit 1 = white) */
	write_data(fb, EPD_MIN_FB_SIZE);

	write_cmd(0x22); /* display update control */
	write_data1(0xF7);
	write_cmd(0x20); /* activate update sequence */
	return wait_busy();
}

#else /* !DT_NODE_EXISTS(EPD_NODE) */

int epd_min_init(void) { return -1; }
int epd_min_show(const uint8_t *fb) { (void)fb; return -1; }

#endif
