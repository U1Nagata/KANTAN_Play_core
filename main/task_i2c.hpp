// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_TASK_I2C_HPP
#define KANPLAY_TASK_I2C_HPP

/*
task_spi は 内部I2Cを使用するタスクです。
 - M5.update() (タッチパネル・電源IC制御)
 - 下記かんぷれハードウェアとの通信
   - STM32  (かんぷれ本体制御)
   - ES8388 (I2S ADC/DAC)
   - Si5351 (ES8388のMCLK生成)
   - BMI270 (IMU 加速度ジャイロセンサ)
*/

namespace kanplay_ns {
//-------------------------------------------------------------------------
class task_i2c_t {
public:
    bool start(void);
    void requestAudioCodecRestore(void);
    bool audioCodecRestorePending(void) const;
private:
    static void task_func(task_i2c_t* me);
    volatile bool _audio_codec_restore_pending = false;
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
