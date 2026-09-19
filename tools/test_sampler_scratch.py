#!/usr/bin/env python3
"""Exercise the production Master Scratch fixed-window cursor on the host."""

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


def implementation(source: str, signature: str) -> str:
    """Extract the last occurrence, skipping forward declarations."""
    start = source.rindex(signature)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


scratch_types = AUDIO[
    AUDIO.index("enum class master_scratch_state_t"):
    AUDIO.index("enum class master_repeat_state_t")
]

harness = f"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

static constexpr uint32_t output_sample_rate = 48000;
struct tape_stop_t {{ int16_t* pcm = nullptr; uint32_t capacity = 0; }};
static tape_stop_t tape_stop;
struct deck_buffer_t {{
  volatile bool enabled = true;
  volatile bool reset_pending = false;
  uint64_t write_frames = 0;
  uint32_t valid_frames = 0;
  uint32_t recent_frames = 0;
}};
static deck_buffer_t deck_buffer;
{scratch_types}

{function(AUDIO, "static inline int32_t tape_stop_pcm32(")}
{function(AUDIO, "static inline bool read_deck_frame(")}
{function(AUDIO, "static inline void begin_master_scratch_curve(")}
{function(AUDIO, "static inline void advance_master_scratch_curve(")}
{function(AUDIO, "static inline void process_master_scratch(")}

static int16_t pcm[24000 * 2];

static void prepare(int8_t direction) {{
  for (uint32_t i = 0; i < 24000; ++i) {{
    pcm[i * 2] = (int16_t)(i & 0x7fff);
    pcm[i * 2 + 1] = (int16_t)(i & 0x7fff);
  }}
  tape_stop.pcm = pcm;
  tape_stop.capacity = 24000;
  deck_buffer.write_frames = 20000;
  deck_buffer.valid_frames = 20000;
  deck_buffer.recent_frames = 20000;
  master_scratch = {{}};
  master_scratch.requested = true;
  master_scratch.target_direction = direction;
  master_scratch.motion_request = 1;
}}

static unsigned run_to(int8_t direction) {{
  master_scratch.target_direction = direction;
  master_scratch.target_reached = false;
  master_scratch.motion_request = master_scratch.motion_request + 1u;
  int64_t l = 0, r = 0;
  unsigned frames = 0;
  for (; frames < 20000 && !master_scratch.target_reached; ++frames) {{
    process_master_scratch(l, r);
  }}
  assert(master_scratch.target_reached);
  assert(master_scratch.read_fp == (direction < 0
    ? master_scratch.window_min_fp : master_scratch.window_max_fp));
  return frames;
}}

int main() {{
  for (int8_t first : {{int8_t(-1), int8_t(1)}}) {{
    prepare(first);
    run_to(first);
    const int64_t low = master_scratch.window_min_fp;
    const int64_t high = master_scratch.window_max_fp;
    assert(high > low);
    assert(((high - low) >> 16) == 2 * (int64_t)scratch_headroom_frames);
    for (unsigned cycle = 0; cycle < 64; ++cycle) {{
      run_to(-1);
      assert(master_scratch.read_fp == low);
      run_to(1);
      assert(master_scratch.read_fp == high);
    }}
  }}

  // A reversal must start from the current cursor immediately rather than
  // completing the old endpoint first.
  prepare(-1);
  int64_t l = 0, r = 0;
  for (unsigned i = 0; i < 400; ++i) {{ process_master_scratch(l, r); }}
  const int64_t reversed_at = master_scratch.read_fp;
  assert(reversed_at > master_scratch.window_min_fp);
  master_scratch.target_direction = 1;
  master_scratch.target_reached = false;
  master_scratch.motion_request = master_scratch.motion_request + 1u;
  process_master_scratch(l, r);
  assert(master_scratch.motion_start_fp == reversed_at);
  assert(master_scratch.motion_target_fp == master_scratch.window_max_fp);
  run_to(1);

  // The fixed-point smoothstep reaches approximately 2.0x only around its
  // midpoint and converges to zero speed at both endpoints.
  run_to(-1);
  master_scratch.target_direction = 1;
  master_scratch.target_reached = false;
  master_scratch.motion_request = master_scratch.motion_request + 1u;
  int64_t previous = master_scratch.read_fp;
  uint32_t peak_rate_q8 = 0;
  while (!master_scratch.target_reached) {{
    process_master_scratch(l, r);
    if (master_scratch.read_fp < previous) {{
      std::printf("non-monotonic cursor: previous=%lld current=%lld start=%lld target=%lld direction=%d request=%u seen=%u phase=%llu step=%llu\\n",
        (long long)previous, (long long)master_scratch.read_fp,
        (long long)master_scratch.motion_start_fp, (long long)master_scratch.motion_target_fp,
        (int)master_scratch.target_direction, (unsigned)master_scratch.motion_request,
        (unsigned)master_scratch.motion_request_seen,
        (unsigned long long)master_scratch.motion_phase_q32,
        (unsigned long long)master_scratch.motion_phase_step_q32);
      assert(false);
    }}
    const uint64_t delta_fp = (uint64_t)(master_scratch.read_fp - previous);
    peak_rate_q8 = std::max<uint32_t>(peak_rate_q8, (uint32_t)(delta_fp >> 8));
    previous = master_scratch.read_fp;
  }}
  std::printf("Scratch S-curve peak: %.3fx\\n", peak_rate_q8 / 256.0);
  assert(peak_rate_q8 >= 500 && peak_rate_q8 <= 520);
  std::puts("PASS: 2x S-curve, immediate reversal and 64 drift-free endpoint cycles");
}}
"""

app_harness = f"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace kp {{ namespace def {{ namespace button_bitmask {{
static constexpr uint32_t KNOB_L = 1u << 0;
static constexpr uint32_t KNOB_R = 1u << 1;
}} }} }}

struct m5_stub_t {{
  uint32_t now = 0;
  uint32_t millis() const {{ return now; }}
}};
static m5_stub_t M5;

enum class sampler_mode_t : uint8_t {{ mode_fx, mode_other }};
static sampler_mode_t current_mode = sampler_mode_t::mode_fx;
static uint32_t prev_bitmask = 0;
static bool loop_repeat_armed = false;
static bool loop_repeat_running = false;
static int fx_pad_active = -1;
static int pad_display_number(uint8_t value) {{ return value; }}
static void request_urgent_pad_draw(int) {{}}

struct sampler_audio_t {{
  static inline bool requested = false;
  static inline bool reached = false;
  static inline int8_t direction = 0;
  static bool masterScratchAvailable() {{ return true; }}
  static void setTapeStop(bool) {{}}
  static void setMasterScratch(bool active) {{ requested = active; }}
  static void setMasterScratchTargetDirection(int8_t value) {{
    direction = value;
    reached = false;
  }}
  static bool masterScratchTargetReached() {{ return reached; }}
}};

static volatile bool master_scratch_active = false;
enum class master_scratch_phase_t : uint8_t {{ idle, outward, returning, neutral }};
static master_scratch_phase_t master_scratch_phase = master_scratch_phase_t::idle;
static uint32_t master_scratch_neutral_until_msec = 0;
static int8_t master_scratch_outward_direction = 0;
static constexpr const uint32_t master_scratch_neutral_hold_msec = 60;

{implementation(APP, "static void begin_master_scratch_motion(")}
{implementation(APP, "static void reset_master_scratch_control(")}
{implementation(APP, "static void begin_master_scratch_outward(")}
{implementation(APP, "static void set_master_scratch_lever(")}
{implementation(APP, "static void service_master_scratch(")}

static void release_lever(int8_t direction) {{
  prev_bitmask = 0;
  set_master_scratch_lever(direction, false);
}}

int main() {{
  // Every edge retargets immediately; no old endpoint is queued ahead of the
  // player's rhythm.
  M5.now = 0;
  set_master_scratch_lever(-1, true);
  assert(master_scratch_phase == master_scratch_phase_t::outward);
  assert(sampler_audio_t::direction == -1);
  release_lever(-1);
  assert(master_scratch_phase == master_scratch_phase_t::returning);
  assert(sampler_audio_t::direction == 1);
  set_master_scratch_lever(-1, true);
  assert(master_scratch_phase == master_scratch_phase_t::outward);
  assert(sampler_audio_t::direction == -1);
  release_lever(-1);
  assert(master_scratch_phase == master_scratch_phase_t::returning);
  assert(sampler_audio_t::direction == 1);

  sampler_audio_t::reached = true;
  M5.now = 80;
  service_master_scratch(M5.now);
  assert(master_scratch_phase == master_scratch_phase_t::neutral);
  assert(master_scratch_neutral_until_msec == 140);

  M5.now = 139;
  service_master_scratch(M5.now);
  assert(master_scratch_active);
  M5.now = 140;
  service_master_scratch(M5.now);
  assert(!master_scratch_active);
  assert(!sampler_audio_t::requested);

  // The opposite first movement uses the same state machine naturally.
  M5.now = 300;
  set_master_scratch_lever(1, true);
  assert(master_scratch_phase == master_scratch_phase_t::outward);
  assert(sampler_audio_t::direction == 1);
  std::puts("PASS: Scratch input edges retarget immediately and neutral retains the window for 60 ms");
}}
"""

with tempfile.TemporaryDirectory(prefix="sampler-scratch-test-") as directory:
    source = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    source.write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-O2", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    app_source = pathlib.Path(directory) / "app_test.cpp"
    app_binary = pathlib.Path(directory) / "app_test"
    app_source.write_text(app_harness)
    subprocess.run(["c++", "-std=c++17", "-O2", str(app_source), "-o", str(app_binary)], check=True)
    subprocess.run([str(app_binary)], check=True)
