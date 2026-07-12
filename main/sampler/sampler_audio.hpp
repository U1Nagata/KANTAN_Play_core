// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_AUDIO_HPP
#define KANTAN_SAMPLER_AUDIO_HPP

#include <stdint.h>
#include <stddef.h>

namespace sampler_ns {
//-------------------------------------------------------------------------
// サンプル再生エンジン
// KANTAN Play base のオーディオ経路 (SI5351 → ES8388(I2Sマスター) → アンプ) を
// task_i2s.cpp と同一の I2S スレーブ設定で使用し、入力(SAM音源/マイク)の
// パススルーに対して PCM16 WAV のボイスをミキシングして出力する。
// 実効サンプルレートは 48kHz (MCLK 6.144MHz / 128)。
class sampler_audio_t {
public:
  static constexpr const size_t max_voice = 14;  // 12 Pad + background loop + menu preview
  static constexpr const uint32_t sample_rate = 48000;

  bool start(void);

  // モノラルPCM16を指定ボイスで再生開始 (再生中なら先頭から再トリガ)
  // pcm はプール(PSRAM)上に常駐しているデータであること
  static bool play(uint8_t voice, const int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                   bool loop = false, bool reverse = false, uint16_t volume_q8 = 256,
                   uint16_t pitch_q8 = 256, uint32_t start_frame = 0);
  static void stop(uint8_t voice);
  static void stopAll(void);
  static bool isPlaying(uint8_t voice);
  static void setOutputMuted(bool muted);

  // FX。index: 0=Pitch, 1=Filter。Repeatはsampler_app側のLOOPイベント処理。
  static void setFx(uint8_t index, bool active, int8_t param);
  static void setFxActive(uint8_t index, bool active);
  static void setFxParam(uint8_t index, int8_t param);
  static void setFxQuantizeStepMs(uint32_t step_ms);

  // I2S入力(マイク/ライン)をPCM16 monoで呼び出し元バッファへ録音する。
  // buffer は stopRecording() まで有効であること。
  static bool startRecording(int16_t* buffer, uint32_t capacity_frames, uint32_t initial_frames = 0);
  static uint32_t stopRecording(void);
  static uint32_t recordingFrames(void);
  static bool isRecording(void);
  static bool recordingOverflowed(void);

private:
  static void task_func(sampler_audio_t* me);
};

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
