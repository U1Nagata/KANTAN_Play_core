// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

// サンプル再生エンジン (KANPLAY_SAMPLER ビルド時のみ有効)
// I2S 初期化部は main/task_i2s.cpp と同一構成 (ES8388がI2Sマスター、48kHz/32bit/stereo)

#if defined (KANPLAY_SAMPLER)

#include <M5Unified.h>

#include "sampler_audio.hpp"

#include "../common_define.hpp"
#include "../system_registry.hpp"

#if !defined (M5UNIFIED_PC_BUILD)

#if __has_include(<driver/i2s_std.h>)
 #include <driver/i2s_std.h>
#else
 #include <driver/i2s.h>
#endif

#include <esp_heap_caps.h>

#endif

namespace sampler_ns {
namespace kp = kanplay_ns;
//-------------------------------------------------------------------------

static constexpr const uint16_t i2s_dma_frame_num = 96;
static constexpr const uint32_t output_sample_rate = sampler_audio_t::sample_rate;

//-------------------------------------------------------------------------
// ボイス管理

struct voice_t {
  const int16_t* pcm = nullptr;  // モノラルPCMデータ先頭
  uint32_t frames = 0;           // 総フレーム数
  int64_t pos_fp = 0;            // 再生位置 (16.16固定小数、フレーム単位)
  volatile uint32_t frame_for_ui = 0;  // UI参照用の現在フレーム (PCM範囲内)
  volatile uint32_t seek_frame = 0;
  volatile bool seek_pending = false;
  uint32_t seek_target_frame = 0;
  uint16_t seek_fade_remaining = 0;
  uint8_t seek_fade_state = 0;  // 0=idle, 1=fade out, 2=fade in
  uint32_t nominal_step_fp = 0;  // Voice固有の音程。演奏中Pitch Bendの基準。
  uint32_t base_step_fp = 0;     // FXなしの再生ステップ
  uint32_t step_fp = 0;          // 現在の再生ステップ
  volatile int16_t playback_rate_q8 = 256;  // テープ速度。負値は逆方向。
  volatile uint8_t tone_cutoff = 127;
  volatile uint8_t tone_resonance = 0;
  int32_t tone_filter_1 = 0;
  int32_t tone_filter_2 = 0;
  bool loop = false;
  uint32_t loop_start_frame = 0;
  uint32_t loop_end_frame = 0;
  uint16_t loop_crossfade_frames = 0;
  const int16_t* attack_cache_pcm = nullptr;
  uint32_t attack_cache_frames = 0;
  const int16_t* sustain_cache_pcm = nullptr;
  uint32_t sustain_cache_frames = 0;
  uint8_t sustain_cache_stride = 1;
  uint8_t sustain_cache_slot = 0xFF;
  bool reverse = false;
  uint16_t volume_q8 = 256;
  volatile uint16_t target_volume_q8 = 256;
  uint16_t pitch_q8 = 256;
  // Chord Pad synthesis can run four sustained, pitch-shifted PCM voices at
  // once. Nearest-neighbour reads keep that expressive mode within the I2S
  // deadline; one-note Melody/Bass retains the higher-quality interpolation.
  bool linear_interpolation = true;
  // A divider of two renders a Pad-sourced chord at 24kHz internally while
  // retaining the 48kHz output envelope. This halves PSRAM reads for the four
  // simultaneous chord voices without affecting one-note instruments.
  uint8_t render_divider = 1;
  uint8_t render_phase = 0;
  int32_t render_sample = 0;
  bool render_sample_valid = false;
  uint16_t envelope_q15 = 32768;
  uint16_t attack_step_q15 = 0;
  uint16_t release_step_q15 = 0;
  uint32_t auto_release_frames = 0;
  // Set by the input/loop task and consumed by the I2S task. This must remain
  // observable on every audio frame; otherwise a dense Pad-sourced chord can
  // keep a cached false value after its physical Note Off.
  volatile bool release_requested = false;
  volatile bool active = false;
};

static voice_t voices[sampler_audio_t::max_voice];
// Most performance frames use only a small subset of the 22 available
// voices. Keep a lock-free active set so the I2S callback never burns time
// checking every dormant slot. A 32-bit mask is atomic on ESP32-S3.
static volatile uint32_t active_voice_mask = 0;

static inline void activate_voice(uint8_t voice)
{
  __atomic_fetch_or(&active_voice_mask, 1u << voice, __ATOMIC_RELEASE);
}

static inline void deactivate_voice(uint8_t voice)
{
  __atomic_fetch_and(&active_voice_mask, ~(1u << voice), __ATOMIC_RELEASE);
}

// Melody, Chord and Bass each get one internal-RAM source cache. A chord's
// four voices share its attack and sustain windows rather than repeatedly
// reading the same PSRAM data during dense loop recording.
static constexpr uint8_t synth_sustain_cache_count = 6;
static constexpr uint32_t synth_sustain_cache_max_frames = 8192;
static constexpr uint32_t synth_attack_cache_max_frames = 4096;
struct synth_sustain_cache_t {
  int16_t* pcm = nullptr;
  int16_t* attack_pcm = nullptr;
  const int16_t* source = nullptr;
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t frames = 0;
  uint32_t attack_frames = 0;
  uint32_t capacity = 0;
  uint32_t attack_capacity = 0;
  uint8_t stride = 1;
};
static synth_sustain_cache_t synth_sustain_cache[synth_sustain_cache_count];

static void reset_synth_sustain_cache_entry(synth_sustain_cache_t& entry)
{
  entry.source = nullptr;
  entry.start = 0;
  entry.end = 0;
  entry.frames = 0;
  entry.attack_frames = 0;
  entry.stride = 1;
}

static inline int16_t voice_pcm_at(const voice_t& v, uint32_t index)
{
  if (v.attack_cache_pcm && index < v.attack_cache_frames) {
    return v.attack_cache_pcm[index];
  }
  if (v.sustain_cache_pcm && index >= v.loop_start_frame && index < v.loop_end_frame) {
    const uint32_t cache_index = (index - v.loop_start_frame) / v.sustain_cache_stride;
    if (cache_index < v.sustain_cache_frames) { return v.sustain_cache_pcm[cache_index]; }
  }
  const uint32_t sample_index = v.reverse ? (v.frames - 1 - index) : index;
  return v.pcm[sample_index];
}
// Never expose codec/I2S startup transients.  sampler_app releases this only
// after restoring the kit and selecting the input route.
static volatile bool output_muted = true;
static volatile uint16_t output_fade_target_q15 = 0;
static volatile uint16_t output_fade_step_q15 = 32768;
static uint16_t output_fade_q15 = 0;
static volatile uint16_t output_gain_q8 = 256;
// The UI/input task raises this around a physical performance hit.  It is
// deliberately a single volatile flag: the I2S task must not take a lock or
// allocate just to make room for a new attack.
static volatile bool performance_priority = false;
// The mixer owns a 1ms I2S block. Keep a compact estimate so optional work
// such as live visualisation and future motion/Delay FX can yield before they
// threaten an audible deadline.
static volatile uint8_t processing_load_q8 = 0;
static volatile uint8_t processing_peak_q8 = 0;
static volatile bool live_wave_capture_enabled = false;
static volatile bool live_wave_clear_pending = false;

struct fx_state_t {
  volatile bool active = false;
  volatile int8_t param = 0;
};

static fx_state_t fx[3];
static volatile uint16_t fx_speed_ratio_q8 = 256;
static int32_t filter_l = 0;
static int32_t filter_r = 0;
static int32_t limiter_gain_q15 = 32768;

// Tape Stop intentionally keeps no rolling history while idle. Once pressed,
// it starts capturing the final mix and reads that short-lived stream at a
// progressively slower rate. The normal transport and all voices continue
// advancing underneath, ready to rejoin on release.
static constexpr uint32_t tape_stop_ramp_frames = output_sample_rate * 220u / 1000u;
static constexpr uint32_t tape_stop_release_frames = output_sample_rate * 12u / 1000u;
static constexpr uint32_t tape_stop_buffer_frames = tape_stop_ramp_frames + 384u;
enum class tape_stop_state_t : uint8_t { idle, slowing, stopped, releasing };
struct tape_stop_t {
  volatile bool requested = false;
  int16_t* pcm = nullptr;  // interleaved stereo, allocated once but idle when unused
  uint32_t capacity = 0;
  uint32_t write_frames = 0;
  uint32_t read_fp = 0;
  uint32_t elapsed_frames = 0;
  uint32_t release_frames = 0;
  tape_stop_state_t state = tape_stop_state_t::idle;
};
static tape_stop_t tape_stop;

struct recorder_t {
  volatile bool active = false;
  volatile bool overflow = false;
  int16_t* buffer = nullptr;
  volatile uint32_t frames = 0;
  uint32_t capacity = 0;
};

static recorder_t recorder;
struct output_capture_t {
  volatile bool active = false;
  int16_t* buffer = nullptr;
  uint32_t capacity = 0;
  volatile uint32_t frames = 0;
};
static output_capture_t output_capture;
struct output_stream_capture_t {
  volatile bool active = false;
  volatile bool overflow = false;
  int16_t* buffer = nullptr;
  uint32_t capacity = 0;
  volatile uint32_t write_pos = 0;
  volatile uint32_t read_pos = 0;
};
static output_stream_capture_t output_stream_capture;

static inline uint32_t pitch_step_fp(uint32_t base_step)
{
  if (!fx[0].active || fx_speed_ratio_q8 == 256) { return base_step; }
  return (uint32_t)(((uint64_t)base_step * fx_speed_ratio_q8) >> 8);
}

static void update_voice_steps(void)
{
  for (auto& voice : voices) {
    if (voice.active) { voice.step_fp = pitch_step_fp(voice.base_step_fp); }
  }
}

bool sampler_audio_t::play(uint8_t voice, const int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                           bool loop, bool reverse, uint16_t volume_q8, uint16_t pitch_q8, uint32_t start_frame)
{
  if (voice >= max_voice || pcm == nullptr || frames == 0 || sample_rate == 0) { return false; }

  auto& v = voices[voice];
  deactivate_voice(voice);
  v.active = false;  // 再生中の再トリガに備え一旦停止してから書き換える
  v.pcm = pcm;
  v.frames = frames;
  // Sample Edit remains 50-200%, while pitched instrument voices may request
  // up to +/-2 octaves around that value.
  if (pitch_q8 < 32) { pitch_q8 = 32; }
  if (pitch_q8 > 2048) { pitch_q8 = 2048; }
  v.nominal_step_fp = (uint32_t)((((uint64_t)sample_rate << 16) * pitch_q8) / ((uint64_t)output_sample_rate << 8));
  v.base_step_fp = v.nominal_step_fp;
  v.step_fp = pitch_step_fp(v.base_step_fp);
  v.playback_rate_q8 = 256;
  v.tone_cutoff = 127;
  v.tone_resonance = 0;
  v.tone_filter_1 = 0;
  v.tone_filter_2 = 0;
  if (start_frame >= frames) { start_frame = 0; }
  v.pos_fp = (int64_t)start_frame << 16;
  v.frame_for_ui = start_frame;
  v.seek_pending = false;
  v.seek_fade_state = 0;
  v.seek_fade_remaining = 0;
  v.loop = loop;
  v.loop_start_frame = 0;
  v.loop_end_frame = frames;
  v.loop_crossfade_frames = 0;
  v.attack_cache_pcm = nullptr;
  v.attack_cache_frames = 0;
  v.sustain_cache_pcm = nullptr;
  v.sustain_cache_frames = 0;
  v.sustain_cache_stride = 1;
  v.sustain_cache_slot = 0xFF;
  v.reverse = reverse;
  v.volume_q8 = volume_q8;
  v.target_volume_q8 = volume_q8;
  v.pitch_q8 = pitch_q8;
  v.linear_interpolation = true;
  v.render_divider = 1;
  v.render_phase = 0;
  v.render_sample = 0;
  v.render_sample_valid = false;
  v.envelope_q15 = 32768;
  v.attack_step_q15 = 0;
  v.release_step_q15 = 0;
  v.auto_release_frames = 0;
  v.release_requested = false;
  v.active = true;
  activate_voice(voice);
  return true;
}

void sampler_audio_t::primeSynthSustainCache(uint8_t cache_slot, const int16_t* pcm,
                                             uint32_t sustain_start, uint32_t sustain_end,
                                             uint32_t attack_cache_limit,
                                             uint32_t sustain_cache_limit)
{
  if (cache_slot >= synth_sustain_cache_count) { return; }
  auto& cache = synth_sustain_cache[cache_slot];
  attack_cache_limit = std::clamp<uint32_t>(attack_cache_limit, 32, synth_attack_cache_max_frames);
  sustain_cache_limit = std::clamp<uint32_t>(sustain_cache_limit, 32, synth_sustain_cache_max_frames);
  if (!pcm || sustain_end <= sustain_start || sustain_end - sustain_start < 32) {
    reset_synth_sustain_cache_entry(cache);
    return;
  }
  if (cache.source == pcm && cache.start == sustain_start && cache.end == sustain_end
   && (cache.attack_frames != 0 || cache.frames != 0)) {
    return;
  }
  if (!cache.attack_pcm) {
#if defined (M5UNIFIED_PC_BUILD)
    cache.attack_pcm = (int16_t*)malloc(attack_cache_limit * sizeof(int16_t));
#else
    cache.attack_pcm = (int16_t*)heap_caps_malloc(attack_cache_limit * sizeof(int16_t),
                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    cache.attack_capacity = cache.attack_pcm ? attack_cache_limit : 0;
  }
  if (!cache.attack_pcm) {
    reset_synth_sustain_cache_entry(cache);
    return;
  }
  cache.attack_frames = std::min<uint32_t>(sustain_start, cache.attack_capacity);
  for (uint32_t i = 0; i < cache.attack_frames; ++i) { cache.attack_pcm[i] = pcm[i]; }
  cache.source = pcm;
  cache.start = sustain_start;
  cache.end = sustain_end;
  cache.frames = 0;
  cache.stride = 1;
  const uint32_t loop_frames = sustain_end - sustain_start;
  // A synth loop needs sample-accurate material. Downsampling it into the
  // cache turns the cached source into a sample-and-hold waveform, which is
  // especially audible as a metallic tone at Loop In/Out. Cache only an
  // entire loop at its original resolution; longer loops stay in PSRAM.
  if (loop_frames > sustain_cache_limit) {
    return;
  }
  if (!cache.pcm) {
#if defined (M5UNIFIED_PC_BUILD)
    cache.pcm = (int16_t*)malloc(sustain_cache_limit * sizeof(int16_t));
#else
    cache.pcm = (int16_t*)heap_caps_malloc(sustain_cache_limit * sizeof(int16_t),
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    cache.capacity = cache.pcm ? sustain_cache_limit : 0;
  }
  if (!cache.pcm) {
    return;
  }
  if (loop_frames > cache.capacity) {
    return;
  }
  for (uint32_t i = 0; i < loop_frames; ++i) {
    cache.pcm[i] = pcm[sustain_start + i];
  }
  cache.frames = loop_frames;
}

void sampler_audio_t::clearSynthSustainCache(uint8_t cache_slot)
{
  const uint8_t first = cache_slot < synth_sustain_cache_count ? cache_slot : 0;
  const uint8_t last = cache_slot < synth_sustain_cache_count
    ? (uint8_t)(cache_slot + 1) : synth_sustain_cache_count;
  for (uint8_t i = first; i < last; ++i) { reset_synth_sustain_cache_entry(synth_sustain_cache[i]); }
}

bool sampler_audio_t::isSynthSustainCacheInUse(uint8_t cache_slot)
{
  if (cache_slot >= synth_sustain_cache_count) { return false; }
  for (const auto& voice : voices) {
    if (voice.active && voice.sustain_cache_slot == cache_slot) { return true; }
  }
  return false;
}

bool sampler_audio_t::playSynth(uint8_t voice, const int16_t* pcm, uint32_t frames,
                                uint32_t sample_rate, bool sustain_loop, bool reverse,
                                uint16_t volume_q8, uint16_t pitch_q8,
                                uint16_t attack_ms, uint16_t release_ms,
                                uint32_t sustain_start, uint32_t sustain_end,
                                uint16_t sustain_crossfade, uint16_t auto_release_ms,
                                bool linear_interpolation, uint8_t render_divider,
                                uint8_t sustain_cache_slot)
{
  if (!play(voice, pcm, frames, sample_rate, sustain_loop, reverse,
            volume_q8, pitch_q8, 0)) {
    return false;
  }
  auto& v = voices[voice];
  v.linear_interpolation = linear_interpolation;
  v.render_divider = std::clamp<uint8_t>(render_divider, 1, 2);
  v.render_phase = 0;
  v.render_sample_valid = false;
  if (sustain_loop && !reverse && sustain_end > sustain_start
   && sustain_end <= frames && sustain_end - sustain_start >= 32) {
    v.loop_start_frame = sustain_start;
    v.loop_end_frame = sustain_end;
    v.loop_crossfade_frames = (uint16_t)std::min<uint32_t>(
      sustain_crossfade, (sustain_end - sustain_start) / 4);
    if (sustain_cache_slot < synth_sustain_cache_count) {
      const auto& cache = synth_sustain_cache[sustain_cache_slot];
      if (cache.source == pcm && cache.start == sustain_start && cache.end == sustain_end
       && (cache.attack_frames != 0 || cache.frames != 0)) {
        if (cache.attack_frames != 0 && cache.attack_pcm) {
          v.attack_cache_pcm = cache.attack_pcm;
          v.attack_cache_frames = cache.attack_frames;
          v.sustain_cache_slot = sustain_cache_slot;
        }
        if (cache.frames != 0 && cache.pcm) {
          v.sustain_cache_pcm = cache.pcm;
          v.sustain_cache_frames = cache.frames;
          v.sustain_cache_stride = cache.stride;
          v.sustain_cache_slot = sustain_cache_slot;
        }
      }
    }
  }
  const uint32_t attack_frames = std::max<uint32_t>(1, (output_sample_rate * attack_ms) / 1000);
  const uint32_t release_frames = std::max<uint32_t>(1, (output_sample_rate * release_ms) / 1000);
  v.envelope_q15 = attack_ms == 0 ? 32768 : 0;
  v.attack_step_q15 = attack_ms == 0 ? 0
    : (uint16_t)std::max<uint32_t>(1, (32768u + attack_frames - 1) / attack_frames);
  v.release_step_q15 = release_ms == 0 ? 32768
    : (uint16_t)std::max<uint32_t>(1, (32768u + release_frames - 1) / release_frames);
  v.auto_release_frames = (output_sample_rate * (uint32_t)auto_release_ms) / 1000u;
  return true;
}

void sampler_audio_t::release(uint8_t voice)
{
  if (voice < max_voice && voices[voice].active) {
    voices[voice].release_requested = true;
  }
}

void sampler_audio_t::stop(uint8_t voice)
{
  if (voice < max_voice) {
    deactivate_voice(voice);
    voices[voice].active = false;
  }
}

void sampler_audio_t::stopAll(void)
{
  __atomic_store_n(&active_voice_mask, 0, __ATOMIC_RELEASE);
  for (auto& v : voices) { v.active = false; }
}

void sampler_audio_t::seek(uint8_t voice, uint32_t frame)
{
  if (voice >= max_voice || !voices[voice].active) { return; }
  auto& v = voices[voice];
  v.seek_frame = frame;
  v.seek_pending = true;
}

bool sampler_audio_t::isPlaying(uint8_t voice)
{
  return (voice < max_voice) && voices[voice].active;
}

void sampler_audio_t::setVoiceVolumeQ8(uint8_t voice, uint16_t volume_q8)
{
  if (voice >= max_voice) { return; }
  voices[voice].target_volume_q8 = std::min<uint16_t>(volume_q8, 1024);
}

void sampler_audio_t::setVoicePitchScaleQ12(uint8_t voice, uint16_t scale_q12)
{
  if (voice >= max_voice) { return; }
  if (scale_q12 < 2048) { scale_q12 = 2048; }
  if (scale_q12 > 8192) { scale_q12 = 8192; }
  auto& v = voices[voice];
  v.base_step_fp = (uint32_t)(((uint64_t)v.nominal_step_fp * scale_q12) >> 12);
  v.step_fp = pitch_step_fp(v.base_step_fp);
}

void sampler_audio_t::setVoicePlaybackRateQ8(uint8_t voice, int16_t rate_q8)
{
  if (voice >= max_voice) { return; }
  if (rate_q8 < -512) { rate_q8 = -512; }
  if (rate_q8 > 512) { rate_q8 = 512; }
  voices[voice].playback_rate_q8 = rate_q8;
}

void sampler_audio_t::setVoiceToneFilter(uint8_t voice, uint8_t cutoff, uint8_t resonance)
{
  if (voice >= max_voice) { return; }
  voices[voice].tone_cutoff = std::min<uint8_t>(cutoff, 127);
  voices[voice].tone_resonance = std::min<uint8_t>(resonance, 127);
}

void sampler_audio_t::setPerformancePriority(bool active)
{
  performance_priority = active;
}

uint8_t sampler_audio_t::processingLoadQ8(void)
{
  return processing_load_q8;
}

uint8_t sampler_audio_t::processingPeakQ8(void)
{
  return processing_peak_q8;
}

void sampler_audio_t::setLiveWaveCapture(bool enabled)
{
  if (live_wave_capture_enabled == enabled) { return; }
  live_wave_capture_enabled = enabled;
  // Let the I2S task clear its own shared ring before the next live scope.
  // This avoids showing stale output from the preceding Loop/Edit page.
  if (enabled) { live_wave_clear_pending = true; }
}

bool sampler_audio_t::getPlaybackPosition(uint8_t voice, uint32_t* frame, uint32_t* frames)
{
  if (voice >= max_voice || !voices[voice].active) { return false; }
  if (frame != nullptr) { *frame = voices[voice].frame_for_ui; }
  if (frames != nullptr) { *frames = voices[voice].frames; }
  return true;
}

void sampler_audio_t::setOutputMuted(bool muted)
{
  output_muted = muted;
  output_fade_target_q15 = muted ? 0 : 32768;
  // Recording changes must remain responsive, but unmuting still must not
  // create an edge at the DAC.  10ms is inaudible in normal operation.
  output_fade_step_q15 = muted ? 32768
    : (uint16_t)((32768 + output_sample_rate / 100 - 1) / (output_sample_rate / 100));
  if (muted) { output_fade_q15 = 0; }
}

void sampler_audio_t::releaseStartupMute(void)
{
  output_muted = false;
  output_fade_q15 = 0;
  output_fade_target_q15 = 32768;
  // One second from digital silence to the saved master level.  The first
  // part of this ramp is the safe initial output level, while the existing
  // master-volume slew and limiter continue to protect the final level.
  output_fade_step_q15 = 1;
}

void sampler_audio_t::setOutputGainPercent(uint8_t percent)
{
  if (percent < 100) { percent = 100; }
  if (percent > 200) { percent = 200; }
  output_gain_q8 = (uint16_t)(((uint32_t)percent << 8) / 100);
}

static int8_t clamp_fx_param(int value)
{
  if (value < -100) { value = -100; }
  if (value > 100) { value = 100; }
  return (int8_t)value;
}

void sampler_audio_t::setFx(uint8_t index, bool active, int8_t param)
{
  if (index >= 3) { return; }
  fx[index].param = clamp_fx_param(param);
  fx[index].active = active;
  if (index == 0 && !active) { fx_speed_ratio_q8 = 256; }
  if (index == 0) { update_voice_steps(); }
}

void sampler_audio_t::setFxActive(uint8_t index, bool active)
{
  if (index < 3) {
    fx[index].active = active;
    if (index == 0 && !active) { fx_speed_ratio_q8 = 256; }
    if (index == 0) { update_voice_steps(); }
  }
}

void sampler_audio_t::setFxParam(uint8_t index, int8_t param)
{
  if (index >= 3) { return; }
  fx[index].param = clamp_fx_param(param);
  if (index == 0) { update_voice_steps(); }
}

void sampler_audio_t::setFxSpeedRatioQ8(uint16_t ratio_q8)
{
  if (ratio_q8 < 128) { ratio_q8 = 128; }
  if (ratio_q8 > 512) { ratio_q8 = 512; }
  fx_speed_ratio_q8 = ratio_q8;
  if (fx[0].active) { update_voice_steps(); }
}

void sampler_audio_t::setFxQuantizeStepMs(uint32_t step_ms)
{
  (void)step_ms;  // Repeatはsampler_app側のLOOPイベント再生で処理する。
}

void sampler_audio_t::setTapeStop(bool active)
{
  tape_stop.requested = active && tape_stop.pcm != nullptr;
}

bool sampler_audio_t::tapeStopAvailable(void)
{
  return tape_stop.pcm != nullptr;
}

bool sampler_audio_t::startRecording(int16_t* buffer, uint32_t capacity_frames, uint32_t initial_frames)
{
  if (buffer == nullptr || capacity_frames == 0 || initial_frames >= capacity_frames) { return false; }
  recorder.active = false;
  recorder.buffer = buffer;
  recorder.capacity = capacity_frames;
  recorder.frames = initial_frames;
  recorder.overflow = false;
  recorder.active = true;
  return true;
}

uint32_t sampler_audio_t::stopRecording(void)
{
  recorder.active = false;
  return recorder.frames;
}

uint32_t sampler_audio_t::recordingFrames(void)
{
  return recorder.frames;
}

bool sampler_audio_t::isRecording(void)
{
  return recorder.active;
}

bool sampler_audio_t::recordingOverflowed(void)
{
  return recorder.overflow;
}

bool sampler_audio_t::startOutputCapture(int16_t* buffer, uint32_t capacity_frames)
{
  if (!buffer || capacity_frames == 0 || output_stream_capture.active) { return false; }
  output_capture.active = false;
  output_capture.buffer = buffer;
  output_capture.capacity = capacity_frames;
  output_capture.frames = 0;
  output_capture.active = true;
  return true;
}

uint32_t sampler_audio_t::stopOutputCapture(void)
{
  output_capture.active = false;
  return output_capture.frames;
}

uint32_t sampler_audio_t::outputCaptureFrames(void)
{
  return output_capture.frames;
}

bool sampler_audio_t::startOutputStreamCapture(int16_t* ring, uint32_t capacity_frames)
{
  // Keep one empty frame to distinguish a full ring from an empty one.
  if (!ring || capacity_frames < 2 || output_capture.active) { return false; }
  output_stream_capture.active = false;
  output_stream_capture.buffer = ring;
  output_stream_capture.capacity = capacity_frames;
  output_stream_capture.write_pos = 0;
  output_stream_capture.read_pos = 0;
  output_stream_capture.overflow = false;
  output_stream_capture.active = true;
  return true;
}

void sampler_audio_t::stopOutputStreamCapture(void)
{
  output_stream_capture.active = false;
}

uint32_t sampler_audio_t::readOutputStreamCapture(int16_t* dst, uint32_t max_frames)
{
  if (!dst || max_frames == 0 || !output_stream_capture.buffer
   || output_stream_capture.capacity < 2) { return 0; }
  const uint32_t capacity = output_stream_capture.capacity;
  const uint32_t read = output_stream_capture.read_pos;
  const uint32_t write = output_stream_capture.write_pos;
  uint32_t available = write >= read ? write - read : capacity - read + write;
  if (available > max_frames) { available = max_frames; }
  if (available == 0) { return 0; }
  const uint32_t first = std::min<uint32_t>(available, capacity - read);
  memcpy(dst, output_stream_capture.buffer + read, first * sizeof(int16_t));
  if (available > first) {
    memcpy(dst + first, output_stream_capture.buffer, (available - first) * sizeof(int16_t));
  }
  output_stream_capture.read_pos = (read + available) % capacity;
  return available;
}

bool sampler_audio_t::outputStreamCaptureActive(void)
{
  return output_stream_capture.active;
}

bool sampler_audio_t::outputStreamCaptureOverflowed(void)
{
  return output_stream_capture.overflow;
}

static inline void capture_output_frame(int32_t left, int32_t right)
{
  if ((!output_capture.active || output_capture.frames >= output_capture.capacity)
   && !output_stream_capture.active) { return; }
  int64_t mono = ((int64_t)left + right) / 2;
  mono >>= 16;
  if (mono > INT16_MAX) { mono = INT16_MAX; }
  if (mono < INT16_MIN) { mono = INT16_MIN; }
  if (output_capture.active && output_capture.frames < output_capture.capacity) {
    output_capture.buffer[output_capture.frames++] = (int16_t)mono;
    if (output_capture.frames >= output_capture.capacity) { output_capture.active = false; }
  }

  if (!output_stream_capture.active || !output_stream_capture.buffer
   || output_stream_capture.capacity < 2) { return; }
  const uint32_t write = output_stream_capture.write_pos;
  const uint32_t next = (write + 1u) % output_stream_capture.capacity;
  if (next == output_stream_capture.read_pos) {
    // I2S must never wait for SD. Flag the incomplete take and drop only the
    // newest frame; the writer will report failure when the transport stops.
    output_stream_capture.overflow = true;
    return;
  }
  output_stream_capture.buffer[write] = (int16_t)mono;
  output_stream_capture.write_pos = next;
}

// 1フレーム分のボイス合成値を求めて加算する (16bit値を32bitフルスケールに拡張して加算)
static inline int64_t mix_voices(void)
{
  int64_t mixed = 0;
  uint32_t active = __atomic_load_n(&active_voice_mask, __ATOMIC_ACQUIRE);
  while (active) {
    const uint8_t n = (uint8_t)__builtin_ctz(active);
    active &= active - 1;
    auto& v = voices[n];
    if (!v.active) {
      deactivate_voice(n);
      continue;
    }
    // Seeking a running loop at a non-zero crossing can click. Fade only this
    // voice for two milliseconds, move its position while silent, then fade
    // it back in. The request itself still comes from the UI without touching
    // playback fields concurrently.
    static constexpr uint16_t seek_fade_frames = 96;  // 2ms at 48kHz
    if (v.seek_pending && v.seek_fade_state == 0) {
      v.seek_target_frame = v.seek_frame;
      v.seek_pending = false;
      v.seek_fade_remaining = seek_fade_frames;
      v.seek_fade_state = 1;
    }
    uint16_t seek_gain_q15 = 32768;
    if (v.seek_fade_state == 1) {
      seek_gain_q15 = (uint16_t)(((uint32_t)v.seek_fade_remaining << 15) / seek_fade_frames);
      if (--v.seek_fade_remaining == 0) {
        uint32_t target = v.seek_target_frame;
        if (target >= v.frames) { target = 0; }
        v.pos_fp = (int64_t)target << 16;
        v.frame_for_ui = target;
        v.seek_fade_remaining = seek_fade_frames;
        v.seek_fade_state = 2;
      }
    } else if (v.seek_fade_state == 2) {
      seek_gain_q15 = (uint16_t)(((uint32_t)(seek_fade_frames - v.seek_fade_remaining) << 15) / seek_fade_frames);
      if (--v.seek_fade_remaining == 0) { v.seek_fade_state = 0; }
    }
    // The sustain body must preserve the source resolution as well. A prior
    // performance-priority shortcut raised this divider to four while a new
    // note arrived. That sample-and-hold path made a held synth note acquire
    // a metallic texture exactly on the next Note On/Off. RAM source caches
    // and UI throttling provide the headroom without degrading audio here.
    const uint8_t divider = v.render_divider;
    const bool render_now = !v.render_sample_valid || divider == 1 || v.render_phase == 0;
    int32_t s = v.render_sample;
    if (render_now) {
      const uint32_t loop_end = v.loop_end_frame ? v.loop_end_frame : v.frames;
      const int64_t loop_start_fp = (int64_t)v.loop_start_frame << 16;
      const int64_t loop_end_fp = (int64_t)loop_end << 16;
      // Forward sustain playback must pass through the attack once. Being
      // before Loop In is valid until the first arrival at Loop Out. The lower
      // boundary is only a wrap condition while tape/scratch playback moves
      // backwards.
      const bool crossed_loop_end = v.playback_rate_q8 >= 0 && v.pos_fp >= loop_end_fp;
      const bool crossed_loop_start = v.playback_rate_q8 < 0 && v.pos_fp < loop_start_fp;
      if (crossed_loop_end || crossed_loop_start) {
        if (v.loop && loop_end > v.loop_start_frame) {
          if (v.pos_fp >= loop_end_fp) {
            uint32_t restart = v.loop_start_frame + v.loop_crossfade_frames;
            if (restart >= loop_end) { restart = v.loop_start_frame; }
            const int64_t restart_fp = (int64_t)restart << 16;
            const int64_t span_fp = ((int64_t)loop_end - restart) << 16;
            const int64_t over_fp = v.pos_fp - loop_end_fp;
            v.pos_fp = restart_fp + (span_fp ? over_fp % span_fp : 0);
          } else {
            const int64_t span_fp = loop_end_fp - loop_start_fp;
            const int64_t under_fp = loop_start_fp - v.pos_fp;
            const int64_t remainder = span_fp ? under_fp % span_fp : 0;
            v.pos_fp = remainder == 0 ? loop_start_fp : loop_end_fp - remainder;
          }
        } else {
          v.active = false;
          deactivate_voice(n);
          continue;
        }
      }
      uint32_t idx = (uint32_t)(v.pos_fp >> 16);
      v.frame_for_ui = idx;
      uint32_t frac = v.pos_fp & 0xFFFF;
      s = voice_pcm_at(v, idx);
      if (frac != 0 && v.linear_interpolation) {
        uint32_t idx1 = idx + 1;
        if (v.reverse) {
          idx1 = idx1 >= v.frames ? (v.loop ? 0 : idx) : idx1;
        } else {
          idx1 = idx1 >= loop_end && v.loop
            ? std::min<uint32_t>(v.loop_start_frame + v.loop_crossfade_frames, loop_end - 1)
            : (idx1 >= v.frames ? idx : idx1);
        }
        const int32_t s1 = voice_pcm_at(v, idx1);
        s += ((s1 - s) * (int32_t)frac) >> 16;
      }
      if (!v.reverse && v.playback_rate_q8 >= 0 && v.loop_crossfade_frames
       && idx >= loop_end - v.loop_crossfade_frames && idx < loop_end) {
        uint32_t offset = idx - (loop_end - v.loop_crossfade_frames);
        uint32_t head_idx = v.loop_start_frame + offset;
        if (head_idx < v.frames) {
          const int32_t head = voice_pcm_at(v, head_idx);
          const uint32_t mix_q15 = (offset << 15) / v.loop_crossfade_frames;
          s = (int32_t)(((int64_t)s * (32768 - mix_q15) + (int64_t)head * mix_q15) >> 15);
        }
      }
      v.render_sample = s;
      v.render_sample_valid = true;
    }
    if (v.volume_q8 < v.target_volume_q8) { ++v.volume_q8; }
    else if (v.volume_q8 > v.target_volume_q8) { --v.volume_q8; }
    if (v.volume_q8 != 256) { s = (int32_t)(((int64_t)s * v.volume_q8) >> 8); }
    if (seek_gain_q15 != 32768) { s = (int32_t)(((int64_t)s * seek_gain_q15) >> 15); }
    if (!v.release_requested && v.auto_release_frames != 0
     && --v.auto_release_frames == 0) {
      v.release_requested = true;
    }
    if (v.release_requested) {
      if (v.envelope_q15 <= v.release_step_q15) {
        v.envelope_q15 = 0;
        v.active = false;
        deactivate_voice(n);
        continue;
      }
      v.envelope_q15 -= v.release_step_q15;
    } else if (v.envelope_q15 < 32768) {
      uint32_t next = v.envelope_q15 + v.attack_step_q15;
      v.envelope_q15 = (uint16_t)std::min<uint32_t>(32768, next);
    }
    if (v.envelope_q15 != 32768) {
      s = (int32_t)(((int64_t)s * v.envelope_q15) >> 15);
    }
    if (v.tone_cutoff < 127 || v.tone_resonance != 0) {
      // Two cheap one-pole stages make a stable low-pass. The difference
      // between stages is the band around its cutoff; adding a little of it
      // back gives Touch Play a musical resonance without affecting any
      // other voice or requiring a biquad per playing Pad.
      // The Touch Play surface needs a clearly audible left-to-right sweep.
      // A near-zero alpha at the far left makes the low-pass genuinely dark;
      // 127 still resolves to 256, which is a transparent bypass.
      const int alpha = 4 + ((int)v.tone_cutoff * 252) / 127;
      v.tone_filter_1 += ((s - v.tone_filter_1) * alpha) >> 8;
      v.tone_filter_2 += ((v.tone_filter_1 - v.tone_filter_2) * alpha) >> 8;
      const int32_t band = v.tone_filter_1 - v.tone_filter_2;
      // At the right side of Touch Play, give the selected high band enough
      // gain to be heard as a distinct bright/resonant state. The limiter
      // after the mixer remains the final guard for several simultaneous pads.
      s = v.tone_filter_2 + (band * v.tone_resonance * 3) / 256;
    }
    mixed += (int64_t)s << 16;
    if (++v.render_phase >= divider) { v.render_phase = 0; }
    if (render_now) {
      v.pos_fp += (((int64_t)v.step_fp * v.playback_rate_q8) >> 8) * divider;
    }
  }
  return mixed;
}

static inline int32_t saturate32(int64_t value)
{
  if (value > INT32_MAX) { return INT32_MAX; }
  if (value < INT32_MIN) { return INT32_MIN; }
  return (int32_t)value;
}

static inline int64_t abs64_limit(int64_t value)
{
  if (value == INT64_MIN) { return INT64_MAX; }
  return value < 0 ? -value : value;
}

static inline void process_output_limiter(int64_t& l, int64_t& r)
{
  static constexpr const int64_t threshold = (int64_t)INT32_MAX / 4 * 3;  // 約75%。多重発音時の余裕を確保する。
  int64_t peak_l = abs64_limit(l);
  int64_t peak_r = abs64_limit(r);
  int64_t peak = peak_l > peak_r ? peak_l : peak_r;
  // A dense Pad chord can remain above the raw threshold for thousands of
  // samples. The former code performed a 64-bit division for every one of
  // those samples even when the existing limiter gain was already protecting
  // the output. First test that protected peak; the expensive exact division
  // now runs only on a genuine new overload, while recovery still happens at
  // the same gentle rate.
  // The overwhelmingly common one-shot path is already at unity gain and
  // below the threshold. Avoid even the 64-bit gain check there; this keeps
  // the inexpensive case inexpensive while the overload path below remains
  // sample-accurate.
  const bool unity_gain = limiter_gain_q15 == 32768;
  const int64_t protected_peak = unity_gain ? peak : (peak * limiter_gain_q15) >> 15;
  if (protected_peak > threshold) {
    int32_t target_gain_q15 = (int32_t)((threshold << 15) / peak);
    if (target_gain_q15 < 8192) { target_gain_q15 = 8192; }
    if (target_gain_q15 < limiter_gain_q15) {
      limiter_gain_q15 = target_gain_q15;  // attack: immediate
    }
  } else if (limiter_gain_q15 < 32768) {
    const int32_t diff = 32768 - limiter_gain_q15;
    limiter_gain_q15 += diff > 1024 ? diff >> 10 : 1;  // release: about 20ms
  }

  if (limiter_gain_q15 < 32768) {
    l = (l * limiter_gain_q15) >> 15;
    r = (r * limiter_gain_q15) >> 15;
  }
}

static inline int16_t tape_stop_pcm16(int64_t value)
{
  value >>= 16;
  if (value > INT16_MAX) { return INT16_MAX; }
  if (value < INT16_MIN) { return INT16_MIN; }
  return (int16_t)value;
}

static inline int32_t tape_stop_pcm32(int16_t value)
{
  return (int32_t)value << 16;
}

static inline void process_tape_stop(int64_t& l, int64_t& r)
{
  if (tape_stop.pcm == nullptr || tape_stop.capacity == 0) { return; }
  if (tape_stop.requested && (tape_stop.state == tape_stop_state_t::idle
                           || tape_stop.state == tape_stop_state_t::releasing)) {
    tape_stop.state = tape_stop_state_t::slowing;
    tape_stop.write_frames = 0;
    tape_stop.read_fp = 0;
    tape_stop.elapsed_frames = 0;
    tape_stop.release_frames = 0;
  } else if (!tape_stop.requested && tape_stop.state != tape_stop_state_t::idle
             && tape_stop.state != tape_stop_state_t::releasing) {
    tape_stop.state = tape_stop_state_t::releasing;
    tape_stop.release_frames = 0;
  }

  if (tape_stop.state == tape_stop_state_t::idle) { return; }
  if (tape_stop.state == tape_stop_state_t::releasing) {
    const uint32_t progress = std::min<uint32_t>(tape_stop.release_frames++, tape_stop_release_frames);
    const int32_t dry_q15 = (int32_t)((progress * 32768u) / tape_stop_release_frames);
    l = (l * dry_q15) >> 15;
    r = (r * dry_q15) >> 15;
    if (progress >= tape_stop_release_frames) { tape_stop.state = tape_stop_state_t::idle; }
    return;
  }
  if (tape_stop.state == tape_stop_state_t::stopped) {
    l = 0;
    r = 0;
    return;
  }

  // Store only frames created after the press. The slower read cursor trails
  // the normal-rate write cursor, so the effect needs no idle history/cache.
  if (tape_stop.write_frames >= tape_stop.capacity) {
    tape_stop.state = tape_stop_state_t::stopped;
    l = 0;
    r = 0;
    return;
  }
  const uint32_t write = tape_stop.write_frames++;
  tape_stop.pcm[write * 2u] = tape_stop_pcm16(l);
  tape_stop.pcm[write * 2u + 1u] = tape_stop_pcm16(r);

  const uint32_t read = std::min<uint32_t>(tape_stop.read_fp >> 16, write);
  const uint32_t next = std::min<uint32_t>(read + 1u, write);
  const uint16_t fraction = (uint16_t)(tape_stop.read_fp & 0xFFFFu);
  const int32_t left0 = tape_stop_pcm32(tape_stop.pcm[read * 2u]);
  const int32_t left1 = tape_stop_pcm32(tape_stop.pcm[next * 2u]);
  const int32_t right0 = tape_stop_pcm32(tape_stop.pcm[read * 2u + 1u]);
  const int32_t right1 = tape_stop_pcm32(tape_stop.pcm[next * 2u + 1u]);
  l = left0 + ((((int64_t)left1 - left0) * fraction) >> 16);
  r = right0 + ((((int64_t)right1 - right0) * fraction) >> 16);

  const uint32_t elapsed = std::min<uint32_t>(tape_stop.elapsed_frames++, tape_stop_ramp_frames);
  const uint32_t remaining = tape_stop_ramp_frames - elapsed;
  const uint32_t rate_q16 = (uint32_t)(((uint64_t)remaining * remaining << 16)
    / ((uint64_t)tape_stop_ramp_frames * tape_stop_ramp_frames));
  tape_stop.read_fp += rate_q16;
  const uint32_t fade_frames = output_sample_rate * 24u / 1000u;
  if (remaining < fade_frames) {
    const int32_t gain_q15 = (int32_t)((remaining * 32768u) / fade_frames);
    l = (l * gain_q15) >> 15;
    r = (r * gain_q15) >> 15;
  }
  if (elapsed >= tape_stop_ramp_frames) { tape_stop.state = tape_stop_state_t::stopped; }
}

static inline void process_master_fx(int64_t& l, int64_t& r)
{
  if (!fx[1].active) { return; }
  const int32_t input_l = saturate32(l);
  const int32_t input_r = saturate32(r);
  if (fx[1].active && fx[1].param != 0) {
    int param = fx[1].param * 2;
    if (param > 100) { param = 100; }
    if (param < -100) { param = -100; }
    int amount = param < 0 ? -param : param;
    // A one-pole high-pass is input minus its low-pass follower.  A faster
    // follower means a higher HP cutoff, so its coefficient must move in the
    // opposite direction from the low-pass control.  The former shared curve
    // made a small positive value cut more than a large one.
    int shift = param < 0
      ? 1 + (amount * 7) / 100        // LP: larger value = lower cutoff
      : 1 + ((100 - amount) * 6) / 100; // HP: larger value = higher cutoff
    filter_l = saturate32((int64_t)filter_l + (((int64_t)input_l - filter_l) >> shift));
    filter_r = saturate32((int64_t)filter_r + (((int64_t)input_r - filter_r) >> shift));
    if (param < 0) {
      // A modest low-shelf lift follows the filtered low band.  362 / 256 is
      // about +3 dB at the extreme, leaving the limiter ample headroom.
      const int low_gain_q8 = 256 + (amount * 106) / 100;
      l = ((int64_t)filter_l * low_gain_q8) >> 8;
      r = ((int64_t)filter_r * low_gain_q8) >> 8;
    } else {
      // Keep the weak end almost dry, then remove the residue as the control
      // rises.  Only the extracted high band receives the same modest shelf
      // lift, so the strong setting cannot become a full-band level increase.
      const int high_gain_q8 = 256 + (amount * 106) / 100;
      const int low_mix_q8 = ((100 - amount) * 64) / 100;
      const int64_t high_l = (int64_t)input_l - filter_l;
      const int64_t high_r = (int64_t)input_r - filter_r;
      l = ((high_l * high_gain_q8) >> 8) + (((int64_t)filter_l * low_mix_q8) >> 8);
      r = ((high_r * high_gain_q8) >> 8) + (((int64_t)filter_r * low_mix_q8) >> 8);
    }
  } else {
    filter_l = input_l;
    filter_r = input_r;
  }

}

static inline int16_t input_to_pcm16(int32_t l, int32_t r)
{
  int32_t mono = ((l >> 16) + (r >> 16)) >> 1;
  if (mono > INT16_MAX) { return INT16_MAX; }
  if (mono < INT16_MIN) { return INT16_MIN; }
  return (int16_t)mono;
}

static inline void record_input_frame(int32_t l, int32_t r)
{
  if (!recorder.active) { return; }
  uint32_t pos = recorder.frames;
  if (pos >= recorder.capacity) {
    recorder.overflow = true;
    recorder.active = false;
    return;
  }
  recorder.buffer[pos] = input_to_pcm16(l, r);
  recorder.frames = pos + 1;
}

// Apply this after the limiter so fade-in cannot alter limiter detection or
// the captured external-input stream.  All output paths share this gate.
static inline uint16_t output_fade_gain_q15(void)
{
  const uint16_t gain = output_fade_q15;
  const uint16_t target = output_fade_target_q15;
  if (gain < target) {
    const uint32_t next = gain + output_fade_step_q15;
    output_fade_q15 = (uint16_t)std::min<uint32_t>(target, next);
  } else if (gain > target) {
    const uint16_t step = output_fade_step_q15;
    output_fade_q15 = gain > step ? std::max<uint16_t>(target, gain - step) : target;
  }
  return gain;
}

static inline int32_t apply_output_fade(int32_t sample, uint16_t gain_q15)
{
  return gain_q15 == 32768 ? sample : (int32_t)(((int64_t)sample * gain_q15) >> 15);
}

//-------------------------------------------------------------------------
// I2S 初期化 (task_i2s.cpp と同一設定)

#if !defined (M5UNIFIED_PC_BUILD)

// CoreS3内蔵マイク(M5.Mic/ES7210)が I2S_NUM_1 を使うため、
// KANTAN base側の出力/パススルーは I2S_NUM_0 で動かす。
static constexpr const i2s_port_t i2s_port = I2S_NUM_0;
static constexpr const uint16_t i2s_dma_desc_num = 4;

#if __has_include(<driver/i2s_std.h>)

static i2s_chan_handle_t _i2s_tx_handle = nullptr;
static i2s_chan_handle_t _i2s_rx_handle = nullptr;
static esp_err_t _i2s_init(void)
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)i2s_port, I2S_ROLE_SLAVE);
  chan_cfg.dma_desc_num = i2s_dma_desc_num;
  chan_cfg.dma_frame_num = i2s_dma_frame_num >> 1;
  esp_err_t err = i2s_new_channel(&chan_cfg, &_i2s_tx_handle, &_i2s_rx_handle);
  if (err != ESP_OK) { return err; }
  i2s_std_config_t i2s_config;
  memset(&i2s_config, 0, sizeof(i2s_std_config_t));
  i2s_config.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
  i2s_config.clk_cfg.sample_rate_hz = 48000; // dummy setting
  i2s_config.slot_cfg.data_bit_width = i2s_data_bit_width_t::I2S_DATA_BIT_WIDTH_32BIT;
  i2s_config.slot_cfg.slot_bit_width = i2s_slot_bit_width_t::I2S_SLOT_BIT_WIDTH_32BIT;
  i2s_config.slot_cfg.slot_mode = i2s_slot_mode_t::I2S_SLOT_MODE_STEREO;
  i2s_config.slot_cfg.slot_mask = i2s_std_slot_mask_t::I2S_STD_SLOT_BOTH;
  i2s_config.slot_cfg.ws_width = 32;
  i2s_config.slot_cfg.ws_pol = false;
  i2s_config.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_1    // For esp32/esp32-s2
  i2s_config.slot_cfg.msb_right = false;
#else
  i2s_config.slot_cfg.left_align = true;
  i2s_config.slot_cfg.big_endian = false;
  i2s_config.slot_cfg.bit_order_lsb = false;
#endif
  i2s_config.gpio_cfg.bclk = kp::def::hw::pin::i2s_bck;
  i2s_config.gpio_cfg.ws   = kp::def::hw::pin::i2s_ws;
  i2s_config.gpio_cfg.dout = kp::def::hw::pin::i2s_out;
  i2s_config.gpio_cfg.mclk = kp::def::hw::pin::i2s_mclk;
  i2s_config.gpio_cfg.din  = kp::def::hw::pin::i2s_in;
  err = i2s_channel_init_std_mode(_i2s_tx_handle, &i2s_config);
  err = i2s_channel_init_std_mode(_i2s_rx_handle, &i2s_config);

  return ESP_OK;
}

static esp_err_t _i2s_start(void) {
  if (_i2s_tx_handle == nullptr) { return ESP_FAIL; }
  return i2s_channel_enable(_i2s_tx_handle) || i2s_channel_enable(_i2s_rx_handle);
}

static esp_err_t _i2s_write(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_channel_write(_i2s_tx_handle, buf, len, result, tick);
}

static esp_err_t _i2s_read(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_channel_read(_i2s_rx_handle, buf, len, result, tick);
}

#else

static esp_err_t _i2s_init(void)
{
    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config_t));
    i2s_config.mode                 = (i2s_mode_t)( I2S_MODE_SLAVE | I2S_MODE_TX | I2S_MODE_RX );
    i2s_config.sample_rate          = 48000; // dummy setting
    i2s_config.bits_per_sample      = i2s_bits_per_sample_t::I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format       = i2s_channel_fmt_t::I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = i2s_comm_format_t::I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_config.tx_desc_auto_clear   = false;
    i2s_config.use_apll             = false;
    i2s_config.fixed_mclk           = 0;
    i2s_config.mclk_multiple        = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_DEFAULT;
    i2s_config.bits_per_chan        = i2s_bits_per_chan_t::I2S_BITS_PER_CHAN_32BIT;
#if I2S_DRIVER_VERSION > 1
    i2s_config.dma_desc_num         = i2s_dma_desc_num;
    i2s_config.dma_frame_num        = i2s_dma_frame_num >> 1;
#else
    i2s_config.dma_buf_count        = i2s_dma_desc_num;
    i2s_config.dma_buf_len          = i2s_dma_frame_num >> 1;
#endif
    esp_err_t err;
    if (ESP_OK != (err = i2s_driver_install(i2s_port, &i2s_config, 0, nullptr)))
    {
      M5_LOGE("i2s_driver_install: %d", err);
      return err;
    }

    i2s_pin_config_t pin_config;
    memset(&pin_config, ~0u, sizeof(i2s_pin_config_t)); /// all pin set to I2S_PIN_NO_CHANGE
    pin_config.bck_io_num     = kp::def::hw::pin::i2s_bck;
    pin_config.ws_io_num      = kp::def::hw::pin::i2s_ws;
    pin_config.data_out_num   = kp::def::hw::pin::i2s_out;
    pin_config.data_in_num    = kp::def::hw::pin::i2s_in;
    pin_config.mck_io_num     = kp::def::hw::pin::i2s_mclk;
    err = i2s_set_pin(i2s_port, &pin_config);
    if (ESP_OK != err)
    {
      M5_LOGE("i2s_set_pin: %d", err);
      return err;
    }

  return ESP_OK;
}

static esp_err_t _i2s_start(void) {
  return i2s_start(i2s_port);
}

static esp_err_t _i2s_write(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_write(i2s_port, buf, len, result, tick);
}

static esp_err_t _i2s_read(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_read(i2s_port, buf, len, result, tick);
}

#endif

#endif

static int32_t* bufdata = nullptr;
static constexpr const size_t buf_size = (i2s_dma_frame_num) * sizeof(int32_t);

bool sampler_audio_t::start(void)
{
  int len = kp::system_registry->raw_wave_length;
  auto wav_buf = kp::system_registry->raw_wave;
  for (int i = 0; i < len; ++i) {
    wav_buf[i] = std::make_pair(128, 128);
  }

  // Reserved memory only; PCM is written solely while Tape Stop is held.
  // PSRAM keeps this short performance effect from competing with I2S/DMA.
  if (tape_stop.pcm == nullptr) {
    const size_t bytes = (size_t)tape_stop_buffer_frames * 2u * sizeof(int16_t);
#if defined(M5UNIFIED_PC_BUILD)
    tape_stop.pcm = (int16_t*)malloc(bytes);
#else
    tape_stop.pcm = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
    if (tape_stop.pcm != nullptr) { tape_stop.capacity = tape_stop_buffer_frames; }
  }

#if defined (M5UNIFIED_PC_BUILD)
  auto thread = SDL_CreateThread((SDL_ThreadFunction)task_func, "i2s", this);
  (void)thread;
#else
  // メモリブロックの断片化への対策 (task_i2s.cpp と同じ手順)
  auto dummy = m5gfx::heap_alloc_dma(heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  bufdata = (int32_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
  m5gfx::heap_free(dummy);
  if (bufdata == nullptr) {
    bufdata = (int32_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
  }
  memset(bufdata, 0, buf_size);

  _i2s_init();

  xTaskCreatePinnedToCore((TaskFunction_t)task_func, "i2s", 1024*3, this, kp::def::system::task_priority_i2s, nullptr, kp::def::system::task_cpu_i2s);
#endif
  return true;
}

// 波形表示用リングバッファに min/max を記録 (task_i2s.cpp と同一形式)
static inline void push_raw_wave(int32_t min_level, int32_t max_level)
{
  min_level = ((min_level >> 16) + 32768 + 128) >> 8;
  max_level = ((max_level >> 16) + 32768 + 128) >> 8;

  auto wav_buf = kp::system_registry->raw_wave;
  auto raw_wave_pos = kp::system_registry->raw_wave_pos;
  wav_buf[raw_wave_pos ++] = std::make_pair<uint8_t, uint8_t>(min_level, max_level);
  if (raw_wave_pos >= (kp::system_registry->raw_wave_length)) {
    raw_wave_pos = 0;
  }
  kp::system_registry->raw_wave_pos = raw_wave_pos;
}

void sampler_audio_t::task_func(sampler_audio_t* me)
{
  (void)me;
#if defined (M5UNIFIED_PC_BUILD)
  // PCビルドでは実音声出力は行わず、ボイスを進めて波形表示のみ更新する
  static int32_t pcbuf[i2s_dma_frame_num];
  for (;;) {
    int32_t min_level = INT32_MAX;
    int32_t max_level = INT32_MIN;
    for (int i = 0; i < i2s_dma_frame_num; i += 2) {
      int64_t l = 0, r = 0;
      int64_t mixed = mix_voices();
      l += mixed;
      r += mixed;
      int64_t ll = saturate32(l);
      int64_t rr = saturate32(r);
      process_master_fx(ll, rr);
      int64_t out_l = ((int64_t)ll * output_gain_q8) >> 8;
      int64_t out_r = ((int64_t)rr * output_gain_q8) >> 8;
      process_output_limiter(out_l, out_r);
      process_tape_stop(out_l, out_r);
      const uint16_t fade_gain = output_muted ? 0 : output_fade_gain_q15();
      pcbuf[i  ] = apply_output_fade(saturate32(out_l), fade_gain);
      pcbuf[i+1] = apply_output_fade(saturate32(out_r), fade_gain);
      capture_output_frame(pcbuf[i], pcbuf[i+1]);
      record_input_frame(pcbuf[i], pcbuf[i+1]);
      if (min_level > pcbuf[i  ]) { min_level = pcbuf[i  ]; }
      if (max_level < pcbuf[i  ]) { max_level = pcbuf[i  ]; }
      if (min_level > pcbuf[i+1]) { min_level = pcbuf[i+1]; }
      if (max_level < pcbuf[i+1]) { max_level = pcbuf[i+1]; }
    }
    push_raw_wave(min_level, max_level);
    SDL_Delay(1);
  }
#else
  int32_t* i2sbuf = bufdata;

  size_t transfer_size;

#if __has_include(<driver/i2s_std.h>)
  do {
    i2s_channel_preload_data(_i2s_tx_handle, i2sbuf, buf_size, &transfer_size);
  } while (transfer_size == buf_size);
#endif

  _i2s_start();

  int32_t current_volume = 0;
  int shifted_volume = 0;
  uint8_t raw_wave_divider = 1;
  uint8_t raw_wave_phase = 0;

  for (;;) {
    _i2s_read(i2sbuf, buf_size, &transfer_size, 128);
    kp::system_registry->task_status.setWorking(kp::system_registry_t::reg_task_status_t::bitindex_t::TASK_I2S);
    const uint32_t processing_started_usec = M5.micros();
    if (live_wave_clear_pending) {
      const int len = kp::system_registry->raw_wave_length;
      auto wav_buf = kp::system_registry->raw_wave;
      for (int i = 0; i < len; ++i) { wav_buf[i] = std::make_pair(128, 128); }
      kp::system_registry->raw_wave_pos = 0;
      live_wave_clear_pending = false;
    }

    // マスターボリュームのレンジ0~100を 1~256に変換 (task_i2s.cpp と同一)
    int32_t target_volume = kp::system_registry->user_setting.getMasterVolume() << 8;
    if (target_volume > 25600) { target_volume = 25600; }
    if (current_volume != target_volume) {
      current_volume += (target_volume - current_volume + (target_volume < current_volume ? 0 : 32)) >> 5;
      shifted_volume = current_volume / 100;
    }

    // The live wave is a visual aid only. When the preceding block consumed
    // most of its 1ms budget, update it at half rate and leave that time for
    // the next audio block. Audio, recording and loop timing stay full rate.
    const bool capture_raw_wave = live_wave_capture_enabled
                               && (++raw_wave_phase % raw_wave_divider) == 0;
    int32_t min_level = INT32_MAX;
    int32_t max_level = INT32_MIN;

    for (int i = 0; i < i2s_dma_frame_num; i += 2) {
      // 入力(SAM音源/マイク)のパススルーにボイスをミキシング
      record_input_frame(i2sbuf[i], i2sbuf[i+1]);
      int64_t l = output_muted ? 0 : i2sbuf[i  ];
      int64_t r = output_muted ? 0 : i2sbuf[i+1];
      if (!output_muted) {
        int64_t mixed = mix_voices();
        l += mixed;
        r += mixed;
      }
      int64_t ll = saturate32(l);
      int64_t rr = saturate32(r);
      if (!output_muted) { process_master_fx(ll, rr); }
      int64_t out_l = ((int64_t)(ll >> 8) * shifted_volume * output_gain_q8) >> 8;
      int64_t out_r = ((int64_t)(rr >> 8) * shifted_volume * output_gain_q8) >> 8;
      if (!output_muted) { process_output_limiter(out_l, out_r); }
      if (!output_muted) { process_tape_stop(out_l, out_r); }
      const uint16_t fade_gain = output_muted ? 0 : output_fade_gain_q15();
      int32_t output_l = apply_output_fade(saturate32(out_l), fade_gain);
      int32_t output_r = apply_output_fade(saturate32(out_r), fade_gain);
      capture_output_frame(output_l, output_r);
      if (capture_raw_wave) {
        if (min_level > output_l) { min_level = output_l; }
        if (max_level < output_l) { max_level = output_l; }
        if (min_level > output_r) { min_level = output_r; }
        if (max_level < output_r) { max_level = output_r; }
      }
      i2sbuf[i  ] = output_l;
      i2sbuf[i+1] = output_r;
    }
    if (capture_raw_wave) { push_raw_wave(min_level, max_level); }

    const uint32_t processing_usec = M5.micros() - processing_started_usec;
    const uint8_t instant_load = (uint8_t)std::min<uint32_t>(255, (processing_usec * 256u) / 1000u);
    const uint8_t previous_load = processing_load_q8;
    processing_load_q8 = (uint8_t)((previous_load * 7u + instant_load + 4u) >> 3);
    if (instant_load > processing_peak_q8) { processing_peak_q8 = instant_load; }
    // 75% is intentionally conservative: raising the visual divider before
    // the deadline keeps Pad attacks from competing with display work.
    raw_wave_divider = processing_load_q8 >= 192 ? 2 : 1;

    kp::system_registry->task_status.setSuspend(kp::system_registry_t::reg_task_status_t::bitindex_t::TASK_I2S);
    _i2s_write(bufdata, buf_size, &transfer_size, 128);
  }
#endif
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif // defined (KANPLAY_SAMPLER)
