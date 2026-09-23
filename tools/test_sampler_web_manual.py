#!/usr/bin/env python3
"""Keep the Sampler Web Manual entry and QR screen wired to the public URL."""

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/sampler/sampler_app.cpp").read_text()
LOCALE = (ROOT / "main/sampler/sampler_menu_locale.hpp").read_text()

root_start = SOURCE.index("static const sampler_menu_item_t* menu_root_items_for_current_page")
root_end = SOURCE.index("static constexpr const sampler_menu_item_t menu_synthesizer_items", root_start)
root = SOURCE[root_start:root_end]

assert root.index('{ "System"') < root.index('{ "WEB Manual"')
assert 'items[7] = { "WEB Manual"' in root
assert 'sampler_web_manual_url = "https://kantan-play.com/sampler/manual/"' in SOURCE
assert "d.qrcode(sampler_web_manual_url" in SOURCE
assert 'd.drawString("https://kantan-play.com"' in SOURCE
assert 'd.drawString("/sampler/manual/"' in SOURCE
assert "if (web_manual_qr_active)" in SOURCE
assert '{ "WEB Manual", "WEBマニュアル" }' in LOCALE

back_start = SOURCE.index("static void menu_back(void)")
back_end = SOURCE.index("if (menu_file_preview_owned)", back_start)
manual_back = SOURCE[back_start:back_end]
assert "M5.Display.fillScreen(0x08080Cu);" in manual_back
assert manual_back.index("fillScreen") < manual_back.index("draw_menu_header(true)")

print("PASS: Web Manual QR opens bilingually and clears the full LCD before restoring the menu")
