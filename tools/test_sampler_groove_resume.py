#!/usr/bin/env python3
"""Guard Tempo & Groove naming and immediate Groove resume persistence."""

from test_sampler_pcm_render import ROOT, function


source = (ROOT / "main/sampler/sampler_app.cpp").read_text()

assert '{ "Tempo & Groove", menu_item_kind_t::submenu' in source
assert 'case menu_page_t::beat_tempo: return "Tempo & Groove";' in source

save = function(source, "static void save_sampler_groove_resume(")
load = function(source, "static void load_sampler_groove_resume(")
for key in ('"quantize"', '"noteGridIndex"', '"swingAmount"'):
    assert key in save, key
    assert key in load, key

menu_set = function(source, "static void menu_value_set(")
assert menu_set.count("menu_groove_save_pending = true") == 3
service = function(source, "static void service_menu_settings_save(")
assert "save_sampler_groove_resume()" in service
assert "load_sampler_groove_resume()" in source[source.index("const bool resumed = load_resume_kit();"):]

print("PASS: Tempo & Groove menu and Quantize/Note Grid/Swing reboot persistence")
