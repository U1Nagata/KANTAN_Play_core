# Chiptune Drums

Original 8-bit-style drum one-shots for KANTAN Sampler. The WAV files are
16-bit PCM, mono, 48 kHz, and use intentional 8-bit amplitude quantization and
11.025 kHz sample-and-hold to give them a compact chip-style character.

The eight sounds are embedded in the firmware as the temporary **Chiptune**
Pattern Beat kit. Open the Beat page, then select **Beat > Select Kit >
Chiptune**. The existing Pattern, tempo, and loop length remain untouched.

They may also be copied to the SD card's `/sampler/samples/` folder and assigned
to regular Sampler Pads. Suggested pad order follows the numeric prefixes:
Kick, Snare, Clap, Closed Hat, Open Hat, Tom, Rim, Cowbell.

The files are generated solely from the deterministic synthesis script at
`tools/generate_chiptune_drums.mjs`; rerun it to recreate the exact kit.
