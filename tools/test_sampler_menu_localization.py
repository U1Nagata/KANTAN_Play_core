#!/usr/bin/env python3
"""Keep the Sampler's fixed menu surface bilingual."""

import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "main/sampler/sampler_app.cpp").read_text()
LOCALE = (ROOT / "main/sampler/sampler_menu_locale.hpp").read_text()


def block(start: str, end: str) -> str:
    return SOURCE[SOURCE.index(start):SOURCE.index(end, SOURCE.index(start))]


translations = dict(re.findall(r'\{ "([^"]+)", "([^"]+)" \}', LOCALE))
menu_definitions = block("static const sampler_menu_item_t* menu_root_items_for_current_page", "static bool menu_visible")
labels = set(re.findall(r'\{ "([^"]+)",\s+menu_item_kind_t::', menu_definitions))

# Protocol, format and product names intentionally remain language-neutral.
language_neutral = {
    "Wi-Fi", "WPS", "LED", "General MIDI", "USB MIDI Device", "USB MIDI PC",
    "USB Keyboard", "BLE MIDI", "USB Gamepad",
}
missing = sorted(labels - translations.keys() - language_neutral)
assert not missing, f"fixed menu labels missing Japanese text: {missing}"

page_titles = block("static const char* menu_page_title", "static menu_page_t menu_parent_page")
fixed_titles = set(re.findall(r'title = "([^"]+)"', page_titles)) | {"Menu"}
missing_titles = sorted(fixed_titles - translations.keys() - language_neutral)
assert not missing_titles, f"menu page titles missing Japanese text: {missing_titles}"

assert "sampler_menu_text(items[index].label)" in SOURCE
assert 'static constexpr const char* langs[] = { "English", "日本語" };' in SOURCE
assert 'return sampler_menu_text(off_on[index ? 1 : 0]);' in SOURCE
assert 'd.drawString(sampler_menu_text("Back")' in SOURCE

for english, japanese in {
    "Beat Setting": "ビート設定",
    "Sample Setting": "サンプル設定",
    "Bass Setting": "ベース設定",
    "Melody Setting": "メロディ設定",
    "Chord Setting": "コード設定",
    "Rec": "演奏記録",
    "Clear Rec": "演奏記録を消去",
    "Learn": "割り当て設定",
}.items():
    assert translations.get(english) == japanese, f"unexpected Japanese label for {english}"

print(f"PASS: {len(labels)} fixed menu labels and {len(fixed_titles)} page titles are bilingual")
