#!/usr/bin/env python3
"""Static regression guards for KANTAN Sequencer compatibility boundaries."""

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_names_and_ota_identity() -> None:
    common = (ROOT / "main/common_define.hpp").read_text()
    catalog = json.loads((ROOT / "docs/firmware/catalog.json").read_text())
    manifest = json.loads((ROOT / "docs/manifest.json").read_text())

    assert 'firmware_display_name = "KANTAN Sequencer"' in common
    assert 'hardware_display_name = "KANTAN Play core"' in common
    assert 'ota_app_id = "kantanplay"' in common
    sequencer_entries = [item for item in catalog["firmware"]
                         if item.get("app") == "kantanplay"]
    assert sequencer_entries
    assert all(item["app"] == "kantanplay" for item in sequencer_entries)
    assert sequencer_entries[0]["name"] == "KANTAN Sequencer"
    assert manifest["name"] == "KANTAN Sequencer for KANTAN Play core"


def test_sd_recovery_is_sequencer_only() -> None:
    source = (ROOT / "main/file_manage.cpp").read_text()
    begin = source[source.index("bool storage_sd_t::beginStorage(void)"):
                   source.index("bool storage_sd_t::mountStorage(void)")]
    assert "#if !defined(KANPLAY_SAMPLER)" in begin
    assert "sd_media_state_t::missing" in begin
    assert "sd_media_state_t::error" in begin
    assert "return loadStorage();" in begin
    assert "sd_media_state_t::safe_to_remove" not in begin

    assert source.count("#if defined(KANPLAY_SAMPLER)\n   &&") >= 2


def test_check_build_has_no_publish_hook() -> None:
    config = (ROOT / "platformio.ini").read_text()
    start = config.index("[env:sequencer_check_s3]")
    end = config.index("; ==========================================================================", start)
    section = config[start:end]
    assert "extra_scripts" not in section
    assert "KANPLAY_SAMPLER" not in section

    sampler_start = config.index("[env:sampler_check_s3]")
    sampler_end = config.index("; CoreS3 サンプラー デバッグビルド", sampler_start)
    sampler_section = config[sampler_start:sampler_end]
    assert "KANPLAY_SAMPLER=1" in sampler_section
    assert "generate_user_custom.py" not in sampler_section


def test_external_device_matches_sampler_route_model() -> None:
    menu = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    midi = (ROOT / "main/menu_data/menu_data_midi.inl").read_text()
    registry = (ROOT / "main/system_registry.hpp").read_text()
    settings = (ROOT / "main/system_registry.cpp").read_text()

    assert "mi_external_input_source_t" in menu
    assert '{ "Input Source"' in menu
    assert "MENU_BUILDER(mi_ble_midi_t" not in menu
    assert "MENU_BUILDER(mi_usb_mode_t" not in menu
    assert "MENU_BUILDER(mi_usb_power_t" not in menu
    assert "MENU_BUILDER(mi_usb_midi_t" not in menu
    for label in ("Off", "USB MIDI Controller", "USB MIDI Computer", "BLE MIDI", "UART MIDI (Port C)"):
        assert label in midi
    assert "setExternalInputSource" in registry
    assert "_reg_data_8[BLE_MIDI] = plan.ble_input" in registry
    assert "_reg_data_8[USB_MIDI] = plan.usb_input" in registry
    assert "external_input_source" in settings
    assert 'json["port_c_output"]' in settings
    assert "external_input_uart_midi" in registry
    assert "plan.uart_input" in registry
    assert "plan.ble_input" in registry
    assert "plan.usb_input" in registry
    assert "host_disabled_on_boot" not in (ROOT / "main/sequencer_external.cpp").read_text()
    source_selector = midi[midi.index("struct mi_external_input_source_t"):midi.index("struct mi_ble_connection_t")]
    assert "changeSource(" in source_selector
    assert "setExternalInputSource(next)" not in source_selector
    assert '"Restart Required"' in source_selector
    assert '"Apply & Restart"' in source_selector
    assert "safe_default_row" in source_selector
    for label in ("Input Status", "Scan & Connect", "Forget Device", "Reset BLE Connection", "Device Info", "Input Assign"):
        assert label in menu


def test_radio_lifecycle_and_sampler_isolation() -> None:
    midi = (ROOT / "main/task_midi.cpp").read_text()
    wifi = (ROOT / "main/task_wifi.cpp").read_text()
    boot = (ROOT / "main/main.cpp").read_text()
    assert "prepareAtBoot" in boot
    assert boot.index("loadPreferredDevice") < boot.index("task_midi->start")
    gate = wifi[wifi.index("const bool radio_requested"):wifi.index("const bool setup_ap_waiting_for_auth")]
    assert "#if !defined(KANPLAY_SAMPLER)" in gate
    assert "isBLEStoppedForWiFi()" in gate
    assert "goal = prev_goal" in gate and "continue;" in gate
    assert "setWiFiAPInfo" not in gate and "setMidiPortStateBLE" not in gate
    assert "ble_wifi_stopped.store(true)" in midi
    assert midi.index("ble_midi_transport.setUseTxRx(ble_out, ble_in)") < midi.index("ble_wifi_stopped.store(true)")
    ble_transport = (ROOT / "main/midi/midi_transport_ble.cpp").read_text()
    assert "BLEDevice::deinit(_release_memory_on_disable)" in ble_transport
    assert "releaseControllerMemoryOnDisable" in midi
    gui = (ROOT / "main/gui/gui_popup.inl").read_text()
    external = (ROOT / "main/sequencer_external.cpp").read_text()
    assert "ui_restart_notice_t" in gui
    assert "Please do not turn off the power" in gui
    assert "restart_not_before_msec = M5.millis() + 1800" in external
    operator = (ROOT / "main/task_operator.cpp").read_text()
    reset_case = operator[operator.index("case def::command::system_control_t::sc_reset:"):
                          operator.index("case def::command::system_control_t::sc_save:")]
    assert "requestRestart" in reset_case
    assert "firmware_update" in reset_case


def test_wifi_setup_uses_ap_ip_and_keeps_mdns_for_file_editor() -> None:
    common = (ROOT / "main/common_define.hpp").read_text()
    menu = (ROOT / "main/menu_data/menu_data_system.inl").read_text()
    popup = (ROOT / "main/gui/gui_popup.inl").read_text()
    sampler = (ROOT / "main/sampler/sampler_app.cpp").read_text()

    assert 'wifi_setup_url = "http://192.168.4.1"' in common
    assert "QRCODE_URL_WIFI_SETUP" in menu
    setup_case = popup[popup.index("case def::qrcode_type_t::QRCODE_URL_WIFI_SETUP:"):
                       popup.index("case def::qrcode_type_t::QRCODE_URL_DEVICE:")]
    assert "wifi_setup_url" in setup_case
    assert "wifi_mdns" not in setup_case
    file_editor_case = popup[popup.index("case def::qrcode_type_t::QRCODE_URL_DEVICE:"):
                             popup.index("case def::qrcode_type_t::QRCODE_URL_DEVICE_NO_WIFI:")]
    assert "wifi_mdns" in file_editor_case
    assert sampler.count("wifi_setup_url") >= 2


def test_simple_genre_uses_current_song_format() -> None:
    menu = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    genre_menu = menu[menu.index('"Genre"'):menu.index("// 2. Arrange")]
    assert genre_menu.index('"Simple"') < genre_menu.index('"Pop"')
    assert "data_song_preset_genre_simple" in genre_menu

    preset_dir = ROOT / "incbin/preset/song_genre/simple"
    expected = ["Simple_Guitar.json", "Simple_Guitarx2.json", "Simple_Piano.json"]
    listing = (preset_dir / "_list.inl").read_text()
    assert [name for name in expected if name in listing] == expected
    for name in expected:
        song = json.loads((preset_dir / name).read_text())
        assert song["format"] == "KANTANPlayCore"
        assert song["type"] == "Song"
        assert song["version"] == 3
        assert song["num_slot"] == 8
        assert len(song["slot"]) == 8
        assert all("play_mode" not in slot for slot in song["slot"])


def test_main_menu_follows_user_journey() -> None:
    menu = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    system_menu = menu[:menu.index("nullptr, // end of menu")]
    top_level = re.findall(
        r'MENU_BUILDER\([^,]+,\s*1\s*,\s*\{\s*"([^"]+)"', system_menu)
    assert top_level == [
        "Song", "Arrange", "Edit", "Play",
        "External Device", "Wi-Fi", "System",
    ]

    song = menu[menu.index("// 1. Song"):menu.index("// 2. Arrange")]
    assert song.index('"New Song"') < song.index('"Open Song"')
    assert song.index('"Blank"') < song.index('"Genre"')
    for label in ("Genre", "Simple", "Save Song", "Song Manager", "Reset Song"):
        assert f'"{label}"' in song
    assert '"Tempo & Groove"' not in song
    assert '"Number of Sections"' not in song

    arrange = menu[menu.index("// 2. Arrange"):menu.index("// 3. Edit")]
    assert '"Tempo & Groove"' in arrange
    assert '"Number of Sections"' in arrange
    assert '"Genre"' not in arrange

    edit = menu[menu.index("// 3. Edit"):menu.index("// 4. Play")]
    for label in ("Chord Sequence", "Melody", "Section", "Part"):
        assert f'"{label}"' in edit
    assert "mi_melody_edit_t" in edit
    assert edit.count("mi_part_settings_link_t") == 6
    for part_index in range(1, 7):
        assert f'"Part {part_index}"' in edit

    play = menu[menu.index("// 4. Play"):menu.index("// 5-7")]
    for label in ("Play Mode", "Auto Repeat", "Recording"):
        assert f'"{label}"' in play

    file_items = (ROOT / "main/menu_data/menu_data_file.inl").read_text()
    new_song = file_items[file_items.index("struct mi_new_song_t"):
                          file_items.index("struct mi_reset_progression_t")]
    assert '"Create New"' in new_song
    assert "data_song_blank" in new_song

    part_items = (ROOT / "main/menu_data/menu_data_part.inl").read_text()
    melody = part_items[part_items.index("struct mi_melody_edit_t"):]
    assert "def::command::melody_edit_enter" in melody
    assert "def::command::mf_exit" in melody


def test_section_wording_preserves_slot_storage_compatibility() -> None:
    menu = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    common = (ROOT / "main/common_define.hpp").read_text()
    gui = (ROOT / "main/gui/gui_misc.inl").read_text()
    registry = (ROOT / "main/system_registry.cpp").read_text()

    for label in ("Number of Sections", "Section", "Section Button"):
        assert f'"{label}"' in menu
    assert '"Number of Slots"' not in menu
    assert '"Part / Slot"' not in menu
    assert '"Section / Part"' not in menu
    assert '"Slot Button"' not in menu
    buttons = (ROOT / "main/gui/gui_buttons.inl").read_text()
    assert '"Sec.1"' in common and '"Sec.8"' in common
    assert '"Sec.%d"' in gui
    assert '"Sec.%u"' in buttons
    assert "command_param.param <= def::app::max_slot" in buttons
    assert '"Copy Section"' in common and '"Paste Section"' in common
    assert '{ "Section 1"' in common and '"セクション 1 へ"' in common

    # Persisted keys stay unchanged so existing Song and mapping files load.
    assert 'json["num_slot"]' in registry
    assert 'json["slot"]' in registry
    assert '"slot_button"' in registry


def test_part_menu_links_reuse_one_settings_menu() -> None:
    menu = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    part_items = (ROOT / "main/menu_data/menu_data_part.inl").read_text()

    edit = menu[menu.index("// 3. Edit"):menu.index("// 4. Play")]
    assert edit.count("mi_part_settings_link_t") == 6
    assert edit.count("menu_part_quick_edit") == 0

    link = part_items[part_items.index("struct mi_part_settings_link_t"):
                      part_items.index("struct mi_melody_edit_t")]
    assert "setEditTargetPart(_part_index)" in link
    assert "def::menu_category_t::menu_part_quick_edit" in link

    # Settings remain single-source: one shared menu is selected by all links.
    quick_menu = menu[menu.index("static constexpr menu_item_ptr menu_part_quick_edit[]"):]
    assert quick_menu.count('"Tone"') == 1
    assert quick_menu.count('"Volume"') == 1


def test_section_settings_follow_selected_section() -> None:
    registry_hpp = (ROOT / "main/system_registry.hpp").read_text()
    registry_cpp = (ROOT / "main/system_registry.cpp").read_text()
    part_items = (ROOT / "main/menu_data/menu_data_part.inl").read_text()
    operator = (ROOT / "main/task_operator.cpp").read_text()

    assert "PERFORM_STYLE" in registry_hpp
    assert "setPerformStyle(def::perform_style_t style)" in registry_hpp
    assert 'json["version"] = 5' in registry_cpp
    assert 'slot_info["perform_style"]' in registry_cpp
    assert "current_slot->slot_info.getPerformStyle()" in part_items
    assert "current_slot->slot_info.setPerformStyle(mode)" in part_items

    slot_switch = operator[operator.index("void task_operator_t::setSlotIndex"):]
    assert "current_slot->slot_info.getPerformStyle()" in slot_switch


def test_auto_song_supports_automatic_and_tap_beat() -> None:
    common = (ROOT / "main/common_define.hpp").read_text()
    registry = (ROOT / "main/system_registry.hpp").read_text()
    menus = (ROOT / "main/menu_data/menu_data_arrays.inl").read_text()
    part_items = (ROOT / "main/menu_data/menu_data_part.inl").read_text()
    operator = (ROOT / "main/task_operator.cpp").read_text()
    player = (ROOT / "main/task_kantanplay.cpp").read_text()

    assert "auto_song_advance_automatic" in common
    assert "auto_song_advance_tap_beat" in common
    tap_table = common[common.index("command_mapping_auto_song_tap_table"):]
    tap_table = tap_table[:tap_table.index("command_mapping_melody_edit_table")]
    assert tap_table.count("{ chord_beat, 1 }") == 15
    assert '{ "beat"         , { "Beat"' in common

    assert "AUTO_SONG_ADVANCE" in registry
    assert "setAutoSongAdvance" in registry
    assert "getAutoSongAdvance" in registry
    gui_state = registry[registry.index("getGuiAutoplayState"):]
    assert "auto_song_advance_tap_beat" in gui_state
    assert "auto_play_none" in gui_state

    auto_menu = menus[menus.index("static constexpr menu_item_ptr menu_autosong[]"):]
    assert '"Advance Mode"' in auto_menu
    assert "mi_auto_song_advance_t" in auto_menu
    assert '"Automatic"' in part_items and '"Tap Beat"' in part_items
    advance_item = part_items[part_items.index("struct mi_auto_song_advance_t"):]
    assert "autoplay_stop" in advance_item

    song_play = operator[operator.index("case def::gui_mode_t::gm_song_play"):]
    assert "command_mapping_auto_song_tap_table" in song_play
    input_handler = operator[operator.index("case def::command::chord_beat:"):
                             operator.index("case def::command::set_velocity:")]
    assert "auto_song_advance_tap_beat" in input_handler
    assert "def::command::progression_pos_ud, 1" in input_handler

    # Internal playback calls chordBeat() after loading the stored chord. It
    # must not route back through Tap Beat or it would advance recursively.
    tap_handler = player[player.index("void task_kantanplay_t::procChordBeat"):]
    tap_handler = tap_handler[:tap_handler.index("bool offbeat_auto")]
    assert "auto_song_advance_tap_beat" not in tap_handler


def test_menu_navigation_sound_stays_inside_se_pitch_storage() -> None:
    player = (ROOT / "main/task_kantanplay.cpp").read_text()

    navigation = player[player.index("case def::command::menu_navigate_sound:"):
                        player.index("case def::command::chord_degree:")]
    assert "nav_pitch_count = def::app::max_pitch_with_drum - nav_pitch_first" in navigation
    assert "nav_pitch_first + (i % nav_pitch_count)" in navigation
    assert "4 + i" not in navigation

    pitch_manager = player[player.index("void task_kantanplay_t::setPitchManage"):]
    assert "part > def::app::max_chord_part" in pitch_manager
    assert "pitch >= def::app::max_pitch_with_drum" in pitch_manager


def test_sequencer_output_gain_matches_sampler() -> None:
    sequencer = (ROOT / "main/task_i2s.cpp").read_text()
    sampler_app = (ROOT / "main/sampler/sampler_app.cpp").read_text()

    gain_pattern = r"fixed_output_gain_percent\s*=\s*(\d+)"
    sequencer_gain = re.search(gain_pattern, sequencer)
    sampler_gain = re.search(gain_pattern, sampler_app)
    assert sequencer_gain and sampler_gain
    assert sequencer_gain.group(1) == sampler_gain.group(1) == "175"
    assert "fixed_output_gain_q8" in sequencer
    assert "process_output_limiter(out_l, out_r, limiter_gain_q15)" in sequencer
    assert sequencer.index("process_output_limiter(out_l, out_r, limiter_gain_q15)") \
        < sequencer.index("i2sbuf[i  ] = saturate32(out_l)")
    limiter_start = sequencer.index("static inline void process_output_limiter")
    limiter = sequencer[limiter_start:
                        sequencer.index("#if !defined (M5UNIFIED_PC_BUILD)", limiter_start)]
    assert "INT32_MAX / 4 * 3" in limiter
    assert "limiter_gain_q15 = target_gain_q15" in limiter
    assert "diff >> 10" in limiter


def test_melody_exit_rebuilds_slot_button_cache() -> None:
    buttons = (ROOT / "main/gui/gui_buttons.inl").read_text()
    sub_buttons = buttons[buttons.index("struct ui_sub_buttons_t"):]

    assert "const bool mode_changed" in sub_buttons
    melody_branch = sub_buttons[sub_buttons.index("} else if (is_melody_edit)"):
                                sub_buttons.index("} else {", sub_buttons.index("} else if (is_melody_edit)"))]
    assert "bool flg_update = mode_changed || xor_mask != 0" in melody_branch
    normal_branch = sub_buttons[sub_buttons.index("// ---- 演奏時：スロットボタン表示"):
                                sub_buttons.index("void draw_impl", sub_buttons.index("// ---- 演奏時：スロットボタン表示"))]
    assert "bool flg_update = mode_changed" in normal_branch


if __name__ == "__main__":
    test_names_and_ota_identity()
    test_sd_recovery_is_sequencer_only()
    test_check_build_has_no_publish_hook()
    test_external_device_matches_sampler_route_model()
    test_radio_lifecycle_and_sampler_isolation()
    test_wifi_setup_uses_ap_ip_and_keeps_mdns_for_file_editor()
    test_simple_genre_uses_current_song_format()
    test_main_menu_follows_user_journey()
    test_section_wording_preserves_slot_storage_compatibility()
    test_part_menu_links_reuse_one_settings_menu()
    test_section_settings_follow_selected_section()
    test_auto_song_supports_automatic_and_tap_beat()
    test_menu_navigation_sound_stays_inside_se_pitch_storage()
    test_sequencer_output_gain_matches_sampler()
    test_melody_exit_rebuilds_slot_button_cache()
    print("PASS: Sequencer identity, Arrange/Edit menu, Section UI, routing, output gain, and redraw")
