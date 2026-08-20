// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_MUSIC_PLAYER_HPP
#define KANTAN_SAMPLER_MUSIC_PLAYER_HPP

#include <stdint.h>

namespace sampler_ns {

class sampler_music_player_t {
public:
  enum class state_t : uint8_t { idle, loading, ready, playing, paused, ended, error };
  enum class load_stage_t : uint8_t { idle, opening, indexing, reading };

  static bool begin(void);
  static void end(void);
  static bool load(const char* path);
  static void clear(void);
  static void playPause(void);
  static void stop(void);
  static void seekRelative(int32_t seconds);
  static state_t state(void);
  static bool isPlaying(void);
  // Exact 48 kHz deck position used as the master clock for long-running
  // Music/Rec synchronisation. Millisecond UI positions are too coarse for it.
  static uint32_t positionFrames(void);
  static uint32_t positionSeconds(void);
  static uint32_t positionMilliseconds(void);
  static uint32_t durationSeconds(void);
  static uint32_t underruns(void);
  static bool hasTrack(void);
  static const char* path(void);
  static const char* title(void);
  // A newly loaded track exposes one compact 8 kHz mono analysis capture.
  // The caller owns no memory and must release it after key detection.
  static bool keyAnalysis(const int16_t** pcm, uint32_t* frames, uint32_t* sample_rate);
  // A separate 1 kHz capture spans enough time to compare 8-16 loop heads
  // without retaining another long full-band PCM buffer.
  static bool timingAnalysis(const int16_t** pcm, uint32_t* frames,
                             uint32_t* sample_rate);
  static bool startTimingAnalysis(const uint32_t* start_msec, uint8_t count);
  static bool timingAnalysisPending(void);
  static uint8_t timingAnalysisMaxProbeCount(void);
  static uint8_t timingAnalysisProbeCount(void);
  static uint32_t timingAnalysisProbeFrames(void);
  static uint32_t timingAnalysisProbeStartMilliseconds(uint8_t probe);
  static uint8_t timingAnalysisCompletedProbeCount(void);
  static uint32_t keyAnalysisStartMilliseconds(void);
  static bool keyAnalysisPending(void);
  static load_stage_t loadStage(void);
  static uint8_t loadProgress(void);
  static void releaseKeyAnalysis(void);
};

} // namespace sampler_ns

#endif
