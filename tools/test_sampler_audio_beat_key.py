#!/usr/bin/env python3
"""Exercise music-key analysis and explicit Audio Beat filename hints."""
import pathlib
import subprocess
import tempfile

from test_sampler_pcm_render import ROOT, function


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>
namespace sampler_ns {
static uint8_t* temp_alloc(size_t bytes) { return (uint8_t*)malloc(bytes); }
static void draw_recording_processing_frame(const char*) {}
static void draw_busy_status_dots_tick() {}
struct { void delay(unsigned) {} } M5;
static int8_t last_auto_beat_key = -1;
static uint8_t current_key = 0;
static uint8_t harmony_scale = 0;
struct { uint8_t scale = 0; } melody_settings, bass_settings;
static void set_harmony_key(uint8_t key) { current_key = key; }
static void reset_harmony_tuning() {}
'''

HARNESS = r'''
}
int main() {
  using namespace sampler_ns;
  auto hint = audio_beat_filename_key_hint(
    "/sampler/loops/City Pop Urban-05_112_F_composed+chord+bass+drums.mp3");
  assert(hint.key == 5 && !hint.minor);
  hint = audio_beat_filename_key_hint("/sampler/loops/song_Eb_112.mp3");
  assert(hint.key == 3 && !hint.minor);
  hint = audio_beat_filename_key_hint("/sampler/loops/song_Am_112.mp3");
  assert(hint.key == 9 && hint.minor);
  hint = audio_beat_filename_key_hint("/sampler/loops/song_Ebm_112.mp3");
  assert(hint.key == 3 && hint.minor);
  hint = audio_beat_filename_key_hint("/sampler/loops/song_F#m_112.mp3");
  assert(hint.key == 6 && hint.minor);
  assert(audio_beat_filename_key_hint("/sampler/loops/song.mp3").key == -1);
  assert(audio_beat_filename_key_hint("/sampler/loops/F_G.mp3").key == -1);
  assert(audio_beat_filename_key_hint("/sampler/loops/A_Am.mp3").key == -1);
  const uint32_t rate = 48000, frames = rate * 4;
  std::vector<int16_t> pcm(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = (double)i / rate;
    pcm[i] = (int16_t)(5400 * sin(2 * M_PI * 174.614 * t)
                     + 4300 * sin(2 * M_PI * 220.000 * t)
                     + 4000 * sin(2 * M_PI * 261.626 * t));
  }
  const auto key = detect_chop_music_key(pcm.data(), frames, rate);
  assert(key.valid && key.key == 5);
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song_C.wav");
  assert(current_key == 0 && last_auto_beat_key == 0 && harmony_scale == 1);
  assert(melody_settings.scale == 1 && bass_settings.scale == 1);
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song_Am.wav");
  assert(current_key == 9 && last_auto_beat_key == 9);
  assert(harmony_scale == 5 && melody_settings.scale == 5 && bass_settings.scale == 5);
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song_Eb.wav");
  assert(current_key == 3 && harmony_scale == 1); // bare key defaults to Major
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song_Am.wav");
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song.wav");
  assert(current_key == 5 && last_auto_beat_key == 5 && harmony_scale == 1);
  set_detected_harmony(9, false); // Chop's valid major-key result
  assert(current_key == 9 && harmony_scale == 1);
  assert(melody_settings.scale == 1 && bass_settings.scale == 1);
  std::fill(pcm.begin(), pcm.end(), 0);
  harmony_scale = 0;
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song_Am.wav");
  assert(current_key == 9 && harmony_scale == 5); // filename works without audio confidence
  apply_audio_beat_key_detection(pcm.data(), frames, rate, "/loops/song.wav");
  assert(current_key == 9 && harmony_scale == 5 && last_auto_beat_key == -1);
  puts("PASS: audio key and filename tonic/minor precedence");
}
'''


def main():
    source = (ROOT / "main/sampler/sampler_app.cpp").read_text()
    structures = source[source.index("struct chop_key_result_t {"):
                        source.index("// Estimate a musical centre", source.index("struct chop_key_result_t {"))]
    signatures = ["static chop_key_result_t detect_chop_music_key(",
                  "static audio_beat_key_hint_t audio_beat_filename_key_hint(",
                  "static void apply_audio_beat_key_detection("]
    detected_harmony = function(source, "static void set_detected_harmony(")
    assert("set_detected_harmony(detected_key.key, false);" in source)
    hint_structure = source[source.index("struct audio_beat_key_hint_t {"):
                            source.index("static audio_beat_key_hint_t audio_beat_filename_key_hint(")]
    with tempfile.TemporaryDirectory(prefix="sampler-key-test-") as directory:
        path = pathlib.Path(directory)
        harness = path / "test.cpp"
        harness.write_text(PREAMBLE + structures + detected_harmony + hint_structure + "\n".join(
            function(source, signature) for signature in signatures) + HARNESS)
        binary = path / "test"
        subprocess.run(["c++", "-std=c++17", "-O2", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
