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
//   - 内部フォーマットは 16bit / mono。44.1kHzインポートは48kHzへ事前変換し、
//     通常ピッチ時の再生補間を省略する。録音など他レートは元レートを維持する。
//   - プール上限 5MB。BGM・Wi-Fi/TLS・Webサーバー用にPSRAMの余白を残す

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
  uint8_t base_note = 60;    // C3。外部MIDI Pad音源の基準ノート
  bool base_note_auto = true;
  bool synth_sustain_auto = false;
  uint8_t synth_sustain_confidence = 0;
  uint32_t synth_loop_start = 0;  // Melody/Chord用。slot先頭からのフレーム
  uint32_t synth_loop_end = 0;
  uint16_t synth_loop_crossfade = 0;
  bool reverse = false;
  bool hold_enabled = false;
  bool loop_enabled = false;
  uint8_t loop_grid_half_steps = 8;  // Note Grid x 4.0 (0.5 step units)
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
  static constexpr const size_t pool_budget_bytes = 5 * 1024 * 1024;
  static constexpr const uint32_t max_sample_sec = 16;  // Loop上限16秒 (OneShot推奨は10秒)

  static sample_slot_t slot[def::pad::pad_count];

  static size_t usedBytes(void);
  static size_t freeBytes(void);

  // WAVデータ(PCM16 mono/stereo 〜48kHz)をモノラル変換してスロットへ登録
  // 上限秒数・プール残量に収まらない場合は末尾を切り詰める
  static bool loadWav(uint8_t index, const char* display_name, const uint8_t* wav_data, size_t wav_size);

  // PCM16 monoをスロットへ登録する。録音済みバッファや編集結果の登録に使う。
  static bool loadPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate);

  // マイク録音用。通常のインポート素材より少し大きい基準へ正規化するが、
  // 多重発音とマスター・リミッターのため十分なヘッドルームは残す。
  static bool loadRecordedPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate);

  // PSRAM上に確保済みのPCM16 monoをスロットへ登録し、所有権を引き取る。
  // 成功時のみ pcm_data をプールが解放する。失敗時は呼び出し元が解放すること。
  static bool loadPcmOwned(uint8_t index, const char* display_name, int16_t* pcm_data, uint32_t frames, uint32_t sample_rate);

  // 現在のStart/End範囲から基音を推定する。手動設定の保護は呼び出し側で判断する。
  static void analyzeBaseNote(uint8_t index);
  // 安定した伸ばし音だけをMelody/Chord用サステイン候補にする。
  // 読み込み・録音終了・トリム確定時にのみ呼び出す。
  static void analyzeSynthSustain(uint8_t index);

  // スロットを解放する (呼び出し前に該当ボイスを停止しておくこと)
  static void erase(uint8_t index);
};

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
