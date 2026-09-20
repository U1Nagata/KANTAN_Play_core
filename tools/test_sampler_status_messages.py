#!/usr/bin/env python3
"""Regression check for timed Sampler status notices."""

from pathlib import Path


SOURCE = (Path(__file__).resolve().parents[1] / "main/sampler/sampler_app.cpp").read_text()


def main() -> None:
    assert "static void service_status_message_timeout(uint32_t now)" in SOURCE
    assert "clear_status_message(true);" in SOURCE.split(
        "static void service_status_message_timeout(uint32_t now)", 1)[1].split(
        "static void draw_learn_overlay", 1)[0]
    assert "service_status_message_timeout(msec);" in SOURCE

    # The performance overlay restores the Wave Canvas after the common
    # timeout service clears its message; it must not retain a second,
    # mode-specific expiration path.
    overlay = SOURCE.split("static void service_performance_status_overlay(uint32_t now)\n{", 1)[1].split(
        "static void draw_busy_status_dots", 1)[0]
    assert "status_message_until = 0;" not in overlay
    assert "request_wave_draw();" in overlay
    print("PASS: timed status notices expire on menu and performance surfaces")


if __name__ == "__main__":
    main()
