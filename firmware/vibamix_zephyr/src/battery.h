#ifndef VIBAMIX_BATTERY_H
#define VIBAMIX_BATTERY_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery-voltage monitor. VBAT runs through a TPS22916 load switch (enable =
 * P1.15 / vbat_enable) to a 10K/10K divider whose midpoint is on SAADC AIN7
 * (P1.14). The divider is only powered while the switch is on, so reads are
 * one-shot: enable, settle, sample, disable. battery_mV = AIN7_mV * 2.
 */

/* One-time setup of the ADC channel + enable GPIO. Returns 0 on success. */
int battery_init(void);

/* One-shot battery voltage in millivolts, or a negative errno. Gates the
 * divider on only for the duration of the sample. */
int battery_read_mv(void);

/* Approximate 1S-LiPo state-of-charge (0..100%) for a given battery millivolts,
 * from a representative resting-voltage curve. Under load the reading sags, so
 * treat this as an estimate. */
int battery_percent(int mv);

#ifdef __cplusplus
}
#endif

#endif /* VIBAMIX_BATTERY_H */
