#include "ambient_light_sensor.h"
#include "LTR329ALS.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ambient_light_sensor, LOG_LEVEL_INF);

K_MUTEX_DEFINE(s_reading_mutex);
static struct als_reading s_reading = {0, 0, 0, 0, false, "Pending"};

static void set_diag(const char *msg) {
  k_mutex_lock(&s_reading_mutex, K_FOREVER);
  strncpy(s_reading.diag, msg, sizeof(s_reading.diag) - 1);
  s_reading.diag[sizeof(s_reading.diag) - 1] = '\0';
  k_mutex_unlock(&s_reading_mutex);
}

static void set_reading(uint32_t lux, uint32_t gain, uint16_t ch0,
                        uint16_t ch1) {
  k_mutex_lock(&s_reading_mutex, K_FOREVER);
  s_reading.lux = lux;
  s_reading.gain = gain;
  s_reading.ch0_raw = ch0;
  s_reading.ch1_raw = ch1;
  s_reading.valid = true;
  k_mutex_unlock(&s_reading_mutex);
}

struct als_reading ambient_light_sensor_get(void) {
  k_mutex_lock(&s_reading_mutex, K_FOREVER);
  struct als_reading value = s_reading;
  k_mutex_unlock(&s_reading_mutex);
  return value;
}

// Piecewise lux formula from LTR-329ALS-01 Appendix A.
// gain_f: actual gain multiplier (1, 2, 4, 8, 48, 96)
// integ_f: integration time relative to 100ms (0.5, 1.0, 2.0, 4.0 ...)
static uint32_t compute_lux(uint16_t ch0, uint16_t ch1, uint32_t gain_f,
                            float integ_f) {
  if (ch0 == 0 && ch1 == 0) {
    return 0;
  }
  float ratio = (float)ch1 / ((float)ch0 + (float)ch1);
  float lux;
  if (ratio < 0.45f) {
    lux = (1.7743f * ch0 + 1.1059f * ch1) / gain_f / integ_f;
  } else if (ratio < 0.64f) {
    lux = (4.2785f * ch0 - 1.9548f * ch1) / gain_f / integ_f;
    if (lux < 0.0f)
      lux = 0.0f;
  } else if (ratio < 0.85f) {
    lux = (0.5926f * ch0 + 0.1185f * ch1) / gain_f / integ_f;
  } else {
    lux = 0.0f;
  }
  return (uint32_t)(lux + 0.5f);
}

static uint32_t cfg_gain_factor(ltr329als_gain g) {
  switch (g) {
  case LTR329ALS_GAIN_1X:
    return 1;
  case LTR329ALS_GAIN_2X:
    return 2;
  case LTR329ALS_GAIN_4X:
    return 4;
  case LTR329ALS_GAIN_8X:
    return 8;
  case LTR329ALS_GAIN_48X:
    return 48;
  case LTR329ALS_GAIN_96X:
    return 96;
  default:
    return 1;
  }
}

static float cfg_integ_factor(ltr329als_integ_time t) {
  switch (t) {
  case LTR329ALS_INTEG_50MS:
    return 0.5f;
  case LTR329ALS_INTEG_100MS:
    return 1.0f;
  case LTR329ALS_INTEG_150MS:
    return 1.5f;
  case LTR329ALS_INTEG_200MS:
    return 2.0f;
  case LTR329ALS_INTEG_250MS:
    return 2.5f;
  case LTR329ALS_INTEG_300MS:
    return 3.0f;
  case LTR329ALS_INTEG_350MS:
    return 3.5f;
  case LTR329ALS_INTEG_400MS:
    return 4.0f;
  default:
    return 1.0f;
  }
}

// Base config — gain is managed dynamically by the auto-gain loop.
static const struct ltr329als_config s_base_cfg = {
    .gain = LTR329ALS_GAIN_8X, // starting point; auto-gain adjusts from here
    .integ_time = LTR329ALS_INTEG_100MS,
    .meas_rate = LTR329ALS_RATE_200MS,
};

// Gain steps ordered lowest sensitivity (outdoor) → highest (dark indoor).
static const ltr329als_gain k_gain_seq[] = {
    LTR329ALS_GAIN_1X,  LTR329ALS_GAIN_2X,  LTR329ALS_GAIN_4X,
    LTR329ALS_GAIN_8X, // index 3 — default start
    LTR329ALS_GAIN_48X, LTR329ALS_GAIN_96X,
};
#define GAIN_SEQ_LEN ARRAY_SIZE(k_gain_seq)
#define GAIN_START_IDX 3

// Step down when ch0 exceeds this (80% of full scale) — preempts -ERANGE.
#define AUTOGAIN_HIGH_THRESHOLD 52000
// Step up after this many consecutive low readings.
#define AUTOGAIN_LOW_THRESHOLD 1000
#define AUTOGAIN_LOW_CONSEC 3

#define STACK_SIZE 1024
#define PRIORITY K_PRIO_PREEMPT(10)

K_THREAD_STACK_DEFINE(s_stack, STACK_SIZE);
static struct k_thread s_thread;

static void thread_entry(void *p1, void *p2, void *p3) {
  const float integ_f = cfg_integ_factor(s_base_cfg.integ_time);
  int gain_idx = GAIN_START_IDX;

  while (1) {
    // Build config with current auto-gain step.
    struct ltr329als_config cfg = s_base_cfg;
    cfg.gain = k_gain_seq[gain_idx];

    // Init loop — retries until the sensor responds, with backoff.
    // Re-entered on hard read errors and after gain changes.
    set_diag("Initializing...");
    int ret;
    do {
      ret = ltr329als_init(&cfg);
      if (ret != 0) {
        char msg[32];
        if (ret == -ENODEV) {
          snprintf(msg, sizeof(msg), "I2C not ready");
        } else if (ret == -EPROTO) {
          snprintf(msg, sizeof(msg), "Bad part ID");
        } else {
          snprintf(msg, sizeof(msg), "Init err: %d", ret);
        }
        LOG_ERR("%s", msg);
        set_diag(msg);
        k_msleep(200);
      }
    } while (ret != 0);

    int low_count = 0;

    // Read loop — breaks back to the outer loop on gain change or hard error.
    while (1) {
      k_sleep(K_MSEC(500));

      uint16_t ch0 = 0, ch1 = 0;
      ltr329als_gain active_gain;
      ret = ltr329als_read(&ch0, &ch1, &active_gain);

      if (ret == -EAGAIN) {
        continue;
      } else if (ret == -ERANGE) {
        // Hardware flagged saturation — step down if possible.
        if (gain_idx > 0) {
          gain_idx--;
          LOG_INF("Auto-gain down (saturated): step %d", gain_idx);
          break;
        }
        continue; // already at minimum gain, skip this sample
      } else if (ret != 0) {
        // Hard I2C/bus error — reinitialize.
        LOG_WRN("Read err %d, reinitializing", ret);
        k_mutex_lock(&s_reading_mutex, K_FOREVER);
        s_reading.valid = false;
        strncpy(s_reading.diag, "Reinitializing...",
                sizeof(s_reading.diag) - 1);
        k_mutex_unlock(&s_reading_mutex);
        break;
      }

      // Valid reading — use the gain the sensor actually reported, not what
      // we configured, so the lux formula is always correct.
      uint32_t gain_f = cfg_gain_factor(active_gain);
      uint32_t lux = compute_lux(ch0, ch1, gain_f, integ_f);
      set_reading(lux, gain_f, ch0, ch1);
      LOG_INF("ch0=%u ch1=%u lux=%u gain_idx=%d", ch0, ch1, lux, gain_idx);

      // Pre-saturation: step down before the hardware flags it.
      if (ch0 > AUTOGAIN_HIGH_THRESHOLD && gain_idx > 0) {
        gain_idx--;
        LOG_INF("Auto-gain down (high): step %d", gain_idx);
        break;
      }

      // Low signal: step up after several consecutive dim readings.
      if (ch0 < AUTOGAIN_LOW_THRESHOLD) {
        if (++low_count >= AUTOGAIN_LOW_CONSEC &&
            gain_idx < (int)GAIN_SEQ_LEN - 1) {
          gain_idx++;
          LOG_INF("Auto-gain up: step %d", gain_idx);
          break;
        }
      } else {
        low_count = 0;
      }
    }
  }
}

void ambient_light_sensor_start(void) {
  k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
                  thread_entry, NULL, NULL, NULL, PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&s_thread, "als_monitor");
}
