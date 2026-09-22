#!/usr/bin/env python3
"""Guard Learn support for every meaningful local Sampler control."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/sampler/sampler_app.cpp").read_text()


targets = (
    "side_left", "side_right",
    "lever_down", "lever_up",
    "dial_1_left", "dial_1_right", "dial_1_push",
    "dial_2_left", "dial_2_right", "dial_2_push",
    "jog_left", "jog_right",
)
for target in targets:
    assert f"midi_assign_target_t::{target}" in SOURCE, f"missing target: {target}"

for mask in ("SIDE_1", "SIDE_2", "KNOB_L", "KNOB_R", "ENC1_PUSH", "ENC2_PUSH"):
    assert f"bb::{mask}" in SOURCE, f"Learn does not capture {mask}"

assert "learn_capture_encoder_target(encoder, delta)" in SOURCE
assert "process_encoder_delta(0, -1)" in SOURCE
assert "process_encoder_delta(1, -1)" in SOURCE
assert "process_encoder_delta(2, -1)" in SOURCE
assert "handle_performance_lever_control(mask, pressed)" in SOURCE
assert "prev_bitmask | assigned_local_control_mask" in SOURCE
assert SOURCE.count("midi_assign_target_valid(target)") >= 6
assert SOURCE.count("midi_assign_target_is_local_control(target)") >= 6

print("PASS: Input Assign learns side buttons, all dial motions/pushes, jog and lever directions")
