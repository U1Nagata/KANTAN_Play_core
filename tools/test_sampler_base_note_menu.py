#!/usr/bin/env python3
"""Keep the three synth Base Note pickers under Sound Source with GM preview."""

from pathlib import Path


APP = (Path(__file__).resolve().parents[1] / "main/sampler/sampler_app.cpp").read_text()


def section(start: str, end: str) -> str:
    return APP.split(start, 1)[1].split(end, 1)[0]


for part in ("melody", "bass", "chord"):
    root = section(
        f"static constexpr const sampler_menu_item_t menu_synth_{part}_items[] = {{",
        f"static constexpr const sampler_menu_item_t menu_synth_{part}_sound_items[] = {{",
    )
    sound = section(
        f"static constexpr const sampler_menu_item_t menu_synth_{part}_sound_items[] = {{",
        f"static constexpr const sampler_menu_item_t menu_synth_{part}_midi_items[] = {{",
    )
    assert '"Base Note"' not in root
    assert sound.count('"Base Note"') == 1
    assert sound.index('"KANTAN Synth"') < sound.index('"Base Note"')
    assert "menu_action_t::synth_pad_base_note_select" in sound
    assert "visibility_t::editable_sample" in sound
    assert f"menu_page = selecting_base_note ? menu_page_t::synth_{part}_sound" in APP

assert "menu_cursor = selecting_base_note ? (page_sound_select ? 3 : 2) : 0;" in APP
entry = section("case menu_action_t::synth_pad_base_note_select: {", "case menu_action_t::wifi_setup:")
assert "preview_synth_menu_selection();" in entry
preview = section("static void preview_synth_menu_selection(void)\n{", "static uint32_t menu_ktsynth_preview_pitch_q16")
assert "kit_edit_state == kit_edit_state_t::select_external_pad_base_note" in preview
assert "&& synth_sound_select_active" in preview
assert "send_sam_midi(0xC0 | synth_menu_preview_channel, 80);" in preview
assert "synth_menu_preview_note = menu_cursor;" in preview
assert "synth_menu_preview_stop_msec = M5.millis() + preview_ms;" in preview
assert "const uint32_t preview_ms = 450;" in preview

print("PASS: three synth Base Note menus are nested and preview LD Square for 450 ms")
