#include "LTR329ALS.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ltr329als, LOG_LEVEL_INF);

#if !DT_NODE_EXISTS(DT_NODELABEL(als))
#error "LTR-329ALS-01 DTS node 'als' not found"
#endif

#define ALS_NODE DT_NODELABEL(als)

// Register addresses
#define REG_ALS_CONTR 0x80
#define REG_ALS_MEAS_RATE 0x85
#define REG_PART_ID 0x86
#define REG_MANUFAC_ID 0x87
#define REG_ALS_DATA_CH1_0 0x88 // IR low
#define REG_ALS_DATA_CH1_1 0x89 // IR high
#define REG_ALS_DATA_CH0_0 0x8A // Visible+IR low
#define REG_ALS_DATA_CH0_1 0x8B // Visible+IR high
#define REG_ALS_STATUS 0x8C

#define PART_ID_EXPECTED 0xA0
#define MANUFAC_ID_EXPECTED 0x05

// ALS_STATUS bits (0x8C)
#define ALS_STATUS_DATA_NEW BIT(2) // 1 = new data available, clears on read
#define ALS_STATUS_INVALID BIT(7)  // 1 = data invalid (0 = valid)

static const struct i2c_dt_spec s_i2c = I2C_DT_SPEC_GET(ALS_NODE);

int ltr329als_init(const struct ltr329als_config *cfg) {
  if (!device_is_ready(s_i2c.bus)) {
    LOG_ERR("I2C bus not ready");
    return -ENODEV; // bus not found / not ready
  }

  uint8_t part_id;
  int ret = i2c_reg_read_byte_dt(&s_i2c, REG_PART_ID, &part_id);
  if (ret < 0) {
    LOG_ERR("Failed to read PART_ID: %d", ret);
    return ret; // I2C comms failure (e.g. -EIO, -ENXIO)
  }
  if (part_id != PART_ID_EXPECTED) {
    LOG_ERR("Unexpected PART_ID: 0x%02X (expected 0x%02X)", part_id,
            PART_ID_EXPECTED);
    return -EPROTO; // sensor responded but gave wrong ID
  }

  uint8_t meas_rate_reg =
      ((uint8_t)cfg->integ_time << 3) | (uint8_t)cfg->meas_rate;
  ret = i2c_reg_write_byte_dt(&s_i2c, REG_ALS_MEAS_RATE, meas_rate_reg);
  if (ret < 0) {
    LOG_ERR("Failed to write ALS_MEAS_RATE: %d", ret);
    return ret;
  }

  // Writing ALS_CONTR last activates the sensor
  uint8_t contr_reg = ((uint8_t)cfg->gain << 2) | 0x01 /* active mode */;
  ret = i2c_reg_write_byte_dt(&s_i2c, REG_ALS_CONTR, contr_reg);
  if (ret < 0) {
    LOG_ERR("Failed to write ALS_CONTR: %d", ret);
    return ret;
  }

  // Allow first measurement to complete (worst-case 400ms integration + margin)
  k_msleep(410);

  LOG_INF("LTR-329ALS-01 ready");
  return 0;
}

int ltr329als_read(uint16_t *ch0, uint16_t *ch1, ltr329als_gain *active_gain) {
  uint8_t status;
  int ret = i2c_reg_read_byte_dt(&s_i2c, REG_ALS_STATUS, &status);
  if (ret < 0) {
    return ret;
  }
  if (status & ALS_STATUS_INVALID) {
    return -ERANGE;  // sensor saturated — not a bus/sensor fault
  }
  if (!(status & ALS_STATUS_DATA_NEW)) {
    return -EAGAIN;
  }

  // ALS_STATUS bits [6:4] report the actual gain used for this measurement.
  // The encoding matches ltr329als_gain enum values directly.
  *active_gain = (ltr329als_gain)((status >> 4) & 0x07);

  // Datasheet requires reading CH1 before CH0, low byte before high byte.
  // The 4 registers lock as a group from the first read until 0x8B is read.
  uint8_t ch1_low, ch1_high, ch0_low, ch0_high;
  if ((ret = i2c_reg_read_byte_dt(&s_i2c, REG_ALS_DATA_CH1_0, &ch1_low)) < 0)
    return ret;
  if ((ret = i2c_reg_read_byte_dt(&s_i2c, REG_ALS_DATA_CH1_1, &ch1_high)) < 0)
    return ret;
  if ((ret = i2c_reg_read_byte_dt(&s_i2c, REG_ALS_DATA_CH0_0, &ch0_low)) < 0)
    return ret;
  if ((ret = i2c_reg_read_byte_dt(&s_i2c, REG_ALS_DATA_CH0_1, &ch0_high)) < 0)
    return ret;

  *ch1 = (uint16_t)ch1_low | ((uint16_t)ch1_high << 8);
  *ch0 = (uint16_t)ch0_low | ((uint16_t)ch0_high << 8);
  return 0;
}
