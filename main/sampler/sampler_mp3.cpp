// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined(KANPLAY_SAMPLER)

#include "sampler_mp3.hpp"
#include "../file_manage.hpp"

#include <algorithm>
#include <stdlib.h>

#if defined(ARDUINO)
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <mp3dec.h>
#endif

namespace sampler_ns {

#if defined(ARDUINO)
namespace {

static constexpr uint32_t output_rate = 48000;
static constexpr size_t decoder_samples = MAX_NCHAN * MAX_NGRAN * MAX_NSAMP;
static constexpr size_t stream_input_bytes = 16 * 1024;

struct mp3_scan_t {
  uint64_t source_frames = 0;
  uint32_t source_rate = 0;
  uint32_t decoded_frames = 0;
};

struct mp3_gapless_t {
  uint32_t delay = 0;
  uint32_t padding = 0;
  uint32_t declared_frames = 0;
  uint32_t samples_per_frame = 0;
  bool valid = false;
};

static uint32_t mp3_be32(const uint8_t* p)
{
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
       | ((uint32_t)p[2] << 8) | p[3];
}

// LAME stores encoder delay and end padding in the first Xing/Info frame.
// The decoded frame count alone includes both, creating a short silence at
// the wrap even when the authored audio is exactly loopable.
static mp3_gapless_t read_mp3_gapless(kanplay_ns::storage_read_stream_t* stream)
{
  mp3_gapless_t info;
  if (!kanplay_ns::storage_sd.seekStream(stream, 0)) { return info; }
  uint8_t id3[10] = {};
  if (kanplay_ns::storage_sd.readStream(stream, id3, sizeof(id3)) != (int)sizeof(id3)) {
    return info;
  }
  size_t offset = 0;
  if (memcmp(id3, "ID3", 3) == 0) {
    if ((id3[6] | id3[7] | id3[8] | id3[9]) & 0x80u) { return info; }
    offset = 10u + ((size_t)id3[6] << 21) + ((size_t)id3[7] << 14)
           + ((size_t)id3[8] << 7) + id3[9];
    if (id3[5] & 0x10u) { offset += 10u; }
  }
  if (offset + 192u >= stream->size || !kanplay_ns::storage_sd.seekStream(stream, offset)) {
    return info;
  }
  uint8_t head[1024] = {};
  const size_t count = std::min<size_t>(sizeof(head), stream->size - offset);
  if (kanplay_ns::storage_sd.readStream(stream, head, count) != (int)count) { return info; }
  const int sync = MP3FindSyncWord(head, (int)count);
  if (sync < 0 || (size_t)sync + 4 >= count) { return info; }
  const uint8_t* frame = head + sync;
  const uint8_t version = (frame[1] >> 3) & 3u;
  if (version == 1 || ((frame[1] >> 1) & 3u) != 1) { return info; }
  info.samples_per_frame = version == 3 ? 1152u : 576u;
  const bool mono = (frame[3] >> 6) == 3u;
  const size_t side_bytes = version == 3 ? (mono ? 17u : 32u) : (mono ? 9u : 17u);
  const size_t xing = (size_t)sync + 4u + ((frame[1] & 1u) ? 0u : 2u) + side_bytes;
  if (xing + 8u >= count
   || (memcmp(head + xing, "Xing", 4) && memcmp(head + xing, "Info", 4))) {
    return info;
  }
  const uint32_t flags = mp3_be32(head + xing + 4u);
  size_t tag = xing + 8u;
  if (flags & 1u) {
    if (tag + 4u > count) { return info; }
    info.declared_frames = mp3_be32(head + tag);
    tag += 4u;
  }
  if (flags & 2u) { tag += 4u; }
  if (flags & 4u) { tag += 100u; }
  if (flags & 8u) { tag += 4u; }
  if (tag + 24u > count || memcmp(head + tag, "LAME", 4)) { return info; }
  info.delay = ((uint32_t)head[tag + 21u] << 4) | (head[tag + 22u] >> 4);
  info.padding = ((uint32_t)(head[tag + 22u] & 15u) << 8) | head[tag + 23u];
  info.valid = info.declared_frames != 0 && info.delay < 4096u
            && info.padding < 4096u;
  return info;
}

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

static bool walk_mp3_stream(kanplay_ns::storage_read_stream_t* stream,
                            mp3_scan_t* scan, int16_t* output,
                            uint32_t target_frames, uint32_t* written,
                            bool* memory_error, uint32_t trim_head,
                            void (*progress)(void))
{
  if (!kanplay_ns::storage_sd.seekStream(stream, 0)) { return false; }
  auto* input = (uint8_t*)heap_caps_malloc(stream_input_bytes, MALLOC_CAP_SPIRAM);
  auto* scratch = (int16_t*)heap_caps_malloc(decoder_samples * sizeof(int16_t), MALLOC_CAP_INTERNAL);
  HMP3Decoder decoder = MP3InitDecoder();
  if (!input || !scratch || !decoder) {
    *memory_error = true;
    free(input);
    free(scratch);
    if (decoder) { MP3FreeDecoder(decoder); }
    return false;
  }

  size_t valid = 0;
  size_t position = 0;
  uint64_t source_index = 0;
  uint32_t output_index = 0;
  int16_t previous = 0;
  bool have_previous = false;
  bool read_error = false;
  while (true) {
    if (valid - position < 4096 && stream->position < stream->size) {
      memmove(input, input + position, valid - position);
      valid -= position;
      position = 0;
      const int got = kanplay_ns::storage_sd.readStream(stream, input + valid,
                                                        stream_input_bytes - valid);
      if (got <= 0) { read_error = true; break; }
      valid += (size_t)got;
    }
    if (valid - position < 4 || (output && output_index >= target_frames)) { break; }
    const int sync = MP3FindSyncWord(input + position, (int)(valid - position));
    if (sync < 0) {
      position = valid - std::min<size_t>(valid - position, 3);
      if (stream->position >= stream->size) { break; }
      continue;
    }
    position += (size_t)sync;
    unsigned char* cursor = input + position;
    int remaining = (int)(valid - position);
    const int result = MP3Decode(decoder, &cursor, &remaining, scratch, 0);
    if (result == ERR_MP3_INDATA_UNDERFLOW && stream->position < stream->size
     && valid - position < stream_input_bytes) {
      memmove(input, input + position, valid - position);
      valid -= position;
      position = 0;
      const int got = kanplay_ns::storage_sd.readStream(stream, input + valid,
                                                        stream_input_bytes - valid);
      if (got <= 0) { read_error = true; break; }
      valid += (size_t)got;
      continue;
    }
    if (result == ERR_MP3_NONE) {
      MP3FrameInfo info = {};
      MP3GetLastFrameInfo(decoder, &info);
      if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2
       || info.samprate < 8000 || info.samprate > 48000 || info.outputSamps <= 0
       || info.outputSamps > (int)decoder_samples || info.outputSamps % info.nChans != 0) {
        read_error = true;
        break;
      }
      if (scan->source_rate == 0) { scan->source_rate = (uint32_t)info.samprate; }
      if (scan->source_rate != (uint32_t)info.samprate) { read_error = true; break; }
      const uint32_t count = (uint32_t)(info.outputSamps / info.nChans);
      scan->source_frames += count;
      ++scan->decoded_frames;
      if (output) {
        for (uint32_t i = 0; i < count && output_index < target_frames; ++i, ++source_index) {
          const int16_t current = mono_sample(scratch, i, (uint8_t)info.nChans);
          if (!have_previous) { previous = current; have_previous = true; }
          while (output_index < target_frames) {
            const uint64_t position_q = (uint64_t)output_index * scan->source_rate;
            const uint64_t base = trim_head + position_q / output_rate;
            const uint32_t fraction = (uint32_t)(position_q % output_rate);
            if (base + (fraction ? 1u : 0u) > source_index) { break; }
            output[output_index++] = base == source_index || source_index == 0
              ? current : (int16_t)(((int64_t)previous * (output_rate - fraction)
                                     + (int64_t)current * fraction) / output_rate);
          }
          previous = current;
        }
      }
      if ((scan->decoded_frames & 15u) == 0 && progress) { progress(); }
      if ((scan->decoded_frames & 63u) == 0) { M5.delay(1); }
    }
    const size_t next = (size_t)(cursor - input);
    position = next > position ? next : position + 1;
  }
  if (output && have_previous) {
    while (output_index < target_frames) { output[output_index++] = previous; }
  }
  if (written) { *written = output_index; }
  MP3FreeDecoder(decoder);
  free(scratch);
  free(input);
  return !read_error && scan->decoded_frames != 0 && scan->source_frames > 1;
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

mp3_decode_result_t decode_mp3_stream_mono_48k(
  kanplay_ns::storage_read_stream_t* stream, uint32_t max_frames,
  size_t max_pcm_bytes, int16_t** pcm, uint32_t* frames,
  void (*progress)(void))
{
  if (pcm) { *pcm = nullptr; }
  if (frames) { *frames = 0; }
  if (!stream || !stream->isOpen() || !pcm || !frames || max_frames < 16) {
    return mp3_decode_result_t::invalid_data;
  }
#if !defined(ARDUINO)
  return mp3_decode_result_t::unsupported_format;
#else
  bool memory_error = false;
  const mp3_gapless_t gapless = read_mp3_gapless(stream);
  mp3_scan_t scan;
  if (!walk_mp3_stream(stream, &scan, nullptr, 0, nullptr, &memory_error,
                       0, progress)) {
    return memory_error ? mp3_decode_result_t::no_memory
                        : mp3_decode_result_t::invalid_data;
  }
  uint32_t trim_head = 0;
  uint32_t trim_tail = 0;
  if (gapless.valid) {
    const uint64_t declared = (uint64_t)gapless.declared_frames * gapless.samples_per_frame;
    const uint64_t extra = scan.source_frames > declared ? scan.source_frames - declared : 0;
    if (extra <= gapless.samples_per_frame
     && scan.source_frames + gapless.samples_per_frame >= declared
     && extra + gapless.delay + gapless.padding < scan.source_frames) {
      // Some decoders emit the Xing metadata frame as silent PCM; others do
      // not. Trim it only when the scan reports one extra decoded frame.
      trim_head = (uint32_t)extra + gapless.delay;
      trim_tail = gapless.padding;
    }
  }
  const uint64_t audible_frames = scan.source_frames - trim_head - trim_tail;
  const uint64_t converted = (audible_frames * output_rate + scan.source_rate / 2)
                           / scan.source_rate;
  if (converted > max_frames) { return mp3_decode_result_t::too_long; }
  const size_t bytes = (size_t)converted * sizeof(int16_t);
  if (converted < 16) { return mp3_decode_result_t::invalid_data; }
  if (bytes > max_pcm_bytes) { return mp3_decode_result_t::over_budget; }
  int16_t* output = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!output) { return mp3_decode_result_t::no_memory; }
  mp3_scan_t second_pass;
  uint32_t written = 0;
  if (!walk_mp3_stream(stream, &second_pass, output, (uint32_t)converted,
                       &written, &memory_error, trim_head, progress)
   || written != converted || second_pass.source_rate != scan.source_rate) {
    free(output);
    return memory_error ? mp3_decode_result_t::no_memory
                        : mp3_decode_result_t::invalid_data;
  }
  *pcm = output;
  *frames = written;
  return mp3_decode_result_t::ok;
#endif
}

} // namespace sampler_ns

#endif
