// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined(KANPLAY_SAMPLER)

#include "sampler_music_player.hpp"

#include "sampler_audio.hpp"
#include "sampler_text.hpp"
#include "../file_manage.hpp"

#include <M5Unified.h>
#include <algorithm>
#include <math.h>
#include <string.h>

#if defined(ARDUINO)
#include <esp_heap_caps.h>
#include <mp3dec.h>
#endif

namespace sampler_ns {
namespace {

namespace kp = kanplay_ns;

static constexpr uint32_t output_rate = sampler_audio_t::sample_rate;
static constexpr uint32_t ring_frames = output_rate * 2u;
static constexpr uint32_t prebuffer_frames = output_rate / 3u;
static constexpr uint32_t decode_output_frames = 4096u;
// Refill while plenty of audio remains. Waiting for an entire decode buffer
// to become free made the producer wake in large bursts and miss its deadline
// when Sampler voices and LCD transfers happened at the same time.
static constexpr uint32_t decode_refill_frames = 2048u;
// Shared MP3 decode/index buffer. The shorter initial analysis released more
// PSRAM, so use it to reduce SD seek/read turns while indexing a whole track.
static constexpr size_t input_bytes = 64u * 1024u;
static constexpr uint32_t key_analysis_rate = 8000u;
// The initial detector needs only several raw 1.6-4 second cycles; sparse
// late-song probes provide the long baseline afterwards. Sixteen seconds
// keeps key/chord evidence while halving MP3 decode wait and PSRAM use.
static constexpr uint32_t key_analysis_seconds = 16u;
static constexpr uint32_t key_analysis_capacity = key_analysis_rate * key_analysis_seconds;
static constexpr uint32_t timing_analysis_rate = 1000u;
// Probe phrase boundaries together with their neighbouring cycles. Exact
// 8/16/32-cycle heads often contain a fill or breakdown; 7/9 and 15/17 give
// the robust fit a normal downbeat without requiring a full-song decode.
static constexpr uint8_t timing_probe_max_count = 10u;
static constexpr uint32_t timing_probe_msec = 2000u;
static constexpr uint32_t timing_probe_frames =
  timing_analysis_rate * timing_probe_msec / 1000u;
static constexpr uint32_t timing_analysis_capacity =
  timing_probe_frames * timing_probe_max_count;

enum class command_t : uint8_t { none, load, clear, toggle, stop, seek, timing_probes };
enum class format_t : uint8_t { none, wav, mp3 };

struct wav_stream_t {
  uint16_t format = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
  uint16_t block_align = 0;
  uint32_t sample_rate = 0;
  uint32_t data_offset = 0;
  uint32_t data_bytes = 0;
};

static volatile sampler_music_player_t::state_t player_state = sampler_music_player_t::state_t::idle;
static volatile command_t pending_command = command_t::none;
static volatile int32_t pending_seek_seconds = 0;
static char pending_path[160] = {};
static char current_path[160] = {};
static char current_title[64] = {};
static volatile uint32_t position_frames = 0;
static volatile uint32_t duration_frames = 0;
static volatile uint32_t source_rate = output_rate;
static volatile uint32_t base_position_frames = 0;
// Exact audible transport: base at the last destructive ring reset plus
// frames physically consumed by I2S. Decoder queues may run seconds ahead.
static volatile uint32_t deck_transport_base_frames = 0;
static volatile bool start_when_buffered = false;
static volatile bool source_eof = false;
static volatile bool source_open = false;
static volatile bool track_selected = false;
static uint32_t source_read_failure_since_msec = 0;
static uint32_t source_recovery_retry_msec = 0;
static int16_t* ring_pcm = nullptr;
static int16_t* output_pcm = nullptr;
static uint8_t* input_buffer = nullptr;
static kp::storage_read_stream_t file_stream;
static format_t format = format_t::none;
static wav_stream_t wav;
static uint32_t mp3_bitrate = 0;
static size_t mp3_data_offset = 0;
static size_t mp3_input_offset = 0;
static size_t mp3_input_valid = 0;
static uint64_t resample_source_index = 0;
static uint64_t resample_output_index = 0;
static uint32_t discard_until_output_frame = 0;
static int16_t previous_l = 0;
static int16_t previous_r = 0;
static bool have_previous = false;
static int16_t* key_analysis_pcm = nullptr;
static volatile uint32_t key_analysis_frames = 0;
static int16_t* timing_analysis_pcm = nullptr;
static volatile uint32_t timing_analysis_frames = 0;
static volatile bool key_analysis_active = false;
static volatile bool key_analysis_ready = false;
static volatile bool key_analysis_complete_pending = false;
static volatile bool key_analysis_seek_pending = false;
static volatile bool timing_analysis_active = false;
static volatile bool timing_analysis_ready = false;
static volatile bool timing_probe_complete_pending = false;
static volatile sampler_music_player_t::load_stage_t load_stage =
  sampler_music_player_t::load_stage_t::idle;
static volatile uint8_t load_index_progress = 0;
static uint8_t key_analysis_decimation_phase = 0;
static uint8_t timing_analysis_decimation_phase = 0;
static uint32_t key_analysis_source_phase = 0;
static uint32_t timing_analysis_source_phase = 0;
static uint32_t analysis_source_frames_to_skip = 0;
static uint32_t key_analysis_start_msec = 0;
static uint32_t timing_probe_start_msec[timing_probe_max_count] = {};
static uint8_t timing_probe_count = 0;
static uint8_t timing_probe_index = 0;
#if defined(ARDUINO)
struct mp3_seek_point_t {
  uint32_t source_frame = 0;
  uint32_t byte_offset = 0;
};
static mp3_seek_point_t* mp3_seek_points = nullptr;
static uint16_t mp3_seek_point_count = 0;
static size_t mp3_index_file_size = 0;
static constexpr uint16_t mp3_seek_point_capacity = 1024u;
static constexpr uint16_t mp3_seek_point_stride = 64u;
#endif
#if defined(ARDUINO)
static TaskHandle_t player_task_handle = nullptr;
static volatile bool player_task_exit = false;
static HMP3Decoder mp3_decoder = nullptr;
static int16_t* mp3_scratch = nullptr;
#endif

static uint16_t read_u16(const uint8_t* p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t* p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool read_exact(void* dst, size_t length)
{
  auto* bytes = static_cast<uint8_t*>(dst);
  size_t read = 0;
  while (read < length) {
    const int got = kp::storage_sd.readStream(&file_stream, bytes + read, length - read);
    if (got <= 0) { return false; }
    read += (size_t)got;
  }
  return true;
}

static bool suffix(const char* path, const char* extension)
{
  if (!path || !extension) { return false; }
  const size_t a = strlen(path);
  const size_t b = strlen(extension);
  if (a < b) { return false; }
  for (size_t i = 0; i < b; ++i) {
    char lhs = path[a - b + i];
    char rhs = extension[i];
    if (lhs >= 'A' && lhs <= 'Z') { lhs += 'a' - 'A'; }
    if (rhs >= 'A' && rhs <= 'Z') { rhs += 'a' - 'A'; }
    if (lhs != rhs) { return false; }
  }
  return true;
}

#if defined(ARDUINO)
static bool parse_mp3_frame_header(const uint8_t* bytes, uint32_t* frame_bytes,
                                   uint32_t* sample_rate, uint16_t* samples)
{
  const uint32_t header = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16)
                        | ((uint32_t)bytes[2] << 8) | bytes[3];
  if ((header & 0xFFE00000u) != 0xFFE00000u) { return false; }
  const uint8_t version = (header >> 19) & 3u;
  const uint8_t layer = (header >> 17) & 3u;
  const uint8_t bitrate_index = (header >> 12) & 15u;
  const uint8_t rate_index = (header >> 10) & 3u;
  if (version == 1u || layer != 1u || bitrate_index == 0u
   || bitrate_index == 15u || rate_index == 3u) { return false; }
  static constexpr uint16_t bitrate_mpeg1[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
  };
  static constexpr uint16_t bitrate_mpeg2[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
  };
  static constexpr uint32_t base_rates[3] = { 44100u, 48000u, 32000u };
  uint32_t rate = base_rates[rate_index];
  if (version == 2u) { rate /= 2u; }
  else if (version == 0u) { rate /= 4u; }
  const uint32_t bitrate = (version == 3u ? bitrate_mpeg1[bitrate_index]
                                          : bitrate_mpeg2[bitrate_index]) * 1000u;
  const uint32_t padding = (header >> 9) & 1u;
  const uint32_t length = ((version == 3u ? 144u : 72u) * bitrate) / rate + padding;
  if (length < 24u || length > 2048u) { return false; }
  if (frame_bytes) { *frame_bytes = length; }
  if (sample_rate) { *sample_rate = rate; }
  if (samples) { *samples = version == 3u ? 1152u : 576u; }
  return true;
}

// Scan headers only: payload and PCM decoding are skipped. This gives VBR
// tracks an exact duration and sparse seek map in a fraction of the time of a
// 90-second decode pass.
static bool build_mp3_seek_index(void)
{
  free(mp3_seek_points);
  mp3_seek_points = (mp3_seek_point_t*)heap_caps_malloc(
    sizeof(mp3_seek_point_t) * mp3_seek_point_capacity, MALLOC_CAP_SPIRAM);
  mp3_seek_point_count = 0;
  if (!mp3_seek_points || !input_buffer || file_stream.size <= mp3_data_offset + 4u) {
    return false;
  }
  size_t scan = mp3_data_offset;
  size_t buffer_start = SIZE_MAX;
  size_t buffer_size = 0;
  uint64_t source_frames = 0;
  uint32_t indexed_rate = 0;
  uint32_t frame_index = 0;
  uint8_t reported_progress = 0;
  while (scan + 4u <= file_stream.size) {
    if (buffer_start == SIZE_MAX || scan < buffer_start
     || scan + 4u > buffer_start + buffer_size) {
      if (!kp::storage_sd.seekStream(&file_stream, scan)) { break; }
      const int got = kp::storage_sd.readStream(&file_stream, input_buffer,
        std::min<size_t>(input_bytes, file_stream.size - scan));
      if (got < 4) { break; }
      buffer_start = scan;
      buffer_size = (size_t)got;
    }
    const uint8_t* header = input_buffer + (scan - buffer_start);
    uint32_t frame_bytes = 0;
    uint32_t rate = 0;
    uint16_t samples = 0;
    if (!parse_mp3_frame_header(header, &frame_bytes, &rate, &samples)
     || scan + frame_bytes > file_stream.size
     || (indexed_rate != 0u && indexed_rate != rate)) {
      ++scan;
      continue;
    }
    if (indexed_rate == 0u) { indexed_rate = rate; }
    if ((frame_index % mp3_seek_point_stride) == 0u
     && mp3_seek_point_count < mp3_seek_point_capacity) {
      mp3_seek_points[mp3_seek_point_count++] = {
        (uint32_t)std::min<uint64_t>(UINT32_MAX, source_frames), (uint32_t)scan
      };
    }
    source_frames += samples;
    ++frame_index;
    scan += frame_bytes;
    if (load_stage == sampler_music_player_t::load_stage_t::indexing) {
      const uint8_t progress = (uint8_t)std::min<size_t>(
        99u, ((scan - mp3_data_offset) * 100u)
          / std::max<size_t>(1u, file_stream.size - mp3_data_offset));
      if (progress != reported_progress) {
        reported_progress = progress;
        load_index_progress = progress;
      }
    }
    if ((frame_index & 255u) == 0u) { M5.delay(1); }
  }
  mp3_input_offset = 0;
  mp3_input_valid = 0;
  if (frame_index < 2u || indexed_rate == 0u || source_frames == 0u) {
    free(mp3_seek_points);
    mp3_seek_points = nullptr;
    mp3_seek_point_count = 0;
    return false;
  }
  source_rate = indexed_rate;
  duration_frames = (uint32_t)std::min<uint64_t>(UINT32_MAX,
    (source_frames * output_rate + indexed_rate / 2u) / indexed_rate);
  mp3_index_file_size = file_stream.size;
  load_index_progress = 100u;
  return kp::storage_sd.seekStream(&file_stream, mp3_data_offset);
}
#endif

static void reset_resampler(void)
{
  resample_source_index = 0;
  resample_output_index = 0;
  previous_l = 0;
  previous_r = 0;
  have_previous = false;
  discard_until_output_frame = 0;
}

static uint32_t resample_frame(int16_t left, int16_t right, int16_t* output,
                               uint32_t count, uint32_t capacity)
{
  if (!have_previous) {
    previous_l = left;
    previous_r = right;
    have_previous = true;
  }
  while (count < capacity) {
    const uint64_t position = resample_output_index * source_rate;
    const uint64_t base = position / output_rate;
    const uint32_t fraction = (uint32_t)(position % output_rate);
    const uint64_t required = base + (fraction ? 1u : 0u);
    if (required > resample_source_index) { break; }
    int32_t out_l = left;
    int32_t out_r = right;
    if (base != resample_source_index && resample_source_index != 0) {
      out_l = (int32_t)(((int64_t)previous_l * (output_rate - fraction)
                       + (int64_t)left * fraction) / output_rate);
      out_r = (int32_t)(((int64_t)previous_r * (output_rate - fraction)
                       + (int64_t)right * fraction) / output_rate);
    }
    output[count * 2] = (int16_t)out_l;
    output[count * 2 + 1] = (int16_t)out_r;
    ++count;
    ++resample_output_index;
  }
  previous_l = left;
  previous_r = right;
  ++resample_source_index;
  return count;
}

static void reset_mp3_input(void)
{
  mp3_input_offset = 0;
  mp3_input_valid = 0;
}

static void close_source(bool clear_track = true)
{
  sampler_audio_t::setDeckStreamPlaying(false);
  kp::storage_sd.closeReadStream(&file_stream);
  source_open = false;
#if defined(ARDUINO)
  if (mp3_decoder) {
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = nullptr;
  }
#endif
  format = format_t::none;
  position_frames = 0;
  if (clear_track) { duration_frames = 0; }
  base_position_frames = 0;
  start_when_buffered = false;
  source_eof = false;
  source_read_failure_since_msec = 0;
  source_recovery_retry_msec = 0;
  if (clear_track) {
    source_rate = output_rate;
    mp3_bitrate = 0;
    mp3_data_offset = 0;
  }
#if defined(ARDUINO)
  if (clear_track) {
    free(mp3_seek_points);
    mp3_seek_points = nullptr;
    mp3_seek_point_count = 0;
    mp3_index_file_size = 0;
  }
#endif
  reset_mp3_input();
  reset_resampler();
  if (clear_track) {
    current_path[0] = 0;
    current_title[0] = 0;
  }
  if (ring_pcm) { sampler_audio_t::attachDeckStream(ring_pcm, ring_frames); }
}

static bool parse_wav_header(void)
{
  if (!kp::storage_sd.seekStream(&file_stream, 0)) { return false; }
  uint8_t riff[12] = {};
  if (!read_exact(riff, sizeof(riff)) || memcmp(riff, "RIFF", 4) != 0
                                      || memcmp(riff + 8, "WAVE", 4) != 0) { return false; }
  wav = {};
  size_t cursor = 12;
  while (cursor + 8 <= file_stream.size) {
    uint8_t header[8] = {};
    if (!kp::storage_sd.seekStream(&file_stream, cursor)
     || !read_exact(header, sizeof(header))) { return false; }
    const uint32_t chunk_size = read_u32(header + 4);
    const size_t data = cursor + 8;
    if (data > file_stream.size) { return false; }
    if (memcmp(header, "fmt ", 4) == 0 && chunk_size >= 16) {
      uint8_t fmt[40] = {};
      const size_t read_len = std::min<size_t>(chunk_size, sizeof(fmt));
      if (!read_exact(fmt, read_len)) { return false; }
      wav.format = read_u16(fmt);
      wav.channels = read_u16(fmt + 2);
      wav.sample_rate = read_u32(fmt + 4);
      wav.block_align = read_u16(fmt + 12);
      wav.bits = read_u16(fmt + 14);
      if (wav.format == 0xFFFE && read_len >= 26) { wav.format = read_u16(fmt + 24); }
    } else if (memcmp(header, "data", 4) == 0) {
      wav.data_offset = (uint32_t)data;
      wav.data_bytes = std::min<uint32_t>(chunk_size, (uint32_t)file_stream.size - wav.data_offset);
      break;
    }
    cursor = data + chunk_size + (chunk_size & 1u);
  }
  if ((wav.format != 1 && wav.format != 3) || wav.channels < 1 || wav.channels > 2
   || wav.sample_rate < 8000 || wav.sample_rate > 48000 || wav.block_align == 0
   || (wav.bits != 16 && wav.bits != 24 && wav.bits != 32) || wav.data_bytes == 0) {
    return false;
  }
  source_rate = wav.sample_rate;
  const uint64_t source_frames = wav.data_bytes / wav.block_align;
  duration_frames = (uint32_t)std::min<uint64_t>(UINT32_MAX,
    (source_frames * output_rate + source_rate / 2u) / source_rate);
  return kp::storage_sd.seekStream(&file_stream, wav.data_offset);
}

static int16_t wav_sample(const uint8_t* p)
{
  if (wav.format == 3 && wav.bits == 32) {
    float value = 0.0f;
    memcpy(&value, p, sizeof(value));
    value = std::max(-1.0f, std::min(1.0f, value));
    return (int16_t)lrintf(value * 32767.0f);
  }
  if (wav.bits == 16) { return (int16_t)read_u16(p); }
  if (wav.bits == 24) {
    int32_t value = (int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16);
    if (value & 0x00800000) { value |= 0xFF000000; }
    return (int16_t)(value >> 8);
  }
  return (int16_t)((int32_t)read_u32(p) >> 16);
}

static bool open_source(const char* path, bool reuse_metadata = false)
{
  char path_copy[sizeof(current_path)] = {};
  if (path) { snprintf(path_copy, sizeof(path_copy), "%s", path); }
  close_source(!reuse_metadata);
  if (!path_copy[0] || !kp::storage_sd.beginStorage()
   || !kp::storage_sd.openReadStream(path_copy, &file_stream)) { return false; }
  source_open = true;
  snprintf(current_path, sizeof(current_path), "%s", path_copy);
  const char* name = strrchr(path_copy, '/');
  name = name ? name + 1 : path_copy;
  const std::string display_title = normalize_japanese_display_text(name);
  snprintf(current_title, sizeof(current_title), "%s", display_title.c_str());
  char* dot = strrchr(current_title, '.');
  if (dot) { *dot = 0; }
  reset_resampler();
  base_position_frames = 0;
  source_eof = false;
  if (suffix(path_copy, ".wav")) {
    format = format_t::wav;
    return parse_wav_header();
  }
  if (!suffix(path_copy, ".mp3")) { return false; }
#if defined(ARDUINO)
  format = format_t::mp3;
  mp3_decoder = MP3InitDecoder();
  reset_mp3_input();
  if (!mp3_decoder || !kp::storage_sd.seekStream(&file_stream, 0)) { return false; }

  // Stop closes the SD handle so File Editor remains free to use the card,
  // but the compact frame index stays in PSRAM. Reopening an unchanged track
  // therefore needs only a short playback prebuffer, not another full scan.
  if (reuse_metadata && mp3_seek_points && mp3_seek_point_count != 0u
   && mp3_index_file_size == file_stream.size && source_rate != 0u
   && duration_frames != 0u) {
    return kp::storage_sd.seekStream(&file_stream, mp3_data_offset);
  }
  source_rate = 44100;
  duration_frames = 0;
  mp3_bitrate = 0;
  mp3_data_offset = 0;
  free(mp3_seek_points);
  mp3_seek_points = nullptr;
  mp3_seek_point_count = 0;
  mp3_index_file_size = 0;

  // Skip ID3v2 metadata before scanning for MPEG frames. Album artwork can
  // otherwise contain false sync words and make startup take several seconds.
  uint8_t id3[10] = {};
  if (read_exact(id3, sizeof(id3)) && memcmp(id3, "ID3", 3) == 0
   && (id3[6] & 0x80u) == 0 && (id3[7] & 0x80u) == 0
   && (id3[8] & 0x80u) == 0 && (id3[9] & 0x80u) == 0) {
    mp3_data_offset = 10u + ((size_t)id3[6] << 21) + ((size_t)id3[7] << 14)
                            + ((size_t)id3[8] << 7) + (size_t)id3[9];
    if ((id3[5] & 0x10u) != 0) { mp3_data_offset += 10u; }
    mp3_data_offset = std::min(mp3_data_offset, file_stream.size);
  }
  if (load_stage == sampler_music_player_t::load_stage_t::opening) {
    load_index_progress = 0;
    load_stage = sampler_music_player_t::load_stage_t::indexing;
  }
  return build_mp3_seek_index();
#else
  return false;
#endif
}

static bool enqueue_output(uint32_t frames)
{
  const uint64_t absolute_end = (uint64_t)base_position_frames + resample_output_index;
  const uint64_t absolute_begin = absolute_end >= frames ? absolute_end - frames : 0u;
  uint32_t skip = 0;
  if (discard_until_output_frame > absolute_begin) {
    skip = (uint32_t)std::min<uint64_t>(
      frames, (uint64_t)discard_until_output_frame - absolute_begin);
  }
  if (skip >= frames) { return true; }
  const int16_t* decoded_pcm = output_pcm + skip * 2u;
  frames -= skip;

  if (key_analysis_active) {
    // MP3 duration/bitrate becomes known only after its first decoded frame.
    // Discard that probe frame, seek to the musical analysis region, then
    // start the compact capture there.
    if (key_analysis_seek_pending) { return true; }
    const uint32_t key_decimation = output_rate / key_analysis_rate;
    uint32_t key_captured = key_analysis_frames;
    for (uint32_t i = 0; i < frames; ++i) {
      const bool capture_key = key_captured < key_analysis_capacity
                            && key_analysis_decimation_phase == 0;
      if (capture_key) {
        const int32_t mono = ((int32_t)decoded_pcm[i * 2]
                            + (int32_t)decoded_pcm[i * 2 + 1]) / 2;
        if (capture_key) { key_analysis_pcm[key_captured++] = (int16_t)mono; }
      }
      if (++key_analysis_decimation_phase >= key_decimation) {
        key_analysis_decimation_phase = 0;
      }
    }
    key_analysis_frames = key_captured;
    if (key_captured >= key_analysis_capacity) { key_analysis_complete_pending = true; }
    // Analysis runs before playback. Do not fill the deck ring with audio
    // that would make the selected track begin twelve seconds into the song.
    return true;
  }
  if (timing_analysis_active) {
    const uint32_t timing_decimation = output_rate / timing_analysis_rate;
    const uint32_t segment_end = (timing_probe_index + 1u) * timing_probe_frames;
    uint32_t captured = timing_analysis_frames;
    for (uint32_t i = 0; i < frames && captured < segment_end; ++i) {
      if (timing_analysis_decimation_phase == 0u) {
        const int32_t mono = ((int32_t)decoded_pcm[i * 2]
                            + (int32_t)decoded_pcm[i * 2 + 1]) / 2;
        timing_analysis_pcm[captured++] = (int16_t)mono;
      }
      if (++timing_analysis_decimation_phase >= timing_decimation) {
        timing_analysis_decimation_phase = 0;
      }
    }
    timing_analysis_frames = captured;
    if (captured >= segment_end) { timing_probe_complete_pending = true; }
    return true;
  }
  uint32_t offset = 0;
  while (offset < frames) {
    if (pending_command != command_t::none) { return false; }
    const uint32_t wrote = sampler_audio_t::enqueueDeckStream(decoded_pcm + offset * 2,
                                                               frames - offset);
    offset += wrote;
    if (offset < frames) { M5.delay(2); }
  }
  return true;
}

static bool decode_wav_chunk(void)
{
  if (file_stream.position >= wav.data_offset + wav.data_bytes) { return false; }
  const size_t remaining = wav.data_offset + wav.data_bytes - file_stream.position;
  const uint64_t max_source_frames = std::max<uint64_t>(1,
    ((uint64_t)(decode_output_frames - 2u) * source_rate) / output_rate);
  size_t wanted = std::min<size_t>(input_bytes,
    (size_t)std::min<uint64_t>(remaining, max_source_frames * wav.block_align));
  wanted -= wanted % wav.block_align;
  const int bytes = kp::storage_sd.readStream(&file_stream, input_buffer, wanted);
  if (bytes <= 0) { return false; }
  const uint32_t frames = (uint32_t)bytes / wav.block_align;
  const uint8_t sample_bytes = wav.bits / 8u;
  uint32_t output_count = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const uint8_t* frame = input_buffer + i * wav.block_align;
    const int16_t left = wav_sample(frame);
    const int16_t right = wav.channels == 2 ? wav_sample(frame + sample_bytes) : left;
    output_count = resample_frame(left, right, output_pcm, output_count, decode_output_frames);
  }
  return output_count != 0 && enqueue_output(output_count);
}

#if defined(ARDUINO)
static bool fill_mp3_input(void)
{
  if (mp3_input_offset != 0) {
    const size_t remaining = mp3_input_valid - mp3_input_offset;
    if (remaining != 0) {
      memmove(input_buffer, input_buffer + mp3_input_offset, remaining);
    }
    mp3_input_valid = remaining;
    mp3_input_offset = 0;
  }
  if (mp3_input_valid >= input_bytes || file_stream.position >= file_stream.size) {
    return mp3_input_valid != 0;
  }
  const int got = kp::storage_sd.readStream(&file_stream,
    input_buffer + mp3_input_valid, input_bytes - mp3_input_valid);
  if (got > 0) { mp3_input_valid += (size_t)got; }
  return mp3_input_valid != 0;
}

static bool decode_mp3_frame(void)
{
  if (mp3_input_valid - mp3_input_offset < 4096u) { fill_mp3_input(); }
  const size_t available = mp3_input_valid - mp3_input_offset;
  if (available < 4) { return false; }
  int offset = MP3FindSyncWord(input_buffer + mp3_input_offset, (int)available);
  if (offset < 0) {
    // Keep three bytes in case a frame header crosses the read boundary.
    if (file_stream.position < file_stream.size) {
      mp3_input_offset = mp3_input_valid - std::min<size_t>(available, 3u);
      const size_t previous_position = file_stream.position;
      fill_mp3_input();
      // Keeping the same three bytes is not forward progress. Report the
      // stalled SD read so the task's timed reopen/remount path can recover;
      // otherwise this branch spins forever and Music remains in Loading.
      return file_stream.position != previous_position;
    }
    // No sync remains after the physical file end. Consume trailing tags or
    // padding explicitly so the task can distinguish true EOF from a
    // recoverable decode failure while compressed frames are still buffered.
    mp3_input_offset = mp3_input_valid;
    return false;
  }
  const size_t sync = mp3_input_offset + (size_t)offset;
  unsigned char* cursor = input_buffer + sync;
  int remaining = (int)(mp3_input_valid - sync);
  const int result = MP3Decode(mp3_decoder, &cursor, &remaining, mp3_scratch, 0);
  if (result == ERR_MP3_INDATA_UNDERFLOW) {
    // Retain the incomplete frame and append more bytes. A completely full
    // buffer cannot grow, so skip this false sync instead of spinning forever.
    if (sync == 0 && mp3_input_valid == input_bytes) {
      mp3_input_offset = 1;
    } else {
      mp3_input_offset = sync;
      const size_t previous_position = file_stream.position;
      fill_mp3_input();
      if (file_stream.position == previous_position
       && file_stream.position < file_stream.size) {
        return false;
      }
    }
    return true;
  }
  if (result == ERR_MP3_MAINDATA_UNDERFLOW && cursor > input_buffer + sync) {
    mp3_input_offset = (size_t)(cursor - input_buffer);
    return true;
  }
  if (result != ERR_MP3_NONE) {
    mp3_input_offset = sync + 1u;
    return true;
  }
  mp3_input_offset = (size_t)(cursor - input_buffer);
  MP3FrameInfo info = {};
  MP3GetLastFrameInfo(mp3_decoder, &info);
  if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2
   || info.samprate < 8000 || info.samprate > 48000 || info.outputSamps <= 0) {
    // MP3Decode already consumed this malformed frame. Continue scanning the
    // prefetched tail instead of treating one bad frame as end-of-file.
    return true;
  }
  if (mp3_bitrate == 0 && info.bitrate > 0) {
    mp3_bitrate = (uint32_t)info.bitrate;
    if (duration_frames == 0u) {
      const uint64_t audio_bytes = file_stream.size > mp3_data_offset
        ? file_stream.size - mp3_data_offset : 0;
      const uint64_t seconds = (audio_bytes * 8u) / mp3_bitrate;
      duration_frames = (uint32_t)std::min<uint64_t>(
        UINT32_MAX, seconds * output_rate);
    }
  }
  if (source_rate != (uint32_t)info.samprate) {
    source_rate = (uint32_t)info.samprate;
    reset_resampler();
  }
  const uint32_t frames = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
  const bool capture_key = key_analysis_active && !key_analysis_seek_pending;
  const bool capture_timing = timing_analysis_active;
  if (capture_key || capture_timing) {
    const uint32_t target_rate = capture_key ? key_analysis_rate : timing_analysis_rate;
    uint32_t phase = capture_key ? key_analysis_source_phase
                                 : timing_analysis_source_phase;
    uint32_t captured = capture_key ? key_analysis_frames : timing_analysis_frames;
    const uint32_t capture_end = capture_key
      ? key_analysis_capacity
      : (timing_probe_index + 1u) * timing_probe_frames;
    for (uint32_t i = 0; i < frames && captured < capture_end; ++i) {
      if (analysis_source_frames_to_skip != 0u) {
        --analysis_source_frames_to_skip;
        continue;
      }
      phase += target_rate;
      if (phase < source_rate) { continue; }
      phase -= source_rate;
      const int16_t left = mp3_scratch[i * info.nChans];
      const int16_t right = info.nChans == 2 ? mp3_scratch[i * 2 + 1] : left;
      const int16_t mono = (int16_t)(((int32_t)left + (int32_t)right) / 2);
      if (capture_key) { key_analysis_pcm[captured++] = mono; }
      else { timing_analysis_pcm[captured++] = mono; }
    }
    if (capture_key) {
      key_analysis_source_phase = phase;
      key_analysis_frames = captured;
      if (captured >= key_analysis_capacity) { key_analysis_complete_pending = true; }
    } else {
      timing_analysis_source_phase = phase;
      timing_analysis_frames = captured;
      if (captured >= capture_end) { timing_probe_complete_pending = true; }
    }
    // Analysis does not need the 48 kHz stereo playback conversion. Keeping
    // it out of this path substantially shortens Music loading on MP3 files.
    return true;
  }
  uint32_t output_count = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t left = mp3_scratch[i * info.nChans];
    const int16_t right = info.nChans == 2 ? mp3_scratch[i * 2 + 1] : left;
    output_count = resample_frame(left, right, output_pcm, output_count, decode_output_frames);
  }
  if (output_count == 0) { return true; }
  return enqueue_output(output_count);
}
#endif

static bool source_really_exhausted(void)
{
  if (format == format_t::wav) {
    return file_stream.position >= wav.data_offset + wav.data_bytes;
  }
#if defined(ARDUINO)
  if (format == format_t::mp3) {
    return file_stream.position >= file_stream.size
        && mp3_input_valid - mp3_input_offset < 4u;
  }
#endif
  return file_stream.position >= file_stream.size;
}

static bool seek_milliseconds(uint32_t msec, bool preserve_deck_audio = false)
{
  if (!preserve_deck_audio) {
    sampler_audio_t::setDeckStreamPlaying(false);
    sampler_audio_t::attachDeckStream(ring_pcm, ring_frames);
  }
  reset_resampler();
  const uint32_t target_output_frame = (uint32_t)std::min<uint64_t>(
    UINT32_MAX, ((uint64_t)msec * output_rate) / 1000u);
  if (!preserve_deck_audio) { deck_transport_base_frames = target_output_frame; }
  base_position_frames = target_output_frame;
  if (!preserve_deck_audio) { position_frames = base_position_frames; }
  source_eof = false;
  reset_mp3_input();
  analysis_source_frames_to_skip = 0;
  if (format == format_t::wav) {
    uint64_t source_frame = ((uint64_t)msec * wav.sample_rate) / 1000u;
    source_frame = std::min<uint64_t>(source_frame, wav.data_bytes / wav.block_align);
    base_position_frames = (uint32_t)((source_frame * output_rate) / wav.sample_rate);
    if (!preserve_deck_audio) { position_frames = base_position_frames; }
    discard_until_output_frame = target_output_frame;
    return kp::storage_sd.seekStream(&file_stream,
      wav.data_offset + (size_t)source_frame * wav.block_align);
  }
#if defined(ARDUINO)
  if (format == format_t::mp3 && mp3_seek_point_count != 0 && source_rate != 0u) {
    const uint64_t target_source_frame = ((uint64_t)msec * source_rate) / 1000u;
    uint16_t point = 0;
    while (point + 1u < mp3_seek_point_count
        && mp3_seek_points[point + 1u].source_frame <= target_source_frame) {
      ++point;
    }
    const auto selected = mp3_seek_points[point];
    analysis_source_frames_to_skip = (uint32_t)std::min<uint64_t>(
      UINT32_MAX, target_source_frame - selected.source_frame);
    base_position_frames = (uint32_t)(
      ((uint64_t)selected.source_frame * output_rate) / source_rate);
    if (!preserve_deck_audio) { position_frames = base_position_frames; }
    discard_until_output_frame = target_output_frame;
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = MP3InitDecoder();
    return mp3_decoder && kp::storage_sd.seekStream(&file_stream, selected.byte_offset);
  }
  if (format == format_t::mp3 && mp3_bitrate != 0) {
    size_t byte = (size_t)std::min<uint64_t>(file_stream.size - 1u,
      mp3_data_offset + ((uint64_t)msec * mp3_bitrate) / 8000u);
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = MP3InitDecoder();
    return mp3_decoder && kp::storage_sd.seekStream(&file_stream, byte);
  }
#endif
  return false;
}

static bool seek_seconds(uint32_t second)
{
  return seek_milliseconds(second * 1000u);
}

// A marginal card or a long shared-SPI stall can leave an SdFat file handle
// unable to make further progress even though the card is still present.
// Keep the MP3 seek map and parsed metadata: rebuilding the complete index here
// can monopolise SD/CPU for seconds and briefly stop Beat as well as Music.
static bool recover_source_after_read_error(void)
{
  if (!current_path[0]) { return false; }
  char path[sizeof(current_path)] = {};
  snprintf(path, sizeof(path), "%s", current_path);
  const uint32_t resume_msec = (uint32_t)(
    ((uint64_t)position_frames * 1000u) / output_rate);
  const bool resume = player_state == sampler_music_player_t::state_t::playing
                   || start_when_buffered;
  const uint32_t buffered_frames = sampler_audio_t::deckStreamBufferedFrames();
  const bool preserve_deck_audio = resume && buffered_frames != 0u;
  const uint64_t decoded_head_frames = std::min<uint64_t>(UINT32_MAX,
    (uint64_t)base_position_frames + resample_output_index);
  const uint32_t reopen_msec = preserve_deck_audio
    ? (uint32_t)(decoded_head_frames * 1000u / output_rate)
    : resume_msec;

  if (!preserve_deck_audio) { sampler_audio_t::setDeckStreamPlaying(false); }
  kp::storage_sd.closeReadStream(&file_stream);
  source_open = false;
  bool reopened = false;
  for (uint8_t attempt = 0; attempt < 6u && !reopened; ++attempt) {
    if (attempt != 0u) {
      kp::storage_sd.endStorage();
      M5.delay(40);
    }
    if (!kp::storage_sd.beginStorage()) {
      M5.delay(40);
      continue;
    }
    reopened = kp::storage_sd.openReadStream(path, &file_stream);
    source_open = reopened;
    if (!reopened) { M5.delay(40); }
  }
  if (!reopened || !seek_milliseconds(reopen_msec, preserve_deck_audio)) {
    kp::storage_sd.closeReadStream(&file_stream);
    source_open = false;
    reset_mp3_input();
    // A temporary remount failure must not permanently turn the player into
    // Error. Keep its audible position and retry from the player task; the
    // File menu becomes available again as soon as the card remount succeeds.
    kp::storage_sd.endStorage();
    M5.delay(40);
    kp::storage_sd.beginStorage();
    player_state = sampler_music_player_t::state_t::loading;
    start_when_buffered = true;
    source_recovery_retry_msec = M5.millis() + 600u;
    return false;
  }

  source_read_failure_since_msec = 0;
  source_recovery_retry_msec = 0;
  player_state = preserve_deck_audio ? sampler_music_player_t::state_t::playing
               : resume ? sampler_music_player_t::state_t::loading
                        : sampler_music_player_t::state_t::paused;
  start_when_buffered = resume && !preserve_deck_audio;
  track_selected = true;
  M5_LOGW("Music SD stream recovered at %u ms (%u buffered)",
          (unsigned)reopen_msec, (unsigned)buffered_frames);
  return true;
}

static uint32_t key_analysis_start_second(void)
{
  const uint32_t seconds = duration_frames / output_rate;
  if (seconds >= 30u + key_analysis_seconds) { return 30u; }
  if (seconds > key_analysis_seconds) {
    return (seconds - key_analysis_seconds) / 2u;
  }
  return 0;
}

static void stop_source_and_ready(void)
{
  close_source(false);
  player_state = sampler_music_player_t::state_t::ready;
}

static void service_command(void)
{
  const command_t command = pending_command;
  if (command == command_t::none) { return; }
  pending_command = command_t::none;
  switch (command) {
  case command_t::load:
    player_state = sampler_music_player_t::state_t::loading;
    load_stage = sampler_music_player_t::load_stage_t::opening;
    load_index_progress = 0;
    free(key_analysis_pcm);
    key_analysis_pcm = nullptr;
    free(timing_analysis_pcm);
    timing_analysis_pcm = nullptr;
    key_analysis_frames = 0;
    timing_analysis_frames = 0;
    key_analysis_active = false;
    key_analysis_ready = false;
    key_analysis_complete_pending = false;
    key_analysis_seek_pending = false;
    timing_analysis_active = false;
    timing_analysis_ready = false;
    timing_probe_complete_pending = false;
    timing_probe_index = 0;
    key_analysis_decimation_phase = 0;
    timing_analysis_decimation_phase = 0;
    key_analysis_source_phase = 0;
    timing_analysis_source_phase = 0;
    analysis_source_frames_to_skip = 0;
    key_analysis_start_msec = 0;
    if (!open_source(pending_path)) {
      load_stage = sampler_music_player_t::load_stage_t::idle;
      close_source();
      track_selected = false;
      player_state = sampler_music_player_t::state_t::error;
    } else {
#if defined(M5UNIFIED_PC_BUILD)
      key_analysis_pcm = (int16_t*)malloc(
        (size_t)key_analysis_capacity * sizeof(int16_t));
#else
      key_analysis_pcm = (int16_t*)heap_caps_malloc(
        (size_t)key_analysis_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
#endif
      key_analysis_active = key_analysis_pcm != nullptr;
      if (key_analysis_active) {
        load_stage = sampler_music_player_t::load_stage_t::reading;
        if (format == format_t::mp3 && duration_frames == 0u) {
          key_analysis_seek_pending = true;
        } else {
          const uint32_t start_second = key_analysis_start_second();
          key_analysis_start_msec = start_second * 1000u;
          seek_seconds(start_second);
        }
      }
      else { load_stage = sampler_music_player_t::load_stage_t::idle; }
    }
    break;
  case command_t::clear:
    close_source();
    free(key_analysis_pcm);
    key_analysis_pcm = nullptr;
    free(timing_analysis_pcm);
    timing_analysis_pcm = nullptr;
    key_analysis_frames = 0;
    timing_analysis_frames = 0;
    key_analysis_active = false;
    key_analysis_ready = false;
    key_analysis_complete_pending = false;
    key_analysis_seek_pending = false;
    timing_analysis_active = false;
    timing_analysis_ready = false;
    timing_probe_complete_pending = false;
    timing_probe_count = 0;
    key_analysis_start_msec = 0;
    load_stage = sampler_music_player_t::load_stage_t::idle;
    pending_path[0] = 0;
    track_selected = false;
    player_state = sampler_music_player_t::state_t::idle;
    break;
  case command_t::timing_probes:
    free(timing_analysis_pcm);
#if defined(M5UNIFIED_PC_BUILD)
    timing_analysis_pcm = (int16_t*)malloc(
      (size_t)timing_analysis_capacity * sizeof(int16_t));
#else
    timing_analysis_pcm = (int16_t*)heap_caps_malloc(
      (size_t)timing_analysis_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
#endif
    timing_analysis_frames = 0;
    timing_probe_index = 0;
    timing_analysis_decimation_phase = 0;
    timing_analysis_source_phase = 0;
    timing_analysis_ready = false;
    timing_probe_complete_pending = false;
    if (timing_analysis_pcm) {
      memset(timing_analysis_pcm, 0,
        (size_t)timing_analysis_capacity * sizeof(int16_t));
    }
    timing_analysis_active = timing_analysis_pcm != nullptr
      && seek_milliseconds(timing_probe_start_msec[0]);
    if (!timing_analysis_active) { timing_analysis_ready = true; }
    break;
  case command_t::toggle:
    if (player_state == sampler_music_player_t::state_t::playing) {
      sampler_audio_t::setDeckStreamPlaying(false);
      start_when_buffered = false;
      player_state = sampler_music_player_t::state_t::paused;
    } else if (start_when_buffered) {
      start_when_buffered = false;
    } else if (format == format_t::none && current_path[0]) {
      char path[sizeof(current_path)] = {};
      snprintf(path, sizeof(path), "%s", current_path);
      if (open_source(path, true)) {
        player_state = sampler_music_player_t::state_t::loading;
        start_when_buffered = true;
      } else {
        player_state = sampler_music_player_t::state_t::error;
      }
    } else if (player_state == sampler_music_player_t::state_t::ended
            && format != format_t::none) {
      if (seek_seconds(0)) {
        player_state = sampler_music_player_t::state_t::loading;
        start_when_buffered = true;
      }
    } else if (format != format_t::none) {
      if (sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames || source_eof) {
        sampler_audio_t::setDeckStreamPlaying(true);
        player_state = sampler_music_player_t::state_t::playing;
      } else {
        start_when_buffered = true;
      }
    }
    break;
  case command_t::stop:
    if (format != format_t::none) {
      // A stopped player must release its SD file completely. Reopen it when
      // Play is requested so File Editor and other SD operations cannot be
      // starved by background decoding.
      stop_source_and_ready();
    } else if (current_path[0]) {
      player_state = sampler_music_player_t::state_t::ready;
    }
    break;
  case command_t::seek: {
    const int64_t current = position_frames / output_rate;
    const int64_t duration = duration_frames ? duration_frames / output_rate : INT32_MAX;
    const uint32_t target = (uint32_t)std::max<int64_t>(0,
      std::min<int64_t>(duration, current + pending_seek_seconds));
    const bool resume = player_state == sampler_music_player_t::state_t::playing;
    if (seek_seconds(target)) {
      player_state = sampler_music_player_t::state_t::paused;
      start_when_buffered = resume;
    }
    break;
  }
  default: break;
  }
}

#if defined(ARDUINO)
static void player_task(void*)
{
  uint32_t observed_underruns = sampler_audio_t::deckStreamUnderruns();
  for (;;) {
    if (player_task_exit) {
      close_source();
      player_task_handle = nullptr;
      vTaskDelete(nullptr);
    }
    service_command();
    const auto state = player_state;
    if (state == sampler_music_player_t::state_t::loading
     && !source_open && current_path[0]
     && (int32_t)(M5.millis() - source_recovery_retry_msec) >= 0) {
      recover_source_after_read_error();
    }
    if ((state == sampler_music_player_t::state_t::loading
      || state == sampler_music_player_t::state_t::ready
     || state == sampler_music_player_t::state_t::paused
     || state == sampler_music_player_t::state_t::playing)
     && format != format_t::none
     && source_open
     && !source_eof
     && (!key_analysis_ready || timing_analysis_active)
     && sampler_audio_t::deckStreamWritableFrames() >= decode_refill_frames) {
      const bool decoded = format == format_t::wav ? decode_wav_chunk() : decode_mp3_frame();
      if (!decoded && pending_command == command_t::none) {
        if (source_really_exhausted()) {
          source_eof = true;
          source_read_failure_since_msec = 0;
        } else if (source_read_failure_since_msec == 0) {
          // CoreS3 shares SPI2 between its LCD and SD slot. A live-wave LCD
          // transfer can make one SdFat read return no bytes even though the
          // stream has not reached EOF. The two-second PCM ring gives us time
          // to wait for the bus rather than turning that transient into Stop.
          source_read_failure_since_msec = M5.millis();
          M5.delay(8);
        } else if (M5.millis() - source_read_failure_since_msec >= 900) {
          // Let shared-SPI contention clear without reopening a healthy stream.
          // The two-second Deck ring normally covers this window; a genuinely
          // stuck handle then takes the lightweight reopen path above.
          recover_source_after_read_error();
        } else {
          M5.delay(8);
        }
    } else if (decoded) {
        source_read_failure_since_msec = 0;
      }
      // Private analysis bypasses the audio ring. Yield after every source
      // block so Core 0's idle task and watchdog housekeeping stay responsive.
      if (key_analysis_active || timing_analysis_active) { vTaskDelay(1); }
    }
    if (key_analysis_active && key_analysis_seek_pending && mp3_bitrate != 0) {
      const uint32_t start_second = key_analysis_start_second();
      if (!seek_seconds(start_second)) {
        free(key_analysis_pcm);
        key_analysis_pcm = nullptr;
        free(timing_analysis_pcm);
        timing_analysis_pcm = nullptr;
        key_analysis_frames = 0;
        timing_analysis_frames = 0;
        key_analysis_active = false;
        key_analysis_ready = false;
        close_source(false);
        player_state = sampler_music_player_t::state_t::error;
      } else {
        key_analysis_start_msec = start_second * 1000u;
        key_analysis_frames = 0;
        timing_analysis_frames = 0;
        key_analysis_decimation_phase = 0;
        timing_analysis_decimation_phase = 0;
        key_analysis_source_phase = 0;
        timing_analysis_source_phase = 0;
        key_analysis_seek_pending = false;
      }
    }
    const bool analysis_eof = key_analysis_active
      && !key_analysis_complete_pending && source_eof;
    if (key_analysis_active && (key_analysis_complete_pending || analysis_eof)) {
      key_analysis_active = false;
      key_analysis_complete_pending = false;
      key_analysis_ready = key_analysis_frames >= key_analysis_rate / 2u;
      load_stage = sampler_music_player_t::load_stage_t::idle;
      // Rewind after the private analysis pass. Normal prebuffering now starts
      // at frame zero and retains the player's established SD/ring behaviour.
      if (!seek_seconds(0)) {
        free(key_analysis_pcm);
        key_analysis_pcm = nullptr;
        free(timing_analysis_pcm);
        timing_analysis_pcm = nullptr;
        key_analysis_frames = 0;
        timing_analysis_frames = 0;
        key_analysis_ready = false;
        close_source(false);
        player_state = sampler_music_player_t::state_t::error;
      }
    }
    if (timing_analysis_active
     && (timing_probe_complete_pending || source_eof)) {
      timing_probe_complete_pending = false;
      source_eof = false;
      if (timing_probe_index + 1u < timing_probe_count) {
        ++timing_probe_index;
        // Keep fixed segment offsets even if a very short source could not
        // fill one probe completely; missing samples remain silent.
        timing_analysis_frames = timing_probe_index * timing_probe_frames;
        timing_analysis_decimation_phase = 0;
        timing_analysis_source_phase = 0;
        if (!seek_milliseconds(timing_probe_start_msec[timing_probe_index])) {
          timing_analysis_active = false;
          timing_analysis_ready = true;
        }
      } else {
        timing_analysis_active = false;
        timing_analysis_ready = true;
        seek_seconds(0);
      }
    }
    if (start_when_buffered
     && (sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames || source_eof)) {
      start_when_buffered = false;
      if (sampler_audio_t::deckStreamBufferedFrames() != 0) {
        sampler_audio_t::setDeckStreamPlaying(true);
        player_state = sampler_music_player_t::state_t::playing;
      } else {
        close_source(false);
        player_state = sampler_music_player_t::state_t::error;
      }
    }
    if (player_state == sampler_music_player_t::state_t::loading
     && (sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames || source_eof)) {
      player_state = sampler_music_player_t::state_t::ready;
    }
    if (source_eof && player_state == sampler_music_player_t::state_t::playing
     && sampler_audio_t::deckStreamBufferedFrames() == 0) {
      // Natural EOF follows exactly the same resource-release path as Stop.
      // The selected title remains available and Play can reopen from zero.
      stop_source_and_ready();
    }
    const uint32_t underruns = sampler_audio_t::deckStreamUnderruns();
    if (player_state == sampler_music_player_t::state_t::playing
     && !source_eof && underruns != observed_underruns
     && sampler_audio_t::deckStreamBufferedFrames() == 0) {
      // A temporary decode/SPI stall must not turn into a permanent silent
      // player. Pause only the Deck stream, refill its ring, then resume from
      // the same transport position. Beat and the other parts keep running.
      sampler_audio_t::setDeckStreamPlaying(false);
      player_state = sampler_music_player_t::state_t::loading;
      start_when_buffered = true;
    }
    observed_underruns = underruns;
    if (player_state == sampler_music_player_t::state_t::playing) {
      position_frames = (uint32_t)std::min<uint64_t>(UINT32_MAX,
        (uint64_t)deck_transport_base_frames
          + sampler_audio_t::deckStreamPlayedFrames());
    }
    if ((player_state == sampler_music_player_t::state_t::ready
      || player_state == sampler_music_player_t::state_t::paused)
     && sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames) {
      // Ready means prepared, not auto-playing. Keep the buffered audio still.
    }
    M5.delay(2);
  }
}
#endif

} // namespace

bool sampler_music_player_t::begin(void)
{
  if (ring_pcm) {
    sampler_audio_t::attachDeckStream(ring_pcm, ring_frames);
    return true;
  }
#if defined(M5UNIFIED_PC_BUILD)
  ring_pcm = (int16_t*)malloc((size_t)ring_frames * 2u * sizeof(int16_t));
  output_pcm = (int16_t*)malloc((size_t)decode_output_frames * 2u * sizeof(int16_t));
  input_buffer = (uint8_t*)malloc(input_bytes);
#else
  ring_pcm = (int16_t*)heap_caps_malloc((size_t)ring_frames * 2u * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  output_pcm = (int16_t*)heap_caps_malloc((size_t)decode_output_frames * 2u * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  input_buffer = (uint8_t*)heap_caps_malloc(input_bytes, MALLOC_CAP_SPIRAM);
  mp3_scratch = (int16_t*)heap_caps_malloc(MAX_NCHAN * MAX_NGRAN * MAX_NSAMP
                                            * sizeof(int16_t), MALLOC_CAP_INTERNAL);
#endif
  if (!ring_pcm || !output_pcm || !input_buffer
#if defined(ARDUINO)
   || !mp3_scratch
#endif
  ) {
    end();
    return false;
  }
  memset(ring_pcm, 0, (size_t)ring_frames * 2u * sizeof(int16_t));
  sampler_audio_t::attachDeckStream(ring_pcm, ring_frames);
#if defined(ARDUINO)
  player_task_exit = false;
  if (player_task_handle == nullptr) {
    xTaskCreatePinnedToCore(player_task, "music_decode", 5 * 1024, nullptr, 3,
                            &player_task_handle, 0);
  }
  if (!player_task_handle) {
    end();
    return false;
  }
#endif
  return true;
}

void sampler_music_player_t::end(void)
{
#if defined(ARDUINO)
  pending_command = command_t::none;
  player_task_exit = true;
  for (uint32_t wait = 0; player_task_handle && wait < 500; ++wait) { M5.delay(2); }
  if (player_task_handle) {
    // Keep the buffers allocated if the task has not acknowledged shutdown yet.
    return;
  }
#else
  close_source();
#endif
  sampler_audio_t::detachDeckStream();
  free(ring_pcm); ring_pcm = nullptr;
  free(output_pcm); output_pcm = nullptr;
  free(input_buffer); input_buffer = nullptr;
  free(key_analysis_pcm); key_analysis_pcm = nullptr;
  free(timing_analysis_pcm); timing_analysis_pcm = nullptr;
  key_analysis_frames = 0;
  timing_analysis_frames = 0;
  key_analysis_active = false;
  key_analysis_ready = false;
  key_analysis_complete_pending = false;
  key_analysis_seek_pending = false;
  timing_analysis_active = false;
  timing_analysis_ready = false;
  timing_probe_complete_pending = false;
  timing_probe_index = 0;
  key_analysis_start_msec = 0;
#if defined(ARDUINO)
  free(mp3_scratch); mp3_scratch = nullptr;
#endif
  player_state = state_t::idle;
  track_selected = false;
}

bool sampler_music_player_t::load(const char* path)
{
  if (!begin() || !path || !path[0]) { return false; }
  snprintf(pending_path, sizeof(pending_path), "%s", path);
  track_selected = true;
  load_stage = load_stage_t::opening;
  load_index_progress = 0;
  pending_command = command_t::load;
  return true;
}

void sampler_music_player_t::clear(void)
{
#if defined(ARDUINO)
  if (player_task_handle == nullptr) {
    close_source();
    pending_path[0] = 0;
    track_selected = false;
    player_state = state_t::idle;
    return;
  }
#endif
  pending_command = command_t::clear;
#if defined(ARDUINO)
  for (uint16_t wait = 0; wait < 500; ++wait) {
    if (!track_selected && pending_command != command_t::clear) { break; }
    M5.delay(1);
  }
#endif
}

void sampler_music_player_t::playPause(void)
{
  if (player_state == state_t::idle || player_state == state_t::loading
   || player_state == state_t::error || key_analysis_active || key_analysis_ready) { return; }
  pending_command = command_t::toggle;
}

void sampler_music_player_t::stop(void)
{
#if defined(ARDUINO)
  if (player_task_handle == nullptr) {
    pending_command = command_t::none;
    return;
  }
#endif
  pending_command = command_t::stop;
#if defined(ARDUINO)
  // SD remounts and file operations may follow stop_all_audio() immediately.
  // Wait for the decode task to close its FILE handle before returning.
  for (uint16_t wait = 0; wait < 500; ++wait) {
    if (!source_open && pending_command != command_t::stop) { break; }
    M5.delay(1);
  }
#endif
}

void sampler_music_player_t::seekRelative(int32_t seconds)
{
  if (player_state == state_t::idle || player_state == state_t::error) { return; }
  pending_seek_seconds = seconds;
  pending_command = command_t::seek;
}

sampler_music_player_t::state_t sampler_music_player_t::state(void) { return player_state; }
bool sampler_music_player_t::isPlaying(void) { return player_state == state_t::playing; }
uint32_t sampler_music_player_t::positionFrames(void) { return position_frames; }
uint32_t sampler_music_player_t::positionSeconds(void) { return position_frames / output_rate; }
uint32_t sampler_music_player_t::positionMilliseconds(void)
{
  return (uint32_t)(((uint64_t)position_frames * 1000u) / output_rate);
}
uint32_t sampler_music_player_t::durationSeconds(void) { return duration_frames / output_rate; }
uint32_t sampler_music_player_t::underruns(void) { return sampler_audio_t::deckStreamUnderruns(); }
bool sampler_music_player_t::hasTrack(void) { return track_selected; }
const char* sampler_music_player_t::path(void)
{
  return current_path[0] ? current_path : pending_path;
}

bool sampler_music_player_t::keyAnalysis(const int16_t** pcm, uint32_t* frames,
                                         uint32_t* sample_rate)
{
  if (!key_analysis_ready || !key_analysis_pcm || key_analysis_frames == 0) {
    return false;
  }
  if (pcm) { *pcm = key_analysis_pcm; }
  if (frames) { *frames = key_analysis_frames; }
  if (sample_rate) { *sample_rate = key_analysis_rate; }
  return true;
}

bool sampler_music_player_t::timingAnalysis(const int16_t** pcm, uint32_t* frames,
                                            uint32_t* sample_rate)
{
  if (!timing_analysis_ready || !timing_analysis_pcm
   || timing_probe_count == 0u
   || timing_analysis_frames < timing_probe_frames * timing_probe_count) {
    return false;
  }
  if (pcm) { *pcm = timing_analysis_pcm; }
  if (frames) { *frames = timing_analysis_frames; }
  if (sample_rate) { *sample_rate = timing_analysis_rate; }
  return true;
}

bool sampler_music_player_t::startTimingAnalysis(const uint32_t* start_msec,
                                                 uint8_t count)
{
  if (!key_analysis_ready || !start_msec || count < 2u
   || count > timing_probe_max_count
   || timing_analysis_active || pending_command != command_t::none) { return false; }
  timing_probe_count = count;
  for (uint8_t i = 0; i < timing_probe_count; ++i) {
    timing_probe_start_msec[i] = start_msec[i];
  }
  timing_analysis_ready = false;
  pending_command = command_t::timing_probes;
  return true;
}

bool sampler_music_player_t::timingAnalysisPending(void)
{
  return pending_command == command_t::timing_probes || timing_analysis_active;
}

uint8_t sampler_music_player_t::timingAnalysisProbeCount(void)
{
  return timing_probe_count;
}

uint8_t sampler_music_player_t::timingAnalysisMaxProbeCount(void)
{
  return timing_probe_max_count;
}

uint32_t sampler_music_player_t::timingAnalysisProbeFrames(void)
{
  return timing_probe_frames;
}

uint32_t sampler_music_player_t::timingAnalysisProbeStartMilliseconds(uint8_t probe)
{
  return probe < timing_probe_count ? timing_probe_start_msec[probe] : 0u;
}

uint8_t sampler_music_player_t::timingAnalysisCompletedProbeCount(void)
{
  if (timing_analysis_ready) { return timing_probe_count; }
  if (!timing_analysis_active) { return 0u; }
  return timing_probe_index;
}

bool sampler_music_player_t::keyAnalysisPending(void)
{
  return key_analysis_active || key_analysis_ready || key_analysis_seek_pending
      || timing_analysis_active || pending_command == command_t::timing_probes;
}

sampler_music_player_t::load_stage_t sampler_music_player_t::loadStage(void)
{
  return load_stage;
}

uint8_t sampler_music_player_t::loadProgress(void)
{
  if (load_stage == load_stage_t::indexing) { return load_index_progress; }
  if (load_stage == load_stage_t::reading) {
    return (uint8_t)std::min<uint32_t>(100u,
      ((uint64_t)key_analysis_frames * 100u)
        / std::max<uint32_t>(1u, key_analysis_capacity));
  }
  return 0u;
}

uint32_t sampler_music_player_t::keyAnalysisStartMilliseconds(void)
{
  return key_analysis_start_msec;
}

void sampler_music_player_t::releaseKeyAnalysis(void)
{
  key_analysis_ready = false;
  free(key_analysis_pcm);
  key_analysis_pcm = nullptr;
  free(timing_analysis_pcm);
  timing_analysis_pcm = nullptr;
  key_analysis_frames = 0;
  timing_analysis_frames = 0;
  timing_analysis_active = false;
  timing_analysis_ready = false;
  timing_probe_complete_pending = false;
  timing_probe_count = 0;
  key_analysis_seek_pending = false;
  key_analysis_start_msec = 0;
}
const char* sampler_music_player_t::title(void) { return current_title; }

} // namespace sampler_ns

#endif
