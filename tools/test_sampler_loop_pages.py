#!/usr/bin/env python3
"""Regression checks for Loop Page switching, storage and ownership."""

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

    assert "static constexpr uint8_t loop_page_max = 4;" in source
    assert "static constexpr const size_t loop_event_max = 512;" in source
    assert "#define loop_events (loop_pages[current_loop_page].events)" in source
    assert "static loop_event_t loop_playback_events[loop_event_max];" in source
    assert "static_assert(sizeof(loop_event_t) <= 16" in source
    print("PASS: four pages each own a capped event vector; playback snapshots only one page")

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
        '"Add Loop Page"',
        '"Delete Loop Page"',
        '"Save as Beat"',
    )

    encoder = function(source, "static void process_encoder_delta(")
    assert "if (encoder == 2) { request_loop_page(delta); }" in encoder
    assert "else { page_selector_move(delta); }" in encoder
    assert encoder.index("if (edit_pad >= 0)") < encoder.index("request_loop_page(delta)")
    print("PASS: Rec menu order and dedicated idle ENC3 routing are fixed")

    request = function(source, "static void request_loop_page(")
    assert "const int base = pending_loop_page >= 0 ? pending_loop_page : current_loop_page;" in request
    assert "if (target == current_loop_page)" in request
    assert "pending_loop_page = -1;" in request
    ordered(
        request,
        "loop_playing && loop_length_fixed",
        "pending_loop_page = (int8_t)target;",
        "activate_loop_page((uint8_t)target);",
    )

    service = function(source, "static void service_loop(uint32_t now)")
    ordered(
        service,
        "commit_pending_loop_page_at_boundary();",
        "refresh_loop_playback_events();",
        "dispatch_due_loop_events(loop_prev_pos_ms, pos);",
    )
    assert "if (loop_wrapped) { commit_pending_loop_page_at_boundary(); }" in service
    print("PASS: stopped switches are immediate; playing switches commit before head dispatch")

    activate = function(source, "static void activate_loop_page(")
    ordered(
        activate,
        "close_active_recording_layers_for_loop_page();",
        "release_old_loop_page_recorded_voices();",
        "store_active_loop_page_pattern_metadata();",
        "current_loop_page = target;",
        "restore_active_loop_page_pattern_metadata();",
        "advance_loop_events_revision();",
    )
    release = function(source, "static void release_old_loop_page_recorded_voices(")
    assert "!sample_voice_live[pad]" in release
    assert "!synth_trigger_state[(uint8_t)part][pad].live" in release
    close = function(source, "static void close_active_recording_layers_for_loop_page(")
    assert "loop_event_type_t::note_off, 0, layer" in close
    print("PASS: page boundaries close recorded gates while preserving physically held voices")

    add = function(source, "static bool add_loop_page(")
    assert "loop_page_count >= loop_page_max" in add
    ordered(add, "close_active_recording_layers_for_loop_page();", "duplicate.events.assign")
    assert "const uint8_t insert = current_loop_page + 1u;" in add
    assert "duplicate.events.assign(source.events.begin(), source.events.end());" in add
    assert "pending_loop_page = (int8_t)insert" in add

    delete = function(source, "static bool request_delete_loop_page(")
    assert "loop_page_count <= 1" in delete
    assert "loop_page_delete_confirm_until_msec = now + 2500" in delete
    assert "pending_loop_page_delete = (int8_t)current_loop_page" in delete
    commit = function(source, "static void commit_pending_loop_page_at_boundary(")
    ordered(commit, "pending_loop_page_delete", "delete_active_loop_page_now();")
    print("PASS: Add clones after current; Delete is double-tap, min-one and boundary-queued")

    clear = function(source, "static void clear_rec_data(")
    assert "event.page == performance_page_t::drum && event.layer == 0" in clear
    assert "loop_events = std::move(beat_pattern_events);" in clear
    assert "loop_pages" not in clear
    print("PASS: Clear Rec affects only the current page and retains Pattern layer 0")

    save = function(source, "static bool save_kit_to_storage(")
    assert 'doc["version"] = project_format_version;' in save
    assert 'loop["activePage"] = current_loop_page;' in save
    assert 'JsonArray pages = loop["pages"].to<JsonArray>();' in save
    assert 'item["part"] = (uint8_t)e.page;' in save
    assert save.count('loop["background"]') == 1

    load = function(source, "static bool load_kit_from_storage(")
    assert "document_version != project_format_version" in load
    assert "document_version != legacy_project_format_version" in load
    assert 'JsonArray stored_pages = loop["pages"].as<JsonArray>();' in load
    assert "loop_page_count = std::min<uint8_t>(loop_page_max, stored_pages.size());" in load
    assert 'load_events(loop["events"].as<JsonArray>(), page_data.events, true);' in load
    assert "if (destination.size() >= loop_event_max) { break; }" in load
    print("PASS: v11 persists all pages and active selection; v10 migrates to capped P1")

    playback = function(source, "static void refresh_loop_playback_events(")
    assert "beat_format == beat_format_t::audio" in playback
    assert "event.page == performance_page_t::drum" in playback
    print("PASS: shared Audio Beat suppresses retained per-page Pattern playback without duplicating PCM")


if __name__ == "__main__":
    main()
