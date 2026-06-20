#pragma once

#include <stdint.h>

class LEDStrip;  // the LTR-329 shares the LED power rail (see ambient_light_sensor.cpp)

struct als_reading {
  uint32_t lux;             // estimated illuminance in lux
  uint32_t gain;            // active gain multiplier (1, 2, 4, 8, 48, or 96)
  uint16_t ch0_raw;         // raw ADC count, CH0 (visible + IR)
  uint16_t ch1_raw;         // raw ADC count, CH1 (IR only)
  bool valid;               // false until the first successful read
  char diag[32];            // human-readable status, populated when !valid
};

// Spawn the ALS monitor thread. `leds` owns the shared LED/sensor power rail; the
// monitor only reads I2C while that rail is powered (the LTR-329 is powered from
// the WS2812 gate). Pass the app's LEDStrip. Call before ambient_light_sensor_get().
void ambient_light_sensor_start(LEDStrip *leds);

// Enter/leave config-mode sampling. In config mode the LEDs (and thus the sensor)
// are normally off, so the monitor switches to briefly pulsing the rail for a
// reading instead of continuous polling. set ... (false) blocks until any in-flight
// pulse has released the rail, so the caller can safely re-take the LED gate.
void ambient_light_sensor_set_config_mode(bool on);

// Returns the latest ambient light reading.
struct als_reading ambient_light_sensor_get(void);
