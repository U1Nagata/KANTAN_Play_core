#!/usr/bin/env python3
"""Verify Sample/Edit preview cursor coordinates for forward and Reverse."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
APP = (ROOT / "main/sampler/sampler_app.cpp").read_text()


def function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


harness = f"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

{function(APP, "static inline uint32_t sample_preview_display_frame(")}

int main() {{
  constexpr uint32_t frames = 100;
  assert(sample_preview_display_frame(0, frames, false) == 0);
  assert(sample_preview_display_frame(99, frames, false) == 99);
  assert(sample_preview_display_frame(99, frames, true) == 0);
  assert(sample_preview_display_frame(0, frames, true) == 99);

  // Reverse playback visits source 79..20. On the already reversed waveform,
  // those frames must visit display 20..79: always left-to-right.
  uint32_t previous = 0;
  for (uint32_t local = 0; local < 60; ++local) {{
    const uint32_t source = 79u - local;
    const uint32_t display = sample_preview_display_frame(source, frames, true);
    if (local != 0) {{ assert(display > previous); }}
    previous = display;
  }}

  std::puts("PASS: Reverse waveform preview cursor advances left-to-right");
}}
"""

with tempfile.TemporaryDirectory(prefix="sampler-reverse-cursor-test-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-O2", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
