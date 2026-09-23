#!/usr/bin/env python3
"""Guard Tune on both sustained and non-sustained Sampler playback paths."""

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


for signature in (
    "static bool play_sample_sustain_voice(int pad, bool auto_release)\n{",
    "static void play_sample_once(int pad, uint32_t source_offset_frames = 0,",
    "static void play_sample_whole_loop(int pad)\n{",
):
    path = function(signature)
    assert "sample_synth_pitch_scale_q12(slot)" in path, signature
    assert "sampler_audio_t::setVoicePitchScaleQ12((uint8_t)pad" in path, signature

for signature in (
    "static bool play_selected_menu_file_preview(void)\n{",
    "static bool shared_sample_page_pad_press(int pad)\n{",
    "static bool play_sound_page_preview(void)\n{",
    "static void preview_synth_menu_selection(void)\n{",
):
    path = function(signature)
    assert "sampler_audio_t::setVoicePitchScaleQ12(menu_preview_voice" in path, signature
    assert "sample_synth_pitch_scale_q12(" in path, signature

edit = function("static void edit_value_add(int diff)\n{")
tune = edit.split("if (edit_param == 13) {", 1)[1].split("if (edit_param == 10) {", 1)[0]
assert "set_sample_synth_tune(slot," in tune
assert "edit_source_page != performance_page_t::drum" in tune
assert "sampler_audio_t::isPlaying((uint8_t)edit_pad)" in tune
assert "sampler_audio_t::setVoicePitchScaleQ12((uint8_t)edit_pad" in tune

print("PASS: Sustain, one-shot, Whole Sample and live Tune edits use the same pitch scale")
