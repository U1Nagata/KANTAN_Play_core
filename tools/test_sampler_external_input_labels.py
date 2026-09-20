#!/usr/bin/env python3
"""Regression check for Sampler external-input terminology."""

from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "main/sampler/sampler_app.cpp").read_text()
PRODUCT_SPEC = (Path(__file__).resolve().parents[1]
                / "docs/development/sampler/product-spec.md").read_text()
REFERENCE = (Path(__file__).resolve().parents[1]
             / "docs/docs/ja/sampler/reference/external-devices.md").read_text()


def main() -> None:
    for text in (SOURCE, PRODUCT_SPEC, REFERENCE):
        assert "USB MIDI Controller" not in text
        assert "USB MIDI Computer" not in text

    for label in ("USB MIDI Device", "USB MIDI PC"):
        assert label in SOURCE
        assert label in PRODUCT_SPEC
        assert label in REFERENCE

    assert 'usb_midi_host: return "USB MIDI Device"' in SOURCE
    assert 'usb_midi_device: return "USB MIDI PC"' in SOURCE
    print("PASS: Sampler external-input labels use USB MIDI Device and USB MIDI PC")


if __name__ == "__main__":
    main()
