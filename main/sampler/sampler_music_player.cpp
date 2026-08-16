// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined(KANPLAY_SAMPLER)

#include "sampler_music_player.hpp"

#include "sampler_audio.hpp"
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
static constexpr uint32_t decode_output_frames = 8192u;
static constexpr size_t input_bytes = 24u * 1024u;

enum class command_t : uint8_t { none, load, toggle, stop, seek };
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
static volatile bool start_when_buffered = false;
static volatile bool source_eof = false;
static int16_t* ring_pcm = nullptr;
static int16_t* output_pcm = nullptr;
static uint8_t* input_buffer = nullptr;
static kp::storage_read_stream_t file_stream;
static format_t format = format_t::none;
static wav_stream_t wav;
static uint32_t mp3_bitrate = 0;
static uint64_t resample_source_index = 0;
static uint64_t resample_output_index = 0;
static int16_t previous_l = 0;
static int16_t previous_r = 0;
static bool have_previous = false;
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

static void reset_resampler(void)
{
  resample_source_index = 0;
  resample_output_index = 0;
  previous_l = 0;
  previous_r = 0;
  have_previous = false;
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

static void close_source(void)
{
  sampler_audio_t::setDeckStreamPlaying(false);
  kp::storage_sd.closeReadStream(&file_stream);
#if defined(ARDUINO)
  if (mp3_decoder) {
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = nullptr;
  }
#endif
  format = format_t::none;
  position_frames = 0;
  duration_frames = 0;
  base_position_frames = 0;
  start_when_buffered = false;
  source_eof = false;
  source_rate = output_rate;
  mp3_bitrate = 0;
  reset_resampler();
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

static bool open_source(const char* path)
{
  close_source();
  if (!path || !path[0] || !kp::storage_sd.beginStorage()
   || !kp::storage_sd.openReadStream(path, &file_stream)) { return false; }
  snprintf(current_path, sizeof(current_path), "%s", path);
  const char* name = strrchr(path, '/');
  name = name ? name + 1 : path;
  snprintf(current_title, sizeof(current_title), "%s", name);
  char* dot = strrchr(current_title, '.');
  if (dot) { *dot = 0; }
  reset_resampler();
  base_position_frames = 0;
  source_eof = false;
  if (suffix(path, ".wav")) {
    format = format_t::wav;
    return parse_wav_header();
  }
  if (!suffix(path, ".mp3")) { return false; }
#if defined(ARDUINO)
  format = format_t::mp3;
  mp3_decoder = MP3InitDecoder();
  source_rate = 44100;
  duration_frames = 0;
  return mp3_decoder != nullptr && kp::storage_sd.seekStream(&file_stream, 0);
#else
  return false;
#endif
}

static bool enqueue_output(uint32_t frames)
{
  uint32_t offset = 0;
  while (offset < frames) {
    if (pending_command != command_t::none) { return false; }
    const uint32_t wrote = sampler_audio_t::enqueueDeckStream(output_pcm + offset * 2,
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
static bool decode_mp3_frame(void)
{
  size_t valid = 0;
  while (valid < input_bytes) {
    const int got = kp::storage_sd.readStream(&file_stream, input_buffer + valid, input_bytes - valid);
    if (got <= 0) { break; }
    valid += (size_t)got;
    if (valid >= 4096) { break; }
  }
  if (valid < 4) { return false; }
  int offset = MP3FindSyncWord(input_buffer, (int)valid);
  if (offset < 0) {
    // Large ID3/APEv2 metadata blocks can exceed one read window. Keep the
    // last three bytes so a frame sync split across windows is still found.
    if (file_stream.position < file_stream.size) {
      kp::storage_sd.seekStream(&file_stream, file_stream.position - std::min<size_t>(valid, 3u));
      return true;
    }
    return false;
  }
  unsigned char* cursor = input_buffer + offset;
  int remaining = (int)valid - offset;
  const int before = remaining;
  const int result = MP3Decode(mp3_decoder, &cursor, &remaining, mp3_scratch, 0);
  if (result != ERR_MP3_NONE) {
    kp::storage_sd.seekStream(&file_stream, file_stream.position - valid + offset + 1u);
    return true;
  }
  const size_t consumed = (size_t)(cursor - input_buffer);
  kp::storage_sd.seekStream(&file_stream, file_stream.position - valid + consumed);
  MP3FrameInfo info = {};
  MP3GetLastFrameInfo(mp3_decoder, &info);
  if (info.bitsPerSample != 16 || info.nChans < 1 || info.nChans > 2
   || info.samprate < 8000 || info.samprate > 48000 || info.outputSamps <= 0) {
    return false;
  }
  if (mp3_bitrate == 0 && info.bitrate > 0) {
    mp3_bitrate = (uint32_t)info.bitrate;
    const uint64_t seconds = ((uint64_t)file_stream.size * 8u) / mp3_bitrate;
    duration_frames = (uint32_t)std::min<uint64_t>(UINT32_MAX, seconds * output_rate);
  }
  if (source_rate != (uint32_t)info.samprate) {
    source_rate = (uint32_t)info.samprate;
    reset_resampler();
  }
  const uint32_t frames = (uint32_t)info.outputSamps / (uint32_t)info.nChans;
  uint32_t output_count = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t left = mp3_scratch[i * info.nChans];
    const int16_t right = info.nChans == 2 ? mp3_scratch[i * 2 + 1] : left;
    output_count = resample_frame(left, right, output_pcm, output_count, decode_output_frames);
  }
  (void)before;
  return output_count != 0 && enqueue_output(output_count);
}
#endif

static bool seek_seconds(uint32_t second)
{
  sampler_audio_t::setDeckStreamPlaying(false);
  sampler_audio_t::attachDeckStream(ring_pcm, ring_frames);
  reset_resampler();
  base_position_frames = second * output_rate;
  position_frames = base_position_frames;
  source_eof = false;
  if (format == format_t::wav) {
    uint64_t source_frame = ((uint64_t)second * wav.sample_rate);
    source_frame = std::min<uint64_t>(source_frame, wav.data_bytes / wav.block_align);
    return kp::storage_sd.seekStream(&file_stream,
      wav.data_offset + (size_t)source_frame * wav.block_align);
  }
#if defined(ARDUINO)
  if (format == format_t::mp3 && mp3_bitrate != 0) {
    size_t byte = (size_t)std::min<uint64_t>(file_stream.size - 1u,
      ((uint64_t)second * mp3_bitrate) / 8u);
    MP3FreeDecoder(mp3_decoder);
    mp3_decoder = MP3InitDecoder();
    return mp3_decoder && kp::storage_sd.seekStream(&file_stream, byte);
  }
#endif
  return false;
}

static void service_command(void)
{
  const command_t command = pending_command;
  if (command == command_t::none) { return; }
  pending_command = command_t::none;
  switch (command) {
  case command_t::load:
    player_state = sampler_music_player_t::state_t::loading;
    if (!open_source(pending_path)) {
      close_source();
      player_state = sampler_music_player_t::state_t::error;
    }
    break;
  case command_t::toggle:
    if (player_state == sampler_music_player_t::state_t::playing) {
      sampler_audio_t::setDeckStreamPlaying(false);
      start_when_buffered = false;
      player_state = sampler_music_player_t::state_t::paused;
    } else if (start_when_buffered) {
      start_when_buffered = false;
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
      seek_seconds(0);
      start_when_buffered = false;
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
  for (;;) {
    if (player_task_exit) {
      close_source();
      player_task_handle = nullptr;
      vTaskDelete(nullptr);
    }
    service_command();
    const auto state = player_state;
    if ((state == sampler_music_player_t::state_t::loading
      || state == sampler_music_player_t::state_t::ready
      || state == sampler_music_player_t::state_t::paused
      || state == sampler_music_player_t::state_t::playing)
     && !source_eof
     && sampler_audio_t::deckStreamWritableFrames() >= decode_output_frames) {
      const bool decoded = format == format_t::wav ? decode_wav_chunk() : decode_mp3_frame();
      if (!decoded && pending_command == command_t::none) {
        source_eof = true;
        if (player_state == sampler_music_player_t::state_t::playing
         && sampler_audio_t::deckStreamBufferedFrames() == 0) {
          sampler_audio_t::setDeckStreamPlaying(false);
          player_state = sampler_music_player_t::state_t::ended;
        }
      }
    }
    if (start_when_buffered
     && (sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames || source_eof)) {
      start_when_buffered = false;
      if (sampler_audio_t::deckStreamBufferedFrames() != 0) {
        sampler_audio_t::setDeckStreamPlaying(true);
        player_state = sampler_music_player_t::state_t::playing;
      } else {
        player_state = sampler_music_player_t::state_t::ended;
      }
    }
    if (player_state == sampler_music_player_t::state_t::loading
     && (sampler_audio_t::deckStreamBufferedFrames() >= prebuffer_frames || source_eof)) {
      player_state = sampler_music_player_t::state_t::ready;
    }
    if (source_eof && player_state == sampler_music_player_t::state_t::playing
     && sampler_audio_t::deckStreamBufferedFrames() == 0) {
      sampler_audio_t::setDeckStreamPlaying(false);
      player_state = sampler_music_player_t::state_t::ended;
    }
    if (player_state == sampler_music_player_t::state_t::playing) {
      const uint64_t played = resample_output_index >= sampler_audio_t::deckStreamBufferedFrames()
        ? resample_output_index - sampler_audio_t::deckStreamBufferedFrames() : 0;
      position_frames = (uint32_t)std::min<uint64_t>(UINT32_MAX,
        (uint64_t)base_position_frames + played);
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
    xTaskCreatePinnedToCore(player_task, "music_decode", 5 * 1024, nullptr, 2,
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
#if defined(ARDUINO)
  free(mp3_scratch); mp3_scratch = nullptr;
#endif
  player_state = state_t::idle;
}

bool sampler_music_player_t::load(const char* path)
{
  if (!begin() || !path || !path[0]) { return false; }
  snprintf(pending_path, sizeof(pending_path), "%s", path);
  pending_command = command_t::load;
  return true;
}

void sampler_music_player_t::playPause(void)
{
  if (player_state == state_t::idle || player_state == state_t::error) { return; }
  pending_command = command_t::toggle;
}

void sampler_music_player_t::stop(void)
{
  if (player_state == state_t::idle || player_state == state_t::error) { return; }
  pending_command = command_t::stop;
}

void sampler_music_player_t::seekRelative(int32_t seconds)
{
  if (player_state == state_t::idle || player_state == state_t::error) { return; }
  pending_seek_seconds = seconds;
  pending_command = command_t::seek;
}

sampler_music_player_t::state_t sampler_music_player_t::state(void) { return player_state; }
bool sampler_music_player_t::isPlaying(void) { return player_state == state_t::playing; }
uint32_t sampler_music_player_t::positionSeconds(void) { return position_frames / output_rate; }
uint32_t sampler_music_player_t::durationSeconds(void) { return duration_frames / output_rate; }
uint32_t sampler_music_player_t::underruns(void) { return sampler_audio_t::deckStreamUnderruns(); }
const char* sampler_music_player_t::title(void) { return current_title; }

} // namespace sampler_ns

#endif
