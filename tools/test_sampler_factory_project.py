#!/usr/bin/env python3
"""Regression checks for the saved factory Start Project settings."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "main/sampler/sampler_app.cpp"


def main() -> None:
    source = APP.read_text()
    factory = re.search(
        r"static void load_factory_start_project\(void\)\s*\{(.*?)\n\}",
        source,
        re.DOTALL,
    )
    assert factory, "factory Start Project function"
    body = factory.group(1)

    expected_samples = [
        (0, "AIR HORN", 178, 60, "off", "false", 0),
        (1, "JUMP", 100, 60, "automatic", "false", 0),
        (4, "TOM", 256, 56, "automatic", "false", 0),
        (5, "WOOD", 256, 67, "automatic", "false", 0),
        (6, "CHIN", 256, 57, "automatic", "false", 0),
        (7, "PIKO", 204, 78, "automatic", "false", 0),
        (8, "VOICE 1", 256, 60, "automatic", "true", 2496),
        (9, "VOICE 2", 282, 47, "automatic", "false", 0),
        (10, "VOICE 3", 256, 44, "automatic", "true", 2528),
        (11, "GO", 256, 60, "automatic", "false", 0),
    ]
    entries = re.findall(
        r'\{\s*(\d+),\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*'
        r"sample_sustain_mode_t::(\w+),\s*(true|false),\s*(\d+)\s*\}",
        body,
    )
    parsed = [
        (int(pad), name, int(volume), int(note), sustain, anchor, int(frame))
        for pad, name, volume, note, sustain, anchor, frame in entries
    ]
    assert parsed == expected_samples, parsed

    expected_lines = [
        "beat_volume = 70;",
        "sampler_volume = 80;",
        "loop_quantize_option_index = 2;",
        "loop_note_off_quantize_option_index = 3;",
        "loop_swing_amount = 0;",
        "fx_param[fx_filter_index] = -25;",
        "fx_param[fx_gater_index] = 55;",
        "fx_param[fx_crusher_index] = 10;",
        "fx_param[fx_repeat_index] = 2;",
        "synth_tone_source_t::general_midi, 81, 8,",
        "synth_tone_source_t::general_midi, 90, 8,",
        "synth_tone_source_t::general_midi, 38, 8,",
        "0, 0, 1, 75,",
        "0, 0, 0, 90, pitch_bend_range_t::semitone",
    ]
    for line in expected_lines:
        assert line in body, line

    assert "load_builtin_beat_pattern(beat_preset_disco)" in body
    assert "audio_beat.loop_repeats = 2;" in body
    assert "beat_drum_kit = beat_drum_kit_t::dance;" in body
    assert "load_factory_ktsynth" not in body
    print("PASS: factory Start Project matches 20260918_0153 project settings")


if __name__ == "__main__":
    main()
