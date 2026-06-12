#pragma once

#include <stdint.h>

// ALS_CONTR [4:2] — analogue gain
enum ltr329als_gain {
    LTR329ALS_GAIN_1X  = 0,
    LTR329ALS_GAIN_2X  = 1,
    LTR329ALS_GAIN_4X  = 2,
    LTR329ALS_GAIN_8X  = 3,
    LTR329ALS_GAIN_48X = 6,
    LTR329ALS_GAIN_96X = 7,
};

// ALS_MEAS_RATE [5:3] — ADC integration time (register encoding per datasheet)
enum ltr329als_integ_time {
    LTR329ALS_INTEG_100MS = 0,  // 000
    LTR329ALS_INTEG_50MS  = 1,  // 001
    LTR329ALS_INTEG_200MS = 2,  // 010
    LTR329ALS_INTEG_400MS = 3,  // 011
    LTR329ALS_INTEG_150MS = 4,  // 100
    LTR329ALS_INTEG_250MS = 5,  // 101
    LTR329ALS_INTEG_300MS = 6,  // 110
    LTR329ALS_INTEG_350MS = 7,  // 111
};

// ALS_MEAS_RATE [2:0] — time between measurements (must be >= integration time)
enum ltr329als_meas_rate {
    LTR329ALS_RATE_50MS   = 0,
    LTR329ALS_RATE_100MS  = 1,
    LTR329ALS_RATE_200MS  = 2,
    LTR329ALS_RATE_500MS  = 3,
    LTR329ALS_RATE_1000MS = 4,
    LTR329ALS_RATE_2000MS = 5,
};

struct ltr329als_config {
    ltr329als_gain      gain;
    ltr329als_integ_time integ_time;
    ltr329als_meas_rate  meas_rate;
};

// Initialize the LTR-329ALS-01. Verifies part ID, applies config, and activates the sensor.
// Returns 0 on success, negative errno on failure.
int ltr329als_init(const struct ltr329als_config *cfg);


// Read raw ADC counts from both channels.
// ch0: visible + IR light (primary channel for lux estimation)
// ch1: IR-only light
// active_gain: actual gain the sensor used for this measurement (from ALS_STATUS [6:4])
// Returns 0 on success, -EAGAIN if no new data, -ERANGE if saturated, negative errno on error.
int ltr329als_read(uint16_t *ch0, uint16_t *ch1, ltr329als_gain *active_gain);
