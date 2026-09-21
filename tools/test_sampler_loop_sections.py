#!/usr/bin/env python3
"""Regression checks for Loop Section switching, storage and ownership."""

from pathlib import Path
import re

from test_sampler_pcm_render import function


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "main/sampler/sampler_app.cpp"


def ordered(body: str, *needles: str) -> None:
    positions = [body.index(needle) for needle in needles]
    assert positions == sorted(positions), (needles, positions)


def main() -> None:
    source = APP.read_text()

    assert "static constexpr uint8_t loop_section_max = 4;" in source
    assert "static constexpr const size_t loop_event_max = 512;" in source
    assert "#define loop_events (loop_sections[current_loop_section].events)" in source
    assert "static loop_event_t loop_playback_events[loop_event_max];" in source
    assert "static_assert(sizeof(loop_event_t) <= 16" in source
    print("PASS: four sections each own a capped event vector; playback snapshots only one section")

    menu = re.search(
        r"static constexpr const sampler_menu_item_t menu_loop_items\[\]\s*=\s*\{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    assert menu, "Rec menu definition"
    ordered(
        menu.group(1),
        '"Quantize"',
        '"Clear Rec"',
        '"Add Loop Section"',
        '"Delete Loop Section"',
        '"Save as Beat"',
    )
    add_menu = re.search(
        r"static constexpr const sampler_menu_item_t menu_loop_section_add_items\[\]\s*=\s*\{(.*?)\n\};",
        source,
        re.DOTALL,
    )
    assert add_menu, "Add Loop Section submenu definition"
    ordered(add_menu.group(1), '"Duplicate Current Loop"', '"Add Blank Loop"')

    encoder = function(source, "static void process_encoder_delta(")
    assert "if (encoder == 2) { request_loop_section(delta); }" in encoder
    assert "else { page_selector_move(delta); }" in encoder
    assert encoder.index("if (edit_pad >= 0)") < encoder.index("request_loop_section(delta)")
    print("PASS: Rec menu hierarchy and dedicated idle ENC3 routing are fixed")

    request = function(source, "static void request_loop_section(")
    assert "? pending_loop_section : current_loop_section;" in request
    assert "if (target == current_loop_section)" in request
    assert "pending_loop_section = -1;" in request
    ordered(
        request,
        "loop_playing && loop_length_fixed",
        "pending_loop_section = (int8_t)target;",
        "activate_loop_section((uint8_t)target);",
    )

    service = function(source, "static void service_loop(uint32_t now)")
    ordered(
        service,
        "commit_pending_loop_section_at_boundary();",
        "refresh_loop_playback_events();",
        "dispatch_due_loop_events(loop_prev_pos_ms, pos);",
    )
    assert "if (loop_wrapped) { commit_pending_loop_section_at_boundary(); }" in service
    print("PASS: stopped switches are immediate; playing switches commit before head dispatch")

    activate = function(source, "static void activate_loop_section(")
    ordered(
        activate,
        "close_active_recording_layers_for_loop_section();",
        "release_old_loop_section_recorded_voices();",
        "store_active_loop_section_pattern_metadata();",
        "current_loop_section = target;",
        "restore_active_loop_section_pattern_metadata();",
        "advance_loop_events_revision();",
    )
    release = function(source, "static void release_old_loop_section_recorded_voices(")
    assert "!sample_voice_live[pad]" in release
    assert "!synth_trigger_state[(uint8_t)part][pad].live" in release
    close = function(source, "static void close_active_recording_layers_for_loop_section(")
    assert "loop_event_type_t::note_off, 0, layer" in close
    print("PASS: section boundaries close recorded gates while preserving physically held voices")

    add = function(source, "static bool add_loop_section(")
    assert "loop_section_count >= loop_section_max" in add
    ordered(add, "close_active_recording_layers_for_loop_section();", "added.events.assign")
    assert "const uint8_t insert = current_loop_section + 1u;" in add
    assert "added.events.assign(source.events.begin(), source.events.end());" in add
    assert "event.page == performance_page_t::drum && event.layer == 0" in add
    assert "pending_loop_section = (int8_t)insert" in add

    actions = function(source, "static void menu_execute_action(")
    assert '"S%u DUPLICATED AS S%u"' in actions
    assert '"S%u COPY > S%u QUEUED"' in actions
    assert '"S%u PATTERN > BLANK S%u"' in actions
    assert '"S%u PATTERN > S%u QUEUED"' in actions

    delete = function(source, "static bool request_delete_loop_section(")
    assert "loop_section_count <= 1" in delete
    assert "loop_section_delete_confirm_until_msec = now + 2500" in delete
    assert "pending_loop_section_delete = (int8_t)current_loop_section" in delete
    commit = function(source, "static void commit_pending_loop_section_at_boundary(")
    ordered(commit, "pending_loop_section_delete", "delete_active_loop_section_now();")
    print("PASS: Duplicate and Blank insert after current; Delete is protected and boundary-queued")

    clear = function(source, "static void clear_rec_data(")
    assert "event.page == performance_page_t::drum && event.layer == 0" in clear
    assert "loop_events = std::move(beat_pattern_events);" in clear
    assert "loop_sections" not in clear
    print("PASS: Clear Rec affects only the current section and retains Pattern layer 0")

    save = function(source, "static bool save_kit_to_storage(")
    assert 'doc["version"] = project_format_version;' in save
    assert 'loop["activeSection"] = current_loop_section;' in save
    assert 'JsonArray sections = loop["sections"].to<JsonArray>();' in save
    assert 'item["part"] = (uint8_t)e.page;' in save
    assert save.count('loop["background"]') == 1

    load = function(source, "static bool load_kit_from_storage(")
    assert "document_version != project_format_version" in load
    assert "document_version != legacy_project_format_version" in load
    assert 'JsonArray stored_sections = loop["sections"].as<JsonArray>();' in load
    assert "loop_section_count = std::min<uint8_t>(loop_section_max, stored_sections.size());" in load
    assert 'load_events(loop["events"].as<JsonArray>(), section_data.events, true);' in load
    assert "if (destination.size() >= loop_event_max) { break; }" in load
    print("PASS: v11 persists all sections and active selection; v10 migrates to capped S1")

    playback = function(source, "static void refresh_loop_playback_events(")
    assert "beat_format == beat_format_t::audio" in playback
    assert "event.page == performance_page_t::drum" in playback
    print("PASS: shared Audio Beat suppresses retained per-section Pattern playback without duplicating PCM")


if __name__ == "__main__":
    main()
