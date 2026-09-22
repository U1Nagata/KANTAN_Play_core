#!/usr/bin/env python3
"""Keep every Sampler QR screen dismissible by any button or screen tap."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/sampler/sampler_app.cpp").read_text()

menu_start = SOURCE.index("static bool menu_handle_input(uint32_t pressed_edge)")
menu_end = SOURCE.index("static bool finish_learn_target", menu_start)
menu_input = SOURCE[menu_start:menu_end]

bitmask_start = SOURCE.index("static void process_bitmask(")
bitmask_end = SOURCE.index("static void process_touch(", bitmask_start)
bitmask = SOURCE[bitmask_start:bitmask_end]

touch_start = bitmask_end
touch_end = SOURCE.index("//-------------------------------------------------------------------------\n// サンプルの読み込み", touch_start)
touch = SOURCE[touch_start:touch_end]

assert "if (web_manual_qr_active || wifi_setup_qr_active)" in menu_input
assert "if (pressed_edge) { menu_back(); }" in menu_input

file_server_block = bitmask[bitmask.index("if (wifi_file_server_qr_active)") :]
file_server_block = file_server_block[:file_server_block.index("if (performance_record_confirm_active)")]
assert "if (pressed_edge)" in file_server_block
assert 'stop_file_server_session("button")' in file_server_block
assert "encoder_pushes" not in file_server_block

assert 'stop_file_server_session("touch")' in touch
assert "if (web_manual_qr_active || wifi_setup_qr_active)" in touch
assert "if (pressed) { menu_back(); }" in touch

print("PASS: Wi-Fi Setup, File Editor and Web Manual QR screens close from any button or tap")
