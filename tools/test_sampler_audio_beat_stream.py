#!/usr/bin/env python3
"""Exercise the production Audio Beat WAV stream parser/decoder without hardware."""
import pathlib
import subprocess
import tempfile

from test_sampler_pcm_render import ROOT, function


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "main/sampler/sampler_wav.hpp"
namespace kanplay_ns {
struct storage_read_stream_t {
  std::vector<uint8_t> bytes;
  size_t size = 0;
  size_t position = 0;
  size_t max_read = 0;
};
struct storage_t {
  bool seekStream(storage_read_stream_t* stream, size_t position) {
    if (position > stream->size) return false;
    stream->position = position;
    return true;
  }
  int readStream(storage_read_stream_t* stream, uint8_t* dest, size_t size) {
    stream->max_read = std::max(stream->max_read, size);
    const size_t count = std::min(size, stream->size - stream->position);
    memcpy(dest, stream->bytes.data() + stream->position, count);
    stream->position += count;
    return (int)count;
  }
} storage_sd;
}
namespace sampler_ns {
namespace kp = kanplay_ns;
static uint8_t* temp_alloc(size_t bytes) { return (uint8_t*)malloc(bytes); }
static void draw_busy_status_dots_tick() {}
struct beat_t {
  uint32_t frames = 0, sample_rate = 0, fitted_length_msec = 0;
  uint8_t loop_repeats = 1;
  bool isValid() const { return frames && sample_rate; }
};
static beat_t audio_beat;
static constexpr uint32_t loop_min_length_ms = 250;
'''

HARNESS = r'''
}
static void write32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) data[offset + i] = (uint8_t)(value >> (8 * i));
}
int main() {
  using namespace sampler_ns;
  kp::storage_read_stream_t stream;
  static constexpr uint32_t frames = 20 * 48000;
  stream.bytes.resize(44 + (size_t)frames * 4);
  stream.size = stream.bytes.size();
  auto& wav = stream.bytes;
  memcpy(wav.data(), "RIFF", 4);
  write32(wav, 4, (uint32_t)wav.size() - 8);
  memcpy(wav.data() + 8, "WAVEfmt ", 8);
  write32(wav, 16, 16);
  wav[20] = 1; wav[22] = 2;
  write32(wav, 24, 48000);
  write32(wav, 28, 48000 * 4);
  wav[32] = 4; wav[34] = 16;
  memcpy(wav.data() + 36, "data", 4);
  write32(wav, 40, frames * 4);
  for (size_t i = 44; i < wav.size(); i += 4) {
    wav[i] = 0xD2; wav[i + 1] = 0x04;
    wav[i + 2] = 0xD2; wav[i + 3] = 0x04;
  }
  audio_beat_wav_stream_t info;
  assert(parse_audio_beat_wav_stream(&stream, &info));
  assert(info.frames == frames && info.sample_rate == 48000);
  std::vector<int16_t> output(frames);
  assert(decode_audio_beat_wav_stream(&stream, info, 48000, output.data(), frames));
  assert(output.front() == 1234 && output[frames / 2] == 1234 && output.back() == 1234);
  assert(stream.max_read <= 8192);
  audio_beat.frames = 822858; audio_beat.sample_rate = 48000;
  assert(audio_beat_length_ms() == 17143); // 112 BPM x 32 beats
  puts("PASS: 20-second WAV streamed in 8 KiB reads; gapless length rounds to 17,143 ms");
}
'''


def main():
    source = (ROOT / "main/sampler/sampler_app.cpp").read_text()
    structures = source[source.index("struct audio_beat_wav_stream_t {"):
                        source.index("static bool read_audio_beat_stream_bytes(")]
    signatures = ["static uint32_t audio_beat_length_ms(void)",
                  "static bool read_audio_beat_stream_bytes(",
                  "static bool parse_audio_beat_wav_stream(",
                  "static bool decode_audio_beat_wav_stream("]
    with tempfile.TemporaryDirectory(prefix="audio-beat-stream-test-") as directory:
        path = pathlib.Path(directory)
        harness = path / "test.cpp"
        harness.write_text(PREAMBLE + structures + "\n".join(
            function(source, signature) for signature in signatures) + HARNESS)
        binary = path / "test"
        subprocess.run(["c++", "-std=c++17", "-O2", "-I", str(ROOT),
                        str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
