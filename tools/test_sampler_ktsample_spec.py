#!/usr/bin/env python3
"""Check that the KTSAMPLE v1 contract covers audio and Pad edit state."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = (ROOT / "docs/development/sampler/ktsample-format.md").read_text()

for token in (
    "ASCII magic `KTSAMPLE`",
    "container format version (`1`)",
    "signed mono PCM16",
    "startFrame",
    "endFrameExclusive",
    "beatAnchorEnabled",
    "beatAnchorFrame",
    "synthAttackMs",
    "synthReleaseMs",
    "CRC32",
    ".ktsample.tmp",
):
    assert token in SPEC, token

assert "pre-roll" in SPEC
assert "Reverse playback keeps the authored value" in SPEC
print("PASS: KTSAMPLE v1 specifies PCM, Pad metadata, Beat Anchor and validation")
