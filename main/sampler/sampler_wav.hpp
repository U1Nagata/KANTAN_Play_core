// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_WAV_HPP
#define KANTAN_SAMPLER_WAV_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace sampler_ns {
//-------------------------------------------------------------------------
// PCM/float WAVの最小パーサ。取り込み時に16bit monoへ変換する。

struct wav_info_t {
  const uint8_t* pcm;
  uint32_t frames;
  uint32_t sample_rate;
  uint16_t channels;
  uint16_t audio_format;
  uint16_t bits_per_sample;
  uint16_t bytes_per_frame;
};

static inline bool parse_wav(const uint8_t* data, size_t size, wav_info_t* out)
{
  if (data == nullptr || size < 44) { return false; }
  if (memcmp(data, "RIFF", 4) || memcmp(data + 8, "WAVE", 4)) { return false; }

  uint16_t audio_format = 0;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits = 0;
  const uint8_t* pcm = nullptr;
  uint32_t pcm_size = 0;

  size_t pos = 12;
  while (pos + 8 <= size) {
    const uint8_t* chunk = &data[pos];
    uint32_t chunk_size;
    memcpy(&chunk_size, chunk + 4, 4);
    const uint8_t* body = chunk + 8;
    if (!memcmp(chunk, "fmt ", 4) && chunk_size >= 16) {
      memcpy(&audio_format, body     , 2);
      memcpy(&channels,     body +  2, 2);
      memcpy(&sample_rate,  body +  4, 4);
      memcpy(&bits,         body + 14, 2);
      if (audio_format == 0xFFFE && chunk_size >= 40) {
        // WAVE_FORMAT_EXTENSIBLE: SubFormat GUID先頭はPCM=1 / IEEE float=3。
        memcpy(&audio_format, body + 24, 2);
      }
    } else if (!memcmp(chunk, "data", 4)) {
      pcm = body;
      pcm_size = chunk_size;
      if (pos + 8 + pcm_size > size) { pcm_size = size - (pos + 8); }
    }
    pos += 8 + chunk_size + (chunk_size & 1);
  }

  if (!((audio_format == 1 && (bits == 16 || bits == 24 || bits == 32))
     || (audio_format == 3 && bits == 32))
   || channels == 0 || channels > 2
   || sample_rate == 0 || sample_rate > 48000
   || pcm == nullptr) {
    return false;
  }
  uint16_t bytes_per_sample = bits / 8;
  uint16_t bytes_per_frame = channels * bytes_per_sample;
  if (bytes_per_frame == 0 || pcm_size < bytes_per_frame) { return false; }
  out->pcm = pcm;
  out->frames = pcm_size / bytes_per_frame;
  out->sample_rate = sample_rate;
  out->channels = channels;
  out->audio_format = audio_format;
  out->bits_per_sample = bits;
  out->bytes_per_frame = bytes_per_frame;
  return true;
}

static inline int16_t wav_sample_to_i16(const wav_info_t& info, uint32_t frame, uint16_t channel)
{
  if (frame >= info.frames) { frame = info.frames ? info.frames - 1 : 0; }
  if (channel >= info.channels) { channel = 0; }
  const uint8_t* src = info.pcm + (size_t)frame * info.bytes_per_frame
                     + (size_t)channel * (info.bits_per_sample / 8);
  if (info.audio_format == 3) {
    float value = 0.0f;
    memcpy(&value, src, sizeof(value));
    if (!(value == value)) { return 0; }
    if (value >= 1.0f) { return INT16_MAX; }
    if (value <= -1.0f) { return INT16_MIN; }
    return (int16_t)(value * 32767.0f);
  }
  if (info.bits_per_sample == 16) {
    int16_t value;
    memcpy(&value, src, sizeof(value));
    return value;
  }
  if (info.bits_per_sample == 24) {
    int32_t value = (int32_t)src[0] | ((int32_t)src[1] << 8) | ((int32_t)src[2] << 16);
    if (value & 0x800000) { value |= ~0xFFFFFF; }
    return (int16_t)(value >> 8);
  }
  int32_t value;
  memcpy(&value, src, sizeof(value));
  return (int16_t)(value >> 16);
}

// 44.1kHz素材を48kHz出力へ取り込む時に使う、読み込み時限定の線形補間。
// 演奏中のPSRAM読み出しと補間をなくすため、変換コストはロード時へ寄せる。
static inline uint32_t resampled_frame_count(uint32_t frames, uint32_t source_rate, uint32_t target_rate)
{
  if (frames == 0 || source_rate == 0 || target_rate == 0) { return 0; }
  if (source_rate == target_rate) { return frames; }
  return (uint32_t)(((uint64_t)frames * target_rate + source_rate / 2) / source_rate);
}

static inline int16_t wav_mono_frame(const wav_info_t& info, uint32_t frame)
{
  if (frame >= info.frames) { frame = info.frames ? info.frames - 1 : 0; }
  if (info.channels == 2) {
    return (int16_t)(((int32_t)wav_sample_to_i16(info, frame, 0)
                    + (int32_t)wav_sample_to_i16(info, frame, 1)) >> 1);
  }
  return wav_sample_to_i16(info, frame, 0);
}

static inline int16_t wav_resampled_mono_frame(const wav_info_t& info, uint32_t output_frame, uint32_t output_rate)
{
  if (info.frames == 0 || output_rate == 0) { return 0; }
  if (info.sample_rate == output_rate) { return wav_mono_frame(info, output_frame); }
  uint64_t position = ((uint64_t)output_frame * info.sample_rate << 16) / output_rate;
  uint32_t index = (uint32_t)(position >> 16);
  uint32_t fraction = (uint32_t)position & 0xFFFFu;
  int32_t a = wav_mono_frame(info, index);
  if (fraction == 0 || index + 1 >= info.frames) { return (int16_t)a; }
  int32_t b = wav_mono_frame(info, index + 1);
  return (int16_t)(a + (((b - a) * (int32_t)fraction) >> 16));
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
