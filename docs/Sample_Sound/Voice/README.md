# Voice preset Beat Anchors

The spoken Voice presets retain the leading consonant in their PCM and place
the first stable voiced/vowel region on the Note Grid through the built-in
`beat_anchor_ms` metadata.

| Preset | File | Beat Anchor |
|---|---|---:|
| VOICE 1 | `v_1.wav` | 0ms |
| VOICE 2 | `v_2.wav` | 40ms |
| VOICE 3 | `v_3.wav` | 70ms |
| VOICE 4 | `v_4.wav` | 65ms |
| GO | `v_Go.wav` | 25ms |
| HA | `v_Ha.wav` | 75ms |
| HEY | `v_Hey.wav` | 40ms |
| YEAH | `v_Yeah.wav` | 20ms |
| HAI | `v_jp_Hai.wav` | 55ms |

These initial values were selected from 5ms energy and 25ms periodicity
windows, then rounded to 5ms. They are authored defaults, not destructive WAV
edits; the Sample Edit `Beat` control can override them per Pad at 1ms
resolution. `CAT` and `SHEEP` remain without authored anchors because an animal
call has no unambiguous linguistic consonant-to-vowel boundary.
