#!/usr/bin/env python3
"""Create compact 48 kHz mono Pattern Beat assets from the raw drum sources."""

from pathlib import Path
import subprocess
import wave


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "Sample_Sound"
DESTINATION = SOURCE / "Acoustic_Drums"

# Pattern Beat voices are capped at two seconds. Keep the useful attack and
# enough body for cymbals without embedding the much longer studio originals.
SOURCES = (
    ("Tom_Low_Acc.wav", "01_Acoustic_Tom_Low.wav", 900),
    ("Tom_Mid_Acc.wav", "02_Acoustic_Tom_Mid.wav", 800),
    ("Tom_High_Acc", "03_Acoustic_Tom_High.wav", 700),
    ("Snare_Rim_Acc.wav", "04_Acoustic_Rim.wav", 400),
    ("Crash.wav", "05_Acoustic_Crash.wav", 1800),
    ("Ride_Acc.wav", "06_Acoustic_Ride.wav", 1800),
    ("Saker.wav", "07_Acoustic_Shaker.wav", 650),
)


def convert(source_name: str, destination_name: str, milliseconds: int) -> None:
    source = SOURCE / source_name
    destination = DESTINATION / destination_name
    temporary = destination.with_suffix(".full.wav")
    subprocess.run(
        ["afconvert", str(source), "-o", str(temporary), "-f", "WAVE",
         "-d", "LEI16@48000", "-c", "1", "--no-filler"],
        check=True,
    )
    with wave.open(str(temporary), "rb") as decoded:
        frame_count = min(decoded.getnframes(), decoded.getframerate() * milliseconds // 1000)
        frames = decoded.readframes(frame_count)
        with wave.open(str(destination), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(48000)
            output.writeframes(frames)
    temporary.unlink()


def main() -> None:
    DESTINATION.mkdir(exist_ok=True)
    for source, destination, milliseconds in SOURCES:
        convert(source, destination, milliseconds)


if __name__ == "__main__":
    main()
