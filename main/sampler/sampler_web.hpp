// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_WEB_HPP
#define KANTAN_SAMPLER_WEB_HPP

#if defined(KANPLAY_SAMPLER)

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace sampler_ns {

// HTTPタスクは状態を直接変更せず、JSON要求をアプリ更新ループへ渡す。
bool sampler_web_enqueue_command(const uint8_t* data, size_t size);
bool sampler_web_export_state(std::string& out);

struct sampler_web_audio_t {
  const int16_t* pcm = nullptr;
  uint32_t frames = 0;
  uint32_t sample_rate = 0;
};
bool sampler_web_get_audio(bool background, uint8_t pad, sampler_web_audio_t& out);

} // namespace sampler_ns

#endif // defined(KANPLAY_SAMPLER)
#endif // KANTAN_SAMPLER_WEB_HPP
