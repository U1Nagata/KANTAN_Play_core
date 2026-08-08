// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_INTERNAL_BMI270_HPP
#define KANPLAY_INTERNAL_BMI270_HPP

#include <inttypes.h>
#include <stddef.h>

namespace kanplay_ns {
//-------------------------------------------------------------------------
class internal_bmi270_t {
public:
  static constexpr const size_t fifo_size = 32;
  struct imu_3d_t {
    int16_t x;
    int16_t y;
    int16_t z;
  };
  bool init(void);
  enum sensor_mask_t : uint8_t {
    sensor_mask_none = 0,
    sensor_mask_accel = 2,
    sensor_mask_gyro = 1,
  };
  uint32_t update(void);
  bool readGyro(imu_3d_t* gyro);
  // sensor_mask_t update(void);
  const imu_3d_t& getAccel(uint8_t index) const { return _accel_fifo[(_accel_fifo_index - index) & (fifo_size - 1)]; };
  const imu_3d_t& getGyro(uint8_t index) const { return _gyro_fifo[(_gyro_fifo_index - index) & (fifo_size - 1)]; };
  void clearFifo(void);

private:
  imu_3d_t _accel_fifo[fifo_size];
  imu_3d_t _gyro_fifo[fifo_size];
  uint8_t _accel_fifo_index;
  uint8_t _gyro_fifo_index;
  bool _fifo_header_enable = false;
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
