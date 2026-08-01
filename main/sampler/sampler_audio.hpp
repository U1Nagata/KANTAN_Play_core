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
  // 12 Pad + background loop + menu preview + pitched Pad synth (8 voices)
  static constexpr const size_t max_voice = 22;
  static constexpr const uint32_t sample_rate = 48000;

  bool start(void);

  // モノラルPCM16を指定ボイスで再生開始 (再生中なら先頭から再トリガ)
  // pcm はプール(PSRAM)上に常駐しているデータであること
  static bool play(uint8_t voice, const int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                   bool loop = false, bool reverse = false, uint16_t volume_q8 = 256,
                   uint16_t pitch_q8 = 256, uint32_t start_frame = 0);
  // Melody/Chord用。保持音は短いAttack/Releaseを通し、必要なら
  // 検出済みの安定区間をsustain loopする。通常Pad経路とは分離する。
  static bool playSynth(uint8_t voice, const int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                        bool sustain_loop, bool reverse, uint16_t volume_q8,
                        uint16_t pitch_q8, uint16_t attack_ms = 5, uint16_t release_ms = 12,
                        uint32_t sustain_start = 0, uint32_t sustain_end = 0,
                        uint16_t sustain_crossfade = 0, uint16_t auto_release_ms = 0,
                        bool linear_interpolation = true);
  static void release(uint8_t voice);
  static void stop(uint8_t voice);
  static void stopAll(void);
  // Queue a position correction for the audio task. Unlike play(), this does
  // not reconfigure the voice from the UI task while it is being mixed.
  static void seek(uint8_t voice, uint32_t frame);
  static bool isPlaying(uint8_t voice);
  // Runtime mixer gain for an already playing voice. The audio task slews to
  // the target over a few milliseconds to prevent clicks.
  static void setVoiceVolumeQ8(uint8_t voice, uint16_t volume_q8);
  // Runtime pitch scale for an already playing voice. 4096 is neutral; this
  // lets the Melody lever bend Pad sounds without retriggering their attack.
  static void setVoicePitchScaleQ12(uint8_t voice, uint16_t scale_q12);
  // Per-voice tape speed. 256 is normal forward playback, zero holds the
  // current frame, and negative values run the PCM backwards.
  static void setVoicePlaybackRateQ8(uint8_t voice, int16_t rate_q8);
  // A lightweight per-voice low-pass/resonance pair for Touch Play. Unlike
  // the global FX Filter, this never changes the BGM or other Pad voices.
  static void setVoiceToneFilter(uint8_t voice, uint8_t cutoff, uint8_t resonance);
  // UI用の軽量な再生位置。frameはplay()へ渡したPCM範囲の先頭からのフレーム数。
  static bool getPlaybackPosition(uint8_t voice, uint32_t* frame, uint32_t* frames);
  static void setOutputMuted(bool muted);
  // Keep the physical output silent while the codec/I2S path and saved kit
  // are brought up, then raise it slowly from silence.  This is intentionally
  // separate from the short fades used by recording.
  static void releaseStartupMute(void);
  // ES8388のアナログ出力は上限を超えず、後段のリミッターで保護する。
  static void setOutputGainPercent(uint8_t percent);

  // FX。index: 0=Pitch, 1=Filter。Repeatはsampler_app側のLOOPイベント処理。
  static void setFx(uint8_t index, bool active, int8_t param);
  static void setFxActive(uint8_t index, bool active);
  static void setFxParam(uint8_t index, int8_t param);
  // Speed FX is smoothed by sampler_app.  Supplying its instantaneous ratio
  // directly keeps all active voices in lockstep with the loop transport.
  static void setFxSpeedRatioQ8(uint16_t ratio_q8);
  static void setFxQuantizeStepMs(uint32_t step_ms);

  // I2S入力(マイク/ライン)をPCM16 monoで呼び出し元バッファへ録音する。
  // buffer は stopRecording() まで有効であること。
  static bool startRecording(int16_t* buffer, uint32_t capacity_frames, uint32_t initial_frames = 0);
  static uint32_t stopRecording(void);
  static uint32_t recordingFrames(void);
  static bool isRecording(void);
  static bool recordingOverflowed(void);

  // Capture the final mixed mono output for a one-shot WAV export. The audio
  // task owns the write cursor so the UI task can start/stop this safely.
  static bool startOutputCapture(int16_t* buffer, uint32_t capacity_frames);
  static uint32_t stopOutputCapture(void);
  static uint32_t outputCaptureFrames(void);

private:
  static void task_func(sampler_audio_t* me);
};

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
