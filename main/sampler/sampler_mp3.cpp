// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined(KANPLAY_SAMPLER)

#include "sampler_mp3.hpp"

#include <algorithm>
#include <stdlib.h>

#if defined(ARDUINO)
#include <esp_heap_caps.h>
#include <mp3dec.h>
#endif

namespace sampler_ns {

#if defined(ARDUINO)
namespace {

static constexpr uint32_t output_rate = 48000;
static constexpr size_t decoder_samples = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;

struct mp3_scan_t {
  uint64_t source_frames = 0;
  uint32_t source_rate = 0;
  uint32_t decoded_frames = 0;
};

static bool scan_mp3(const uint8_t* data, size_t size, mp3_scan_t* scan)
{
  HMP3Decoder decoder = MP3InitDecoder();
  int16_t* scratch = (int16_t*)heap_caps_malloc(decoder_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL);
  if (!decoder || !scratch) {
    if (decoder) { MP3FreeDecoder(decoder); }
    free(scratch);
    return false;
  }

  unsigned char* cursor = const_cast<unsigned char*>(data);
  int remaining = (int)std::min<size_t>(size, INT32_MAX);
  while (remaining > 4) {
    int offset = MP3FindSyncWord(cursor, remaining);
    if (offset < 0) { break; }
    cursor += offset;
    remaining -= offset;
    int before = remaining;
    int result = MP3Decode(decoder, &cursor, &remaining, scratch, 0);
    if (result == ERR_MP3_NONE) {
      MP3FrameInfo info = {};
      MP3GetLastFrameInfo(decoder, &info);
      if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2
       || info.samprate < 8000 || info.samprate > 48000 || info.outputSamps <= 0
       || info.outputSamps > (int)decoder_samples || info.outputSamps % info.nChans != 0) {
        MP3FreeDecoder(decoder);
        free(scratch);
        return false;
      }
      if (scan->source_rate == 0) { scan->source_rate = (uint32_t)info.samprate; }
      if (scan->source_rate != (uint32_t)info.samprate) {
        MP3FreeDecoder(decoder);
        free(scratch);
        return false;
      }
      scan->source_frames += (uint32_t)(info.outputSamps / info.nChans);
      ++scan->decoded_frames;
    }
    if (remaining >= before) {
      ++cursor;
      --remaining;
    }
  }

  MP3FreeDecoder(decoder);
  free(scratch);
  return scan->decoded_frames != 0 && scan->source_frames > 1 && scan->source_rate != 0;
}

static inline int16_t mono_sample(const int16_t* interleaved, uint32_t frame, uint8_t channels)
{
  if (channels == 1) { return interleaved[frame]; }
  int32_t mixed = (int32_t)interleaved[frame * 2] + interleaved[frame * 2 + 1];
  return (int16_t)(mixed / 2);
}

} // namespace
#endif

mp3_decode_result_t decode_mp3_mono_48k(const uint8_t* data, size_t size,
                                        uint32_t max_frames, bool truncate,
                                        int16_t** pcm, uint32_t* frames)
{
  if (pcm) { *pcm = nullptr; }
  if (frames) { *frames = 0; }
  if (!data || size < 4 || !pcm || !frames || max_frames < 16) {
    return mp3_decode_result_t::invalid_data;
  }
#if !defined(ARDUINO)
  return mp3_decode_result_t::unsupported_format;
#else
  mp3_scan_t scan;
  if (!scan_mp3(data, size, &scan)) { return mp3_decode_result_t::invalid_data; }

  uint64_t converted_frames = (scan.source_frames * output_rate + scan.source_rate / 2) / scan.source_rate;
  if (converted_frames > max_frames && !truncate) { return mp3_decode_result_t::too_long; }
  uint32_t target_frames = (uint32_t)std::min<uint64_t>(converted_frames, max_frames);
  if (target_frames < 16) { return mp3_decode_result_t::invalid_data; }

  int16_t* output = (int16_t*)heap_caps_malloc((size_t)target_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  int16_t* scratch = (int16_t*)heap_caps_malloc(decoder_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL);
  HMP3Decoder decoder = MP3InitDecoder();
  if (!output || !scratch || !decoder) {
    free(output);
    free(scratch);
    if (decoder) { MP3FreeDecoder(decoder); }
    return mp3_decode_result_t::no_memory;
  }

  unsigned char* cursor = const_cast<unsigned char*>(data);
  int remaining = (int)std::min<size_t>(size, INT32_MAX);
  uint64_t source_index = 0;
  uint32_t output_index = 0;
  int16_t previous = 0;
  bool have_previous = false;

  while (remaining > 4 && output_index < target_frames) {
    int offset = MP3FindSyncWord(cursor, remaining);
    if (offset < 0) { break; }
    cursor += offset;
    remaining -= offset;
    int before = remaining;
    int result = MP3Decode(decoder, &cursor, &remaining, scratch, 0);
    if (result == ERR_MP3_NONE) {
      MP3FrameInfo info = {};
      MP3GetLastFrameInfo(decoder, &info);
      uint32_t frame_count = (uint32_t)(info.outputSamps / info.nChans);
      for (uint32_t i = 0; i < frame_count && output_index < target_frames; ++i, ++source_index) {
        int16_t current = mono_sample(scratch, i, (uint8_t)info.nChans);
        if (!have_previous) {
          previous = current;
          have_previous = true;
        }
        while (output_index < target_frames) {
          uint64_t position = (uint64_t)output_index * scan.source_rate;
          uint64_t base = position / output_rate;
          uint32_t fraction = (uint32_t)(position % output_rate);
          uint64_t required = base + (fraction ? 1 : 0);
          if (required > source_index) { break; }
          if (base == source_index || source_index == 0) {
            output[output_index] = current;
          } else {
            int32_t interpolated = (int32_t)(((int64_t)previous * (output_rate - fraction)
                                             + (int64_t)current * fraction) / output_rate);
            output[output_index] = (int16_t)interpolated;
          }
          ++output_index;
        }
        previous = current;
      }
    }
    if (remaining >= before) {
      ++cursor;
      --remaining;
    }
  }

  while (output_index < target_frames && have_previous) { output[output_index++] = previous; }
  MP3FreeDecoder(decoder);
  free(scratch);
  if (output_index < 16) {
    free(output);
    return mp3_decode_result_t::invalid_data;
  }
  *pcm = output;
  *frames = output_index;
  return mp3_decode_result_t::ok;
#endif
}

} // namespace sampler_ns

#endif
