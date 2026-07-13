// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined (KANPLAY_SAMPLER)

#include <M5Unified.h>

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

bool sampler_pool_t::loadWav(uint8_t index, const char* display_name, const uint8_t* wav_data, size_t wav_size)
{
  if (index >= def::pad::pad_count) { return false; }

  wav_info_t info;
  if (!parse_wav(wav_data, wav_size, &info)) { return false; }

  erase(index);

  // 上限秒数・プール残量に合わせて切り詰める
  uint32_t frames = info.frames;
  uint32_t frames_max = info.sample_rate * max_sample_sec;
  if (frames > frames_max) { frames = frames_max; }
  size_t free_bytes = freeBytes();
  if ((size_t)frames * sizeof(int16_t) > free_bytes) {
    frames = free_bytes / sizeof(int16_t);
  }
  if (frames < 16) { return false; }  // プール枯渇

  int16_t* pcm = pool_alloc((size_t)frames * sizeof(int16_t));
  if (pcm == nullptr) { return false; }

  // モノラル変換しつつコピー
  if (info.channels == 2) {
    for (uint32_t i = 0; i < frames; ++i) {
      pcm[i] = (int16_t)(((int32_t)info.pcm[i * 2] + info.pcm[i * 2 + 1]) >> 1);
    }
  } else {
    memcpy(pcm, info.pcm, (size_t)frames * sizeof(int16_t));
  }

  auto& s = slot[index];
  s.pcm = pcm;
  s.frames = frames;
  s.sample_rate = info.sample_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  build_waveform_cache(s);
  snprintf(s.name, sizeof(s.name), "%s", display_name ? display_name : "");
  s.file_path[0] = 0;
  return true;
}

bool sampler_pool_t::loadPcm(uint8_t index, const char* display_name, const int16_t* pcm_data, uint32_t frames, uint32_t sample_rate)
{
  if (index >= def::pad::pad_count || pcm_data == nullptr || frames == 0 || sample_rate == 0 || sample_rate > 48000) {
    return false;
  }

  erase(index);

  uint32_t frames_max = sample_rate * max_sample_sec;
  if (frames > frames_max) { frames = frames_max; }
  size_t free_bytes = freeBytes();
  if ((size_t)frames * sizeof(int16_t) > free_bytes) {
    frames = free_bytes / sizeof(int16_t);
  }
  if (frames < 16) { return false; }

  int16_t* pcm = pool_alloc((size_t)frames * sizeof(int16_t));
  if (pcm == nullptr) { return false; }
  memcpy(pcm, pcm_data, (size_t)frames * sizeof(int16_t));

  auto& s = slot[index];
  s.pcm = pcm;
  s.frames = frames;
  s.sample_rate = sample_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  build_waveform_cache(s);
  snprintf(s.name, sizeof(s.name), "%s", display_name ? display_name : "");
  s.file_path[0] = 0;
  return true;
}

bool sampler_pool_t::loadPcmOwned(uint8_t index, const char* display_name, int16_t* pcm_data, uint32_t frames, uint32_t sample_rate)
{
  if (index >= def::pad::pad_count || pcm_data == nullptr || frames < 16 || sample_rate == 0 || sample_rate > 48000) {
    return false;
  }
  uint32_t frames_max = sample_rate * max_sample_sec;
  if (frames > frames_max) { frames = frames_max; }

  erase(index);

  auto& s = slot[index];
  s.pcm = pcm_data;
  s.frames = frames;
  s.sample_rate = sample_rate;
  s.start_frame = 0;
  s.end_frame = frames;
  s.volume_q8 = 256;
  s.pitch_q8 = 256;
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
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
  s.reverse = false;
  s.hold_enabled = false;
  s.loop_enabled = false;
  memset(s.waveform_min, 0, sizeof(s.waveform_min));
  memset(s.waveform_max, 0, sizeof(s.waveform_max));
  s.name[0] = 0;
  s.file_path[0] = 0;
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif // defined (KANPLAY_SAMPLER)
