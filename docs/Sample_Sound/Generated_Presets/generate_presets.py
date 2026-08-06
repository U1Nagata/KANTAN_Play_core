#!/usr/bin/env python3
"""Generate the small, original factory preset WAV set.

The sounds are deliberately synthetic and deterministic.  They are source
material for the sampler/synth, not recordings copied from a sound library.
"""

import math
import random
import struct
import wave
from pathlib import Path


# Factory sources trade a little top end for Flash headroom.  The playback
# engine resamples arbitrary source rates, while 18 kHz still preserves the
# character of these deliberately simple waveforms.
RATE = 18000
SYNTH_RATE = 48000
SYNTH_BASE_NOTE = 60
SYNTH_BASE_HZ = 261.625565  # C4
PEAK = 0.78 * 32767
OUT = Path(__file__).resolve().parent


def osc(kind, phase):
    phase %= 1.0
    if kind == "sine":
        return math.sin(phase * math.tau)
    if kind == "triangle":
        return 4.0 * abs(phase - 0.5) - 1.0
    if kind == "square":
        return 1.0 if phase < 0.5 else -1.0
    if kind == "saw":
        return phase * 2.0 - 1.0
    return 0.0


def render(name, seconds, sample_fn, rate=RATE):
    global RATE
    RATE = rate
    frames = max(1, round(seconds * RATE))
    values = [max(-1.0, min(1.0, sample_fn(i / RATE, i, frames))) for i in range(frames)]
    # Tiny fades prevent clicks without obscuring drum attacks.
    fade = min(64, frames // 8)
    for i in range(fade):
        values[i] *= i / fade
        values[-1 - i] *= i / fade
    with wave.open(str(OUT / name), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(b"".join(struct.pack("<h", round(v * PEAK)) for v in values))


def exp_env(t, decay):
    return math.exp(-t * decay)


def pitched(kind, freq, decay=1.5, harmonics=()):
    def sample(t, _i, _n):
        value = osc(kind, t * freq)
        for multiple, level in harmonics:
            value += osc("sine", t * freq * multiple) * level
        return value * exp_env(t, decay) / (1.0 + sum(abs(level) for _, level in harmonics))
    return sample


def bandlimited(kind, freq, t):
    """Additive saw/square/triangle with no partial above Nyquist."""
    limit = max(1, int((RATE * 0.48) / freq))
    value = 0.0
    weight = 0.0
    for harmonic in range(1, limit + 1):
        if kind in ("square", "triangle") and harmonic % 2 == 0:
            continue
        level = 1.0 / harmonic
        if kind == "triangle":
            level = ((-1.0) ** ((harmonic - 1) // 2)) / (harmonic * harmonic)
        value += math.sin(math.tau * freq * harmonic * t) * level
        weight += abs(level)
    return value / max(1.0, weight * 0.72)


def synth_source(kind, attack_ms, harmonics=(), transient=0.0, detune=0.0):
    """Short C4 source: attack followed by a stable region intended to loop."""
    def sample(t, i, _n):
        attack = min(1.0, t * 1000.0 / max(1, attack_ms))
        if kind in ("saw", "square", "triangle"):
            value = bandlimited(kind, SYNTH_BASE_HZ, t)
        else:
            value = osc("sine", SYNTH_BASE_HZ * t)
        if detune:
            value = (value + osc("sine", SYNTH_BASE_HZ * (1.0 + detune) * t) * 0.24) / 1.24
        for multiple, level in harmonics:
            value += math.sin(math.tau * SYNTH_BASE_HZ * multiple * t) * level
        if transient:
            noise = math.sin(i * 12.9898 + 78.233) * 43758.5453
            noise = (noise - math.floor(noise)) * 2.0 - 1.0
            value += noise * transient * math.exp(-t * 38.0)
        return value * attack / (1.0 + sum(abs(level) for _, level in harmonics))
    return sample


def voice_source(vowels, attack_ms=45):
    def sample(t, _i, _n):
        attack = min(1.0, t * 1000.0 / attack_ms)
        fundamental = bandlimited("saw", SYNTH_BASE_HZ, t) * 0.32
        colour = sum(math.sin(math.tau * f * t) * level for f, level in vowels)
        return (fundamental + colour) * attack / 1.55
    return sample


def noise_hit(seed, decay, tone=0.0):
    rng = random.Random(seed)
    state = 0.0
    def sample(t, _i, _n):
        nonlocal state
        raw = rng.uniform(-1.0, 1.0)
        state += (raw - state) * (0.12 + tone * 0.7)
        return (raw * (1.0 - tone) + state * tone) * exp_env(t, decay)
    return sample


def kick(base, sweep, click):
    phase = 0.0
    rng = random.Random(round(base * 100))
    def sample(t, _i, _n):
        nonlocal phase
        freq = base + sweep * math.exp(-t * 28.0)
        phase += freq / RATE
        body = math.sin(phase * math.tau) * exp_env(t, 8.0)
        return body + rng.uniform(-1, 1) * click * exp_env(t, 90.0)
    return sample


def tom(freq, bend):
    phase = 0.0
    def sample(t, _i, _n):
        nonlocal phase
        phase += (freq + bend * math.exp(-t * 14.0)) / RATE
        return (math.sin(phase * math.tau) + 0.22 * math.sin(phase * math.tau * 2.02)) * exp_env(t, 6.0) / 1.22
    return sample


def metallic(freqs, decay):
    def sample(t, _i, _n):
        return sum(math.sin(math.tau * f * t) for f in freqs) / len(freqs) * exp_env(t, decay)
    return sample


def pluck(freq, brightness):
    rng = random.Random(round(freq))
    period = max(2, round(RATE / freq))
    ring = [rng.uniform(-1, 1) for _ in range(period)]
    cursor = 0
    def sample(t, _i, _n):
        nonlocal cursor
        value = ring[cursor]
        nxt = (cursor + 1) % period
        ring[cursor] = (ring[cursor] * brightness + ring[nxt] * (1.0 - brightness)) * 0.996
        cursor = nxt
        return value * exp_env(t, 0.8)
    return sample


def formant(freq, vowels):
    def sample(t, _i, _n):
        carrier = osc("saw", t * freq)
        colour = sum(math.sin(math.tau * f * t) * level for f, level in vowels)
        return (carrier * 0.35 + colour) * exp_env(t, 1.1) / 1.7
    return sample


def sweep(start, end, decay, noise=0.0, seed=1):
    phase = 0.0
    rng = random.Random(seed)
    def sample(t, _i, n):
        nonlocal phase
        x = min(1.0, t / (n / RATE))
        phase += (start * ((end / start) ** x)) / RATE
        return (math.sin(phase * math.tau) * (1 - noise) + rng.uniform(-1, 1) * noise) * exp_env(t, decay)
    return sample


PRESETS = [
    ("Kick_Deep.wav", 0.55, kick(48, 135, 0.10)),
    ("Kick_Tight.wav", 0.34, kick(62, 190, 0.20)),
    ("Snare_Soft.wav", 0.42, noise_hit(11, 13, .15)),
    ("Snare_Bright.wav", 0.32, noise_hit(12, 18, .02)),
    ("Tom_Low.wav", 0.55, tom(92, 42)),
    ("Tom_High.wav", 0.40, tom(168, 65)),
    ("HiHat_Closed.wav", 0.18, noise_hit(21, 32, .0)),
    ("HiHat_Open.wav", 0.62, noise_hit(22, 7.5, .0)),
    ("Cymbal_Crash.wav", 0.90, noise_hit(31, 3.8, .05)),
    ("Cymbal_Ride.wav", 0.75, metallic((431, 607, 863, 1151), 4.2)),
    ("Perc_Shaker.wav", 0.28, noise_hit(41, 15, .08)),
    ("Perc_Wood.wav", 0.30, pitched("sine", 410, 16, ((2.7, .4),))),
    ("Bass_Sub.wav", 0.22, synth_source("sine", 8, ((2, .16),)), SYNTH_RATE),
    ("Bass_Saw.wav", 0.22, synth_source("saw", 6, ((2, .08),)), SYNTH_RATE),
    ("Lead_Square.wav", 0.22, synth_source("square", 12, ((2, .08),)), SYNTH_RATE),
    ("Lead_Sync.wav", 0.22, synth_source("saw", 15, ((2.01, .24), (3.98, .10))), SYNTH_RATE),
    ("Pad_Warm.wav", 0.36, synth_source("triangle", 170, ((2, .16),), detune=.004), SYNTH_RATE),
    ("Pad_Airy.wav", 0.36, synth_source("sine", 180, ((2.004, .22), (3.997, .10)), detune=.006), SYNTH_RATE),
    ("Keys_EP.wav", 0.28, synth_source("sine", 7, ((2, .34), (3, .12)), transient=.05), SYNTH_RATE),
    ("Keys_Organ.wav", 0.28, synth_source("sine", 18, ((2, .38), (3, .20), (4, .08))), SYNTH_RATE),
    ("Pluck_Nylon.wav", 0.28, synth_source("triangle", 4, ((2, .16),), transient=.18), SYNTH_RATE),
    ("Pluck_Harp.wav", 0.28, synth_source("sine", 3, ((2, .26), (3, .10)), transient=.12), SYNTH_RATE),
    ("Mallet_Bell.wav", 0.30, synth_source("sine", 3, ((2.76, .25), (5.40, .12)), transient=.05), SYNTH_RATE),
    ("Mallet_Marimba.wav", 0.30, synth_source("sine", 3, ((4, .24), (10, .07)), transient=.16), SYNTH_RATE),
    ("Voice_Ah.wav", 0.30, voice_source(((700, .52), (1220, .28), (2600, .10))), SYNTH_RATE),
    ("Voice_Ooh.wav", 0.30, voice_source(((330, .56), (870, .24), (2240, .09))), SYNTH_RATE),
    ("FX_Riser.wav", 0.95, sweep(90, 2400, .35, .18, 51)),
    ("FX_Fall.wav", 0.95, sweep(2200, 75, .7, .12, 52)),
    ("Wave_Sine.wav", 0.16, synth_source("sine", 2), SYNTH_RATE),
    ("Wave_Saw.wav", 0.16, synth_source("saw", 2), SYNTH_RATE),
]


if __name__ == "__main__":
    for preset in PRESETS:
        render(*preset)
    print(f"generated {len(PRESETS)} presets in {OUT}")
