// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_MUSIC_PLAYER_HPP
#define KANTAN_SAMPLER_MUSIC_PLAYER_HPP

#include <stdint.h>

namespace sampler_ns {

class sampler_music_player_t {
public:
  enum class state_t : uint8_t { idle, loading, ready, playing, paused, ended, error };

  static bool begin(void);
  static void end(void);
  static bool load(const char* path);
  static void playPause(void);
  static void stop(void);
  static void seekRelative(int32_t seconds);
  static state_t state(void);
  static bool isPlaying(void);
  static uint32_t positionSeconds(void);
  static uint32_t durationSeconds(void);
  static uint32_t underruns(void);
  static const char* title(void);
};

} // namespace sampler_ns

#endif
