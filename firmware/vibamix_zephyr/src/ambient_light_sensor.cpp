#include "ambient_light_sensor.h"
#include "LEDStrip.h"
#include "LTR329ALS.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(ambient_light_sensor, LOG_LEVEL_INF);

K_MUTEX_DEFINE(s_reading_mutex);
static struct als_reading s_reading = {0, 0, 0, 0, false, "Pending"};

// The LTR-329 is powered from the WS2812 LED rail (LEDStrip's power gate). Reading
// it while that rail is gated off (config mode / pre-LED-init) just storms the I2C
// bus, so the monitor only touches I2C while the rail is powered. In config mode the
// LEDs are deliberately dark, so we briefly *pulse* the rail for each reading. All
// I2C lives on this one thread, so the continuous reader and the pulse can never
// collide (no extra bus mutex needed).
static LEDStrip *s_leds;                        // owns the shared power rail
static atomic_t s_config_mode = ATOMIC_INIT(0); // 1 -> pulse instead of poll
static atomic_t s_in_pulse = ATOMIC_INIT(0);    // 1 while a pulse holds the rail
static atomic_t s_in_poll = ATOMIC_INIT(0);     // 1 while a continuous read holds the I2C bus

// How often to pulse a reading in config mode (each pulse powers the rail ~0.5 s).
#define PULSE_PERIOD_MS 5000
#define PULSE_STEP_MS   250   // re-check the mode this often between pulses

static void set_diag(const char *msg) {
  k_mutex_lock(&s_reading_mutex, K_FOREVER);
  s_reading.valid = false;
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

static bool rail_powered(void) { return s_leds && s_leds->powered(); }

// Take one reading after configuring the sensor (assumes it is powered). Returns
// true on a fresh sample (cached via set_reading). ltr329als_init() already waits
// out the first integration; poll a few times in case auto-gain forced a re-measure.
static bool read_once(void) {
  struct ltr329als_config cfg = s_base_cfg;
  cfg.gain = ltr329als_current_gain();
  if (ltr329als_init(&cfg) != 0) {
    return false;
  }
  struct ltr329als_sample sample;
  for (int i = 0; i < 6; i++) {
    int ret = ltr329als_read_lux(&sample);
    if (ret == 0) {
      set_reading(sample.lux, sample.gain, sample.ch0, sample.ch1);
      return true;
    }
    if (ret == -EAGAIN || ret == -ENODATA) {
      k_msleep(60); // data not ready / auto-gain re-measure — give it a moment
      continue;
    }
    break; // hard I2C error
  }
  return false;
}

// Config-mode reading: momentarily power the rail (LEDs kept dark), sample, cut it.
static void pulse_sample(void) {
  if (!s_leds) {
    return;
  }
  atomic_set(&s_in_pulse, 1);
  const bool was_powered = s_leds->powered();
  if (!was_powered) {
    s_leds->power_on_dark();
  }
  if (!read_once()) {
    set_diag("Pulse: no sample");
  }
  if (!was_powered) {
    s_leds->off();
  }
  atomic_set(&s_in_pulse, 0);
}

// Continuous reader for normal (LEDs-on) mode. Returns when the rail goes unpowered
// or we switch to config mode, so the outer loop can re-evaluate.
static void poll_while_powered(void) {
  // Init with backoff + log-once so a genuine fault can't storm the bus/logs.
  int backoff = 100;
  bool logged = false;
  int ret = -1;
  while (rail_powered() && !atomic_get(&s_config_mode)) {
    struct ltr329als_config cfg = s_base_cfg;
    cfg.gain = ltr329als_current_gain();
    // Claim the bus, then re-check: if config mode just switched (which cuts the shared
    // rail), bail before touching I2C so we can't be powered down mid-transfer (TWIM
    // hang). set_config_mode(true) waits on s_in_poll, closing the race the other way.
    atomic_set(&s_in_poll, 1);
    if (!rail_powered() || atomic_get(&s_config_mode)) {
      atomic_set(&s_in_poll, 0);
      return;
    }
    ret = ltr329als_init(&cfg);
    atomic_set(&s_in_poll, 0);
    if (ret == 0) {
      break;
    }
    if (!logged) {
      LOG_WRN("init failed (%d); backing off", ret);
      set_diag("Init failed");
      logged = true;
    }
    k_msleep(backoff);
    backoff = MIN(backoff * 2, 3000);
  }
  if (ret != 0) {
    return; // lost power / left normal mode during init
  }

  // Read loop — breaks on a hard error (to re-init) or a mode/power change.
  while (rail_powered() && !atomic_get(&s_config_mode)) {
    k_sleep(K_MSEC(500));
    if (!rail_powered() || atomic_get(&s_config_mode)) {
      return;
    }
    struct ltr329als_sample sample;
    // Same claim-then-recheck handshake as the init above, so a config-mode rail cut
    // can't land in the middle of this read.
    atomic_set(&s_in_poll, 1);
    if (!rail_powered() || atomic_get(&s_config_mode)) {
      atomic_set(&s_in_poll, 0);
      return;
    }
    ret = ltr329als_read_lux(&sample);
    atomic_set(&s_in_poll, 0);
    if (ret == -EAGAIN || ret == -ENODATA) {
      continue;
    }
    if (ret != 0) {
      LOG_WRN("read err %d, reinitializing", ret);
      set_diag("Reinitializing");
      return; // re-init on the next outer iteration
    }
    set_reading(sample.lux, sample.gain, sample.ch0, sample.ch1);
    LOG_INF("ch0=%u ch1=%u lux=%u", sample.ch0, sample.ch1, sample.lux);
  }
}

#define STACK_SIZE 1024
#define PRIORITY K_PRIO_PREEMPT(10)

K_THREAD_STACK_DEFINE(s_stack, STACK_SIZE);
static struct k_thread s_thread;

static void thread_entry(void *p1, void *p2, void *p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (1) {
    if (atomic_get(&s_config_mode)) {
      // Config mode: LEDs/sensor rail are dark — pulse for a reading, then idle
      // (re-checking the mode often so leaving config is responsive).
      pulse_sample();
      for (int slept = 0;
           slept < PULSE_PERIOD_MS && atomic_get(&s_config_mode);
           slept += PULSE_STEP_MS) {
        k_msleep(PULSE_STEP_MS);
      }
      continue;
    }

    if (!rail_powered()) {
      // Normal mode but the rail is off (LEDs off / pre-init): never touch I2C.
      set_diag("Unpowered");
      k_msleep(PULSE_STEP_MS);
      continue;
    }

    poll_while_powered();
  }
}

void ambient_light_sensor_start(LEDStrip *leds) {
  s_leds = leds;
  k_thread_create(&s_thread, s_stack, K_THREAD_STACK_SIZEOF(s_stack),
                  thread_entry, NULL, NULL, NULL, PRIORITY, 0, K_NO_WAIT);
  k_thread_name_set(&s_thread, "als_monitor");
}

void ambient_light_sensor_set_config_mode(bool on) {
  atomic_set(&s_config_mode, on ? 1 : 0);
  if (on) {
    // Entering config mode: the caller is about to cut the shared rail. Wait for any
    // in-flight continuous read to finish so the LTR-329 isn't powered down mid-I2C
    // (which hangs the TWIM bus and, on the just-booted reset-wake path, the badge).
    while (atomic_get(&s_in_poll)) {
      k_msleep(5);
    }
  } else {
    // Leaving config mode: wait for any in-flight pulse to release the rail before the
    // caller (main) re-takes the LED gate, so the two can't fight over the power pin.
    while (atomic_get(&s_in_pulse)) {
      k_msleep(5);
    }
  }
}
