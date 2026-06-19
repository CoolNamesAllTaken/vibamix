#pragma once

#include <stdint.h>

struct als_reading {
  uint32_t lux;             // estimated illuminance in lux
  uint32_t gain;            // active gain multiplier (1, 2, 4, 8, 48, or 96)
  uint16_t ch0_raw;         // raw ADC count, CH0 (visible + IR)
  uint16_t ch1_raw;         // raw ADC count, CH1 (IR only)
  bool valid;               // false until the first successful read
  char diag[32];            // human-readable status, populated when !valid
};

// Spawn the ALS monitor thread. Must be called before ambient_light_sensor_get().
void ambient_light_sensor_start(void);

// Returns the latest ambient light reading.
struct als_reading ambient_light_sensor_get(void);
