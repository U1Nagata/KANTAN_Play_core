// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#include "internal_bmi270.hpp"

#include <M5Unified.h>

namespace kanplay_ns {
//-------------------------------------------------------------------------

static constexpr const uint32_t i2c_freq = 400000;
static constexpr const uint8_t i2c_addr = 0x68;

static bool writeRegister8(uint8_t reg, uint8_t data)
{
  return M5.In_I2C.writeRegister8(i2c_addr, reg, data, i2c_freq);
}

static bool writeRegister(uint8_t reg, const uint8_t* data, size_t length)
{
  return M5.In_I2C.writeRegister(i2c_addr, reg, data, length, i2c_freq);
}

static bool readRegister(uint8_t reg, uint8_t* result, size_t length)
{
  return M5.In_I2C.readRegister(i2c_addr, reg, result, length, i2c_freq);
}

static uint8_t readRegister8(uint8_t reg)
{
  return M5.In_I2C.readRegister8(i2c_addr, reg, i2c_freq);
}

static void bulk_write(const uint8_t* reg_data)
{
  while (*reg_data) {
    uint8_t len = *reg_data++;
    int retry = 16;
    while (!writeRegister(reg_data[0], &reg_data[1], len - 1) && --retry) { M5.delay(1); }
    reg_data += len;
  }
}

static constexpr const uint8_t INT_STATUS_1_ADDR = 0x1D;
static constexpr const uint8_t AUX_X_LSB_ADDR    = 0x04;
static constexpr const uint8_t ACC_X_LSB_ADDR    = 0x0C;
static constexpr const uint8_t GYR_X_LSB_ADDR    = 0x12;
static constexpr const uint8_t FIFO_LENGTH_0     = 0x24;
static constexpr const uint8_t FIFO_LENGTH_1     = 0x25;
static constexpr const uint8_t FIFO_DATA         = 0x26;

bool internal_bmi270_t::init(void)
{
  const uint8_t init_config[] = {
    2, 0x7D, 0x06,  // PWR_CTRL : enable accel and gyro
    2, 0x6B, 0x00,  // IF_CONF : disable AUX
    // 2, 0x40, 0x76,  // ACC_CONF 25Hz / avg128 / power optimized.
    // 2, 0x40, 0x86,  // ACC_CONF   25Hz / no filter / performance opt.
    // 2, 0x40, 0xAB,  // ACC_CONF  800Hz / avg 4 filter / performance opt.
    // 2, 0x40, 0x8B,  // ACC_CONF  800Hz / no filter / performance opt.
    // 2, 0x40, 0x8C,  // ACC_CONF  1600Hz / OSR4 filter / performance opt.
    // 2, 0x40, 0xAC,  // ACC_CONF  1600Hz / avg 4 filter / performance opt.
    // 2, 0x40, 0x7C,  // ACC_CONF  1600Hz / avg128 / power optimized.
    // 2, 0x40, 0x3C,  // ACC_CONF 1600Hz / avg 8 filter / power optimized.
    // 2, 0x40, 0x2C,  // ACC_CONF 1600Hz / avg 4 filter / power optimized.
    // 2, 0x40, 0x0C,  // ACC_CONF 1600Hz / no avg filter / power optimized.
    // 2, 0x40, 0xAA,  // ACC_CONF  400Hz / normal mode / performance opt.
    2, 0x40, 0xAB,  // ACC_CONF  800Hz / normal mode / performance opt.
    // 2, 0x40, 0x8B,  // ACC_CONF  800Hz / OSR4 mode / performance opt.
    // 2, 0x40, 0xAC,  // ACC_CONF 1600Hz / normal mode / performance opt.
    2, 0x42, 0xA8,  // GYRO_CONF : 100Hz, filtered performance mode
    2, 0x43, 0x02,  // GYR_RANGE : +/-500 degrees/sec

    // 2, 0x45, 0x00,  // FIFO_DOWNS / ダウンサンプリングなし
    2, 0x45, 0x88,  // FIFO_DOWNS 

    2, 0x48, 0x02,  // FIFO_CONFIG_0 : don't stop writing to FIFO when full

    2, 0x7C, 0x03,  // PWR_CONF

    0 // sentinel
  };
  bulk_write(init_config);

  // FIFO_CONFIG_1 : ヘッダ無効設定か否かによってheader_enビットの有無を変更する aux_dis,acc_EN,gyr_dis
  {
    uint8_t fifo_config_1 = 0x40;
    if (_fifo_header_enable) { fifo_config_1 |= 0x10; }
    writeRegister8(0x49, fifo_config_1); // FIFO_CONFIG_1
  }

  return true;
}

bool internal_bmi270_t::readGyro(imu_3d_t* gyro)
{
  if (!gyro) { return false; }
  uint8_t data[6];
  if (!readRegister(GYR_X_LSB_ADDR, data, sizeof(data))) { return false; }
  gyro->x = (int16_t)(data[0] | (data[1] << 8));
  gyro->y = (int16_t)(data[2] | (data[3] << 8));
  gyro->z = (int16_t)(data[4] | (data[5] << 8));
  return true;
}

void internal_bmi270_t::clearFifo(void)
{
  writeRegister8(0x7E, 0xB0); // FIFO_FLUSH
}

// internal_bmi270_t::sensor_mask_t internal_bmi270_t::update(void)
uint32_t internal_bmi270_t::update(void)
{
  uint32_t result = 0;
  uint16_t len = 0;
  if (readRegister(FIFO_LENGTH_0, (uint8_t*)&len, 2)) {
    // M5_LOGE("FIFO_LEN: %d", len);
    if (len) {
      uint8_t buffer[256];
      if (len > sizeof(buffer)) { len = sizeof(buffer); }
      readRegister(FIFO_DATA, buffer, len);
      auto b = buffer;
      auto b_end = b + len;

      if (_fifo_header_enable)
      {
        while (b < b_end) {
          uint8_t header = *b++;
          uint8_t fh_mode = header >> 6;
          uint8_t fh_parm = (header >> 2) & 0x0F;
          switch (fh_mode) {
          case 0b01: // Control frame
            switch (fh_parm) {
            case 0b000: // skip frame
              b++;
              break;
            case 0b001: // Sensortime Frame
              b += 3;
              break;
            case 0b010: // Fifo_Input_Config Frame
              b += 4;
              break;
            default:
              M5_LOGE("IMU FIFO Control frame: %02X", header);
              b++;
            }
            break;

          case 0b10: // Regular frame
            if (fh_parm & 0x04) { // aux data
              b += 8;
              M5_LOGE("IMU AUX");
            }
            if (fh_parm & 0x02) { // gyro data
              b += 6;
              M5_LOGE("IMU Gyro");
            }
            if (fh_parm & 0x01) { // accel data
              int fifo_idx = _accel_fifo_index + 1;
              if (fifo_idx >= fifo_size) { fifo_idx = 0; }
              auto& accel = _accel_fifo[fifo_idx];
              accel.x = (int16_t)(b[0] | (b[1] << 8));
              accel.y = (int16_t)(b[2] | (b[3] << 8));
              accel.z = (int16_t)(b[4] | (b[5] << 8));
              b += 6;
              _accel_fifo_index = fifo_idx;
              ++result;
            }
          }
        }
      } else {
        // non header mode
        while (b < b_end) {
          int fifo_idx = _accel_fifo_index + 1;
          if (fifo_idx >= fifo_size) { fifo_idx = 0; }
          auto& accel = _accel_fifo[fifo_idx];
          accel.x = (int16_t)(b[0] | (b[1] << 8));
          accel.y = (int16_t)(b[2] | (b[3] << 8));
          accel.z = (int16_t)(b[4] | (b[5] << 8));
          b += 6;
          _accel_fifo_index = fifo_idx;
          ++result;
        }
      }
      // M5_LOGE("FIFO_LEN: %d  %d %d %d", len, _accel_fifo[_accel_fifo_index].x, _accel_fifo[_accel_fifo_index].y, _accel_fifo[_accel_fifo_index].z);
      // return sensor_mask_accel;
    }
  }
  return result;
  // return sensor_mask_none;
}

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

