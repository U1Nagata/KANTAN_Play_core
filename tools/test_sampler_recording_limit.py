#!/usr/bin/env python3
"""Check that the production recording cap follows the shared PCM budget."""
import pathlib
import subprocess
import tempfile

from test_sampler_pcm_render import ROOT, function


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#define M5UNIFIED_PC_BUILD 1
namespace sampler_ns {
enum class performance_page_t { drum, sample };
struct sampler_pool_t {
  static constexpr uint32_t max_sample_sec = 20;
  static size_t free_bytes;
  static size_t freeBytes() { return free_bytes; }
};
size_t sampler_pool_t::free_bytes = 0;
struct beat_pool_t { static constexpr uint32_t max_sample_sec = 2; };
static performance_page_t recording_target_page = performance_page_t::sample;
static uint32_t recording_sample_rate_current = 48000;
static int16_t* recording_buffer = nullptr;
static uint32_t recording_buffer_capacity_frames = 0;
static constexpr uint32_t recording_chunk_frames = 4096;
static constexpr uint32_t external_probe_frames = 9600;
'''

HARNESS = r'''
}
int main() {
  using namespace sampler_ns;
  sampler_pool_t::free_bytes = 5u * 1024u * 1024u;
  assert(recording_max_frames() == 20u * 48000u);
  sampler_pool_t::free_bytes = 96000;
  assert(recording_max_frames() == 48000); // one second of PCM16
  sampler_pool_t::free_bytes = 19000;
  assert(recording_max_frames() == 0); // insufficient for source probe
  int16_t buffer[1]; recording_buffer = buffer;
  recording_buffer_capacity_frames = 48000;
  assert(recording_max_frames() == 48000); // fixed after allocation
  recording_buffer = nullptr;
  recording_target_page = performance_page_t::drum;
  assert(recording_max_frames() == 2u * 48000u); // Pattern Beat remains separate
  puts("PASS: Sample recording limit follows shared PCM free space, while Beat cap stays separate");
}
'''


def main():
    source = (ROOT / "main/sampler/sampler_app.cpp").read_text()
    body = function(source, "static uint32_t recording_max_frames(void)")
    with tempfile.TemporaryDirectory(prefix="sampler-recording-cap-test-") as directory:
        path = pathlib.Path(directory)
        harness = path / "test.cpp"
        harness.write_text(PREAMBLE + body + HARNESS)
        binary = path / "test"
        subprocess.run(["c++", "-std=c++17", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
