// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#include "internal_es8388.hpp"

#include <M5Unified.h>

#define KANPLAY_I2S_32BIT
// #define KANPLAY_I2S_MASTER_ESP

namespace kanplay_ns {
//-------------------------------------------------------------------------

static constexpr const uint32_t i2c_freq = 400000;
static constexpr const uint8_t i2c_addr = 0x10;

static bool writeRegister8(uint8_t reg, uint8_t data)
{
  return M5.In_I2C.writeRegister8(i2c_addr, reg, data, i2c_freq);
}

/*
static bool writeRegister(uint8_t reg, const uint8_t* data, size_t length)
{
  return M5.In_I2C.writeRegister(i2c_addr, reg, data, length, i2c_freq);
}

static bool readRegister(uint8_t reg, uint8_t* result, size_t length)
{
  return M5.In_I2C.readRegister(i2c_addr, reg, result, length, i2c_freq);
}
//*/

static void bulk_write(const uint8_t* reg_data, size_t size) {
  for (int i = 0; i < size; i += 2) {
// M5.delay(500);
// M5_LOGE("ES8388 %d reg%d:%02x", i>>1, reg_data[i], reg_data[i+1]);
    int retry = 30;
    do {
      // M5.delay(1);
    } while (!writeRegister8( reg_data[i], reg_data[i+1]) && --retry);
    if (retry < 20) {
      M5_LOGE("ES8388 write error: %d reg%d:%02x: retry:%d", i>>1, reg_data[i], reg_data[i+1], retry);
    }
  }
}
void internal_es8388_t::init(void)
{
  static constexpr const uint8_t es8388_reg_data_set[] = {
     0, 0x80, // reset register
     0, 0x00,
     0, 0x00,

     8, 0x00, //set I2S slave mode (ES8388)
    43, 0x80, //ADC and DAC have the same LRCK
     0, 0x35, //ADC Fs == DAC Fs and DACMCLK  ／  VMID 5kΩ
     1, 0x40,
     7, 0x7C, //VSEL

    // マイクアンプは0で良い
     9, 0x00, // 7:4MicAmpL / 3:0MicAmpR (取り込み時の音量に影響,値が大きい＝音が大きい
    10, 0x00, //ADC Ctrl 2  LINPUT1/RINPUT1
    11, 0x02, //ADC Ctrl 3  デフォルト値のまま
#if defined ( KANPLAY_I2S_32BIT )
    12, 0x10, //I2S format (ADC側 32bit) (REG 23と共通に)
    23, 0x20, //I2S format (DAC側 32bit serial) (REG 12と共通に)
#else
    12, 0x0C, //I2S format (ADC側 16bit) (REG 23と共通に)
    23, 0x18, //I2S format (DAC側 16bit serial) (REG 12と共通に)
#endif
    // 23, 0x00, //I2S format (DAC側 24bit serial) (REG 12と共通に)
    // 12, 0x00, //I2S format (ADC側 24bit) (REG 23と共通に)
    13, 0x20, // double speed, div128 (REG 24と共通に)
    // 13, 0x30, //I2S MCLK ratio (ADC側) double speed, div125 (REG 24と共通に)
    // 13, 0x20, //I2S MCLK ratio (ADC側) double speed, div128 (REG 24と共通に)
    // 13, 0x38, //I2S MCLK ratio (ADC側) double speed, div1000 (REG 24と共通に)
    // 13, 0x35, //I2S MCLK ratio (ADC側) double speed, div500 (REG 24と共通に)
    14, 0x30, //ADC Ctrl 6  デフォルト値のまま
    // 15, 0x00, //ADC Ctrl 7  ADC soft ramp off
    16, 0x00, //ADC left volume 0x00~0xC0 値が小さい = 音が大きい
    17, 0x00, //ADC right volume 0x00~0xC0 値が小さい = 音が大きい

    18, 0x38, //ALC off
    22, 0x00, //ADC Ctrl 14 noise gate
    24, 0x00,
/*
    18, 0xF8, //ALC stereo / MAX 0b111 / MIN 0b000
    19, 0x30, //ALC level 0b0011 / ALCHLD 0b0000
    20, 0xA6, //ALC DCY 0b1010 / ALCATK 0b0110
    21, 0x06, //ALC MODE 0 / ALCZC off / TIMEOUT 0 / WIN_SIZE 0b00110
    22, 0x59, //NGTH 0b01011 / NGG 00 / NGAT 1(enabled)
//*/
    // 22, 0xF3, //ADC Ctrl 14 noise gate
    // 24, 0x30, //I2S MCLK ratio (DAC側) double speed, div125 (REG 13と共通に)
    // 24, 0x20, //I2S MCLK ratio (DAC側) double speed, div128 (REG 13と共通に)
    // 24, 0x35, //I2S MCLK ratio (DAC側) double speed, div500 (REG 13と共通に)
    // 24, 0x38, //I2S MCLK ratio (DAC側) double speed, div1000 (REG 13と共通に)
    // 25, 0x20, //DAC mute解除
    28, 0xC8, //enable digital click free power up and down
    29, 0x06,
    45, 0x00, // 0x00=1.5k VREF analog output / 0x10=40kVREF analog output
    //  2, 0x00, //CHIPPOWER: power up all
    //  3, 0x00, //ADCPOWER: power up all
    //  4, 0x30, //DACPOWER: power up and LOUT1/ROUT1 enable
  };
  bulk_write(es8388_reg_data_set, sizeof(es8388_reg_data_set));
}

void internal_es8388_t::mute() {
  static constexpr const uint8_t es8388_reg_data_set[] = {
  15, 0x34, //ADC mute
  25, 0x36, //DAC mute
  26, 0xC0, //LDACVOL/ I2Sの音量に影響,パススルー音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)
  27, 0xC0, //RDACVOL/ I2Sの音量に影響,パススルー音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)
  49, 0x00, //ROUT2VOL OFF
  48, 0x00, //LOUT2VOL OFF
  47, 0x00, //ROUT1VOL OFF
  46, 0x00, //LOUT1VOL OFF
  42, 0x38, //右チャネルのミキサ設定
  39, 0x38, //左チャネルのミキサ設定
  // 26, 0xC0, //LDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が大きいほど音が小さい)
  // 27, 0xC0, //RDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が大きいほど音が小さい)
  //  4, 0x00, //DACPOWER: output disable
  //  5, 0xE8, //ChipLowPower1 (ビットを立てるとlow powerになるらしいが動作に差は感じない)
  //  6, 0xC3, //ChipLowPower2 (ビットを立てるとlow powerになるらしいが動作に差は感じない)

     2, 0xF3, //CHIPPOWER: power down all
     3, 0xFC, //ADCPOWER: power up all
     4, 0xC0, //DACPOWER: power up and LOUT1/ROUT1/LOUT2/ROUT2 enable
  };
  bulk_write(es8388_reg_data_set, sizeof(es8388_reg_data_set));
}

void internal_es8388_t::unmute() {
  static constexpr const uint8_t es8388_reg_data_set[] = {
     5, 0x00, //ChipLowPower1
     6, 0x00, //ChipLowPower2
 // 26, 0x00, //LDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)
 // 27, 0x00, //RDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)
    26, 0x00, //LDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)
    27, 0x00, //RDACVOL/ I2Sの音量に影響,SAMの音には影響なし 0x00~0xC0 /数字が小さいほど音が大きい)

// ※46,47,48,49の値はsetOutVolumeで設定されるようになったのでここでは指定しない
    // OUT1 = イヤホンの音量
    // 46, 0x21, //0x1E == 0dB.  0x21が最大ボリューム 4.5dB, //LOUT1VOL/ 両方の音量に影響、数字が大きいほど音が大きい (I2Sの音がある時のみボリューム増幅される。SAMの音量も影響を受けるので扱いに注意が必要)
    // 47, 0x21, //0x1E == 0dB.  0x21が最大ボリューム 4.5dB, //ROUT1VOL/ 両方の音量に影響、数字が大きいほど音が大きい (I2Sの音がある時のみボリューム増幅される。SAMの音量も影響を受けるので扱いに注意が必要)
    // OUT2 = スピーカーの音量
    // 48, 0x21, //0x1E == 0dB.  0x21が最大ボリューム, //LOUT2VOL/ 両方の音量に影響、数字が大きいほど音が大きい (I2Sの音がある時のみボリューム増幅される。SAMの音量も影響を受けるので扱いに注意が必要)
    // 49, 0x21, //0x1E == 0dB.  0x21が最大ボリューム, //ROUT2VOL/ 両方の音量に影響、数字が大きいほど音が大きい (I2Sの音がある時のみボリューム増幅される。SAMの音量も影響を受けるので扱いに注意が必要)
     3, 0x00, //ADCPOWER: power up all
     4, 0x3C, //DACPOWER: power up and LOUT1/ROUT1/LOUT2/ROUT2 enable
    // 25, 0xE8, //DAC mute解除 : volume soft ramp
    15, 0x30, //ADC mute解除
    25, 0x32, //DAC mute解除
     2, 0x00, //CHIPPOWER: power up all
    42, 0xB8, //右チャネルのミキサ設定
    39, 0xB8, //左チャネルのミキサ設定

#if !defined ( KANPLAY_I2S_MASTER_ESP )
     8, 0x80, //set I2S master mode (ES8388)
#endif
  };
  bulk_write(es8388_reg_data_set, sizeof(es8388_reg_data_set));
}

void internal_es8388_t::setOutVolume(uint8_t volume)
{
  if (volume > 33) {
    volume = 33;
  }
  _out_volume = volume;
  uint8_t reg_data_set[] = {
  46, volume,
  47, volume,
  48, volume,
  49, volume,
  };
  bulk_write(reg_data_set, sizeof(reg_data_set));
}

void internal_es8388_t::setInVolume(uint8_t volume)
{
  if (volume > 15) {
    volume = 15;
  }
  _in_volume = volume;
  volume = volume * 0x11;
  uint8_t reg_data_set[] = {
  9, volume,
  };
  bulk_write(reg_data_set, sizeof(reg_data_set));
}

void internal_es8388_t::setExternalInput(bool enabled)
{
  // LINSEL/RINSEL: 00=INPUT1, 01=INPUT2.
  writeRegister8(10, enabled ? 0x50 : 0x00);
}

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

