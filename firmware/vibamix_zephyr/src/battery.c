#include "battery.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

static const struct adc_dt_spec s_adc = ADC_DT_SPEC_GET(DT_PATH(zephyr_user));
static const struct gpio_dt_spec s_vbat_en =
	GPIO_DT_SPEC_GET(DT_NODELABEL(vbat_enable), gpios);

/* RC on the divider node (~10K source + 100nF) settles in ~1 ms; allow margin. */
#define VBAT_SETTLE_MS 2

int battery_init(void)
{
	if (!adc_is_ready_dt(&s_adc)) {
		printk("battery: ADC not ready\n");
		return -ENODEV;
	}
	int err = adc_channel_setup_dt(&s_adc);
	if (err < 0) {
		printk("battery: adc_channel_setup failed (%d)\n", err);
		return err;
	}
	if (!gpio_is_ready_dt(&s_vbat_en)) {
		printk("battery: VBAT_EN not ready\n");
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&s_vbat_en, GPIO_OUTPUT_INACTIVE);
}

int battery_read_mv(void)
{
	uint16_t sample = 0;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	gpio_pin_set_dt(&s_vbat_en, 1);  /* power the divider */
	k_msleep(VBAT_SETTLE_MS);

	adc_sequence_init_dt(&s_adc, &seq);
	int err = adc_read_dt(&s_adc, &seq);

	gpio_pin_set_dt(&s_vbat_en, 0);  /* cut the divider current */

	if (err < 0) {
		printk("battery: adc_read failed (%d)\n", err);
		return err;
	}

	int32_t mv = sample;
	err = adc_raw_to_millivolts_dt(&s_adc, &mv);
	if (err < 0) {
		printk("battery: raw_to_mv failed (%d)\n", err);
		return err;
	}

	mv *= 2;  /* 10K/10K divider */
	printk("battery: %d mV (%d%%)\n", mv, battery_percent(mv));
	return mv;
}

/* Representative 1S-LiPo resting-voltage state-of-charge curve (mV -> %),
 * descending. Linear-interpolated between points. Approximate. */
struct soc_point { int mv; int pct; };
static const struct soc_point s_curve[] = {
	{ 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3930, 70 }, { 3870, 60 },
	{ 3800, 50 },  { 3750, 40 }, { 3700, 30 }, { 3650, 20 }, { 3550, 10 },
	{ 3400, 5 },   { 3000, 0 },
};

int battery_percent(int mv)
{
	if (mv >= s_curve[0].mv) {
		return 100;
	}
	const int n = (int)(sizeof(s_curve) / sizeof(s_curve[0]));
	if (mv <= s_curve[n - 1].mv) {
		return 0;
	}
	for (int i = 1; i < n; i++) {
		if (mv >= s_curve[i].mv) {
			const struct soc_point *hi = &s_curve[i - 1];
			const struct soc_point *lo = &s_curve[i];
			/* interpolate between lo (lower mv) and hi (higher mv) */
			return lo->pct + (mv - lo->mv) * (hi->pct - lo->pct) / (hi->mv - lo->mv);
		}
	}
	return 0;
}
