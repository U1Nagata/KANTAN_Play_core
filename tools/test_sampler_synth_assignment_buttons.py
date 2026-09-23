#!/usr/bin/env python3
"""Guard the Synth edit buttons' assignment badges and targeted refresh."""

from pathlib import Path


APP = (Path(__file__).resolve().parents[1] / "main/sampler/sampler_app.cpp").read_text()


def function(signature: str) -> str:
    start = APP.index(signature)
    brace = APP.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (APP[end] == "{") - (APP[end] == "}")
        end += 1
    return APP[start:end]


assignment = function(
    "static bool synth_pad_assignment_matches(performance_page_t page, uint8_t pad)"
)
assert "settings.source == synth_tone_source_t::pad && settings.pad == pad" in assignment

background = function("static uint32_t edit_pad_background(int pad)")
draw = function("static void draw_pad_content(m5gfx::LovyanGFX& d, int pad,")
for page in ("melody", "chord", "bass"):
    match = f"synth_pad_assignment_matches(performance_page_t::{page}, (uint8_t)edit_pad)"
    assert match in background
    assert match in draw

assert "if (edit_synth_page && number >= 1 && number <= 3)" in draw
assert "if (synth_assignment)" in draw
assert "d.fillCircle(badge_x, badge_y, 6, accent)" in draw
assert "d.drawCircle(badge_x, badge_y, 5," in draw

toggle = function("static bool toggle_edit_synth_assignment(performance_page_t page, uint32_t now)")
assert "const bool currently_assigned = synth_pad_assignment_matches(page," in toggle

handler = function("static void handle_edit_function_pad(")
assert "if (toggle_edit_synth_assignment(page, now))" in handler
assert "request_pad_draw(button_pad);" in handler
assert "update_pad_led(button_pad);" in handler

print("PASS: Synth assignment badges and one-button display/LED refresh")
