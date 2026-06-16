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

// Base config — gain is managed dynamically by ltr329als_autogain().
static const struct ltr329als_config s_base_cfg = {
    .gain = LTR329ALS_GAIN_8X, // used only on first init; autogain owns it after that
    .integ_time = LTR329ALS_INTEG_100MS,
    .meas_rate = LTR329ALS_RATE_200MS,
};

#define STACK_SIZE 1024
#define PRIORITY K_PRIO_PREEMPT(10)

K_THREAD_STACK_DEFINE(s_stack, STACK_SIZE);
static struct k_thread s_thread;

static void thread_entry(void *p1, void *p2, void *p3) {

  while (1) {
    // Sync init config with whatever gain autogain last selected.
    struct ltr329als_config cfg = s_base_cfg;
    cfg.gain = ltr329als_current_gain();

    // Init loop — retries until the sensor responds, with backoff.
    // Re-entered only on hard read errors.
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

    // Read loop — breaks to outer loop only on hard I2C error.
    while (1) {
      k_sleep(K_MSEC(500));

      struct ltr329als_sample sample;
      ret = ltr329als_read_lux(&sample);

      if (ret == -EAGAIN || ret == -ENODATA) {
        continue;
      }
      if (ret != 0) {
        LOG_WRN("Read err %d, reinitializing", ret);
        k_mutex_lock(&s_reading_mutex, K_FOREVER);
        s_reading.valid = false;
        strncpy(s_reading.diag, "Reinitializing...", sizeof(s_reading.diag) - 1);
        k_mutex_unlock(&s_reading_mutex);
        break;
      }

      set_reading(sample.lux, sample.gain, sample.ch0, sample.ch1);
      LOG_INF("ch0=%u ch1=%u lux=%u", sample.ch0, sample.ch1, sample.lux);
    }
  }
}

void ambient_light_sensor_start(void) {
  k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
                  thread_entry, NULL, NULL, NULL, PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&s_thread, "als_monitor");
}
