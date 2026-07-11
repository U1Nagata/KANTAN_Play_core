// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_WAV_HPP
#define KANTAN_SAMPLER_WAV_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace sampler_ns {
//-------------------------------------------------------------------------
// PCM16 WAVの最小パーサ (fmt/dataチャンクのみ解釈)

struct wav_info_t {
  const int16_t* pcm;
  uint32_t frames;
  uint32_t sample_rate;
  uint16_t channels;
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
    } else if (!memcmp(chunk, "data", 4)) {
      pcm = body;
      pcm_size = chunk_size;
      if (pos + 8 + pcm_size > size) { pcm_size = size - (pos + 8); }
    }
    pos += 8 + chunk_size + (chunk_size & 1);
  }

  if (audio_format != 1 || bits != 16 || channels == 0 || channels > 2
   || sample_rate == 0 || sample_rate > 48000
   || pcm == nullptr || pcm_size < channels * 2u) {
    return false;
  }
  out->pcm = (const int16_t*)pcm;
  out->frames = pcm_size / (channels * 2);
  out->sample_rate = sample_rate;
  out->channels = channels;
  return true;
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
