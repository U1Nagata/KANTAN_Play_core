#!/usr/bin/env python3
"""Verify that Gater subdivisions share the Beat/Rec Loop grid phase."""

import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
AUDIO = (ROOT / "main/sampler/sampler_audio.cpp").read_text()
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

static uint32_t gater_phase = 0;
static uint32_t gater_transport_phase = 0;
static uint32_t gater_step_frames = 0;
static uint8_t gater_amount = 0;
static volatile uint32_t gater_phase_request_frames = 0;
static volatile uint32_t gater_phase_request = 0;
static uint32_t gater_phase_request_seen = 0;

{function(AUDIO, "static inline uint32_t gater_period_frames(")}
{function(AUDIO, "static inline uint32_t sync_gater_clock(")}

static void request_phase(uint32_t frames) {{
  gater_phase_request_frames = frames;
  gater_phase_request = gater_phase_request + 1u;
}}

int main() {{
  constexpr uint32_t grid = 12000;  // 250 ms at 48 kHz

  // Activate 2.5 grids into the transport. A 1-Grid Gate must begin halfway
  // through its cycle, not at phase zero when the pad happens to be pressed.
  request_phase(grid * 2u + grid / 2u);
  assert(sync_gater_clock(50, grid) == grid);
  assert(gater_transport_phase == grid * 2u + grid / 2u);
  assert(gater_phase == grid / 2u);

  // Every knob subdivision derives from the same absolute musical position.
  assert(sync_gater_clock(75, grid) == grid / 2u);
  assert(gater_phase == 0);
  assert(sync_gater_clock(25, grid) == grid * 2u);
  assert(gater_phase == grid / 2u);
  assert(sync_gater_clock(5, grid) == grid * 4u);
  assert(gater_phase == grid * 2u + grid / 2u);

  // A fresh transport sync replaces the previous pad-origin phase.
  request_phase(grid * 7u + grid / 4u);
  assert(sync_gater_clock(50, grid) == grid);
  assert(gater_transport_phase == grid * 3u + grid / 4u);
  assert(gater_phase == grid / 4u);

  std::puts("PASS: Gater phase follows transport across all Grid subdivisions");
}}
"""

# The UI must source the phase from the live Beat/Rec Loop transport.
assert "setFxGaterTransportPhaseMs(phase_ms)" in APP
assert "loop_pos_ms(M5.millis())" in APP

with tempfile.TemporaryDirectory(prefix="sampler-gater-test-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-O2", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
