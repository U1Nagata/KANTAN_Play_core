# Generated factory presets

`generate_presets.py` creates two small, original WAV sources for each built-in
sample category. The files are deterministic mono, 16-bit PCM and may be
regenerated without downloading third-party assets. One-shot sounds use 18 kHz;
the short loopable synth sources use 48 kHz and are uniformly tuned to C4
(MIDI note 60).

Categories: Kick, Snare, Tom, HiHat, Cymbal, Percussion, Bass, Lead, Pad, Keys,
Pluck, Mallet, Voice, FX, and Waveform.

Run from the repository root:

```sh
python3 docs/Sample_Sound/Generated_Presets/generate_presets.py
```

When a WAV changes, also touch or edit `main/sampler/sampler_samples.hpp` before
an incremental PlatformIO build. The assembler `.incbin` dependency is not
automatically tracked by PlatformIO.
