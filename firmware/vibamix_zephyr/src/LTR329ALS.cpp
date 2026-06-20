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
static ltr329als_integ_time s_integ_time = LTR329ALS_INTEG_100MS;

static const ltr329als_gain k_gain_seq[] = {
    LTR329ALS_GAIN_1X, LTR329ALS_GAIN_2X, LTR329ALS_GAIN_4X,
    LTR329ALS_GAIN_8X,   // index 3 — default start
    LTR329ALS_GAIN_48X, LTR329ALS_GAIN_96X,
};
#define GAIN_SEQ_LEN     ARRAY_SIZE(k_gain_seq)
#define GAIN_START_IDX   3
#define AUTOGAIN_HIGH    52000
#define AUTOGAIN_LOW     1000
#define AUTOGAIN_LOW_CONSEC 3

static int s_gain_idx  = GAIN_START_IDX;
static int s_low_count = 0;

static int apply_gain(ltr329als_gain g) {
  uint8_t contr = ((uint8_t)g << 2) | 0x01;
  return i2c_reg_write_byte_dt(&s_i2c, REG_ALS_CONTR, contr);
}

uint32_t ltr329als_gain_factor(ltr329als_gain g) {
  switch (g) {
  case LTR329ALS_GAIN_1X:  return 1;
  case LTR329ALS_GAIN_2X:  return 2;
  case LTR329ALS_GAIN_4X:  return 4;
  case LTR329ALS_GAIN_8X:  return 8;
  case LTR329ALS_GAIN_48X: return 48;
  case LTR329ALS_GAIN_96X: return 96;
  default:                  return 1;
  }
}

static float integ_factor(ltr329als_integ_time t) {
  switch (t) {
  case LTR329ALS_INTEG_50MS:  return 0.5f;
  case LTR329ALS_INTEG_100MS: return 1.0f;
  case LTR329ALS_INTEG_150MS: return 1.5f;
  case LTR329ALS_INTEG_200MS: return 2.0f;
  case LTR329ALS_INTEG_250MS: return 2.5f;
  case LTR329ALS_INTEG_300MS: return 3.0f;
  case LTR329ALS_INTEG_350MS: return 3.5f;
  case LTR329ALS_INTEG_400MS: return 4.0f;
  default:                    return 1.0f;
  }
}

uint32_t ltr329als_compute_lux(uint16_t ch0, uint16_t ch1,
                                ltr329als_gain active_gain,
                                ltr329als_integ_time integ_time) {
  if (ch0 == 0 && ch1 == 0) {
    return 0;
  }
  uint32_t gain_f = ltr329als_gain_factor(active_gain);
  float integ_f   = integ_factor(integ_time);
  float ratio = (float)ch1 / ((float)ch0 + (float)ch1);
  float lux;
  if (ratio < 0.45f) {
    lux = (1.7743f * ch0 + 1.1059f * ch1) / gain_f / integ_f;
  } else if (ratio < 0.64f) {
    lux = (4.2785f * ch0 - 1.9548f * ch1) / gain_f / integ_f;
    if (lux < 0.0f) lux = 0.0f;
  } else if (ratio < 0.85f) {
    lux = (0.5926f * ch0 + 0.1185f * ch1) / gain_f / integ_f;
  } else {
    lux = 0.0f;
  }
  return (uint32_t)(lux + 0.5f);
}

int ltr329als_read_lux(struct ltr329als_sample *out) {
  uint16_t ch0 = 0, ch1 = 0;
  ltr329als_gain active_gain;
  int ret = ltr329als_read(&ch0, &ch1, &active_gain);

  if (ret == -EAGAIN) {
    return -EAGAIN;
  }
  if (ret == -ERANGE) {
    ltr329als_autogain(ret, 0);
    return -ENODATA;
  }
  if (ret != 0) {
    return ret;
  }
  if (ltr329als_autogain(ret, ch0)) {
    return -ENODATA;
  }

  out->ch0  = ch0;
  out->ch1  = ch1;
  out->gain = ltr329als_gain_factor(active_gain);
  out->lux  = ltr329als_compute_lux(ch0, ch1, active_gain, s_integ_time);
  return 0;
}

ltr329als_gain ltr329als_current_gain(void) {
  return k_gain_seq[s_gain_idx];
}

bool ltr329als_autogain(int read_ret, uint16_t ch0) {
  if (read_ret == -ERANGE) {
    if (s_gain_idx > 0) {
      s_low_count = 0;
      apply_gain(k_gain_seq[--s_gain_idx]);
      LOG_INF("Auto-gain down (saturated): step %d", s_gain_idx);
      return true;
    }
    return false;
  }
  if (read_ret != 0) {
    return false;
  }
  if (ch0 > AUTOGAIN_HIGH && s_gain_idx > 0) {
    s_low_count = 0;
    apply_gain(k_gain_seq[--s_gain_idx]);
    LOG_INF("Auto-gain down (high): step %d", s_gain_idx);
    return true;
  }
  if (ch0 < AUTOGAIN_LOW) {
    if (++s_low_count >= AUTOGAIN_LOW_CONSEC &&
        s_gain_idx < (int)GAIN_SEQ_LEN - 1) {
      s_low_count = 0;
      apply_gain(k_gain_seq[++s_gain_idx]);
      LOG_INF("Auto-gain up: step %d", s_gain_idx);
      return true;
    }
  } else {
    s_low_count = 0;
  }
  return false;
}

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

  s_integ_time = cfg->integ_time;

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


