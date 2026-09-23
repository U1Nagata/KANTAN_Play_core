#!/usr/bin/env python3
"""Test the Audio Beat Xing/LAME gapless metadata parser."""
import pathlib
import subprocess
import sys
import tempfile

from test_sampler_pcm_render import ROOT, function


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
namespace kanplay_ns {
struct storage_read_stream_t {
  std::vector<uint8_t> bytes;
  size_t size = 0, position = 0;
};
struct storage_t {
  bool seekStream(storage_read_stream_t* s, size_t p) {
    if (p > s->size) return false;
    s->position = p;
    return true;
  }
  int readStream(storage_read_stream_t* s, uint8_t* dst, size_t bytes) {
    size_t n = std::min(bytes, s->size - s->position);
    memcpy(dst, s->bytes.data() + s->position, n);
    s->position += n;
    return (int)n;
  }
} storage_sd;
}
static int MP3FindSyncWord(const unsigned char* data, int length) {
  for (int i = 0; i + 4 < length; ++i) {
    if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) return i;
  }
  return -1;
}
namespace sampler_ns {
'''

HARNESS = r'''
}
int main(int argc, char** argv) {
  using namespace sampler_ns;
  kanplay_ns::storage_read_stream_t stream;
  if (argc > 1) {
    std::ifstream file(argv[1], std::ios::binary);
    stream.bytes.assign(std::istreambuf_iterator<char>(file), {});
  } else {
    stream.bytes.resize(1024);
    auto& d = stream.bytes;
    memcpy(d.data(), "ID3", 3); d[9] = 0x68; d[8] = 1;
    d[242] = 0xFF; d[243] = 0xFB; d[244] = 0x94; d[245] = 0x64;
    memcpy(d.data() + 278, "Xing", 4); d[285] = 15;
    d[288] = 2; d[289] = 0xCC; // 716 MPEG frames
    memcpy(d.data() + 398, "LAME3.100", 9);
    d[419] = 0x24; d[420] = 0x05; d[421] = 0x76;
  }
  stream.size = stream.bytes.size();
  const auto info = read_mp3_gapless(&stream);
  assert(info.valid && info.delay == 576 && info.padding == 1398);
  assert(info.declared_frames == 716 && info.samples_per_frame == 1152);
  assert((uint64_t)info.declared_frames * info.samples_per_frame
         - info.delay - info.padding == 822858);
  stream.bytes[398] = 'X';
  assert(!read_mp3_gapless(&stream).valid); // unknown tag keeps legacy decode
  puts("PASS: LAME delay/padding and declared frame count parsed correctly");
}
'''


def main():
    source = (ROOT / "main/sampler/sampler_mp3.cpp").read_text()
    structures = source[source.index("struct mp3_gapless_t {"):
                        source.index("static uint32_t mp3_be32(")]
    signatures = ["static uint32_t mp3_be32(", "static mp3_gapless_t read_mp3_gapless("]
    with tempfile.TemporaryDirectory(prefix="sampler-gapless-test-") as directory:
        path = pathlib.Path(directory)
        harness = path / "test.cpp"
        harness.write_text(PREAMBLE + structures + "\n".join(
            function(source, signature) for signature in signatures) + HARNESS)
        binary = path / "test"
        subprocess.run(["c++", "-std=c++17", "-O2", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary), *sys.argv[1:]], check=True)


if __name__ == "__main__":
    main()
