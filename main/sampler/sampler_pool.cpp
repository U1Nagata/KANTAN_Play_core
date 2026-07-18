// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined (KANPLAY_SAMPLER)

#include <M5Unified.h>
#include <algorithm>
#include <math.h>

#include "sampler_pool.hpp"
#include "sampler_wav.hpp"

namespace sampler_ns {
//-------------------------------------------------------------------------

sample_slot_t sampler_pool_t::slot[def::pad::pad_count];

static int16_t* pool_alloc(size_t bytes)
{
#if defined (M5UNIFIED_PC_BUILD)
  return (int16_t*)malloc(bytes);
#else
  return (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
}

static void pool_free(int16_t* ptr)
{
  if (ptr) { free(ptr); }
}

// 100%のPadを複数同時に鳴らしても余裕が残るよう、通常素材はピークを約-12 dBFSへ揃える。
// 小さすぎる録音はノイズまで持ち上げないため、増幅量を最大8倍に抑える。
static void normalize_pcm_for_pad(int16_t* pcm, uint32_t frames, uint32_t target_peak = 8192)
{
  static constexpr uint32_t minimum_peak = 256;
  static constexpr uint32_t maximum_gain_q16 = 8u << 16;
  if (!pcm || frames == 0) { return; }

  uint32_t peak = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t value = pcm[i];
    uint32_t magnitude = value < 0 ? (uint32_t)-value : (uint32_t)value;
    if (magnitude > peak) { peak = magnitude; }
  }
  if (peak < minimum_peak) { return; }

  uint32_t gain_q16 = (uint32_t)(((uint64_t)target_peak << 16) / peak);
  if (gain_q16 > maximum_gain_q16) { gain_q16 = maximum_gain_q16; }
  if (gain_q16 == (1u << 16)) { return; }
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t value = (int32_t)(((int64_t)pcm[i] * gain_q16) >> 16);
    if (value > INT16_MAX) { value = INT16_MAX; }
    if (value < INT16_MIN) { value = INT16_MIN; }
    pcm[i] = (int16_t)value;
  }
}

// Pad音源を鍵盤で鳴らすための基音推定。読み込み時だけ実行し、演奏中には
// 一切負荷を持ち込まない。アタックを避けた約250msを8kHz相当に間引き、
// 自己相関の最初の強いピークから単音素材の周期を得る。
static uint8_t detect_base_note(const int16_t* pcm, uint32_t frames, uint32_t sample_rate)
{
  static constexpr uint8_t fallback_note = 60;  // C3
  static constexpr uint32_t analysis_rate = 8000;
  static constexpr uint32_t min_frequency = 65;    // C2付近
  static constexpr uint32_t max_frequency = 1046;  // C6付近
  static constexpr uint16_t confidence_min_q12 = 2050;  // 正規化相関 0.5
  if (!pcm || sample_rate == 0 || frames < sample_rate / 12) { return fallback_note; }

  const uint32_t stride = std::max<uint32_t>(1, sample_rate / analysis_rate);
  const uint32_t effective_rate = sample_rate / stride;
  const uint32_t start = std::min<uint32_t>(frames, sample_rate / 25);  // attackを40ms除外
  const uint32_t available = (frames > start) ? (frames - start) / stride : 0;
  const uint32_t count = std::min<uint32_t>(2048, available);
  if (count < 512 || effective_rate <= max_frequency) { return fallback_note; }

  int64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) { sum += pcm[start + i * stride]; }
  const int32_t mean = (int32_t)(sum / (int64_t)count);

  uint64_t signal_energy = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t value = (int32_t)pcm[start + i * stride] - mean;
    signal_energy += (int64_t)value * value;
  }
  // 無音や小さなノイズを音程付き素材と誤認しない。
  if (signal_energy / count < 256u * 256u) { return fallback_note; }

  const uint32_t min_lag = std::max<uint32_t>(4, effective_rate / max_frequency);
  const uint32_t max_lag = std::min<uint32_t>(count / 2, effective_rate / min_frequency);
  if (max_lag <= min_lag + 2 || max_lag - min_lag >= 192) { return fallback_note; }

  uint16_t scores[192] = {};
  uint16_t best_score = 0;
  uint32_t best_lag = 0;
  for (uint32_t lag = min_lag; lag <= max_lag; ++lag) {
    int64_t correlation = 0;
    uint64_t energy_a = 0;
    uint64_t energy_b = 0;
    for (uint32_t i = lag; i < count; ++i) {
      const int32_t a = (int32_t)pcm[start + i * stride] - mean;
      const int32_t b = (int32_t)pcm[start + (i - lag) * stride] - mean;
      correlation += (int64_t)a * b;
      energy_a += (int64_t)a * a;
      energy_b += (int64_t)b * b;
    }
    if (correlation <= 0) { continue; }
    const uint64_t energy = std::min(energy_a, energy_b);
    if (energy == 0) { continue; }
    const uint16_t score = (uint16_t)std::min<int64_t>(4095, (correlation << 12) / (int64_t)energy);
    scores[lag - min_lag] = score;
    if (score > best_score) {
      best_score = score;
      best_lag = lag;
    }
  }
  if (best_score < confidence_min_q12 || best_lag == 0) { return fallback_note; }

  // 倍音の山ではなく、十分に強い最初の局所ピークを基音として選ぶ。
  const uint16_t fundamental_threshold = (uint16_t)((best_score * 3u) / 4u);
  for (uint32_t lag = min_lag + 1; lag < max_lag; ++lag) {
    const uint16_t previous = scores[lag - min_lag - 1];
    const uint16_t current = scores[lag - min_lag];
    const uint16_t next = scores[lag - min_lag + 1];
    if (current >= fundamental_threshold && current >= previous && current > next) {
      best_lag = lag;
      break;
    }
  }

  const float frequency = (float)effective_rate / (float)best_lag;
  const int note = (int)lroundf(69.0f + 12.0f * log2f(frequency / 440.0f));
  return (note >= 24 && note <= 96) ? (uint8_t)note : fallback_note;
}

static void build_waveform_cache(sample_slot_t& slot)
{
  for (uint8_t bin = 0; bin < sample_slot_t::waveform_bins; ++bin) {
    uint32_t start = ((uint64_t)bin * slot.frames) / sample_slot_t::waveform_bins;
    uint32_t end = ((uint64_t)(bin + 1) * slot.frames) / sample_slot_t::waveform_bins;
    if (end <= start) { end = start + 1; }
    if (end > slot.frames) { end = slot.frames; }
    int16_t min_value = INT16_MAX;
    int16_t max_value = INT16_MIN;
    for (uint32_t i = start; i < end; ++i) {
      int16_t value = slot.pcm[i];
      if (value < min_value) { min_value = value; }
      if (value > max_value) { max_value = value; }
    }
    slot.waveform_min[bin] = min_value;
    slot.waveform_max[bin] = max_value;
  }
}

size_t sampler_pool_t::usedBytes(void)
{
  size_t used = 0;
  for (auto& s : slot) { used += s.bytes(); }
  return used;
}

size_t sampler_pool_t::freeBytes(void)
{
  size_t used = usedBytes();
  return (used < pool_budget_bytes) ? (pool_budget_bytes - used) : 0;
}

void sampler_pool_t::analyzeBaseNote(uint8_t index)
{
  if (index >= def::pad::pad_count) { return; }
  auto& s = slot[index];
  if (!s.isValid() || s.playFrames() == 0) { return; }
  s.base_note = detect_base_note(s.pcm + s.playStart(), s.playFrames(), s.sample_rate);
  s.base_note_auto = true;
}

bool sampler_pool_t::loadWav(uint8_t index, const char* display_name, const uint8_t* wav_data, size_t wav_size)
{
  if (index >= def::pad::pad_count) { return false; }

  wav_info_t info;
  if (!parse_wav(wav_data, wav_size, &info)) { return false; }

  erase(index);

  // 44.1kHz素材だけは48kHzへ事前変換する。通常ピッチ再生時に
  // I2Sタスクが補間せずに済むため、多重発音時の負荷を大きく抑えられる。
  const uint32_t target_rate = info.sample_rate == 44100 ? 48000 : info.sample_rate;
  uint32_t source_frames = info.frames;
  uint32_t source_frames_max = info.sample_rate * max_sample_sec;
  if (source_frames > source_frames_max) { source_frames = source_frames_max; }
  uint32_t frames = resampled_frame_count(source_frames, info.sample_rate, target_rate);
  size_t free_bytes = freeBytes();
  if ((size_t)frames * sizeof(int16_t) > free_bytes) {
    frames = free_bytes / sizeof(int16_t);
  }
  if (frames < 16) { return false; }  // プール枯渇

  int16_t* pcm = pool_alloc((size_t)frames * sizeof(int16_t));
  if (pcm == nullptr) { return false; }

  // モノラル化と必要時の48kHz変換を一度だけ行う。
  for (uint32_t i = 0; i < frames; ++i) {
    pcm[i] = wav_resampled_mono_frame(info, i, target_rate);
  }
  normalize_pcm_for_pad(pcm, frames);

  auto& s = slot[index];
  s.pcm = pcm;
  s.frames = frames;
  s.sample_rate = target_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.base_note = 60;
  s.base_note_auto = true;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  s.loop_grid_half_steps = 8;
  analyzeBaseNote(index);
  build_waveform_cache(s);
  snprintf(s.name, sizeof(s.name), "%s", display_name ? display_name : "");
  s.file_path[0] = 0;
  return true;
}

static bool load_pcm_for_pad(uint8_t index, const char* display_name, const int16_t* pcm_data,
                             uint32_t frames, uint32_t sample_rate, uint32_t target_peak)
{
  if (index >= def::pad::pad_count || pcm_data == nullptr || frames == 0 || sample_rate == 0 || sample_rate > 48000) {
    return false;
  }

  sampler_pool_t::erase(index);

  uint32_t frames_max = sample_rate * sampler_pool_t::max_sample_sec;
  if (frames > frames_max) { frames = frames_max; }
  size_t free_bytes = sampler_pool_t::freeBytes();
  if ((size_t)frames * sizeof(int16_t) > free_bytes) {
    frames = free_bytes / sizeof(int16_t);
  }
  if (frames < 16) { return false; }

  int16_t* pcm = pool_alloc((size_t)frames * sizeof(int16_t));
  if (pcm == nullptr) { return false; }
  memcpy(pcm, pcm_data, (size_t)frames * sizeof(int16_t));
  normalize_pcm_for_pad(pcm, frames, target_peak);

  auto& s = sampler_pool_t::slot[index];
  s.pcm = pcm;
  s.frames = frames;
  s.sample_rate = sample_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.base_note = 60;
  s.base_note_auto = true;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  s.loop_grid_half_steps = 8;
  sampler_pool_t::analyzeBaseNote(index);
  build_waveform_cache(s);
  snprintf(s.name, sizeof(s.name), "%s", display_name ? display_name : "");
  s.file_path[0] = 0;
  return true;
}

bool sampler_pool_t::loadPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate)
{
  return load_pcm_for_pad(index, display_name, pcm_data, frames, sample_rate, 8192);
}

bool sampler_pool_t::loadRecordedPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate)
{
  // 通常素材より約2dB大きい-10dBFS相当。録音した音を埋もれにくくする一方、
  // 複数Padの同時演奏でも出力リミッターが常時働きにくい水準に留める。
  return load_pcm_for_pad(index, display_name, pcm_data, frames, sample_rate, 10240);
}

bool sampler_pool_t::loadPcmOwned(uint8_t index, const char* display_name, int16_t* pcm_data, uint32_t frames, uint32_t sample_rate)
{
  if (index >= def::pad::pad_count || pcm_data == nullptr || frames < 16 || sample_rate == 0 || sample_rate > 48000) {
    return false;
  }
  uint32_t frames_max = sample_rate * max_sample_sec;
  if (frames > frames_max) { frames = frames_max; }

  // 録音バッファなど、すでに確保済みのPCMもプール上限に含める。
  // BGMとWi-Fi/TLSが必要とするPSRAMを食い切らないようにする。
  const size_t new_bytes = (size_t)frames * sizeof(int16_t);
  const size_t replacing_bytes = slot[index].bytes();
  if (new_bytes > freeBytes() + replacing_bytes) { return false; }

  erase(index);
  normalize_pcm_for_pad(pcm_data, frames);

  auto& s = slot[index];
  s.pcm = pcm_data;
  s.frames = frames;
  s.sample_rate = sample_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.base_note = 60;
  s.base_note_auto = true;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  s.loop_grid_half_steps = 8;
  analyzeBaseNote(index);
  build_waveform_cache(s);
  snprintf(s.name, sizeof(s.name), "%s", display_name ? display_name : "");
  s.file_path[0] = 0;
  return true;
}

void sampler_pool_t::erase(uint8_t index)
{
  if (index >= def::pad::pad_count) { return; }
  auto& s = slot[index];
  if (s.pcm) {
    // 再生ボイスが停止済みでも、オーディオタスクが現在のブロックを処理し終えるのを待つ
    M5.delay(8);
    pool_free(s.pcm);
  }
  s.pcm = nullptr;
  s.frames = 0;
  s.start_frame = 0;
  s.end_frame = 0;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.base_note = 60;
  s.base_note_auto = true;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  s.loop_grid_half_steps = 8;
  memset(s.waveform_min, 0, sizeof(s.waveform_min));
  memset(s.waveform_max, 0, sizeof(s.waveform_max));
  s.name[0] = 0;
  s.file_path[0] = 0;
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif // defined (KANPLAY_SAMPLER)
