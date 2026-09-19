#!/usr/bin/env python3
"""Regression checks for the built-in Voice one-shot preset collection."""

from pathlib import Path
import wave
import re


ROOT = Path(__file__).resolve().parents[1]
VOICE = ROOT / "docs/Sample_Sound/Voice"
SAMPLES = ROOT / "main/sampler/sampler_samples.hpp"

FILES = {
    "v_1.wav": ("VOICE 1", 0),
    "v_2.wav": ("VOICE 2", 40),
    "v_3.wav": ("VOICE 3", 70),
    "v_4.wav": ("VOICE 4", 65),
    "v_Go.wav": ("GO", 25),
    "v_Ha.wav": ("HA", 75),
    "v_Hey.wav": ("HEY", 40),
    "v_Yeah.wav": ("YEAH", 20),
    "v_jp_Hai.wav": ("HAI", 55),
}


def main() -> None:
    source = SAMPLES.read_text()
    total_bytes = 0
    for filename, (label, anchor_ms) in FILES.items():
        path = VOICE / filename
        assert path.is_file(), path
        with wave.open(str(path), "rb") as audio:
            assert audio.getnchannels() == 1, filename
            assert audio.getsampwidth() == 2, filename
            assert audio.getframerate() == 32000, filename
            assert 0 < audio.getnframes() <= 32000, filename
        total_bytes += path.stat().st_size
        assert f'"Voice/{filename}"' in source, filename
        assert f'{{ "{label}"' in source, label
        entry = re.search(
            rf'\{{\s*"{re.escape(label)}".*?sample_category_t::voice\s*,\s*(-?\d+)\s*\}}',
            source,
        )
        assert entry and int(entry.group(1)) == anchor_ms, (label, anchor_ms)

    assert total_bytes == 188514, total_bytes
    assert total_bytes < 200 * 1024
    assert source.count("sample_category_t::voice") >= len(FILES)
    app = (ROOT / "main/sampler/sampler_app.cpp").read_text()
    assert "source.beat_anchor_ms >= 0" in app
    assert "slot.beat_anchor_frame = slot.beat_anchor_enabled ? anchor : 0;" in app
    print("PASS: 9 Voice presets include authored vowel Beat Anchors")


if __name__ == "__main__":
    main()
