# KTSAMPLE file format

`.ktsample` is the portable, self-contained single-sample format used by
KANTAN Sampler. A plain WAV or MP3 contains source audio only. KTSAMPLE keeps
that audio together with the non-destructive Pad edit settings needed to make
the sound play musically on another device or in another Project.

KTSAMPLE does not replace WAV import, Project, or `.ktkit`:

- WAV/MP3 import starts with default edit settings.
- KTSAMPLE imports one authored sound and all of its Pad settings.
- KTKIT stores a complete twelve-Pad Sample Kit or Beat Kit.
- Project stores the complete performance state.

## Version 1 layout

All integer fields are unsigned little-endian. Offsets are absolute file
offsets. PCM is stored decoded so the result is independent of the original
WAV/MP3 codec and remains cheap to load on the device.

| Header offset | Size | Field |
|---:|---:|---|
| 0 | 8 | ASCII magic `KTSAMPLE` |
| 8 | 2 | container format version (`1`) |
| 10 | 2 | header size (`32`) |
| 12 | 4 | UTF-8 JSON manifest size |
| 16 | 4 | absolute PCM payload offset |
| 20 | 4 | PCM payload byte size |
| 24 | 4 | complete file size |
| 28 | 4 | CRC32 of manifest followed by PCM payload |

The manifest begins immediately after the header. The PCM payload begins at
`pcmOffset`, which must equal `headerSize + manifestSize`. Version 1 has no
padding or additional sections. The payload is signed mono PCM16
little-endian, and `pcmBytes` must equal `frameCount * 2`.

## Version 1 manifest

The root object must contain `formatVersion: 1`, `kind: "sample"`, and every
required field below. Frame ranges use an inclusive start and exclusive end.

| Key | Type | Constraint / meaning |
|---|---|---|
| `name` | string | UTF-8 display name, 1–63 bytes |
| `sampleRate` | integer | 8,000–96,000 Hz |
| `frameCount` | integer | positive and consistent with `pcmBytes` |
| `startFrame` | integer | `0 <= startFrame < endFrameExclusive` |
| `endFrameExclusive` | integer | `<= frameCount` |
| `volumeQ8` | integer | `0–512`; `256` is unity |
| `pitchQ8` | integer | `32–2048`; `256` is original speed/pitch |
| `baseNote` | integer | MIDI note `0–127` |
| `baseNoteAuto` | boolean | whether Base Note may be re-analysed after trim |
| `reverse` | boolean | reverse playback; disables Beat Anchor at runtime |
| `hold` | boolean | gate follows Pad release |
| `choke` | boolean | participates in the Sample choke group |
| `repeatEnabled` | boolean | enables Whole Sample or Note Grid repeat |
| `repeatMode` | string | `"whole"` or `"grid"`; ignored when disabled |
| `repeatGridHalfSteps` | integer | Note Grid width in half-step units |
| `beatAnchorEnabled` | boolean | enables the musical onset marker |
| `beatAnchorFrame` | integer | `startFrame <= frame < endFrameExclusive` |
| `synthSustainMode` | string | `"off"`, `"auto"`, or `"manual"` |
| `synthLoopStartFrame` | integer | manual sustain-loop start |
| `synthLoopEndFrameExclusive` | integer | manual sustain-loop end |
| `synthLoopCrossfadeFrames` | integer | at most one quarter of loop length |
| `synthAttackMs` | integer | `0–5000` |
| `synthReleaseMs` | integer | `10–10000` |
| `synthDelay100us` | integer | envelope delay in 0.1ms units |
| `synthHoldMs` | integer | `0–5000` |
| `synthDecayMs` | integer | `0–60000` |
| `synthSustainLevelQ15` | integer | `0–32768` |
| `synthTuneCents` | integer | `-100–100` |

Optional Chop fields are `chopGroup`, `chopIndex`, `chopCount`,
`chopNativeLoopMs`, and `chopTempoQ8`. A reader must either validate the
complete group metadata or clear it; a single imported slice must never retain
a dangling group relationship.

### Beat Anchor semantics

`startFrame` is where audible playback starts. `beatAnchorFrame` is the point
that belongs on the quantized Note Grid. The range from Start to Beat Anchor is
pre-roll: it begins before the event so a leading consonant or pickup remains
audible while the vowel/transient lands exactly on the beat. A disabled Anchor
does not shift scheduling. Reverse playback keeps the authored value but does
not use it until Reverse is turned off.

For example, a spoken “HA” can start with its `/h/` at frame 0 and place the
first stable `/a/` frame at `beatAnchorFrame`. Splitting a phrase into several
independently quantized words requires Chop; each resulting KTSAMPLE then has
its own Anchor.

## Validation, safety, and atomic writes

Readers must reject unknown magic/version, malformed or oversized JSON,
integer overflow, inconsistent offsets or PCM length, invalid edit ranges,
and CRC failure before replacing a Pad. Version 1 files must not exceed the
same decoded single-Pad budget used by WAV/MP3 import.

Writers use `<name>.ktsample.tmp`, close and re-open it for complete validation,
then replace the destination using the same rename/backup sequence as KTKIT.
Interrupted writes therefore leave the previous valid file recoverable.
CRC32 uses the IEEE polynomial `0xEDB88320`, initial state `0xFFFFFFFF`, and a
final XOR with `0xFFFFFFFF`, matching KTKIT.

## Compatibility

Unknown additive manifest keys may be ignored when all required Version 1
fields remain valid. A change to the header, PCM encoding, required field
meaning, or validation rules requires a new container version. Future payload
encodings require a versioned header extension rather than guessing from the
filename.
