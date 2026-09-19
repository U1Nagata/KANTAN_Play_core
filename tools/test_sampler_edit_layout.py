#!/usr/bin/env python3
"""Verify Sample Edit entry behavior and Synth-page Pad assignments."""

import pathlib


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


enter_edit = function(APP, "static void enter_edit(int pad)\n{")
handler = function(APP, "static void handle_edit_function_pad(int pad)")
draw = function(APP, "static void draw_pad_content(")

assert "if (loop_playing) { loop_toggle_play(); }" in enter_edit

for mapping in (
    "case 5: edit_param = 12; break;",
    "case 6: edit_param = 9; break;",
    "case 7: edit_param = 13; break;",
):
    assert mapping in handler

for label in ('label = "Atk";', 'label = "Rel";', 'label = "Tune";', 'label = "Sus";'):
    assert label in draw

assert 'label = "Beat";' in draw
assert "case 3:" in handler
assert "mark_edit_beat_anchor_from_preview()" in handler
assert "edit_param = 14;" in handler
assert '"BEAT"' in APP

print("PASS: Sample Edit transport, Beat Anchor and Synth control layout")
