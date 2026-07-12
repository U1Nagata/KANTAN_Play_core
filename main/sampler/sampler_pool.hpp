// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_POOL_HPP
#define KANTAN_SAMPLER_POOL_HPP

#include <stdint.h>
#include <stddef.h>

#include "sampler_define.hpp"

namespace sampler_ns {
//-------------------------------------------------------------------------
// PSRAM上のサンプルプール管理
//
// 設計方針 (レスポンス最優先):
//   - 演奏中に使う全サンプルを PSRAM に常駐させ、再生時に SD へアクセスしない
//   - 内部フォーマットは 16bit / mono。録音は 44.1kHz、インポートは元レート維持(〜48kHz)
//     (再生エンジンが線形補間で 48kHz 出力へ変換するためレート混在可)
//   - プール上限 6MB ≒ 44.1kHz mono で合計約68秒

struct sample_slot_t {
  static constexpr const uint8_t waveform_bins = 96;
  char name[24] = { 0 };
  char file_path[80] = { 0 };  // Kit保存用のSD上WAVパス。録音直後など未保存PCMは空。
  int16_t* pcm = nullptr;   // PSRAM上のモノラルPCM (未使用時 nullptr)
  uint32_t frames = 0;
  uint32_t sample_rate = 44100;
  uint32_t start_frame = 0;
  uint32_t end_frame = 0;    // 0ならサンプル末尾
  uint16_t volume_q8 = 256;  // 256 = 100%
  uint16_t pitch_q8 = 256;   // 256 = 100%, 128 = 50%, 512 = 200%
  bool reverse = false;
  bool hold_enabled = false;
  bool loop_enabled = false;
  // 登録時に作る縮小波形。Pad再描画時のPCM全走査を避ける。
  int16_t waveform_min[waveform_bins] = { 0 };
  int16_t waveform_max[waveform_bins] = { 0 };

  bool isValid(void) const { return pcm != nullptr && frames != 0; }
  size_t bytes(void) const { return (size_t)frames * sizeof(int16_t); }
  float seconds(void) const { return sample_rate ? (float)frames / sample_rate : 0.0f; }
  uint32_t playStart(void) const { return start_frame < frames ? start_frame : 0; }
  uint32_t playEnd(void) const { return (end_frame > playStart() && end_frame <= frames) ? end_frame : frames; }
  uint32_t playFrames(void) const { return playEnd() - playStart(); }
};

class sampler_pool_t {
public:
  static constexpr const size_t pool_budget_bytes = 6 * 1024 * 1024;
  static constexpr const uint32_t max_sample_sec = 16;  // Loop上限16秒 (OneShot推奨は10秒)

  static sample_slot_t slot[def::pad::pad_count];

  static size_t usedBytes(void);
  static size_t freeBytes(void);

  // WAVデータ(PCM16 mono/stereo 〜48kHz)をモノラル変換してスロットへ登録
  // 上限秒数・プール残量に収まらない場合は末尾を切り詰める
  static bool loadWav(uint8_t index, const char* display_name, const uint8_t* wav_data, size_t wav_size);

  // PCM16 monoをスロットへ登録する。録音済みバッファや編集結果の登録に使う。
  static bool loadPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate);

  // スロットを解放する (呼び出し前に該当ボイスを停止しておくこと)
  static void erase(uint8_t index);
};

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
