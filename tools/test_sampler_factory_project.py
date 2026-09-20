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
    project_body = factory.group(1)
    sample_kit = re.search(
        r"static void load_factory_start_sample_kit\(void\)\s*\{(.*?)\n\}",
        source,
        re.DOTALL,
    )
    assert sample_kit, "factory sample Kit function"
    body = sample_kit.group(1)

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
        assert line in project_body, line

    assert "load_factory_start_sample_kit();" in project_body
    assert "load_builtin_beat_pattern(beat_preset_disco)" in project_body
    assert "audio_beat.loop_repeats = 2;" in project_body
    assert "beat_drum_kit = beat_drum_kit_t::dance;" in project_body
    assert "load_factory_ktsynth" not in project_body

    reset = re.search(
        r"static void reset_factory_sample_kit\(void\)\s*\{(.*?)\n\}",
        source,
        re.DOTALL,
    )
    assert reset, "Reset Kit function"
    assert "load_factory_start_sample_kit();" in reset.group(1)
    assert "reset_default_or_builtin_kit" not in source
    print("PASS: DISCO Beat Project and Reset Kit share one factory sample Kit")


if __name__ == "__main__":
    main()
