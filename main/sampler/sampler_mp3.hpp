// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_MP3_HPP
#define KANTAN_SAMPLER_MP3_HPP

#include <stddef.h>
#include <stdint.h>

namespace kanplay_ns { struct storage_read_stream_t; }

namespace sampler_ns {

enum class mp3_decode_result_t : uint8_t {
  ok,
  invalid_data,
  unsupported_format,
  too_long,
  over_budget,
  no_memory,
};

// MP3を演奏用の48kHz / mono / signed PCM16へ変換する。
// 成功時は呼び出し側がfree()するPSRAMバッファを返す。
mp3_decode_result_t decode_mp3_mono_48k(const uint8_t* data, size_t size,
                                        uint32_t max_frames, bool truncate,
                                        int16_t** pcm, uint32_t* frames);

// Two sequential SD passes count frames, then fill only the final PCM.
mp3_decode_result_t decode_mp3_stream_mono_48k(
  kanplay_ns::storage_read_stream_t* stream, uint32_t max_frames,
  size_t max_pcm_bytes, int16_t** pcm, uint32_t* frames,
  void (*progress)(void) = nullptr);

} // namespace sampler_ns

#endif
