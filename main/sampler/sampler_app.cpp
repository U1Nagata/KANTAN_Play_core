// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

// KANTAN Sampler エントリポイント (KANPLAY_SAMPLER ビルド時のみ有効)
// KANTAN Play core と同一のハードウェア (M5Stack CoreS3 + KANTAN Play base) で動作する
// 別ファームウェア。ハードウェア初期化手順は main/main.cpp と共通。
//
// 画面構成 (KANTAN Play の設計思想を踏襲):
//   上半分: タイトル・波形表示・モードタブ (メニュー/編集UI領域)
//   下半分: 4x3 メインPad + 右列Fnボタンの機能表示 (押下で色が変わり、LEDも連動)
//
// サンプル運用:
//   SDカード /sampler/samples/ のWAV/MP3を取り込み、内部PCMへ変換する
//   PSRAMプールへ読み込む。SDに無い場合は組み込みサンプルを使用。
//   演奏中は SD にアクセスしない (レスポンス保証のため)。

#if defined (KANPLAY_SAMPLER)

#include <ArduinoJson.h>
#include <M5Unified.h>
#if !defined(M5UNIFIED_PC_BUILD)
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

#include "../common_define.hpp"
#include "../system_registry.hpp"
#include "../task_i2c.hpp"
#include "../task_midi.hpp"
#include "../task_port_a.hpp"
#include "../task_wifi.hpp"
#include "../file_manage.hpp"

#include "sampler_define.hpp"
#include "sampler_audio.hpp"
#include "sampler_pool.hpp"
#include "sampler_mp3.hpp"
#include "sampler_samples.hpp"
#include "sampler_wav.hpp"
#include "sampler_web.hpp"

namespace sampler_ns {
namespace kp = kanplay_ns;
using def::mode::sampler_mode_t;

//-------------------------------------------------------------------------
// 状態

struct pad_state_t {
  bool pressed = false;
  bool playing_shown = false;  // 再生中ハイライトの表示状態 (再描画判定用)
  uint32_t press_msec = 0;
};

static sampler_audio_t audio;
static kp::task_i2c_t task_i2c;
static kp::task_midi_t task_midi;
static kp::task_port_a_t task_port_a;
static kp::task_wifi_t task_wifi;

static void send_sam_midi(uint8_t status, uint8_t data1, uint8_t data2 = 0)
{
  if (!task_midi.sendInternalRealtime(status, data1, data2)) {
    kp::system_registry->midi_out_control.setMessage(status, data1, data2);
  }
}

#if !defined(M5UNIFIED_PC_BUILD)
static SemaphoreHandle_t sampler_web_command_mutex = nullptr;
#endif
static std::string sampler_web_pending_command;
static volatile bool sampler_web_storage_stop_requested = false;
static volatile bool sampler_web_storage_stop_done = false;
static volatile bool sampler_web_storage_remount_requested = false;
static volatile bool sampler_web_storage_operation_ok = false;

static sampler_mode_t current_mode = sampler_mode_t::mode_play;

enum class performance_page_t : uint8_t {
  sample,
  melody,
  chord,
  drum,
  // Append new page IDs so loop events saved by older beta builds keep their
  // original Sample/Melody/Chord/Drum meaning.
  bass,
  max,
};

enum class synth_tone_source_t : uint8_t {
  general_midi,
  pad,
};

struct pitched_page_settings_t {
  synth_tone_source_t source = synth_tone_source_t::general_midi;
  uint8_t program = 81;  // Lead 2 (sawtooth)
  uint8_t pad = 0;
  uint8_t key = 0;
  uint8_t scale = 0;
  int8_t octave = 0;
  uint8_t volume = 100;
};

static performance_page_t current_page = performance_page_t::sample;
// Internal pad indexes are top-to-bottom. Pad 1 is index 8; Pad 9 is index
// 0 and is intentionally empty in the factory kit.
static constexpr uint8_t factory_pad_sound_pad = 8;
// GM programs are zero-based internally: displayed 82 / 91 are 81 / 90 here.
static pitched_page_settings_t melody_settings = {
  synth_tone_source_t::general_midi, 81, factory_pad_sound_pad, 0, 0, 0, 80,
};
static pitched_page_settings_t chord_settings = {
  synth_tone_source_t::general_midi, 90, factory_pad_sound_pad, 0, 0, 0, 60,
};
// GM programs are zero-based: 38 is displayed as 39 Synth Bass 1.
static pitched_page_settings_t bass_settings = {
  synth_tone_source_t::general_midi, 38, factory_pad_sound_pad, 0, 0, 0, 80,
};
// Bass Octave 0 is musically one octave below Melody Octave 0. Keep this
// separate from the user value so Bass still offers the full -2..+2 range.
static constexpr int8_t bass_base_octave = -1;
// Chord owns the shared harmony key. Bass always follows it; Melody follows
// by default but can keep an independent key for modal/advanced playing.
static bool melody_follow_harmony_key = true;
// KeyとScaleはMelody / Bass / Chordが共有する曲全体の設定。個別の
// pitched_page_settings_tにも互換用に値を残すが、演奏時は必ずこちらを使う。
static uint8_t harmony_scale = 0;
static uint8_t drum_volume = 100;
// Sampler Drum page, in display order P1 (bottom-left) through P12 (top-right).
// Keep this independent from KANTAN Play's 15-button drum map.
static constexpr uint8_t sampler_drum_notes[def::pad::pad_count] = {
  36, 40, 37, 39,  // Kick, Snare, Side Stick, Clap
  41, 43, 45, 42,  // Low/Mid/High Tom, Closed Hi-Hat
  54, 49, 51, 46,  // Tambourine, Crash, Ride, Open Hi-Hat
};
static constexpr const char* sampler_drum_labels[def::pad::pad_count] = {
  "KICK", "SNARE", "SIDE", "CLAP",
  "TOM-L", "TOM-M", "TOM-H", "HH-C",
  "TAMB", "CRASH", "RIDE", "HH-O",
};
static constexpr const char* performance_page_names[] = {
  "SAMPLER", "MELODY", "CHORD", "DRUM", "BASS"
};
// Page color is reserved for page identity (header and subtle information
// area tint). Mode color continues to own the mode tabs and outer frame.
static constexpr uint32_t performance_page_colors[] = {
  0xE05050u, 0x40C0A0u, 0xE0A040u, 0xB060E0u, 0x4088D8u,
};
static constexpr uint32_t performance_page_backgrounds[] = {
  0x0C080Au, 0x07100Fu, 0x100C07u, 0x0E0812u, 0x070C13u,
};
// Header backgrounds stay visibly tied to the current page while retaining
// enough contrast for the battery, volume and Wi-Fi icons.
static constexpr uint32_t performance_page_header_backgrounds[] = {
  0x4A1A1Au, 0x164234u, 0x4A3518u, 0x361D48u, 0x173354u,
};
static constexpr int live_wave_info_bottom_y = 40;
// Keep enum values stable for saved loop data. This is only the physical/page
// navigation order: Drum <- Sample -> Bass -> Melody -> Chord.
static constexpr performance_page_t performance_page_order[] = {
  performance_page_t::drum,
  performance_page_t::sample,
  performance_page_t::bass,
  performance_page_t::melody,
  performance_page_t::chord,
};

static constexpr uint8_t performance_page_order_index(performance_page_t page)
{
  for (uint8_t i = 0; i < (uint8_t)performance_page_t::max; ++i) {
    if (performance_page_order[i] == page) { return i; }
  }
  return 0;
}
static constexpr const char* key_names[] = {
  "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
};
// Keep the original five sampler scale IDs stable and append the new choices.
// This table is local to the Sampler firmware and does not alter KANTAN Play.
static constexpr uint8_t sampler_scale_count = 9;
static constexpr const char* sampler_scale_names[sampler_scale_count] = {
  "Pentatonic", "Major", "Chromatic", "Blues", "Japanese",
  "Minor", "Pentatonic Min", "Dorian", "Mixolydian",
};
static constexpr uint8_t sampler_scale_notes[sampler_scale_count][def::pad::pad_count] = {
  { 48, 50, 52, 55, 57, 60, 62, 64, 67, 69, 72, 74 },
  { 48, 50, 52, 53, 55, 57, 59, 60, 62, 64, 65, 67 },
  { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 },
  { 48, 51, 53, 54, 55, 58, 60, 63, 65, 66, 67, 70 },
  { 48, 50, 51, 55, 56, 60, 62, 63, 67, 68, 72, 74 },
  { 48, 50, 51, 53, 55, 56, 58, 60, 62, 63, 65, 67 },
  { 48, 51, 53, 55, 58, 60, 63, 65, 67, 70, 72, 75 },
  { 48, 50, 51, 53, 55, 57, 58, 60, 62, 63, 65, 67 },
  { 48, 50, 52, 53, 55, 57, 58, 60, 62, 64, 65, 67 },
};

enum class chord_quality_t : uint8_t {
  major,
  minor,
  diminished,
  dominant7,
};

struct chord_template_entry_t {
  int8_t root_semitones;
  chord_quality_t quality;
};

// 各Scaleの7つのルートPadに対する、初心者向けの基本道筋。
// 5音／6音のScaleでは和声に借用音を含めるが、Melody / Bass / Chordは
// 同じKeyとScaleを共有し、中心となる進行を外しにくくする。
static constexpr chord_template_entry_t chord_template_major[7] = {
  { 0, chord_quality_t::major }, { 2, chord_quality_t::minor },
  { 4, chord_quality_t::minor }, { 5, chord_quality_t::major },
  { 7, chord_quality_t::major }, { 9, chord_quality_t::minor },
  { 11, chord_quality_t::diminished },
};
static constexpr chord_template_entry_t chord_template_minor[7] = {
  { 0, chord_quality_t::minor }, { 2, chord_quality_t::diminished },
  { 3, chord_quality_t::major }, { 5, chord_quality_t::minor },
  { 7, chord_quality_t::minor }, { 8, chord_quality_t::major },
  { 10, chord_quality_t::major },
};
static constexpr chord_template_entry_t chord_template_dorian[7] = {
  { 0, chord_quality_t::minor }, { 2, chord_quality_t::minor },
  { 3, chord_quality_t::major }, { 5, chord_quality_t::major },
  { 7, chord_quality_t::minor }, { 9, chord_quality_t::diminished },
  { 10, chord_quality_t::major },
};
static constexpr chord_template_entry_t chord_template_mixolydian[7] = {
  { 0, chord_quality_t::major }, { 2, chord_quality_t::minor },
  { 4, chord_quality_t::diminished }, { 5, chord_quality_t::major },
  { 7, chord_quality_t::minor }, { 9, chord_quality_t::minor },
  { 10, chord_quality_t::major },
};
static constexpr chord_template_entry_t chord_template_blues[7] = {
  { 0, chord_quality_t::dominant7 }, { 2, chord_quality_t::minor },
  { 4, chord_quality_t::minor }, { 5, chord_quality_t::dominant7 },
  { 7, chord_quality_t::dominant7 }, { 9, chord_quality_t::minor },
  { 10, chord_quality_t::dominant7 },
};

static const chord_template_entry_t* chord_template_for_scale(uint8_t scale)
{
  switch (std::min<uint8_t>(scale, sampler_scale_count - 1)) {
  case 3: return chord_template_blues;
  case 5:
  case 6:
  case 4: return chord_template_minor;      // Minor Pentatonic / Hirajoshi
  case 7: return chord_template_dorian;
  case 8: return chord_template_mixolydian;
  case 0:
  case 1:
  case 2:
  default: return chord_template_major;     // Pentatonic / Major / Chromatic
  }
}

static uint8_t harmony_key(void)
{
  return chord_settings.key;
}

static uint8_t pitched_page_key(performance_page_t page)
{
  (void)page;
  return harmony_key();
}

static int pitched_page_octave_semitones(performance_page_t page,
                                         const pitched_page_settings_t& settings)
{
  const int octave = settings.octave
    + (page == performance_page_t::bass ? bass_base_octave : 0);
  return octave * 12;
}
static pad_state_t pads[def::pad::pad_count];
// 通常演奏中はFn列を更新せず、Padを意図して保持した時だけ操作ヒントを出す。
static bool fn_modifier_hint_visible = false;
static constexpr const uint32_t fn_modifier_hint_hold_ms = 240;
static bool fn_pressed[3] = { false, false, false };
static uint32_t fn_press_msec[3] = { 0, 0, 0 };
static int recording_pad = -1;
static int rec_wave_pad = -1;
static int16_t* recording_buffer = nullptr;
static uint32_t recording_frames = 0;
static uint16_t recording_seq = 1;
static int edit_pad = -1;
static uint8_t edit_param = 0;  // 0=Start, 1=End, 2=Volume, 3=Pitch, 4=Repeat
static bool edit_synth_page = false;
static bool edit_chop_page = false;
enum class chop_fit_mode_t : uint8_t { fit_bgm, keep_speed };
enum class chop_count_mode_t : uint8_t { four, eight, twelve, automatic };
static chop_fit_mode_t edit_chop_fit_mode = chop_fit_mode_t::keep_speed;
static chop_count_mode_t edit_chop_count_mode = chop_count_mode_t::eight;
static bool edit_chop_preview_plan_valid = false;
static uint8_t edit_chop_preview_count = 0;
static uint8_t edit_chop_preview_index = 0;
static int8_t edit_chop_preview_last = -1;
static uint16_t edit_chop_preview_pitch_q8 = 256;
static uint32_t edit_chop_preview_boundaries[13] = {};
static uint32_t edit_chop_preview_starts[12] = {};
static uint32_t edit_chop_preview_ends[12] = {};
static uint32_t edit_chop_preview_anchors[12] = {};
static bool edit_trim_changed = false;
static uint32_t edit_value_activity_until = 0;
static bool edit_value_compact_visible = false;
enum class edit_notice_t : uint8_t {
  none,
  hold,
  reverse,
  repeat,
  confirm_melody,
  confirm_chord,
  confirm_bass,
  confirm_unassign_melody,
  confirm_unassign_chord,
  confirm_unassign_bass,
  confirm_delete,
  confirm_chop,
  chopped,
  assigned_melody,
  assigned_chord,
  assigned_bass,
  unassigned_melody,
  unassigned_chord,
  unassigned_bass,
  stop_loop_to_edit_synth,
  stop_loop_to_change_sound,
  stop_loop_to_edit_sample,
  chop_needs_bgm,
};
static edit_notice_t edit_notice = edit_notice_t::none;
static uint32_t edit_notice_until_msec = 0;
static constexpr uint32_t edit_notice_duration_msec = 1200;
static constexpr uint32_t edit_confirm_duration_msec = 3200;
static bool synth_sustain_analysis_pending[def::pad::pad_count] = {};
// Three small caches cover the Sample-page sustain loops most likely to be
// used together. They are prepared only while performance input is idle.
static constexpr uint8_t sampler_sustain_cache_slot_base = 3;
static constexpr uint8_t sampler_sustain_cache_slot_count = 3;
static int8_t sampler_sustain_cache_owner[sampler_sustain_cache_slot_count] = { -1, -1, -1 };
static bool sampler_sustain_cache_pending[def::pad::pad_count] = {};
static int session_save_pending_pad = -1;
static int fx_pad_active = -1;

enum class mixer_part_t : uint8_t {
  bgm,
  drum,
  sampler,
  bass,
  melody,
  chord,
  count,
};
static constexpr uint8_t mixer_part_count = (uint8_t)mixer_part_t::count;
static constexpr const char* mixer_part_labels[mixer_part_count] = {
  "BGM", "DRUM", "SAMPLER", "BASS", "MELODY", "CHORD"
};
static constexpr uint32_t mixer_part_colors[mixer_part_count] = {
  0x70B8FFu, 0xB060E0u, 0xE05050u, 0x4088D8u, 0x40C0A0u, 0xE0A040u
};
// Physical Mixer layout:
//   P5 Melody / P6 Chord / P7 BGM
//   P1 Drum   / P2 Sample / P3 Bass
static constexpr uint8_t mixer_part_pad_numbers[mixer_part_count] = {
  7, 1, 2, 3, 5, 6
};
static constexpr mixer_part_t mixer_panel_parts[mixer_part_count] = {
  mixer_part_t::melody, mixer_part_t::chord, mixer_part_t::bgm,
  mixer_part_t::drum, mixer_part_t::sampler, mixer_part_t::bass
};

static uint8_t display_order_to_pad(uint8_t order);

static mixer_part_t mixer_part_for_pad_number(uint8_t number)
{
  for (uint8_t part = 0; part < mixer_part_count; ++part) {
    if (mixer_part_pad_numbers[part] == number) { return (mixer_part_t)part; }
  }
  return mixer_part_t::count;
}

static uint8_t mixer_pad_for_part(uint8_t part)
{
  if (part >= mixer_part_count) { return def::pad::pad_count; }
  return display_order_to_pad(mixer_part_pad_numbers[part] - 1);
}
struct mixer_snapshot_t {
  bool valid = false;
  uint8_t volume[mixer_part_count] = { 100, 100, 100, 100, 100, 100 };
  bool muted[mixer_part_count] = {};
};
static bool mixer_active = false;
static uint8_t mixer_part_volume[mixer_part_count] = { 100, 100, 100, 100, 100, 100 };
static bool mixer_part_muted[mixer_part_count] = {};
static mixer_snapshot_t mixer_snapshot[4];
static int8_t mixer_pending_snapshot = -1;
// A recalled Mix remains highlighted until a part gain or mute is edited.
static int8_t mixer_applied_snapshot = -1;
static int8_t mixer_held_part = -1;
static bool mixer_pad_adjusted[def::pad::pad_count] = {};
static bool mixer_pad_armed[def::pad::pad_count] = {};
static char mixer_notice[24] = {};
static uint32_t mixer_notice_until_msec = 0;
static constexpr uint32_t mixer_snapshot_hold_msec = 420;

enum class loop_event_type_t : uint8_t {
  note_on,
  note_off,
  pitch_bend_down,
  pitch_bend_center,
  pitch_bend_up,
};

static bool loop_event_is_pitch_bend(loop_event_type_t type)
{
  return type == loop_event_type_t::pitch_bend_down
      || type == loop_event_type_t::pitch_bend_center
      || type == loop_event_type_t::pitch_bend_up;
}

static const char* loop_event_type_name(loop_event_type_t type)
{
  switch (type) {
  case loop_event_type_t::note_off:         return "off";
  case loop_event_type_t::pitch_bend_down:  return "bendDown";
  case loop_event_type_t::pitch_bend_center:return "bendCenter";
  case loop_event_type_t::pitch_bend_up:    return "bendUp";
  case loop_event_type_t::note_on:
  default:                                  return "on";
  }
}

static loop_event_type_t parse_loop_event_type(const char* name)
{
  if (!name) { return loop_event_type_t::note_on; }
  if (!strcmp(name, "off")) { return loop_event_type_t::note_off; }
  if (!strcmp(name, "bendDown")) { return loop_event_type_t::pitch_bend_down; }
  if (!strcmp(name, "bendCenter")) { return loop_event_type_t::pitch_bend_center; }
  if (!strcmp(name, "bendUp")) { return loop_event_type_t::pitch_bend_up; }
  return loop_event_type_t::note_on;
}

struct loop_event_t {
  performance_page_t page = performance_page_t::sample;
  uint8_t pad = 0;
  loop_event_type_t type = loop_event_type_t::note_on;
  uint32_t pos_ms = 0;
  uint16_t layer = 0;
  // Chord roots retain the modifier state present at Note On. This keeps a
  // recorded chord independent from buttons held during later playback.
  uint8_t chord_flags = 0;

  loop_event_t() = default;
  loop_event_t(uint8_t pad_, loop_event_type_t type_, uint32_t pos_ms_, uint16_t layer_,
               uint8_t chord_flags_ = 0)
  : page(performance_page_t::sample), pad(pad_), type(type_), pos_ms(pos_ms_), layer(layer_),
    chord_flags(chord_flags_) {}
  loop_event_t(performance_page_t page_, uint8_t pad_, loop_event_type_t type_,
               uint32_t pos_ms_, uint16_t layer_, uint8_t chord_flags_ = 0)
  : page(page_), pad(pad_), type(type_), pos_ms(pos_ms_), layer(layer_),
    chord_flags(chord_flags_) {}
};

enum class recording_source_t : uint8_t {
  internal_mic,
  external_input,
};
enum class recording_source_mode_t : uint8_t {
  automatic,
  internal_mic,
  external_input,
};
static recording_source_t recording_source = recording_source_t::internal_mic;
static recording_source_mode_t recording_source_mode = recording_source_mode_t::automatic;
static uint32_t recording_sample_rate_current = 16000;

static uint32_t prev_bitmask = 0;
// Physical input is sampled by the I2C task. Keep that edge time while the
// main task drains its history so display transfers cannot move recorded notes.
static uint32_t input_event_msec = 0;
static kp::registry_base_t::history_code_t input_history_code = 0;
static kp::registry_base_t::history_code_t external_input_history_code = 0;
static uint32_t external_input_prev_bitmask = 0;
static kp::registry_base_t::history_code_t opcmd_history_code = 0;
static int touch_pad = -1;  // タッチ演奏中のPad番号 (-1=なし)
// Fn3 Touch Play is a temporary full-screen live surface. It never enters
// the loop recorder and owns no physical pad state while it is visible.
static bool touch_play_active = false;
static int touch_play_pad = -1;
static int touch_play_target_pad = -1;
static int touch_play_drawn_pad = -1;
static uint8_t touch_play_cutoff = 127;
static uint8_t touch_play_resonance = 0;
static uint8_t touch_play_expression = 127;
static uint8_t touch_play_tone_position = 64;
static uint8_t touch_play_target_cutoff = 127;
static uint8_t touch_play_target_resonance = 0;
static uint8_t touch_play_target_expression = 127;
static uint8_t touch_play_target_tone_position = 64;
static uint8_t touch_play_drawn_tone_position = 0xFF;
static uint32_t touch_play_filter_msec = 0;
static uint32_t touch_play_visual_msec = 0;
static uint32_t touch_play_note_msec = 0;
// Touch events can arrive much faster than an audio block.  Coalesce a swipe
// to its newest note, then leave more time for mixing sustained Pad voices.
static constexpr uint32_t touch_play_note_interval_msec = 8;
static constexpr uint32_t touch_play_filter_interval_msec = 20;
static constexpr uint32_t touch_play_visual_interval_msec = 33;

// UI profiling deliberately keeps only aggregate values. It is cheap enough
// for release builds and lets us compare later cache/page-switch changes with
// the same measurements instead of relying on visual impressions alone.
struct ui_render_metrics_t {
  volatile uint32_t jobs = 0;
  volatile uint32_t last_usec = 0;
  volatile uint32_t max_usec = 0;
  volatile uint32_t last_pixels = 0;
  volatile uint32_t max_pixels = 0;
  volatile uint32_t dropped_requests = 0;
  volatile uint32_t psram_free_bytes = 0;
  volatile uint32_t psram_largest_block = 0;
};
static ui_render_metrics_t ui_render_metrics;

#if !defined(M5UNIFIED_PC_BUILD)
enum class touch_render_command_t : uint8_t { full, update, stop, wave_strip };
struct touch_render_state_t {
  touch_render_command_t command = touch_render_command_t::update;
  performance_page_t page = performance_page_t::melody;
  int8_t pad = -1;
  uint8_t tone_position = 64;
  int16_t wave_y = 0;
  int16_t wave_h = 0;
  uint32_t wave_generation = 0;
};
struct ui_tile_transfer_t {
  enum class kind_t : uint8_t { tile, grid } kind = kind_t::tile;
  uint8_t canvas_index = 0;
  int16_t x = 0;
  int16_t y = 0;
  uint32_t page_generation = 0;
};
static QueueHandle_t touch_render_queue = nullptr;
static QueueHandle_t ui_tile_render_queue = nullptr;
static QueueSetHandle_t ui_render_queue_set = nullptr;
static SemaphoreHandle_t touch_render_stopped = nullptr;
static TaskHandle_t touch_render_task_handle = nullptr;
#endif
// Every page switch invalidates queued Pad/Fn/grid transfers from the prior
// page. The renderer checks this token before touching the LCD.
static volatile uint32_t ui_page_generation = 1;
static std::vector<loop_event_t> loop_events;
// Undo is intentionally a short, page-local performance history. Events stay
// in the loop when changing pages, but the former page's Undo entries do not.
static std::vector<uint16_t> loop_undo_history[(uint8_t)performance_page_t::max];
static bool loop_playing = false;
static uint32_t loop_start_msec = 0;
static uint32_t loop_prev_pos_ms = 0;
// 空のループを最初に叩き始める時だけ、同時打鍵をひとまとまりにする。
// タッチPadは同じ演奏でも数msずつ順に届くため、ここを揃えないと
// 先に届いた一打が録音開始処理を占有し、続くPadの記録が不安定になる。
static uint32_t loop_capture_zero_until_msec = 0;
static uint16_t loop_layer_seq = 1;
static bool loop_record_enabled = true;
static bool loop_length_fixed = false;
static uint32_t loop_length_msec = 4000;
static bool loop_bgm_save_active = false;
static int16_t* loop_bgm_save_pcm = nullptr;
static uint32_t loop_bgm_save_frames = 0;
static uint32_t loop_bgm_save_started_msec = 0;
// Performance recording is intentionally separate from loop-event recording:
// it captures the final audible mix into a WAV while the loop runs.
static constexpr uint32_t performance_record_ring_frames = sampler_audio_t::sample_rate * 2;
static constexpr uint32_t performance_record_write_frames = 4096;
// Fn1 remains an immediate Play/Stop control on release.  Suppress any hint
// during a normal tap; only a deliberate 300ms hold exposes the record meter,
// which then takes a further half-second to arm performance recording.
static constexpr uint32_t performance_record_hold_hint_delay_ms = 300;
static constexpr uint32_t performance_record_toggle_hold_ms = 500;
static bool performance_record_armed = false;
static bool performance_record_fn_consumed = false;
static volatile bool performance_record_active = false;
static volatile bool performance_record_finishing = false;
static volatile bool performance_record_done = false;
static volatile bool performance_record_failed = false;
static int16_t* performance_record_ring = nullptr;
static int16_t* performance_record_write_buffer = nullptr;
static char performance_record_path[80] = {};
static volatile uint32_t performance_record_data_bytes = 0;
#if !defined(M5UNIFIED_PC_BUILD)
static TaskHandle_t performance_record_writer_task_handle = nullptr;
#endif
static bool loop_quantize_enabled = true;
static uint8_t loop_quantize_option_index = 2;  // 32分割
static uint8_t loop_note_off_quantize_option_index = 3;  // 64分割
static bool loop_pad_mute[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static bool loop_page_mute[(uint8_t)performance_page_t::max] = {};
static uint16_t loop_active_layer[def::pad::pad_count] = { 0 };
static uint16_t loop_deferred_note_on_layer[def::pad::pad_count] = { 0 };
static uint32_t loop_live_min_gate_until[def::pad::pad_count] = {};
static bool loop_live_release_pending[def::pad::pad_count] = {};
static uint16_t loop_live_release_layer[def::pad::pad_count] = {};
static uint16_t loop_live_release_pending_mask = 0;
static bool loop_deferred_live_pad[def::pad::pad_count] = { false };
static uint32_t loop_deferred_live_pos_ms[def::pad::pad_count] = { 0 };
static uint32_t loop_deferred_live_start_frame[def::pad::pad_count] = { 0 };
static bool soft_snap_pending[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static bool soft_snap_released[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint32_t soft_snap_pos_ms[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint8_t soft_snap_chord_flags[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint32_t soft_snap_release_due[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint32_t soft_snap_release_epoch[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static bool loop_del_touched_pad = false;
static bool loop_recording_notice_shown = false;
static uint32_t sound_priority_until_msec = 0;
static uint32_t sound_attack_guard_until_msec = 0;
static uint16_t dirty_pad_mask = 0;
static uint16_t dirty_pad_state_mask = 0;
static uint8_t dirty_fn_mask = 0;
static bool dirty_wave = false;
static bool dirty_header = false;
// Retained regions normally follow page changes through short dirty updates.
// Keep their last confirmed identity so a boot-time async transfer cannot
// leave a previous page (notably Bass) on the upper surface indefinitely.
static performance_page_t rendered_header_page = performance_page_t::max;
static performance_page_t rendered_wave_page = performance_page_t::max;
static sampler_mode_t rendered_header_mode = sampler_mode_t::mode_max;
static sampler_mode_t rendered_wave_mode = sampler_mode_t::mode_max;
static uint32_t surface_sync_deadline_msec = 0;
// QRを表示するFile Editor中は、本体UIの部分描画がQR面を上書きしないようにする。
// 状態は裏で更新し、終了時のdraw_all()で一度だけ復帰する。
static bool ui_surface_exclusive = false;
// Playモードのライブ波形は、かんぷれと同様にCanvasへ直接描画し、
// 変化した横帯だけをSPI転送する。発音中の全面転送を避けるための状態。
static bool wave_transfer_active = false;
static bool wave_transfer_full_frame = false;
static int16_t wave_transfer_y = 0;
static int16_t wave_transfer_h = 0;
static volatile bool wave_transfer_job_pending = false;
static volatile uint32_t wave_transfer_generation = 1;
static bool live_wave_initialized = false;
static int live_wave_prev_top = 0;
static int live_wave_prev_bottom = 0;
// Sampleモードで選択Padを試聴するときだけ表示する再生位置カーソル。
static int sample_preview_cursor_prev_x = -1;
static uint32_t sample_preview_cursor_prev_msec = 0;
// Fnの説明パネルからライブ波形へ戻る際、一度だけCanvas全体を復元する。
static bool fn_information_panel_visible = false;
// 2サンプルを1本のエンベロープとして描き、時間軸の速度を保ったまま
// ライブ波形の走査量を半分にする。
static constexpr const int live_wave_scan_stride = 2;
static bool loop_timeline_cache_valid = false;
static int loop_cursor_prev_x = -1;
// The stopped-state length text is deliberately kept out of the retained
// piano-roll image.  Transport start can then restore only this small region
// instead of rebuilding the whole timeline before the first Pad attack.
static bool loop_length_label_overlay_pending = false;
static bool loop_length_label_overlay_visible = false;
static bool loop_length_label_restore_pending = false;
static constexpr int loop_length_label_overlay_w = 122;
static constexpr int loop_length_label_overlay_h = 20;
// Append-only loop recording normally changes only a few pixels.  Keep a
// handful of narrow dirty spans so adding notes never forces a full piano-roll
// sprite transfer while the performer is playing.
struct loop_timeline_dirty_span_t {
  int left = 0;
  int right = -1;
};
static constexpr uint8_t loop_timeline_dirty_span_max = 8;
static loop_timeline_dirty_span_t loop_timeline_dirty_spans[loop_timeline_dirty_span_max] = {};
static uint8_t loop_timeline_dirty_span_count = 0;
static constexpr const uint32_t sample_move_hold_ms = 390;
static int sample_move_source_pad = -1;
// 空Padへ移動した直後だけ、元Padを押したまま移動先をもう一度タップすると
// 元の位置へ複写できる。Mix後は音の意味が変わるため、この状態には入らない。
static int sample_move_copy_source_pad = -1;
static int sample_move_copy_target_pad = -1;
// SAMPLEモードのFn3削除は、同じPadをもう一度タップして初めて確定する。
// Fn3を離した時点で必ず解除し、通常演奏へ確認状態を持ち越さない。
static int sample_delete_confirm_pad = -1;
static uint32_t sample_delete_confirm_until_msec = 0;
// SAMPLE uses a deliberate two-step entry: the first tap is always a safe
// audition, while the next tap enters Edit on release or becomes Move/Mix if
// held.  This keeps ordinary listening separate from destructive rearranging.
static int sample_edit_armed_pad = -1;
// The first audition leaves a persistent, explicit next-step hint. It is
// cleared only by another Pad action or by leaving the SAMPLE surface.
static int sample_edit_pending_pad = -1;
// Empty pads use a deliberate two-stage add gesture. A normal missed tap is
// inert; hold once to arm, then tap to import or hold again to record.
static constexpr uint32_t sample_add_arm_hold_ms = 520;
static constexpr uint32_t sample_add_action_hold_ms = 420;
static constexpr uint32_t sample_add_armed_timeout_ms = 2600;
static int sample_add_candidate_pad = -1;
static int sample_add_armed_pad = -1;
static uint32_t sample_add_armed_until_msec = 0;
static int sample_add_action_pad = -1;
// 長押しで確定する操作は、用途ごとに別のUIを持たず共通の小さな進捗表示を
// 使う。演奏中も波形やタイムラインを隠さず、確定直前だけを明確に伝える。
enum class hold_progress_kind_t : uint8_t {
  none,
  loop_clear,
  sample_move,
  sample_add,
  mix_save,
  open_menu,
  performance_record,
};
static hold_progress_kind_t hold_progress_kind = hold_progress_kind_t::none;
static uint32_t hold_progress_start_msec = 0;
static uint32_t hold_progress_duration_msec = 0;
static uint32_t hold_progress_color = 0x80C0FFu;
static char hold_progress_wait_text[28] = {};
static char hold_progress_ready_text[28] = {};
static uint32_t hold_progress_draw_msec = 0;
static uint8_t hold_progress_drawn_percent = 255;
static bool hold_progress_static_drawn = false;
static bool hold_progress_needs_redraw = false;
static constexpr uint8_t fx_param_count = 4;
static constexpr uint8_t fx_tempo_index = 0;
static constexpr uint8_t fx_filter_index = 1;
static constexpr uint8_t fx_repeat_index = 2;
static constexpr uint8_t fx_delay_index = 3;
static constexpr uint8_t fx_tape_stop_index = 4;
static uint8_t fx_selected = 0;
static int8_t fx_param[fx_param_count] = { 0, 0, 0, 1 }; // Delay defaults to 2 Grid.
static bool fx_speed_active = false;
static bool fx_speed_pressed = false;
static uint16_t fx_speed_ratio_current_q8 = 256;
static uint16_t fx_speed_ratio_target_q8 = 256;
static uint32_t fx_speed_last_msec = 0;
static constexpr uint32_t fx_speed_ramp_msec = 240;
static bool fx_speed_returning = false;
static bool fx_speed_reference_active = false;
static uint32_t fx_speed_reference_origin_msec = 0;
static uint32_t fx_speed_reference_origin_pos_ms = 0;
static uint32_t fx_speed_reference_length_ms = 0;
static uint32_t fx_speed_return_started_msec = 0;
static uint32_t fx_speed_return_duration_msec = 0;
static constexpr uint32_t background_resync_skip_ms = 12;
static constexpr uint32_t background_resync_tolerance_ms = 2;
// A speed ramp uses two schedulers (audio frames and loop milliseconds).
// Re-align the BGM once at the next musical cycle boundary to erase any
// fractional-frame difference accumulated during that ramp.
static volatile bool background_loop_resync_pending = false;
// Master Scratch reads a short dry final-mix history. Every transport and
// voice keeps advancing underneath, so release can crossfade straight home.
static volatile bool bgm_scratch_active = false;
static volatile bool bgm_scratch_return_pending = false;
static int16_t bgm_scratch_rate_q8 = 256;
static int16_t bgm_scratch_target_q8 = 256;
static uint32_t bgm_scratch_last_msec = 0;
static uint32_t bgm_scratch_gesture_until_msec = 0;
static bool bgm_scratch_rejoin_after_stop = false;
static constexpr const uint32_t bgm_scratch_ramp_msec = 70;
static constexpr const uint32_t bgm_scratch_gesture_msec = 105;
static bool loop_repeat_armed = false;
static bool loop_repeat_running = false;
static uint32_t loop_repeat_release_confirm_msec = 0;
static constexpr uint32_t loop_repeat_release_debounce_msec = 45;
static uint32_t loop_repeat_start_pos_ms = 0;
static uint32_t loop_repeat_length_ms = 0;
static constexpr const uint8_t loop_repeat_half_steps[] = { 16, 8, 4, 2, 1 };  // 8, 4, 2, 1, 0.5 step
static constexpr const char* loop_repeat_labels[] = { "8", "4", "2", "1", "0.5" };
static constexpr const uint8_t delay_half_steps[] = { 8, 4, 2, 1 };  // 4, 2, 1, 0.5 step
static constexpr const char* delay_grid_labels[] = { "4", "2", "1", "0.5" };
static constexpr size_t delay_grid_option_count = sizeof(delay_half_steps) / sizeof(delay_half_steps[0]);
static int8_t fx_index_for_pad_number(uint8_t number)
{
  if (number >= 1 && number <= 4) { return fx_repeat_index; }
  if (number == 5) { return fx_filter_index; }
  if (number == 6) { return fx_tempo_index; }
  if (number == 7) { return fx_tape_stop_index; }
  if (number == 8) { return fx_delay_index; }
  return -1;
}
static constexpr const char* sample_repeat_grid_labels[] = { "8 Grid", "4 Grid", "2 Grid", "1 Grid", "0.5 Grid" };
static constexpr size_t loop_repeat_option_count = sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0]);
static constexpr const char* whole_sample_repeat_label = "Whole Sample";
enum class pad_repeat_mode_t : uint8_t { none, grid, half_grid };
static pad_repeat_mode_t pad_repeat_mode = pad_repeat_mode_t::none;
static uint32_t pad_repeat_next_msec[def::pad::pad_count] = { 0 };
static uint16_t pad_repeat_active_mask = 0;
// ループ再生中は実時間ではなくトランスポート上の拍位置を次回発音に使う。
// これにより一発目の遅れや1周ごとの丸め誤差を次の発音で解消できる。
static bool pad_repeat_transport_locked[def::pad::pad_count] = { false };
static uint32_t pad_repeat_next_pos_ms[def::pad::pad_count] = { 0 };
static uint16_t pad_repeat_phase_half_step[def::pad::pad_count] = { 0 };
static uint16_t pad_repeat_last_layer[def::pad::pad_count] = { 0 };
static performance_page_t pad_repeat_page[def::pad::pad_count] = {};
// The jog lever is read through the I2C button expander.  A single transient
// released sample must not cancel a held Repeat, otherwise a fast repeat sounds
// like only two or three hits.  Release is confirmed shortly afterwards.
static uint32_t pad_repeat_lever_mask = 0;
static uint32_t pad_repeat_release_confirm_msec = 0;
static constexpr uint32_t pad_repeat_lever_release_debounce_msec = 40;
static bool sample_grid_loop_active[def::pad::pad_count] = { false };
static bool sample_whole_loop_active[def::pad::pad_count] = { false };
static uint16_t sample_grid_loop_active_mask = 0;
static uint32_t sample_grid_loop_next_msec[def::pad::pad_count] = { 0 };
static bool sample_grid_loop_transport_locked[def::pad::pad_count] = { false };
static uint32_t sample_grid_loop_next_pos_ms[def::pad::pad_count] = { 0 };
static uint16_t sample_grid_loop_phase_half_step[def::pad::pad_count] = { 0 };
static int play_focus_pad = -1;
static constexpr const uint8_t background_loop_voice = def::pad::pad_count;
static constexpr const uint8_t menu_preview_voice = def::pad::pad_count + 1;
static constexpr const uint8_t external_midi_voice_base = def::pad::pad_count + 2;
static constexpr const uint8_t external_midi_voice_count = 8;
static int8_t external_midi_voice_note[external_midi_voice_count] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static uint8_t external_midi_voice_next = 0;
enum class pitched_voice_owner_t : uint8_t { none, external, melody, chord, bass };
struct pitched_voice_state_t {
  pitched_voice_owner_t owner = pitched_voice_owner_t::none;
  uint8_t trigger = 0xFF;
  uint8_t note = 0;
  uint32_t age = 0;
  uint32_t generation = 0;
};
static pitched_voice_state_t pitched_voice_state[external_midi_voice_count];
static uint32_t pitched_voice_age = 1;
static uint32_t pitched_voice_generation = 1;
struct synth_trigger_state_t {
  bool midi = false;
  uint8_t note_count = 0;
  uint8_t notes[4] = {};
  uint8_t voices[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
  uint32_t voice_generation[4] = {};
};
static synth_trigger_state_t synth_trigger_state[(uint8_t)performance_page_t::max][def::pad::pad_count];
// Identifies the latest sounding trigger independently of its MIDI/PCM voice.
// A delayed soft-snap release may only stop the generation that scheduled it.
static uint32_t synth_trigger_epoch[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint16_t synth_loop_active_layer[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint16_t synth_deferred_note_on_layer[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint32_t synth_live_min_gate_until[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static bool synth_live_release_pending[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint16_t synth_live_release_layer[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static uint16_t synth_live_release_pending_mask[(uint8_t)performance_page_t::max] = {};
static uint16_t pitch_bend_record_layer[(uint8_t)performance_page_t::max] = {};
// Layer currently sounding on each synth pad. A quantized Note Off may arrive
// after the next Note On; matching the layer prevents it from cutting the new note.
static uint16_t synth_sounding_layer[(uint8_t)performance_page_t::max][def::pad::pad_count] = {};
static bool chord_modifier_pressed[5] = {};
// Melody and Bass keep independent bend automation so both recorded pages can
// play back together. Q12 +/-4096 equals one semitone.
struct page_pitch_bend_state_t {
  int16_t target_q12 = 0;
  int16_t current_q12 = 0;
  uint32_t last_msec = 0;
  uint32_t last_apply_msec = 0;
  bool down_held = false;
  bool up_held = false;
};
static page_pitch_bend_state_t page_pitch_bend[(uint8_t)performance_page_t::max];

static inline void set_loop_live_release_pending(uint8_t pad, bool pending)
{
  if (pad >= def::pad::pad_count) { return; }
  loop_live_release_pending[pad] = pending;
  const uint16_t bit = (uint16_t)(1u << pad);
  if (pending) { loop_live_release_pending_mask |= bit; }
  else { loop_live_release_pending_mask &= ~bit; }
}

static inline void set_synth_live_release_pending(uint8_t page, uint8_t pad, bool pending)
{
  if (page >= (uint8_t)performance_page_t::max || pad >= def::pad::pad_count) { return; }
  synth_live_release_pending[page][pad] = pending;
  const uint16_t bit = (uint16_t)(1u << pad);
  if (pending) { synth_live_release_pending_mask[page] |= bit; }
  else { synth_live_release_pending_mask[page] &= ~bit; }
}

static bool& loop_mute(performance_page_t page, uint8_t pad)
{
  return loop_pad_mute[(uint8_t)page][pad];
}

static bool performance_page_part_muted(performance_page_t page)
{
  bool part_muted = loop_page_mute[(uint8_t)page];
  switch (page) {
  case performance_page_t::melody:
    part_muted = mixer_part_muted[(uint8_t)mixer_part_t::melody];
    break;
  case performance_page_t::bass:
    part_muted = mixer_part_muted[(uint8_t)mixer_part_t::bass];
    break;
  case performance_page_t::chord:
    part_muted = mixer_part_muted[(uint8_t)mixer_part_t::chord];
    break;
  default:
    break;
  }
  return part_muted;
}

static bool loop_is_muted(performance_page_t page, uint8_t pad)
{
  return performance_page_part_muted(page) || loop_mute(page, pad);
}
static constexpr const char* sampler_resume_path = "/sampler_resume.json";
static constexpr const char* sampler_folder_settings_path = "/sampler_folder_settings.json";
static constexpr const char* sampler_session_dir = "/sampler/session";
static constexpr uint8_t fixed_output_gain_percent = 175;

// BGM level uses a simple, predictable 0-100% scale.
static constexpr uint8_t volume_20_percent_step_count = 6;
static constexpr uint8_t volume_percent_from_20_percent_step(uint8_t step)
{
  return (step >= volume_20_percent_step_count ? volume_20_percent_step_count - 1 : step) * 20;
}

static constexpr uint16_t volume_q8_from_20_percent_step(uint8_t step)
{
  return ((uint32_t)volume_percent_from_20_percent_step(step) * 256 + 50) / 100;
}

static uint8_t volume_20_percent_step_from_percent(uint8_t percent)
{
  return std::min<uint8_t>(volume_20_percent_step_count - 1, (percent + 10) / 20);
}

static uint8_t volume_20_percent_step_from_q8(uint16_t volume_q8)
{
  const uint8_t percent = std::min<uint16_t>(100, ((uint32_t)volume_q8 * 100 + 128) / 256);
  return volume_20_percent_step_from_percent(percent);
}

// Synth pages keep a musical working range while allowing useful fine adjustment.
static constexpr uint8_t synth_volume_step_count = 6;
static constexpr uint8_t synth_volume_percent_from_step(uint8_t step)
{
  return 50 + std::min<uint8_t>(step, synth_volume_step_count - 1) * 10;
}

static uint8_t synth_volume_step_from_percent(uint8_t percent)
{
  const uint8_t clamped = std::clamp<uint8_t>(percent, 50, 100);
  return std::min<uint8_t>(synth_volume_step_count - 1, (clamped - 50 + 5) / 10);
}

struct background_loop_t {
  int16_t* pcm = nullptr;
  uint32_t frames = 0;
  uint32_t sample_rate = sampler_audio_t::sample_rate;
  uint16_t volume_q8 = volume_q8_from_20_percent_step(4);
  uint8_t loop_repeats = 2;
  char name[24] = { 0 };
  char file_path[80] = { 0 };
  bool isValid(void) const { return pcm != nullptr && frames != 0 && sample_rate != 0; }
  size_t bytes(void) const { return (size_t)frames * sizeof(int16_t); }
};
static background_loop_t background_loop;
static char background_loop_error[40] = { 0 };
static int16_t* menu_preview_pcm = nullptr;
static uint32_t menu_preview_frames = 0;
static uint32_t menu_preview_sample_rate = 44100;
static constexpr uint8_t synth_menu_preview_channel = kp::def::midi::channel_15;
static uint8_t synth_menu_preview_note = 60;
static bool synth_menu_preview_note_active = false;
static bool synth_menu_preview_sample_active = false;
static uint32_t synth_menu_preview_stop_msec = 0;

static M5Canvas wave_canvas(&M5.Display);
static M5Canvas menu_canvas(&M5.Display);
// メニュー遷移中は旧ページと新ページを保持する。かんぷれと同じ二面方式にして、
// 日本語テキストをフレームごとに描き直さず、スプライト転送だけでスライドする。
static M5Canvas menu_transition_canvas(&M5.Display);
static bool menu_transition_canvas_ready = false;
// Page selector is an independent, small sprite so jog navigation never
// redraws the waveform or pads while the player is deciding where to go.
static M5Canvas page_selector_canvas(&M5.Display);
static bool page_selector_canvas_ready = false;
// Melody and Bass use the same Kaoss-style surface. Keep the static rows in
// PSRAM and transfer them as one image; live input only redraws the markers.
static M5Canvas touch_play_surface_canvas(&M5.Display);
static bool touch_play_surface_canvas_ready = false;
static uint8_t touch_play_surface_cache_key = 0xFF;
static uint8_t touch_play_surface_cache_scale = 0xFF;
static bool performance_ui_arena_suspended = false;
static bool performance_ui_arena_resume_pending = false;
static uint32_t performance_ui_arena_resume_msec = 0;
static bool page_selector_visible = false;
static bool page_selector_dirty = false;
static bool page_selector_slide_in = false;
static bool page_selector_restore_pending = false;
static uint8_t page_selector_index = 0;
static uint32_t page_selector_until_msec = 0;
static uint32_t page_selector_last_draw_msec = 0;
static M5Canvas wifi_qr_canvas(&M5.Display);
static bool wifi_qr_canvas_ready = false;
// Pad/Fn全体の更新用。LCDへ多数の小さな描画命令を送らず、固定DMAバッファで
// 合成してから一回だけ転送する。再生中の枠だけ更新は従来の直接描画を維持する。
static M5Canvas ui_dirty_canvas[2] = { M5Canvas(&M5.Display), M5Canvas(&M5.Display) };
static uint16_t* ui_dirty_buffer[2] = { nullptr, nullptr };
static uint8_t ui_dirty_canvas_index = 0;
static bool ui_dirty_renderer_ready = false;
static volatile bool ui_dirty_canvas_busy[2] = { false, false };
static bool ui_async_tile_submit = false;
// Play and FX are the surfaces switched during a performance. Their completed
// Pad/Fn regions stay in PSRAM so later transitions need no text/waveform
// composition on the input core.
// Keep one Play grid per performance page plus one shared FX grid. A completed
// page can then be restored without recomposing text and Pad waveforms when
// the player returns to it. Wi-Fi operations release every canvas below.
static constexpr uint8_t play_grid_cache_count = (uint8_t)performance_page_t::max;
static constexpr uint8_t fx_grid_cache_index = play_grid_cache_count;
static constexpr uint8_t grid_cache_count = play_grid_cache_count + 1;
static M5Canvas grid_cache_canvas[grid_cache_count] = {
  M5Canvas(&M5.Display), M5Canvas(&M5.Display), M5Canvas(&M5.Display),
  M5Canvas(&M5.Display), M5Canvas(&M5.Display), M5Canvas(&M5.Display)
};
static bool grid_cache_ready[grid_cache_count] = {};
static volatile bool grid_cache_busy[grid_cache_count] = {};
static uint16_t grid_cache_pad_mask[grid_cache_count] = {};
static uint8_t grid_cache_fn_mask[grid_cache_count] = {};
static performance_page_t grid_cache_page[grid_cache_count] = {
  performance_page_t::max, performance_page_t::max, performance_page_t::max,
  performance_page_t::max, performance_page_t::max, performance_page_t::max
};

static void loop_repeat_set_active(bool active);
static uint32_t loop_repeat_width_ms(void);
static uint32_t loop_next_quantize_pos_ms(uint32_t pos_ms);
static void set_pad_repeat_mode(pad_repeat_mode_t mode);
static void service_pad_repeat(uint32_t now);
static void service_page_selector(uint32_t now);
static void page_selector_move(int delta);
static void service_melody_pitch_bend(uint32_t now);
static void set_melody_pitch_bend_lever(bool down, bool pressed);
static void cancel_live_pitch_bend_levers(void);
static uint16_t melody_pitch_bend_scale_q12(performance_page_t page);
static void record_page_pitch_bend(performance_page_t page, int16_t target_q12);
static void reset_page_pitch_bend(performance_page_t page, bool send_midi);
static void service_sample_grid_loops(uint32_t now);
static void clear_sample_grid_loops(void);
static void refresh_sample_grid_loop_intervals(void);
static void sample_loop_grid_add(int diff);
static bool loop_event_crossed(uint32_t prev_pos, uint32_t pos, uint32_t event_pos);
static const char* fn_information_text(void);
static bool sample_edit_armed_active(uint32_t now);
static bool play_loop_grid_information_active(void);
static bool fn_information_chip_bounds(int* x, int* y, int* w, int* h);
static bool draw_fn_information_chip(M5Canvas& canvas);
static void draw_all(void);
static bool ui_async_display_busy(void);
static void restore_performance_surface_from_cache(void);
static void draw_wave(void);
static void draw_live_wave_frame(void);
static void service_wifi_setup_qr(void);
static void service_wifi_setup_result(void);
static void service_wifi_radio_start(void);
static void suspend_performance_ui_arena(void);
static void request_performance_ui_arena_resume(void);
static void service_performance_ui_arena(uint32_t now);
static const chord_template_entry_t& chord_template_entry(uint8_t degree);
static const char* chord_quality_suffix(chord_quality_t base, bool swap);
static bool wifi_sta_connected(void);
static void service_wifi_update(void);
static void service_ble_device_ui(uint32_t now);
static void start_wifi_update(void);
static void cancel_wifi_update(void);
static void stop_file_server_session(void);
static void draw_menu(bool redraw_keypad);
static void draw_menu_keypad(bool force = false);
static void reset_wifi_qr_canvas(void);
static void draw_learn_target_keypad(void);
static void show_status_message(const char* msg, uint32_t duration_ms, bool redraw);
static void service_sampler_web_command(void);
static bool ensure_sampler_sd_dirs(void);
static void load_sampler_folder_settings(void);
static void save_sampler_folder_settings(void);
static void load_builtin_samples(void);
static bool load_builtin_background_loop(const char* builtin_id = nullptr);
static bool load_builtin_sample_to_pad(uint8_t pad, const char* builtin_id);
static int load_sd_samples(void);
static void clear_kit(void);
static bool save_current_kit(const char* path);
static bool save_kit_to_storage(kp::storage_base_t& storage, const char* path);
static bool save_session_pad(uint8_t pad);
static bool load_kit_file(const char* path);
static bool load_kit_from_storage(kp::storage_base_t& storage, const char* path, bool allow_sd_assets = true);
static bool load_resume_kit(void);
static void save_resume_kit(void);
static void reset_builtin_kit(void);
static bool reset_default_or_builtin_kit(void);
static void reset_sampler_sd_folder_selection(void);
static void process_encoder_value(uint8_t encoder, uint32_t value);
static bool load_audio_to_pad(uint8_t pad, const char* path, const char* display_name, char* error, size_t error_len);
static void prepare_pad_for_new_sample(uint8_t pad);
static void clear_pad_sample(uint8_t pad, bool remove_loop_events);
static void clear_all_pad_samples(void);
static bool load_background_loop_file(const char* path, const char* display_name);
static bool load_background_loop_memory(const uint8_t* data, size_t len, const char* display_name, const char* file_path, uint8_t loop_repeats);
static void clear_menu_preview(void);
static bool play_menu_audio_preview(const char* path, uint32_t max_ms);
static bool play_menu_builtin_preview(const char* builtin_id, uint32_t max_ms);
static void service_synth_menu_preview(uint32_t now);
static void preview_synth_menu_selection(void);
static void set_background_loop_error(const char* msg);
static void clear_background_loop(void);
static void play_background_loop_at(uint32_t pos_ms);
static void stop_background_loop(void);
static void loop_reset_recording_state(void);
static void stop_all_audio(bool reset_mixer = true);
static void save_loop_as_bgm(void);
static bool begin_performance_recording(void);
static void finish_performance_recording(void);
static void service_performance_recording(void);
static void clear_synth_runtime(void);
static void apply_external_midi_ch1_tone(void);
static void apply_synth_tones(bool force = false);
static void apply_synth_page_volume(performance_page_t page, bool force = false);
static uint8_t synth_sustain_cache_slot(performance_page_t page);
static pitched_page_settings_t& page_settings(performance_page_t page);
static std::string sampler_file_display_name(const std::string& source_name, const char* source_dir);
static uint8_t chord_degree_for_order(uint8_t order);
static int8_t chord_modifier_for_order(uint8_t order);
static void request_chord_label_draw(void);
static uint8_t allocate_pitched_voice(pitched_voice_owner_t owner, uint8_t trigger, uint8_t note,
                                      bool live_performance = false);
static uint16_t sample_pitch_for_note(const sample_slot_t& slot, uint8_t note);
static bool synth_sustain_parameters(const sample_slot_t& slot, uint32_t source_start,
                                     uint32_t* start, uint32_t* end, uint16_t* crossfade);
static uint16_t sample_sustain_auto_release_ms(const sample_slot_t& slot,
                                               uint32_t sustain_start,
                                               uint32_t sustain_end);
static bool play_sample_sustain_voice(int pad, bool auto_release);
static void request_sampler_sustain_cache(int pad);
static void service_sampler_sustain_cache(void);
static void trigger_synth_pad(performance_page_t page, uint8_t pad, int chord_flags = -1,
                              bool live_performance = false);
static void release_synth_trigger(performance_page_t page, uint8_t pad);
static void stop_synth_page(performance_page_t page);
static void set_touch_play_active(bool active);
static void handle_touch_play(int x, int y, bool pressed);
static void service_touch_play(uint32_t now);
static bool ble_midi_cache_guard_active(void);
static void begin_hold_progress(hold_progress_kind_t kind, uint32_t start_msec,
                                uint32_t duration_msec, uint32_t color,
                                const char* wait_text, const char* ready_text);
static void cancel_hold_progress(hold_progress_kind_t kind = hold_progress_kind_t::none);
static void service_hold_progress(uint32_t now);
static void hold_progress_bounds(int* x, int* y, int* w, int* h);
static void wait_ui_dirty_transfers(void);
static void release_other_bass_notes(uint8_t selected_pad);
static void cancel_sample_move(void);
static void pad_press(int pad);
static void pad_release(int pad);
static void set_mode(sampler_mode_t mode);
static bool page_selector_confirm(bool defer_visual_restore = false);
static void service_fx_speed(uint32_t now);
static uint32_t fx_delay_frames(void);
static void set_bgm_scratch_lever(int8_t direction, bool pressed);
static void service_bgm_scratch(uint32_t now);
static uint32_t background_expected_frame(uint32_t loop_pos_ms, uint32_t frames);
static void fx_set_active(uint8_t index, bool active);
static void fx_pad_press(int pad);
static void fx_pad_release(int pad);
static void mixer_set_active(bool active);
static void mixer_pad_press(int pad);
static void mixer_pad_release(int pad);
static void mixer_volume_add(int diff);
static void apply_mixer_part(mixer_part_t part);
static void apply_all_mixer_parts(void);
static void set_mixer_part_muted(mixer_part_t part, bool muted);
static void reset_mixer_mix(void);
static void apply_mixer_snapshot(uint8_t snapshot);
static void apply_pending_mixer_snapshot(void);
static mixer_part_t mixer_part_for_page(performance_page_t page);
static uint16_t mixer_scaled_volume_q8(mixer_part_t part, uint16_t base_q8);
static void loop_repeat_update_width_preserving_start(void);
static void enter_edit(int pad);
static void exit_edit(void);
static void repair_pitched_pad_sources(void);
static void trigger_pad(int pad);

//-------------------------------------------------------------------------
// レイアウト定数 (240x320 縦画面)

static constexpr const int32_t header_h   = 24;
static constexpr const int32_t wave_y     = 25;
static constexpr const int32_t wave_h     = 112;
static constexpr const int32_t tab_y      = 143;
static constexpr const int32_t tab_h      = 30;
static constexpr const int32_t menu_area_y = wave_y;
// Status messages extend 2px below the mode-tab area to leave visible padding
// around the double-height font.  The pad grid starts at y=180, so this still
// leaves a 5px gap below the menu canvas.
static constexpr const int32_t menu_area_h = tab_y + tab_h + 2 - wave_y;
static constexpr const int32_t grid_y     = 180;
static constexpr const int32_t cell_h     = 44;
static constexpr const int32_t row_pitch  = 47;
static constexpr const int32_t pad_w      = 44;
static constexpr const int32_t col_pitch  = 47;
static constexpr const int32_t grid_x     = 2;
static constexpr const int32_t fn_x       = 194;
static constexpr const int32_t fn_w       = 44;
static constexpr const int32_t grid_cache_w = fn_x + fn_w;
static constexpr const int32_t grid_cache_h = row_pitch * 2 + cell_h;
// Part selection owns the retained information area, but leaves the header,
// mode tabs and performance pads visible.  This makes the instrument layer a
// primary concept without requiring another full-screen cache.
static constexpr const int32_t page_selector_x = 0;
static constexpr const int32_t page_selector_y = wave_y;
static constexpr const int32_t page_selector_w = 240;
static constexpr const int32_t page_selector_h = wave_h;
static constexpr const uint32_t page_selector_timeout_ms = 700;
// パッド波形は画像ではなく18本の縦線形状だけを保持する。
// RGB565画像をキャッシュしないため、画面とCanvas間の色並びに依存しない。
static constexpr const uint8_t pad_wave_columns = 18;
static constexpr const uint8_t pad_wave_column_width = 2;
struct pad_wave_shape_t {
  uint32_t signature = 0;
  int8_t top[pad_wave_columns] = {};
  int8_t bottom[pad_wave_columns] = {};
  bool valid = false;
};
static pad_wave_shape_t pad_wave_shape[def::pad::pad_count];
// 内蔵Micは32kHzで録音する。高域のアタックを残しつつ、48kHzより録音負荷を抑える。
static constexpr const uint32_t recording_internal_sample_rate = 32000;
static constexpr const uint32_t recording_external_sample_rate = sampler_audio_t::sample_rate;
static constexpr const uint32_t recording_buffer_frames = recording_external_sample_rate * sampler_pool_t::max_sample_sec;
static constexpr const uint32_t recording_chunk_frames = 512;
static constexpr const uint32_t external_probe_frames = recording_external_sample_rate / 5;  // 200ms
static constexpr const uint32_t loop_default_length_ms = 4000;  // 未確定時の表示用
static constexpr const uint32_t loop_min_length_ms = 250;
static constexpr const uint32_t background_loop_max_sec = 8;
static constexpr const size_t background_loop_max_wav_file_size =
  (size_t)sampler_audio_t::sample_rate * 2 /* stereo */ * sizeof(int16_t) * background_loop_max_sec + 4096;
static constexpr const uint32_t loop_quantize_step_options[] = { 8, 16, 32, 64, 128 };
static constexpr const uint32_t loop_del_long_press_ms = 480;
static constexpr const size_t loop_event_max = 256;

// LoopイベントはUI入力タスクが更新し、1ms再生タスクが別コアから読む。
// 短いクリティカル区間でvectorの再配置・erase中の読み込みを防ぐ。
#if !defined (M5UNIFIED_PC_BUILD)
static portMUX_TYPE loop_events_mux = portMUX_INITIALIZER_UNLOCKED;
#endif
struct loop_events_guard_t {
  loop_events_guard_t() {
#if !defined (M5UNIFIED_PC_BUILD)
    portENTER_CRITICAL(&loop_events_mux);
#endif
  }
  ~loop_events_guard_t() {
#if !defined (M5UNIFIED_PC_BUILD)
    portEXIT_CRITICAL(&loop_events_mux);
#endif
  }
};
static loop_event_t loop_due_events[loop_event_max];
static loop_event_t loop_playback_events[loop_event_max];
static volatile size_t loop_playback_event_count = 0;
static volatile uint32_t loop_events_revision = 1;
static volatile uint32_t loop_playback_revision = 0;
// The loop clock runs every millisecond.  Index recorded events by their
// transport region so a dense arrangement does not require scanning all 256
// entries for every tick.  Four 64-bit words cover the fixed event array.
static constexpr uint8_t loop_playback_bucket_count = 32;
static constexpr uint8_t loop_playback_bucket_words = (loop_event_max + 63) / 64;
static uint64_t loop_playback_bucket_mask[loop_playback_bucket_count][loop_playback_bucket_words] = {};
static uint32_t loop_playback_bucket_length_ms = 0;

//-------------------------------------------------------------------------
// 色定義

static constexpr uint32_t led_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (r << 16) | (g << 8) | b;
}

static constexpr uint32_t led_from_rgb24(uint32_t rgb) {
  return led_rgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

static constexpr uint32_t scale_rgb24(uint32_t color, uint8_t numerator, uint8_t denominator) {
  uint32_t r = ((color >> 16) & 0xFF) * numerator / denominator;
  uint32_t g = ((color >> 8) & 0xFF) * numerator / denominator;
  uint32_t b = (color & 0xFF) * numerator / denominator;
  return (r << 16) | (g << 8) | b;
}

static constexpr uint32_t darken_rgb24(uint32_t color, uint8_t shift = 3) {
  return (((color >> 16) & 0xFF) >> shift) << 16
       | (((color >> 8) & 0xFF) >> shift) << 8
       | ((color & 0xFF) >> shift);
}

struct mode_info_t {
  const char* name;
  uint32_t screen_color;
  uint32_t led_color;
};

static constexpr const mode_info_t mode_info[] = {
  { "SAMPLE", 0x4080E0u, led_rgb( 32,  96, 255) },
  { "PLAY", 0x40C040u, led_rgb( 32, 255,  32) },
  { "REC", 0xE04040u, led_rgb(255,  32,  32) },
  { "FX",   0xC040C0u, led_rgb(255,  32, 255) },
};

// モードごとのFnボタン機能名 (上から順)
static constexpr const char* const fn_labels[][3] = {
  { "PLAY", "MUTE", "DEL" },  // SAMPLE
  { "PLAY", "MUTE", "" },  // PLAY
  { "END",  "MUTE", "DEL"  },  // LOOP
  { "PLAY", "MUTE", "" },  // FX
};
static constexpr const char* const edit_param_labels[] = {
  "START", "END", "VOLUME", "PITCH", "REPEAT", "HOLD", "REVERSE",
  "LOOP IN", "LOOP OUT", "RELEASE", "LOOP"
};

// Pad配色 { 画面通常, 画面押下, LED通常, LED押下 }。
// 表示番号1〜6/7〜12で、赤・橙・黄・緑・青・紫を繰り返す。
struct pad_color_t { uint32_t bg; uint32_t bg_hi; uint32_t led; uint32_t led_hi; };
static constexpr const pad_color_t sample_colors[] = {
  { 0x702020u, 0xFF5050u, led_from_rgb24(0x702020u), led_from_rgb24(0xFF5050u) },
  { 0x703020u, 0xFF8040u, led_from_rgb24(0x703020u), led_from_rgb24(0xFF8040u) },
  { 0x707020u, 0xFFFF40u, led_from_rgb24(0x707020u), led_from_rgb24(0xFFFF40u) },
  { 0x207030u, 0x40FF70u, led_from_rgb24(0x207030u), led_from_rgb24(0x40FF70u) },
  { 0x203070u, 0x5080FFu, led_from_rgb24(0x203070u), led_from_rgb24(0x5080FFu) },
  { 0x602080u, 0xC040FFu, led_from_rgb24(0x602080u), led_from_rgb24(0xC040FFu) },
};
static constexpr const pad_color_t empty_color =
  { 0x282830u, 0x484858u, led_from_rgb24(0x282830u), led_from_rgb24(0x484858u) };
static constexpr const pad_color_t fn_color =
  { 0x303048u, 0x6060A0u, led_rgb(6, 6, 16), led_rgb(64, 64, 160) };
// FXの役割色。Tempo / Filter / Repeat / Delayの順で、上部パラメーター表示と
// Padコントロール面の両方に使う。
static constexpr uint32_t fx_control_colors[] = {
  0xF06050u,  // Tempo
  0x58C8E8u,  // Filter
  0xF0D050u,  // Repeat
  0x58D080u,  // Delay
};
static constexpr uint32_t fx_tape_stop_color = 0xB070E8u;

// Melody / BassのスケールPadは、音階上の役割を色で示す。ルートと
// 5度はキーが変わっても同じ役割なので、スケール表へ色を持たせず
// 実際の発音音程から一度だけ組み立ててキャッシュする。
static constexpr const pad_color_t pitched_root_color =
  { 0x603808u, 0xFF9A28u, led_from_rgb24(0x603808u), led_from_rgb24(0xFF9A28u) };
static constexpr const pad_color_t pitched_fifth_color =
  { 0x382060u, 0xB070FFu, led_from_rgb24(0x382060u), led_from_rgb24(0xB070FFu) };
static pad_color_t pitched_pad_colors[(uint8_t)performance_page_t::max][def::pad::pad_count];
struct pitched_pad_palette_state_t {
  uint8_t scale = 0xFF;
  uint8_t key = 0xFF;
  bool valid = false;
};
static pitched_pad_palette_state_t pitched_pad_palette_state[(uint8_t)performance_page_t::max];

//-------------------------------------------------------------------------
// ボタン配置変換
// 物理ボタンindex (0=左下, 5列x3行, 各行右端がFn) と Pad番号 (0=左上, 4列x3行) の対応

static int button_to_pad(int btn) {  // -1:Fn列 -2:範囲外
  if (btn < 0 || btn >= 15) { return -2; }
  int row = btn / 5;  // 0=下段
  int col = btn % 5;
  if (col == 4) { return -1; }
  return (2 - row) * 4 + col;
}

static uint8_t pad_to_button(uint8_t pad) {
  return (2 - pad / 4) * 5 + (pad % 4);
}

static uint8_t display_order_to_pad(uint8_t order) {
  return (2 - order / 4) * 4 + (order % 4);
}

static uint8_t pad_display_number(uint8_t pad) {
  return (2 - pad / 4) * 4 + (pad % 4) + 1;
}

static void refresh_pitched_pad_palette(performance_page_t page)
{
  if (page != performance_page_t::melody && page != performance_page_t::bass) { return; }
  const uint8_t page_index = (uint8_t)page;
  const uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
  const uint8_t key = pitched_page_key(page) % 12;
  auto& state = pitched_pad_palette_state[page_index];
  if (state.valid && state.scale == scale && state.key == key) { return; }

  const uint32_t page_color = performance_page_colors[page_index];
  // 役割を持たない音は少し無彩色寄りにして、橙のRootと紫の5度を
  // ひと目で拾えるようにする。これはScale/Key変更時だけ実行する。
  const uint32_t gray = (((page_color >> 16) & 0xFF)
                       + ((page_color >> 8) & 0xFF)
                       + (page_color & 0xFF)) / 3;
  const uint32_t bright = ((((page_color >> 16) & 0xFF) * 3 + gray) / 4) << 16
                        | ((((page_color >> 8) & 0xFF) * 3 + gray) / 4) << 8
                        | (((page_color & 0xFF) * 3 + gray) / 4);
  const uint32_t dim = darken_rgb24(bright, 2);
  const pad_color_t scale_color = {
    dim, bright, led_from_rgb24(dim), led_from_rgb24(bright)
  };
  const uint8_t fifth = (key + 7) % 12;
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    const uint8_t display_index = pad_display_number(pad) - 1;
    const uint8_t pitch_class = (sampler_scale_notes[scale][display_index] + key) % 12;
    pitched_pad_colors[page_index][pad] = pitch_class == key ? pitched_root_color
      : pitch_class == fifth ? pitched_fifth_color
      : scale_color;
  }
  state = { scale, key, true };
}

static void invalidate_pitched_pad_palette(performance_page_t page)
{
  if (page == performance_page_t::melody || page == performance_page_t::bass) {
    pitched_pad_palette_state[(uint8_t)page].valid = false;
  }
}

static int button_to_fn(int btn) {  // Fn番号 (0=上段) / -1:Fn以外
  if (btn % 5 != 4) { return -1; }
  return 2 - btn / 5;
}

static uint8_t fn_to_button(uint8_t fn) {
  return (2 - fn) * 5 + 4;
}

//-------------------------------------------------------------------------
// LED制御

static const pad_color_t& pad_colors(int pad) {
  if (current_page == performance_page_t::melody || current_page == performance_page_t::bass) {
    refresh_pitched_pad_palette(current_page);
    return pitched_pad_colors[(uint8_t)current_page][pad];
  }
  if (current_page != performance_page_t::sample) {
    return sample_colors[(pad_display_number((uint8_t)pad) - 1) % 6];
  }
  if (!sampler_pool_t::slot[pad].isValid()) { return empty_color; }
  return sample_colors[(pad_display_number((uint8_t)pad) - 1) % 6];
}

static bool pad_highlighted(int pad) {
  return pads[pad].pressed || pads[pad].playing_shown || recording_pad == pad || edit_pad == pad
      || sample_move_source_pad == pad || sample_move_copy_source_pad == pad
      || sample_add_armed_pad == pad || sample_add_action_pad == pad;
}

static uint32_t pad_off_background(const pad_color_t& color)
{
  return scale_rgb24(color.bg_hi, 3, 8);
}

// The display and button LEDs share the same logical RGB source.  The
// physical LEDs have different apparent brightness from the LCD, but using
// the actual control-surface background here keeps their hue and state in
// lockstep without per-screen LED palettes.
static uint32_t edit_pad_background(int pad)
{
  const uint8_t number = pad_display_number((uint8_t)pad);
  const auto& edited = sampler_pool_t::slot[edit_pad];
  const bool sustain_ready = edited.synth_sustain_mode == sample_sustain_mode_t::manual
    ? edited.synth_loop_end > edited.synth_loop_start + 31
    : edited.synth_sustain_mode == sample_sustain_mode_t::automatic && edited.synth_sustain_auto;
  uint32_t accent = 0x606068u;
  bool assigned = false;
  bool enabled = false;
  bool focused = false;
  bool menu_back = false;
  if (edit_chop_page) {
    switch (number) {
    case 1: accent = 0x70D8FFu; assigned = true; enabled = edit_chop_fit_mode == chop_fit_mode_t::fit_bgm; break;
    case 2: accent = 0xA0A8B8u; assigned = true; enabled = edit_chop_fit_mode == chop_fit_mode_t::keep_speed; break;
    case 5: accent = 0xF0C050u; assigned = true; enabled = edit_chop_count_mode == chop_count_mode_t::four; break;
    case 6: accent = 0xF0C050u; assigned = true; enabled = edit_chop_count_mode == chop_count_mode_t::eight; break;
    case 7: accent = 0xF0C050u; assigned = true; enabled = edit_chop_count_mode == chop_count_mode_t::twelve; break;
    case 8: accent = 0xF0C050u; assigned = true; enabled = edit_chop_count_mode == chop_count_mode_t::automatic; break;
    default: break;
    }
  } else if (edit_synth_page) {
    switch (number) {
    case 1:
      accent = performance_page_colors[(uint8_t)performance_page_t::melody];
      assigned = true;
      enabled = melody_settings.source == synth_tone_source_t::pad && melody_settings.pad == (uint8_t)edit_pad;
      break;
    case 2:
      accent = performance_page_colors[(uint8_t)performance_page_t::chord];
      assigned = true;
      enabled = chord_settings.source == synth_tone_source_t::pad && chord_settings.pad == (uint8_t)edit_pad;
      break;
    case 3:
      accent = performance_page_colors[(uint8_t)performance_page_t::bass];
      assigned = true;
      enabled = bass_settings.source == synth_tone_source_t::pad && bass_settings.pad == (uint8_t)edit_pad;
      break;
    case 8: accent = 0xFFD0D0u; assigned = enabled = menu_back = true; break;
    case 9: accent = 0x80E0B0u; assigned = true; enabled = sustain_ready; focused = edit_param == 10; break;
    case 10: accent = 0x50D8D0u; assigned = true; focused = edit_param == 7; break;
    case 11: accent = 0x50D8D0u; assigned = true; focused = edit_param == 8; break;
    case 12: accent = 0xF0A050u; assigned = true; focused = edit_param == 9; break;
    default: break;
    }
  } else {
    switch (number) {
    case 1: accent = 0xF0C050u; assigned = true; break;
    case 4: accent = 0xFF6060u; assigned = true; break;
    case 5: accent = 0x50C8D8u; assigned = true; enabled = edited.hold_enabled; focused = edit_param == 5; break;
    case 6: accent = 0xF0C050u; assigned = true; enabled = edited.loop_enabled; focused = edit_param == 4; break;
    case 7: accent = 0xD080E0u; assigned = true; enabled = edited.reverse; focused = edit_param == 6; break;
    case 8: accent = 0x50D8D0u; assigned = true; enabled = sustain_ready; break;
    case 9: accent = 0xFF7050u; assigned = true; focused = edit_param == 0; break;
    case 10: accent = 0x50A0FFu; assigned = true; focused = edit_param == 1; break;
    case 11: accent = 0x60E080u; assigned = true; focused = edit_param == 2; break;
    case 12: accent = 0xB080FFu; assigned = true; focused = edit_param == 3; break;
    default: break;
    }
  }
  const bool active = focused || pads[pad].pressed;
  if (menu_back) { return 0x483030u; }
  if (!assigned) { return 0x18181Eu; }
  return scale_rgb24(accent, active ? 3 : enabled ? 2 : 1, active ? 8 : enabled ? 9 : 7);
}

static uint32_t pad_led_surface_color(int pad)
{
  const auto& color = pad_colors(pad);
  if (edit_pad >= 0) { return edit_pad_background(pad); }
  if (current_mode == sampler_mode_t::mode_fx) {
    const uint8_t number = pad_display_number((uint8_t)pad);
    if (mixer_active) {
      const mixer_part_t mapped = mixer_part_for_pad_number(number);
      if (mapped != mixer_part_t::count) {
        const uint8_t part = (uint8_t)mapped;
        return mixer_part_muted[part] ? 0x141418u : scale_rgb24(mixer_part_colors[part], 1, 5);
      }
      if (number >= 9 && number <= 12) {
        return mixer_applied_snapshot == (int8_t)(number - 9) ? 0x80D0FFu : 0x08080Cu;
      }
      return pad_off_background(color);
    }
    if (number >= 1 && number <= 4) {
      return fx_pad_active == pad ? fx_control_colors[2] : 0x18181Eu;
    }
    if (number == 5) { return fx_pad_active == pad ? fx_control_colors[1] : 0x18181Eu; }
    if (number == 6) { return fx_pad_active == pad ? fx_control_colors[0] : 0x18181Eu; }
    if (number == 7) { return fx_pad_active == pad ? fx_tape_stop_color : 0x18181Eu; }
    if (number == 8) { return fx_pad_active == pad ? fx_control_colors[3] : 0x18181Eu; }
    return pad_off_background(empty_color);
  }
  return pad_highlighted(pad) || pad_repeat_next_msec[pad] ? color.bg_hi : pad_off_background(color);
}

static bool fn_modifier_hint(int fn)
{
  if (!fn_modifier_hint_visible || edit_pad >= 0) { return false; }
  return fn == 1
      && (current_mode == sampler_mode_t::mode_rec
       || current_mode == sampler_mode_t::mode_play)
      && (current_page == performance_page_t::sample
       || current_page == performance_page_t::drum);
}

static void update_pad_led(int pad) {
  kp::system_registry->rgbled_control.setColor(pad_to_button(pad),
    led_from_rgb24(pad_led_surface_color(pad)));
}

static void update_fn_led(int fn) {
  bool active = fn_pressed[fn];
  if (current_mode == sampler_mode_t::mode_fx && fn == 2 && mixer_active) { active = true; }
  if (fn == 1 && (current_page == performance_page_t::melody
               || current_page == performance_page_t::bass
               || current_page == performance_page_t::chord)
      && performance_page_part_muted(current_page)) { active = true; }
  uint32_t surface = active ? fn_color.bg_hi : fn_color.bg;
  if (!active && fn_modifier_hint(fn)) { surface = 0x34344Cu; }
  kp::system_registry->rgbled_control.setColor(fn_to_button(fn), led_from_rgb24(surface));
}

static void update_mode_leds(void) {
  for (int i = 0; i < (int)sampler_mode_t::mode_max; ++i) {
    bool active = (i == (int)current_mode);
    uint32_t color = mode_info[i].led_color;
    if (!active) {  // 非選択モードは減光
      color = led_rgb(((color >> 16) & 0xFF) >> 4, ((color >> 8) & 0xFF) >> 4, (color & 0xFF) >> 4);
    }
    kp::system_registry->rgbled_control.setColor(15 + i, color);
  }
}

static void update_all_leds(void) {
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { update_pad_led(i); }
  for (int i = 0; i < 3; ++i) { update_fn_led(i); }
  update_mode_leds();
}

static void request_grid_draw(void);

static void refresh_pitched_pad_visuals(performance_page_t page)
{
  invalidate_pitched_pad_palette(page);
  refresh_pitched_pad_palette(page);
  if (current_page != page) { return; }
  // Scale / Key変更時だけ全Padを更新する。演奏中のPad描画には入らない。
  request_grid_draw();
  update_all_leds();
}

//-------------------------------------------------------------------------
// 画面描画

static bool sound_priority_active(uint32_t now = M5.millis())
{
  return (int32_t)(sound_priority_until_msec - now) > 0;
}

static bool sound_attack_guard_active(uint32_t now = M5.millis())
{
  return (int32_t)(sound_attack_guard_until_msec - now) > 0;
}

static void mark_sound_priority(uint32_t hold_ms = 90)
{
  const uint32_t now = M5.millis();
  uint32_t until = now + hold_ms;
  if ((int32_t)(until - sound_priority_until_msec) > 0) {
    sound_priority_until_msec = until;
  }
  sampler_audio_t::setPerformancePriority(true);
  // LCD work only needs to stay clear of the attack edge itself. Longer UI
  // suppression remains in force for pad/header decoration, while the live
  // scope may resume shortly afterwards through the Core-0 strip renderer.
  const uint32_t attack_until = now + 12;
  if ((int32_t)(attack_until - sound_attack_guard_until_msec) > 0) {
    sound_attack_guard_until_msec = attack_until;
  }
}

static void invalidate_loop_timeline_cache(void)
{
  loop_timeline_cache_valid = false;
  loop_cursor_prev_x = -1;
  loop_timeline_dirty_span_count = 0;
  uint32_t next = loop_events_revision + 1u;
  loop_events_revision = next ? next : 1u;
}

static void advance_loop_events_revision(void)
{
  uint32_t next = loop_events_revision + 1u;
  loop_events_revision = next ? next : 1u;
}

static bool append_loop_timeline_event(const loop_event_t& event);
static void refresh_loop_playback_events(void);

static bool loop_playback_buckets_ready(void)
{
  return loop_length_fixed && loop_length_msec != 0
      && loop_playback_bucket_length_ms == loop_length_msec;
}

static uint8_t loop_playback_bucket_for_pos(uint32_t pos_ms)
{
  const uint32_t length = loop_length_msec;
  if (length == 0) { return 0; }
  const uint32_t pos = pos_ms % length;
  return (uint8_t)std::min<uint32_t>(loop_playback_bucket_count - 1,
    ((uint64_t)pos * loop_playback_bucket_count) / length);
}

static uint32_t sample_anchor_preroll_ms(const sample_slot_t& slot)
{
  if (!slot.beatAnchorValid() || !slot.sample_rate || !slot.pitch_q8) { return 0; }
  const uint32_t frames = slot.beat_anchor_frame - slot.playStart();
  return (uint32_t)(((uint64_t)frames * 1000u * 256u)
                  / ((uint64_t)slot.sample_rate * slot.pitch_q8));
}

static uint32_t loop_event_playback_pos(const loop_event_t& event)
{
  if (!loop_length_msec || event.page != performance_page_t::sample
   || event.type != loop_event_type_t::note_on
   || event.pad >= def::pad::pad_count) { return event.pos_ms; }
  const uint32_t pre_roll = sample_anchor_preroll_ms(sampler_pool_t::slot[event.pad]);
  if (!pre_roll) { return event.pos_ms; }
  const uint32_t pos = event.pos_ms % loop_length_msec;
  const uint32_t shift = pre_roll % loop_length_msec;
  return pos >= shift ? pos - shift : loop_length_msec - shift + pos;
}

static void add_loop_playback_bucket_event(size_t index, const loop_event_t& event)
{
  if (!loop_playback_buckets_ready() || index >= loop_event_max) { return; }
  const uint8_t bucket = loop_playback_bucket_for_pos(loop_event_playback_pos(event));
  loop_playback_bucket_mask[bucket][index >> 6] |= 1ull << (index & 63);
}

// Record events are normally append-only.  When the playback snapshot is
// current, publish that single new entry directly instead of making the
// 1ms clock copy the complete loop list again.  Deletions and a full list
// still take the regular locked rebuild path below.
static bool append_loop_playback_snapshot(const loop_event_t& event)
{
  if (loop_playback_revision != loop_events_revision
   || loop_playback_event_count >= loop_event_max) { return false; }
  const size_t index = loop_playback_event_count;
  loop_playback_events[index] = event;
  add_loop_playback_bucket_event(index, event);
  __sync_synchronize();
  loop_playback_event_count = index + 1;
  return true;
}

static uint32_t performance_event_time(void)
{
  return input_event_msec ? input_event_msec : M5.millis();
}

static bool physical_input_pending(void)
{
  return kp::system_registry != nullptr
      && kp::system_registry->internal_input.getButtonBitmask() != prev_bitmask;
}

static void request_wave_draw(void)
{
  dirty_wave = true;
  if (current_mode == sampler_mode_t::mode_loop || (current_mode == sampler_mode_t::mode_play && loop_playing)) {
    invalidate_loop_timeline_cache();
  }
}

static void request_header_draw(void)
{
  dirty_header = true;
}

static void request_pad_draw(int pad)
{
  if (pad >= 0 && pad < (int)def::pad::pad_count) {
    dirty_pad_mask |= (uint16_t)(1u << pad);
  }
}

static void request_pad_state_draw(int pad)
{
  if (pad >= 0 && pad < (int)def::pad::pad_count) {
    dirty_pad_state_mask |= (uint16_t)(1u << pad);
  }
}

static void request_fn_draw(int fn)
{
  if (fn >= 0 && fn < 3) {
    dirty_fn_mask |= (uint8_t)(1u << fn);
  }
}

static void request_all_fn_draw(void)
{
  dirty_fn_mask |= 0x07u;
}

static void request_grid_draw(void)
{
  dirty_pad_mask = (1u << def::pad::pad_count) - 1;
  dirty_pad_state_mask = 0;
  request_all_fn_draw();
}

static void request_surface_sync(void)
{
  // Dirty regions handle the immediate response. This is only a delayed
  // recovery path for a dropped async LCD transfer during startup or a fast
  // page/mode switch.
  surface_sync_deadline_msec = M5.millis() + 420;
}

static void draw_battery_icon(int x, int y, int w, int h, uint8_t battery, bool charging)
{
  auto& d = M5.Display;
  y += 2;
  h -= 4;
  d.fillRect(x, y - 2, w, 2, 0x000000u);
  d.fillRect(x + (w >> 2), y, w - (w >> 1), -2, TFT_WHITE);
  d.drawRect(x, y, w, h, TFT_WHITE);

  x += 2;
  w -= 4;
  y += 2;
  h -= 4;
  int level_h = h * battery / 100;
  d.fillRect(x, y, w, h, 0x000000u);
  d.fillRect(x, y + h - level_h, w, level_h, charging ? TFT_GREEN : TFT_WHITE);
}

static void draw_volume_icon(int x, int y, int w, int h, uint8_t volume)
{
  auto& d = M5.Display;
  int r = 10;
  int cx = x + w - 12;
  int cy = y + 11;
  int arc = volume * 36 / 10;
  d.fillCircle(cx, cy, r, 0x303030u);
  d.drawCircle(cx, cy, r, TFT_LIGHTGRAY);
  d.setColor(TFT_WHITE);
  d.fillArc(cx, cy, r, 0, 90, 90 + arc);
}

// KANTAN Play本体と同じSTA Wi-Fi表示。接続待ちは小さなドットだけを
// 暗く表示し、受信強度に応じてアークを増やす。
static void draw_wifi_icon(int x, int y, int w, int h, kp::def::command::wifi_sta_info_t status)
{
  int level = 0;
  switch (status) {
  case kp::def::command::wifi_sta_info_t::wsi_waiting:  level = 1; break;
  case kp::def::command::wifi_sta_info_t::wsi_signal_1: level = 2; break;
  case kp::def::command::wifi_sta_info_t::wsi_signal_2: level = 3; break;
  case kp::def::command::wifi_sta_info_t::wsi_signal_3: level = 4; break;
  case kp::def::command::wifi_sta_info_t::wsi_signal_4: level = 5; break;
  default: return;
  }

  auto& d = M5.Display;
  const uint32_t on = 0xFFFFFFu;
  const uint32_t off = 0x000000u;
  const int cx = x + (w >> 1);
  const int cy = y + h - 5;
  d.fillCircle(cx, cy, 2, level > 0 ? on : off);
  d.fillArc(cx, cy, 7, 6, 230, 310, level > 1 ? on : off);
  d.fillArc(cx, cy, 11, 10, 230, 310, level > 2 ? on : off);
  d.fillArc(cx, cy, 15, 14, 230, 310, level > 3 ? on : off);
  d.fillArc(cx, cy, 19, 18, 230, 310, level > 4 ? on : off);
}

static void draw_performance_record_icon(int x, int y, int w, int h, bool recording)
{
  auto& d = M5.Display;
  const int radius = recording ? 6 : 5;
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  d.fillCircle(cx, cy, radius + 2, 0x000000u);
  d.fillCircle(cx, cy, radius, recording ? 0xF02020u : 0x803030u);
}

static void draw_record_button_icon(m5gfx::LovyanGFX& d, int cx, int cy, bool active, uint32_t bg)
{
  // Keep the armed state unmistakable without introducing another bitmap
  // asset: Fn1 uses the same red record mark as the header status gauge.
  const uint32_t ring = active ? 0xFFFFFFu : 0xA04040u;
  const uint32_t fill = active ? 0xFF4040u : 0xD02020u;
  d.fillCircle(cx, cy, 7, bg);
  d.drawCircle(cx, cy, 6, ring);
  d.fillCircle(cx, cy, 4, fill);
}

static void draw_header(bool force = false) {
  if (ui_surface_exclusive) { return; }
  auto& d = M5.Display;
  struct header_cache_t {
    bool valid = false;
    uint8_t battery = 255;
    bool charging = false;
    uint8_t volume = 255;
    uint8_t wifi_sta = 255;
    bool performance_record = false;
    uint8_t update_status = 255;
    uint8_t page = 255;
    uint8_t mode = 255;
  };
  static header_cache_t cache;

  uint8_t battery = kp::system_registry->runtime_info.getBatteryLevel();
  if (battery > 100) { battery = 100; }
  battery = (battery + 2) / 5 * 5;  // 細かな揺れでアイコンが点滅しないように丸める
  bool charging = kp::system_registry->runtime_info.getBatteryCharging();
  uint8_t volume = kp::system_registry->user_setting.getMasterVolume();
  auto wifi_sta = kp::system_registry->runtime_info.getWiFiSTAInfo();
  const bool performance_record = performance_record_armed || performance_record_active;
  uint8_t update_status = kp::system_registry->runtime_info.getWiFiOtaProgress();

  static constexpr const int32_t battery_icon_w = 14;
  static constexpr const int32_t icon_gap = 2;
  static constexpr const int32_t wifi_icon_w = 23;
  static constexpr const int32_t volume_icon_w = header_h;
  bool show_wifi = wifi_sta != kp::def::command::wifi_sta_info_t::wsi_off;
  const bool show_status_icon = show_wifi || performance_record;
  int32_t right_w = battery_icon_w + icon_gap + volume_icon_w;
  if (show_status_icon) { right_w += wifi_icon_w + icon_gap; }
  int32_t right_x = d.width() - right_w;
  int32_t status_x = right_x;
  if (show_status_icon) { right_x += wifi_icon_w + icon_gap; }
  const int32_t battery_x = right_x;
  const int32_t volume_x = battery_x + battery_icon_w + icon_gap;

  const bool layout_changed = !cache.valid
    || (show_status_icon != (cache.wifi_sta != (uint8_t)kp::def::command::wifi_sta_info_t::wsi_off
                          || cache.performance_record));
  const bool static_changed = layout_changed || cache.page != (uint8_t)current_page
                           || cache.mode != (uint8_t)current_mode
                           || cache.update_status != update_status
                           || cache.performance_record != performance_record;
  if (!force && cache.valid && !static_changed
   && cache.battery == battery && cache.charging == charging
   && cache.volume == volume && cache.wifi_sta == (uint8_t)wifi_sta
   && cache.performance_record == performance_record) {
    return;
  }

  d.startWrite();
  if (force || !cache.valid || static_changed) {
    // レイアウトや文字列が変わる時だけヘッダー全体を組み直す。
    // The header identifies the active performance page without competing
    // with the white status icons. Mode identity remains with the tabs and
    // outer frame below it.
    const uint32_t header_background = performance_page_header_backgrounds[(uint8_t)current_page];
    d.fillRect(0, 0, d.width(), header_h, header_background);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_left);
    d.setTextColor(0xFFFFFFu, header_background);
    d.drawString(performance_page_names[(uint8_t)current_page], 7, header_h / 2);
    // Four compact markers make the side-button page cycle visible without
    // consuming the pad or mode areas.
    const int page_mark_x = 73;
    const uint8_t page_index = performance_page_order_index(current_page);
    for (uint8_t i = 0; i < (uint8_t)performance_page_t::max; ++i) {
      d.fillCircle(page_mark_x + i * 8, header_h / 2, i == page_index ? 3 : 2,
                   i == page_index ? 0xFFFFFFu : 0x202024u);
    }
    if (update_status == (uint8_t)kp::def::command::wifi_ota_state_t::ota_update_available) {
      d.setTextColor(0xFFFFFFu, header_background);
      d.drawString("UP!", 104, header_h / 2);
    }
    if (performance_record) { draw_performance_record_icon(status_x, 0, wifi_icon_w, header_h, performance_record_active); }
    else if (show_wifi) { draw_wifi_icon(status_x, 0, wifi_icon_w, header_h, wifi_sta); }
    draw_battery_icon(battery_x, 0, battery_icon_w, header_h, battery, charging);
    draw_volume_icon(volume_x, 0, volume_icon_w, header_h, volume);
  } else {
    // 通常更新は変化したアイコンの矩形だけ。黒塗りでヘッダー全体を消さない。
    const uint32_t header_background = performance_page_header_backgrounds[(uint8_t)current_page];
    if (cache.battery != battery || cache.charging != charging) {
      d.fillRect(battery_x, 0, battery_icon_w, header_h, header_background);
      draw_battery_icon(battery_x, 0, battery_icon_w, header_h, battery, charging);
    }
    if (cache.volume != volume) {
      d.fillRect(volume_x, 0, volume_icon_w, header_h, header_background);
      draw_volume_icon(volume_x, 0, volume_icon_w, header_h, volume);
    }
    if (cache.wifi_sta != (uint8_t)wifi_sta || cache.performance_record != performance_record) {
      // Wi-Fiアークは表示枠の左へ張り出すため、右側のアイコン領域だけを再描画する。
      d.fillRect(status_x, 0, d.width() - status_x, header_h, header_background);
      if (performance_record) { draw_performance_record_icon(status_x, 0, wifi_icon_w, header_h, performance_record_active); }
      else if (show_wifi) { draw_wifi_icon(status_x, 0, wifi_icon_w, header_h, wifi_sta); }
      draw_battery_icon(battery_x, 0, battery_icon_w, header_h, battery, charging);
      draw_volume_icon(volume_x, 0, volume_icon_w, header_h, volume);
    }
  }
  d.endWrite();

  cache.valid = true;
  cache.battery = battery;
  cache.charging = charging;
  cache.volume = volume;
  cache.wifi_sta = (uint8_t)wifi_sta;
  cache.performance_record = performance_record;
  cache.update_status = update_status;
  cache.page = (uint8_t)current_page;
  cache.mode = (uint8_t)current_mode;
  rendered_header_page = current_page;
  rendered_header_mode = current_mode;
}

static inline int32_t apply_wave_volume(int16_t value, uint16_t volume_q8)
{
  int32_t scaled = ((int32_t)value * volume_q8) >> 8;
  if (scaled > INT16_MAX) { return INT16_MAX; }
  if (scaled < INT16_MIN) { return INT16_MIN; }
  return scaled;
}

// パッド用の安全な再生レベルは約-12dBFSなので、そのままでは編集波形が小さく見える。
// 表示だけを3倍に拡大し、音量やリミッターには一切影響させない。
static inline int16_t apply_sample_wave_display_gain(int16_t value, uint16_t volume_q8)
{
  int32_t scaled = apply_wave_volume(value, volume_q8) * 3;
  if (scaled > INT16_MAX) { return INT16_MAX; }
  if (scaled < INT16_MIN) { return INT16_MIN; }
  return (int16_t)scaled;
}

static uint32_t loop_speed_ratio_q8(void)
{
  return fx_speed_active ? fx_speed_ratio_current_q8 : 256;
}

static uint32_t loop_display_length_ms(uint32_t now)
{
  if (loop_playing && loop_record_enabled && !loop_length_fixed) {
    uint32_t elapsed = ((uint64_t)(now - loop_start_msec) * loop_speed_ratio_q8()) >> 8;
    if (elapsed < loop_min_length_ms) { elapsed = loop_min_length_ms; }
    return elapsed;
  }
  return loop_length_fixed ? loop_length_msec : loop_default_length_ms;
}

static uint32_t loop_pos_ms(uint32_t now)
{
  uint32_t length_ms = loop_display_length_ms(now);
  if (!loop_playing) {
    return loop_prev_pos_ms < length_ms ? loop_prev_pos_ms : 0;
  }
  if (loop_record_enabled && !loop_length_fixed) {
    return loop_display_length_ms(now);
  }
  return (((uint64_t)(now - loop_start_msec) * loop_speed_ratio_q8()) >> 8) % length_ms;
}

// While capturing a loop without BGM, the display keeps a 250ms minimum so
// the timeline remains drawable. Recording must not inherit that minimum:
// the first hit is the transport origin and has to be stored at exactly 0ms.
static uint32_t loop_record_pos_ms(uint32_t now)
{
  if (loop_playing && loop_record_enabled && !loop_length_fixed) {
    if ((int32_t)(now - loop_capture_zero_until_msec) < 0) { return 0; }
    return ((uint64_t)(now - loop_start_msec) * loop_speed_ratio_q8()) >> 8;
  }
  return loop_pos_ms(now);
}

static uint32_t background_loop_length_ms(void)
{
  if (!background_loop.isValid()) { return 0; }
  uint32_t repeats = std::max<uint8_t>(1, background_loop.loop_repeats);
  uint32_t real_ms = ((uint64_t)background_loop.frames * 1000) / background_loop.sample_rate;
  return std::max<uint32_t>(loop_min_length_ms, real_ms * repeats);
}

static uint8_t loop_quantize_option_count(void)
{
  return sizeof(loop_quantize_step_options) / sizeof(loop_quantize_step_options[0]);
}

static uint32_t loop_quantize_steps(void)
{
  uint8_t count = loop_quantize_option_count();
  uint8_t index = loop_quantize_option_index < count ? loop_quantize_option_index : 0;
  return loop_quantize_step_options[index];
}

static uint32_t loop_note_off_quantize_steps(void)
{
  uint8_t count = loop_quantize_option_count();
  uint8_t index = loop_note_off_quantize_option_index < count ? loop_note_off_quantize_option_index : loop_quantize_option_index;
  return loop_quantize_step_options[index];
}

static uint32_t loop_quantize_step_ms(uint32_t length_ms)
{
  return std::max<uint32_t>(1, length_ms / loop_quantize_steps());
}

// Keep the playable subdivision near a 120 BPM sixteenth note without
// exposing BPM to the user. Quantize enable remains a separate preference;
// this only selects the resolution used when it is enabled.
static void auto_configure_loop_grid(uint32_t length_ms)
{
  if (length_ms == 0) { return; }
  static constexpr uint32_t target_step_ms = 125;
  uint8_t best_index = 0;
  uint32_t best_error = UINT32_MAX;
  for (uint8_t index = 0; index < loop_quantize_option_count(); ++index) {
    const uint32_t steps = loop_quantize_step_options[index];
    const uint32_t step_ms = std::max<uint32_t>(1, (length_ms + steps / 2) / steps);
    const uint32_t error = step_ms > target_step_ms
      ? step_ms - target_step_ms : target_step_ms - step_ms;
    if (error < best_error) {
      best_error = error;
      best_index = index;
    }
  }
  loop_quantize_option_index = best_index;
  loop_note_off_quantize_option_index = std::min<uint8_t>(
    best_index + 1, loop_quantize_option_count() - 1);
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(length_ms));
  refresh_sample_grid_loop_intervals();
}

static uint8_t sample_loop_grid_index(uint8_t half_steps)
{
  for (uint8_t i = 0; i < loop_repeat_option_count; ++i) {
    if (loop_repeat_half_steps[i] == half_steps) { return i; }
  }
  return 1;  // 4 steps
}

// Repeat selector order: None, Whole Sample, then musical grid values.
// Keeping Whole Sample separate from loop_grid_half_steps avoids treating a
// sample-duration loop as a malformed Note Grid value when Kits are restored.
static int sample_repeat_option_index(const sample_slot_t& slot)
{
  if (!slot.loop_enabled) { return -1; }
  return slot.loop_whole_sample ? 0 : 1 + sample_loop_grid_index(slot.loop_grid_half_steps);
}

static const char* sample_repeat_label(const sample_slot_t& slot)
{
  if (!slot.loop_enabled) { return "None"; }
  if (slot.loop_whole_sample) { return whole_sample_repeat_label; }
  return sample_repeat_grid_labels[sample_loop_grid_index(slot.loop_grid_half_steps)];
}

static int sample_loop_target_pad(void)
{
  if (play_focus_pad >= 0 && play_focus_pad < (int)def::pad::pad_count
   && sampler_pool_t::slot[play_focus_pad].isValid()) {
    return play_focus_pad;
  }
  return -1;
}

static uint32_t sample_loop_interval_ms(const sample_slot_t& slot)
{
  uint32_t length = loop_length_fixed ? loop_length_msec : loop_default_length_ms;
  uint32_t step_ms = loop_quantize_step_ms(length);
  uint32_t interval = std::max<uint32_t>(1, ((uint64_t)step_ms * slot.loop_grid_half_steps) / 2);
  // Pitch FX中はトランスポート/BGMと同じ実時間に合わせる。
  uint32_t speed_q8 = loop_playing ? loop_speed_ratio_q8() : 256;
  return std::max<uint32_t>(1, (uint32_t)(((uint64_t)interval << 8) / speed_q8));
}

static bool loop_grid_transport_active(void)
{
  // Master Repeat only replaces the final PCM output. The dry transport and
  // all note schedulers continue on their normal musical grid underneath it.
  return loop_playing && loop_length_fixed && loop_length_msec != 0;
}

static uint32_t loop_grid_total_half_steps(void)
{
  return loop_quantize_steps() * 2;
}

// 演奏した位置に最も近い最小グリッドを位相として採用する。
// interval自体の倍数へ丸めないため、裏拍で始めた2拍Loopも裏拍のまま続く。
static uint16_t loop_nearest_grid_phase_half_step(uint32_t pos_ms, uint8_t minimum_half_steps)
{
  const uint32_t length = loop_length_msec;
  const uint32_t total = loop_grid_total_half_steps();
  if (length == 0 || total == 0 || minimum_half_steps == 0) { return 0; }
  pos_ms %= length;
  uint32_t nearest = (uint32_t)(((uint64_t)pos_ms * total + length / 2) / length);
  nearest = ((nearest + minimum_half_steps / 2) / minimum_half_steps) * minimum_half_steps;
  return (uint16_t)(nearest % total);
}

static uint32_t loop_next_phase_position_ms(uint32_t pos_ms, uint16_t phase_half_step, uint8_t interval_half_steps)
{
  const uint32_t length = loop_length_msec;
  const uint32_t total = loop_grid_total_half_steps();
  if (length == 0 || total == 0 || interval_half_steps == 0) { return 0; }
  pos_ms %= length;

  // A grid position is converted to milliseconds with integer truncation.
  // Converting that millisecond value back with floor() can therefore yield
  // the preceding grid index (for example with an odd-length BGM), causing
  // the scheduler to select the same position repeatedly.  Start from the
  // first grid point strictly after pos_ms, then align it to this loop's
  // phase and interval.
  const uint32_t first_after = (uint32_t)((((uint64_t)pos_ms + 1) * total
                                         + length - 1) / length);
  // Work with the phase modulo the repeat interval, not with its absolute
  // position in this particular loop pass.  For example a Repeat 8 started
  // at grid 48 must continue as 0, 16, 32, 48 after wrapping.  Reusing 48 as
  // an absolute anchor here would incorrectly skip 16 and 32 on the next lap.
  const uint32_t phase = phase_half_step % interval_half_steps;
  const uint32_t remainder = first_after % interval_half_steps;
  const uint32_t advance = (phase + interval_half_steps - remainder) % interval_half_steps;
  const uint32_t next = (first_after + advance) % total;
  return (uint32_t)(((uint64_t)next * length) / total);
}

// Convert a transport grid point into an absolute scheduler deadline. Unlike
// loop_prev_pos_ms this is not touched by recording, so a held lever Repeat
// keeps firing even while it adds notes to the current loop.
static uint32_t loop_transport_deadline_msec(uint32_t now, uint32_t target_pos_ms)
{
  if (!loop_grid_transport_active() || loop_length_msec == 0) { return now; }
  const uint32_t pos = loop_pos_ms(now);
  target_pos_ms %= loop_length_msec;
  const uint32_t distance = target_pos_ms >= pos
    ? target_pos_ms - pos : loop_length_msec - pos + target_pos_ms;
  const uint16_t speed = std::max<uint16_t>(1, loop_speed_ratio_q8());
  return now + (uint32_t)(((uint64_t)distance << 8) / speed);
}

static void arm_sample_grid_loop_next(int pad, uint32_t now, bool preserve_phase = false)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  const auto& slot = sampler_pool_t::slot[pad];
  if (loop_grid_transport_active()) {
    if (!preserve_phase || !sample_grid_loop_transport_locked[pad]) {
      // Pad Loopの最小単位は現在のNote Grid。例: 32分Gridなら32分位置へ丸める。
      sample_grid_loop_phase_half_step[pad] = loop_nearest_grid_phase_half_step(loop_pos_ms(now), 2);
    }
    sample_grid_loop_transport_locked[pad] = true;
    sample_grid_loop_next_pos_ms[pad] = loop_next_phase_position_ms(
      loop_pos_ms(now), sample_grid_loop_phase_half_step[pad], slot.loop_grid_half_steps);
    // Keep an absolute deadline as the scheduler source of truth.  The loop
    // playhead is also updated by recording and BGM resync, so using only a
    // "did it cross this position" test can skip a Grid Repeat pulse.
    sample_grid_loop_next_msec[pad] = loop_transport_deadline_msec(
      now, sample_grid_loop_next_pos_ms[pad]);
  } else {
    sample_grid_loop_transport_locked[pad] = false;
    sample_grid_loop_next_msec[pad] = now + sample_loop_interval_ms(slot);
  }
}

static uint8_t sampler_sustain_cache_slot_for_pad(int pad)
{
  for (uint8_t i = 0; i < sampler_sustain_cache_slot_count; ++i) {
    if (sampler_sustain_cache_owner[i] == pad) {
      return sampler_sustain_cache_slot_base + i;
    }
  }
  return 0xFF;
}

static void request_sampler_sustain_cache(int pad)
{
  if (pad >= 0 && pad < (int)def::pad::pad_count) {
    sampler_sustain_cache_pending[pad] = true;
  }
}

static void clear_sampler_sustain_cache_for_pad(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  synth_sustain_analysis_pending[pad] = false;
  sampler_sustain_cache_pending[pad] = false;
  for (uint8_t i = 0; i < sampler_sustain_cache_slot_count; ++i) {
    if (sampler_sustain_cache_owner[i] != pad) { continue; }
    const uint8_t cache_slot = sampler_sustain_cache_slot_base + i;
    // The caller stops the Pad voice before deleting its PCM.  Leave a cache
    // referenced by any other voice intact as a final guard, even though the
    // Sample-page cache normally belongs only to this Pad voice.
    if (!sampler_audio_t::isSynthSustainCacheInUse(cache_slot)) {
      sampler_audio_t::clearSynthSustainCache(cache_slot);
      sampler_sustain_cache_owner[i] = -1;
    }
  }
}

static void service_sampler_sustain_cache(void)
{
  // Never copy PSRAM material while a performance may need the I2S deadline.
  if (loop_playing || sound_priority_active() || physical_input_pending()) { return; }
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (!sampler_sustain_cache_pending[pad]) { continue; }
    const auto& slot = sampler_pool_t::slot[pad];
    const uint32_t source_start = slot.playStart();
    uint32_t sustain_start = 0;
    uint32_t sustain_end = 0;
    uint16_t sustain_crossfade = 0;
    if (!slot.isValid() || !synth_sustain_parameters(slot, source_start,
        &sustain_start, &sustain_end, &sustain_crossfade)) {
      sampler_sustain_cache_pending[pad] = false;
      continue;
    }

    uint8_t cache_slot = sampler_sustain_cache_slot_for_pad(pad);
    if (cache_slot != 0xFF && sampler_audio_t::isSynthSustainCacheInUse(cache_slot)) {
      continue;  // An edited range must not overwrite an audible loop body.
    }
    if (cache_slot == 0xFF) {
      for (uint8_t i = 0; i < sampler_sustain_cache_slot_count; ++i) {
        const uint8_t candidate = sampler_sustain_cache_slot_base + i;
        if (!sampler_audio_t::isSynthSustainCacheInUse(candidate)) {
          cache_slot = candidate;
          sampler_sustain_cache_owner[i] = (int8_t)pad;
          break;
        }
      }
    }
    if (cache_slot == 0xFF) { return; }

    // Sample-page caches are intentionally compact: 1,024 attack frames and
    // 4,096 sustain frames per source keep all three inside roughly 30KB.
    sampler_audio_t::primeSynthSustainCache(cache_slot, slot.pcm + source_start,
                                             sustain_start, sustain_end, 1024, 4096);
    sampler_sustain_cache_pending[pad] = false;
    break;  // one maintenance copy per idle update
  }
}

static bool play_sample_sustain_voice(int pad, bool auto_release)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return false; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return false; }
  uint32_t sustain_start = 0;
  uint32_t sustain_end = 0;
  uint16_t sustain_crossfade = 0;
  const uint32_t source_start = slot.playStart();
  if (!synth_sustain_parameters(slot, source_start,
                                &sustain_start, &sustain_end, &sustain_crossfade)) {
    return false;
  }
  const uint16_t auto_release_ms = auto_release
    ? sample_sustain_auto_release_ms(slot, sustain_start, sustain_end) : 0;
  request_sampler_sustain_cache(pad);
  const uint8_t cache_slot = sampler_sustain_cache_slot_for_pad(pad);
  return sampler_audio_t::playSynth((uint8_t)pad, slot.pcm + source_start,
    slot.playEnd() - source_start, slot.sample_rate, true, false,
    mixer_scaled_volume_q8(mixer_part_t::sampler, slot.volume_q8),
    slot.pitch_q8, 0, slot.synth_release_ms,
    sustain_start, sustain_end, sustain_crossfade, auto_release_ms,
    true, 1, cache_slot);
}

static void play_sample_once(int pad, uint32_t source_offset_frames = 0)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  if (source_offset_frames == 0 && play_sample_sustain_voice(pad, !slot.hold_enabled)) { return; }
  source_offset_frames = std::min<uint32_t>(source_offset_frames, slot.playFrames() - 1);
  sampler_audio_t::play((uint8_t)pad, slot.pcm + slot.playStart() + source_offset_frames,
                        slot.playFrames() - source_offset_frames, slot.sample_rate,
                        false, slot.reverse,
                        mixer_scaled_volume_q8(mixer_part_t::sampler, slot.volume_q8),
                        slot.pitch_q8);
}

static void play_sample_whole_loop(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  sampler_audio_t::play((uint8_t)pad, slot.pcm + slot.playStart(), slot.playFrames(), slot.sample_rate,
                        true, slot.reverse,
                        mixer_scaled_volume_q8(mixer_part_t::sampler, slot.volume_q8),
                        slot.pitch_q8);
  sample_whole_loop_active[pad] = true;
}

static void stop_sample_grid_loop(int pad, bool stop_voice = true)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  sample_grid_loop_active[pad] = false;
  sample_grid_loop_active_mask &= ~(uint16_t)(1u << pad);
  sample_whole_loop_active[pad] = false;
  sample_grid_loop_next_msec[pad] = 0;
  sample_grid_loop_transport_locked[pad] = false;
  sample_grid_loop_next_pos_ms[pad] = 0;
  sample_grid_loop_phase_half_step[pad] = 0;
  if (stop_voice) { sampler_audio_t::stop((uint8_t)pad); }
}

static void start_sample_grid_loop(int pad, uint32_t now, bool play_now = true)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || !slot.loop_enabled || slot.loop_whole_sample) { return; }
  if (play_now) { play_sample_once(pad); }
  sample_grid_loop_active[pad] = true;
  sample_grid_loop_active_mask |= (uint16_t)(1u << pad);
  arm_sample_grid_loop_next(pad, now);
}

static void clear_sample_grid_loops(void)
{
  sample_grid_loop_active_mask = 0;
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    sample_grid_loop_active[pad] = false;
    if (sample_whole_loop_active[pad]) { sampler_audio_t::stop((uint8_t)pad); }
    sample_whole_loop_active[pad] = false;
    sample_grid_loop_next_msec[pad] = 0;
    sample_grid_loop_transport_locked[pad] = false;
    sample_grid_loop_next_pos_ms[pad] = 0;
    sample_grid_loop_phase_half_step[pad] = 0;
  }
}

static void refresh_sample_grid_loop_intervals(void)
{
  uint32_t now = M5.millis();
  uint16_t active = sample_grid_loop_active_mask;
  while (active) {
    const int pad = __builtin_ctz(active);
    active &= active - 1;
    arm_sample_grid_loop_next(pad, now, true);
  }
}

static void service_sample_grid_loops(uint32_t now)
{
  uint16_t active = sample_grid_loop_active_mask;
  while (active) {
    const int pad = __builtin_ctz(active);
    active &= active - 1;
    auto& slot = sampler_pool_t::slot[pad];
    if (!slot.isValid() || !slot.loop_enabled || slot.loop_whole_sample) {
      stop_sample_grid_loop(pad);
      continue;
    }
    if (sample_grid_loop_transport_locked[pad]) {
      if (!loop_grid_transport_active()) {
        arm_sample_grid_loop_next(pad, now);
        continue;
      }
      if ((int32_t)(now - sample_grid_loop_next_msec[pad]) < 0) { continue; }
      play_sample_once(pad);
      sample_grid_loop_next_pos_ms[pad] = loop_next_phase_position_ms(
        sample_grid_loop_next_pos_ms[pad], sample_grid_loop_phase_half_step[pad], slot.loop_grid_half_steps);
      sample_grid_loop_next_msec[pad] = loop_transport_deadline_msec(
        now, sample_grid_loop_next_pos_ms[pad]);
      continue;
    }
    if ((int32_t)(now - sample_grid_loop_next_msec[pad]) < 0) { continue; }
    play_sample_once(pad);
    uint32_t interval = sample_loop_interval_ms(slot);
    do {
      sample_grid_loop_next_msec[pad] += interval;
    } while ((int32_t)(now - sample_grid_loop_next_msec[pad]) >= 0);
  }
}

static void sample_loop_grid_add(int diff)
{
  int pad = sample_loop_target_pad();
  if (pad < 0 || diff == 0) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  int index = sample_loop_grid_index(slot.loop_grid_half_steps) + diff;
  index = std::clamp<int>(index, 0, (int)loop_repeat_option_count - 1);
  if (sample_whole_loop_active[pad]) { stop_sample_grid_loop(pad); }
  slot.loop_whole_sample = false;
  slot.loop_grid_half_steps = loop_repeat_half_steps[index];
  if (sample_grid_loop_active[pad]) {
    arm_sample_grid_loop_next(pad, M5.millis());
  }
  request_wave_draw();
}

static bool loop_recording_notice_active(void)
{
  return current_mode == sampler_mode_t::mode_loop
      && loop_playing
      && loop_record_enabled
      && !loop_length_fixed;
}

static bool uses_incremental_wave_transfer(void)
{
  return !page_selector_visible
      && current_page == performance_page_t::sample
      && current_mode == sampler_mode_t::mode_play
      && !loop_playing
      && !play_loop_grid_information_active();
}

static void wait_wave_transfer_job(void)
{
#if !defined(M5UNIFIED_PC_BUILD)
  if (!wave_transfer_job_pending) { return; }
  const uint32_t deadline = M5.millis() + 20;
  while (wave_transfer_job_pending && (int32_t)(M5.millis() - deadline) < 0) {
    M5.delay(1);
  }
#endif
}

static void service_wave_transfer(void)
{
  if (!wave_transfer_active) { return; }
  // Compact overlays are direct LCD draws. Pause the background transfer
  // while one is visible instead of making both cores repaint each other.
  if (hold_progress_kind != hold_progress_kind_t::none) { return; }
  if (hold_progress_kind == hold_progress_kind_t::loop_clear) {
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    return;
  }
  if (ui_surface_exclusive) {
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    return;
  }
  if (!wave_transfer_full_frame && !uses_incremental_wave_transfer()) {
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    return;
  }
  // A newly started free-length loop replaces the stopped timeline with the
  // RECORDING surface.  Let its first few narrow strips through even during
  // the attack guard: this clears the old length label immediately without
  // paying for a full-frame LCD transfer in the performance path.
  const bool recording_notice_leading_strip = loop_recording_notice_active()
    && wave_transfer_full_frame && wave_transfer_y < 24;
  if ((sound_attack_guard_active() || physical_input_pending())
   && !recording_notice_leading_strip) { return; }
  if (wave_transfer_job_pending) { return; }

  if (wave_transfer_h <= 0) {
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    return;
  }

  // 強いアタックでは更新帯が波形エリア全体まで広がる。まとめてSPI転送すると
  // 次のPad入力を数ms以上待たせるため、小さな横帯に分割して転送する。
  // 物理ボタンに未処理の変化がある時は、表示より入力を先に処理する。
  if (kp::system_registry->internal_input.getButtonBitmask() != prev_bitmask) { return; }
  static constexpr int live_wave_transfer_chunk_height = 8;
  const int transfer_h = std::min<int>(wave_transfer_h, live_wave_transfer_chunk_height);
  if (hold_progress_kind != hold_progress_kind_t::none) {
    int chip_x = 0;
    int chip_y = 0;
    int chip_w = 0;
    int chip_h = 0;
    hold_progress_bounds(&chip_x, &chip_y, &chip_w, &chip_h);
    (void)chip_x;
    (void)chip_w;
    if (wave_transfer_y < chip_y + chip_h && wave_transfer_y + transfer_h > chip_y) {
      hold_progress_needs_redraw = true;
    }
  }
#if !defined(M5UNIFIED_PC_BUILD)
  if (touch_render_queue != nullptr && touch_render_task_handle != nullptr) {
    touch_render_state_t state;
    state.command = touch_render_command_t::wave_strip;
    state.wave_y = wave_transfer_y;
    state.wave_h = transfer_h;
    state.wave_generation = wave_transfer_generation;
    wave_transfer_job_pending = true;
    if (xQueueOverwrite(touch_render_queue, &state) == pdTRUE) { return; }
    wave_transfer_job_pending = false;
  }
#endif
  M5.Display.setClipRect(0, wave_y + wave_transfer_y, wave_canvas.width(), transfer_h);
  wave_canvas.pushSprite(0, wave_y);
  M5.Display.clearClipRect();
  wave_transfer_y += transfer_h;
  wave_transfer_h -= transfer_h;
  if (wave_transfer_h <= 0) {
    // ClipRectの実装によって外枠の一部が欠ける機種があるため、
    // 最後の帯の転送後に外枠だけを復元する。波形そのものは再描画しない。
    if (!wave_transfer_full_frame) { draw_live_wave_frame(); }
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
  }
}

static void push_wave_canvas(void)
{
  wait_wave_transfer_job();
  auto& c = wave_canvas;
  uint32_t color = mode_info[(int)current_mode].screen_color;
  c.drawRect(0, 0, c.width(), c.height(), color);
  c.drawRect(1, 1, c.width() - 2, c.height() - 2, color);
  wave_transfer_active = false;
  wave_transfer_full_frame = false;
  c.pushSprite(0, wave_y);
  if (hold_progress_kind != hold_progress_kind_t::none) {
    hold_progress_needs_redraw = true;
  }
}

static void queue_wave_canvas_full_transfer(void)
{
  auto& c = wave_canvas;
  uint32_t color = mode_info[(int)current_mode].screen_color;
  c.drawRect(0, 0, c.width(), c.height(), color);
  c.drawRect(1, 1, c.width() - 2, c.height() - 2, color);
  wave_transfer_y = 0;
  wave_transfer_h = c.height();
  wave_transfer_full_frame = true;
  wave_transfer_active = true;
}

static void reset_live_wave(void)
{
  wave_transfer_generation = wave_transfer_generation + 1;
  live_wave_initialized = false;
  live_wave_prev_top = 0;
  live_wave_prev_bottom = 0;
  wave_transfer_active = false;
  wave_transfer_full_frame = false;
}

static void restore_sample_preview_cursor_columns(int center_x)
{
  if (center_x < 0) { return; }
  const int x = std::max(0, center_x - 1);
  const int width = std::min<int>(3, wave_canvas.width() - x);
  if (width <= 0) { return; }
  M5.Display.setClipRect(x, wave_y, width, wave_canvas.height());
  wave_canvas.pushSprite(0, wave_y);
  M5.Display.clearClipRect();
}

static void service_sample_preview_cursor(uint32_t now)
{
  if (wave_transfer_job_pending || wave_transfer_active) { return; }
#if !defined(M5UNIFIED_PC_BUILD)
  // Pad/Fn/grid sprites are transferred from Core 0.  The preview cursor is
  // decorative, so skip a frame instead of sharing M5GFX's direct LCD state
  // with another renderer and risking a cross-core SPI transaction race.
  if (ui_dirty_canvas_busy[0] || ui_dirty_canvas_busy[1]) { return; }
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    if (grid_cache_busy[i]) { return; }
  }
#endif
  // The jog selector owns this part of the display until it closes. Do not
  // restore or draw cursor columns through its opaque sprite.
  if (page_selector_visible) { return; }
  // The preview cursor also writes directly to the LCD. While a long-press
  // popup is visible, do not even restore its previous columns because that
  // clipped Wave Canvas transfer can erase the popup. Popup teardown requests
  // a complete wave redraw, so forgetting the old cursor is sufficient.
  if (hold_progress_kind != hold_progress_kind_t::none || sample_edit_armed_active(now)) {
    sample_preview_cursor_prev_x = -1;
    return;
  }
  const int pad = edit_pad >= 0 ? edit_pad : rec_wave_pad;
  const bool preview_mode = current_page == performance_page_t::sample
                         && current_mode == sampler_mode_t::mode_rec
                         && !loop_playing
                         && pad >= 0 && pad < (int)def::pad::pad_count
                         && sampler_pool_t::slot[pad].isValid();
  uint32_t frame = 0;
  uint32_t frames = 0;
  if (!preview_mode || !sampler_audio_t::getPlaybackPosition((uint8_t)pad, &frame, &frames)
   || frames == 0) {
    if (sample_preview_cursor_prev_x >= 0) {
      restore_sample_preview_cursor_columns(sample_preview_cursor_prev_x);
      sample_preview_cursor_prev_x = -1;
    }
    return;
  }
  if (sound_priority_active(now)) { return; }
  static constexpr uint32_t cursor_interval_msec = 33;
  if (now - sample_preview_cursor_prev_msec < cursor_interval_msec) { return; }
  sample_preview_cursor_prev_msec = now;

  // The audio voice reports its position relative to the edited Start--End
  // range. Convert it back into the original sample's coordinate space before
  // drawing over the full waveform. This also makes a Whole Sample loop snap
  // back to the visible Start marker every time it repeats.
  const auto& slot = sampler_pool_t::slot[pad];
  const uint32_t play_start = slot.playStart();
  const uint32_t play_end = slot.playEnd();
  const uint32_t play_frames = play_end > play_start ? play_end - play_start : 0;
  if (slot.frames == 0 || play_frames == 0) { return; }
  uint32_t source_frame = play_start;
  if (edit_chop_page && edit_chop_preview_plan_valid
   && edit_chop_preview_last >= 0
   && edit_chop_preview_last < (int8_t)edit_chop_preview_count) {
    // Chop PLAY feeds only one prospective Slice to the audio voice, so its
    // reported frame starts at zero on every press. Rebase that local frame
    // onto the original Start--End waveform to show which Slice is sounding.
    const uint8_t slice = (uint8_t)edit_chop_preview_last;
    const uint32_t slice_start = edit_chop_preview_starts[slice];
    const uint32_t slice_end = edit_chop_preview_ends[slice];
    const uint32_t slice_frames = slice_end > slice_start ? slice_end - slice_start : 1;
    source_frame = play_start + slice_start
                 + std::min<uint32_t>(frame, slice_frames - 1);
  } else {
    const uint32_t local_frame = std::min<uint32_t>(frame, play_frames - 1);
    source_frame = slot.reverse
      ? play_end - 1 - local_frame
      : play_start + local_frame;
  }
  const int width = wave_canvas.width();
  const int cursor_x = std::min<int>(width - 1,
    (int)(((uint64_t)std::min<uint32_t>(source_frame, slot.frames - 1) * width) / slot.frames));
  if (cursor_x == sample_preview_cursor_prev_x) { return; }
  restore_sample_preview_cursor_columns(sample_preview_cursor_prev_x);
  restore_sample_preview_cursor_columns(cursor_x);

  auto& d = M5.Display;
  d.startWrite();
  d.drawFastVLine(cursor_x, wave_y + 2, wave_canvas.height() - 4, 0xB0E8FFu);
  d.endWrite();
  sample_preview_cursor_prev_x = cursor_x;
}

static void draw_live_wave_frame(void)
{
  auto& d = M5.Display;
  const uint32_t color = mode_info[(int)current_mode].screen_color;
  d.startWrite();
  d.drawRect(0, wave_y, wave_canvas.width(), wave_canvas.height(), color);
  d.drawRect(1, wave_y + 1, wave_canvas.width() - 2, wave_canvas.height() - 2, color);
  d.endWrite();
}

// M5CanvasのRGB565バッファを直接扱う。M5GFX内部のバイト順に合わせることで、
// drawFastVLineを数百回呼ぶより小さいCPU負荷で波形の更新帯だけを描ける。
static void clear_live_wave_band(M5Canvas& canvas, int y, int h)
{
  if (h <= 0) { return; }
  const int width = canvas.width();
  auto* pixels = (m5gfx::swap565_t*)canvas.getBuffer();
  // 外枠は静的なまま残し、内側だけを背景色へ戻す。
  pixels += y * width + 2;
  const uint16_t background = __builtin_bswap16(0x0841u);
  for (int yy = 0; yy < h; ++yy) {
    auto* row = pixels;
    pixels += width;
    for (int x = 2; x < width - 2; ++x) {
      row[x - 2].raw = background;
    }
  }
}

static void or_live_wave_bar(M5Canvas& canvas, int x, int y0, int y1, uint16_t color)
{
  if (y1 <= y0) { return; }
  const int width = canvas.width();
  auto* pixels = (m5gfx::swap565_t*)canvas.getBuffer();
  pixels += y0 * width + x;
  color = __builtin_bswap16(color);
  for (int y = y0; y < y1; ++y) {
    pixels[0].raw |= color;
    pixels[1].raw |= color;
    pixels += width;
  }
}

static void or_live_wave_center_line(M5Canvas& canvas, int y, uint16_t color)
{
  const int width = canvas.width();
  auto* pixels = (m5gfx::swap565_t*)canvas.getBuffer();
  pixels += y * width + 2;
  color = __builtin_bswap16(color);
  for (int x = 2; x < width - 2; ++x) {
    pixels[x - 2].raw |= color;
  }
}

static void draw_live_wave(void)
{
  auto& c = wave_canvas;
  const int w = c.width();
  const int h = c.height();
  auto reg = kp::system_registry;
  if (w <= 0 || h <= 0 || reg == nullptr) { return; }

  // SAMPLER page keeps its identity and the live scope in one panel.  The
  // title area is static; only the lower plot is touched by the fast transfer.
  const int plot_top = live_wave_info_bottom_y + 2;
  const int plot_bottom = h - 3;
  const int plot_h = std::max(1, plot_bottom - plot_top + 1);

  uint8_t low = UINT8_MAX;
  uint8_t high = 0;
  for (int i = 0; i < reg->raw_wave_length; ++i) {
    auto level = reg->raw_wave[i];
    if (level.first < low) { low = level.first; }
    if (level.second > high) { high = level.second; }
  }
  int current_top = plot_bottom - (high * plot_h) / 256;
  int current_bottom = plot_bottom - (low * plot_h) / 256;
  if (current_bottom <= current_top) { current_bottom = current_top + 1; }
  current_top = std::max(plot_top, current_top);
  current_bottom = std::min(plot_bottom, current_bottom);

  const bool first_frame = !live_wave_initialized;
  if (first_frame) {
    const uint32_t background = performance_page_backgrounds[(uint8_t)performance_page_t::sample];
    const uint32_t accent = performance_page_colors[(uint8_t)performance_page_t::sample];
    c.fillScreen(background);
    c.drawRect(0, 0, w, h, mode_info[(int)current_mode].screen_color);
    c.drawRect(1, 1, w - 2, h - 2, mode_info[(int)current_mode].screen_color);
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(2);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextColor(accent, background);
    c.drawString("SAMPLER", w / 2, 17);
    c.drawFastHLine(12, 36, w - 24, darken_rgb24(accent, 1));
    c.fillRect(2, plot_top, w - 4, plot_h, 0x080810u);
    live_wave_initialized = true;
  }

  int band_top = first_frame ? plot_top : std::max(plot_top, std::min(current_top, live_wave_prev_top) - 1);
  int band_bottom = first_frame ? plot_bottom : std::min(plot_bottom, std::max(current_bottom, live_wave_prev_bottom) + 1);
  if (!first_frame) {
    clear_live_wave_band(c, band_top, band_bottom - band_top + 1);
  }
  live_wave_prev_top = current_top;
  live_wave_prev_bottom = current_bottom;

  const int center = plot_top + plot_h / 2;
  if (band_top <= center && center <= band_bottom) {
    or_live_wave_center_line(c, center, 0x1244u);
  }

  const int pos = reg->raw_wave_pos;
  for (int x = 2; x < w - 2; x += live_wave_scan_stride) {
    // 最新の音を左端へ入れ、右方向へ飛ばす。演奏の立ち上がりが
    // パッド配置と同じ左→右の視線移動で追える、レーザー風の表示になる。
    int offset = x - 2;
    int idx0 = (pos - 1 - offset + reg->raw_wave_length * 2) % reg->raw_wave_length;
    int idx1 = (idx0 - 1 + reg->raw_wave_length) % reg->raw_wave_length;
    auto level0 = reg->raw_wave[idx0];
    auto level1 = reg->raw_wave[idx1];
    uint8_t low = std::min(level0.first, level1.first);
    uint8_t high = std::max(level0.second, level1.second);
    int y0 = plot_bottom - (high * plot_h) / 256;
    int y1 = plot_bottom - (low * plot_h) / 256;
    y0 = std::max(y0, band_top);
    y1 = std::min(y1, band_bottom + 1);
    if (y1 <= y0) { continue; }
    or_live_wave_bar(c, x, y0, y1, 0x35EEu);
  }

  // 初回だけはタイトルと外枠も含めて分割転送する。全面pushSpriteを避けるため、
  // ここでも同じ小さな帯のキューに載せる。
  wave_transfer_y = first_frame ? 0 : band_top;
  wave_transfer_h = first_frame ? h : band_bottom - band_top + 1;
  wave_transfer_active = true;
}

// 選択中の最小グリッドをすべて残し、偶数番目だけ少し広く吸着させる。
// 2倍間隔を約18%優遇し、4倍/8倍位置には追加の重みを付けない。
static uint32_t weighted_quantize_loop_pos_ms(uint32_t pos_ms, uint32_t length_ms, uint32_t steps)
{
  if (length_ms == 0 || steps == 0) { return 0; }
  pos_ms %= length_ms;
  uint32_t best_pos = 0;
  uint32_t best_distance = UINT32_MAX;
  uint64_t best_score = UINT64_MAX;
  for (uint32_t index = 0; index < steps; ++index) {
    uint32_t candidate = ((uint64_t)index * length_ms) / steps;
    uint32_t distance = candidate > pos_ms ? candidate - pos_ms : pos_ms - candidate;
    if (distance > length_ms - distance) { distance = length_ms - distance; }

    // scoreが小さい候補ほど選ばれる。偶数位置の840/1024は、
    // 最小グリッドより約18%広い吸着範囲になる。
    const uint32_t weight = (index & 1u) == 0 ? 840 : 1024;
    uint64_t score = (uint64_t)distance * weight;
    if (score < best_score || (score == best_score && distance < best_distance)) {
      best_score = score;
      best_distance = distance;
      best_pos = candidate;
    }
  }
  return best_pos;
}

static void set_loop_quantize_enabled(bool enabled)
{
  loop_quantize_enabled = enabled;
}

static void set_loop_quantize_option(uint8_t index, bool requantize_existing)
{
  uint8_t count = loop_quantize_option_count();
  if (index >= count) { index = count ? count - 1 : 0; }
  loop_quantize_option_index = index;
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_display_length_ms(M5.millis())));
  refresh_sample_grid_loop_intervals();
  if (requantize_existing && loop_quantize_enabled && loop_length_fixed) {
    loop_events_guard_t guard;
    for (auto& e : loop_events) {
      if (e.type == loop_event_type_t::note_off) {
        uint32_t steps = loop_note_off_quantize_steps();
        uint32_t step = ((uint64_t)e.pos_ms * steps + (loop_length_msec / 2)) / loop_length_msec;
        e.pos_ms = ((uint64_t)(step % steps) * loop_length_msec) / steps;
      } else {
        e.pos_ms = weighted_quantize_loop_pos_ms(e.pos_ms, loop_length_msec, loop_quantize_steps());
      }
    }
  }
}

static void set_loop_note_off_quantize_option(uint8_t index, bool requantize_existing)
{
  uint8_t count = loop_quantize_option_count();
  if (index >= count) { index = count ? count - 1 : 0; }
  loop_note_off_quantize_option_index = index;
  if (requantize_existing && loop_quantize_enabled && loop_length_fixed) {
    loop_events_guard_t guard;
    uint32_t steps = loop_note_off_quantize_steps();
    for (auto& e : loop_events) {
      if (e.type != loop_event_type_t::note_off) { continue; }
      uint32_t step = ((uint64_t)e.pos_ms * steps + (loop_length_msec / 2)) / loop_length_msec;
      step %= steps;
      e.pos_ms = ((uint64_t)step * loop_length_msec) / steps;
    }
  }
}

static uint32_t quantize_loop_pos_ms(uint32_t pos_ms, uint32_t length_ms)
{
  if (!loop_quantize_enabled) { return pos_ms; }
  if (length_ms < loop_min_length_ms) { length_ms = loop_min_length_ms; }
  return weighted_quantize_loop_pos_ms(pos_ms, length_ms, loop_quantize_steps());
}

static uint32_t quantize_loop_pos_ms(uint32_t pos_ms)
{
  return quantize_loop_pos_ms(pos_ms, loop_display_length_ms(M5.millis()));
}

static uint32_t quantize_loop_note_off_pos_ms(uint32_t pos_ms, uint32_t length_ms)
{
  if (!loop_quantize_enabled) { return pos_ms; }
  if (length_ms < loop_min_length_ms) { length_ms = loop_min_length_ms; }
  uint32_t steps = loop_note_off_quantize_steps();
  uint32_t step = ((uint64_t)pos_ms * steps + (length_ms / 2)) / length_ms;
  step %= steps;
  return ((uint64_t)step * length_ms) / steps;
}

// A Note On deferred to the upcoming beat must survive a quick physical
// release.  Put its Note Off on the first finer grid after the Note On so the
// event has an audible, repeatable duration instead of being deleted before
// the transport reaches its scheduled start.
static uint32_t loop_note_off_after_note_on(uint32_t note_on_pos_ms, uint32_t length_ms)
{
  if (!loop_quantize_enabled || length_ms < loop_min_length_ms) {
    return note_on_pos_ms;
  }
  const uint32_t steps = loop_note_off_quantize_steps();
  if (steps == 0) { return note_on_pos_ms; }
  uint32_t step = ((uint64_t)(note_on_pos_ms % length_ms) * steps) / length_ms;
  step = (step + 1) % steps;
  return ((uint64_t)step * length_ms) / steps;
}

static uint32_t separate_overlapping_note_off(uint16_t layer, uint32_t off_pos,
                                              uint32_t length_ms)
{
  if (layer == 0 || length_ms < loop_min_length_ms) { return off_pos; }
  const auto note_on = std::find_if(loop_events.begin(), loop_events.end(),
    [layer](const loop_event_t& event) {
      return event.layer == layer && event.type == loop_event_type_t::note_on;
    });
  if (note_on == loop_events.end() || note_on->pos_ms != off_pos) { return off_pos; }
  return loop_note_off_after_note_on(note_on->pos_ms, length_ms);
}

static uint32_t loop_forward_distance_ms(uint32_t from_ms, uint32_t to_ms, uint32_t length_ms)
{
  if (length_ms == 0) { return 0; }
  from_ms %= length_ms;
  to_ms %= length_ms;
  return to_ms >= from_ms ? (to_ms - from_ms) : (length_ms - from_ms + to_ms);
}

static bool loop_should_defer_quantized_note(uint32_t raw_pos, uint32_t quantized_pos)
{
  if (!loop_quantize_enabled || !loop_playing || !loop_length_fixed || loop_length_msec == 0) { return false; }
  uint32_t step_ms = loop_quantize_step_ms(loop_length_msec);
  // Playページでは短い先読みだけを行い、通常演奏のレスポンスを守る。
  uint32_t early_window_ms = std::min<uint32_t>(24, std::max<uint32_t>(10, step_ms / 5));
  uint32_t ahead_ms = loop_forward_distance_ms(raw_pos, quantized_pos, loop_length_msec);
  return ahead_ms > 0 && ahead_ms <= early_window_ms;
}

static bool loop_should_defer_recorded_note(uint32_t raw_pos, uint32_t quantized_pos)
{
  if (!loop_quantize_enabled || !loop_playing || !loop_length_fixed || loop_length_msec == 0) { return false; }
  const uint32_t min_grid_ms = loop_quantize_step_ms(loop_length_msec);
  const uint32_t ahead_ms = loop_forward_distance_ms(raw_pos, quantized_pos, loop_length_msec);
  // Loop録音は固定msではなく、選択中の最小グリッドの半分を境界にする。
  // 前半は次のグリッドへ、後半は直前のグリッドへ置く。
  return ahead_ms > 0 && ahead_ms <= min_grid_ms / 2;
}

// Quantizeは記録位置にだけ適用する。最小グリッドの半分を越えて
// 次へ進むイベントは待たせず直前のグリッドへ戻し、同じ周回での
// 二重発音を防ぐ。
static uint32_t loop_previous_quantize_pos_ms(uint32_t raw_pos, uint32_t length_ms)
{
  if (!loop_quantize_enabled || length_ms == 0) { return raw_pos; }
  const uint32_t steps = loop_quantize_steps();
  if (steps == 0) { return raw_pos; }
  const uint32_t step = ((uint64_t)(raw_pos % length_ms) * steps) / length_ms;
  return ((uint64_t)step * length_ms) / steps;
}

static uint32_t loop_record_note_on_pos_ms(uint32_t raw_pos, bool allow_defer, bool* defer)
{
  if (defer) { *defer = false; }
  if (!loop_length_fixed || loop_length_msec == 0 || !loop_quantize_enabled) { return raw_pos; }

  const uint32_t quantized = quantize_loop_pos_ms(raw_pos, loop_length_msec);
  const uint32_t ahead_ms = loop_forward_distance_ms(raw_pos, quantized, loop_length_msec);
  if (ahead_ms == 0) { return quantized; }
  if (allow_defer && loop_should_defer_recorded_note(raw_pos, quantized)) {
    if (defer) { *defer = true; }
    return quantized;
  }
  return loop_previous_quantize_pos_ms(raw_pos, loop_length_msec);
}

// Input history carries the physical edge time. If the main task receives an
// edge just after its intended grid boundary, that boundary is already in the
// past even though the recorded position is correct. In that case play now;
// waiting for the event scanner would postpone live audio by a whole cycle.
static bool loop_deferred_grid_is_still_ahead(uint32_t quantized_pos)
{
  if (!loop_playing || !loop_length_fixed || loop_length_msec == 0) { return false; }
  const uint32_t current_pos = loop_record_pos_ms(M5.millis());
  const uint32_t ahead_ms = loop_forward_distance_ms(current_pos, quantized_pos, loop_length_msec);
  const uint32_t half_grid_ms = std::max<uint32_t>(1, loop_quantize_step_ms(loop_length_msec) / 2);
  return ahead_ms > 0 && ahead_ms <= half_grid_ms;
}

static constexpr uint32_t loop_live_min_gate_ms = 16;

static void quantize_loop_events_to_length(uint32_t length_ms)
{
  loop_events_guard_t guard;
  for (auto& e : loop_events) {
    e.pos_ms = e.type == loop_event_type_t::note_off
      ? quantize_loop_note_off_pos_ms(e.pos_ms, length_ms)
      : quantize_loop_pos_ms(e.pos_ms, length_ms);
  }
}

static constexpr const int loop_timeline_inset_x = 4;

static int loop_timeline_x(uint32_t pos_ms, uint32_t length_ms)
{
  if (length_ms == 0) { return loop_timeline_inset_x; }
  int left = loop_timeline_inset_x;
  int right = wave_canvas.width() - 1 - loop_timeline_inset_x;
  if (right <= left) { return left; }
  return left + (((uint64_t)pos_ms * (right - left)) / length_ms);
}

// The waveform/timeline canvas is the retained background for every moving
// cursor. Restoring even a narrow column through writePixel() creates hundreds
// of SPI operations and becomes conspicuously jerky under dense playback.
// A clipped sprite push keeps the same cached pixels but transfers them as one
// short DMA-friendly strip instead.
static void restore_wave_canvas_columns(int center_x)
{
  if (center_x < 0) { return; }
  const int x = std::max(0, center_x - 1);
  const int width = std::min<int>(3, wave_canvas.width() - x);
  if (width <= 0) { return; }
  M5.Display.setClipRect(x, wave_y, width, wave_canvas.height());
  wave_canvas.pushSprite(0, wave_y);
  M5.Display.clearClipRect();
}

static void queue_loop_timeline_dirty_span(int center_x, int radius = 3)
{
  const int left = std::max(0, center_x - radius);
  const int right = std::min<int>(wave_canvas.width() - 1, center_x + radius);
  if (right < left) { return; }
  for (uint8_t i = 0; i < loop_timeline_dirty_span_count; ++i) {
    auto& span = loop_timeline_dirty_spans[i];
    if (right + 1 < span.left || left > span.right + 1) { continue; }
    span.left = std::min(span.left, left);
    span.right = std::max(span.right, right);
    return;
  }
  if (loop_timeline_dirty_span_count >= loop_timeline_dirty_span_max) {
    // Dense input can exceed the cheap span budget.  Fall back to one correct
    // full redraw on the next UI turn rather than issuing a wide LCD transfer
    // on the input path.
    loop_timeline_cache_valid = false;
    loop_cursor_prev_x = -1;
    loop_timeline_dirty_span_count = 0;
    return;
  }
  loop_timeline_dirty_spans[loop_timeline_dirty_span_count++] = { left, right };
}

static void flush_loop_timeline_dirty_spans(void)
{
  for (uint8_t i = 0; i < loop_timeline_dirty_span_count; ++i) {
    const auto& span = loop_timeline_dirty_spans[i];
    const int width = span.right - span.left + 1;
    if (width <= 0) { continue; }
    M5.Display.setClipRect(span.left, wave_y, width, wave_canvas.height());
    wave_canvas.pushSprite(0, wave_y);
    M5.Display.clearClipRect();
  }
  loop_timeline_dirty_span_count = 0;
}

static int loop_timeline_lane_y(uint8_t pad, int lane_h)
{
  const int display_order = (int)pad_display_number(pad) - 1;
  return 8 + ((int)def::pad::pad_count - 1 - display_order) * lane_h;
}

static void draw_loop_timeline_event_to_canvas(const loop_event_t& event, uint32_t length_ms)
{
  auto& c = wave_canvas;
  const int h = c.height();
  const int lane_h = std::max<int>(1, (h - 16) / def::pad::pad_count);
  const int mark_h = std::min<int>(6, std::max<int>(3, lane_h - 1));
  const int x = loop_timeline_x(event.pos_ms, length_ms);
  if (loop_event_is_pitch_bend(event.type)) {
    const int y = event.type == loop_event_type_t::pitch_bend_up ? h - 7
      : event.type == loop_event_type_t::pitch_bend_down ? h - 3 : h - 5;
    const uint32_t color = performance_page_part_muted(event.page)
      ? 0x606068u : performance_page_colors[(uint8_t)event.page];
    c.fillCircle(x, y, 2, color);
    queue_loop_timeline_dirty_span(x, 3);
    return;
  }
  if (event.pad >= def::pad::pad_count) { return; }
  const int y = loop_timeline_lane_y(event.pad, lane_h);
  const uint32_t color = loop_is_muted(event.page, event.pad) ? 0x606068u : pad_colors(event.pad).bg_hi;
  if (event.type == loop_event_type_t::note_off) {
    c.drawFastHLine(x - 2, y + mark_h / 2, 5, color);
  } else {
    c.fillRect(x - 1, y, 3, mark_h, color);
  }
  queue_loop_timeline_dirty_span(x, 3);
}

static bool append_loop_timeline_event(const loop_event_t& event)
{
  if (!loop_timeline_cache_valid || !loop_length_fixed
   || event.page != current_page || loop_recording_notice_active()
   || ui_surface_exclusive || page_selector_visible) { return false; }
  const bool timeline_visible = current_mode == sampler_mode_t::mode_loop
    || (current_mode == sampler_mode_t::mode_play && loop_playing);
  if (!timeline_visible || loop_length_msec == 0) { return false; }
  draw_loop_timeline_event_to_canvas(event, loop_length_msec);
  return true;
}

static void draw_loop_cursor_only(uint32_t length_ms)
{
  // Keep the timeline visually still behind an opaque overlay. The transport
  // and audio continue; only this direct-to-LCD cursor pauses, then the
  // overlay teardown restores one clean timeline frame at the current point.
  // In particular, the page selector occupies the same Wave area and must
  // never race the cursor's per-column restore operation.
  if (hold_progress_kind != hold_progress_kind_t::none || page_selector_visible) { return; }
  if (wave_transfer_job_pending) { return; }
#if !defined(M5UNIFIED_PC_BUILD)
  // Pad/Fn/grid sprites are transferred by the Core-0 renderer. M5GFX keeps
  // one SPI transaction depth for the display, so starting this direct cursor
  // draw while another task owns that transaction can make the wrong task
  // release its mutex. The cursor is decorative: skip this frame instead of
  // blocking input/audio or allowing cross-task startWrite/endWrite pairs.
  if (ui_dirty_canvas_busy[0] || ui_dirty_canvas_busy[1]) { return; }
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    if (grid_cache_busy[i]) { return; }
  }
#endif
  if (ui_surface_exclusive) { return; }
  if (!loop_timeline_cache_valid || length_ms == 0) { return; }
  // New recorded notes were already composited into the retained sprite. Push
  // only their tiny dirty columns before restoring/redrawing the cursor, so
  // the cursor remains on top of both old and just-recorded events.
  flush_loop_timeline_dirty_spans();
  int play_x = loop_timeline_x(loop_pos_ms(M5.millis()), length_ms);
  if (play_x == loop_cursor_prev_x) { return; }
  // Restore the old cursor and the destination from the retained timeline in
  // two small clipped blits. This is dramatically cheaper than 3 x 112
  // individual writePixel calls and keeps the static dots perfectly intact.
  restore_wave_canvas_columns(loop_cursor_prev_x);
  restore_wave_canvas_columns(play_x);
  auto& d = M5.Display;
  d.startWrite();
  uint32_t cursor_color = loop_playing ? 0xFFFFFFu : 0x808090u;
  int chip_x = 0;
  int chip_y = 0;
  int chip_w = 0;
  int chip_h = 0;
  if (fn_information_chip_bounds(&chip_x, &chip_y, &chip_w, &chip_h)
   && play_x >= chip_x && play_x < chip_x + chip_w) {
    if (chip_y > 0) { d.drawFastVLine(play_x, wave_y, chip_y, cursor_color); }
    int lower_y = chip_y + chip_h;
    if (lower_y < wave_canvas.height()) {
      d.drawFastVLine(play_x, wave_y + lower_y, wave_canvas.height() - lower_y, cursor_color);
    }
  } else {
    d.drawFastVLine(play_x, wave_y, wave_canvas.height(), cursor_color);
  }
  d.endWrite();
  loop_cursor_prev_x = play_x;
}

static void loop_length_label_text(char* label, size_t size)
{
  if (background_loop.isValid()) {
    snprintf(label, size, "BGM %.1fs", (double)background_loop_length_ms() / 1000.0);
  } else if (loop_length_fixed && loop_length_msec != 0) {
    snprintf(label, size, "%.1fs", (double)loop_length_msec / 1000.0);
  } else {
    snprintf(label, size, "No Loop Data");
  }
}

static void service_loop_length_label_overlay(void)
{
  if ((!loop_length_label_overlay_pending && !loop_length_label_restore_pending)
   || ui_surface_exclusive || page_selector_visible || wave_transfer_active
   || wave_transfer_job_pending || ui_async_display_busy()) {
    return;
  }
  const int w = wave_canvas.width();
  const int x = std::max(2, w - loop_length_label_overlay_w);
  const int y = 2;
  const int width = std::max(1, w - 4 - x);
  if (loop_length_label_restore_pending) {
    // The label was drawn directly over this untouched canvas region while
    // stopped.  Restoring this tiny clipped area exposes the cached notes
    // immediately when the transport begins.
    M5.Display.setClipRect(x, wave_y + y, width, loop_length_label_overlay_h);
    wave_canvas.pushSprite(0, wave_y);
    M5.Display.clearClipRect();
    loop_length_label_restore_pending = false;
    loop_length_label_overlay_visible = false;
    return;
  }
  if (loop_playing || !loop_timeline_cache_valid) { return; }
  auto& d = M5.Display;
  d.fillRect(x, wave_y + y, width, loop_length_label_overlay_h, 0x080810u);
  char label[40];
  loop_length_label_text(label, sizeof(label));
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::top_right);
  d.setTextColor(0xB0E0FFu, 0x080810u);
  d.drawString(label, w - 4, wave_y + y);
  loop_length_label_overlay_pending = false;
  loop_length_label_overlay_visible = true;
}

static void draw_loop_timeline(bool cursor_only = false)
{
  if (ui_surface_exclusive || page_selector_visible) { return; }
  auto& c = wave_canvas;
  const int w = c.width();
  const int h = c.height();
  uint32_t length_ms = loop_display_length_ms(M5.millis());
  if (cursor_only && loop_timeline_cache_valid && !loop_recording_notice_active()) {
    draw_loop_cursor_only(length_ms);
    return;
  }
  c.fillScreen(0x080810u);
  if (loop_recording_notice_active()) {
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(2);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextColor(0xFF7070u, 0x080810u);
    c.drawString("RECORDING", w / 2, h / 2);
    draw_fn_information_chip(c);
    loop_length_label_overlay_pending = false;
    loop_length_label_overlay_visible = false;
    loop_length_label_restore_pending = false;
    loop_recording_notice_shown = true;
    queue_wave_canvas_full_transfer();
    return;
  }
  const int plot_left = loop_timeline_inset_x;
  const int plot_right = w - 1 - loop_timeline_inset_x;
  const int plot_w = std::max<int>(1, plot_right - plot_left + 1);
  for (int step = 0; step <= 16; ++step) {
    int x = plot_left + (step * (plot_right - plot_left)) / 16;
    bool major = (step % 4) == 0;
    c.drawFastVLine(x, 0, h, step == 0 ? 0x5070B0u : (major ? 0x304870u : 0x1A2438u));
  }
  int lane_h = std::max<int>(1, (h - 16) / def::pad::pad_count);
  int mark_h = std::min<int>(6, std::max<int>(3, lane_h - 1));
  // P1-4 / P5-8 / P9-12 の三段レイアウトに対応する区切り線。
  // ループ譜面の静的な背景にだけ描くため、カーソル更新には影響しない。
  const int lane_top = 8;
  for (int bank = 1; bank < 3; ++bank) {
    c.drawFastHLine(plot_left, lane_top + bank * 4 * lane_h, plot_w, 0x365272u);
  }
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    if (loop_is_muted(current_page, (uint8_t)pad)) {
      int y = loop_timeline_lane_y((uint8_t)pad, lane_h) + mark_h / 2;
      c.drawFastHLine(plot_left, y, plot_w, 0x303038u);
    }
  }
  for (const auto& e : loop_events) {
    if (e.page != current_page) { continue; }
    draw_loop_timeline_event_to_canvas(e, length_ms);
  }
  // Keep stopped-state length text out of the cached piano roll.  It is drawn
  // as a tiny overlay after this base image has reached the LCD.
  loop_length_label_overlay_pending = !loop_playing;
  loop_length_label_overlay_visible = false;
  loop_length_label_restore_pending = false;
  draw_fn_information_chip(c);
  loop_timeline_cache_valid = true;
  loop_cursor_prev_x = -1;
  loop_timeline_dirty_span_count = 0;
  queue_wave_canvas_full_transfer();
}

static void draw_fx_panel(void)
{
  auto& c = wave_canvas;
  const int w = c.width();
  const int h = c.height();
  if (mixer_active) {
    c.fillScreen(0x0A0C10u);
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(2);
    c.setTextDatum(m5gfx::textdatum_t::top_center);
    c.setTextColor(0xE8E8F0u, 0x0A0C10u);
    c.drawString("MIXER", w / 2, 3);
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    for (uint8_t position = 0; position < mixer_part_count; ++position) {
      const uint8_t part = (uint8_t)mixer_panel_parts[position];
      const int row = position / 3;
      const int col = position % 3;
      const int x = 5 + col * (w / 3);
      const int y = 30 + row * 35;
      uint32_t color = mixer_part_muted[part] ? 0x606068u : mixer_part_colors[part];
      c.setTextColor(color, 0x0A0C10u);
      c.drawString(mixer_part_labels[part], x, y);
      c.drawRect(x, y + 18, w / 3 - 12, 8, 0x505058u);
      if (!mixer_part_muted[part]) {
        c.fillRect(x + 1, y + 19,
          ((w / 3 - 14) * mixer_part_volume[part]) / 100, 6, color);
      }
    }
    if (mixer_notice[0] && (int32_t)(mixer_notice_until_msec - M5.millis()) > 0) {
      c.setTextDatum(m5gfx::textdatum_t::bottom_center);
      c.setTextColor(0xFFFFFFu, 0x0A0C10u);
      c.drawString(mixer_notice, w / 2, h - 2);
    }
    push_wave_canvas();
    return;
  }
  if (fx_pad_active >= 0 && pad_display_number((uint8_t)fx_pad_active) == 7) {
    c.fillScreen(0x140A1Au);
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(2);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextColor(fx_tape_stop_color, 0x140A1Au);
    c.drawString("TAPE STOP", w / 2, h / 2 - 10);
    c.setTextSize(1);
    c.setTextColor(0xD0C0E0u, 0x140A1Au);
    c.drawString("RELEASE TO RETURN", w / 2, h / 2 + 20);
    push_wave_canvas();
    return;
  }
  c.fillScreen(0x100818u);
  static constexpr const char* labels[] = { "TEMPO", "FILTER", "REPEAT", "DELAY" };
  for (int i = 0; i < fx_param_count; ++i) {
    int row_h = h / fx_param_count;
    int y = i * row_h + 3;
    uint32_t color = fx_control_colors[i];
    bool held = fx_pad_active >= 0
      && ((i == 2 && pad_display_number((uint8_t)fx_pad_active) <= 4)
       || (i == 1 && pad_display_number((uint8_t)fx_pad_active) == 5)
       || (i == 0 && pad_display_number((uint8_t)fx_pad_active) == 6)
       || (i == 3 && pad_display_number((uint8_t)fx_pad_active) == 8));
    if (!held) { color = 0x606060u; }
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(i == fx_selected ? 0xFFFFFFu : 0xB0B0C0u, 0x100818u);
    c.drawString(labels[i], 4, y);
    int bx = 58;
    int bw = w - bx - 42;
    int by = y + 4;
    int bh = std::max(18, row_h - 5);
    c.drawRect(bx, by, bw, bh, i == fx_selected ? 0xFFFFFFu : 0x505060u);
    if (i < 2) {
      int cx = bx + bw / 2;
      int fill = ((bw / 2 - 1) * (int)fx_param[i]) / 50;
      c.drawFastVLine(cx, by - 1, bh + 2, 0x707080u);
      if (fill >= 0) {
        c.fillRect(cx, by + 4, fill, bh - 7, color);
      } else {
        c.fillRect(cx + fill, by + 4, -fill, bh - 7, color);
      }
    } else if (i == fx_repeat_index) {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0]) - 1));
      c.fillRect(bx + 1, by + 4, ((bw - 2) * (index + 1)) / (sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0])), bh - 7, color);
    } else {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)delay_grid_option_count - 1u);
      c.fillRect(bx + 1, by + 4, ((bw - 2) * (index + 1)) / delay_grid_option_count, bh - 7, color);
    }
    char value[12];
    if (i == fx_repeat_index) {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)(sizeof(loop_repeat_labels) / sizeof(loop_repeat_labels[0]) - 1));
      snprintf(value, sizeof(value), "%s", loop_repeat_labels[index]);
    } else if (i == fx_delay_index) {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)delay_grid_option_count - 1u);
      snprintf(value, sizeof(value), "%s", delay_grid_labels[index]);
    } else {
      snprintf(value, sizeof(value), "%+d", (int)fx_param[i]);
    }
    c.setTextColor(color, 0x100818u);
    c.setTextDatum(m5gfx::textdatum_t::top_right);
    c.drawString(value, w - 4, y);
  }
  push_wave_canvas();
}

static void draw_sample_points(M5Canvas& c, const sample_slot_t& slot, bool show_active_param)
{
  if (slot.frames == 0) { return; }
  const int w = c.width();
  const int h = c.height();
  int sx = 0;
  int ex = 0;
  if (slot.reverse) {
    sx = ((uint64_t)(slot.frames - slot.playStart()) * w) / slot.frames;
    ex = ((uint64_t)(slot.frames - slot.playEnd()) * w) / slot.frames;
  } else {
    sx = ((uint64_t)slot.playStart() * w) / slot.frames;
    ex = ((uint64_t)slot.playEnd() * w) / slot.frames;
  }
  if (sx < 0) { sx = 0; }
  if (sx >= w) { sx = w - 1; }
  if (ex < 0) { ex = 0; }
  if (ex >= w) { ex = w - 1; }
  int left = std::min(sx, ex);
  int right = std::max(sx, ex);
  c.fillRect(0, 0, left, h, 0x181820u);
  c.fillRect(right + 1, 0, w - right - 1, h, 0x181820u);
  uint32_t start_color = (show_active_param && edit_param == 0) ? 0xFF7050u : 0xB05040u;
  uint32_t end_color = (show_active_param && edit_param == 1) ? 0x50A0FFu : 0x4070B0u;
  c.drawFastVLine(sx, 0, h, start_color);
  c.drawFastVLine(sx + 1 < w ? sx + 1 : sx, 0, h, start_color);
  c.drawFastVLine(ex, 0, h, end_color);
  c.drawFastVLine(ex > 0 ? ex - 1 : ex, 0, h, end_color);
  c.fillTriangle(sx, 0, sx + 5, 0, sx, 5, start_color);
  c.fillTriangle(ex, h - 1, ex - 5, h - 1, ex, h - 6, end_color);
}

static void draw_sample_sustain_points(M5Canvas& c, const sample_slot_t& slot)
{
  if (slot.frames == 0 || slot.synth_sustain_mode == sample_sustain_mode_t::off
   || (slot.synth_sustain_mode == sample_sustain_mode_t::automatic
    && !slot.synth_sustain_auto)
   || slot.synth_loop_end <= slot.synth_loop_start) {
    return;
  }
  const int w = c.width();
  const int h = c.height();
  int in_x = (int)(((uint64_t)slot.synth_loop_start * w) / slot.frames);
  int out_x = (int)(((uint64_t)slot.synth_loop_end * w) / slot.frames);
  in_x = std::clamp(in_x, 0, w - 1);
  out_x = std::clamp(out_x, 0, w - 1);
  const int left = std::min(in_x, out_x);
  const int right = std::max(in_x, out_x);
  const uint32_t loop_color = 0x50D8D0u;
  // A sparse tint identifies the sustain region without covering the waveform.
  for (int x = left; x <= right; x += 4) {
    c.drawFastVLine(x, 2, h - 4, 0x183838u);
  }
  c.drawFastVLine(in_x, 0, h, edit_param == 7 ? 0xFFFFFFu : loop_color);
  c.drawFastVLine(out_x, 0, h, edit_param == 8 ? 0xFFFFFFu : loop_color);
}

static inline uint32_t display_wave_sample_index(const sample_slot_t& slot, uint32_t index)
{
  if (index >= slot.frames) { index = slot.frames - 1; }
  return slot.reverse ? (slot.frames - 1 - index) : index;
}

// The full PCM buffer can be several hundred thousand frames long.  Edit and
// Sample views redraw often while a knob is moving, so use the envelope built
// when the sample was loaded instead of scanning every PCM frame again.
static void sample_wave_display_envelope(const sample_slot_t& slot,
                                         uint32_t display_start, uint32_t display_end,
                                         int16_t* min_value, int16_t* max_value)
{
  if (min_value == nullptr || max_value == nullptr || slot.frames == 0) { return; }
  display_start = std::min<uint32_t>(display_start, slot.frames - 1);
  display_end = std::clamp<uint32_t>(display_end, display_start + 1, slot.frames);
  uint32_t source_start = display_start;
  uint32_t source_end = display_end;
  if (slot.reverse) {
    source_start = slot.frames - display_end;
    source_end = slot.frames - display_start;
  }
  uint32_t first_bin = ((uint64_t)source_start * sample_slot_t::waveform_bins) / slot.frames;
  uint32_t last_bin = ((uint64_t)(source_end - 1) * sample_slot_t::waveform_bins) / slot.frames;
  first_bin = std::min<uint32_t>(first_bin, sample_slot_t::waveform_bins - 1);
  last_bin = std::min<uint32_t>(last_bin, sample_slot_t::waveform_bins - 1);
  int16_t mn = INT16_MAX;
  int16_t mx = INT16_MIN;
  for (uint32_t bin = first_bin; bin <= last_bin; ++bin) {
    const int16_t bin_min = apply_sample_wave_display_gain(slot.waveform_min[bin], slot.volume_q8);
    const int16_t bin_max = apply_sample_wave_display_gain(slot.waveform_max[bin], slot.volume_q8);
    if (bin_min < mn) { mn = bin_min; }
    if (bin_max > mx) { mx = bin_max; }
  }
  *min_value = mn;
  *max_value = mx;
}

static void draw_sample_recording_panel(M5Canvas& c)
{
  const int w = c.width();
  const int h = c.height();
  c.fillScreen(0x2A0608u);
  c.fillRect(0, 0, w, h, 0x3A080Au);
  for (int y = 0; y < h; y += 8) {
    uint32_t color = (y / 8) & 1 ? 0x3A080Au : 0x440C10u;
    c.drawFastHLine(0, y, w, color);
  }

  int cx = w / 2;
  int cy = h / 2 - 6;
  uint32_t mic = 0xFFE0E0u;
  uint32_t accent = 0xFF5050u;
  c.fillRoundRect(cx - 16, cy - 36, 32, 50, 15, mic);
  c.fillRoundRect(cx - 10, cy - 30, 20, 38, 10, 0x5A1014u);
  c.drawRoundRect(cx - 24, cy - 22, 48, 48, 22, mic);
  c.fillRect(cx - 3, cy + 24, 6, 18, mic);
  c.fillRoundRect(cx - 20, cy + 39, 40, 5, 2, mic);
  c.fillCircle(cx + 33, cy - 26, 4 + ((M5.millis() / 180) & 1), accent);

  c.setFont(&fonts::efontJA_16_b);
  c.setTextDatum(m5gfx::textdatum_t::top_left);
  c.setTextSize(1);
  c.setTextColor(0xFFD0D0u, 0x3A080Au);
  char pad_label[16];
  snprintf(pad_label, sizeof(pad_label), "P%d", pad_display_number((uint8_t)recording_pad));
  c.drawString(pad_label, 8, 6);

  c.setTextDatum(m5gfx::textdatum_t::middle_center);
  c.setTextSize(1, 2);
  c.setTextColor(0xFFFFFFu, 0x3A080Au);
  c.drawString("SAMPLING", cx, h - 30);
  c.setTextSize(1);
  c.setTextColor(0xFFB0B0u, 0x3A080Au);
  c.drawString(recording_source == recording_source_t::external_input ? "LINE INPUT" : "MIC INPUT", cx, h - 10);
}

static void draw_sample_move_panel(M5Canvas& c)
{
  const int w = c.width();
  const int h = c.height();
  c.fillScreen(0x100818u);
  c.fillRect(0, 0, w, h, 0x1A1020u);
  const bool copy_ready = sample_move_copy_source_pad >= 0;
  int src = copy_ready ? sample_move_copy_source_pad : sample_move_source_pad;
  uint32_t color = 0xFFB050u;
  c.setFont(&fonts::efontJA_16_b);
  c.setTextDatum(m5gfx::textdatum_t::middle_center);
  c.setTextSize(1, 2);
  c.setTextColor(0xFFFFFFu, 0x1A1020u);
  c.drawString(copy_ready ? "MOVED" : "MOVE / MIX", w / 2, 24);
  c.setTextSize(1);
  c.setTextColor(0xFFD090u, 0x1A1020u);
  char line[40];
  snprintf(line, sizeof(line), "SOURCE P%d", src >= 0 ? pad_display_number((uint8_t)src) : 0);
  c.drawString(line, w / 2, 52);

  int ax = w / 2;
  int ay = 76;
  c.drawFastHLine(ax - 42, ay, 84, color);
  c.fillTriangle(ax + 42, ay, ax + 30, ay - 6, ax + 30, ay + 6, color);
  c.drawCircle(ax - 54, ay, 10, color);
  c.fillCircle(ax + 54, ay, 10, color);

  c.setTextColor(0xC8C8D8u, 0x1A1020u);
  if (copy_ready) {
    snprintf(line, sizeof(line), "TAP P%d AGAIN: COPY",
             pad_display_number((uint8_t)sample_move_copy_target_pad));
    c.drawString(line, w / 2, h - 22);
    c.drawString("RELEASE SOURCE: DONE", w / 2, h - 6);
  } else {
    c.drawString("EMPTY PAD: MOVE", w / 2, h - 30);
    c.drawString("FILLED PAD: MIX", w / 2, h - 12);
  }
}

static const char* fn_information_text(void)
{
  if (edit_pad >= 0) { return nullptr; }
  if (sample_delete_confirm_pad >= 0) { return "DELETE_CONFIRM"; }
  int fn = -1;
  for (int i = 0; i < 3; ++i) {
    if (fn_pressed[i]) { fn = i; break; }
  }
  if (fn < 0) { return nullptr; }
  if (current_mode == sampler_mode_t::mode_rec
   || current_mode == sampler_mode_t::mode_play
   || current_mode == sampler_mode_t::mode_fx) {
    if (fn == 0) {
      if (!loop_playing && performance_record_armed) { return "RECORD MODE"; }
      return loop_playing ? "STOP LOOP" : "PLAY LOOP";
    }
    if (fn == 1) {
      return current_page == performance_page_t::melody
          || current_page == performance_page_t::bass
          || current_page == performance_page_t::chord ? "MUTE PAGE" : "MUTE PAD";
    }
    if (fn == 2 && current_mode == sampler_mode_t::mode_rec
     && current_page == performance_page_t::sample) {
      return "DELETE SAMPLE";
    }
    if (fn == 2 && current_mode == sampler_mode_t::mode_play
     && (current_page == performance_page_t::melody || current_page == performance_page_t::bass)) {
      return "TOUCH PLAY";
    }
    return nullptr;
  }
  if (current_mode == sampler_mode_t::mode_loop) {
    if (fn == 0) {
      if (!loop_length_fixed) { return "SET LOOP END"; }
      return loop_playing ? "STOP LOOP" : "PLAY LOOP";
    }
    if (fn == 1) { return "MUTE PAD"; }
    return loop_playing ? "UNDO / DELETE PAD" : "DELETE LOOP";
  }
  return nullptr;
}

static bool sample_edit_armed_active(uint32_t now)
{
  (void)now;
  return sample_edit_armed_pad >= 0;
}

static bool play_loop_grid_information_active(void)
{
  return false;
}

static uint8_t hold_progress_percent(uint32_t now)
{
  if (hold_progress_kind == hold_progress_kind_t::none || hold_progress_duration_msec == 0) {
    return 0;
  }
  const uint32_t elapsed = now - hold_progress_start_msec;
  return (uint8_t)std::min<uint32_t>(100, (elapsed * 100u) / hold_progress_duration_msec);
}

static void begin_hold_progress(hold_progress_kind_t kind, uint32_t start_msec,
                                uint32_t duration_msec, uint32_t color,
                                const char* wait_text, const char* ready_text)
{
  hold_progress_kind = kind;
  hold_progress_start_msec = start_msec;
  hold_progress_duration_msec = duration_msec;
  hold_progress_color = color;
  snprintf(hold_progress_wait_text, sizeof(hold_progress_wait_text), "%s", wait_text ? wait_text : "HOLD");
  snprintf(hold_progress_ready_text, sizeof(hold_progress_ready_text), "%s", ready_text ? ready_text : "RELEASE");
  hold_progress_draw_msec = 0;
  hold_progress_drawn_percent = 255;
  hold_progress_static_drawn = false;
  hold_progress_needs_redraw = false;
  // Hold feedback is a direct LCD overlay. A preview cursor is already
  // paused while it is visible, but a wave strip may have been queued on
  // Core 0 just before the hold threshold. Invalidate and drain that one
  // pending transfer for every hold kind, otherwise it can repaint across
  // the popup and make the Sample waveform appear to flicker.
  dirty_wave = false;
  wave_transfer_generation = wave_transfer_generation + 1;
  wave_transfer_active = false;
  wave_transfer_full_frame = false;
  wait_wave_transfer_job();
  wait_ui_dirty_transfers();
}

static void cancel_hold_progress(hold_progress_kind_t kind)
{
  if (hold_progress_kind == hold_progress_kind_t::none
   || (kind != hold_progress_kind_t::none && hold_progress_kind != kind)) {
    return;
  }
  hold_progress_kind = hold_progress_kind_t::none;
  hold_progress_drawn_percent = 255;
  hold_progress_static_drawn = false;
  hold_progress_needs_redraw = false;
  // The popup is drawn directly over the Wave Canvas. Restore its area only
  // once after release instead of repainting it throughout the hold gesture.
  request_wave_draw();
}

static void hold_progress_bounds(int* x, int* y, int* w, int* h)
{
  const int chip_w = 184;
  const int chip_h = 36;
  if (x) { *x = (wave_canvas.width() - chip_w) / 2; }
  if (y) { *y = (wave_canvas.height() - chip_h) / 2; }
  if (w) { *w = chip_w; }
  if (h) { *h = chip_h; }
}

static uint32_t hold_progress_fill_color(uint32_t color, bool ready)
{
  const uint8_t divisor = 5;
  const uint8_t numerator = ready ? 5 : 4;
  const uint8_t r = (uint8_t)((((color >> 16) & 0xFFu) * numerator) / divisor);
  const uint8_t g = (uint8_t)((((color >> 8) & 0xFFu) * numerator) / divisor);
  const uint8_t b = (uint8_t)(((color & 0xFFu) * numerator) / divisor);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void draw_hold_progress_chip(M5Canvas& c, uint32_t now)
{
  if (hold_progress_kind == hold_progress_kind_t::none) { return; }
  int chip_x = 0;
  int chip_y = 0;
  int chip_w = 0;
  int chip_h = 0;
  hold_progress_bounds(&chip_x, &chip_y, &chip_w, &chip_h);
  const uint8_t percent = hold_progress_percent(now);
  const bool ready = percent >= 100;
  const uint32_t frame = ready ? 0xFFFFFFu : hold_progress_color;
  const uint32_t fill = hold_progress_fill_color(hold_progress_color, ready);
  c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x08080Cu);
  c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x383848u);
  c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, frame);
  const int bar_x = chip_x + 5;
  const int bar_y = chip_y + chip_h - 9;
  const int bar_w = chip_w - 10;
  c.fillRect(bar_x, bar_y, bar_w, 5, 0x202030u);
  c.fillRect(bar_x, bar_y, (bar_w * percent) / 100, 5, fill);
  c.setFont(&fonts::efontJA_16_b);
  c.setTextSize(1);
  c.setTextDatum(m5gfx::textdatum_t::middle_center);
  c.setTextColor(ready ? 0xFFFFFFu : 0xE8E8F0u, 0x08080Cu);
  c.drawString(ready ? hold_progress_ready_text : hold_progress_wait_text,
               chip_x + chip_w / 2, chip_y + 14);
}

static void service_hold_progress(uint32_t now)
{
  if (hold_progress_kind == hold_progress_kind_t::none || ui_surface_exclusive) { return; }
  const uint8_t percent = hold_progress_percent(now);
  if (!hold_progress_needs_redraw && now - hold_progress_draw_msec < 33) { return; }
  if (!hold_progress_needs_redraw && hold_progress_static_drawn
   && hold_progress_drawn_percent == percent) { return; }
  hold_progress_draw_msec = now;
  // Draw directly to the display: this keeps a held button responsive even
  // when the background timeline is intentionally throttled.
  auto& d = M5.Display;
  d.startWrite();
  int chip_x = 0;
  int chip_y = 0;
  int chip_w = 0;
  int chip_h = 0;
  hold_progress_bounds(&chip_x, &chip_y, &chip_w, &chip_h);
  const bool ready = percent >= 100;
  const uint32_t frame = ready ? 0xFFFFFFu : hold_progress_color;
  const uint32_t fill = hold_progress_fill_color(hold_progress_color, ready);
  const int screen_y = wave_y + chip_y;
  const int bar_x = chip_x + 5;
  const int bar_y = screen_y + chip_h - 9;
  const int bar_w = chip_w - 10;
  const bool became_ready = hold_progress_static_drawn
                         && hold_progress_drawn_percent < 100 && ready;
  if (!hold_progress_static_drawn || hold_progress_needs_redraw) {
    d.fillRoundRect(chip_x, screen_y, chip_w, chip_h, 5, 0x08080Cu);
    d.drawRoundRect(chip_x, screen_y, chip_w, chip_h, 5, 0x383848u);
    d.drawRoundRect(chip_x + 1, screen_y + 1, chip_w - 2, chip_h - 2, 4, frame);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(ready ? 0xFFFFFFu : 0xE8E8F0u, 0x08080Cu);
    d.drawString(ready ? hold_progress_ready_text : hold_progress_wait_text,
                 chip_x + chip_w / 2, screen_y + 14);
    hold_progress_static_drawn = true;
  } else if (became_ready) {
    // Completion is the only moment the frame/text changes. The in-between
    // animation is just the three-pixel progress bar below it.
    d.fillRect(chip_x + 4, screen_y + 4, chip_w - 8, chip_h - 13, 0x08080Cu);
    d.drawRoundRect(chip_x + 1, screen_y + 1, chip_w - 2, chip_h - 2, 4, frame);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(0xFFFFFFu, 0x08080Cu);
    d.drawString(hold_progress_ready_text, chip_x + chip_w / 2, screen_y + 14);
  }
  d.fillRect(bar_x, bar_y, bar_w, 5, 0x202030u);
  d.fillRect(bar_x, bar_y, (bar_w * percent) / 100, 5, fill);
  hold_progress_drawn_percent = percent;
  hold_progress_needs_redraw = false;
  d.endWrite();
}

static bool fn_information_chip_bounds(int* x, int* y, int* w, int* h)
{
  if (hold_progress_kind != hold_progress_kind_t::none) {
    hold_progress_bounds(x, y, w, h);
    return true;
  }
  const char* text = fn_information_text();
  const bool sample_edit_armed = sample_edit_armed_active(M5.millis());
  if (!text && !sample_edit_armed) { return false; }
  const bool delete_confirm = sample_delete_confirm_pad >= 0;
  bool grid = play_loop_grid_information_active();
  int chip_w = sample_edit_armed || delete_confirm ? 184
              : grid ? 96 : std::min<int>(176, std::max<int>(88, (int)strlen(text) * 9 + 20));
  int chip_h = sample_edit_armed || delete_confirm ? 46 : grid ? 42 : 30;
  if (x) { *x = (wave_canvas.width() - chip_w) / 2; }
  if (y) { *y = (wave_canvas.height() - chip_h) / 2; }
  if (w) { *w = chip_w; }
  if (h) { *h = chip_h; }
  return true;
}

static bool draw_fn_information_chip(M5Canvas& c)
{
  const char* text = fn_information_text();
  const bool sample_edit_armed = sample_edit_armed_active(M5.millis());
  int chip_x = 0;
  int chip_y = 0;
  int chip_w = 0;
  int chip_h = 0;
  if ((hold_progress_kind == hold_progress_kind_t::none && !text && !sample_edit_armed)
   || !fn_information_chip_bounds(&chip_x, &chip_y, &chip_w, &chip_h)) { return false; }

  if (hold_progress_kind != hold_progress_kind_t::none) {
    draw_hold_progress_chip(c, M5.millis());
    return true;
  }

  uint32_t accent = mode_info[(int)current_mode].screen_color;
  c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x040408u);
  c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x303038u);
  c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, accent);
  c.setFont(&fonts::efontJA_16_b);
  c.setTextSize(1);
  c.setTextDatum(m5gfx::textdatum_t::middle_center);

  if (play_loop_grid_information_active()) {
    int pad = sample_loop_target_pad();
    const auto& slot = sampler_pool_t::slot[pad];
    uint8_t index = sample_loop_grid_index(slot.loop_grid_half_steps);
    char title[24];
    snprintf(title, sizeof(title), "P%u LOOP GRID", (unsigned)pad_display_number((uint8_t)pad));
    c.setTextColor(0xFFFFFFu);
    c.drawString(title, chip_x + chip_w / 2, chip_y + 12);
    c.setTextColor(0x80FF80u);
    c.drawString(sample_repeat_grid_labels[index], chip_x + chip_w / 2, chip_y + 29);
  } else if (sample_edit_armed) {
    c.setTextColor(0xD8E8FFu);
    c.drawString("TAP AGAIN: EDIT", chip_x + chip_w / 2, chip_y + 13);
    c.setTextColor(0xFFFFFFu);
    c.drawString("HOLD: MOVE", chip_x + chip_w / 2, chip_y + 31);
  } else if (sample_delete_confirm_pad >= 0) {
    c.setTextColor(0xFFD0D0u);
    c.drawString("DELETE?", chip_x + chip_w / 2, chip_y + 13);
    c.setTextColor(0xFFFFFFu);
    c.drawString("TAP AGAIN TO DELETE", chip_x + chip_w / 2, chip_y + 31);
  } else {
    c.setTextColor(0xFFFFFFu);
    c.drawString(text, chip_x + chip_w / 2, chip_y + chip_h / 2);
  }
  return true;
}

static bool draw_fn_information_panel(void)
{
  const char* text = fn_information_text();
  // 削除確認は通常のFn説明画面へ遷移せず、選択Padの波形を残したまま
  // 小さな二段ポップアップだけを重ねる。
  if (sample_delete_confirm_pad >= 0 || sample_edit_armed_active(M5.millis())) {
    if (fn_information_panel_visible) {
      fn_information_panel_visible = false;
      reset_live_wave();
    }
    return false;
  }
  if (!text) {
    if (fn_information_panel_visible) {
      fn_information_panel_visible = false;
      // ライブ波形は通常は変化した帯だけを送るため、説明パネルを覆った
      // 領域が残らないよう、次の一枚だけ全面更新に戻す。
      reset_live_wave();
    }
    return false;
  }
  if (!fn_information_panel_visible) {
    fn_information_panel_visible = true;
    reset_live_wave();
  }
  auto& c = wave_canvas;
  if (play_loop_grid_information_active()) {
    draw_live_wave();
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    draw_fn_information_chip(c);
    push_wave_canvas();
    return true;
  }
  const int w = c.width();
  const int h = c.height();
  uint32_t color = mode_info[(int)current_mode].screen_color;
  c.fillScreen(0x080810u);
  c.drawRect(0, 0, w, h, color);
  c.drawRect(1, 1, w - 2, h - 2, color);
  c.setFont(&fonts::efontJA_16_b);
  c.setTextSize(1);
  c.setTextDatum(m5gfx::textdatum_t::middle_center);
  c.setTextColor(0xFFFFFFu, 0x080810u);
  c.drawString(text, w / 2, h / 2);
  push_wave_canvas();
  return true;
}

static void draw_wave(void) {
  if (ui_surface_exclusive || page_selector_visible) { return; }
  // Core 0 may still be reading one 8px strip from this Canvas. Complete that
  // tiny transfer before the producer rewrites the retained pixels.
  wait_wave_transfer_job();
  rendered_wave_page = current_page;
  rendered_wave_mode = current_mode;
  bool timeline_visible = current_mode == sampler_mode_t::mode_loop
                       || (current_mode == sampler_mode_t::mode_play && loop_playing);
  if (timeline_visible) { fn_information_panel_visible = false; }
  if (!timeline_visible && draw_fn_information_panel()) { return; }
  if (uses_incremental_wave_transfer()) {
    draw_live_wave();
    return;
  }
  // Loop/REC/FXの静的表示を描く間は、ライブ波形の残像を無効化する。
  // Playへ戻った最初の一枚だけ全面転送し、以後は帯域更新に戻る。
  reset_live_wave();
  auto& c = wave_canvas;
  c.fillScreen(0x080810u);
  const int w = c.width();
  const int h = c.height();
  if (edit_pad >= 0 && edit_pad < (int)def::pad::pad_count && sampler_pool_t::slot[edit_pad].isValid()) {
    auto& slot = sampler_pool_t::slot[edit_pad];
    for (int x = 0; x < w; ++x) {
      uint32_t a = ((uint64_t)x * slot.frames) / w;
      uint32_t b = ((uint64_t)(x + 1) * slot.frames) / w;
      if (b <= a) { b = a + 1; }
      if (b > slot.frames) { b = slot.frames; }
      int16_t mn = INT16_MAX;
      int16_t mx = INT16_MIN;
      sample_wave_display_envelope(slot, a, b, &mn, &mx);
      int y0 = (h / 2) - ((int)mx * (h / 2 - 3)) / 32768;
      int y1 = (h / 2) - ((int)mn * (h / 2 - 3)) / 32768;
      if (y1 <= y0) { y1 = y0 + 1; }
      c.drawFastVLine(x, y0, y1 - y0, 0xD0B050u);
    }
    draw_sample_points(c, slot, true);
    draw_sample_sustain_points(c, slot);
    if (edit_chop_page) {
      const bool needs_bgm = edit_notice == edit_notice_t::chop_needs_bgm
        && (int32_t)(edit_notice_until_msec - M5.millis()) > 0;
      const char* mode = edit_chop_fit_mode == chop_fit_mode_t::fit_bgm ? "FIT TO BGM" : "KEEP SPEED";
      const char* count = edit_chop_count_mode == chop_count_mode_t::four ? "4"
                        : edit_chop_count_mode == chop_count_mode_t::eight ? "8"
                        : edit_chop_count_mode == chop_count_mode_t::twelve ? "12" : "AUTO";
      const int chip_w = 174;
      const int chip_h = 58;
      const int chip_x = (w - chip_w) / 2;
      const int chip_y = (h - chip_h) / 2;
      c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x040408u);
      c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x303038u);
      c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, 0xF0C050u);
      c.setFont(&fonts::efontJA_16_b);
      c.setTextSize(1);
      c.setTextDatum(m5gfx::textdatum_t::middle_center);
      if (needs_bgm) {
        c.setTextColor(0xFFFFFFu);
        c.drawString("NOTHING TO FIT", w / 2, chip_y + chip_h / 2);
      } else {
        c.setTextColor(0xF0C050u);
        char title[24] = "CHOP";
        if (edit_chop_preview_last >= 0 && edit_chop_preview_count) {
          snprintf(title, sizeof(title), "PREVIEW P%u/%u",
                   (unsigned)edit_chop_preview_last + 1,
                   (unsigned)edit_chop_preview_count);
        }
        c.drawString(title, w / 2, chip_y + 11);
        c.setTextColor(edit_chop_fit_mode == chop_fit_mode_t::fit_bgm ? 0x70D8FFu : 0xC0C8D8u);
        c.drawString(mode, w / 2, chip_y + 29);
        char count_text[24] = {};
        snprintf(count_text, sizeof(count_text), "%s SLICES", count);
        c.setTextColor(0xFFFFFFu);
        c.drawString(count_text, w / 2, chip_y + 47);
      }
      sample_preview_cursor_prev_x = -1;
      push_wave_canvas();
      return;
    }
    uint32_t accent = edit_param == 0 ? 0xFF7050u : edit_param == 1 ? 0x50A0FFu
                    : edit_param == 2 ? 0x60E080u : edit_param == 3 ? 0xB080FFu
                    : edit_param == 4 ? 0xF0D060u : edit_param == 5 ? 0x50C8D8u
                    : edit_param == 6 ? 0xD080E0u : edit_param <= 8 ? 0x50D8D0u
                    : edit_param == 9 ? 0xF0A050u : 0x80E0B0u;
    char value[24];
    if (edit_param == 0) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playStart() / slot.sample_rate : 0.0f);
    } else if (edit_param == 1) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playEnd() / slot.sample_rate : 0.0f);
    } else if (edit_param == 2) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)((slot.volume_q8 * 100u) / 256u));
    } else if (edit_param == 3) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)((slot.pitch_q8 * 100u) / 256u));
    } else if (edit_param == 4) {
      snprintf(value, sizeof(value), "%s", sample_repeat_label(slot));
    } else if (edit_param == 5) {
      snprintf(value, sizeof(value), "%s", slot.hold_enabled ? "ON" : "OFF");
    } else if (edit_param == 6) {
      snprintf(value, sizeof(value), "%s", slot.reverse ? "ON" : "OFF");
    } else if (edit_param == 7 || edit_param == 8) {
      const uint32_t frame = edit_param == 7 ? slot.synth_loop_start : slot.synth_loop_end;
      snprintf(value, sizeof(value), "%.2fs",
               slot.sample_rate ? (float)frame / slot.sample_rate : 0.0f);
    } else if (edit_param == 9) {
      snprintf(value, sizeof(value), "%ums", (unsigned)slot.synth_release_ms);
    } else {
      const char* mode = slot.synth_sustain_mode == sample_sustain_mode_t::off ? "OFF"
        : slot.synth_sustain_mode == sample_sustain_mode_t::manual ? "ON"
        : slot.synth_sustain_auto ? "AUTO" : "NO LOOP";
      snprintf(value, sizeof(value), "%s", mode);
    }
    const bool notice_visible = edit_notice != edit_notice_t::none
      && (int32_t)(edit_notice_until_msec - M5.millis()) > 0;
    if (notice_visible) {
      switch (edit_notice) {
      case edit_notice_t::confirm_melody:
      case edit_notice_t::assigned_melody:
      case edit_notice_t::confirm_unassign_melody:
      case edit_notice_t::unassigned_melody:
        accent = performance_page_colors[(uint8_t)performance_page_t::melody];
        break;
      case edit_notice_t::confirm_chord:
      case edit_notice_t::assigned_chord:
      case edit_notice_t::confirm_unassign_chord:
      case edit_notice_t::unassigned_chord:
        accent = performance_page_colors[(uint8_t)performance_page_t::chord];
        break;
      case edit_notice_t::confirm_bass:
      case edit_notice_t::assigned_bass:
      case edit_notice_t::confirm_unassign_bass:
      case edit_notice_t::unassigned_bass:
        accent = performance_page_colors[(uint8_t)performance_page_t::bass];
        break;
      case edit_notice_t::confirm_chop:
      case edit_notice_t::chopped:
        accent = 0xF0C050u;
        break;
      default:
        break;
      }
    }
    bool compact_chip = edit_value_compact_visible
      && (int32_t)(edit_value_activity_until - M5.millis()) > 0;
    if (!compact_chip) { edit_value_compact_visible = false; }
    if (notice_visible) { compact_chip = false; }
    const int chip_w = notice_visible ? 218
                     : compact_chip ? std::max<int>(46, (int)strlen(value) * 10 + 18)
                     : edit_param >= 4 ? std::max<int>(80, (int)strlen(value) * 10 + 18) : 80;
    const int chip_h = notice_visible ? 46 : compact_chip ? 24 : 42;
    const int chip_x = (w - chip_w) / 2;
    const int chip_y = (h - chip_h) / 2;
    c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x040408u);
    c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x303038u);
    c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, accent);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(1);
    if (notice_visible) {
      const char* line1 = "";
      const char* line2 = "";
      switch (edit_notice) {
      case edit_notice_t::hold:
        line1 = "Hold";
        line2 = slot.hold_enabled ? "ON" : "OFF";
        break;
      case edit_notice_t::reverse:
        line1 = "Reverse";
        line2 = slot.reverse ? "ON" : "OFF";
        break;
      case edit_notice_t::repeat:
        line1 = "Repeat";
        line2 = value;
        break;
      case edit_notice_t::confirm_melody:
        line1 = "PRESS AGAIN TO";
        line2 = "ASSIGN TO MELODY";
        break;
      case edit_notice_t::confirm_chord:
        line1 = "PRESS AGAIN TO";
        line2 = "ASSIGN TO CHORD";
        break;
      case edit_notice_t::confirm_bass:
        line1 = "PRESS AGAIN TO";
        line2 = "ASSIGN TO BASS";
        break;
      case edit_notice_t::confirm_unassign_melody:
        line1 = "MELODY ASSIGNED";
        line2 = "PRESS AGAIN TO REMOVE";
        break;
      case edit_notice_t::confirm_unassign_chord:
        line1 = "CHORD ASSIGNED";
        line2 = "PRESS AGAIN TO REMOVE";
        break;
      case edit_notice_t::confirm_unassign_bass:
        line1 = "BASS ASSIGNED";
        line2 = "PRESS AGAIN TO REMOVE";
        break;
      case edit_notice_t::confirm_delete:
        line1 = "PRESS AGAIN TO";
        line2 = "DELETE SAMPLE";
        break;
      case edit_notice_t::confirm_chop:
        line1 = "PRESS AGAIN TO";
        line2 = "CHOP INTO P1-P8";
        break;
      case edit_notice_t::chopped:
        line1 = "CHOPPED INTO";
        line2 = "P1 - P8";
        break;
      case edit_notice_t::assigned_melody:
        line1 = "MELODY";
        line2 = "ASSIGNED";
        break;
      case edit_notice_t::assigned_chord:
        line1 = "CHORD";
        line2 = "ASSIGNED";
        break;
      case edit_notice_t::assigned_bass:
        line1 = "BASS";
        line2 = "ASSIGNED";
        break;
      case edit_notice_t::unassigned_melody:
        line1 = "MELODY";
        line2 = "REMOVED";
        break;
      case edit_notice_t::unassigned_chord:
        line1 = "CHORD";
        line2 = "REMOVED";
        break;
      case edit_notice_t::unassigned_bass:
        line1 = "BASS";
        line2 = "REMOVED";
        break;
      case edit_notice_t::stop_loop_to_edit_synth:
        line1 = "STOP LOOP TO EDIT";
        line2 = "SYNTH LOOP";
        break;
      case edit_notice_t::stop_loop_to_change_sound:
        line1 = "STOP LOOP TO CHANGE";
        line2 = "SOUND";
        break;
      case edit_notice_t::stop_loop_to_edit_sample:
        line1 = "STOP LOOP TO EDIT";
        line2 = "SAMPLE";
        break;
      case edit_notice_t::chop_needs_bgm:
        line1 = "NOTHING TO FIT";
        line2 = "";
        break;
      default:
        break;
      }
      c.setTextColor(0xFFFFFFu);
      c.drawString(line1, w / 2, chip_y + 13);
      c.setTextColor(accent);
      c.drawString(line2, w / 2, chip_y + 33);
    } else if (compact_chip) {
      c.setTextColor(accent);
      c.drawString(value, w / 2, chip_y + chip_h / 2);
    } else {
      c.setTextColor(0xFFFFFFu);
      c.drawString(edit_param_labels[std::min<uint8_t>(edit_param, std::size(edit_param_labels) - 1)], w / 2, chip_y + 12);
      c.setTextColor(accent);
      c.drawString(value, w / 2, chip_y + 29);
    }

    char info[48];
    snprintf(info, sizeof(info), "P%d %.2fs Vol %u Pit %u"
      , pad_display_number((uint8_t)edit_pad)
      , slot.sample_rate ? (float)slot.playFrames() / slot.sample_rate : 0.0f
      , (unsigned)((slot.volume_q8 * 100u) / 256u)
      , (unsigned)((slot.pitch_q8 * 100u) / 256u));
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(0xFFFFFFu, 0x080810u);
    c.drawString(info, 3, h - 16);
    sample_preview_cursor_prev_x = -1;
    push_wave_canvas();
    return;
  }
  if (current_page != performance_page_t::sample
   && current_mode != sampler_mode_t::mode_fx
   && !(current_mode == sampler_mode_t::mode_loop
     || (current_mode == sampler_mode_t::mode_play && loop_playing))) {
    const uint32_t background = performance_page_backgrounds[(uint8_t)current_page];
    const uint32_t accent = performance_page_colors[(uint8_t)current_page];
    c.fillScreen(background);
    const char* title = performance_page_names[(uint8_t)current_page];
    char lines[4][48] = {};
    uint8_t line_count = 0;
    const int content_x = 12;

    if (current_page == performance_page_t::melody) {
      snprintf(lines[line_count++], sizeof(lines[0]), "Key   : %s",
        key_names[pitched_page_key(current_page)]);
      snprintf(lines[line_count++], sizeof(lines[0]), "Scale : %s",
        sampler_scale_names[std::min<uint8_t>(harmony_scale, sampler_scale_count - 1)]);
      if (melody_settings.source == synth_tone_source_t::pad) {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : Pad %u",
          (unsigned)pad_display_number(melody_settings.pad));
      } else {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : %s",
          kp::def::midi::program_name_table.at(melody_settings.program)->get());
      }
      snprintf(lines[line_count++], sizeof(lines[0]), "Oct.  : %+d", melody_settings.octave);
    } else if (current_page == performance_page_t::bass) {
      snprintf(lines[line_count++], sizeof(lines[0]), "Key   : %s",
        key_names[harmony_key()]);
      snprintf(lines[line_count++], sizeof(lines[0]), "Scale : %s",
        sampler_scale_names[std::min<uint8_t>(harmony_scale, sampler_scale_count - 1)]);
      if (bass_settings.source == synth_tone_source_t::pad) {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : Pad %u",
          (unsigned)pad_display_number(bass_settings.pad));
      } else {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : %s",
          kp::def::midi::program_name_table.at(bass_settings.program)->get());
      }
      snprintf(lines[line_count++], sizeof(lines[0]), "Oct.  : %+d", bass_settings.octave);
    } else if (current_page == performance_page_t::chord) {
      snprintf(lines[line_count++], sizeof(lines[0]), "Key   : %s", key_names[harmony_key()]);
      snprintf(lines[line_count++], sizeof(lines[0]), "Scale : %s",
        sampler_scale_names[std::min<uint8_t>(harmony_scale, sampler_scale_count - 1)]);
      if (chord_settings.source == synth_tone_source_t::pad) {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : Pad %u",
          (unsigned)pad_display_number(chord_settings.pad));
      } else {
        snprintf(lines[line_count++], sizeof(lines[0]), "Sound : %s",
          kp::def::midi::program_name_table.at(chord_settings.program)->get());
      }
      snprintf(lines[line_count++], sizeof(lines[0]), "Oct.  : %+d", chord_settings.octave);
    } else {
      title = "DRUM KIT";
      snprintf(lines[line_count++], sizeof(lines[0]), "Volume : %u%%", (unsigned)drum_volume);
    }

    c.setFont(&fonts::efontJA_16_b);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextSize(2);
    c.setTextColor(accent, background);
    c.drawString(title, w / 2, 17);
    c.drawFastHLine(content_x, 36, w - content_x * 2, darken_rgb24(accent, 1));

    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(0xF0F0F8u, background);
    for (uint8_t i = 0; i < line_count; ++i) {
      c.drawString(lines[i], content_x, 41 + i * 18);
    }
    push_wave_canvas();
    return;
  }
  if (current_mode == sampler_mode_t::mode_rec) {
    if (recording_pad >= 0) {
      draw_sample_recording_panel(c);
      push_wave_canvas();
      return;
    }
    if (sample_move_source_pad >= 0 || sample_move_copy_source_pad >= 0) {
      draw_sample_move_panel(c);
      push_wave_canvas();
      return;
    }
    if (rec_wave_pad >= 0 && rec_wave_pad < (int)def::pad::pad_count
     && sampler_pool_t::slot[rec_wave_pad].isValid()) {
      auto& slot = sampler_pool_t::slot[rec_wave_pad];
      for (int x = 0; x < w; ++x) {
        uint32_t a = ((uint64_t)x * slot.frames) / w;
        uint32_t b = ((uint64_t)(x + 1) * slot.frames) / w;
        if (b <= a) { b = a + 1; }
        if (b > slot.frames) { b = slot.frames; }
        int16_t mn = INT16_MAX;
        int16_t mx = INT16_MIN;
        sample_wave_display_envelope(slot, a, b, &mn, &mx);
        int y0 = (h / 2) - ((int)mx * (h / 2 - 3)) / 32768;
        int y1 = (h / 2) - ((int)mn * (h / 2 - 3)) / 32768;
        if (y1 <= y0) { y1 = y0 + 1; }
        c.drawFastVLine(x, y0, y1 - y0, 0xD0B050u);
      }
      draw_sample_points(c, slot, false);
      c.setTextSize(1);
      c.setTextDatum(m5gfx::textdatum_t::top_left);
      c.setTextColor(0xFFFFFFu, 0x080810u);
      char info[48];
      snprintf(info, sizeof(info), "REC P%d %.2fs V%u P%u%s"
        , pad_display_number((uint8_t)rec_wave_pad)
        , slot.sample_rate ? (float)slot.playFrames() / slot.sample_rate : 0.0f
        , (unsigned)((slot.volume_q8 * 100u) / 256u)
        , (unsigned)((slot.pitch_q8 * 100u) / 256u)
        , slot.reverse ? " R" : "");
      c.drawString(info, 3, 3);
    } else {
      c.setFont(&fonts::efontJA_16_b);
      c.setTextSize(1);
      c.setTextDatum(m5gfx::textdatum_t::middle_center);
      c.setTextColor(0x808090u, 0x080810u);
      c.drawString(recording_pad >= 0 ? "REC" : "SELECT PAD", w / 2, h / 2);
    }
    sample_preview_cursor_prev_x = -1;
    // 削除確認など、通常の波形を残す小窓はここで最後に重ねる。
    draw_fn_information_chip(c);
    push_wave_canvas();
    return;
  }
  if (current_mode == sampler_mode_t::mode_loop
   || (current_mode == sampler_mode_t::mode_play && loop_playing)) {
    draw_loop_timeline();
    return;
  }
  if (current_mode == sampler_mode_t::mode_fx) {
    draw_fx_panel();
    return;
  }
  if (current_mode == sampler_mode_t::mode_play) {
    auto reg = kp::system_registry;
    int pos = reg->raw_wave_pos;
    for (int x = 0; x < w; ++x) {
      int idx = (pos - w + x + reg->raw_wave_length * 2) % reg->raw_wave_length;
      auto mm = reg->raw_wave[idx];
      int y0 = h - 1 - (mm.second * h) / 256;
      int y1 = h - 1 - (mm.first  * h) / 256;
      if (y1 <= y0) { y1 = y0 + 1; }
      c.drawFastVLine(x, y0, y1 - y0, 0x30C070u);
    }
    c.drawFastHLine(0, h / 2, w, 0x204030u);
    push_wave_canvas();
    return;
  }
  auto reg = kp::system_registry;
  int pos = reg->raw_wave_pos;
  for (int x = 0; x < w; ++x) {
    int idx = (pos - w + x + reg->raw_wave_length * 2) % reg->raw_wave_length;
    auto mm = reg->raw_wave[idx];
    int y0 = h - 1 - (mm.second * h) / 256;
    int y1 = h - 1 - (mm.first  * h) / 256;
    if (y1 <= y0) { y1 = y0 + 1; }
    c.drawFastVLine(x, y0, y1 - y0, 0x30C070u);
  }
  c.drawFastHLine(0, h / 2, w, 0x204030u);
  push_wave_canvas();
}

static void draw_tabs(void) {
  if (ui_surface_exclusive) { return; }
  auto& d = M5.Display;
  const int tab_count = (int)sampler_mode_t::mode_max;
  const int tab_w = d.width() / tab_count;
  d.startWrite();
  for (int i = 0; i < tab_count; ++i) {
    const auto& info = mode_info[i];
    const bool active = (i == (int)current_mode);
    const int x = i * tab_w;
    d.fillRoundRect(x + 2, tab_y, tab_w - 4, tab_h, 4, active ? info.screen_color : 0x282830u);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(active ? 0x000000u : 0x9090A0u);
    int tx = x + tab_w / 2;
    int ty = tab_y + tab_h / 2;
    d.drawString(info.name, tx, ty);
  }
  d.endWrite();
}

//-------------------------------------------------------------------------
// アイコン描画 (Fnボタン・Padバッジ共用)
// Phosphor Icons (Fill) のアルファマップを前景色/背景色ブレンドで描画する

#include "sampler_icons.inl"

enum class icon_t : uint8_t {
  pencil,      // 編集
  reverse,     // 逆再生
  trash,       // 削除
  volume,      // 音量
  exit_door,   // 編集終了
  one_shot,    // One (▶+終端バー)
  hold_gate,   // Hold (ゲート波形)
  loop_arrow,  // Loop (循環矢印)
  play,        // 再生
  stop,        // 停止
  loop_end,    // ループ長確定 (循環矢印+終端バー)
  mute,        // ミュート
};

static const sampler_icon_t& icon_asset(icon_t icon, bool large)
{
  switch (icon) {
  default:
  case icon_t::pencil:     return large ? icon_pencil_24  : icon_pencil_12;
  case icon_t::reverse:    return large ? icon_reverse_24 : icon_reverse_12;
  case icon_t::trash:      return large ? icon_trash_24   : icon_trash_12;
  case icon_t::volume:     return large ? icon_volume_24  : icon_volume_12;
  case icon_t::mute:       return large ? icon_mute_24    : icon_mute_12;
  case icon_t::exit_door:  return large ? icon_exit_24    : icon_exit_12;
  case icon_t::one_shot:   return large ? icon_one_24     : icon_one_12;
  case icon_t::hold_gate:  return large ? icon_hold_24    : icon_hold_12;
  case icon_t::loop_arrow:
  case icon_t::loop_end:   return large ? icon_loop_24    : icon_loop_12;
  case icon_t::play:       return large ? icon_play_24    : icon_play_12;
  case icon_t::stop:       return large ? icon_stop_24    : icon_stop_12;
  }
}

// s>=8 で24pxアセット、それ未満は12pxアセットを使用。
// bg は描画先の下地色 (アルファブレンドに使うため、単色の下地に描くこと)
static void draw_icon(m5gfx::LovyanGFX& g, icon_t icon, int cx, int cy, int s, uint32_t fg, uint32_t bg)
{
  const auto& ic = icon_asset(icon, s >= 8);
  static m5gfx::rgb565_t linebuf[32];
  const int x0 = cx - ic.w / 2;
  const int y0 = cy - ic.h / 2;
  const int fr = (fg >> 16) & 0xFF;
  const int fg8 = (fg >> 8) & 0xFF;
  const int fb = fg & 0xFF;
  const int br = (bg >> 16) & 0xFF;
  const int bg8 = (bg >> 8) & 0xFF;
  const int bb = bg & 0xFF;
  const uint8_t* p = ic.alpha;
  for (int yy = 0; yy < ic.h; ++yy) {
    for (int xx = 0; xx < ic.w; ++xx) {
      int a = *p++;
      linebuf[xx] = m5gfx::rgb565_t(
        (uint8_t)(br  + (((fr  - br ) * a) >> 8)),
        (uint8_t)(bg8 + (((fg8 - bg8) * a) >> 8)),
        (uint8_t)(bb  + (((fb  - bb ) * a) >> 8)));
    }
    g.pushImage(x0, y0 + yy, ic.w, 1, linebuf);
  }
  if (icon == icon_t::loop_end) {
    // ループ終端バーを右側に追加
    g.fillRect(cx + ic.w / 2 + 2, cy - ic.h / 2 + 2, 3, ic.h - 4, fg);
  }
}

static uint32_t pad_wave_shape_signature(const sample_slot_t& slot)
{
  uint32_t signature = (uint32_t)(uintptr_t)slot.pcm;
  signature ^= slot.frames * 2654435761u;
  signature ^= slot.playStart() * 2246822519u;
  signature ^= slot.playEnd() * 3266489917u;
  signature ^= (uint32_t)slot.volume_q8 << 16;
  return signature;
}

static void update_pad_wave_shape(int pad, const sample_slot_t& slot)
{
  auto& shape = pad_wave_shape[pad];
  uint32_t signature = pad_wave_shape_signature(slot);
  if (shape.valid && shape.signature == signature) { return; }

  const int wy = 9;
  const int wh = cell_h - 18;
  const int cy = wy + wh / 2;
  uint32_t start = slot.playStart();
  uint32_t frames = slot.playFrames();
  for (uint8_t column = 0; column < pad_wave_columns; ++column) {
    uint32_t a = start + ((uint64_t)column * frames) / pad_wave_columns;
    uint32_t b = start + ((uint64_t)(column + 1) * frames) / pad_wave_columns;
    if (b <= a) { b = std::min<uint32_t>(a + 1, slot.playEnd()); }
    int16_t mn = INT16_MAX;
    int16_t mx = INT16_MIN;
    uint32_t first_bin = ((uint64_t)a * sample_slot_t::waveform_bins) / slot.frames;
    uint32_t last_bin = ((uint64_t)(b ? b - 1 : 0) * sample_slot_t::waveform_bins) / slot.frames;
    if (first_bin >= sample_slot_t::waveform_bins) { first_bin = sample_slot_t::waveform_bins - 1; }
    if (last_bin >= sample_slot_t::waveform_bins) { last_bin = sample_slot_t::waveform_bins - 1; }
    for (uint32_t bin = first_bin; bin <= last_bin; ++bin) {
      // 音声は多重発音の余裕を残して約-12dBFSへ正規化している。Pad内の
      // 小さな波形にも編集ビューと同じ表示専用の3倍ゲインを使い、視認性を保つ。
      int16_t min_value = apply_sample_wave_display_gain(slot.waveform_min[bin], slot.volume_q8);
      int16_t max_value = apply_sample_wave_display_gain(slot.waveform_max[bin], slot.volume_q8);
      if (mn > min_value) { mn = min_value; }
      if (mx < max_value) { mx = max_value; }
    }
    int top = cy - ((int)mx * (wh / 2 - 1)) / 32768;
    int bottom = cy - ((int)mn * (wh / 2 - 1)) / 32768;
    if (bottom <= top) { bottom = top + 1; }
    shape.top[column] = std::max(0, std::min((int)cell_h - 1, top));
    shape.bottom[column] = std::max(0, std::min((int)cell_h, bottom));
  }
  shape.signature = signature;
  shape.valid = true;
}

static void draw_pad_frame(int pad)
{
  if (ui_surface_exclusive) { return; }
  // FX/Mixer pads have their own role colours and backgrounds.  A generic
  // performance-frame refresh would repaint only their border with the
  // underlying sample colour (notably after releasing the previous Repeat
  // button during a chorded FX gesture).  Route every such refresh back
  // through the complete role-aware Pad renderer instead.
  if (current_mode == sampler_mode_t::mode_fx) {
    request_pad_draw(pad);
    return;
  }
  const auto& color = pad_colors(pad);
  const bool active = pad_highlighted(pad) || pad_repeat_next_msec[pad];
  const uint32_t frame = active ? color.bg_hi : pad_off_background(color);
  const int x = grid_x + (pad % 4) * col_pitch;
  const int y = grid_y + (pad / 4) * row_pitch;
  auto& d = M5.Display;
  d.startWrite();
  d.drawRoundRect(x, y, pad_w, cell_h, 6, frame);
  d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, frame);
  d.endWrite();
}

static void draw_pad_content(m5gfx::LovyanGFX& d, int pad, int origin_x = 0, int origin_y = 0)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& c = pad_colors(pad);
  auto& slot = sampler_pool_t::slot[pad];
  const int x = grid_x + (pad % 4) * col_pitch - origin_x;
  const int y = grid_y + (pad / 4) * row_pitch - origin_y;
  // パッド本体は常にOFF色で描く。演奏状態は最後に外枠だけで示す。
  uint32_t background = pad_off_background(c);
  d.fillRoundRect(x, y, pad_w, cell_h, 6, background);
  if (edit_pad >= 0) {
    const uint8_t number = pad_display_number((uint8_t)pad);
    auto& edited = sampler_pool_t::slot[edit_pad];
    const bool sustain_ready =
      edited.synth_sustain_mode == sample_sustain_mode_t::manual
      ? edited.synth_loop_end > edited.synth_loop_start + 31
      : edited.synth_sustain_mode == sample_sustain_mode_t::automatic
        && edited.synth_sustain_auto;
    const char* label = "";
    uint32_t accent = 0x606068u;
    bool enabled = false;
    bool focused = false;
    bool trash = false;
    bool menu_back = false;
    if (edit_chop_page) {
      switch (number) {
      case 1: label = "FIT"; accent = 0x70D8FFu; enabled = edit_chop_fit_mode == chop_fit_mode_t::fit_bgm; break;
      case 2: label = "KEEP"; accent = 0xA0A8B8u; enabled = edit_chop_fit_mode == chop_fit_mode_t::keep_speed; break;
      case 5: label = "4"; accent = 0xF0C050u; enabled = edit_chop_count_mode == chop_count_mode_t::four; break;
      case 6: label = "8"; accent = 0xF0C050u; enabled = edit_chop_count_mode == chop_count_mode_t::eight; break;
      case 7: label = "12"; accent = 0xF0C050u; enabled = edit_chop_count_mode == chop_count_mode_t::twelve; break;
      case 8: label = "AUTO"; accent = 0xF0C050u; enabled = edit_chop_count_mode == chop_count_mode_t::automatic; break;
      default: break;
      }
    } else if (edit_synth_page) {
      switch (number) {
      case 1:
        label = "Mel";
        accent = performance_page_colors[(uint8_t)performance_page_t::melody];
        enabled = melody_settings.source == synth_tone_source_t::pad && melody_settings.pad == (uint8_t)edit_pad;
        break;
      case 2:
        label = "Chord";
        accent = performance_page_colors[(uint8_t)performance_page_t::chord];
        enabled = chord_settings.source == synth_tone_source_t::pad && chord_settings.pad == (uint8_t)edit_pad;
        break;
      case 3:
        label = "Bass";
        accent = performance_page_colors[(uint8_t)performance_page_t::bass];
        enabled = bass_settings.source == synth_tone_source_t::pad && bass_settings.pad == (uint8_t)edit_pad;
        break;
      case 8:
        label = "Back";
        accent = 0xFFD0D0u;
        enabled = true;
        menu_back = true;
        break;
      case 9:
        label = "Loop";
        accent = 0x80E0B0u;
        enabled = sustain_ready;
        focused = edit_param == 10;
        break;
      case 10:
        label = "In";
        accent = 0x50D8D0u;
        focused = edit_param == 7;
        break;
      case 11:
        label = "Out";
        accent = 0x50D8D0u;
        focused = edit_param == 8;
        break;
      case 12:
        label = "Rel";
        accent = 0xF0A050u;
        focused = edit_param == 9;
        break;
      default:
        break;
      }
    } else switch (number) {
    case 1:
      label = "Chop";
      accent = 0xF0C050u;
      break;
    case 4:
      trash = true;
      accent = 0xFF6060u;
      break;
    case 5:
      label = "Hold";
      accent = 0x50C8D8u;
      enabled = sampler_pool_t::slot[edit_pad].hold_enabled;
      focused = edit_param == 5;
      break;
    case 6:
      label = "Rep";
      enabled = sampler_pool_t::slot[edit_pad].loop_enabled;
      focused = edit_param == 4;
      accent = 0xF0C050u;
      break;
    case 7:
      label = "Rev";
      accent = 0xD080E0u;
      enabled = edited.reverse;
      focused = edit_param == 6;
      break;
    case 8:
      label = "Synth";
      accent = 0x50D8D0u;
      enabled = sustain_ready;
      break;
    case 9: label = "Start"; focused = edit_param == 0; accent = 0xFF7050u; break;
    case 10: label = "End"; focused = edit_param == 1; accent = 0x50A0FFu; break;
    case 11: label = "Vol"; focused = edit_param == 2; accent = 0x60E080u; break;
    case 12: label = "Pitch"; focused = edit_param == 3; accent = 0xB080FFu; break;
    default: break;
    }
    const bool assigned = label[0] || trash;
    const bool pressed = pads[pad].pressed;
    const bool active = focused || pressed;
    const uint32_t function_bg = menu_back ? 0x483030u : assigned
      ? scale_rgb24(accent, active ? 3 : enabled ? 2 : 1, active ? 8 : enabled ? 9 : 7)
      : 0x18181Eu;
    d.fillRoundRect(x, y, pad_w, cell_h, 6, function_bg);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(active ? 0xFFFFFFu : menu_back ? 0xFFD0D0u : accent, function_bg);
    if (trash) {
      draw_icon(d, icon_t::trash, x + pad_w / 2, y + cell_h / 2, 8,
                active ? 0xFFFFFFu : accent, function_bg);
    } else {
      d.drawString(label, x + pad_w / 2, y + cell_h / 2);
    }
    if (focused) { d.fillRect(x + 7, y + cell_h - 5, pad_w - 14, 2, accent); }
    const uint32_t frame = menu_back ? 0x606078u : assigned
      ? (active ? accent : enabled ? scale_rgb24(accent, 3, 4) : scale_rgb24(accent, 1, 2))
      : 0x282830u;
    d.drawRoundRect(x, y, pad_w, cell_h, 6, frame);
    d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, frame);
    return;
  }
  if (current_mode == sampler_mode_t::mode_fx) {
    const uint8_t number = pad_display_number((uint8_t)pad);
    if (mixer_active) {
      const mixer_part_t mapped_part = mixer_part_for_pad_number(number);
      if (mapped_part != mixer_part_t::count) {
        const uint8_t part = (uint8_t)mapped_part;
        const uint32_t color = mixer_part_colors[part];
        const uint32_t bg = mixer_part_muted[part] ? 0x141418u : scale_rgb24(color, 1, 5);
        d.fillRoundRect(x + 2, y + 2, pad_w - 4, cell_h - 4, 4, bg);
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(1);
        d.setTextDatum(m5gfx::textdatum_t::top_center);
        d.setTextColor(mixer_part_muted[part] ? 0x707078u : color, bg);
        d.drawString(mixer_part_labels[part], x + pad_w / 2, y + 5);
        char value[8];
        snprintf(value, sizeof(value), mixer_part_muted[part] ? "MUTE" : "%u%%",
                 (unsigned)mixer_part_volume[part]);
        d.setTextDatum(m5gfx::textdatum_t::bottom_center);
        d.drawString(value, x + pad_w / 2, y + cell_h - 5);
        const bool held = pads[pad].pressed;
        d.drawRoundRect(x, y, pad_w, cell_h, 6, held ? 0xFFFFFFu : color);
        return;
      }
      if (number >= 9 && number <= 12) {
        const uint8_t scene = number - 9;
        const bool valid = mixer_snapshot[scene].valid;
        const bool pending = mixer_pending_snapshot == (int8_t)scene;
        const bool selected = pads[pad].pressed;
        const bool applied = mixer_applied_snapshot == (int8_t)scene;
        static constexpr uint32_t scene_color = 0x80D0FFu;
        const bool blink = ((M5.millis() / 250) & 1u) == 0;
        const uint32_t bg = applied ? scene_color : 0x08080Cu;
        const uint32_t text = applied ? 0x08080Cu
          : valid ? scene_color : 0x909098u;
        const uint32_t frame = selected ? 0xFFFFFFu
          : pending && blink ? 0xFFFFFFu
          : valid ? scene_color : 0x202028u;
        d.fillRoundRect(x + 2, y + 2, pad_w - 4, cell_h - 4, 4, bg);
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(2);
        d.setTextDatum(m5gfx::textdatum_t::middle_center);
        d.setTextColor(text, bg);
        char label[2] = { (char)('1' + scene), 0 };
        d.drawString(label, x + pad_w / 2, y + cell_h / 2);
        d.drawRoundRect(x, y, pad_w, cell_h, 6, frame);
        if (valid || pending || selected || applied) {
          d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, frame);
        }
        return;
      }
      d.drawRoundRect(x, y, pad_w, cell_h, 6, 0x282830u);
      return;
    }
    const char* line1 = "";
    const char* line2 = "";
    uint32_t accent = 0x606068u;
    switch (number) {
    case 1: line1 = "REP"; line2 = "4"; accent = fx_control_colors[2]; break;
    case 2: line1 = "REP"; line2 = "2"; accent = fx_control_colors[2]; break;
    case 3: line1 = "REP"; line2 = "1"; accent = fx_control_colors[2]; break;
    case 4: line1 = "REP"; line2 = "0.5"; accent = fx_control_colors[2]; break;
    case 5: line1 = "FIL"; accent = fx_control_colors[1]; break;
    case 6: line1 = "TMP"; accent = fx_control_colors[0]; break;
    case 7: line1 = "TAPE"; line2 = "STOP"; accent = fx_tape_stop_color; break;
    case 8: line1 = "DLY"; line2 = delay_grid_labels[std::min<uint8_t>(
      (uint8_t)fx_param[fx_delay_index], (uint8_t)delay_grid_option_count - 1u)];
      accent = fx_control_colors[3]; break;
    default: break;
    }
    const bool selected = fx_pad_active == pad;
    const bool assigned = line1[0] != 0;
    const uint32_t background = assigned ? (selected ? accent : 0x18181Eu)
                                         : pad_off_background(empty_color);
    const uint32_t text = assigned ? (selected ? 0x08080Cu : accent) : 0x606068u;
    const uint32_t frame = assigned ? (selected ? 0xFFFFFFu : accent) : 0x282830u;
    d.fillRoundRect(x + 2, y + 2, pad_w - 4, cell_h - 4, 4, background);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(text, background);
    if (line2[0]) {
      d.drawString(line1, x + pad_w / 2, y + 14);
      d.drawString(line2, x + pad_w / 2, y + 31);
    } else {
      d.drawString(line1, x + pad_w / 2, y + cell_h / 2);
    }
    d.drawRoundRect(x, y, pad_w, cell_h, 6, frame);
    d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, frame);
    return;
  }
  if (current_page != performance_page_t::sample) {
    const uint8_t order = pad_display_number((uint8_t)pad) - 1;
    char label[12] = {};
    const bool muted = loop_is_muted(current_page, (uint8_t)pad);
    bool chord_degree_label_drawn = false;
    if (current_page == performance_page_t::melody
     || current_page == performance_page_t::bass) {
      static constexpr const char* note_names[] = {
        "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
      };
      const auto& settings = page_settings(current_page);
      uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
      uint8_t note = std::clamp<int>(
        sampler_scale_notes[scale][order] + pitched_page_key(current_page)
          + pitched_page_octave_semitones(current_page, settings),
        0, 127);
      snprintf(label, sizeof(label), "%s%d", note_names[note % 12], note / 12 - 1);
    } else if (current_page == performance_page_t::chord) {
      const uint8_t degree = chord_degree_for_order(order);
      if (degree != 0) {
        static constexpr const char* sharp_names[] = {
          "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        static constexpr const char* flat_names[] = {
          "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
        };
        // Key signature preference: C/Db/Eb/F/Ab/Bb use flats; the other
        // keys use sharps. This keeps every chord pad internally consistent.
        static constexpr bool prefer_flats[] = {
          true, true, false, true, false, true, false, false, true, false, true, false
        };
        const auto& chord = chord_template_entry(degree);
        const uint8_t root_note = (harmony_key() + chord.root_semitones) % 12;
        const char* root = (prefer_flats[chord_settings.key] ? flat_names : sharp_names)[root_note];
        const char* suffix = chord_quality_suffix(chord.quality, chord_modifier_pressed[0]);
        const uint32_t text_color = muted ? 0x707078u : 0xFFFFFFu;

        // Make the root letter the visual anchor; accidental and quality stay
        // compact so C#/Dbm still fit in the 44px pad.
        d.setFont(&fonts::efontJA_16_b);
        char root_letter[2] = { root[0], 0 };
        char accidental[2] = { root[1], 0 };
        d.setTextSize(2);
        const int root_width = d.textWidth(root_letter);
        d.setTextSize(1);
        const int suffix_width = root[1] ? d.textWidth(accidental) : 0;
        const int quality_width = d.textWidth(suffix);
        const int total_width = root_width + suffix_width + quality_width;
        const int label_x = x + (pad_w - total_width) / 2;
        d.setTextDatum(m5gfx::textdatum_t::top_left);
        d.setTextColor(text_color, background);
        d.setTextSize(2);
        d.drawString(root_letter, label_x, y + 5);
        d.setTextSize(1);
        if (root[1]) { d.drawString(accidental, label_x + root_width, y + 5); }
        if (suffix[0]) { d.drawString(suffix, label_x + root_width + suffix_width, y + 20); }
        d.setTextDatum(m5gfx::textdatum_t::top_left);
        d.setTextColor(0x808090u, background);
        char degree_text[2] = { (char)('0' + degree), 0 };
        d.drawString(degree_text, x + 3, y + 2);
        chord_degree_label_drawn = true;
      }
      static constexpr const char* modifier_labels[] = {
        "", "", "", "SWAP", "", "", "", "7th", "", "sus4", "9th", "M7"
      };
      snprintf(label, sizeof(label), "%s", modifier_labels[order]);
    } else {
      snprintf(label, sizeof(label), "%s", sampler_drum_labels[order]);
    }
    if (!chord_degree_label_drawn) {
      d.setFont(&fonts::efontJA_16_b);
      d.setTextSize(1);
      d.setTextDatum(m5gfx::textdatum_t::middle_center);
      d.setTextColor(muted ? 0x707078u : 0xFFFFFFu, background);
      d.drawString(label, x + pad_w / 2, y + cell_h / 2);
      d.setTextDatum(m5gfx::textdatum_t::top_left);
      d.setTextColor(0x808090u, background);
      if (current_page != performance_page_t::chord || chord_degree_for_order(order) != 0) {
        char number[4];
        snprintf(number, sizeof(number), "%u", (unsigned)(order + 1));
        d.drawString(number, x + 3, y + 2);
      }
    }
  } else if (recording_pad == pad) {
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(0xFFFFFFu);
    d.drawString("REC", x + pad_w / 2, y + cell_h / 2);
  } else if (slot.isValid()) {
    int wx = x + 4;
    int wy = y + 9;
    int ww = pad_w - 8;
    int wh = cell_h - 18;
    int cy = wy + wh / 2;
    // ミュート中は波形を減光してひと目で分かるようにする
    bool muted = loop_is_muted(current_page, (uint8_t)pad);
    uint32_t wave_color = muted ? 0x686868u : 0xF0D060u;
    uint32_t center_color = muted ? 0x404040u : 0x805020u;
    d.drawFastHLine(wx, cy, ww, center_color);
    update_pad_wave_shape(pad, slot);
    const auto& shape = pad_wave_shape[pad];
    for (uint8_t column = 0; column < pad_wave_columns; ++column) {
      int height = shape.bottom[column] - shape.top[column];
      d.fillRect(wx + column * pad_wave_column_width, y + shape.top[column],
                 pad_wave_column_width, std::max(1, height), wave_color);
    }
    // 再生方式/ミュートのアイコンバッジ。縁取りを避け、単色面に大きめに描く。
    int bx = x + pad_w - 10;
    int by = y + 10;
    uint32_t plate = 0x08080Cu;
    d.fillRoundRect(x + pad_w - 19, y + 1, 18, 18, 3, plate);
    if (muted) {
      draw_icon(d, icon_t::mute, bx, by, 5, 0xFF7070u, plate);
    } else {
      icon_t icon = slot.loop_enabled ? icon_t::loop_arrow
                  : slot.hold_enabled ? icon_t::hold_gate
                                      : icon_t::one_shot;
      draw_icon(d, icon, bx, by, 5, 0xFFFFFFu, plate);
      if (slot.loop_enabled && slot.hold_enabled) {
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(1);
        d.setTextDatum(m5gfx::textdatum_t::top_left);
        d.setTextColor(0x80D0FFu, plate);
        d.drawString("H", x + pad_w - 19, y + 1);
      }
    }
  }
  const bool active = pad_highlighted(pad) || pad_repeat_next_msec[pad];
  const uint32_t frame = active ? c.bg_hi : pad_off_background(c);
  d.drawRoundRect(x, y, pad_w, cell_h, 6, frame);
  d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, frame);
}

static int current_grid_cache_index()
{
  if (current_mode == sampler_mode_t::mode_play) { return (uint8_t)current_page; }
  if (current_mode == sampler_mode_t::mode_fx) { return fx_grid_cache_index; }
  return -1;
}

static void reset_grid_cache(int index)
{
  if (index < 0 || index >= grid_cache_count || !grid_cache_ready[index]) { return; }
  if (grid_cache_busy[index]) {
    grid_cache_pad_mask[index] = 0;
    grid_cache_fn_mask[index] = 0;
    grid_cache_page[index] = performance_page_t::max;
    return;
  }
  grid_cache_canvas[index].fillScreen(0x101018u);
  grid_cache_pad_mask[index] = 0;
  grid_cache_fn_mask[index] = 0;
  grid_cache_page[index] = current_page;
}

static void cache_grid_tile(M5Canvas& tile, int x, int y, int pad, int fn)
{
  const int index = current_grid_cache_index();
  if (index < 0 || !grid_cache_ready[index] || grid_cache_busy[index]) { return; }
  // The sample voices and BGM also stream from PSRAM. Cache maintenance is
  // optional, so never spend that shared bandwidth during loop playback or
  // the attack edge of a physical performance gesture.
  if (ble_midi_cache_guard_active()
   || loop_playing || sound_priority_active() || physical_input_pending()) {
    if (pad >= 0) { grid_cache_pad_mask[index] &= (uint16_t)~(1u << pad); }
    if (fn >= 0) { grid_cache_fn_mask[index] &= (uint8_t)~(1u << fn); }
    return;
  }
  if (grid_cache_page[index] != current_page) { reset_grid_cache(index); }
  auto* source = static_cast<const uint16_t*>(tile.getBuffer());
  auto* target = static_cast<uint16_t*>(grid_cache_canvas[index].getBuffer());
  if (source == nullptr || target == nullptr) { return; }
  const int local_y = y - grid_y;
  if (x < 0 || local_y < 0 || x + pad_w > grid_cache_w
   || local_y + cell_h > grid_cache_h) { return; }
  for (int row = 0; row < cell_h; ++row) {
    memcpy(target + (local_y + row) * grid_cache_w + x,
           source + row * pad_w, pad_w * sizeof(uint16_t));
  }
  if (pad >= 0) { grid_cache_pad_mask[index] |= (uint16_t)(1u << pad); }
  if (fn >= 0) { grid_cache_fn_mask[index] |= (uint8_t)(1u << fn); }
}

static void draw_pad(int pad)
{
  if (ui_surface_exclusive) { return; }
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  const int x = grid_x + (pad % 4) * col_pitch;
  const int y = grid_y + (pad / 4) * row_pitch;
  if (ui_dirty_renderer_ready) {
    int canvas_index = -1;
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      const uint8_t candidate = (ui_dirty_canvas_index + attempt) & 1;
      if (!ui_dirty_canvas_busy[candidate]) {
        canvas_index = candidate;
        ui_dirty_canvas_index = (candidate + 1) & 1;
        ui_dirty_canvas_busy[candidate] = true;
        break;
      }
    }
    if (canvas_index < 0) {
      auto& d = M5.Display;
      d.startWrite();
      draw_pad_content(d, pad);
      d.endWrite();
      return;
    }
    auto& c = ui_dirty_canvas[canvas_index];
    c.fillScreen(0x101018u);
    draw_pad_content(c, pad, x, y);
    cache_grid_tile(c, x, y, pad, -1);
#if !defined(M5UNIFIED_PC_BUILD)
    if (ui_async_tile_submit && ui_tile_render_queue != nullptr
     && touch_render_task_handle != nullptr) {
      const ui_tile_transfer_t transfer {
        ui_tile_transfer_t::kind_t::tile, (uint8_t)canvas_index,
        (int16_t)x, (int16_t)y, ui_page_generation
      };
      if (xQueueSend(ui_tile_render_queue, &transfer, 0) == pdTRUE) { return; }
      // Core 0 is still presenting an older surface. Never fall back to a
      // concurrent direct LCD write; keep this Pad dirty for a later turn.
      ui_dirty_canvas_busy[canvas_index] = false;
      request_pad_draw(pad);
      ui_render_metrics.dropped_requests = ui_render_metrics.dropped_requests + 1;
      return;
    }
#endif
    c.pushSprite(x, y);
    ui_dirty_canvas_busy[canvas_index] = false;
    return;
  }
  auto& d = M5.Display;
  d.startWrite();
  draw_pad_content(d, pad);
  d.endWrite();
}

static constexpr const uint32_t edit_start_color = 0xFF7050u;
static constexpr const uint32_t edit_end_color   = 0x50A0FFu;
static constexpr const uint32_t edit_vol_color   = 0x60E080u;
static constexpr const uint32_t edit_pitch_color = 0xB080FFu;
static constexpr const uint32_t edit_preview_label_color = 0x70E080u;
static constexpr const uint32_t menu_ok_label_color = 0xB0FFD0u;
static constexpr const uint32_t menu_back_label_color = 0xFFD0D0u;

static void draw_fn_content(m5gfx::LovyanGFX& d, int fn, int origin_x = 0, int origin_y = 0)
{
  const int x = fn_x - origin_x;
  const int y = grid_y + fn * row_pitch - origin_y;
  bool active = fn_pressed[fn];
  if (current_mode == sampler_mode_t::mode_fx && fn == 2 && mixer_active) {
    active = true;
  }
  if (fn == 1
   && (current_page == performance_page_t::melody
    || current_page == performance_page_t::bass
    || current_page == performance_page_t::chord)
   && performance_page_part_muted(current_page)) {
    active = true;
  }
  bool modifier_hint = !active && fn_modifier_hint(fn);
  auto fn_accent = [fn]() -> uint32_t {
    if (current_mode == sampler_mode_t::mode_rec) {
      static constexpr uint32_t colors[] = { 0xFFB050u, 0x80D0FFu, 0xFF7070u };
      return colors[fn];
    }
    static constexpr uint32_t colors[] = { 0x70E080u, 0x80D0FFu, 0xB080FFu };
    return colors[fn];
  };
  uint32_t bg = active ? fn_color.bg_hi : fn_color.bg;
  if (modifier_hint) { bg = 0x34344Cu; }
  d.fillRoundRect(x, y, fn_w, cell_h, 6, bg);
  if (modifier_hint) {
    d.drawRoundRect(x, y, fn_w, cell_h, 6, 0x585878u);
  }

  const int cx = x + fn_w / 2;
  const int cy = y + cell_h / 2;
  const int s = 9;
  uint32_t color = active ? 0xFFFFFFu : modifier_hint ? 0xB0B0D0u : 0x9090C0u;

  if (edit_pad >= 0) {
    // EDIT中はFnを確定系へ固定し、編集対象はPad 1〜12で選ぶ。
    if (edit_chop_page) {
      if (fn == 0) {
        draw_icon(d, icon_t::volume, cx, cy, s,
                  active ? 0xFFFFFFu : edit_preview_label_color, bg);
      } else {
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(1);
        d.setTextDatum(m5gfx::textdatum_t::middle_center);
        d.setTextColor(active ? 0xFFFFFFu
                       : fn == 1 ? menu_ok_label_color : menu_back_label_color, bg);
        d.drawString(fn == 1 ? "CHOP" : "BACK", cx, cy);
      }
    } else if (fn == 0) {
      draw_icon(d, icon_t::volume, cx, cy, s,
                active ? 0xFFFFFFu : edit_preview_label_color, bg);
    } else {
      d.setFont(&fonts::efontJA_16_b);
      d.setTextSize(1);
      d.setTextDatum(m5gfx::textdatum_t::middle_center);
      d.setTextColor(active ? 0xFFFFFFu
                     : fn == 1 ? menu_ok_label_color : menu_back_label_color, bg);
      d.drawString(fn == 1 ? "OK" : "EXIT", cx, cy);
    }
    return;
  }

  switch (current_mode) {
  case sampler_mode_t::mode_rec:
  case sampler_mode_t::mode_play: {
    if (fn == 0) {
      if (performance_record_armed && !loop_playing) {
        draw_record_button_icon(d, cx, cy, active, bg);
      } else {
        draw_icon(d, loop_playing ? icon_t::stop : icon_t::play, cx, cy, s,
                  active ? 0xFFFFFFu : loop_playing ? 0xFF7070u : 0x70E080u, bg);
      }
    } else if (fn == 1) {
      draw_icon(d, icon_t::mute, cx, cy, s, color, bg);
    } else if (current_mode == sampler_mode_t::mode_rec
            && current_page == performance_page_t::sample) {
      draw_icon(d, icon_t::trash, cx, cy, s, active ? 0xFFFFFFu : 0xFF7070u, bg);
    } else if (current_mode == sampler_mode_t::mode_play
            && (current_page == performance_page_t::melody || current_page == performance_page_t::bass)) {
      d.setFont(&fonts::efontJA_16_b);
      d.setTextSize(1);
      d.setTextDatum(m5gfx::textdatum_t::middle_center);
      d.setTextColor(active ? 0xFFFFFFu : 0xB080FFu, bg);
      d.drawString("TOUCH", cx, cy);
    }
    break; }
  case sampler_mode_t::mode_fx: {
    if (fn == 0) {
      if (performance_record_armed && !loop_playing) {
        draw_record_button_icon(d, cx, cy, active, bg);
      } else {
        draw_icon(d, loop_playing ? icon_t::stop : icon_t::play, cx, cy, s,
                  active ? 0xFFFFFFu : loop_playing ? 0xFF7070u : 0x70E080u, bg);
      }
    } else if (fn == 1) {
      draw_icon(d, icon_t::mute, cx, cy, s, color, bg);
    } else {
      d.setFont(&fonts::efontJA_16_b);
      d.setTextSize(1);
      d.setTextDatum(m5gfx::textdatum_t::middle_center);
      d.setTextColor(active ? 0xFFFFFFu : 0xB080FFu, bg);
      d.drawString("MIX", cx, cy);
    }
    break; }
  case sampler_mode_t::mode_loop:
    if (fn == 0) {
      // 未確定: ループを閉じる / 確定後: 再生状態のトグル
      if (!loop_length_fixed) {
        draw_icon(d, icon_t::loop_end, cx - 2, cy, s, active ? 0xFFFFFFu : 0xFFB050u, bg);
      } else if (loop_playing) {
        draw_icon(d, icon_t::stop, cx, cy, s, active ? 0xFFFFFFu : 0xFF7070u, bg);
      } else {
        draw_icon(d, icon_t::play, cx, cy, s, active ? 0xFFFFFFu : 0x70E080u, bg);
      }
    } else if (fn == 1) {
      draw_icon(d, icon_t::mute, cx, cy, s, color, bg);
    } else {
      draw_icon(d, icon_t::trash, cx, cy, s, color, bg);
    }
    break;
  default:
    break;
  }
}

static void draw_fn(int fn)
{
  if (ui_surface_exclusive) { return; }
  if (fn < 0 || fn >= 3) { return; }
  const int y = grid_y + fn * row_pitch;
  if (ui_dirty_renderer_ready) {
    int canvas_index = -1;
    for (uint8_t attempt = 0; attempt < 2; ++attempt) {
      const uint8_t candidate = (ui_dirty_canvas_index + attempt) & 1;
      if (!ui_dirty_canvas_busy[candidate]) {
        canvas_index = candidate;
        ui_dirty_canvas_index = (candidate + 1) & 1;
        ui_dirty_canvas_busy[candidate] = true;
        break;
      }
    }
    if (canvas_index < 0) {
      auto& d = M5.Display;
      d.startWrite();
      draw_fn_content(d, fn);
      d.endWrite();
      return;
    }
    auto& c = ui_dirty_canvas[canvas_index];
    c.fillScreen(0x101018u);
    draw_fn_content(c, fn, fn_x, y);
    cache_grid_tile(c, fn_x, y, -1, fn);
#if !defined(M5UNIFIED_PC_BUILD)
    if (ui_async_tile_submit && ui_tile_render_queue != nullptr
     && touch_render_task_handle != nullptr) {
      const ui_tile_transfer_t transfer {
        ui_tile_transfer_t::kind_t::tile, (uint8_t)canvas_index,
        (int16_t)fn_x, (int16_t)y, ui_page_generation
      };
      if (xQueueSend(ui_tile_render_queue, &transfer, 0) == pdTRUE) { return; }
      ui_dirty_canvas_busy[canvas_index] = false;
      request_fn_draw(fn);
      ui_render_metrics.dropped_requests = ui_render_metrics.dropped_requests + 1;
      return;
    }
#endif
    c.pushSprite(fn_x, y);
    ui_dirty_canvas_busy[canvas_index] = false;
    return;
  }
  auto& d = M5.Display;
  d.startWrite();
  draw_fn_content(d, fn);
  d.endWrite();
}

static void draw_grid(void) {
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { draw_pad(i); }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static bool ui_dirty_canvas_available()
{
  return !ui_dirty_renderer_ready
      || !ui_dirty_canvas_busy[0]
      || !ui_dirty_canvas_busy[1];
}

static bool ui_async_display_busy()
{
#if !defined(M5UNIFIED_PC_BUILD)
  // Wave strips and Pad/Fn tiles share the same physical SPI transaction.
  // A direct frame draw must wait until the renderer task has returned from
  // pushSprite(), otherwise M5GFX may see startWrite/endWrite on different
  // tasks and assert in spiEndTransaction.
  if (wave_transfer_job_pending) { return true; }
  if (ui_dirty_canvas_busy[0] || ui_dirty_canvas_busy[1]) { return true; }
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    if (grid_cache_busy[i]) { return true; }
  }
#endif
  return false;
}

static void wait_ui_dirty_transfers()
{
#if !defined(M5UNIFIED_PC_BUILD)
  const uint32_t deadline = M5.millis() + 30;
  bool busy = true;
  while (busy && (int32_t)(M5.millis() - deadline) < 0) {
    busy = ui_dirty_canvas_busy[0] || ui_dirty_canvas_busy[1];
    for (uint8_t i = 0; i < grid_cache_count && !busy; ++i) {
      busy = grid_cache_busy[i];
    }
    if (!busy) { break; }
    M5.delay(1);
  }
#endif
}

static bool present_current_grid_cache()
{
  const int index = current_grid_cache_index();
  const uint16_t full_pad_mask = (1u << def::pad::pad_count) - 1;
  if (index < 0 || !grid_cache_ready[index] || grid_cache_busy[index]
   || grid_cache_page[index] != current_page
   || grid_cache_pad_mask[index] != full_pad_mask) {
    return false;
  }
#if !defined(M5UNIFIED_PC_BUILD)
  if (ui_tile_render_queue != nullptr && touch_render_task_handle != nullptr) {
    const ui_tile_transfer_t transfer {
      ui_tile_transfer_t::kind_t::grid, (uint8_t)index,
      0, (int16_t)grid_y, ui_page_generation
    };
    grid_cache_busy[index] = true;
    if (xQueueSend(ui_tile_render_queue, &transfer, 0) == pdTRUE) {
      dirty_pad_mask = 0;
      dirty_pad_state_mask = 0;
      return true;
    }
    grid_cache_busy[index] = false;
    ui_render_metrics.dropped_requests = ui_render_metrics.dropped_requests + 1;
    return false;
  }
#endif
  grid_cache_canvas[index].pushSprite(0, grid_y);
  dirty_pad_mask = 0;
  dirty_pad_state_mask = 0;
  return true;
}

static void flush_dirty_ui(bool force = false)
{
  // Wi-Fi temporarily releases every retained surface for TLS/HTTP memory.
  // Never let ordinary dirty jobs draw through deleted sprites; the compact
  // Wi-Fi screen owns the LCD until the arena has been rebuilt.
  if (performance_ui_arena_suspended) { return; }
  if (ui_surface_exclusive) { return; }
  // Long-press feedback owns a direct LCD overlay. Keep queued Pad/Fn tiles
  // behind it until release; otherwise Core 0 can enter M5GFX while Core 1 is
  // updating the progress chip.
  if (!force && hold_progress_kind != hold_progress_kind_t::none) { return; }
  // Header/wave use direct M5GFX transactions on the input core. Wait for the
  // Core-0 tile presenter instead of nesting display transactions cross-core.
  if (!force && ui_async_display_busy()) { return; }
  uint32_t now = M5.millis();
  if (!force && (sound_priority_active(now) || physical_input_pending())) { return; }

  if (dirty_header) {
    dirty_header = false;
    draw_header();
    // Page changes are intentionally composed over several main-loop turns.
    // This gives physical input another scheduling point between LCD regions.
    if (!force) { return; }
  }
  if (dirty_wave && !wave_transfer_active) {
    dirty_wave = false;
    draw_wave();
    if (!force) { return; }
  }
  // The asynchronous renderer owns two DMA tiles. Limit each turn to those
  // two buffers so composition never waits behind an LCD transfer.
  const uint8_t pad_budget = force ? def::pad::pad_count : 2;
  uint8_t pads_drawn = 0;
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    const uint16_t bit = (uint16_t)(1u << i);
    if (dirty_pad_mask & bit) {
      if (!force && !ui_dirty_canvas_available()) { return; }
      dirty_pad_mask &= ~bit;
      dirty_pad_state_mask &= ~bit;
      update_pad_led(i);
      ui_async_tile_submit = !force && edit_pad < 0;
      draw_pad(i);
      ui_async_tile_submit = false;
      if (++pads_drawn >= pad_budget) { break; }
    } else if (dirty_pad_state_mask & bit) {
      if (!force && ui_async_display_busy()) { return; }
      dirty_pad_state_mask &= ~bit;
      update_pad_led(i);
      draw_pad_frame(i);
      if (++pads_drawn >= pad_budget) { break; }
    }
  }
  if (!force && (dirty_pad_mask != 0 || dirty_pad_state_mask != 0
              || pads_drawn >= pad_budget)) { return; }
  uint8_t fn_budget = force ? 3 : (uint8_t)(pad_budget - pads_drawn);
  uint8_t fns_drawn = 0;
  for (int i = 0; i < 3; ++i) {
    const uint8_t bit = (uint8_t)(1u << i);
    if (dirty_fn_mask & bit) {
      if (!force && (!ui_dirty_canvas_available() || fns_drawn >= fn_budget)) { return; }
      dirty_fn_mask &= ~bit;
      update_fn_led(i);
      ui_async_tile_submit = !force && edit_pad < 0;
      draw_fn(i);
      ui_async_tile_submit = false;
      ++fns_drawn;
    }
  }
}

static void draw_all(void) {
  if (performance_ui_arena_suspended || ui_surface_exclusive) { return; }
  wait_ui_dirty_transfers();
  M5.Display.fillScreen(0x101018u);
  dirty_pad_mask = 0;
  dirty_pad_state_mask = 0;
  dirty_fn_mask = 0;
  dirty_wave = false;
  dirty_header = false;
  invalidate_loop_timeline_cache();
  draw_header(true);
  draw_wave();
  draw_tabs();
  draw_grid();
}

static void restore_performance_surface_from_cache(void)
{
  if (performance_ui_arena_suspended) { return; }
  wait_ui_dirty_transfers();
  // Only the narrow gaps are not owned by a retained region. Clear those and
  // restore the header, information area, tabs and Pad/Fn grid independently.
  // This avoids rebuilding fifteen button tiles after every touch gesture.
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, header_h, d.width(), wave_y - header_h, 0x101018u);
  d.fillRect(0, wave_y + wave_h, d.width(), tab_y - (wave_y + wave_h), 0x101018u);
  d.fillRect(0, tab_y + tab_h, d.width(), grid_y - (tab_y + tab_h), 0x101018u);
  d.fillRect(0, grid_y, d.width(), d.height() - grid_y, 0x101018u);
  d.endWrite();

  dirty_pad_mask = 0;
  dirty_pad_state_mask = 0;
  dirty_fn_mask = 0;
  dirty_wave = false;
  dirty_header = false;
  invalidate_loop_timeline_cache();
  reset_live_wave();
  draw_header(true);
  draw_wave();
  draw_tabs();
  if (!present_current_grid_cache()) {
    // A cache can be absent after Wi-Fi released PSRAM or during its first
    // construction. Keep that uncommon path correct and rebuild it once.
    draw_grid();
  }
}

//-------------------------------------------------------------------------
// 簡易メニュー

enum class menu_page_t : uint8_t {
  root,
  kit,
  kit_edit,
  loop,
  loop_bgm,
  harmony,
  synthesizer,
  synth_melody,
  synth_melody_sound,
  synth_melody_midi,
  synth_melody_pad,
  synth_bass,
  synth_bass_sound,
  synth_bass_midi,
  synth_bass_pad,
  synth_chord,
  synth_chord_sound,
  synth_chord_midi,
  synth_chord_pad,
  synth_drum,
  input_assign,
  connections,
  midi_sound,
  ble_device,
  connection_info,
  input_source,
  wifi,
  wifi_setup,
  system,
};

enum class menu_item_kind_t : uint8_t {
  submenu,
  value,
  action,
};

enum class menu_value_t : uint8_t {
  none,
  loop_quantize,
  loop_note_grid,
  loop_note_off_grid,
  background_volume,
  background_repeat,
  external_input_mode,
  midi_note_action,
  midi_input,
  usb_mode,
  usb_host_power,
  usb_keyboard,
  wifi_auto_update,
  audio_input_source,
  menu_sound,
  harmony_key,
  harmony_scale,
  melody_source,
  melody_key_follow,
  melody_key,
  melody_scale,
  melody_octave,
  melody_volume,
  bass_source,
  bass_key,
  bass_scale,
  bass_octave,
  bass_volume,
  chord_source,
  chord_key,
  chord_octave,
  chord_volume,
  drum_volume,
  display_brightness,
  led_brightness,
  language,
};

enum class menu_action_t : uint8_t {
  none,
  kit_load,
  kit_save,
  kit_new,
  kit_reset_builtin,
  kit_assign_wav,
  kit_clear_pad,
  kit_clear_all_pads,
  kit_pad_list,
  background_load,
  background_clear,
  loop_clear,
  loop_save_as_bgm,
  loop_stop,
  input_learn,
  input_assign_list,
  input_clear_all,
  input_source_select,
  reset_ble_connection,
  ble_device_scan,
  ble_device_forget,
  external_tone_select,
  external_pad_select,
  external_pad_base_note_select,
  synth_tone_select,
  synth_pad_select,
  synth_pad_base_note_select,
  wifi_setup,
  wifi_wps,
  wifi_info,
  wifi_update,
  wifi_file_editor,
  system_info,
  reset_all_settings,
};

struct sampler_menu_item_t {
  const char* label;
  menu_item_kind_t kind;
  menu_page_t child;
  menu_value_t value;
  menu_action_t action;
  enum class visibility_t : uint8_t {
    always,
    ble_midi,
  } visibility = visibility_t::always;
};

// The first root item follows the active performance page. This keeps the
// page's primary settings one menu action away without a Synthesizer detour.
static const sampler_menu_item_t* menu_root_items_for_current_page(size_t* count)
{
  static sampler_menu_item_t items[] = {
    { "Sample",          menu_item_kind_t::submenu, menu_page_t::kit,         menu_value_t::none, menu_action_t::none },
    { "Loop",            menu_item_kind_t::submenu, menu_page_t::loop,        menu_value_t::none, menu_action_t::none },
    { "BGM",             menu_item_kind_t::submenu, menu_page_t::loop_bgm,    menu_value_t::none, menu_action_t::none },
    { "Key/Scale",       menu_item_kind_t::submenu, menu_page_t::harmony,     menu_value_t::none, menu_action_t::none },
    { "External Device", menu_item_kind_t::submenu, menu_page_t::connections, menu_value_t::none, menu_action_t::none },
    { "Wi-Fi",           menu_item_kind_t::submenu, menu_page_t::wifi,        menu_value_t::none, menu_action_t::none },
    { "System",          menu_item_kind_t::submenu, menu_page_t::system,      menu_value_t::none, menu_action_t::none },
  };

  switch (current_page) {
  case performance_page_t::bass:
    items[0] = { "Bass", menu_item_kind_t::submenu, menu_page_t::synth_bass, menu_value_t::none, menu_action_t::none };
    break;
  case performance_page_t::melody:
    items[0] = { "Melody", menu_item_kind_t::submenu, menu_page_t::synth_melody, menu_value_t::none, menu_action_t::none };
    break;
  case performance_page_t::chord:
    items[0] = { "Chord", menu_item_kind_t::submenu, menu_page_t::synth_chord, menu_value_t::none, menu_action_t::none };
    break;
  case performance_page_t::drum:
    items[0] = { "Drum", menu_item_kind_t::submenu, menu_page_t::synth_drum, menu_value_t::none, menu_action_t::none };
    break;
  case performance_page_t::sample:
  default:
    items[0] = { "Sample", menu_item_kind_t::submenu, menu_page_t::kit, menu_value_t::none, menu_action_t::none };
    break;
  }

  *count = std::size(items);
  return items;
}

static constexpr const sampler_menu_item_t menu_synthesizer_items[] = {
  { "Bass",   menu_item_kind_t::submenu, menu_page_t::synth_bass,   menu_value_t::none, menu_action_t::none },
  { "Melody", menu_item_kind_t::submenu, menu_page_t::synth_melody, menu_value_t::none, menu_action_t::none },
  { "Chord",  menu_item_kind_t::submenu, menu_page_t::synth_chord,  menu_value_t::none, menu_action_t::none },
  { "Drum",   menu_item_kind_t::submenu, menu_page_t::synth_drum,   menu_value_t::none, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_melody_items[] = {
  { "Sound Source", menu_item_kind_t::submenu, menu_page_t::synth_melody_sound, menu_value_t::none, menu_action_t::none },
  { "Octave",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::melody_octave, menu_action_t::none },
  { "Volume",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::melody_volume, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_melody_sound_items[] = {
  { "General MIDI",  menu_item_kind_t::submenu, menu_page_t::synth_melody_midi, menu_value_t::none, menu_action_t::none },
  { "Pad",           menu_item_kind_t::submenu, menu_page_t::synth_melody_pad,  menu_value_t::none, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_melody_midi_items[] = {
  { "Tone",          menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_tone_select },
};

static constexpr const sampler_menu_item_t menu_synth_melody_pad_items[] = {
  { "Pad Sound",     menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_select },
  { "Pad Base Note", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_base_note_select },
};

static constexpr const sampler_menu_item_t menu_synth_bass_items[] = {
  { "Sound Source", menu_item_kind_t::submenu, menu_page_t::synth_bass_sound, menu_value_t::none, menu_action_t::none },
  { "Octave",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::bass_octave, menu_action_t::none },
  { "Volume",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::bass_volume, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_bass_sound_items[] = {
  { "General MIDI", menu_item_kind_t::submenu, menu_page_t::synth_bass_midi, menu_value_t::none, menu_action_t::none },
  { "Pad",          menu_item_kind_t::submenu, menu_page_t::synth_bass_pad,  menu_value_t::none, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_bass_midi_items[] = {
  { "Tone", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_tone_select },
};

static constexpr const sampler_menu_item_t menu_synth_bass_pad_items[] = {
  { "Pad Sound",     menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_select },
  { "Pad Base Note", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_base_note_select },
};

static constexpr const sampler_menu_item_t menu_synth_chord_items[] = {
  { "Sound Source", menu_item_kind_t::submenu, menu_page_t::synth_chord_sound, menu_value_t::none, menu_action_t::none },
  { "Octave",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::chord_octave, menu_action_t::none },
  { "Volume",       menu_item_kind_t::value,  menu_page_t::root, menu_value_t::chord_volume, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_chord_sound_items[] = {
  { "General MIDI",  menu_item_kind_t::submenu, menu_page_t::synth_chord_midi, menu_value_t::none, menu_action_t::none },
  { "Pad",           menu_item_kind_t::submenu, menu_page_t::synth_chord_pad,  menu_value_t::none, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_synth_chord_midi_items[] = {
  { "Tone",          menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_tone_select },
};

static constexpr const sampler_menu_item_t menu_synth_chord_pad_items[] = {
  { "Pad Sound",     menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_select },
  { "Pad Base Note", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::synth_pad_base_note_select },
};

static constexpr const sampler_menu_item_t menu_synth_drum_items[] = {
  { "Volume", menu_item_kind_t::value, menu_page_t::root, menu_value_t::drum_volume, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_harmony_items[] = {
  { "Key",   menu_item_kind_t::value, menu_page_t::root, menu_value_t::harmony_key,   menu_action_t::none },
  { "Scale", menu_item_kind_t::value, menu_page_t::root, menu_value_t::harmony_scale, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_kit_items[] = {
  { "Load Kit",       menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_load },
  { "Save Kit",       menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_save },
  { "Import Sample",  menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_assign_wav },
  { "File Editor",    menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::wifi_file_editor },
  { "New Kit",        menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_new },
  { "Reset Kit",      menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_reset_builtin },
};

static constexpr const sampler_menu_item_t menu_kit_edit_items[] = {
  { "Import Sample",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_assign_wav },
  { "Clear Pad",      menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_clear_pad },
  { "Clear All Pads", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_clear_all_pads },
  { "Pad List",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_pad_list },
};

static constexpr const sampler_menu_item_t menu_loop_items[] = {
  { "Quantize",      menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_quantize,      menu_action_t::none },
  { "Note Grid",     menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_note_grid,     menu_action_t::none },
  { "Note Off Grid", menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_note_off_grid, menu_action_t::none },
  { "Save as BGM",   menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::loop_save_as_bgm },
  { "Clear Loop",    menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::loop_clear },
};

static constexpr const sampler_menu_item_t menu_loop_bgm_items[] = {
  { "Load BGM",    menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,              menu_action_t::background_load },
  { "Clear BGM",   menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,              menu_action_t::background_clear },
  { "BGM Volume",  menu_item_kind_t::value,  menu_page_t::root, menu_value_t::background_volume, menu_action_t::none },
  { "BGM Repeat",  menu_item_kind_t::value,  menu_page_t::root, menu_value_t::background_repeat, menu_action_t::none },
  { "File Editor", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::wifi_file_editor },
};

static constexpr const sampler_menu_item_t menu_input_items[] = {
  { "Learn",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_learn },
  { "Assign List", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_assign_list },
  { "Clear All",   menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_clear_all },
};

static constexpr const sampler_menu_item_t menu_connections_items[] = {
  { "Input Source",   menu_item_kind_t::submenu, menu_page_t::input_source, menu_value_t::none,                menu_action_t::none },
  { "MIDI Note Action", menu_item_kind_t::value, menu_page_t::root, menu_value_t::midi_note_action,            menu_action_t::none },
  { "Input Assign",   menu_item_kind_t::submenu, menu_page_t::input_assign, menu_value_t::none,                menu_action_t::none },
  { "BLE MIDI Connection", menu_item_kind_t::submenu, menu_page_t::ble_device, menu_value_t::none,
    menu_action_t::none, sampler_menu_item_t::visibility_t::ble_midi },
  { "Device Info",    menu_item_kind_t::submenu, menu_page_t::connection_info, menu_value_t::none,             menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_ble_device_items[] = {
  { "Scan & Connect", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::ble_device_scan },
  { "Forget Device",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::ble_device_forget },
  { "Reset BLE Connection", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::reset_ble_connection },
};

static constexpr const sampler_menu_item_t menu_midi_sound_items[] = {
  { "General MIDI", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::external_tone_select },
  { "Pad",          menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::external_pad_select },
  { "Base Note",    menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::external_pad_base_note_select },
};

static constexpr const sampler_menu_item_t menu_input_source_items[] = {
  { "Off",                 menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
  { "USB MIDI Controller", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
  { "USB MIDI Computer",   menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
  { "USB Keyboard",        menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
  { "BLE MIDI",            menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
  { "USB Gamepad",         menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_source_select },
};

static constexpr const sampler_menu_item_t menu_wifi_items[] = {
  { "Update",      menu_item_kind_t::action,  menu_page_t::root,       menu_value_t::none,             menu_action_t::wifi_update },
  { "Wi-Fi Setup", menu_item_kind_t::submenu, menu_page_t::wifi_setup, menu_value_t::none,             menu_action_t::none },
  { "File Editor", menu_item_kind_t::action,  menu_page_t::root,       menu_value_t::none,             menu_action_t::wifi_file_editor },
  { "Auto Update", menu_item_kind_t::value,   menu_page_t::root,       menu_value_t::wifi_auto_update, menu_action_t::none },
  { "Wi-Fi Info",  menu_item_kind_t::action,  menu_page_t::root,       menu_value_t::none,             menu_action_t::wifi_info },
};

static constexpr const sampler_menu_item_t menu_wifi_setup_items[] = {
  { "Use Smartphone", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::wifi_setup },
  { "WPS",            menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::wifi_wps },
};

static constexpr const sampler_menu_item_t menu_system_items[] = {
  { "Recording Input", menu_item_kind_t::value,  menu_page_t::root, menu_value_t::audio_input_source,  menu_action_t::none },
  { "Menu Sound",      menu_item_kind_t::value,  menu_page_t::root, menu_value_t::menu_sound,          menu_action_t::none },
  { "Display",    menu_item_kind_t::value,  menu_page_t::root, menu_value_t::display_brightness, menu_action_t::none },
  { "LED",        menu_item_kind_t::value,  menu_page_t::root, menu_value_t::led_brightness,     menu_action_t::none },
  { "Language",   menu_item_kind_t::value,  menu_page_t::root, menu_value_t::language,           menu_action_t::none },
  { "Info",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::system_info },
  { "Reset All",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::reset_all_settings },
};

static bool menu_visible = false;
static void service_surface_sync(uint32_t now)
{
  if (surface_sync_deadline_msec == 0
   || (int32_t)(now - surface_sync_deadline_msec) < 0) {
    return;
  }
  const bool stale = rendered_header_page != current_page
                  || rendered_wave_page != current_page
                  || rendered_header_mode != current_mode
                  || rendered_wave_mode != current_mode;
  if (!stale) {
    surface_sync_deadline_msec = 0;
    return;
  }
  if (menu_visible || ui_surface_exclusive || page_selector_visible
   || sound_priority_active(now) || physical_input_pending()
   || ui_async_display_busy()) {
    return;
  }
  // This path is intentionally rare. It re-establishes all retained regions
  // together, so no old page can survive after an async transfer was dropped.
  draw_all();
  update_all_leds();
  surface_sync_deadline_msec = 0;
}

static menu_page_t menu_page = menu_page_t::root;
static uint8_t menu_cursor = 0;
static uint8_t menu_depth = 0;
// The lower keypad is static for ordinary cursor movement.  Redraw it only
// when its role changes (numbers, +/- value controls, Delete, or pad target).
static uint8_t menu_keypad_state = 0xFF;
// NVS writes can briefly stall the UI.  Value changes stay live immediately
// and are committed after the user pauses, rather than for every encoder tick.
static bool menu_settings_save_pending = false;
static uint32_t menu_settings_save_due_msec = 0;
static char status_message[96] = { 0 };
static uint32_t status_message_until = 0;  // 0なら明示的に消すまで表示
static bool status_message_busy = false;
static uint32_t status_message_anim_msec = 0;
static bool wifi_setup_active = false;
// Wi-Fi設定は本体で文字を打たず、スマホへ渡す。AP参加後は同じ領域を
// 設定ページ用QRへ切り替えるため、QR生成は遷移時だけ行う。
static bool wifi_setup_qr_active = false;
// QRはWi-Fi/HTTPの起動要求と同時には表示せず、接続先が実際に使える状態に
// なってから切り替える。スマホが先にURLを開いて失敗するのを防ぐ。
static bool wifi_qr_preparing = false;
static bool wifi_setup_qr_web_page = false;
static bool wifi_setup_qr_dirty = false;
static bool wifi_setup_waiting_for_connection = false;
static uint32_t wifi_setup_connect_deadline_msec = 0;
static bool wifi_setup_is_wps = false;
static bool wifi_file_server_qr_active = false;
static volatile bool wifi_file_server_client_connected = false;
static uint32_t wifi_file_server_connect_deadline_msec = 0;
enum class wifi_radio_request_t : uint8_t { none, setup_ap, setup_wps, ota, file_server, update_check };
static wifi_radio_request_t wifi_radio_request = wifi_radio_request_t::none;
static uint32_t wifi_radio_request_deadline_msec = 0;
static uint32_t wifi_radio_request_not_before_msec = 0;
static bool wifi_ble_suspended = false;
static bool wifi_ble_resume_pending = false;
static uint32_t wifi_ble_resume_not_before_msec = 0;
static bool wifi_auto_update_check = false;
static bool wifi_update_active = false;
static uint8_t wifi_update_last_state = 0;
static uint32_t wifi_update_finished_msec = 0;
static bool wifi_update_overlay_drawn = false;
static bool startup_loading_active = false;
static bool startup_loading_static_drawn = false;
static uint8_t startup_loading_frame = 0;
static bool recording_processing_static_drawn = false;
static uint8_t recording_processing_frame = 0;
static bool processing_screen_visible = false;
// 起動後の更新確認は、保存済みWi-Fiがある機体だけで一度実行する。
static bool startup_update_check_pending = false;
static bool startup_update_check_active = false;
static uint32_t startup_update_check_not_before = 0;
// Restore the SAM2695 programs once more after its UART worker has fully
// settled.  The registry keeps the values, but an early hardware reset can
// otherwise leave the displayed tone and the audible tone out of sync.
static bool startup_synth_restore_pending = false;
static uint32_t startup_synth_restore_not_before = 0;

static void disable_wifi_and_clear_indicator(void);

enum class kit_edit_state_t : uint8_t {
  idle,
  select_kit_file,
  select_kit_save,
  select_wav,
  select_bgm_wav,
  select_external_tone,
  select_external_pad,
  select_external_pad_base_note,
  assign_wait_pad,
  assign_confirm_shortcut,
  clear_wait_pad,
  pad_list,
};
static kit_edit_state_t kit_edit_state = kit_edit_state_t::idle;
static std::vector<kp::file_info_string_t> kit_wav_list;
static char kit_wav_dir[96] = { 0 };
static char kit_pending_wav_path[96] = { 0 };
static char kit_pending_wav_name[40] = { 0 };
// A performance-surface Import already knows its destination, unlike the
// regular Kit menu flow which still asks the player to select a Pad.
static int kit_shortcut_target_pad = -1;
// The selected target remains visible while its source is being imported.
static int kit_assign_loading_pad = -1;
static char sampler_sd_folders[3][80] = { "/sampler/samples", "/sampler/loops", "/sampler/kits" };
static constexpr const char* sampler_default_kit_dir = "/sampler/kits/Default";
static constexpr const char* sampler_default_kit_path = "/sampler/kits/Default/Default_Kit.json";
// SDから読んだ、または直前に保存したKIT。内蔵KIT/新規KITでは空のままにし、
// 意図しない上書き候補を出さない。
static char current_kit_path[96] = {};
struct kit_save_candidate_t {
  char label[48] = {};
  char path[96] = {};
};
static kit_save_candidate_t kit_save_candidates[4];
static uint8_t kit_save_candidate_count = 0;

enum class learn_state_t : uint8_t {
  idle,
  waiting_target,
  waiting_external,
};
static learn_state_t learn_state = learn_state_t::idle;
static char learn_target_label[16] = { 0 };
static uint32_t learn_target_deadline_msec = 0;
static constexpr uint32_t learn_target_timeout_ms = 5000;

enum class midi_assign_target_t : int16_t {
  none = -1,
  pad_base = 0,
  mode_base = 16,
  stop_all = 24,
  // Keep the existing target IDs stable so saved Kits and resume data remain valid.
  fn_base = 25,
};
static int16_t midi_note_assign[128] = { 0 };
static int16_t midi_cc_assign[128] = { 0 };
static int16_t external_button_assign[32] = { 0 };
static int16_t usb_keyboard_assign[256] = { 0 };
static int16_t usb_gamepad_assign[256] = { 0 };
// GM Programは0始まり。画面上の82番 Lead 2 (sawtooth) はProgram 81。
static uint8_t external_midi_ch1_program = 81;
enum class external_midi_sound_t : uint8_t { general_midi, pad };
static external_midi_sound_t external_midi_sound = external_midi_sound_t::general_midi;
static uint8_t external_midi_pad = 0;
// 外部MIDIノートとInput Assignをどう併用するか。CCや外部ボタンは常にAssign専用。
enum class midi_note_action_t : uint8_t { automatic, play, control };
static midi_note_action_t midi_note_action = midi_note_action_t::automatic;
static performance_page_t synth_menu_target = performance_page_t::melody;
// The dynamic Tone/Pad picker is shared by page and External MIDI settings.
// Keep its owner explicit so a page selection can never rewrite the external
// input route (and vice versa).
static bool synth_sound_select_active = false;
enum class external_input_mode_t : uint8_t {
  off,
  usb_midi_host,
  usb_midi_device,
  usb_keyboard,
  ble_midi,
  usb_gamepad,
  max,
};
static external_input_mode_t external_input_mode = external_input_mode_t::off;
// BLE MIDI is a live input path. Existing completed UI caches remain useful,
// but creating or refreshing them copies a large PSRAM region at exactly the
// moment the Bluetooth task may need to receive a burst of notes.
static bool ble_midi_cache_guard_active(void)
{
  return external_input_mode == external_input_mode_t::ble_midi
      && !wifi_ble_suspended;
}
static bool usb_host_disabled_on_boot = false;
#if !defined(M5UNIFIED_PC_BUILD)
static constexpr uint32_t external_input_restart_magic_value = 0x4B50494Du;
RTC_NOINIT_ATTR static uint32_t external_input_restart_magic;
RTC_NOINIT_ATTR static uint32_t external_input_restart_mode;
RTC_NOINIT_ATTR static uint32_t external_input_restart_check;
#endif
static bool usb_keyboard_enabled = false;
static bool usb_gamepad_enabled = false;
// USB-CへPCが給電している間は、同じ端子をUSBホストに切り替えない。
// Host VBUSとPC側のVBUSが衝突すると、通信・給電の双方を失うため。
static bool usb_host_waiting_for_pc_disconnect = false;
// USB Hostのタスクとクライアント受信先が準備できるまで、
// 外部機器へのVBUS供給を遅らせる。古いUSB機器の接続変化を取り逃さないため。
static bool usb_host_vbus_power_pending = false;
enum class ble_connection_reset_state_t : uint8_t { idle, stopping, connecting };
static ble_connection_reset_state_t ble_connection_reset_state = ble_connection_reset_state_t::idle;
static uint32_t ble_connection_reset_deadline_msec = 0;
enum class ble_device_ui_state_t : uint8_t { idle, scanning, list, confirm, connecting };
static ble_device_ui_state_t ble_device_ui_state = ble_device_ui_state_t::idle;
static constexpr size_t ble_device_list_capacity = 12;
static kp::task_midi_t::ble_scan_device_t ble_device_list[ble_device_list_capacity];
static size_t ble_device_count = 0;
static size_t ble_device_selected = 0;
static uint32_t ble_device_connect_deadline_msec = 0;
static char ble_preferred_address[18] = {};
static char ble_preferred_name[24] = {};
static uint8_t midi_note_assign_count = 0;
static uint8_t external_button_assign_count = 0;
static uint8_t usb_keyboard_assign_count = 0;
static uint8_t usb_gamepad_assign_count = 0;
static int16_t learn_target = (int16_t)midi_assign_target_t::none;

enum class input_source_t : uint8_t { midi, midi_cc, external, usb_keyboard, usb_gamepad };
struct input_assignment_entry_t {
  input_source_t source_type = input_source_t::midi;
  uint8_t source = 0;
  int16_t target = (int16_t)midi_assign_target_t::none;
};
static std::vector<input_assignment_entry_t> input_assignment_list;
static bool input_assignment_list_active = false;
static void begin_input_assignment_list(void);
static void delete_selected_input_assignment(void);

struct menu_feedback_note_t {
  bool active = false;
  bool started = false;
  uint8_t channel = 0;
  uint8_t note = 0;
  uint32_t start_msec = 0;
  uint32_t end_msec = 0;
};

static constexpr uint8_t menu_cursor_channel = kp::def::midi::channel_16;
static constexpr uint8_t menu_navigation_channel = kp::def::midi::channel_10;
static constexpr uint8_t menu_feedback_program = 12;  // Marimba (zero-based GM)
static constexpr size_t menu_navigation_feedback_slot_count = 6;
static constexpr size_t menu_cursor_feedback_slot_base = 8;
static constexpr size_t menu_feedback_note_capacity = 12;
static menu_feedback_note_t menu_feedback_notes[menu_feedback_note_capacity];
static bool menu_navigation_feedback_active = false;
static bool menu_sound_enabled = true;

static void restore_drum_channel_volume(void)
{
  const uint8_t value = (uint8_t)std::min<uint32_t>(127, ((uint32_t)drum_volume * 127 + 50) / 100);
  send_sam_midi(0xB0 | menu_navigation_channel, 7, value);
  menu_navigation_feedback_active = false;
}

static void stop_menu_feedback(void)
{
  for (auto& event : menu_feedback_notes) {
    if (event.active && event.started) {
      send_sam_midi(0x80 | event.channel, event.note, 0);
    }
    event = {};
  }
  if (menu_navigation_feedback_active) { restore_drum_channel_volume(); }
}

static void stop_menu_feedback_channel(uint8_t channel)
{
  for (auto& event : menu_feedback_notes) {
    if (!event.active || event.channel != channel) { continue; }
    if (event.started) { send_sam_midi(0x80 | event.channel, event.note, 0); }
    event = {};
  }
  if (channel == menu_navigation_channel && menu_navigation_feedback_active) {
    restore_drum_channel_volume();
  }
}

static void begin_menu_cursor_feedback(void)
{
  // Selecting a row immediately after entering/leaving a page must not cancel
  // the navigation phrase waiting on CH10.
  stop_menu_feedback_channel(menu_cursor_channel);
  // CH16 is reserved by the sampler for UI feedback. Force its setup for each
  // phrase so a preceding reset or external MIDI message cannot silence it.
  send_sam_midi(0xC0 | menu_cursor_channel, menu_feedback_program);
  send_sam_midi(0xB0 | menu_cursor_channel, 7, 110);
  send_sam_midi(0xB0 | menu_cursor_channel, 10, 64);
}

static void begin_menu_navigation_feedback(void)
{
  stop_menu_feedback();
  menu_navigation_feedback_active = true;
  send_sam_midi(0xB0 | menu_navigation_channel, 7, 110);
}

static void schedule_menu_feedback_note(size_t slot, uint8_t channel, uint8_t note,
                                        uint32_t delay_msec, uint32_t duration_msec)
{
  if (slot >= menu_feedback_note_capacity) { return; }
  const uint32_t now = M5.millis();
  menu_feedback_notes[slot] = { true, false, channel, note, now + delay_msec,
                                now + delay_msec + duration_msec };
}

static void play_menu_feedback_note_now(size_t slot, uint8_t channel, uint8_t note,
                                        uint32_t duration_msec)
{
  if (slot >= menu_feedback_note_capacity) { return; }
  const uint32_t now = M5.millis();
  send_sam_midi(0x90 | channel, note, 108);
  menu_feedback_notes[slot] = { true, true, channel, note, now, now + duration_msec };
}

static void service_menu_feedback(uint32_t now)
{
  bool pending = false;
  for (auto& event : menu_feedback_notes) {
    if (!event.active) { continue; }
    pending = true;
    if (!event.started && (int32_t)(now - event.start_msec) >= 0) {
      event.started = true;
      send_sam_midi(0x90 | event.channel, event.note, 108);
    }
    if (event.started && (int32_t)(now - event.end_msec) >= 0) {
      send_sam_midi(0x80 | event.channel, event.note, 0);
      event = {};
    }
  }
  if (pending) {
    pending = false;
    for (const auto& event : menu_feedback_notes) {
      if (event.active) { pending = true; break; }
    }
  }
  if (!pending && menu_navigation_feedback_active) { restore_drum_channel_volume(); }
}

static void menu_sound_cursor(uint8_t display_number)
{
  if (!menu_sound_enabled) { return; }
  static constexpr uint8_t digit_scale[] = { 11, 12, 14, 16, 17, 19, 21, 23, 24, 26 };
  int value = display_number >= 200 ? display_number / 10 : display_number;
  value = std::max(0, std::min(999, value));
  uint8_t digits[3] = {};
  size_t count = 0;
  if (value >= 100) { digits[count++] = value / 100; }
  if (value >= 10) { digits[count++] = (value / 10) % 10; }
  digits[count++] = value % 10;

  begin_menu_cursor_feedback();
  for (size_t i = 0; i < count; ++i) {
    schedule_menu_feedback_note(menu_cursor_feedback_slot_base + i, menu_cursor_channel,
                                48 + digit_scale[digits[i]], i * 80, 70);
  }
}

static void menu_sound_navigate(uint8_t type)
{
  if (!menu_sound_enabled) { return; }
  uint8_t level = menu_depth;
  if (type == 0 || type == 3) { level = 1; }
  if (level < 1) { level = 1; }
  if (level > 6) { level = 6; }
  const uint8_t note = type == 2 ? 42 : type == 3 ? 36 : 37;
  const uint8_t count = (type == 1 || type == 2) ? level : 1;
  begin_menu_navigation_feedback();
  const size_t note_count = std::min<size_t>(count, menu_navigation_feedback_slot_count);
  if (note_count == 0) { return; }
  // The first drum hit confirms the decision at the exact input edge.  Any
  // additional hits describe the new menu depth without delaying feedback.
  play_menu_feedback_note_now(0, menu_navigation_channel, note, 70);
  for (size_t i = 1; i < note_count; ++i) {
    schedule_menu_feedback_note(i, menu_navigation_channel, note, i * 90, 70);
  }
}

static bool synth_source_branch_for_page(menu_page_t page, performance_page_t* target,
                                         synth_tone_source_t* source)
{
  switch (page) {
  case menu_page_t::synth_melody_midi:
    *target = performance_page_t::melody; *source = synth_tone_source_t::general_midi; return true;
  case menu_page_t::synth_melody_pad:
    *target = performance_page_t::melody; *source = synth_tone_source_t::pad; return true;
  case menu_page_t::synth_bass_midi:
    *target = performance_page_t::bass; *source = synth_tone_source_t::general_midi; return true;
  case menu_page_t::synth_bass_pad:
    *target = performance_page_t::bass; *source = synth_tone_source_t::pad; return true;
  case menu_page_t::synth_chord_midi:
    *target = performance_page_t::chord; *source = synth_tone_source_t::general_midi; return true;
  case menu_page_t::synth_chord_pad:
    *target = performance_page_t::chord; *source = synth_tone_source_t::pad; return true;
  default:
    return false;
  }
}

static performance_page_t synth_target_for_menu_page(menu_page_t page)
{
  switch (page) {
  case menu_page_t::synth_bass:
  case menu_page_t::synth_bass_sound:
  case menu_page_t::synth_bass_midi:
  case menu_page_t::synth_bass_pad:
    return performance_page_t::bass;
  case menu_page_t::synth_chord:
  case menu_page_t::synth_chord_sound:
  case menu_page_t::synth_chord_midi:
  case menu_page_t::synth_chord_pad:
    return performance_page_t::chord;
  default:
    return performance_page_t::melody;
  }
}

static void select_synth_source_branch(menu_page_t page)
{
  performance_page_t target;
  synth_tone_source_t source;
  if (!synth_source_branch_for_page(page, &target, &source)) { return; }
  synth_menu_target = target;
  auto& settings = page_settings(target);
  // The old source's release tail may still reference this page's cache.
  // A source change is a deliberate sound replacement, so stop it before the
  // cache is rebuilt rather than overwriting memory underneath a live voice.
  stop_synth_page(target);
  settings.source = source;
  // Page sound sources are independent from External Device > MIDI Sound.
  // In particular, choosing a Pad here must never reconfigure CH1 used by
  // incoming MIDI, or disturb the shared SAM2695 drum/UI channels.
  // A source switch must never inherit the temporary mute used for recording.
  sampler_audio_t::setOutputMuted(false);
  apply_synth_tones(true);
  save_resume_kit();
}

static const sampler_menu_item_t* menu_raw_items(menu_page_t page, size_t* count)
{
  switch (page) {
  default:
  case menu_page_t::root:         return menu_root_items_for_current_page(count);
  case menu_page_t::kit:          *count = sizeof(menu_kit_items) / sizeof(menu_kit_items[0]); return menu_kit_items;
  case menu_page_t::kit_edit:     *count = sizeof(menu_kit_edit_items) / sizeof(menu_kit_edit_items[0]); return menu_kit_edit_items;
  case menu_page_t::loop:         *count = sizeof(menu_loop_items) / sizeof(menu_loop_items[0]); return menu_loop_items;
  case menu_page_t::loop_bgm:     *count = sizeof(menu_loop_bgm_items) / sizeof(menu_loop_bgm_items[0]); return menu_loop_bgm_items;
  case menu_page_t::harmony:      *count = sizeof(menu_harmony_items) / sizeof(menu_harmony_items[0]); return menu_harmony_items;
  case menu_page_t::synthesizer:  *count = sizeof(menu_synthesizer_items) / sizeof(menu_synthesizer_items[0]); return menu_synthesizer_items;
  case menu_page_t::synth_melody: *count = sizeof(menu_synth_melody_items) / sizeof(menu_synth_melody_items[0]); return menu_synth_melody_items;
  case menu_page_t::synth_melody_sound: *count = sizeof(menu_synth_melody_sound_items) / sizeof(menu_synth_melody_sound_items[0]); return menu_synth_melody_sound_items;
  case menu_page_t::synth_melody_midi: *count = sizeof(menu_synth_melody_midi_items) / sizeof(menu_synth_melody_midi_items[0]); return menu_synth_melody_midi_items;
  case menu_page_t::synth_melody_pad: *count = sizeof(menu_synth_melody_pad_items) / sizeof(menu_synth_melody_pad_items[0]); return menu_synth_melody_pad_items;
  case menu_page_t::synth_bass: *count = sizeof(menu_synth_bass_items) / sizeof(menu_synth_bass_items[0]); return menu_synth_bass_items;
  case menu_page_t::synth_bass_sound: *count = sizeof(menu_synth_bass_sound_items) / sizeof(menu_synth_bass_sound_items[0]); return menu_synth_bass_sound_items;
  case menu_page_t::synth_bass_midi: *count = sizeof(menu_synth_bass_midi_items) / sizeof(menu_synth_bass_midi_items[0]); return menu_synth_bass_midi_items;
  case menu_page_t::synth_bass_pad: *count = sizeof(menu_synth_bass_pad_items) / sizeof(menu_synth_bass_pad_items[0]); return menu_synth_bass_pad_items;
  case menu_page_t::synth_chord:  *count = sizeof(menu_synth_chord_items) / sizeof(menu_synth_chord_items[0]); return menu_synth_chord_items;
  case menu_page_t::synth_chord_sound: *count = sizeof(menu_synth_chord_sound_items) / sizeof(menu_synth_chord_sound_items[0]); return menu_synth_chord_sound_items;
  case menu_page_t::synth_chord_midi: *count = sizeof(menu_synth_chord_midi_items) / sizeof(menu_synth_chord_midi_items[0]); return menu_synth_chord_midi_items;
  case menu_page_t::synth_chord_pad: *count = sizeof(menu_synth_chord_pad_items) / sizeof(menu_synth_chord_pad_items[0]); return menu_synth_chord_pad_items;
  case menu_page_t::synth_drum:   *count = sizeof(menu_synth_drum_items) / sizeof(menu_synth_drum_items[0]); return menu_synth_drum_items;
  case menu_page_t::input_assign: *count = sizeof(menu_input_items) / sizeof(menu_input_items[0]); return menu_input_items;
  case menu_page_t::connections:  *count = sizeof(menu_connections_items) / sizeof(menu_connections_items[0]); return menu_connections_items;
  case menu_page_t::midi_sound:   *count = sizeof(menu_midi_sound_items) / sizeof(menu_midi_sound_items[0]); return menu_midi_sound_items;
  case menu_page_t::ble_device:   *count = sizeof(menu_ble_device_items) / sizeof(menu_ble_device_items[0]); return menu_ble_device_items;
  case menu_page_t::connection_info: *count = 0; return nullptr;
  case menu_page_t::input_source: *count = sizeof(menu_input_source_items) / sizeof(menu_input_source_items[0]); return menu_input_source_items;
  case menu_page_t::wifi:         *count = sizeof(menu_wifi_items) / sizeof(menu_wifi_items[0]); return menu_wifi_items;
  case menu_page_t::wifi_setup:   *count = sizeof(menu_wifi_setup_items) / sizeof(menu_wifi_setup_items[0]); return menu_wifi_setup_items;
  case menu_page_t::system:       *count = sizeof(menu_system_items) / sizeof(menu_system_items[0]); return menu_system_items;
  }
}

static bool menu_item_is_visible(const sampler_menu_item_t& item)
{
  switch (item.visibility) {
  default:
  case sampler_menu_item_t::visibility_t::always:
    return true;
  case sampler_menu_item_t::visibility_t::ble_midi:
    return external_input_mode == external_input_mode_t::ble_midi;
  }
}

// All menu operations use this filtered list, so drawing, encoder movement and
// numeric shortcuts always share the same visible numbering.
static const sampler_menu_item_t* menu_items(menu_page_t page, size_t* count)
{
  size_t raw_count = 0;
  const auto* raw_items = menu_raw_items(page, &raw_count);
  static sampler_menu_item_t visible_items[16];
  size_t visible_count = 0;
  for (size_t i = 0; i < raw_count && visible_count < std::size(visible_items); ++i) {
    if (menu_item_is_visible(raw_items[i])) {
      visible_items[visible_count++] = raw_items[i];
    }
  }
  *count = visible_count;
  return visible_count == 0 ? nullptr : visible_items;
}

static const char* menu_page_title(menu_page_t page)
{
  switch (page) {
  default:
  case menu_page_t::root: return "Menu";
  case menu_page_t::kit: return "Kit";
  case menu_page_t::kit_edit: return "Edit Pad";
  case menu_page_t::loop: return "Loop";
  case menu_page_t::loop_bgm: return "BGM";
  case menu_page_t::harmony: return "Key/Scale";
  case menu_page_t::synthesizer: return "Synthesizer";
  case menu_page_t::synth_melody: return "Melody";
  case menu_page_t::synth_melody_sound: return "Melody Sound Source";
  case menu_page_t::synth_melody_midi: return "Melody General MIDI";
  case menu_page_t::synth_melody_pad: return "Melody Pad";
  case menu_page_t::synth_bass: return "Bass";
  case menu_page_t::synth_bass_sound: return "Bass Sound Source";
  case menu_page_t::synth_bass_midi: return "Bass General MIDI";
  case menu_page_t::synth_bass_pad: return "Bass Pad";
  case menu_page_t::synth_chord: return "Chord";
  case menu_page_t::synth_chord_sound: return "Chord Sound Source";
  case menu_page_t::synth_chord_midi: return "Chord General MIDI";
  case menu_page_t::synth_chord_pad: return "Chord Pad";
  case menu_page_t::synth_drum: return "Drum";
  case menu_page_t::input_assign: return "Input Assign";
  case menu_page_t::connections: return "External Device";
  case menu_page_t::midi_sound: return "MIDI Sound";
  case menu_page_t::ble_device: return "BLE MIDI Connection";
  case menu_page_t::connection_info: return "Connected Device Info";
  case menu_page_t::input_source: return "Input Source";
  case menu_page_t::wifi: return "Wi-Fi";
  case menu_page_t::wifi_setup: return "Wi-Fi Setup";
  case menu_page_t::system: return "System";
  }
}

static menu_page_t menu_parent_page(menu_page_t page)
{
  switch (page) {
  case menu_page_t::kit_edit: return menu_page_t::kit;
  case menu_page_t::loop_bgm: return menu_page_t::root;
  case menu_page_t::harmony: return menu_page_t::root;
  case menu_page_t::synth_melody_sound: return menu_page_t::synth_melody;
  case menu_page_t::synth_melody_midi:
  case menu_page_t::synth_melody_pad: return menu_page_t::synth_melody_sound;
  case menu_page_t::synth_bass_sound: return menu_page_t::synth_bass;
  case menu_page_t::synth_bass_midi:
  case menu_page_t::synth_bass_pad: return menu_page_t::synth_bass_sound;
  case menu_page_t::synth_chord_sound: return menu_page_t::synth_chord;
  case menu_page_t::synth_chord_midi:
  case menu_page_t::synth_chord_pad: return menu_page_t::synth_chord_sound;
  case menu_page_t::synth_melody:
  case menu_page_t::synth_bass:
  case menu_page_t::synth_chord:
  case menu_page_t::synth_drum: return menu_page_t::root;
  case menu_page_t::input_assign: return menu_page_t::connections;
  case menu_page_t::input_source: return menu_page_t::connections;
  case menu_page_t::midi_sound: return menu_page_t::connections;
  case menu_page_t::ble_device: return menu_page_t::connections;
  case menu_page_t::connection_info: return menu_page_t::connections;
  case menu_page_t::wifi_setup: return menu_page_t::wifi;
  case menu_page_t::kit:
  case menu_page_t::loop:
  case menu_page_t::synthesizer:
  case menu_page_t::connections:
  case menu_page_t::wifi:
  case menu_page_t::system:
  default:
    return menu_page_t::root;
  }
}

static uint8_t menu_parent_cursor(menu_page_t page)
{
  const menu_page_t parent = menu_parent_page(page);
  size_t count = 0;
  const auto* items = menu_items(parent, &count);
  for (size_t i = 0; i < count; ++i) {
    if (items[i].kind == menu_item_kind_t::submenu && items[i].child == page) {
      return (uint8_t)i;
    }
  }

  // Dynamic editors do not have a dedicated submenu node.
  switch (page) {
  case menu_page_t::kit_edit: return 2;
  default: return 0;
  }
}

static uint8_t menu_page_depth(menu_page_t page)
{
  switch (page) {
  case menu_page_t::root: return 0;
  case menu_page_t::kit_edit:
  case menu_page_t::input_assign:
  case menu_page_t::input_source:
  case menu_page_t::midi_sound:
  case menu_page_t::ble_device:
  case menu_page_t::connection_info:
  case menu_page_t::wifi_setup: return 2;
  case menu_page_t::synth_melody_sound:
  case menu_page_t::synth_bass_sound:
  case menu_page_t::synth_chord_sound: return 3;
  case menu_page_t::synth_melody_midi:
  case menu_page_t::synth_melody_pad:
  case menu_page_t::synth_bass_midi:
  case menu_page_t::synth_bass_pad:
  case menu_page_t::synth_chord_midi:
  case menu_page_t::synth_chord_pad: return 4;
  default: return 1;
  }
}

static uint8_t menu_dynamic_depth(void)
{
  if (input_assignment_list_active) { return 3; }
  if (ble_device_ui_state == ble_device_ui_state_t::list
   || ble_device_ui_state == ble_device_ui_state_t::confirm) { return 3; }
  switch (kit_edit_state) {
  case kit_edit_state_t::select_wav:
  case kit_edit_state_t::assign_wait_pad:
    return 2;
  case kit_edit_state_t::clear_wait_pad:
  case kit_edit_state_t::pad_list:
    return 3;
  case kit_edit_state_t::select_kit_file:
  case kit_edit_state_t::select_kit_save:
  case kit_edit_state_t::select_bgm_wav:
    return 2;
  case kit_edit_state_t::select_external_tone:
  case kit_edit_state_t::select_external_pad:
  case kit_edit_state_t::select_external_pad_base_note:
    return menu_page_depth(menu_page) + 1;
  default:
    return menu_page_depth(menu_page);
  }
}

static void apply_external_midi_ch1_tone(void)
{
  auto reg = kp::system_registry;
  reg->midi_out_control.setChannelVolume(kp::def::midi::channel_1, 127);
  reg->midi_out_control.setProgramChange(kp::def::midi::channel_1, external_midi_ch1_program);
}

static void apply_external_input_mode(void)
{
  using namespace kp::def::command;
  auto reg = kp::system_registry;
  // Port Aの拡張ボタンとPort C MIDIは常時受信する。主入力だけを択一にする。
  reg->midi_port_setting.setPortCMIDI(midi_input);
  reg->midi_port_setting.setUSBMIDI(midi_off);
  reg->midi_port_setting.setBLEMIDI(midi_off);
  reg->midi_port_setting.setUSBPowerEnabled(false);
  // HostからMac/PC接続へ戻す際は、I2C更新周期を待たずにVBUSを止める。
  // USB-Cの給電競合でUSB-Serial/JTAGが消えるのを防ぐ。
  M5.Power.setUsbOutput(false);
#if !defined(M5UNIFIED_PC_BUILD)
  // 出力停止直後は自身のVBUS残電圧が残る。ここで判定すると外部給電と
  // 誤認して、バスパワー機器へ給電しないままUSBホストを開始してしまう。
  M5.delay(80);
#endif
  usb_keyboard_enabled = false;
  usb_gamepad_enabled = false;
  usb_host_waiting_for_pc_disconnect = false;
  usb_host_vbus_power_pending = false;

  // 外部電源付きOTG/Yケーブルでは、VBUSが高くてもPCではなく補助電源のことがある。
  // USBホストを明示的に選んだ場合は通信を開始し、VBUS出力だけを電圧に応じて抑制する。
  const bool external_vbus_present = M5.Power.getVBUSVoltage() > 4000;

  switch (external_input_mode) {
  case external_input_mode_t::usb_midi_host:
    reg->midi_port_setting.setUSBMode(usb_host);
    // 外部5Vがあれば給電はそちらへ任せる。バスパワー時は
    // USB Hostの受信準備完了後にservice側から実VBUSを立ち上げる。
    usb_host_vbus_power_pending = !external_vbus_present;
    reg->midi_port_setting.setUSBMIDI(midi_input);
    break;
  case external_input_mode_t::usb_midi_device:
    reg->midi_port_setting.setUSBMode(usb_device);
    reg->midi_port_setting.setUSBMIDI(midi_input);
    break;
  case external_input_mode_t::usb_keyboard:
    reg->midi_port_setting.setUSBMode(usb_host);
    usb_host_vbus_power_pending = !external_vbus_present;
    usb_keyboard_enabled = task_midi.startUSBHIDKeyboard();
    break;
  case external_input_mode_t::usb_gamepad:
    reg->midi_port_setting.setUSBMode(usb_host);
    usb_host_vbus_power_pending = !external_vbus_present;
    usb_gamepad_enabled = task_midi.startUSBHIDGamepad();
    break;
  case external_input_mode_t::ble_midi:
    // BLE利用中もUSB-Cは常にPC接続用のデバイスとして待機させる。
    // 直前のUSBホスト設定が残ると、MacからUSB-Serial/JTAGが見えなくなる。
    reg->midi_port_setting.setUSBMode(usb_device);
    task_midi.setBLEMidiPreferredDevice(ble_preferred_address, ble_preferred_name);
    reg->midi_port_setting.setBLEMIDI(midi_input);
    break;
  case external_input_mode_t::off:
  default:
    // 外部入力を使わない時も、開発・充電用のPC接続を優先する。
    reg->midi_port_setting.setUSBMode(usb_device);
    break;
  }
  // Do not apply the legacy external-MIDI Ch1 tone here. Melody also owns
  // Ch1, so doing this during boot overwrote the tone restored from the Kit.
}

static void service_usb_host_after_pc_disconnect(uint32_t now)
{
  static uint32_t next_check = 0;
  if (!usb_host_waiting_for_pc_disconnect || now < next_check) { return; }
  next_check = now + 250;
  if (M5.Power.getVBUSVoltage() > 4000) { return; }

  // The host stack has not been started while the PC owned USB-C.  It is now
  // safe to enable the selected controller or keyboard without a reboot.
  apply_external_input_mode();
}

static void service_usb_host_vbus_power(void)
{
  if (!usb_host_vbus_power_pending) { return; }
  if (external_input_mode != external_input_mode_t::usb_midi_host
      && external_input_mode != external_input_mode_t::usb_keyboard
      && external_input_mode != external_input_mode_t::usb_gamepad) {
    usb_host_vbus_power_pending = false;
    return;
  }
  if (!task_midi.isUSBStackReady()) { return; }

  // task_i2cに供給を任せることで、PMICへのI2Cアクセスと
  // USBホスト初期化の競合を避ける。ここで初めて機器が起動する。
  kp::system_registry->midi_port_setting.setUSBPowerEnabled(true);
  usb_host_vbus_power_pending = false;
}

static void service_ble_connection_reset(uint32_t now)
{
  using namespace kp::def::command;
  if (ble_connection_reset_state == ble_connection_reset_state_t::idle) { return; }

  bool central = false;
  bool peripheral = false;
  uint8_t subscription = 0;
  task_midi.getBLEMidiConnectionDiagnostic(&central, &peripheral, &subscription);
  (void)subscription;
  (void)subscription;

  if (ble_connection_reset_state == ble_connection_reset_state_t::stopping) {
    if ((central || peripheral) && (int32_t)(now - ble_connection_reset_deadline_msec) < 0) { return; }
    kp::system_registry->midi_port_setting.setBLEMIDI(midi_input);
    ble_connection_reset_state = ble_connection_reset_state_t::connecting;
    ble_connection_reset_deadline_msec = now + 12000;
    return;
  }

  if (central || peripheral) {
    ble_connection_reset_state = ble_connection_reset_state_t::idle;
    show_status_message("BLE connected", 1800, false);
    if (menu_visible) { draw_menu(true); }
  } else if ((int32_t)(now - ble_connection_reset_deadline_msec) >= 0) {
    ble_connection_reset_state = ble_connection_reset_state_t::idle;
    show_status_message("BLE connection failed", 2200, false);
    if (menu_visible) { draw_menu(true); }
  }
}

static bool external_input_usb_mode(external_input_mode_t mode,
                                    kp::def::command::usb_mode_t* usb_mode)
{
  using namespace kp::def::command;
  switch (mode) {
  case external_input_mode_t::usb_midi_host:
  case external_input_mode_t::usb_keyboard:
  case external_input_mode_t::usb_gamepad:
    *usb_mode = usb_host;
    return true;
  case external_input_mode_t::usb_midi_device:
    *usb_mode = usb_device;
    return true;
  default:
    return false;
  }
}

static bool external_input_mode_needs_restart(external_input_mode_t next)
{
  kp::def::command::usb_mode_t next_usb_mode;
  if (!task_midi.isUSBStarted()) { return false; }

  const bool next_uses_usb = external_input_usb_mode(next, &next_usb_mode);
  const auto current_usb_mode = task_midi.getUSBMode();

  // Both the ESP USB Host stack and the TinyUSB device stack remain resident
  // after begin().  Neither can be safely dismantled in place, so every
  // transition away from an already-started USB mode must reboot.  Leaving a
  // host session without this kept the USB controller alive behind BLE/Off
  // and could make the USB-Serial/JTAG development port disappear.
  if (!next_uses_usb || next_usb_mode != current_usb_mode) { return true; }
  // USBホストは列挙時に受信するクラスを固定するため、MIDI/Keyboard/Gamepad
  // 間の切替は同じHostロールでも再起動して受信先を作り直す。
  return next_usb_mode == kp::def::command::usb_mode_t::usb_host
      && next != external_input_mode;
}

static void mark_external_input_restart(external_input_mode_t mode)
{
#if !defined(M5UNIFIED_PC_BUILD)
  external_input_restart_mode = (uint32_t)mode;
  external_input_restart_check = external_input_restart_magic_value
                               ^ external_input_restart_mode ^ 0xA55A3CC3u;
  external_input_restart_magic = external_input_restart_magic_value;
#else
  (void)mode;
#endif
}

static bool consume_external_input_restart(void)
{
#if !defined(M5UNIFIED_PC_BUILD)
  const uint32_t magic = external_input_restart_magic;
  const uint32_t mode = external_input_restart_mode;
  const uint32_t check = external_input_restart_check;
  external_input_restart_magic = 0;
  external_input_restart_mode = 0;
  external_input_restart_check = 0;
  if (magic != external_input_restart_magic_value
   || check != (external_input_restart_magic_value ^ mode ^ 0xA55A3CC3u)
   || mode >= (uint32_t)external_input_mode_t::max) { return false; }
  external_input_mode = (external_input_mode_t)mode;
  return true;
#else
  return false;
#endif
}

static void restart_for_external_input_mode(void)
{
  // 実行済みUSBスタックのHost/Device切替だけは再構築が必要。
  save_resume_kit();
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(0x08080Cu);
  d.drawRect(0, 0, d.width(), d.height(), 0x40A0FFu);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextSize(1, 2);
  d.setTextColor(TFT_WHITE, 0x08080Cu);
  d.drawString("APPLYING INPUT", d.width() / 2, d.height() / 2 - 12);
  d.setTextSize(1, 1);
  d.setTextColor(0xA0D0FFu, 0x08080Cu);
  d.drawString("Restarting...", d.width() / 2, d.height() / 2 + 22);
  d.endWrite();
#if defined(M5UNIFIED_PC_BUILD)
  apply_external_input_mode();
#else
  // Do not leave the PMIC driving host VBUS while the USB role is rebuilt.
  // This also makes reconnecting a computer after a host session reliable.
  kp::system_registry->midi_port_setting.setUSBPowerEnabled(false);
  M5.Power.setUsbOutput(false);
  // この再起動だけは選択した入力先を次回起動へ引き継ぐ。マーカーは起動時に
  // 即座に消費されるため、その後の電源再投入やリセットではHostを自動解除できる。
  mark_external_input_restart(external_input_mode);
  // VBUSの放電とUSB-C CC状態の更新を待ってから再起動する。これが短いと
  // 最初の再起動だけMacがUSB-Serial/JTAGとして再列挙しないことがある。
  M5.delay(750);
  esp_restart();
#endif
}

static void set_external_input_mode(external_input_mode_t next)
{
  const bool needs_restart = external_input_mode_needs_restart(next);
  external_input_mode = next;
  if (needs_restart) {
    restart_for_external_input_mode();
    return;
  }
  apply_external_input_mode();
  // 再起動しない切替も、次回起動時に同じ入力ソースを復元する。
  save_resume_kit();
}

static void apply_wifi_radio_request(wifi_radio_request_t request)
{
  using namespace kp::def::command;
  auto reg = kp::system_registry;
  if (reg == nullptr) { return; }
  switch (request) {
  case wifi_radio_request_t::setup_ap:
    reg->wifi_control.setWebServerMode(webserver_mode_t::ws_disable);
    reg->wifi_control.setWifiMode(wifi_mode_t::wifi_enable_ap);
    reg->wifi_control.setOperation(wifi_operation_t::wfop_setup_ap);
    break;
  case wifi_radio_request_t::setup_wps:
    reg->wifi_control.setWebServerMode(webserver_mode_t::ws_disable);
    reg->wifi_control.setWifiMode(wifi_mode_t::wifi_enable_sta);
    reg->wifi_control.setOperation(wifi_operation_t::wfop_setup_wps);
    break;
  case wifi_radio_request_t::ota:
    reg->wifi_control.setWebServerMode(webserver_mode_t::ws_disable);
    reg->wifi_control.setWifiMode(wifi_mode_t::wifi_enable_sta);
    reg->wifi_control.setOperation(wifi_operation_t::wfop_ota_begin);
    break;
  case wifi_radio_request_t::file_server:
    reg->wifi_control.setWebServerMode(webserver_mode_t::ws_disable);
    reg->wifi_control.setWifiMode(wifi_mode_t::wifi_enable_sta);
    reg->wifi_control.setOperation(wifi_operation_t::wfop_web_filer);
    break;
  case wifi_radio_request_t::update_check:
    reg->wifi_control.setWebServerMode(webserver_mode_t::ws_disable);
    reg->wifi_control.setWifiMode(wifi_mode_t::wifi_enable_sta);
    reg->wifi_control.setOperation(wifi_operation_t::wfop_update_check_begin);
    break;
  case wifi_radio_request_t::none:
  default:
    break;
  }
}

static void update_ui_memory_metrics(void)
{
#if !defined(M5UNIFIED_PC_BUILD)
  ui_render_metrics.psram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ui_render_metrics.psram_largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
#endif
}

static void release_ui_dirty_canvases(void)
{
  // These two buffers are DMA-capable internal RAM.  They are normally kept
  // alive to make Pad/Fn updates inexpensive, but TLS needs a large contiguous
  // internal allocation while Wi-Fi is active.
  wait_ui_dirty_transfers();
  ui_dirty_renderer_ready = false;
  ui_dirty_canvas_index = 0;
  for (uint8_t i = 0; i < 2; ++i) {
    ui_dirty_canvas[i].deleteSprite();
    if (ui_dirty_buffer[i] != nullptr) {
      m5gfx::heap_free(ui_dirty_buffer[i]);
      ui_dirty_buffer[i] = nullptr;
    }
    ui_dirty_canvas_busy[i] = false;
  }
}

static void create_ui_dirty_canvases(void)
{
  bool ready = true;
  for (uint8_t i = 0; i < 2; ++i) {
    if (ui_dirty_buffer[i] == nullptr) {
      ui_dirty_buffer[i] = (uint16_t*)m5gfx::heap_alloc_dma(
        pad_w * cell_h * sizeof(uint16_t));
    }
    if (ui_dirty_buffer[i] == nullptr) {
      ready = false;
      continue;
    }
    ui_dirty_canvas[i].setBuffer(ui_dirty_buffer[i], pad_w, cell_h, 16);
    ui_dirty_canvas[i].setFont(&fonts::efontJA_16_b);
    ui_dirty_canvas_busy[i] = false;
  }
  ui_dirty_canvas_index = 0;
  ui_dirty_renderer_ready = ready;
}

static void suspend_performance_ui_arena(void)
{
  performance_ui_arena_resume_pending = false;
  if (performance_ui_arena_suspended) { return; }
  performance_ui_arena_suspended = true;

  // Wi-Fi owns the foreground. Release every retained UI surface, including
  // the two internal-RAM primary canvases, before TLS allocates its context.
  // The normal UI is rebuilt only after the radio and HTTP client are gone.
  wait_wave_transfer_job();
  wave_canvas.deleteSprite();
  menu_canvas.deleteSprite();
  page_selector_visible = false;
  page_selector_restore_pending = false;
  page_selector_canvas.deleteSprite();
  page_selector_canvas_ready = false;
  touch_play_surface_canvas.deleteSprite();
  touch_play_surface_canvas_ready = false;
  touch_play_surface_cache_key = 0xFF;
  touch_play_surface_cache_scale = 0xFF;
  menu_transition_canvas.deleteSprite();
  menu_transition_canvas_ready = false;
  release_ui_dirty_canvases();
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    grid_cache_canvas[i].deleteSprite();
    grid_cache_ready[i] = false;
    grid_cache_busy[i] = false;
    grid_cache_pad_mask[i] = 0;
    grid_cache_fn_mask[i] = 0;
    grid_cache_page[i] = performance_page_t::max;
  }

  // Recording is disabled throughout Wi-Fi operations. Recreate this large
  // PSRAM buffer lazily on the next microphone recording instead of retaining
  // roughly 1.5 MB during OTA or File Editor startup.
  if (recording_pad < 0 && recording_buffer != nullptr) {
    free(recording_buffer);
    recording_buffer = nullptr;
    recording_frames = 0;
  }
  clear_menu_preview();
  update_ui_memory_metrics();
}

static void request_performance_ui_arena_resume(void)
{
  if (!performance_ui_arena_suspended) { return; }
  // Wi-Fi shutdown is asynchronous. Do not recreate PSRAM canvases while the
  // network stack is still releasing its own large allocations.
  performance_ui_arena_resume_pending = true;
  performance_ui_arena_resume_msec = M5.millis() + 1000;
}

static void service_performance_ui_arena(uint32_t now)
{
  if (!performance_ui_arena_resume_pending
   || (int32_t)(now - performance_ui_arena_resume_msec) < 0) { return; }
  performance_ui_arena_resume_pending = false;

  wave_canvas.setColorDepth(16);
  wave_canvas.createSprite(M5.Display.width(), wave_h);
  menu_canvas.setPsram(true);
  menu_canvas.setColorDepth(16);
  menu_canvas.createSprite(M5.Display.width(), menu_area_h);

  create_ui_dirty_canvases();

  menu_transition_canvas.setPsram(true);
  menu_transition_canvas.setColorDepth(16);
  menu_transition_canvas.createSprite(M5.Display.width(), menu_area_h);
  menu_transition_canvas_ready = menu_transition_canvas.getBuffer() != nullptr;

  page_selector_canvas.setPsram(true);
  page_selector_canvas.setColorDepth(16);
  page_selector_canvas.createSprite(page_selector_w, page_selector_h);
  page_selector_canvas_ready = page_selector_canvas.getBuffer() != nullptr;
  touch_play_surface_canvas.setPsram(true);
  touch_play_surface_canvas.setColorDepth(16);
  touch_play_surface_canvas.createSprite(M5.Display.width(), M5.Display.height());
  touch_play_surface_canvas_ready = touch_play_surface_canvas.getBuffer() != nullptr;
  touch_play_surface_cache_key = 0xFF;
  touch_play_surface_cache_scale = 0xFF;
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    grid_cache_canvas[i].setPsram(true);
    grid_cache_canvas[i].setColorDepth(16);
    grid_cache_canvas[i].createSprite(grid_cache_w, grid_cache_h);
    grid_cache_ready[i] = grid_cache_canvas[i].getBuffer() != nullptr;
    if (grid_cache_ready[i]) { reset_grid_cache(i); }
  }
  performance_ui_arena_suspended = false;
  update_ui_memory_metrics();
  // Recompose only after every normal sprite has a valid backing buffer.
  // This prevents a stale Wi-Fi screen from surviving the cache hand-off.
  if (!ui_surface_exclusive) {
    draw_all();
    // Wi-Fi Setup can finish while its parent menu remains open. The arena
    // resume is deliberately delayed for TLS cleanup, so restoring only the
    // performance grid here overwrote the menu keypad about one second after
    // a successful setup. Repaint the active menu last; its keypad also
    // restores the matching LED palette.
    if (menu_visible) { draw_menu(true); }
    else { update_all_leds(); }
  } else {
    request_header_draw();
    request_wave_draw();
    request_grid_draw();
  }
}

static void draw_wifi_compact_status(wifi_radio_request_t request)
{
  const char* title = request == wifi_radio_request_t::update_check
    ? "CHECKING UPDATES" : "PREPARING WI-FI";
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(0x08080Cu);
  d.drawRect(0, 0, d.width(), d.height(), 0x4080A0u);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextSize(1, 2);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(title, d.width() / 2, d.height() / 2 - 10);
  d.setTextSize(1, 1);
  d.setTextColor(0x90B8CCu, 0x08080Cu);
  d.drawString("Wi-Fi is starting", d.width() / 2, d.height() / 2 + 22);
  d.endWrite();
}

static void begin_wifi_radio_request(wifi_radio_request_t request)
{
  using namespace kp::def::command;
  auto reg = kp::system_registry;
  if (reg == nullptr) { return; }
  suspend_performance_ui_arena();
  draw_wifi_compact_status(request);
  // 新しいWi-Fi要求が来た場合は、前セッションのBLE復帰予約を取り消す。
  wifi_ble_resume_pending = false;
  // BLE MIDIはWi-Fiとの同時利用を目的にしない。AP/STAを始める前に停止して
  // Bluetoothコントローラを解放し、設定済みのBLE入力はセッション後に復帰する。
  if (external_input_mode == external_input_mode_t::ble_midi && !wifi_ble_suspended) {
    wifi_ble_suspended = true;
    wifi_radio_request = request;
    wifi_radio_request_deadline_msec = M5.millis() + 3000;
    wifi_radio_request_not_before_msec = 0;
    reg->midi_port_setting.setBLEMIDI(midi_off);
    return;
  }
  wifi_radio_request = wifi_radio_request_t::none;
  wifi_radio_request_deadline_msec = 0;
  wifi_radio_request_not_before_msec = 0;
  apply_wifi_radio_request(request);
}

static void service_wifi_radio_start(void)
{
  if (wifi_radio_request == wifi_radio_request_t::none || kp::system_registry == nullptr) { return; }
  const uint32_t now = M5.millis();
  const bool ble_stopped = kp::system_registry->runtime_info.getMidiPortStateBLE()
                        == kp::def::command::midiport_info_t::mp_off;
  if (!ble_stopped) {
    if ((int32_t)(now - wifi_radio_request_deadline_msec) < 0) { return; }
    // Bluetoothを解放できない状態でWi-Fiを重ねると内部RAM不足になり得る。
    // 要求を安全に失敗させ、Wi-Fiドライバは起動しない。
    const wifi_radio_request_t failed_request = wifi_radio_request;
    wifi_radio_request = wifi_radio_request_t::none;
    wifi_radio_request_deadline_msec = 0;
    wifi_radio_request_not_before_msec = 0;
    if (failed_request == wifi_radio_request_t::ota
     || failed_request == wifi_radio_request_t::update_check) {
      kp::system_registry->runtime_info.setWiFiOtaProgress(
        kp::def::command::wifi_ota_state_t::ota_wifi_connection_error);
    }
    disable_wifi_and_clear_indicator();
    return;
  }
  // BLEDevice::deinit()直後はBluetoothタスクの後処理が残る。Wi-Fiの大きな
  // 内部RAM確保と重ならないよう、停止通知後も短時間だけ安定を待つ。
  if (wifi_radio_request_not_before_msec == 0) {
    wifi_radio_request_not_before_msec = now + 350;
    return;
  }
  if ((int32_t)(now - wifi_radio_request_not_before_msec) < 0) { return; }
  const wifi_radio_request_t request = wifi_radio_request;
  wifi_radio_request = wifi_radio_request_t::none;
  wifi_radio_request_deadline_msec = 0;
  wifi_radio_request_not_before_msec = 0;
  apply_wifi_radio_request(request);
}

static int menu_value_count(menu_value_t value)
{
  switch (value) {
  case menu_value_t::loop_quantize:
  case menu_value_t::usb_host_power:
  case menu_value_t::usb_keyboard:
  case menu_value_t::wifi_auto_update:
  case menu_value_t::menu_sound:
  case menu_value_t::language:
  case menu_value_t::melody_source:
  case menu_value_t::melody_key_follow:
  case menu_value_t::bass_source:
  case menu_value_t::chord_source:
    return 2;
  case menu_value_t::external_input_mode:
    return (int)external_input_mode_t::max;
  case menu_value_t::midi_note_action:
    return 3;
  case menu_value_t::loop_note_grid:
  case menu_value_t::loop_note_off_grid:
  case menu_value_t::midi_input:
  case menu_value_t::display_brightness:
  case menu_value_t::led_brightness:
  case menu_value_t::melody_octave:
  case menu_value_t::bass_octave:
  case menu_value_t::chord_octave:
    return 5;
  case menu_value_t::melody_scale:
  case menu_value_t::bass_scale:
  case menu_value_t::harmony_scale:
    return sampler_scale_count;
  case menu_value_t::background_volume:
    return volume_20_percent_step_count;
  case menu_value_t::melody_volume:
  case menu_value_t::bass_volume:
  case menu_value_t::chord_volume:
  case menu_value_t::drum_volume:
    return synth_volume_step_count;
  case menu_value_t::melody_key:
  case menu_value_t::bass_key:
  case menu_value_t::chord_key:
  case menu_value_t::harmony_key:
    return 12;
  case menu_value_t::background_repeat:
    return 3;
  case menu_value_t::usb_mode:
    return 2;
  case menu_value_t::audio_input_source:
    return 3;
  default:
    return 0;
  }
}

static int menu_value_get(menu_value_t value)
{
  auto reg = kp::system_registry;
  switch (value) {
  case menu_value_t::loop_quantize: return loop_quantize_enabled ? 1 : 0;
  case menu_value_t::external_input_mode: return (int)external_input_mode;
  case menu_value_t::midi_note_action: return (int)midi_note_action;
  case menu_value_t::loop_note_grid: return loop_quantize_option_index;
  case menu_value_t::loop_note_off_grid: return loop_note_off_quantize_option_index;
  case menu_value_t::background_volume:
    return volume_20_percent_step_from_q8(background_loop.volume_q8);
  case menu_value_t::background_repeat:
    if (background_loop.loop_repeats >= 4) { return 2; }
    return background_loop.loop_repeats >= 2 ? 1 : 0;
  case menu_value_t::midi_input: {
    using namespace kp::def::command;
    bool usb = reg->midi_port_setting.getUSBMIDI() == midi_input;
    bool ble = reg->midi_port_setting.getBLEMIDI() == midi_input;
    bool pc = reg->midi_port_setting.getPortCMIDI() == midi_input;
    if (usb && ble && pc) { return 4; }
    if (pc) { return 3; }
    if (ble) { return 2; }
    if (usb) { return 1; }
    return 0; }
  case menu_value_t::usb_mode: return reg->midi_port_setting.getUSBMode() == kp::def::command::usb_device ? 1 : 0;
  case menu_value_t::usb_host_power: return reg->midi_port_setting.getUSBPowerEnabled() ? 1 : 0;
  case menu_value_t::usb_keyboard: return usb_keyboard_enabled ? 1 : 0;
  case menu_value_t::wifi_auto_update: return wifi_auto_update_check ? 1 : 0;
  case menu_value_t::audio_input_source: return (int)recording_source_mode;
  case menu_value_t::menu_sound: return menu_sound_enabled ? 1 : 0;
  case menu_value_t::display_brightness: return reg->user_setting.getDisplayBrightness();
  case menu_value_t::led_brightness: return reg->user_setting.getLedBrightness();
  case menu_value_t::language: return reg->user_setting.getLanguage() == kp::def::lang::language_t::ja ? 1 : 0;
  case menu_value_t::melody_source: return (int)melody_settings.source;
  case menu_value_t::harmony_key: return harmony_key();
  case menu_value_t::harmony_scale: return harmony_scale;
  case menu_value_t::melody_key_follow: return melody_follow_harmony_key ? 1 : 0;
  case menu_value_t::melody_key: return pitched_page_key(performance_page_t::melody);
  case menu_value_t::melody_scale: return melody_settings.scale;
  case menu_value_t::melody_octave: return melody_settings.octave + 2;
  case menu_value_t::melody_volume: return synth_volume_step_from_percent(melody_settings.volume);
  case menu_value_t::bass_source: return (int)bass_settings.source;
  case menu_value_t::bass_key: return harmony_key();
  case menu_value_t::bass_scale: return bass_settings.scale;
  case menu_value_t::bass_octave: return bass_settings.octave + 2;
  case menu_value_t::bass_volume: return synth_volume_step_from_percent(bass_settings.volume);
  case menu_value_t::chord_source: return (int)chord_settings.source;
  case menu_value_t::chord_key: return chord_settings.key;
  case menu_value_t::chord_octave: return chord_settings.octave + 2;
  case menu_value_t::chord_volume: return synth_volume_step_from_percent(chord_settings.volume);
  case menu_value_t::drum_volume: return synth_volume_step_from_percent(drum_volume);
  default: return 0;
  }
}

static const char* menu_value_text(menu_value_t value, int index)
{
  static char buf[16];
  static constexpr const char* off_on[] = { "Off", "On" };
  static constexpr const char* external_inputs[] = { "Off", "USB MIDI Controller", "USB MIDI Computer", "USB Keyboard", "BLE MIDI", "USB Gamepad" };
  static constexpr const char* grids[] = { "8", "16", "32", "64", "128" };
  static constexpr const char* bgm_repeats[] = { "1", "2", "4" };
  static constexpr const char* midi_inputs[] = { "Off", "USB", "BLE", "PortC", "All" };
  static constexpr const char* midi_note_actions[] = { "Auto", "Play", "Control" };
  static constexpr const char* usb_modes[] = { "Host", "Device" };
  static constexpr const char* input_sources[] = { "Auto", "Internal", "External" };
  static constexpr const char* langs[] = { "EN", "JP" };
  switch (value) {
  case menu_value_t::loop_quantize:
  case menu_value_t::usb_host_power:
  case menu_value_t::usb_keyboard:
  case menu_value_t::wifi_auto_update:
  case menu_value_t::menu_sound:
  case menu_value_t::melody_key_follow:
    return off_on[index ? 1 : 0];
  case menu_value_t::melody_source:
  case menu_value_t::bass_source:
  case menu_value_t::chord_source:
    return index ? "Pad" : "General MIDI";
  case menu_value_t::melody_key:
  case menu_value_t::bass_key:
  case menu_value_t::chord_key:
  case menu_value_t::harmony_key:
    return key_names[std::min<int>(index, 11)];
  case menu_value_t::melody_scale:
  case menu_value_t::bass_scale:
  case menu_value_t::harmony_scale:
    return sampler_scale_names[std::min<int>(index, sampler_scale_count - 1)];
  case menu_value_t::melody_octave:
  case menu_value_t::bass_octave:
  case menu_value_t::chord_octave:
    snprintf(buf, sizeof(buf), "%+d", std::clamp(index, 0, 4) - 2);
    return buf;
  case menu_value_t::background_volume:
    snprintf(buf, sizeof(buf), "%d%%", volume_percent_from_20_percent_step(index));
    return buf;
  case menu_value_t::melody_volume:
  case menu_value_t::bass_volume:
  case menu_value_t::chord_volume:
  case menu_value_t::drum_volume:
    snprintf(buf, sizeof(buf), "%d%%", synth_volume_percent_from_step(index));
    return buf;
  case menu_value_t::external_input_mode:
    return external_inputs[std::min<int>(index, 5)];
  case menu_value_t::midi_note_action:
    return midi_note_actions[std::min<int>(index, 2)];
  case menu_value_t::loop_note_grid:
  case menu_value_t::loop_note_off_grid:
    return grids[std::min<int>(index, 4)];
  case menu_value_t::background_repeat:
    return bgm_repeats[std::min<int>(index, 2)];
  case menu_value_t::midi_input:
    return midi_inputs[std::min<int>(index, 4)];
  case menu_value_t::usb_mode:
    return usb_modes[index ? 1 : 0];
  case menu_value_t::audio_input_source:
    return input_sources[std::min<int>(index, 2)];
  case menu_value_t::display_brightness:
  case menu_value_t::led_brightness:
    snprintf(buf, sizeof(buf), "%d", index + 1);
    return buf;
  case menu_value_t::language:
    return langs[index ? 1 : 0];
  default:
    return "";
  }
}

static bool start_file_editor_session(void)
{
#if !defined(M5UNIFIED_PC_BUILD)
  if (!kp::task_wifi_t::hasSavedSTAConfig()) {
    show_status_message("Wi-Fi Setup required", 1800, false);
    draw_menu(true);
    return false;
  }
#endif
  begin_wifi_radio_request(wifi_radio_request_t::file_server);
  wifi_setup_active = true;
  wifi_setup_qr_active = false;
  wifi_file_server_qr_active = true;
  ui_surface_exclusive = true;
  wifi_qr_preparing = true;
  wifi_file_server_client_connected = false;
  wifi_file_server_connect_deadline_msec = M5.millis() + 15000;
  wifi_setup_qr_web_page = true;
  wifi_setup_qr_dirty = true;
  return true;
}

static void menu_value_set(menu_value_t value, int index)
{
  auto reg = kp::system_registry;
  int count = menu_value_count(value);
  if (count <= 0) { return; }
  if (index < 0) { index = count - 1; }
  if (index >= count) { index = 0; }
  switch (value) {
  case menu_value_t::harmony_key:
    chord_settings.key = index;
    melody_settings.key = index;
    bass_settings.key = index;
    request_chord_label_draw();
    refresh_pitched_pad_visuals(performance_page_t::melody);
    refresh_pitched_pad_visuals(performance_page_t::bass);
    request_wave_draw();
    break;
  case menu_value_t::harmony_scale:
    harmony_scale = std::min<int>(sampler_scale_count - 1, index);
    melody_settings.scale = harmony_scale;
    bass_settings.scale = harmony_scale;
    request_chord_label_draw();
    refresh_pitched_pad_visuals(performance_page_t::melody);
    refresh_pitched_pad_visuals(performance_page_t::bass);
    request_wave_draw();
    break;
  case menu_value_t::loop_quantize:
    set_loop_quantize_enabled(index != 0);
    break;
  case menu_value_t::melody_source:
    stop_synth_page(performance_page_t::melody);
    melody_settings.source = index ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    apply_synth_tones(true);
    break;
  case menu_value_t::melody_key_follow:
    melody_follow_harmony_key = index != 0;
    refresh_pitched_pad_visuals(performance_page_t::melody);
    break;
  case menu_value_t::melody_key:
    if (melody_follow_harmony_key) {
      chord_settings.key = index;
      bass_settings.key = index;
      request_chord_label_draw();
      refresh_pitched_pad_visuals(performance_page_t::bass);
    } else {
      melody_settings.key = index;
    }
    refresh_pitched_pad_visuals(performance_page_t::melody);
    break;
  case menu_value_t::melody_scale:
    melody_settings.scale = index;
    refresh_pitched_pad_visuals(performance_page_t::melody);
    break;
  case menu_value_t::melody_octave: melody_settings.octave = index - 2; break;
  case menu_value_t::melody_volume:
    melody_settings.volume = synth_volume_percent_from_step(index);
    apply_synth_page_volume(performance_page_t::melody);
    break;
  case menu_value_t::bass_source:
    stop_synth_page(performance_page_t::bass);
    bass_settings.source = index ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    apply_synth_tones(true);
    break;
  case menu_value_t::bass_key:
    chord_settings.key = index;
    bass_settings.key = index;
    request_chord_label_draw();
    refresh_pitched_pad_visuals(performance_page_t::bass);
    if (melody_follow_harmony_key) { refresh_pitched_pad_visuals(performance_page_t::melody); }
    break;
  case menu_value_t::bass_scale:
    bass_settings.scale = index;
    refresh_pitched_pad_visuals(performance_page_t::bass);
    break;
  case menu_value_t::bass_octave: bass_settings.octave = index - 2; break;
  case menu_value_t::bass_volume:
    bass_settings.volume = synth_volume_percent_from_step(index);
    apply_synth_page_volume(performance_page_t::bass);
    break;
  case menu_value_t::chord_source:
    stop_synth_page(performance_page_t::chord);
    chord_settings.source = index ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    apply_synth_tones(true);
    break;
  case menu_value_t::chord_key:
    chord_settings.key = index;
    bass_settings.key = index;
    request_chord_label_draw();
    refresh_pitched_pad_visuals(performance_page_t::bass);
    if (melody_follow_harmony_key) { refresh_pitched_pad_visuals(performance_page_t::melody); }
    break;
  case menu_value_t::chord_octave: chord_settings.octave = index - 2; break;
  case menu_value_t::chord_volume:
    chord_settings.volume = synth_volume_percent_from_step(index);
    apply_synth_page_volume(performance_page_t::chord);
    break;
  case menu_value_t::drum_volume:
    drum_volume = synth_volume_percent_from_step(index);
    apply_synth_page_volume(performance_page_t::drum);
    break;
  case menu_value_t::external_input_mode:
    set_external_input_mode((external_input_mode_t)index);
    return;
  case menu_value_t::midi_note_action:
    midi_note_action = (midi_note_action_t)std::min<int>(index, 2);
    // Controlへ切り替えた瞬間に、前モードで鳴らした外部MIDI音だけを解放する。
    if (midi_note_action == midi_note_action_t::control) {
      for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
        sampler_audio_t::release(external_midi_voice_base + i);
        external_midi_voice_note[i] = -1;
        pitched_voice_state[i] = {};
      }
    }
    break;
  case menu_value_t::loop_note_grid:
    set_loop_quantize_option((uint8_t)index, false);
    break;
  case menu_value_t::loop_note_off_grid:
    set_loop_note_off_quantize_option((uint8_t)index, false);
    break;
  case menu_value_t::background_volume: {
    background_loop.volume_q8 = volume_q8_from_20_percent_step(index);
    if (loop_playing && background_loop.isValid()) {
      uint32_t start_frame = ((uint64_t)loop_pos_ms(M5.millis()) * background_loop.sample_rate) / 1000;
      if (background_loop.frames) { start_frame %= background_loop.frames; }
      sampler_audio_t::play(background_loop_voice, background_loop.pcm, background_loop.frames,
                            background_loop.sample_rate, true, false,
                            mixer_scaled_volume_q8(mixer_part_t::bgm, background_loop.volume_q8),
                            256, start_frame);
    }
    break; }
  case menu_value_t::background_repeat: {
    static constexpr uint8_t repeats[] = { 1, 2, 4 };
    background_loop.loop_repeats = repeats[std::min<int>(index, 2)];
    if (background_loop.isValid()) {
      const uint32_t now = M5.millis();
      const uint32_t new_length = background_loop_length_ms();
      uint32_t position = loop_playing ? loop_pos_ms(now) : 0;
      if (new_length) { position %= new_length; }
      loop_length_msec = new_length;
      loop_length_fixed = true;
      loop_start_msec = now - position;
      loop_prev_pos_ms = position;
      auto_configure_loop_grid(loop_length_msec);
      if (loop_playing) { play_background_loop_at(position); }
    }
    break; }
  case menu_value_t::midi_input: {
    using namespace kp::def::command;
    reg->midi_port_setting.setUSBMIDI(index == 1 || index == 4 ? midi_input : midi_off);
    reg->midi_port_setting.setBLEMIDI(index == 2 || index == 4 ? midi_input : midi_off);
    reg->midi_port_setting.setPortCMIDI(index == 3 || index == 4 ? midi_input : midi_off);
    break; }
  case menu_value_t::usb_mode:
    reg->midi_port_setting.setUSBMode(index ? kp::def::command::usb_device : kp::def::command::usb_host);
    break;
  case menu_value_t::usb_host_power:
    reg->midi_port_setting.setUSBPowerEnabled(index != 0);
    break;
  case menu_value_t::usb_keyboard:
    if (index && reg->midi_port_setting.getUSBMode() != kp::def::command::usb_mode_t::usb_host) {
      show_status_message("USB Mode: Host required", 1800, false);
      break;
    }
    usb_keyboard_enabled = index != 0;
    if (usb_keyboard_enabled) {
      // キーボード側へ給電してからUSBホストを起動する。
      reg->midi_port_setting.setUSBPowerEnabled(true);
      if (!task_midi.startUSBHIDKeyboard()) {
        usb_keyboard_enabled = false;
        show_status_message("USB keyboard unavailable", 1800, false);
      }
    }
    break;
  case menu_value_t::wifi_auto_update:
    wifi_auto_update_check = index != 0;
    save_sampler_folder_settings();
    break;
  case menu_value_t::audio_input_source:
    recording_source_mode = (recording_source_mode_t)index;
    break;
  case menu_value_t::menu_sound:
    menu_sound_enabled = index != 0;
    if (!menu_sound_enabled) { stop_menu_feedback(); }
    save_sampler_folder_settings();
    break;
  case menu_value_t::display_brightness:
    reg->user_setting.setDisplayBrightness(index);
    break;
  case menu_value_t::led_brightness:
    reg->user_setting.setLedBrightness(index);
    reg->rgbled_control.refresh();
    break;
  case menu_value_t::language:
    reg->user_setting.setLanguage(index ? kp::def::lang::language_t::ja : kp::def::lang::language_t::en);
    break;
  default:
    break;
  }
  menu_settings_save_pending = true;
  menu_settings_save_due_msec = M5.millis() + 450;
}

static void service_menu_settings_save(uint32_t now)
{
  if (!menu_settings_save_pending
   || (int32_t)(now - menu_settings_save_due_msec) < 0
   || !menu_visible
   || sound_priority_active(now)
   || kp::system_registry == nullptr) {
    return;
  }
  kp::system_registry->save();
  menu_settings_save_pending = false;
}

static void clear_status_message(bool redraw = true);
static void draw_menu(bool redraw_keypad = false);

// Value rows are adjusted directly from the two adjacent top-right buttons.
// Dynamic file/device lists retain their existing numeric-keypad behaviour.
static bool menu_current_value(menu_value_t* value)
{
  if (!menu_visible || value == nullptr
   || input_assignment_list_active
   || kit_edit_state != kit_edit_state_t::idle
   || ble_device_ui_state == ble_device_ui_state_t::list
   || ble_device_ui_state == ble_device_ui_state_t::confirm) {
    return false;
  }
  size_t count = 0;
  const auto* items = menu_items(menu_page, &count);
  if (items == nullptr || menu_cursor >= count
   || items[menu_cursor].kind != menu_item_kind_t::value) {
    return false;
  }
  *value = items[menu_cursor].value;
  return true;
}

static uint8_t menu_keypad_state_for_current_menu(void)
{
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad
   || kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    return 3;
  }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) { return 4; }
  if (input_assignment_list_active) { return 2; }
  menu_value_t value = menu_value_t::none;
  return menu_current_value(&value) ? 1 : 0;
}

static bool menu_adjust_current_value(int delta)
{
  menu_value_t value = menu_value_t::none;
  if (delta == 0 || !menu_current_value(&value)) { return false; }
  menu_value_set(value, menu_value_get(value) + delta);
  menu_sound_navigate(delta > 0 ? 1 : 2);
  draw_menu(true);
  return true;
}

static bool status_message_visible(uint32_t now = M5.millis())
{
  return status_message[0] && (status_message_until == 0 || (int32_t)(status_message_until - now) > 0);
}

static void draw_busy_status_dots(bool advance = false)
{
  if (!menu_visible || !status_message_busy || !status_message_visible()) { return; }
  static uint32_t last_draw_msec = 0;
  const uint32_t now = M5.millis();
  if (advance && now - last_draw_msec < 100) { return; }
  last_draw_msec = now;

  auto& d = M5.Display;
  const int status_y = tab_y - 44;
  const int cx = d.width() / 2;
  const uint8_t phase = (uint8_t)(((now - status_message_anim_msec) / 180u) & 3u);
  d.startWrite();
  d.fillRect(cx - 36, status_y + 24, 72, 16, 0x202030u);
  for (int i = 0; i < 4; ++i) {
    const bool active = i == phase;
    d.fillCircle(cx - 24 + i * 16, status_y + 32, active ? 4 : 2,
                 active ? 0x70B8FFu : 0x384858u);
  }
  d.endWrite();
}

static void draw_busy_status_dots_tick(void)
{
  draw_busy_status_dots(true);
}

static void show_status_message(const char* msg, uint32_t duration_ms = 1600, bool redraw = true)
{
  snprintf(status_message, sizeof(status_message), "%s", msg ? msg : "");
  status_message_until = duration_ms ? M5.millis() + duration_ms : 0;
  status_message_busy = false;
  if (redraw && menu_visible) { draw_menu(); }
}

static void show_loading_message(const char* msg = "LOADING")
{
  snprintf(status_message, sizeof(status_message), "%s", msg ? msg : "LOADING");
  status_message_until = 0;
  status_message_busy = true;
  status_message_anim_msec = M5.millis();
  if (menu_visible) { draw_menu(); }
}

// 静的な待機画面は一度だけ描き、進捗はドット部分だけを更新する。
// 起動・ファイル読み込み・録音後処理で共用し、全面再描画の点滅を避ける。
static void draw_wait_screen_frame(bool& static_drawn, uint8_t& frame,
                                   const char* title, const char* detail,
                                   uint32_t accent)
{
  static char last_detail[32] = {};
  auto& d = M5.Display;
  const uint8_t phase = frame++ & 3;
  const uint32_t bg = 0x08080Cu;
  const int cx = d.width() / 2;
  const int cy = d.height() / 2;
  d.startWrite();
  if (!static_drawn) {
    d.fillScreen(bg);
    // Keep a calm, static frame around full-screen wait states. The old
    // animated frame was distracting during SD/audio work, but removing the
    // outline altogether made startup and sample processing feel unfinished.
    d.drawRect(0, 0, d.width(), d.height(), accent);
    d.drawRect(1, 1, d.width() - 2, d.height() - 2, accent);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextSize(1, 2);
    d.setTextColor(0xFFFFFFu, bg);
    d.drawString(def::app::app_name, cx, cy - 58);
    d.setTextSize(1);
    d.setTextColor(0xA0C8E8u, bg);
    char version[20];
    snprintf(version, sizeof(version), "ver.%u.%u.%u",
             (unsigned)def::app::app_version_major,
             (unsigned)def::app::app_version_minor,
             (unsigned)def::app::app_version_patch);
    d.drawString(version, cx, cy - 30);
    d.setTextSize(1, 2);
    d.setTextColor(0xFFFFFFu, bg);
    d.drawString(title ? title : "LOADING", cx, cy + 12);
    d.setTextSize(1);
    d.setTextColor(0xA0C8E8u, bg);
    d.drawString(detail ? detail : "PLEASE WAIT", cx, cy + 43);
    snprintf(last_detail, sizeof(last_detail), "%s", detail ? detail : "PLEASE WAIT");
    static_drawn = true;
  } else if (strcmp(last_detail, detail ? detail : "PLEASE WAIT") != 0) {
    d.fillRect(cx - 104, cy + 32, 208, 22, bg);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextSize(1);
    d.setTextColor(0xA0C8E8u, bg);
    d.drawString(detail ? detail : "PLEASE WAIT", cx, cy + 43);
    snprintf(last_detail, sizeof(last_detail), "%s", detail ? detail : "PLEASE WAIT");
  }
  // Only the dots animate. A changing full-screen frame is distracting while
  // SD and audio processing naturally have uneven execution time.
  d.fillRect(cx - 36, cy + 62, 72, 18, bg);
  for (int i = 0; i < 4; ++i) {
    uint32_t color = i == phase ? accent : 0x283848u;
    d.fillCircle(cx - 24 + i * 16, cy + 71, i == phase ? 5 : 3, color);
  }
  d.endWrite();
}

// Startup restores WAV data synchronously. Draw the static screen once, then
// update only the dots so loading progress never flashes the whole display.
static void draw_startup_loading_frame(const char* detail)
{
  if (!startup_loading_active) {
    draw_busy_status_dots_tick();
    return;
  }
  draw_wait_screen_frame(startup_loading_static_drawn, startup_loading_frame,
                         "LOADING", detail, 0x4090E0u);
}

static void draw_recording_processing_frame(const char* detail)
{
  processing_screen_visible = true;
  draw_wait_screen_frame(recording_processing_static_drawn, recording_processing_frame,
                         "PROCESSING", detail, 0xFF5050u);
}

static void clear_status_message(bool redraw)
{
  status_message[0] = 0;
  status_message_until = 0;
  status_message_busy = false;
  if (redraw && menu_visible) { draw_menu(); }
}

static void draw_learn_overlay(void)
{
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, wave_y, d.width(), tab_y + tab_h - wave_y, 0x08080Cu);
  d.drawRect(0, wave_y, d.width(), tab_y + tab_h - wave_y, 0xA0A0FFu);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString("LEARN", 120, wave_y + 22);
  if (learn_state == learn_state_t::waiting_target) {
    d.setTextColor(0xC0C0D0u, 0x08080Cu);
    d.drawString("Select target", 120, wave_y + 64);
    d.setTextSize(0.75f);
    d.drawString("Pads / Fn / Mode / Stop", 120, wave_y + 88);
  } else if (learn_state == learn_state_t::waiting_external) {
    d.setTextColor(0xC0C0D0u, 0x08080Cu);
    d.drawString("Press MIDI or EXT button", 120, wave_y + 52);
    d.setTextColor(0x80D0FFu, 0x08080Cu);
    d.drawString(learn_target_label, 120, wave_y + 76);
  }
  d.endWrite();
  if (learn_state == learn_state_t::waiting_target) {
    draw_learn_target_keypad();
  } else if (learn_state == learn_state_t::waiting_external) {
    // External-input wait returns the familiar menu Back / Exit affordances.
    draw_menu_keypad();
  }
}

static const char* menu_button_label(int btn)
{
  static char wait_pad_labels[15][4];
  if (input_assignment_list_active && btn == 14) { return "Del"; }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) {
    if (btn == 8) { return "Back"; }
    if (btn == 9) { return "OK"; }
    return "";
  }
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    if (btn == 4) { return "Back"; }
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      snprintf(wait_pad_labels[btn], sizeof(wait_pad_labels[btn]), "%u", (unsigned)pad_display_number((uint8_t)pad));
      return wait_pad_labels[btn];
    }
    return "";
  }
  menu_value_t value;
  if (menu_current_value(&value)) {
    if (btn == 13) { return "-"; }  // Pad 12
    if (btn == 14) { return "+"; }  // Fn 1
  }
  static constexpr const char* labels[15] = {
    "1", "2", "3", "0", "Exit",
    "4", "5", "6", "Back", "OK",
    "7", "8", "9", "", "",
  };
  return (btn >= 0 && btn < 15) ? labels[btn] : "";
}

// Menu buttons are a separate control surface, not sample pads.  Feed the
// LED from the exact same background color used by draw_menu_keypad() so a
// command, +/- adjustment or pad-selection state never keeps the previous
// performance-page color.
static void update_menu_keypad_leds()
{
  const bool wait_pad = kit_edit_state == kit_edit_state_t::assign_wait_pad
                     || kit_edit_state == kit_edit_state_t::clear_wait_pad;
  const bool confirm_import = kit_edit_state == kit_edit_state_t::assign_confirm_shortcut;
  menu_value_t selected_value;
  const bool value_adjust = menu_current_value(&selected_value);
  for (int btn = 0; btn < 15; ++btn) {
    const bool adjust = value_adjust && (btn == 13 || btn == 14);
    const bool command = confirm_import ? (btn == 8 || btn == 9)
      : wait_pad ? (btn == 4) : (btn == 4 || btn == 8 || btn == 9
      || adjust || (input_assignment_list_active && btn == 14));
    uint32_t bg = command ? 0x263048u : 0x202028u;
    if (input_assignment_list_active && btn == 14) {
      bg = 0x483030u;
    } else if ((!wait_pad || confirm_import) && btn == 9) {
      bg = 0x304838u;
    } else if (adjust) {
      bg = btn == 14 ? 0x304838u : 0x364058u;
    } else if (command) {
      bg = 0x483030u;
    }
    kp::system_registry->rgbled_control.setColor(btn, led_from_rgb24(bg));
  }
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad
   && kit_assign_loading_pad >= 0 && kit_assign_loading_pad < (int)def::pad::pad_count) {
    kp::system_registry->rgbled_control.setColor(pad_to_button((uint8_t)kit_assign_loading_pad), 0xFFFFFFu);
  }
}

static void draw_menu_keypad(bool force)
{
  const uint8_t state = menu_keypad_state_for_current_menu();
  if (!force && state == menu_keypad_state) { return; }
  menu_keypad_state = state;
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
      draw_pad(pad);
    }
    auto& d = M5.Display;
    d.startWrite();
    for (int fn = 0; fn < 3; ++fn) {
      int btn = fn_to_button((uint8_t)fn);
      int y = grid_y + fn * row_pitch;
      bool back = btn == 4;
      uint32_t bg = back ? 0x483030u : 0x202028u;
      uint32_t fg = back ? 0xFFD0D0u : 0x606078u;
      d.fillRoundRect(fn_x, y, fn_w, cell_h, 6, bg);
      d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, 0x606078u);
      if (back) {
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(1);
        d.setTextDatum(m5gfx::textdatum_t::middle_center);
        d.setTextColor(fg, bg);
        d.drawString("Back", fn_x + fn_w / 2, y + cell_h / 2);
      }
    }
    if (kit_edit_state == kit_edit_state_t::assign_wait_pad
     && kit_assign_loading_pad >= 0 && kit_assign_loading_pad < (int)def::pad::pad_count) {
      const int x = grid_x + (kit_assign_loading_pad % 4) * col_pitch;
      const int y = grid_y + (kit_assign_loading_pad / 4) * row_pitch;
      d.drawRoundRect(x, y, pad_w, cell_h, 6, 0xFFFFFFu);
      d.drawRoundRect(x + 1, y + 1, pad_w - 2, cell_h - 2, 5, 0xFFFFFFu);
    }
    d.endWrite();
    update_menu_keypad_leds();
    return;
  }
  auto& d = M5.Display;
  menu_value_t selected_value;
  const bool value_adjust = menu_current_value(&selected_value);
  const bool confirm_import = kit_edit_state == kit_edit_state_t::assign_confirm_shortcut;
  d.startWrite();
  for (int btn = 0; btn < 15; ++btn) {
    int row = btn / 5;
    int col = btn % 5;
    int x = (col == 4) ? fn_x : grid_x + col * col_pitch;
    int y = grid_y + (2 - row) * row_pitch;
    bool wait_pad = kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad;
    const bool adjust = value_adjust && (btn == 13 || btn == 14);
    bool command = confirm_import ? (btn == 8 || btn == 9)
                              : wait_pad ? (btn == 4) : (btn == 4 || btn == 8 || btn == 9
                                         || adjust || (input_assignment_list_active && btn == 14));
    uint32_t bg = command ? 0x263048u : 0x202028u;
    uint32_t fg = command ? 0xC8D8FFu : 0xFFFFFFu;
    if (input_assignment_list_active && btn == 14) {
      bg = 0x483030u;
      fg = 0xFFD0D0u;
    } else if ((!wait_pad || confirm_import) && btn == 9) {
      bg = 0x304838u;
      fg = 0xB0FFD0u;
    } else if (adjust) {
      bg = btn == 14 ? 0x304838u : 0x364058u;
      fg = btn == 14 ? 0xB0FFD0u : 0xC8D8FFu;
    } else if (command) {
      bg = 0x483030u;
      fg = 0xFFD0D0u;
    }
    d.fillRoundRect(x, y, pad_w, cell_h, 6, bg);
    d.drawRoundRect(x, y, pad_w, cell_h, 6, 0x606078u);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(fg, bg);
    d.drawString(menu_button_label(btn), x + pad_w / 2, y + cell_h / 2);
  }
  d.endWrite();
  update_menu_keypad_leds();
}

static void draw_learn_target_keypad(void)
{
  auto& d = M5.Display;
  d.startWrite();
  for (int btn = 0; btn < 15; ++btn) {
    int row = btn / 5;
    int col = btn % 5;
    int x = (col == 4) ? fn_x : grid_x + col * col_pitch;
    int y = grid_y + (2 - row) * row_pitch;
    int pad = button_to_pad(btn);
    int fn = button_to_fn(btn);
    char label[5] = {};
    uint32_t bg = 0x202830u;
    uint32_t fg = 0xD8E8FFu;
    if (pad >= 0) {
      snprintf(label, sizeof(label), "P%u", (unsigned)pad_display_number((uint8_t)pad));
    } else if (fn >= 0) {
      snprintf(label, sizeof(label), "Fn%u", (unsigned)(fn + 1));
      bg = 0x303048u;
      fg = 0xD0C8FFu;
    }
    d.fillRoundRect(x, y, col == 4 ? fn_w : pad_w, cell_h, 6, bg);
    d.drawRoundRect(x, y, col == 4 ? fn_w : pad_w, cell_h, 6, 0x707890u);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(fg, bg);
    d.drawString(label, x + (col == 4 ? fn_w : pad_w) / 2, y + cell_h / 2);
    kp::system_registry->rgbled_control.setColor(btn, led_from_rgb24(bg));
  }
  d.endWrite();
}

static int menu_first_visible(uint8_t cursor)
{
  return cursor >= 4 ? cursor - 3 : 0;
}

static const char* menu_dynamic_title(void)
{
  static char import_target_title[24];
  if (input_assignment_list_active) { return "Assign List"; }
  if (ble_device_ui_state == ble_device_ui_state_t::list) { return "BLE Devices"; }
  if (ble_device_ui_state == ble_device_ui_state_t::confirm) { return "Allow Connection"; }
  switch (kit_edit_state) {
  case kit_edit_state_t::select_kit_file: return "Load Kit";
  case kit_edit_state_t::select_kit_save: return "Save Kit";
  case kit_edit_state_t::select_wav: return "Import Sample";
  case kit_edit_state_t::assign_confirm_shortcut:
    snprintf(import_target_title, sizeof(import_target_title), "Import to P%u",
             kit_shortcut_target_pad >= 0
               ? (unsigned)pad_display_number((uint8_t)kit_shortcut_target_pad) : 0u);
    return import_target_title;
  case kit_edit_state_t::select_bgm_wav: return "Load BGM";
  case kit_edit_state_t::select_external_tone:
    if (!synth_sound_select_active) { return "MIDI Tone"; }
    return synth_menu_target == performance_page_t::chord ? "Chord Tone"
      : synth_menu_target == performance_page_t::bass ? "Bass Tone" : "Melody Tone";
  case kit_edit_state_t::select_external_pad:
    if (!synth_sound_select_active) { return "MIDI Pad"; }
    return synth_menu_target == performance_page_t::chord ? "Chord Pad"
      : synth_menu_target == performance_page_t::bass ? "Bass Pad" : "Melody Pad";
  case kit_edit_state_t::select_external_pad_base_note:
    return synth_sound_select_active ? "Pad Base Note" : "MIDI Pad Base Note";
  case kit_edit_state_t::assign_wait_pad: return "Select Pad";
  case kit_edit_state_t::clear_wait_pad: return "Clear Pad";
  case kit_edit_state_t::pad_list: return "Pad List";
  default: return menu_page_title(menu_page);
  }
}

// メニュー見出しは通常画面のヘッダーを一時的に使う。本文をモードボタン領域まで
// 広げ、かんぷれと同じ大きな文字で4行半を見せられるようにする。
static void draw_menu_header(bool force = false)
{
  static char cached_title[48] = {};
  static bool cached_assignment_list = false;
  const char* title = menu_dynamic_title();
  const bool assignment_list = input_assignment_list_active;
  if (!force && !strcmp(cached_title, title) && cached_assignment_list == assignment_list) { return; }

  snprintf(cached_title, sizeof(cached_title), "%s", title);
  cached_assignment_list = assignment_list;

  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, 0, d.width(), header_h, 0x08080Cu);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_left);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(title, 7, header_h / 2);
  d.drawFastHLine(0, header_h - 1, d.width(), 0x303048u);
  d.endWrite();
}

static size_t menu_dynamic_count(void)
{
  if (input_assignment_list_active) { return input_assignment_list.size(); }
  if (ble_device_ui_state == ble_device_ui_state_t::list) { return ble_device_count; }
  if (ble_device_ui_state == ble_device_ui_state_t::confirm) { return 2; }
  switch (kit_edit_state) {
  case kit_edit_state_t::select_kit_file: return kit_wav_list.size();
  case kit_edit_state_t::select_kit_save: return kit_save_candidate_count;
  case kit_edit_state_t::select_wav: return kit_wav_list.size();
  case kit_edit_state_t::select_bgm_wav: return kit_wav_list.size();
  case kit_edit_state_t::select_external_tone: return 128;
  case kit_edit_state_t::select_external_pad: return def::pad::pad_count;
  case kit_edit_state_t::select_external_pad_base_note: return 128;
  case kit_edit_state_t::pad_list: return def::pad::pad_count;
  default: return 0;
  }
}

static bool menu_dynamic_list_active(void)
{
  return input_assignment_list_active
      || ble_device_ui_state == ble_device_ui_state_t::list
      || ble_device_ui_state == ble_device_ui_state_t::confirm
      || kit_edit_state == kit_edit_state_t::select_wav
      || kit_edit_state == kit_edit_state_t::select_kit_file
      || kit_edit_state == kit_edit_state_t::select_kit_save
      || kit_edit_state == kit_edit_state_t::select_bgm_wav
      || kit_edit_state == kit_edit_state_t::select_external_tone
      || kit_edit_state == kit_edit_state_t::select_external_pad
      || kit_edit_state == kit_edit_state_t::select_external_pad_base_note
      || kit_edit_state == kit_edit_state_t::pad_list;
}

static void menu_dynamic_label(size_t index, char* out, size_t out_len)
{
  if (out_len == 0) { return; }
  out[0] = 0;
  if (ble_device_ui_state == ble_device_ui_state_t::list) {
    if (index >= ble_device_count) { return; }
    const auto& device = ble_device_list[index];
    snprintf(out, out_len, "%s%s %ddB", device.advertises_midi ? "* " : "",
             device.name[0] ? device.name : device.address, (int)device.rssi);
    return;
  }
  if (ble_device_ui_state == ble_device_ui_state_t::confirm) {
    if (index == 0) {
      snprintf(out, out_len, "Cancel");
    } else if (ble_device_selected < ble_device_count) {
      snprintf(out, out_len, "Connect %.22s", ble_device_list[ble_device_selected].name);
    }
    return;
  }
  if (input_assignment_list_active) {
    if (index >= input_assignment_list.size()) { return; }
    const auto& entry = input_assignment_list[index];
    char target[16];
    if (entry.target >= (int16_t)midi_assign_target_t::pad_base
     && entry.target < (int16_t)midi_assign_target_t::pad_base + (int)def::pad::pad_count) {
      snprintf(target, sizeof(target), "P%u", (unsigned)pad_display_number((uint8_t)(entry.target - (int16_t)midi_assign_target_t::pad_base)));
    } else if (entry.target >= (int16_t)midi_assign_target_t::mode_base
            && entry.target < (int16_t)midi_assign_target_t::mode_base + (int)sampler_mode_t::mode_max) {
      snprintf(target, sizeof(target), "%s", mode_info[entry.target - (int16_t)midi_assign_target_t::mode_base].name);
    } else if (entry.target == (int16_t)midi_assign_target_t::stop_all) {
      snprintf(target, sizeof(target), "STOP ALL");
    } else if (entry.target >= (int16_t)midi_assign_target_t::fn_base
            && entry.target < (int16_t)midi_assign_target_t::fn_base + 3) {
      snprintf(target, sizeof(target), "Fn%u", (unsigned)(entry.target - (int16_t)midi_assign_target_t::fn_base + 1));
    } else {
      snprintf(target, sizeof(target), "-");
    }
    const char* source = entry.source_type == input_source_t::external ? "EXT"
                       : entry.source_type == input_source_t::usb_keyboard ? "KEY"
                       : entry.source_type == input_source_t::usb_gamepad ? "PAD" : "MIDI";
    snprintf(out, out_len, "%s %u > %s", source,
             (unsigned)(entry.source_type == input_source_t::external ? entry.source + 1 : entry.source), target);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_wav
   || kit_edit_state == kit_edit_state_t::select_bgm_wav
   || kit_edit_state == kit_edit_state_t::select_kit_file) {
    if (index >= kit_wav_list.size()) { return; }
    const std::string label = sampler_file_display_name(kit_wav_list[index].filename, kit_wav_dir);
    snprintf(out, out_len, "%s", label.c_str());
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_kit_save) {
    if (index < kit_save_candidate_count) {
      snprintf(out, out_len, "%s", kit_save_candidates[index].label);
    }
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_tone && index < 128) {
    snprintf(out, out_len, "%s", kp::def::midi::program_name_table.at(index)->get());
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_pad && index < def::pad::pad_count) {
    const uint8_t pad = display_order_to_pad((uint8_t)index);
    const auto& slot = sampler_pool_t::slot[pad];
    snprintf(out, out_len, "P%u %s", (unsigned)(index + 1),
             slot.isValid() ? slot.name : "(empty)");
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_pad_base_note && index < 128) {
    static constexpr const char* note_names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    snprintf(out, out_len, "%s%d (%u)", note_names[index % 12], (int)(index / 12) - 2, (unsigned)index);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::pad_list && index < def::pad::pad_count) {
    uint8_t pad = display_order_to_pad((uint8_t)index);
    auto& slot = sampler_pool_t::slot[pad];
    snprintf(out, out_len, "P%u %s", (unsigned)(index + 1), slot.isValid() ? slot.name : "-");
  }
}

static void draw_menu_wait_pad(M5Canvas& d, const char* title, const char* line1, const char* line2)
{
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::top_left);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(title, 8, 5);
  d.setTextDatum(m5gfx::textdatum_t::top_right);
  d.setTextColor(0x9090B0u, 0x08080Cu);
  d.drawString("Back", M5.Display.width() - 8, 5);
  d.drawFastHLine(6, 25, M5.Display.width() - 12, 0x303048u);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(line1, 120, 62);
  d.setTextColor(0x80D0FFu, 0x08080Cu);
  d.drawString(line2, 120, 96);
}

static void prepare_wifi_setup_qr(bool web_page)
{
  if (!wifi_setup_qr_dirty && wifi_setup_qr_web_page == web_page) { return; }

  wifi_setup_qr_web_page = web_page;
  wifi_setup_qr_dirty = false;
  if (!wifi_qr_canvas_ready) {
    wifi_qr_canvas.setPsram(true);
    wifi_qr_canvas.setColorDepth(1);
    wifi_qr_canvas_ready = wifi_qr_canvas.createSprite(39, 39) != nullptr;
  }
  if (!wifi_qr_canvas_ready) {
    // A later redraw can retry after other transient allocations are released.
    wifi_setup_qr_dirty = true;
    return;
  }
  wifi_qr_canvas.fillScreen(TFT_WHITE);

  char payload[96];
  if (web_page) {
    // 設定用APではmDNSが端末ごとに解決できないことがあるため、CoreS3の
    // 固定ゲートウェイへ直接誘導する。File Server時は通常LANのmDNSを使う。
    if (wifi_file_server_qr_active) {
      snprintf(payload, sizeof(payload), "http://%s.local/", kp::def::app::wifi_mdns);
    } else {
      snprintf(payload, sizeof(payload), "http://192.168.4.1/");
    }
  } else {
    snprintf(payload, sizeof(payload), "WIFI:S:%s;T:%s;P:%s;;",
             kp::def::app::wifi_ap_ssid, kp::def::app::wifi_ap_type, kp::def::app::wifi_ap_pass);
  }
  wifi_qr_canvas.qrcode(payload);
}

static void reset_wifi_qr_canvas(void)
{
  // Keep this tiny PSRAM sprite alive across File Editor / Wi-Fi setup
  // sessions.  Repeated deleteSprite/createSprite around button input was
  // prone to M5GFX allocator races while the display was being restored.
  if (wifi_qr_canvas_ready) { wifi_qr_canvas.fillScreen(TFT_WHITE); }
}

static void draw_wifi_setup_qr(void)
{
  // 接続後にSTAへ移行するとAPの接続人数は0へ戻る。いったん2枚目へ進んだら
  // 設定ページ用QRを保持し、1枚目へ巻き戻さない。
  const bool file_server = wifi_file_server_qr_active;
  const bool file_server_connected = !file_server || wifi_sta_connected();
  const bool web_page = file_server || wifi_setup_qr_web_page
                     || kp::system_registry->runtime_info.getWiFiStationCount() != 0;
  prepare_wifi_setup_qr(web_page);

  if (!wifi_qr_canvas_ready) {
    auto& d = M5.Display;
    d.startWrite();
    d.fillScreen(0x08080Cu);
    d.drawRect(0, 0, d.width(), d.height(), 0xD0A040u);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(0xFFFFFFu, 0x08080Cu);
    d.drawString("QR PREPARING", d.width() / 2, d.height() / 2);
    d.endWrite();
    return;
  }

  // かんぷれのui_popup_qr_tと同じ39x39・5倍表示。メニュー領域に
  // 押し込まず、画面中央の大きなウィンドウにすることでスマホで読めるサイズにする。
  static constexpr int qr_width = 39 * 5;
  static constexpr int window_w = qr_width + 10;
  static constexpr int window_h = qr_width + 60;
  const int x = (M5.Display.width() - window_w) / 2;
  const int y = (M5.Display.height() - window_h) / 2;
  const bool client_connected = file_server && wifi_file_server_client_connected;
  const uint32_t frame_color = client_connected ? 0x40D080u
                           : file_server && !file_server_connected ? 0x707080u
                           : (web_page ? 0xFFFF00u : 0xC0C0C0u);
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(0x08080Cu);
  d.fillRect(x, y, window_w, window_h, frame_color);
  wifi_qr_canvas.pushRotateZoom(&d, x + window_w / 2, y + qr_width / 2, 0.0f, 5.0f, 5.0f);
  d.drawRect(x + 1, y + 1, window_w - 2, window_h - 2, TFT_DARKGRAY);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1, 2);
  d.setTextDatum(m5gfx::textdatum_t::bottom_center);
  d.setTextColor(TFT_BLACK, frame_color);
  const int cx = x + window_w / 2;
  if (web_page) {
    d.drawString(file_server ? "http://kanplay.local" : "http://192.168.4.1", cx, y + window_h - 26);
    // URLは読める大きさを維持し、状態案内だけ通常の高さへ戻す。
    // 2行を分離して、接続中・接続済みの長い文言も枠内に収める。
    d.setTextSize(1, 1);
    d.drawString(file_server ? (!file_server_connected ? "Connecting Wi-Fi..."
                               : client_connected ? "File Editor Connected" : "Scan for File Editor")
                              : "Scan QR or type URL", cx, y + window_h - 4);
  } else {
    d.setTextSize(1, 1);
    char ssid[40];
    char pass[40];
    snprintf(ssid, sizeof(ssid), "SSID: %s", kp::def::app::wifi_ap_ssid);
    snprintf(pass, sizeof(pass), "PASS: %s", kp::def::app::wifi_ap_pass);
    // QRを読めない場合も、この画面だけでアクセスポイントへ参加できる。
    d.drawString(ssid, cx, y + window_h - 36);
    d.drawString(pass, cx, y + window_h - 20);
    d.drawString("Scan to connect", cx, y + window_h - 4);
  }
  d.endWrite();
}

static void draw_wifi_qr_preparing(void)
{
  const bool file_server = wifi_file_server_qr_active;
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(0x08080Cu);
  d.drawRect(0, 0, d.width(), d.height(), file_server ? 0xFFFF00u : 0xC0C0C0u);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1, 2);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  const bool stopping_ble = wifi_radio_request != wifi_radio_request_t::none;
  d.drawString(stopping_ble ? "STOPPING BLUETOOTH" : (file_server ? "CONNECTING WI-FI" : "PREPARING WI-FI"),
               d.width() / 2, d.height() / 2 - 12);
  d.setTextSize(1, 1);
  d.setTextColor(0xA0C0D0u, 0x08080Cu);
  d.drawString(stopping_ble ? "Preparing Wi-Fi..." : "Starting server...", d.width() / 2, d.height() / 2 + 22);
  d.endWrite();
}

static void disable_wifi_and_clear_indicator(void)
{
  auto reg = kp::system_registry;
  if (reg == nullptr) { return; }
  reg->wifi_control.setOperation(kp::def::command::wifi_operation_t::wfop_disable);
  reg->wifi_control.setWifiMode(kp::def::command::wifi_mode_t::wifi_disable);
  // task_wifiの状態反映は最大1秒後になるため、QRを閉じた直後のヘッダーだけ
  // 接続中のまま残らないよう、UI側でも停止状態を確定する。
  reg->runtime_info.setWiFiSTAInfo(kp::def::command::wifi_sta_info_t::wsi_off);
  reg->runtime_info.setWiFiAPInfo(kp::def::command::wifi_ap_info_t::wai_off);
  wifi_radio_request = wifi_radio_request_t::none;
  wifi_radio_request_deadline_msec = 0;
  wifi_radio_request_not_before_msec = 0;
  request_performance_ui_arena_resume();
  if (wifi_ble_suspended) {
    // Wi-Fiタスクがドライバとnetifを解放してからBLEを戻す。ここで即時再開すると
    // 両方の内部RAM確保が重なり、再起動直後の機体でクラッシュすることがある。
    wifi_ble_resume_pending = true;
    wifi_ble_resume_not_before_msec = M5.millis() + 500;
  }
}

static void service_wifi_ble_resume(uint32_t now)
{
  if (!wifi_ble_resume_pending || kp::system_registry == nullptr) { return; }
  auto reg = kp::system_registry;
  if (external_input_mode != external_input_mode_t::ble_midi) {
    wifi_ble_resume_pending = false;
    wifi_ble_suspended = false;
    return;
  }
  if ((int32_t)(now - wifi_ble_resume_not_before_msec) < 0) { return; }
  if (reg->wifi_control.getOperation() != kp::def::command::wifi_operation_t::wfop_disable
   || reg->wifi_control.getWifiMode() != kp::def::command::wifi_mode_t::wifi_disable
   || reg->runtime_info.getWiFiSTAInfo() != kp::def::command::wifi_sta_info_t::wsi_off
   || reg->runtime_info.getWiFiAPInfo() != kp::def::command::wifi_ap_info_t::wai_off) {
    return;
  }
  wifi_ble_resume_pending = false;
  wifi_ble_suspended = false;
  reg->midi_port_setting.setBLEMIDI(kp::def::command::midi_input);
}

static const char* wifi_update_text(uint8_t state, char* out, size_t out_len)
{
  using namespace kp::def::command;
  if (state <= 100) {
    snprintf(out, out_len, "DOWNLOADING %u%%", (unsigned)state);
  } else if (state == (uint8_t)wifi_ota_state_t::ota_connecting) {
    snprintf(out, out_len, "CONNECTING");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_update_available) {
    snprintf(out, out_len, "PREPARING UPDATE");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_already_up_to_date) {
    snprintf(out, out_len, "UP TO DATE");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_update_done) {
    snprintf(out, out_len, "RESTARTING");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_wifi_connection_error) {
    snprintf(out, out_len, "WI-FI CONNECTION FAILED");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_catalog_error) {
    snprintf(out, out_len, "CATALOG REQUEST FAILED");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_no_matching_firmware) {
    snprintf(out, out_len, "NO SAMPLER UPDATE FOUND");
  } else if (state == (uint8_t)wifi_ota_state_t::ota_connection_error) {
    snprintf(out, out_len, "CONNECTION FAILED");
  } else {
    snprintf(out, out_len, "UPDATE FAILED");
  }
  return out;
}

static void draw_wifi_update_overlay(void)
{
  auto& d = M5.Display;
  char text[32];
  const uint8_t state = kp::system_registry->runtime_info.getWiFiOtaProgress();
  static constexpr uint32_t bg = 0x08080Cu;
  static constexpr uint32_t frame = 0x40A0FFu;
  static constexpr int32_t status_y = 152;
  static constexpr int32_t progress_x = 30;
  static constexpr int32_t progress_y = 176;
  static constexpr int32_t progress_w = 180;
  static constexpr int32_t progress_h = 10;

  d.startWrite();
  if (!wifi_update_overlay_drawn) {
    d.fillScreen(bg);
    d.drawRect(0, 0, d.width(), d.height(), frame);
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(0xFFFFFFu, bg);
    d.drawString("FIRMWARE UPDATE", d.width() / 2, 120);
    d.drawRect(progress_x - 1, progress_y - 1, progress_w + 2, progress_h + 2, 0x305070u);
    wifi_update_overlay_drawn = true;
  }

  // OTA進捗は1%ごとに届く。文字列とバーの狭い領域だけを更新し、画面を暗転させない。
  d.fillRect(16, status_y - 12, d.width() - 32, 24, bg);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextColor(0xA0D0FFu, bg);
  d.drawString(wifi_update_text(state, text, sizeof(text)), d.width() / 2, status_y);

  d.fillRect(progress_x, progress_y, progress_w, progress_h, 0x101820u);
  if (state <= 100) {
    int32_t filled = (progress_w * state) / 100;
    if (filled > 0) { d.fillRect(progress_x, progress_y, filled, progress_h, frame); }
  }

  d.fillRect(12, 202, d.width() - 24, 22, bg);
  if (kp::system_registry->wifi_control.getOperation() == kp::def::command::wifi_operation_t::wfop_ota_begin) {
    d.setTextColor(0xC0C0D0u, bg);
    d.drawString("Back / Exit: Cancel", d.width() / 2, 213);
  }
  d.endWrite();
}

static void start_wifi_update(void)
{
  if (wifi_update_active) { return; }
#if !defined(M5UNIFIED_PC_BUILD)
  if (!kp::task_wifi_t::hasSavedSTAConfig()) {
    show_status_message("Wi-Fi Setup required", 1800, false);
    draw_menu(true);
    return;
  }
#endif
  stop_all_audio();
  wifi_setup_active = false;
  wifi_setup_qr_active = false;
  wifi_file_server_qr_active = false;
  ui_surface_exclusive = false;
  wifi_qr_preparing = false;
  wifi_update_active = true;
  wifi_update_finished_msec = 0;
  wifi_update_overlay_drawn = false;
  wifi_update_last_state = (uint8_t)kp::def::command::wifi_ota_state_t::ota_connecting;
  menu_visible = false;
  // Menu keypad LEDs are contextual. Restore the retained performance/edit
  // palette before the normal grid becomes visible again.
  update_all_leds();
  auto reg = kp::system_registry;
  begin_wifi_radio_request(wifi_radio_request_t::ota);
  reg->runtime_info.setWiFiOtaProgress(kp::def::command::wifi_ota_state_t::ota_connecting);
  draw_wifi_update_overlay();
}

static void cancel_wifi_update(void)
{
  if (!wifi_update_active) { return; }
  auto reg = kp::system_registry;
  // OTA HTTPタスクへ渡す前の接続待ちだけは安全に中断できる。
  if (wifi_radio_request != wifi_radio_request_t::ota
   && reg->wifi_control.getOperation() != kp::def::command::wifi_operation_t::wfop_ota_begin) { return; }
  disable_wifi_and_clear_indicator();
  wifi_update_active = false;
  wifi_update_overlay_drawn = false;
  menu_visible = true;
  menu_page = menu_page_t::wifi;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  // 更新画面はLCD全体を直接塗っているため、メニューだけでなく背景、ヘッダ、
  // パッドを先に復元して残像を残さない。
  draw_all();
  show_status_message("Update cancelled", 1600, false);
  draw_menu(true);
}

static void stop_file_server_session(void)
{
  auto reg = kp::system_registry;
  reg->wifi_control.setWebServerMode(kp::def::command::webserver_mode_t::ws_disable);
  disable_wifi_and_clear_indicator();
  wifi_setup_active = false;
  wifi_file_server_qr_active = false;
  ui_surface_exclusive = false;
  wifi_qr_preparing = false;
  wifi_file_server_client_connected = false;
  wifi_file_server_connect_deadline_msec = 0;
  wifi_setup_qr_active = false;
  wifi_setup_waiting_for_connection = false;
  wifi_setup_is_wps = false;
  reset_wifi_qr_canvas();
  menu_visible = false;
  clear_status_message(false);
  draw_all();
}

static void fail_file_server_connection(void)
{
  stop_file_server_session();
  menu_visible = true;
  menu_page = menu_page_t::wifi;
  menu_cursor = 2; // File Server
  menu_depth = menu_page_depth(menu_page);
  draw_all();
  show_status_message("Wi-Fi connection failed", 2400, false);
  draw_menu(true);
}

static void service_wifi_update(void)
{
  if (!wifi_update_active) { return; }
  const uint8_t state = kp::system_registry->runtime_info.getWiFiOtaProgress();
  if (state != wifi_update_last_state) {
    wifi_update_last_state = state;
    draw_wifi_update_overlay();
  }
  using namespace kp::def::command;
  const bool complete = state == (uint8_t)wifi_ota_state_t::ota_already_up_to_date
                     || state == (uint8_t)wifi_ota_state_t::ota_connection_error
                     || state == (uint8_t)wifi_ota_state_t::ota_wifi_connection_error
                     || state == (uint8_t)wifi_ota_state_t::ota_catalog_error
                     || state == (uint8_t)wifi_ota_state_t::ota_no_matching_firmware
                     || state == (uint8_t)wifi_ota_state_t::ota_update_failed;
  if (!complete) { return; }
  if (wifi_update_finished_msec == 0) {
    wifi_update_finished_msec = M5.millis();
    disable_wifi_and_clear_indicator();
    return;
  }
  if (M5.millis() - wifi_update_finished_msec < 1500) { return; }
  wifi_update_active = false;
  wifi_update_overlay_drawn = false;
  menu_visible = true;
  menu_page = menu_page_t::wifi;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  // 全画面の更新オーバーレイから戻る際は通常UIを全面再描画する。
  draw_all();
  const char* result = state == (uint8_t)wifi_ota_state_t::ota_already_up_to_date
    ? "Up to date"
    : state == (uint8_t)wifi_ota_state_t::ota_wifi_connection_error
      ? "Wi-Fi connection failed"
      : state == (uint8_t)wifi_ota_state_t::ota_catalog_error
        ? "Catalog request failed"
        : state == (uint8_t)wifi_ota_state_t::ota_no_matching_firmware
          ? "No sampler update found" : "Update failed";
  show_status_message(result, 1800, false);
  draw_menu(true);
}

// 通常メニューを指定スプライトへ描く。表示への転送を分離することで、ページ遷移時に
// 現在ページと遷移先ページをそれぞれ一度だけレンダリングできる。
static const char* connected_input_name(void)
{
  switch (external_input_mode) {
  case external_input_mode_t::usb_midi_host: return "USB MIDI Controller";
  case external_input_mode_t::usb_midi_device: return "USB MIDI Computer";
  case external_input_mode_t::usb_keyboard: return "USB Keyboard";
  case external_input_mode_t::ble_midi: return "BLE MIDI";
  case external_input_mode_t::usb_gamepad: return "USB Gamepad";
  case external_input_mode_t::off:
  default: return "Off";
  }
}

static void draw_connected_device_info(M5Canvas& d)
{
  bool central = false;
  bool peripheral = false;
  uint8_t subscription = 0;
  task_midi.getBLEMidiConnectionDiagnostic(&central, &peripheral, &subscription);
  char device_name[24] = {};
  task_midi.getBLEMidiCentralDeviceName(device_name, sizeof(device_name));

  const bool is_ble = external_input_mode == external_input_mode_t::ble_midi;
  const auto usb_state = kp::system_registry->runtime_info.getMidiPortStateUSB();
  const bool usb_connected = usb_state == kp::def::command::midiport_info_t::mp_connected;
  const bool connected = is_ble ? (central || peripheral) : usb_connected;
  const char* state = external_input_mode == external_input_mode_t::off ? "Disabled"
                    : usb_host_waiting_for_pc_disconnect ? "Disconnect PC"
                    : is_ble ? (connected ? "Connected" : "Searching")
                    : !task_midi.isUSBStarted() ? "Starting"
                    : usb_connected ? "Connected" : "Waiting device";

  d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_left);
  d.setTextColor(0xA0B0C8u, 0x08080Cu);
  d.drawString("INPUT", 12, 20);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(connected_input_name(), 12, 40);
  d.setTextColor(connected ? 0x80FFD0u : 0xD0B080u, 0x08080Cu);
  d.drawString(state, 12, 66);

  d.drawFastHLine(12, 78, 216, 0x303048u);
  d.setTextColor(0xA0B0C8u, 0x08080Cu);
  d.setTextSize(1);
  d.drawString("DEVICE", 12, 94);

  const char* shown_device_name = device_name[0] ? device_name
                                : (is_ble && ble_preferred_name[0]) ? ble_preferred_name
                                : (is_ble ? "No device selected"
                                : usb_connected ? "USB MIDI device" : "-");
  d.setTextColor(0x80D0FFu, 0x08080Cu);
  d.setTextSize(1, 2);
  if (d.textWidth(shown_device_name) > 216) { d.setTextSize(0.75f, 2); }
  d.drawString(shown_device_name, 12, 125);
}

static constexpr int menu_row_h = 32;

static void render_menu_item_row(M5Canvas& d, int index, int y, size_t count,
                                 const sampler_menu_item_t* items, bool dynamic,
                                 bool paint_background = true)
{
  if (index < 0 || index >= (int)count || y <= -menu_row_h || y >= menu_area_h) { return; }
  const bool selected = index == menu_cursor;
  if (paint_background) {
    d.fillRect(0, y, M5.Display.width(), menu_row_h, 0x08080Cu);
    if (selected) { d.fillRoundRect(5, y + 1, 230, menu_row_h - 3, 4, 0x303058u); }
  }
  auto set_row_color = [&](uint32_t color) {
    if (paint_background) {
      d.setTextColor(color, selected ? 0x303058u : 0x08080Cu);
    } else {
      d.setTextColor(color);
    }
  };
  set_row_color(selected ? 0xFFFFFFu : 0xC0C0D0u);
  d.setTextDatum(m5gfx::textdatum_t::middle_left);
  char label[48];
  if (dynamic) {
    char item_label[40];
    menu_dynamic_label(index, item_label, sizeof(item_label));
    snprintf(label, sizeof(label), "%u %s", (unsigned)(index + 1), item_label);
  } else {
    snprintf(label, sizeof(label), "%u %s", (unsigned)(index + 1), items[index].label);
  }
  d.drawString(label, 10, y + menu_row_h / 2);
  if (dynamic) {
    if (kit_edit_state == kit_edit_state_t::select_wav
     || kit_edit_state == kit_edit_state_t::select_bgm_wav
     || kit_edit_state == kit_edit_state_t::select_kit_file) {
      if (index < (int)kit_wav_list.size()
       && kit_wav_list[index].filename.rfind("builtin:", 0) != 0) {
        d.setTextDatum(m5gfx::textdatum_t::middle_right);
        set_row_color(0x80D0FFu);
        d.drawString("SD", 230, y + menu_row_h / 2);
      }
    }
  } else if (items[index].kind == menu_item_kind_t::submenu) {
    d.setTextDatum(m5gfx::textdatum_t::middle_right);
    performance_page_t target;
    synth_tone_source_t source;
    const bool source_branch = (menu_page == menu_page_t::synth_melody_sound
                             || menu_page == menu_page_t::synth_bass_sound
                             || menu_page == menu_page_t::synth_chord_sound)
                            && synth_source_branch_for_page(items[index].child, &target, &source);
    if (source_branch && page_settings(target).source == source) {
      set_row_color(0x60B8FFu);
      d.drawString("※", 230, y + menu_row_h / 2);
    } else {
      d.drawString(">", 230, y + menu_row_h / 2);
    }
  } else if (menu_page == menu_page_t::input_source
          && index == (int)external_input_mode) {
    d.setTextDatum(m5gfx::textdatum_t::middle_right);
    set_row_color(0x80FFD0u);
    d.drawString("*", 230, y + menu_row_h / 2);
  } else if (items[index].kind == menu_item_kind_t::value) {
    d.setTextDatum(m5gfx::textdatum_t::middle_right);
    set_row_color(0x80D0FFu);
    const int value = menu_value_get(items[index].value);
    d.drawString(menu_value_text(items[index].value, value), 230, y + menu_row_h / 2);
  }
}

static bool render_menu_content(M5Canvas& d, int scroll_px = 0)
{
  if (!menu_visible) { return false; }
  if (menu_page == menu_page_t::connection_info) {
    draw_connected_device_info(d);
    return true;
  }
  size_t count = menu_dynamic_list_active() ? menu_dynamic_count() : 0;
  const auto* items = menu_dynamic_list_active() ? nullptr : menu_items(menu_page, &count);
  // メニュー画面ではモード色の外枠は表示しない
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad) {
    d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
    draw_menu_wait_pad(d, "Import Sample", "Press Pad", kit_pending_wav_name);
    return true;
  }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) {
    d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
    char target[24];
    snprintf(target, sizeof(target), "Assign to P%u",
             kit_shortcut_target_pad >= 0
               ? (unsigned)pad_display_number((uint8_t)kit_shortcut_target_pad) : 0u);
    draw_menu_wait_pad(d, "Import Sample", target, kit_pending_wav_name);
    return true;
  }
  if (kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
    draw_menu_wait_pad(d, "Clear Pad", "Press Pad", "sample will be removed");
    return true;
  }
  if (count == 0) { return false; }
  if (menu_cursor >= count) { menu_cursor = count - 1; }
  d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
  d.setFont(&fonts::efontJA_16_b);
  // かんぷれと同じ縦2倍文字。見出しをヘッダーへ退避しているため、
  // 148pxの本文に4行と次の項目の半行を常に残せる。
  d.setTextSize(1, 2);

  int first = menu_first_visible(menu_cursor);
  for (int row = 0; row < 5; ++row) {
    int index = first + row;
    int y = row * menu_row_h + scroll_px;
    render_menu_item_row(d, index, y, count, items, menu_dynamic_list_active());
  }
  if (status_message_visible()) {
    const int status_h = status_message_busy ? 44 : tab_h + 2;
    const int status_y = tab_y - menu_area_y - status_h;
    d.fillRoundRect(8, status_y, 224, status_h, 5, 0x202030u);
    d.setTextColor(0xFFFFFFu, 0x202030u);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.drawString(status_message, 120, status_message_busy ? status_y + 11 : status_y + status_h / 2);
    if (status_message_busy) {
      const uint8_t phase = (uint8_t)(((M5.millis() - status_message_anim_msec) / 180u) & 3u);
      for (int i = 0; i < 4; ++i) {
        const bool active = i == phase;
        d.fillCircle(96 + i * 16, status_y + 32, active ? 4 : 2,
                     active ? 0x70B8FFu : 0x384858u);
      }
    }
    d.drawRoundRect(8, status_y, 224, status_h, 5, 0x606080u);
  }
  return true;
}

static void draw_menu_content(int scroll_px = 0, int x_offset = 0)
{
  if (learn_state != learn_state_t::idle) {
    draw_all();
    draw_learn_overlay();
    return;
  }
  if (wifi_setup_qr_active || wifi_file_server_qr_active) {
    if (wifi_qr_preparing) { draw_wifi_qr_preparing(); }
    else { draw_wifi_setup_qr(); }
    return;
  }
  draw_menu_header();
  if (render_menu_content(menu_canvas, scroll_px)) {
    menu_canvas.pushSprite(x_offset, menu_area_y);
  }
}

static void draw_menu(bool redraw_keypad)
{
  draw_menu_content();
  if (redraw_keypad && !wifi_setup_qr_active && !wifi_file_server_qr_active) { draw_menu_keypad(true); }
}

static void service_wifi_setup_qr(void)
{
  if ((!wifi_setup_qr_active && !wifi_file_server_qr_active)
   || !menu_visible || kp::system_registry == nullptr) { return; }
  const bool ready = wifi_file_server_qr_active
    ? wifi_sta_connected()
    : kp::system_registry->runtime_info.getWiFiAPInfo() != kp::def::command::wifi_ap_info_t::wai_off;
  if (wifi_qr_preparing) {
    if (!ready) {
      if (wifi_file_server_qr_active && wifi_file_server_connect_deadline_msec != 0
       && (int32_t)(M5.millis() - wifi_file_server_connect_deadline_msec) >= 0) {
        fail_file_server_connection();
      }
      return;
    }
    wifi_file_server_connect_deadline_msec = 0;
    wifi_qr_preparing = false;
    wifi_setup_qr_dirty = true;
    draw_menu(false);
    return;
  }
  static bool last_file_server_client_connected = false;
  static bool last_file_server_sta_connected = false;
  const bool client_changed = wifi_file_server_qr_active
                           && last_file_server_client_connected != wifi_file_server_client_connected;
  const bool sta_changed = wifi_file_server_qr_active
                        && last_file_server_sta_connected != wifi_sta_connected();
  if (!wifi_file_server_qr_active) {
    last_file_server_client_connected = false;
    last_file_server_sta_connected = false;
  }
  const bool web_page = wifi_setup_qr_web_page
                     || kp::system_registry->runtime_info.getWiFiStationCount() != 0;
  if (wifi_setup_qr_dirty || wifi_setup_qr_web_page != web_page || client_changed || sta_changed) {
    last_file_server_client_connected = wifi_file_server_client_connected;
    last_file_server_sta_connected = wifi_sta_connected();
    wifi_setup_qr_dirty = true;
    draw_menu(false);
  }
}

static bool wifi_sta_connected(void)
{
  auto state = kp::system_registry->runtime_info.getWiFiSTAInfo();
  return state >= kp::def::command::wifi_sta_info_t::wsi_signal_1
      && state <= kp::def::command::wifi_sta_info_t::wsi_signal_4;
}

static void finish_wifi_setup(void)
{
  // 接続確認が取れた時点でWi-Fiを解放する。保存済みの設定はNVSに残るため、
  // File Server/Updateを使う時だけ改めてSTAへ接続できる。
  auto reg = kp::system_registry;
  disable_wifi_and_clear_indicator();
  wifi_setup_active = false;
  wifi_setup_qr_active = false;
  wifi_qr_preparing = false;
  wifi_setup_waiting_for_connection = false;
  wifi_setup_connect_deadline_msec = 0;
  wifi_setup_is_wps = false;
  reset_wifi_qr_canvas();
  draw_all();
  show_status_message("Wi-Fi Connected", 2400, false);
  draw_menu(true);
}

static void fail_wifi_setup_connection(void)
{
  disable_wifi_and_clear_indicator();
  wifi_setup_active = false;
  wifi_setup_qr_active = false;
  wifi_file_server_qr_active = false;
  ui_surface_exclusive = false;
  wifi_qr_preparing = false;
  wifi_setup_waiting_for_connection = false;
  wifi_setup_connect_deadline_msec = 0;
  wifi_setup_is_wps = false;
  reset_wifi_qr_canvas();
  menu_visible = true;
  menu_page = menu_page_t::wifi_setup;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  draw_all();
  show_status_message("Wi-Fi failed: check password", 2600, false);
  draw_menu(true);
}

static void service_wifi_setup_result(void)
{
  if (!wifi_setup_active || !menu_visible || kp::system_registry == nullptr) { return; }
  // BLE停止待ちの間はまだWi-Fiセットアップを開始していない。
  if (wifi_radio_request == wifi_radio_request_t::setup_ap
   || wifi_radio_request == wifi_radio_request_t::setup_wps) { return; }
  auto operation = kp::system_registry->wifi_control.getOperation();

  // スマホ設定の保存後は、APを閉じてSTA-onlyで接続する。操作終了だけでは
  // 成功と扱わず、IP取得が確認できるまで本体側で待機する。
  if (!wifi_setup_is_wps && operation == kp::def::command::wifi_operation_t::wfop_disable) {
    if (!wifi_setup_waiting_for_connection) {
      wifi_setup_waiting_for_connection = true;
      wifi_setup_connect_deadline_msec = M5.millis() + 15000;
      wifi_setup_qr_active = false;
      wifi_qr_preparing = false;
      reset_wifi_qr_canvas();
      draw_all();
      show_status_message("Wi-Fi connecting...", 0, false);
      draw_menu(true);
    }
    if (wifi_sta_connected()) { finish_wifi_setup(); }
    else if (wifi_setup_connect_deadline_msec != 0
          && (int32_t)(M5.millis() - wifi_setup_connect_deadline_msec) >= 0) {
      fail_wifi_setup_connection();
    }
    return;
  }
  if (wifi_setup_is_wps && operation == kp::def::command::wifi_operation_t::wfop_disable) {
    if (!wifi_setup_waiting_for_connection) {
      wifi_setup_waiting_for_connection = true;
      show_status_message("WPS connecting...", 0, false);
      draw_menu(true);
    }
    if (wifi_sta_connected()) { finish_wifi_setup(); }
  }
}

static void draw_menu_scroll(int old_cursor, int new_cursor)
{
  if (!menu_visible || old_cursor == new_cursor) {
    draw_menu();
    return;
  }
  int old_first = menu_first_visible((uint8_t)old_cursor);
  int new_first = menu_first_visible((uint8_t)new_cursor);
  const uint32_t now = M5.millis();
  static uint32_t previous_move_msec = 0;
  const bool rapid_input = previous_move_msec != 0 && now - previous_move_msec < 55;
  previous_move_msec = now;
  const bool can_animate = !rapid_input && std::abs(new_cursor - old_cursor) == 1
                        && menu_page != menu_page_t::connection_info
                        && kit_edit_state != kit_edit_state_t::assign_wait_pad
                        && kit_edit_state != kit_edit_state_t::clear_wait_pad
                        && !status_message_visible() && menu_canvas.getBuffer() != nullptr;
  if (can_animate) {
    size_t count = menu_dynamic_list_active() ? menu_dynamic_count() : 0;
    const bool dynamic = menu_dynamic_list_active();
    const auto* items = dynamic ? nullptr : menu_items(menu_page, &count);
    const int old_scroll = old_first * menu_row_h;
    const int new_scroll = new_first * menu_row_h;
    const int old_focus = old_cursor * menu_row_h - old_scroll;
    const int new_focus = new_cursor * menu_row_h - new_scroll;
    static constexpr int frames = 2;
    const bool focus_only = old_first == new_first;
    const int dirty_y = std::min(old_focus, new_focus);
    const int dirty_bottom = std::max(old_focus, new_focus) + menu_row_h;
    const int dirty_h = std::min((int)menu_area_h, dirty_bottom) - std::max(0, dirty_y);
    for (int frame = 1; frame <= frames; ++frame) {
      const int scroll = old_scroll + (new_scroll - old_scroll) * frame / frames;
      const int focus = old_focus + (new_focus - old_focus) * frame / frames;
      if (focus_only) {
        menu_canvas.fillRect(0, dirty_y, M5.Display.width(), dirty_h, 0x08080Cu);
      } else {
        menu_canvas.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
      }
      menu_canvas.fillRoundRect(5, focus + 1, 230, menu_row_h - 3, 4, 0x303058u);
      menu_canvas.setFont(&fonts::efontJA_16_b);
      menu_canvas.setTextSize(1, 2);
      if (focus_only) {
        render_menu_item_row(menu_canvas, old_cursor, old_focus,
                             count, items, dynamic, false);
        render_menu_item_row(menu_canvas, new_cursor, new_focus,
                             count, items, dynamic, false);
      } else {
        const int first_index = std::max(0, scroll / menu_row_h);
        for (int index = first_index; index < first_index + 6; ++index) {
          render_menu_item_row(menu_canvas, index, index * menu_row_h - scroll,
                               count, items, dynamic, false);
        }
      }
      if (focus_only) {
        const int y = std::max(0, dirty_y);
        const int width = M5.Display.width();
        const auto* pixels = static_cast<const m5gfx::swap565_t*>(menu_canvas.getBuffer());
        M5.Display.pushImage(0, menu_area_y + y, width, dirty_h, pixels + y * width);
      } else {
        menu_canvas.pushSprite(0, menu_area_y);
      }
    }
    return;
  }
  if (old_first == new_first && menu_page != menu_page_t::connection_info
   && kit_edit_state != kit_edit_state_t::assign_wait_pad
   && kit_edit_state != kit_edit_state_t::clear_wait_pad
   && !status_message_visible() && menu_canvas.getBuffer() != nullptr) {
    size_t count = menu_dynamic_list_active() ? menu_dynamic_count() : 0;
    const auto* items = menu_dynamic_list_active() ? nullptr : menu_items(menu_page, &count);
    menu_canvas.setFont(&fonts::efontJA_16_b);
    menu_canvas.setTextSize(1, 2);
    const int old_row = old_cursor - old_first;
    const int new_row = new_cursor - new_first;
    render_menu_item_row(menu_canvas, old_cursor, old_row * menu_row_h,
                         count, items, menu_dynamic_list_active());
    render_menu_item_row(menu_canvas, new_cursor, new_row * menu_row_h,
                         count, items, menu_dynamic_list_active());

    const int width = M5.Display.width();
    const auto* pixels = static_cast<const m5gfx::swap565_t*>(menu_canvas.getBuffer());
    auto push_row = [&](int row) {
      const int y = row * menu_row_h;
      const int height = std::min(menu_row_h, (int)menu_area_h - y);
      if (height > 0) {
        M5.Display.pushImage(0, menu_area_y + y, width, height, pixels + y * width);
      }
    };
    M5.Display.startWrite();
    push_row(old_row);
    push_row(new_row);
    M5.Display.endWrite();
    return;
  }
  // Once the visible window changes, render only the latest destination.
  // Encoder deltas are already coalesced, so intermediate full-screen frames
  // would merely make the cursor trail behind the physical control.
  draw_menu();
}

static void draw_menu_page_transition(int direction)
{
  if (!menu_visible || !menu_transition_canvas_ready
   || wifi_setup_qr_active || wifi_file_server_qr_active
   || learn_state != learn_state_t::idle || ble_midi_cache_guard_active()) {
    draw_menu(true);
    return;
  }
  const int w = M5.Display.width();
  const int dir = direction >= 0 ? 1 : -1;
  // Move twice as far per frame as the former six-step transition. The page
  // direction remains readable while the hierarchy change completes in
  // roughly half the time.
  static constexpr const int frames = 3;

  // menu_canvas は遷移前ページを保持したまま、新しい状態を別面へ一度だけ描く。
  if (!render_menu_content(menu_transition_canvas)) {
    draw_menu(true);
    return;
  }
  for (int i = 0; i <= frames; ++i) {
    const int offset = i * w / frames;
    // 旧面と新面は常に境界で接して表示領域を完全に覆う。ここで背景を
    // 消すと、その塗り潰し転送がLCD上で見えて点滅するため行わない。
    if (dir > 0) {
      menu_canvas.pushSprite(-offset, menu_area_y);
      menu_transition_canvas.pushSprite(w - offset, menu_area_y);
    } else {
      menu_canvas.pushSprite(offset, menu_area_y);
      menu_transition_canvas.pushSprite(-w + offset, menu_area_y);
    }
    M5.delay(3);
  }
  // The last animation frame already placed the destination on the LCD.
  // Keep it as the next transition's source with a memory copy instead of
  // rendering and transferring the entire menu area for a second time.
  draw_menu_header();
  if (menu_canvas.getBuffer() != nullptr && menu_transition_canvas.getBuffer() != nullptr) {
    memcpy(menu_canvas.getBuffer(), menu_transition_canvas.getBuffer(),
           (size_t)w * menu_area_h * sizeof(uint16_t));
  } else {
    draw_menu_content();
  }
  // The selected row may now be a value.  Update +/- immediately on both
  // entering and returning from a page, not only after moving the cursor.
  draw_menu_keypad();
}

static void menu_open(void)
{
  clear_menu_preview();
  wave_transfer_active = false;
  wave_transfer_full_frame = false;
  menu_visible = true;
  menu_page = menu_page_t::root;
  menu_cursor = 0;
  menu_depth = 0;
  input_assignment_list_active = false;
  input_assignment_list.clear();
  kit_edit_state = kit_edit_state_t::idle;
  clear_status_message(false);
  menu_sound_navigate(0);
  menu_sound_cursor(1);
  draw_menu_header(true);
  draw_menu(true);
}

static void menu_close(void)
{
  clear_menu_preview();
  if (ble_device_ui_state == ble_device_ui_state_t::scanning
   || ble_device_ui_state == ble_device_ui_state_t::list
   || ble_device_ui_state == ble_device_ui_state_t::confirm) {
    task_midi.cancelBLEMidiScan();
  }
  ble_device_ui_state = ble_device_ui_state_t::idle;
  if (wifi_setup_active) {
    disable_wifi_and_clear_indicator();
    wifi_setup_active = false;
    wifi_setup_qr_active = false;
    wifi_file_server_qr_active = false;
    ui_surface_exclusive = false;
    wifi_qr_preparing = false;
    wifi_setup_waiting_for_connection = false;
    wifi_setup_is_wps = false;
    reset_wifi_qr_canvas();
  }
  menu_visible = false;
  // The menu has its own keypad palette. Restore the underlying page palette
  // before returning to the performance surface.
  update_all_leds();
  // A quick Exit should still persist the most recent value.  During normal
  // editing the quiet-time service above has already completed this write.
  if (menu_settings_save_pending && kp::system_registry != nullptr) {
    kp::system_registry->save();
    menu_settings_save_pending = false;
  }
  input_assignment_list_active = false;
  input_assignment_list.clear();
  clear_status_message(false);
  menu_depth = 0;
  kit_edit_state = kit_edit_state_t::idle;
  kit_shortcut_target_pad = -1;
  menu_sound_navigate(3);
  draw_all();
  draw_header(true);
}

static void cancel_learn(bool exit_menu)
{
  learn_state = learn_state_t::idle;
  learn_target = (int16_t)midi_assign_target_t::none;
  learn_target_label[0] = 0;
  learn_target_deadline_msec = 0;
  if (exit_menu) {
    menu_close();
    return;
  }

  // LearnはInput Assignから始めるため、Backはその項目へ戻す。
  menu_visible = true;
  menu_page = menu_page_t::input_assign;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  menu_sound_navigate(2);
  draw_menu(true);
}

static void service_learn_target_timeout(uint32_t now)
{
  if (learn_state != learn_state_t::waiting_target || learn_target_deadline_msec == 0
   || (int32_t)(now - learn_target_deadline_msec) < 0) {
    return;
  }
  learn_state = learn_state_t::idle;
  learn_target = (int16_t)midi_assign_target_t::none;
  learn_target_label[0] = 0;
  learn_target_deadline_msec = 0;
  menu_visible = true;
  menu_page = menu_page_t::input_assign;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  show_status_message("TIME OUT", 1600, false);
  draw_menu(true);
}

static void begin_ble_device_scan(void)
{
  if (external_input_mode != external_input_mode_t::ble_midi) {
    show_status_message("Select BLE MIDI first", 2000, false);
    draw_menu(true);
    return;
  }
  ble_device_count = 0;
  ble_device_selected = 0;
  ble_device_ui_state = ble_device_ui_state_t::scanning;
  task_midi.requestBLEMidiScan();
  show_loading_message("SCANNING BLE");
  draw_menu(true);
}

static void service_ble_device_ui(uint32_t now)
{
  if (ble_device_ui_state == ble_device_ui_state_t::scanning) {
    const auto state = task_midi.getBLEMidiScanState();
    if (state == kp::task_midi_t::ble_scan_state_t::ready) {
      ble_device_count = task_midi.getBLEMidiScanDevices(ble_device_list, ble_device_list_capacity);
      clear_status_message(false);
      if (ble_device_count == 0) {
        ble_device_ui_state = ble_device_ui_state_t::idle;
        show_status_message("No BLE devices", 1800, false);
      } else {
        ble_device_ui_state = ble_device_ui_state_t::list;
        menu_cursor = 0;
        menu_depth = menu_dynamic_depth();
        menu_sound_cursor(1);
      }
      if (menu_visible) {
        draw_menu_header(true);
        draw_menu(true);
      }
    } else if (state == kp::task_midi_t::ble_scan_state_t::failed) {
      ble_device_ui_state = ble_device_ui_state_t::idle;
      show_status_message("BLE scan failed", 2000, false);
      if (menu_visible) { draw_menu(true); }
    }
    return;
  }

  if (ble_device_ui_state != ble_device_ui_state_t::connecting) { return; }
  bool central = false;
  bool peripheral = false;
  uint8_t subscription = 0;
  task_midi.getBLEMidiConnectionDiagnostic(&central, &peripheral, &subscription);
  (void)peripheral;
  (void)subscription;
  if (central) {
    ble_device_ui_state = ble_device_ui_state_t::idle;
    show_status_message("BLE connected", 1800, false);
    if (menu_visible) { draw_menu(true); }
  } else if ((int32_t)(now - ble_device_connect_deadline_msec) >= 0) {
    ble_device_ui_state = ble_device_ui_state_t::idle;
    show_status_message("BLE connection failed", 2200, false);
    if (menu_visible) { draw_menu(true); }
  }
}

static void menu_back(void)
{
  if (learn_state != learn_state_t::idle) {
    cancel_learn(false);
    return;
  }
  if (!menu_visible) { return; }
  if (ble_device_ui_state == ble_device_ui_state_t::scanning) {
    task_midi.cancelBLEMidiScan();
    ble_device_ui_state = ble_device_ui_state_t::idle;
    clear_status_message(false);
    menu_page = menu_page_t::ble_device;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_header(true);
    draw_menu(true);
    return;
  }
  if (ble_device_ui_state == ble_device_ui_state_t::confirm) {
    ble_device_ui_state = ble_device_ui_state_t::list;
    menu_cursor = ble_device_count == 0 ? 0
                : (uint8_t)std::min(ble_device_selected, ble_device_count - 1);
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(2);
    draw_menu_header(true);
    draw_menu(true);
    return;
  }
  if (ble_device_ui_state == ble_device_ui_state_t::list) {
    task_midi.cancelBLEMidiScan();
    ble_device_ui_state = ble_device_ui_state_t::idle;
    menu_page = menu_page_t::ble_device;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_header(true);
    draw_menu(true);
    return;
  }
  if (input_assignment_list_active) {
    input_assignment_list_active = false;
    input_assignment_list.clear();
    menu_page = menu_page_t::input_assign;
    menu_cursor = 1;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (wifi_setup_active) {
    disable_wifi_and_clear_indicator();
    wifi_setup_active = false;
    wifi_setup_qr_active = false;
    wifi_file_server_qr_active = false;
    ui_surface_exclusive = false;
    wifi_qr_preparing = false;
    wifi_setup_waiting_for_connection = false;
    wifi_setup_is_wps = false;
    reset_wifi_qr_canvas();
    clear_status_message(false);
    // QRモーダルはLCD全体へ直接描画している。かんぷれの無効領域再描画と
    // 同様に、閉じる瞬間だけ通常UIを完全に復元して透明領域の残像を防ぐ。
    draw_all();
    draw_menu(true);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad) {
    clear_menu_preview();
    kit_edit_state = kit_edit_state_t::select_wav;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) {
    clear_menu_preview();
    kit_edit_state = kit_edit_state_t::select_wav;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_tone
   || kit_edit_state == kit_edit_state_t::select_external_pad
   || kit_edit_state == kit_edit_state_t::select_external_pad_base_note) {
    clear_menu_preview();
    const bool page_sound_select = synth_sound_select_active;
    const bool selecting_pad = kit_edit_state == kit_edit_state_t::select_external_pad;
    const bool selecting_base_note = kit_edit_state == kit_edit_state_t::select_external_pad_base_note;
    kit_edit_state = kit_edit_state_t::idle;
    synth_sound_select_active = false;
    if (!page_sound_select) {
      menu_page = menu_page_t::midi_sound;
    } else if (synth_menu_target == performance_page_t::chord) {
      menu_page = selecting_pad || selecting_base_note
        ? menu_page_t::synth_chord_pad : menu_page_t::synth_chord_midi;
    } else if (synth_menu_target == performance_page_t::bass) {
      menu_page = selecting_pad || selecting_base_note
        ? menu_page_t::synth_bass_pad : menu_page_t::synth_bass_midi;
    } else {
      menu_page = selecting_pad || selecting_base_note
        ? menu_page_t::synth_melody_pad : menu_page_t::synth_melody_midi;
    }
    menu_cursor = selecting_base_note ? 1 : 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_kit_file
   || kit_edit_state == kit_edit_state_t::select_kit_save
   || kit_edit_state == kit_edit_state_t::select_wav
   || kit_edit_state == kit_edit_state_t::clear_wait_pad
   || kit_edit_state == kit_edit_state_t::pad_list) {
    kit_edit_state_t prev_state = kit_edit_state;
    const bool shortcut_import = prev_state == kit_edit_state_t::select_wav
                              && kit_shortcut_target_pad >= 0;
    kit_edit_state = kit_edit_state_t::idle;
    if (shortcut_import) {
      kit_shortcut_target_pad = -1;
      menu_close();
      return;
    }
    menu_page = (prev_state == kit_edit_state_t::select_kit_file
              || prev_state == kit_edit_state_t::select_kit_save
              || prev_state == kit_edit_state_t::select_wav)
      ? menu_page_t::kit
      : menu_page_t::kit_edit;
    switch (prev_state) {
    case kit_edit_state_t::select_kit_save: menu_cursor = 1; break;
    case kit_edit_state_t::select_wav: menu_cursor = 2; break;
    case kit_edit_state_t::clear_wait_pad: menu_cursor = 1; break;
    case kit_edit_state_t::pad_list: menu_cursor = 3; break;
    default: menu_cursor = 0; break;
    }
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_bgm_wav) {
    kit_edit_state = kit_edit_state_t::idle;
    menu_page = menu_page_t::loop_bgm;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
    return;
  }
  if (menu_page == menu_page_t::root) {
    menu_close();
  } else {
    menu_page_t current_page = menu_page;
    menu_page = menu_parent_page(current_page);
    menu_cursor = menu_parent_cursor(current_page);
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(2);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(-1);
    draw_menu_keypad();
  }
}

static bool has_lower_suffix(const std::string& n, const char* suffix)
{
  if (!suffix || !suffix[0]) { return true; }
  size_t suffix_len = strlen(suffix);
  if (n.size() < suffix_len) { return false; }
  std::string ext = n.substr(n.size() - suffix_len);
  for (auto& ch : ext) { ch = tolower(ch); }
  return ext == suffix;
}

static bool is_audio_file_name(const std::string& name)
{
  return has_lower_suffix(name, ".wav") || has_lower_suffix(name, ".mp3");
}

// Stored paths remain unchanged. Only the selector label identifies its source.
static std::string sampler_file_display_name(const std::string& source_name, const char* source_dir)
{
  const bool builtin = source_name.rfind("builtin:", 0) == 0;
  std::string name = builtin ? source_name.substr(8) : source_name;
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos) { name.resize(dot); }
  if (builtin) { return name; }

  std::string folder = source_dir ? source_dir : "";
  // The selected root is managed in the File Editor.  Hardware selectors
  // therefore show only the user-facing subfolder, not /sampler/samples etc.
  static constexpr const char* roots[] = {
    "/sampler/samples", "/sampler/loops", "/sampler/kits"
  };
  for (const char* root : roots) {
    const size_t root_len = strlen(root);
    if (folder.compare(0, root_len, root) == 0
     && (folder.size() == root_len || folder[root_len] == '/')) {
      folder.erase(0, root_len);
      break;
    }
  }
  while (!folder.empty() && folder.front() == '/') { folder.erase(0, 1); }
  return folder.empty() ? name : folder + "/" + name;
}

static bool load_menu_file_list_from(const char* dir, const char* suffix)
{
  kit_wav_list.clear();
  if (!dir || !dir[0]) { return false; }
  kp::storage_sd.getFileList(kit_wav_list, dir, "");
  kit_wav_list.erase(std::remove_if(kit_wav_list.begin(), kit_wav_list.end(),
    [suffix](const kp::file_info_string_t& f) { return !has_lower_suffix(f.filename, suffix); }), kit_wav_list.end());
  std::sort(kit_wav_list.begin(), kit_wav_list.end(),
    [](const kp::file_info_string_t& a, const kp::file_info_string_t& b) { return a.filename < b.filename; });
  if (kit_wav_list.empty()) { return false; }
  snprintf(kit_wav_dir, sizeof(kit_wav_dir), "%s", dir);
  return true;
}

static bool load_menu_audio_file_list_from(const char* dir)
{
  kit_wav_list.clear();
  if (!dir || !dir[0]) { return false; }
  kp::storage_sd.getFileList(kit_wav_list, dir, "");
  kit_wav_list.erase(std::remove_if(kit_wav_list.begin(), kit_wav_list.end(),
    [](const kp::file_info_string_t& f) { return !is_audio_file_name(f.filename); }), kit_wav_list.end());
  std::sort(kit_wav_list.begin(), kit_wav_list.end(),
    [](const kp::file_info_string_t& a, const kp::file_info_string_t& b) { return a.filename < b.filename; });
  if (kit_wav_list.empty()) { return false; }
  snprintf(kit_wav_dir, sizeof(kit_wav_dir), "%s", dir);
  return true;
}

static bool begin_kit_assign_wav(int target_pad = -1)
{
  kit_shortcut_target_pad = target_pad;
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  ensure_sampler_sd_dirs();
  load_menu_audio_file_list_from(sampler_sd_folders[0]);
  // SDの選択フォルダに加え、内蔵音色も同じ一覧から直接選べる。
  for (size_t i = 0; i < builtin_sample_count; ++i) {
    kit_wav_list.insert(kit_wav_list.begin() + i, { std::string("builtin:") + builtin_samples[i].name, 0 });
  }
  if (kit_wav_list.empty()) {
    show_status_message("No audio", 1600, true);
    return false;
  }
  kit_edit_state = kit_edit_state_t::select_wav;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
  return true;
}

static bool begin_kit_file_select(void)
{
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  ensure_sampler_sd_dirs();
  load_menu_file_list_from(sampler_sd_folders[2], ".json");
  if (kit_wav_list.empty()) {
    show_status_message("No kit", 1600, true);
    return false;
  }
  kit_edit_state = kit_edit_state_t::select_kit_file;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
  return true;
}

static void kit_path_stem(const char* path, char* out, size_t out_len)
{
  if (out_len == 0) { return; }
  const char* name = path ? strrchr(path, '/') : nullptr;
  name = name ? name + 1 : (path ? path : "");
  snprintf(out, out_len, "%s", name);
  char* dot = strrchr(out, '.');
  if (dot && strcmp(dot, ".json") == 0) { *dot = 0; }
}

static void kit_path_directory(const char* path, char* out, size_t out_len)
{
  if (out_len == 0) { return; }
  snprintf(out, out_len, "%s", path && path[0] ? path : sampler_sd_folders[2]);
  char* slash = strrchr(out, '/');
  if (path && path[0] && slash) { *slash = 0; }
}

static void make_unique_kit_path(const char* directory, const char* stem,
                                 const char* suffix, char* out, size_t out_len)
{
  std::string base = std::string(directory) + "/" + stem + suffix;
  for (uint8_t extra = 0; extra < 32; ++extra) {
    std::string candidate = base + std::string(extra, '_') + ".json";
    if (candidate.size() >= out_len) { break; }
    if (kp::storage_sd.getFileSize(candidate.c_str()) < 0) {
      snprintf(out, out_len, "%s", candidate.c_str());
      return;
    }
  }
  // 異常に多い重複時も、既存ファイルを上書きしない連番へ退避する。
  snprintf(out, out_len, "%s/%s%s%lu.json", directory, stem, suffix,
           (unsigned long)M5.millis());
}

static bool begin_kit_save(void)
{
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  if (!ensure_sampler_sd_dirs()) {
    show_status_message("SD unavailable", 1600, true);
    return false;
  }

  kit_save_candidate_count = 0;
  char directory[96];
  char stem[48];
  kit_path_directory(current_kit_path, directory, sizeof(directory));
  kit_path_stem(current_kit_path, stem, sizeof(stem));
  if (!stem[0]) { snprintf(stem, sizeof(stem), "NEW_KIT"); }

  if (current_kit_path[0] && kit_save_candidate_count < 4) {
    auto& candidate = kit_save_candidates[kit_save_candidate_count++];
    snprintf(candidate.path, sizeof(candidate.path), "%s", current_kit_path);
    snprintf(candidate.label, sizeof(candidate.label), "Update: %s", stem);
  }

  if (kit_save_candidate_count < 4) {
    auto& candidate = kit_save_candidates[kit_save_candidate_count++];
    make_unique_kit_path(directory, stem, "_", candidate.path, sizeof(candidate.path));
    char copy_stem[48];
    kit_path_stem(candidate.path, copy_stem, sizeof(copy_stem));
    snprintf(candidate.label, sizeof(candidate.label), "Copy: %s", copy_stem);
  }

  if (kit_save_candidate_count < 4) {
    char date_stem[48];
    time_t now = time(nullptr);
    tm* local = localtime(&now);
    auto& candidate = kit_save_candidates[kit_save_candidate_count++];
    if (local && local->tm_year >= 124) {
      snprintf(date_stem, sizeof(date_stem), "%04d%02d%02d_%02d%02d",
               local->tm_year + 1900, local->tm_mon + 1, local->tm_mday,
               local->tm_hour, local->tm_min);
      make_unique_kit_path(directory, date_stem, "", candidate.path, sizeof(candidate.path));
    } else {
      // Wi-Fi未設定などで時計が未取得でも新規保存できる連番名。
      for (uint8_t number = 1; number < 100; ++number) {
        snprintf(date_stem, sizeof(date_stem), "KIT_%02u", (unsigned)number);
        snprintf(candidate.path, sizeof(candidate.path), "%s/%s.json", directory, date_stem);
        if (kp::storage_sd.getFileSize(candidate.path) < 0) { break; }
        candidate.path[0] = 0;
      }
      if (!candidate.path[0]) {
        snprintf(date_stem, sizeof(date_stem), "KIT_99");
        make_unique_kit_path(directory, date_stem, "_", candidate.path, sizeof(candidate.path));
      }
    }
    char new_stem[48];
    kit_path_stem(candidate.path, new_stem, sizeof(new_stem));
    snprintf(candidate.label, sizeof(candidate.label), "New: %s", new_stem);
  }

  if (kit_save_candidate_count < 4) {
    auto& candidate = kit_save_candidates[kit_save_candidate_count++];
    snprintf(candidate.path, sizeof(candidate.path), "%s", sampler_default_kit_path);
    snprintf(candidate.label, sizeof(candidate.label), "Save as Default");
  }

  kit_edit_state = kit_edit_state_t::select_kit_save;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
  return true;
}

static bool begin_background_wav_select(void)
{
  set_background_loop_error("");
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  ensure_sampler_sd_dirs();
  load_menu_audio_file_list_from(sampler_sd_folders[1]);
  for (size_t i = 0; i < builtin_background_loop_count; ++i) {
    kit_wav_list.insert(kit_wav_list.begin() + i,
                        { std::string("builtin:") + builtin_background_loops[i].file, 0 });
  }
  if (kit_wav_list.empty()) {
    show_status_message("No BGM audio", 1600, true);
    return false;
  }
  kit_edit_state = kit_edit_state_t::select_bgm_wav;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
  return true;
}

static void begin_kit_clear_pad(void)
{
  kit_edit_state = kit_edit_state_t::clear_wait_pad;
  menu_depth = menu_dynamic_depth();
  draw_menu_page_transition(1);
  draw_menu_keypad();
}

static void show_kit_pad_list(void)
{
  kit_edit_state = kit_edit_state_t::pad_list;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
}

static void select_kit_wav(void)
{
  if (menu_cursor >= kit_wav_list.size()) { return; }
  const auto& f = kit_wav_list[menu_cursor];
  std::string name = f.filename;
  if (name.rfind("builtin:", 0) == 0) {
    snprintf(kit_pending_wav_path, sizeof(kit_pending_wav_path), "%s", name.c_str());
    snprintf(kit_pending_wav_name, sizeof(kit_pending_wav_name), "%s", name.c_str() + 8);
    play_menu_builtin_preview(kit_pending_wav_path, 2000);
    kit_edit_state = kit_shortcut_target_pad >= 0
      ? kit_edit_state_t::assign_confirm_shortcut : kit_edit_state_t::assign_wait_pad;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return;
  }
  if (name.size() > 4) { name.resize(name.size() - 4); }
  std::string path = std::string(kit_wav_dir) + "/" + f.filename;
  snprintf(kit_pending_wav_path, sizeof(kit_pending_wav_path), "%s", path.c_str());
  snprintf(kit_pending_wav_name, sizeof(kit_pending_wav_name), "%s", name.c_str());
  play_menu_audio_preview(kit_pending_wav_path, 2000);
  kit_edit_state = kit_shortcut_target_pad >= 0
    ? kit_edit_state_t::assign_confirm_shortcut : kit_edit_state_t::assign_wait_pad;
  menu_depth = menu_dynamic_depth();
  menu_sound_navigate(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
}

static void select_background_wav(void)
{
  if (menu_cursor >= kit_wav_list.size()) { return; }
  const auto& f = kit_wav_list[menu_cursor];
  std::string name = f.filename;
  if (name.rfind("builtin:", 0) == 0) {
    clear_menu_preview();
    kit_edit_state = kit_edit_state_t::idle;
    menu_page = menu_page_t::loop_bgm;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    bool ok = load_builtin_background_loop(name.c_str());
    show_status_message(ok ? "Built-in BGM" : "BGM error", 1600, false);
    draw_menu(true);
    return;
  }
  if (name.size() > 4) { name.resize(name.size() - 4); }
  std::string path = std::string(kit_wav_dir) + "/" + f.filename;
  clear_menu_preview();
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::loop_bgm;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  menu_sound_navigate(1);
  show_loading_message();
  bool ok = load_background_loop_file(path.c_str(), name.c_str());
  show_status_message(ok ? "BGM loaded" : background_loop_error, 1600, false);
  draw_menu(true);
}

static void select_kit_file(void)
{
  if (menu_cursor >= kit_wav_list.size()) { return; }
  const auto& f = kit_wav_list[menu_cursor];
  std::string path = std::string(kit_wav_dir) + "/" + f.filename;
  clear_menu_preview();
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::kit;
  menu_cursor = 0;
  menu_depth = menu_page_depth(menu_page);
  menu_sound_navigate(1);
  show_loading_message();
  bool ok = load_kit_file(path.c_str());
  show_status_message(ok ? "Kit loaded" : "Load failed", 1600, false);
  draw_menu(true);
}

static void select_kit_save(void)
{
  if (menu_cursor >= kit_save_candidate_count) { return; }
  const auto& candidate = kit_save_candidates[menu_cursor];
  if (!candidate.path[0]) { return; }

  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::kit;
  menu_cursor = 1;
  menu_depth = menu_page_depth(menu_page);
  menu_sound_navigate(1);
  show_loading_message("SAVING KIT");
  bool saved = save_current_kit(candidate.path);
  const bool saved_default = strcmp(candidate.path, sampler_default_kit_path) == 0;
  // Saving a personal default must not turn it into the current editable
  // filename. A later ordinary Save should offer New/Copy, never overwrite it.
  if (saved && !saved_default) { snprintf(current_kit_path, sizeof(current_kit_path), "%s", candidate.path); }
  char message[64];
  snprintf(message, sizeof(message), saved ? (saved_default ? "Default saved" : "Saved: %s") : "Save failed",
           saved && !saved_default ? strrchr(candidate.path, '/') + 1 : "");
  show_status_message(message, 1800, false);
  draw_menu(true);
}

static void assign_pending_wav_to_pad(uint8_t pad)
{
  const bool shortcut_import = kit_edit_state == kit_edit_state_t::assign_confirm_shortcut
                            && kit_shortcut_target_pad == pad;
  char error[32] = { 0 };
  clear_menu_preview();
  kit_assign_loading_pad = pad;
  // Present the target outline before the synchronous SD/decode path begins.
  draw_menu_keypad(true);
  M5.delay(45);
  show_loading_message();
  bool ok = strncmp(kit_pending_wav_path, "builtin:", 8) == 0
    ? load_builtin_sample_to_pad(pad, kit_pending_wav_path)
    : load_audio_to_pad(pad, kit_pending_wav_path, kit_pending_wav_name, error, sizeof(error));
  char msg[48];
  if (ok) {
    snprintf(msg, sizeof(msg), "Assigned P%u", (unsigned)pad_display_number(pad));
  } else {
    snprintf(msg, sizeof(msg), "%s", error[0] ? error : "Assign failed");
  }
  if (shortcut_import && ok) {
    kit_shortcut_target_pad = -1;
    kit_assign_loading_pad = -1;
    kit_edit_state = kit_edit_state_t::idle;
    menu_close();
    show_status_message(msg, 1600, false);
    request_wave_draw();
    return;
  }
  // 「音源を選ぶ → パッドへ割り当てる」は連続作業なので、成功・失敗を問わず
  // 音源一覧へ一段だけ戻す。ここから別の音源を続けて選べ、Backで初めて
  // Kit > Import Sampleへ戻るため、他の単発メニューとも規則が衝突しない。
  kit_edit_state = kit_edit_state_t::select_wav;
  kit_assign_loading_pad = -1;
  menu_depth = menu_dynamic_depth();
  show_status_message(msg, 1600, false);
  draw_menu_page_transition(-1);
  draw_menu_keypad();
}

static void clear_selected_pad_sample(uint8_t pad)
{
  clear_pad_sample(pad, true);
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::kit_edit;
  menu_cursor = 1;
  char msg[32];
  snprintf(msg, sizeof(msg), "Cleared P%u", (unsigned)pad_display_number(pad));
  show_status_message(msg, 1600, false);
  draw_menu(true);
}

static void reset_sampler_preferences(void)
{
  loop_quantize_enabled = true;
  loop_quantize_option_index = 2;          // 32
  loop_note_off_quantize_option_index = 3; // 64
  background_loop.volume_q8 = volume_q8_from_20_percent_step(4);
  background_loop.loop_repeats = 2;
  melody_settings = { synth_tone_source_t::general_midi, 81, factory_pad_sound_pad, 0, 0, 0, 80 };
  chord_settings = { synth_tone_source_t::general_midi, 90, factory_pad_sound_pad, 0, 0, 0, 60 };
  bass_settings = { synth_tone_source_t::general_midi, 38, factory_pad_sound_pad, 0, 0, 0, 80 };
  melody_follow_harmony_key = true;
  harmony_scale = 0;
  drum_volume = 100;
  std::fill(mixer_part_volume, mixer_part_volume + mixer_part_count, 100);
  std::fill(mixer_part_muted, mixer_part_muted + mixer_part_count, false);
  for (auto& snapshot : mixer_snapshot) { snapshot = mixer_snapshot_t{}; }
  mixer_pending_snapshot = -1;
  mixer_applied_snapshot = -1;
  mixer_active = false;
  mixer_held_part = -1;
  std::fill(mixer_pad_armed, mixer_pad_armed + def::pad::pad_count, false);
  std::fill(mixer_pad_adjusted, mixer_pad_adjusted + def::pad::pad_count, false);

  // FX parameters are Kit state, but Reset All must leave neither a held
  // effect nor an old mixer scene audible after the kit has been restored.
  for (uint8_t i = 0; i < 3; ++i) {
    fx_param[i] = 0;
    sampler_audio_t::setFx(i, false, 0);
  }
  fx_param[fx_delay_index] = 1; // 2 Grid
  fx_selected = fx_tempo_index;
  sampler_audio_t::setMasterDelay(false);
  sampler_audio_t::setMasterDelayFrames(fx_delay_frames());
  sampler_audio_t::setTapeStop(false);
  fx_pad_active = -1;
  fx_speed_active = false;
  fx_speed_pressed = false;
  fx_speed_returning = false;
  fx_speed_reference_active = false;
  fx_speed_reference_origin_msec = 0;
  fx_speed_reference_origin_pos_ms = 0;
  fx_speed_reference_length_ms = 0;
  fx_speed_return_started_msec = 0;
  fx_speed_return_duration_msec = 0;
  fx_speed_ratio_current_q8 = 256;
  fx_speed_ratio_target_q8 = 256;
  sampler_audio_t::setFxSpeedRatioQ8(256);
  loop_repeat_armed = false;
  loop_repeat_running = false;
  loop_repeat_release_confirm_msec = 0;

  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_display_length_ms(M5.millis())));
  refresh_sample_grid_loop_intervals();
  apply_all_mixer_parts();
  if (loop_playing && background_loop.isValid()) {
    play_background_loop_at(loop_pos_ms(M5.millis()));
  }
}

static void menu_execute_action(menu_action_t action)
{
  switch (action) {
  case menu_action_t::kit_load: {
    begin_kit_file_select();
    return; }
  case menu_action_t::kit_save: {
    begin_kit_save();
    return; }
  case menu_action_t::kit_new:
    clear_kit();
    current_kit_path[0] = 0;
    show_status_message("New kit", 1600, false);
    break;
  case menu_action_t::kit_reset_builtin:
    show_loading_message();
    show_status_message(reset_default_or_builtin_kit() ? "Default kit" : "Kit reset", 1600, false);
    break;
  case menu_action_t::kit_assign_wav:
    begin_kit_assign_wav();
    return;
  case menu_action_t::kit_clear_pad:
    begin_kit_clear_pad();
    return;
  case menu_action_t::kit_clear_all_pads:
    clear_all_pad_samples();
    show_status_message("Pads cleared", 1600, false);
    break;
  case menu_action_t::kit_pad_list:
    show_kit_pad_list();
    return;
  case menu_action_t::background_load: {
    begin_background_wav_select();
    return; }
  case menu_action_t::background_clear:
    clear_background_loop();
    show_status_message("BGM cleared", 1600, false);
    break;
  case menu_action_t::loop_clear:
    clear_background_loop();
    loop_reset_recording_state();
    show_status_message("Loop cleared", 1600, false);
    break;
  case menu_action_t::loop_save_as_bgm:
    save_loop_as_bgm();
    return;
  case menu_action_t::loop_stop:
    stop_all_audio();
    show_status_message("Stopped", 1600, false);
    break;
  case menu_action_t::input_learn:
    menu_visible = false;
    learn_state = learn_state_t::waiting_target;
    learn_target_label[0] = 0;
    learn_target = (int16_t)midi_assign_target_t::none;
    learn_target_deadline_msec = M5.millis() + learn_target_timeout_ms;
    draw_learn_overlay();
    return;
  case menu_action_t::input_assign_list:
    begin_input_assignment_list();
    return;
  case menu_action_t::input_clear_all:
    std::fill(midi_note_assign, midi_note_assign + 128, (int16_t)midi_assign_target_t::none);
    std::fill(midi_cc_assign, midi_cc_assign + 128, (int16_t)midi_assign_target_t::none);
    std::fill(external_button_assign, external_button_assign + 32, (int16_t)midi_assign_target_t::none);
    std::fill(usb_keyboard_assign, usb_keyboard_assign + 256, (int16_t)midi_assign_target_t::none);
    std::fill(usb_gamepad_assign, usb_gamepad_assign + 256, (int16_t)midi_assign_target_t::none);
    midi_note_assign_count = 0;
    external_button_assign_count = 0;
    usb_keyboard_assign_count = 0;
    usb_gamepad_assign_count = 0;
    show_status_message("Assigns cleared", 1600, false);
    break;
  case menu_action_t::input_source_select: {
    const uint8_t selected = menu_cursor;
    if (selected >= (uint8_t)external_input_mode_t::max) { return; }
    if (selected == (uint8_t)external_input_mode) {
      menu_back();
      return;
    }
    const auto next_mode = (external_input_mode_t)selected;
    const bool needs_restart = external_input_mode_needs_restart(next_mode);
    set_external_input_mode(next_mode);
    if (!needs_restart) {
      show_status_message("Input source set", 1600, false);
      menu_back();
    }
    return; }
  case menu_action_t::ble_device_scan:
    begin_ble_device_scan();
    return;
  case menu_action_t::ble_device_forget: {
    const bool had_device = ble_preferred_address[0] != 0;
    task_midi.forgetBLEMidiPreferredDevice();
    ble_preferred_address[0] = 0;
    ble_preferred_name[0] = 0;
    save_resume_kit();
    show_status_message(had_device ? "BLE device forgotten" : "No saved device", 1800, false);
    break; }
  case menu_action_t::reset_ble_connection:
    if (external_input_mode != external_input_mode_t::ble_midi) {
      show_status_message("Select BLE MIDI first", 2000, false);
      break;
    }
    if (ble_connection_reset_state != ble_connection_reset_state_t::idle) { return; }
    task_midi.clearBLEMidiCentralBond();
    kp::system_registry->midi_port_setting.setBLEMIDI(kp::def::command::midi_off);
    ble_connection_reset_state = ble_connection_reset_state_t::stopping;
    ble_connection_reset_deadline_msec = M5.millis() + 1000;
    show_loading_message("RESETTING BLE");
    return;
  case menu_action_t::external_tone_select:
    synth_sound_select_active = false;
    kit_edit_state = kit_edit_state_t::select_external_tone;
    menu_cursor = external_midi_ch1_program;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return;
  case menu_action_t::external_pad_select:
    synth_sound_select_active = false;
    kit_edit_state = kit_edit_state_t::select_external_pad;
    menu_cursor = pad_display_number(external_midi_pad) - 1;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return;
  case menu_action_t::external_pad_base_note_select:
    synth_sound_select_active = false;
    if (external_midi_pad >= def::pad::pad_count || !sampler_pool_t::slot[external_midi_pad].isValid()) {
      show_status_message("Select Pad first", 1600, false);
      draw_menu(true);
      return;
    }
    kit_edit_state = kit_edit_state_t::select_external_pad_base_note;
    menu_cursor = sampler_pool_t::slot[external_midi_pad].base_note;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return;
  case menu_action_t::synth_tone_select: {
    synth_menu_target = synth_target_for_menu_page(menu_page);
    synth_sound_select_active = true;
    auto& settings = page_settings(synth_menu_target);
    kit_edit_state = kit_edit_state_t::select_external_tone;
    menu_cursor = settings.program;
    preview_synth_menu_selection();
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return; }
  case menu_action_t::synth_pad_select: {
    synth_menu_target = synth_target_for_menu_page(menu_page);
    synth_sound_select_active = true;
    auto& settings = page_settings(synth_menu_target);
    kit_edit_state = kit_edit_state_t::select_external_pad;
    menu_cursor = pad_display_number(settings.pad) - 1;
    preview_synth_menu_selection();
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return; }
  case menu_action_t::synth_pad_base_note_select: {
    synth_menu_target = synth_target_for_menu_page(menu_page);
    synth_sound_select_active = true;
    auto& settings = page_settings(synth_menu_target);
    if (settings.pad >= def::pad::pad_count || !sampler_pool_t::slot[settings.pad].isValid()) {
      show_status_message("Select Pad first", 1600, false);
      draw_menu(true);
      return;
    }
    kit_edit_state = kit_edit_state_t::select_external_pad_base_note;
    menu_cursor = sampler_pool_t::slot[settings.pad].base_note;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    menu_sound_cursor(menu_cursor + 1);
    draw_menu_page_transition(1);
    draw_menu_keypad();
    return; }
  case menu_action_t::wifi_setup:
    // かんぷれ本体と同じAPセットアップ画面を起動する。ブラウザでSSIDと
    // パスワードを登録すると、task_wifiがNVSへ保存してSTA接続へ移行する。
    // 起動直後のバックグラウンド更新確認が走っていても、セットアップを最優先する。
    startup_update_check_pending = false;
    begin_wifi_radio_request(wifi_radio_request_t::setup_ap);
    wifi_setup_active = true;
    wifi_setup_qr_active = true;
    wifi_qr_preparing = true;
    wifi_setup_waiting_for_connection = false;
    wifi_setup_connect_deadline_msec = 0;
    wifi_setup_is_wps = false;
    wifi_setup_qr_web_page = false;
    wifi_setup_qr_dirty = true;
    clear_status_message(false);
    draw_menu(true);
    break;
  case menu_action_t::wifi_wps:
    begin_wifi_radio_request(wifi_radio_request_t::setup_wps);
    wifi_setup_active = true;
    wifi_setup_qr_active = false;
    wifi_setup_waiting_for_connection = false;
    wifi_setup_is_wps = true;
    show_status_message("WPS waiting", 0, false);
    break;
  case menu_action_t::wifi_info: {
    char ssid[33] = {};
    if (kp::task_wifi_t::getSavedSTASSID(ssid, sizeof(ssid))) {
      char msg[48];
      snprintf(msg, sizeof(msg), "SSID: %.24s", ssid);
      show_status_message(msg, 2400, false);
    } else {
      show_status_message("Wi-Fi not configured", 2000, false);
    }
    break; }
  case menu_action_t::wifi_update:
    start_wifi_update();
    return;
  case menu_action_t::wifi_file_editor:
    start_file_editor_session();
    return;
  case menu_action_t::system_info: {
    char msg[64];
    snprintf(msg, sizeof(msg), "v%d.%d.%d RAM %u%%"
      , (int)def::app::app_version_major, (int)def::app::app_version_minor, (int)def::app::app_version_patch
      , (unsigned)((sampler_pool_t::usedBytes() * 100) / sampler_pool_t::pool_budget_bytes));
    show_status_message(msg, 1600, false);
    break; }
  case menu_action_t::reset_all_settings:
    kp::system_registry->reset();
    // Samplerの入力ソースも明示的にOFFへ戻し、USB給電を残さない。
    set_external_input_mode(external_input_mode_t::off);
    reset_sampler_preferences();
    // Default Kit is deliberately separate from the immutable built-in kit.
    // A factory reset removes the personal default, then restores built-in.
    if (ensure_sampler_sd_dirs()) {
      kp::storage_sd.removeFile(sampler_default_kit_path);
    }
    reset_sampler_sd_folder_selection();
    clear_background_loop();
    loop_reset_recording_state();
    show_loading_message();
    reset_builtin_kit();
    save_resume_kit();
    show_status_message("All reset", 1600, false);
    break;
  default:
    break;
  }
  draw_menu(true);
}

static void rebuild_input_assignment_list(void)
{
  input_assignment_list.clear();
  for (uint8_t note = 0; note < 128; ++note) {
    if (midi_note_assign[note] != (int16_t)midi_assign_target_t::none) {
      input_assignment_list.push_back({ input_source_t::midi, note, midi_note_assign[note] });
    }
  }
  for (uint8_t cc = 0; cc < 128; ++cc) {
    if (midi_cc_assign[cc] != (int16_t)midi_assign_target_t::none) {
      input_assignment_list.push_back({ input_source_t::midi_cc, cc, midi_cc_assign[cc] });
    }
  }
  for (uint8_t button = 0; button < 32; ++button) {
    if (external_button_assign[button] != (int16_t)midi_assign_target_t::none) {
      input_assignment_list.push_back({ input_source_t::external, button, external_button_assign[button] });
    }
  }
  for (uint16_t key = 0; key < 256; ++key) {
    if (usb_keyboard_assign[key] != (int16_t)midi_assign_target_t::none) {
      input_assignment_list.push_back({ input_source_t::usb_keyboard, (uint8_t)key, usb_keyboard_assign[key] });
    }
  }
  for (uint16_t code = 0; code < 256; ++code) {
    if (usb_gamepad_assign[code] != (int16_t)midi_assign_target_t::none) {
      input_assignment_list.push_back({ input_source_t::usb_gamepad, (uint8_t)code, usb_gamepad_assign[code] });
    }
  }
}

static void begin_input_assignment_list(void)
{
  rebuild_input_assignment_list();
  if (input_assignment_list.empty()) {
    show_status_message("No assignments", 1600, false);
    draw_menu(true);
    return;
  }
  input_assignment_list_active = true;
  menu_cursor = 0;
  menu_depth = menu_dynamic_depth();
  menu_sound_navigate(1);
  menu_sound_cursor(1);
  draw_menu_page_transition(1);
  draw_menu_keypad();
}

static void menu_select(void)
{
  if (!menu_visible) { return; }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) {
    if (kit_shortcut_target_pad >= 0 && kit_shortcut_target_pad < (int)def::pad::pad_count) {
      menu_sound_navigate(1);
      assign_pending_wav_to_pad((uint8_t)kit_shortcut_target_pad);
    }
    return;
  }
  if (input_assignment_list_active) { return; }
  if (ble_device_ui_state == ble_device_ui_state_t::list) {
    if (menu_cursor >= ble_device_count) { return; }
    ble_device_selected = menu_cursor;
    ble_device_ui_state = ble_device_ui_state_t::confirm;
    menu_cursor = 1;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(1);
    draw_menu_header(true);
    draw_menu(true);
    return;
  }
  if (ble_device_ui_state == ble_device_ui_state_t::confirm) {
    if (menu_cursor == 0) {
      ble_device_ui_state = ble_device_ui_state_t::list;
      menu_cursor = (uint8_t)ble_device_selected;
      menu_depth = menu_dynamic_depth();
      menu_sound_navigate(2);
      draw_menu_header(true);
      draw_menu(true);
      return;
    }
    if (ble_device_selected >= ble_device_count) { return; }
    const auto& device = ble_device_list[ble_device_selected];
    snprintf(ble_preferred_address, sizeof(ble_preferred_address), "%s", device.address);
    snprintf(ble_preferred_name, sizeof(ble_preferred_name), "%s", device.name);
    task_midi.setBLEMidiPreferredDevice(ble_preferred_address, ble_preferred_name);
    save_resume_kit();
    ble_device_ui_state = ble_device_ui_state_t::connecting;
    ble_device_connect_deadline_msec = M5.millis() + 15000;
    menu_page = menu_page_t::ble_device;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    show_loading_message("CONNECTING BLE");
    draw_menu_header(true);
    draw_menu(true);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_kit_file) {
    select_kit_file();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_kit_save) {
    select_kit_save();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_wav) {
    select_kit_wav();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_bgm_wav) {
    select_background_wav();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_tone) {
    if (synth_sound_select_active) {
      auto& settings = page_settings(synth_menu_target);
      settings.program = menu_cursor;
      settings.source = synth_tone_source_t::general_midi;
      apply_synth_tones(true);
    } else {
      external_midi_ch1_program = menu_cursor;
      external_midi_sound = external_midi_sound_t::general_midi;
      apply_external_midi_ch1_tone();
    }
    save_resume_kit();
    menu_back();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_pad) {
    const uint8_t selected_pad = display_order_to_pad(menu_cursor);
    if (menu_cursor >= def::pad::pad_count || !sampler_pool_t::slot[selected_pad].isValid()) {
      show_status_message("Pad is empty", 1400, false);
      draw_menu(true);
      return;
    }
    if (synth_sound_select_active) {
      auto& settings = page_settings(synth_menu_target);
      settings.pad = selected_pad;
      settings.source = synth_tone_source_t::pad;
      // The selected Pad belongs to this performance page only. External MIDI
      // keeps its own sound source and therefore cannot accidentally take over
      // the SAM2695 routing when a page switches to Pad sound.
      sampler_audio_t::setOutputMuted(false);
      apply_synth_tones(true);
    } else {
      external_midi_pad = selected_pad;
      external_midi_sound = external_midi_sound_t::pad;
    }
    save_resume_kit();
    menu_back();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_external_pad_base_note) {
    uint8_t selected_pad = synth_sound_select_active
      ? page_settings(synth_menu_target).pad : external_midi_pad;
    if (selected_pad < def::pad::pad_count) {
      sampler_pool_t::slot[selected_pad].base_note = menu_cursor;
      sampler_pool_t::slot[selected_pad].base_note_auto = false;
      save_resume_kit();
    }
    menu_back();
    return;
  }
  if (kit_edit_state == kit_edit_state_t::pad_list) {
    return;
  }
  size_t count = 0;
  const auto* items = menu_items(menu_page, &count);
  if (menu_cursor >= count) { return; }
  const auto& item = items[menu_cursor];
  if (item.kind == menu_item_kind_t::submenu) {
    select_synth_source_branch(item.child);
    menu_page = item.child;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(1);
    menu_sound_cursor(1);
    draw_menu_page_transition(1);
  } else if (item.kind == menu_item_kind_t::value) {
    // Values are applied live with - / +.  OK is only an acknowledgement,
    // so it never changes a setting as an accidental side effect.
    menu_sound_navigate(1);
    draw_menu_keypad();
  } else {
    menu_sound_navigate(1);
    menu_execute_action(item.action);
  }
}

static void menu_move(int diff)
{
  if (!menu_visible) { return; }
  size_t count = menu_dynamic_list_active() ? menu_dynamic_count() : 0;
  if (!menu_dynamic_list_active()) {
    menu_items(menu_page, &count);
  }
  if (count == 0) { return; }
  int old = menu_cursor;
  int next = (int)menu_cursor + diff;
  // 端ではループせずクランプする
  if (next < 0) { next = 0; }
  if (next >= (int)count) { next = (int)count - 1; }
  if (next == old) { return; }
  menu_cursor = (uint8_t)next;
  menu_sound_cursor(menu_cursor + 1);
  preview_synth_menu_selection();
  draw_menu_scroll(old, menu_cursor);
  draw_menu_keypad();
}

static void menu_input_number(uint8_t number)
{
  if (!menu_visible) { return; }
  size_t count = menu_dynamic_list_active() ? menu_dynamic_count() : 0;
  if (!menu_dynamic_list_active()) {
    menu_items(menu_page, &count);
  }
  uint8_t display_number = (number == 0) ? 10 : number;
  if (display_number < 1 || display_number > count) { return; }
  int old = menu_cursor;
  menu_cursor = display_number - 1;
  menu_sound_cursor(display_number);
  preview_synth_menu_selection();
  draw_menu_scroll(old, menu_cursor);
  draw_menu_keypad();
}

static bool menu_handle_button(int btn)
{
  if (!menu_visible || btn < 0 || btn >= 15) { return false; }
  if (input_assignment_list_active && btn == 14) {
    delete_selected_input_assignment();
    return true;
  }
  if (btn == 13 && menu_adjust_current_value(-1)) { return true; }
  if (btn == 14 && menu_adjust_current_value(1)) { return true; }
  if (kit_edit_state == kit_edit_state_t::assign_confirm_shortcut) {
    if (btn == 8) { menu_back(); }
    else if (btn == 9 && kit_shortcut_target_pad >= 0
          && kit_shortcut_target_pad < (int)def::pad::pad_count) {
      menu_sound_navigate(1);
      assign_pending_wav_to_pad((uint8_t)kit_shortcut_target_pad);
    }
    return true;
  }
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    if (btn == 4) {
      menu_back();
      return true;
    }
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      menu_sound_navigate(1);
      if (kit_edit_state == kit_edit_state_t::assign_wait_pad) {
        assign_pending_wav_to_pad((uint8_t)pad);
      } else {
        clear_selected_pad_sample((uint8_t)pad);
      }
      return true;
    }
  }
  switch (btn) {
  case 0: menu_input_number(1); return true;
  case 1: menu_input_number(2); return true;
  case 2: menu_input_number(3); return true;
  case 3: menu_input_number(0); return true;
  case 4: menu_close(); return true;
  case 5: menu_input_number(4); return true;
  case 6: menu_input_number(5); return true;
  case 7: menu_input_number(6); return true;
  case 8: menu_back(); return true;
  case 9: menu_select(); return true;
  case 10: menu_input_number(7); return true;
  case 11: menu_input_number(8); return true;
  case 12: menu_input_number(9); return true;
  case 13:
  case 14:
    return true;
  default: return false;
  }
}

static bool menu_handle_input(uint32_t pressed_edge)
{
  namespace bb = kp::def::button_bitmask;
  // Target selection turns every visible pad/Fn into a target. Do not let the
  // normal menu shortcuts steal Back/Exit or encoder presses in this state.
  if (learn_state == learn_state_t::waiting_target) {
    const uint32_t target_mask = 0x7FFFu
                               | bb::SUB_1 | (bb::SUB_1 << 1) | (bb::SUB_1 << 2) | (bb::SUB_1 << 3)
                               | bb::ENC1_PUSH;
    // Let target edges reach learn_capture_target(); consume all other local
    // controls so target selection cannot alter the instrument state.
    return (pressed_edge & target_mask) == 0;
  }
  if ((pressed_edge & bb::SIDE_2) && (menu_visible || learn_state != learn_state_t::idle)) {
    if (learn_state != learn_state_t::idle) {
      cancel_learn(true);
    } else if (menu_visible) {
      menu_close();
    }
    return true;
  }
  if (learn_state == learn_state_t::waiting_external) {
    // 外部入力待ちでは通常のBack/Exitを再び有効にする。
    if (pressed_edge & (1u << 8)) {
      cancel_learn(false);
      return true;
    }
    if (pressed_edge & (1u << 4)) {
      cancel_learn(true);
      return true;
    }
    if (pressed_edge & (bb::ENC1_PUSH | bb::ENC2_PUSH)) {
      menu_back();
      return true;
    }
    // Only Back/Exit are meaningful while an external event is awaited.
    return true;
  }
  if (!menu_visible
   && current_mode != sampler_mode_t::mode_fx
   && (pressed_edge & bb::ENC2_PUSH)) {
    menu_open();
    return true;
  }
  if (!menu_visible) { return false; }
  for (int btn = 0; btn < 15; ++btn) {
    if (pressed_edge & (1u << btn)) {
      menu_handle_button(btn);
      return true;
    }
  }
  if (pressed_edge & bb::ENC2_PUSH) { menu_select(); return true; }
  if (pressed_edge & bb::ENC1_PUSH) { menu_back(); return true; }
  return true;
}

static bool learn_capture_target(uint32_t pressed_edge)
{
  if (learn_state != learn_state_t::waiting_target || pressed_edge == 0) { return false; }
  namespace bb = kp::def::button_bitmask;
  for (int btn = 0; btn < 15; ++btn) {
    if (0 == (pressed_edge & (1u << btn))) { continue; }
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      snprintf(learn_target_label, sizeof(learn_target_label), "P%d", pad_display_number((uint8_t)pad));
      learn_target = (int16_t)midi_assign_target_t::pad_base + pad;
    } else {
      int fn = button_to_fn(btn);
      if (fn < 0) { continue; }
      snprintf(learn_target_label, sizeof(learn_target_label), "Fn%d", fn + 1);
      learn_target = (int16_t)midi_assign_target_t::fn_base + fn;
    }
    learn_state = learn_state_t::waiting_external;
    learn_target_deadline_msec = 0;
    draw_learn_overlay();
    return true;
  }
  for (int i = 0; i < (int)sampler_mode_t::mode_max; ++i) {
    if (pressed_edge & (bb::SUB_1 << i)) {
      snprintf(learn_target_label, sizeof(learn_target_label), "%s", mode_info[i].name);
      learn_target = (int16_t)midi_assign_target_t::mode_base + i;
      learn_state = learn_state_t::waiting_external;
      learn_target_deadline_msec = 0;
      draw_learn_overlay();
      return true;
    }
  }
  if (pressed_edge & bb::ENC1_PUSH) {
    snprintf(learn_target_label, sizeof(learn_target_label), "STOP ALL");
    learn_target = (int16_t)midi_assign_target_t::stop_all;
    learn_state = learn_state_t::waiting_external;
    learn_target_deadline_msec = 0;
    draw_learn_overlay();
    return true;
  }
  return false;
}

static void update_midi_assign_count(void)
{
  midi_note_assign_count = 0;
  for (int note = 0; note < 128; ++note) {
    if (midi_note_assign[note] != (int16_t)midi_assign_target_t::none) { ++midi_note_assign_count; }
    if (midi_cc_assign[note] != (int16_t)midi_assign_target_t::none) { ++midi_note_assign_count; }
  }
  external_button_assign_count = 0;
  for (int button = 0; button < 32; ++button) {
    if (external_button_assign[button] != (int16_t)midi_assign_target_t::none) { ++external_button_assign_count; }
  }
  usb_keyboard_assign_count = 0;
  for (int key = 0; key < 256; ++key) {
    if (usb_keyboard_assign[key] != (int16_t)midi_assign_target_t::none) { ++usb_keyboard_assign_count; }
  }
  usb_gamepad_assign_count = 0;
  for (int code = 0; code < 256; ++code) {
    if (usb_gamepad_assign[code] != (int16_t)midi_assign_target_t::none) { ++usb_gamepad_assign_count; }
  }
}

static void delete_selected_input_assignment(void)
{
  if (!input_assignment_list_active || menu_cursor >= input_assignment_list.size()) { return; }
  const auto entry = input_assignment_list[menu_cursor];
  if (entry.source_type == input_source_t::external) {
    external_button_assign[entry.source] = (int16_t)midi_assign_target_t::none;
  } else if (entry.source_type == input_source_t::usb_keyboard) {
    usb_keyboard_assign[entry.source] = (int16_t)midi_assign_target_t::none;
  } else if (entry.source_type == input_source_t::usb_gamepad) {
    usb_gamepad_assign[entry.source] = (int16_t)midi_assign_target_t::none;
  } else if (entry.source_type == input_source_t::midi_cc) {
    midi_cc_assign[entry.source] = (int16_t)midi_assign_target_t::none;
  } else {
    midi_note_assign[entry.source] = (int16_t)midi_assign_target_t::none;
  }
  update_midi_assign_count();
  rebuild_input_assignment_list();

  char message[32];
  const char* source = entry.source_type == input_source_t::external ? "EXT"
                     : entry.source_type == input_source_t::usb_keyboard ? "KEY"
                     : entry.source_type == input_source_t::usb_gamepad ? "PAD"
                     : entry.source_type == input_source_t::midi_cc ? "CC" : "NOTE";
  snprintf(message, sizeof(message), "%s %u deleted", source,
           (unsigned)(entry.source_type == input_source_t::external ? entry.source + 1 : entry.source));
  if (input_assignment_list.empty()) {
    input_assignment_list_active = false;
    menu_page = menu_page_t::input_assign;
    menu_cursor = 1;
    menu_depth = menu_page_depth(menu_page);
  } else if (menu_cursor >= input_assignment_list.size()) {
    menu_cursor = (uint8_t)(input_assignment_list.size() - 1);
  }
  menu_sound_navigate(1);
  show_status_message(message, 1400, false);
  draw_menu(true);
}

static void finish_learn_assign(const char* source_label, int16_t* assignment)
{
  if (learn_target == (int16_t)midi_assign_target_t::none || assignment == nullptr) { return; }
  *assignment = learn_target;
  update_midi_assign_count();
  learn_state = learn_state_t::idle;
  learn_target_deadline_msec = 0;
  menu_visible = true;
  menu_page = menu_page_t::input_assign;
  menu_cursor = 0;
  char message[48];
  snprintf(message, sizeof(message), "%s -> %s", source_label, learn_target_label);
  show_status_message(message, 1800, false);
  draw_menu(true);
}

static void capture_midi_learn(uint8_t note)
{
  char source_label[16];
  snprintf(source_label, sizeof(source_label), "Note %u", (unsigned)note);
  finish_learn_assign(source_label, &midi_note_assign[note]);
}

static void capture_midi_cc_learn(uint8_t controller)
{
  char source_label[16];
  snprintf(source_label, sizeof(source_label), "CC %u", (unsigned)controller);
  finish_learn_assign(source_label, &midi_cc_assign[controller]);
}

static void capture_external_button_learn(uint8_t button)
{
  char source_label[16];
  snprintf(source_label, sizeof(source_label), "EXT %u", (unsigned)(button + 1));
  finish_learn_assign(source_label, &external_button_assign[button]);
}

static void capture_usb_keyboard_learn(uint8_t key)
{
  char source_label[16];
  snprintf(source_label, sizeof(source_label), "KEY %u", (unsigned)key);
  finish_learn_assign(source_label, &usb_keyboard_assign[key]);
}

static void capture_usb_gamepad_learn(uint8_t code)
{
  char source_label[16];
  snprintf(source_label, sizeof(source_label), "PAD %u", (unsigned)code);
  finish_learn_assign(source_label, &usb_gamepad_assign[code]);
}

static bool synth_sustain_parameters(const sample_slot_t& slot, uint32_t source_start,
                                     uint32_t* start,
                                     uint32_t* end, uint16_t* crossfade)
{
  if (start) { *start = 0; }
  if (end) { *end = 0; }
  if (crossfade) { *crossfade = 0; }
  if (!slot.isValid()) { return false; }
  if (slot.synth_sustain_mode == sample_sustain_mode_t::off || slot.reverse) {
    return false;
  }
  const bool points_valid = slot.synth_loop_start >= source_start
                         && slot.synth_loop_end <= slot.playEnd()
                         && slot.synth_loop_end > slot.synth_loop_start + 31;
  const bool use_loop = points_valid
                     && (slot.synth_sustain_mode == sample_sustain_mode_t::manual
                      || slot.synth_sustain_auto);
  if (use_loop) {
    if (start) { *start = slot.synth_loop_start - source_start; }
    if (end) { *end = slot.synth_loop_end - source_start; }
    if (crossfade) { *crossfade = slot.synth_loop_crossfade; }
  }
  return use_loop;
}

static uint16_t sample_sustain_auto_release_ms(const sample_slot_t& slot,
                                               uint32_t sustain_start,
                                               uint32_t sustain_end)
{
  if (slot.sample_rate == 0 || slot.pitch_q8 == 0 || sustain_end <= sustain_start) {
    return 0;
  }
  // One-shot Synth playback enters Release as it reaches Loop Out.  Waiting
  // for another wrapped cycle made short loops feel as if their Release had
  // been ignored, and left a hard stop when the voice reached its boundary.
  // Pitch changes playback time, not the PCM loop points.
  const uint64_t source_frames = sustain_end;
  const uint64_t duration_ms = source_frames * 1000u * 256u
                             / ((uint64_t)slot.sample_rate * slot.pitch_q8);
  return (uint16_t)std::clamp<uint64_t>(duration_ms, 20, 60000);
}

static void handle_fn_button(int fn, bool press);

static void process_assigned_input(int16_t target, bool pressed)
{
  if (wifi_update_active || wifi_file_server_qr_active) { return; }
  if (target == (int16_t)midi_assign_target_t::none) { return; }
  if (target >= (int16_t)midi_assign_target_t::pad_base
   && target < (int16_t)midi_assign_target_t::pad_base + (int)def::pad::pad_count) {
    int pad = target - (int16_t)midi_assign_target_t::pad_base;
    if (pressed) { pad_press(pad); }
    else { pad_release(pad); }
    return;
  }
  if (target >= (int16_t)midi_assign_target_t::fn_base
   && target < (int16_t)midi_assign_target_t::fn_base + 3) {
    handle_fn_button(target - (int16_t)midi_assign_target_t::fn_base, pressed);
    return;
  }
  // モード切替とStop Allはノートオンだけで実行する。
  if (!pressed) { return; }
  if (target >= (int16_t)midi_assign_target_t::mode_base
   && target < (int16_t)midi_assign_target_t::mode_base + (int)sampler_mode_t::mode_max) {
    set_mode((sampler_mode_t)(target - (int16_t)midi_assign_target_t::mode_base));
  } else if (target == (int16_t)midi_assign_target_t::stop_all) {
    stop_all_audio();
  }
}

static void process_external_midi_note(uint8_t status, uint8_t note, uint8_t velocity)
{
  if (wifi_update_active || wifi_file_server_qr_active) { return; }
  if (note >= 128) { return; }
  if (learn_state == learn_state_t::waiting_external) {
    if (velocity != 0) { capture_midi_learn(note); }
    return;
  }
  if (menu_visible) { return; }

  const int16_t target = midi_note_assign[note];
  const bool note_on = (status & 0xF0) == 0x90 && velocity != 0;
  // Auto: Assign済みだけ操作、未Assignは演奏。
  // Play: Assignを無視し、全Noteを演奏。
  // Control: Note音を出さず、Assign済みだけ操作。
  if (midi_note_action == midi_note_action_t::automatic
   && target != (int16_t)midi_assign_target_t::none) {
    process_assigned_input(target, note_on);
    return;
  }
  if (midi_note_action == midi_note_action_t::control) {
    if (target != (int16_t)midi_assign_target_t::none) {
      process_assigned_input(target, note_on);
    }
    return;
  }

  if (melody_settings.source == synth_tone_source_t::pad) {
    // 基準音を中心に最大2オクターブずつ変調して、鍵盤として広い音域を
    // そのまま扱う。PCMボイスだけで処理するため演奏負荷は増えない。
    if (!note_on) {
      for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
        if (external_midi_voice_note[i] == (int8_t)note) {
          sampler_audio_t::release(external_midi_voice_base + i);
          external_midi_voice_note[i] = -1;
          pitched_voice_state[i] = {};
        }
      }
      return;
    }
    if (melody_settings.pad >= def::pad::pad_count) { return; }
    const auto& slot = sampler_pool_t::slot[melody_settings.pad];
    if (!slot.isValid() || slot.playFrames() == 0) { return; }
    const uint16_t pitch = sample_pitch_for_note(slot, note);
    uint32_t volume = ((uint32_t)slot.volume_q8 * velocity) / 127;
    volume = std::max<uint32_t>(1, volume);

    uint8_t voice = allocate_pitched_voice(pitched_voice_owner_t::external, note, note);
    external_midi_voice_next = (voice + 1) % external_midi_voice_count;
    uint32_t sustain_start = 0;
    uint32_t sustain_end = 0;
    uint16_t sustain_crossfade = 0;
    const uint32_t source_start = slot.playStart();
    const uint32_t source_frames = slot.playEnd() - source_start;
    bool sustain = synth_sustain_parameters(slot, source_start,
                                             &sustain_start, &sustain_end, &sustain_crossfade);
    sampler_audio_t::playSynth(external_midi_voice_base + voice,
                          slot.pcm + source_start, source_frames, slot.sample_rate,
                          sustain, slot.reverse, (uint16_t)volume, (uint16_t)pitch,
                          0, slot.synth_release_ms,
                          sustain_start, sustain_end, sustain_crossfade);
    external_midi_voice_note[voice] = (int8_t)note;
    return;
  }

  // 未アサインの外部ノートはSAM2695へそのまま渡す。内部MIDI出力タスクが
  // UART1のSAM2695へ送るため、入力元のチャンネルとベロシティを維持できる。
  const uint8_t output_status = (note_on ? 0x90 : 0x80) | (status & 0x0F);
  send_sam_midi(output_status, note, note_on ? velocity : 0);
}

static void process_external_midi_cc(uint8_t controller, uint8_t value)
{
  if (wifi_update_active || wifi_file_server_qr_active || controller >= 128) { return; }
  if (learn_state == learn_state_t::waiting_external) {
    if (value != 0) { capture_midi_cc_learn(controller); }
    return;
  }
  if (!menu_visible) { process_assigned_input(midi_cc_assign[controller], value != 0); }
}

static void process_external_midi_input(void)
{
  auto& midi = kp::system_registry->midi_input;
  kp::system_registry_t::reg_midi_input_t::message_t message;
  while (midi.popMessage(&message)) {
    if (message.type == kp::system_registry_t::reg_midi_input_t::NOTE_MESSAGE) {
      process_external_midi_note(message.status, message.number, message.value);
    } else if (message.type == kp::system_registry_t::reg_midi_input_t::CC_MESSAGE) {
      process_external_midi_cc(message.number, message.value);
    }
  }
}

static void process_external_button_input(void)
{
  if (wifi_update_active || wifi_file_server_qr_active) { return; }
  const kp::registry_base_t::history_t* h;
  auto& input = kp::system_registry->external_input;
  while ((h = input.getHistory(external_input_history_code)) != nullptr) {
    if (h->index > kp::system_registry_t::reg_external_input_t::PORTA_BITMASK_BYTE3) { continue; }
    uint32_t bitmask = input.getPortAButtonBitmask();
    uint32_t pressed_edge = bitmask & ~external_input_prev_bitmask;
    uint32_t released_edge = ~bitmask & external_input_prev_bitmask;
    external_input_prev_bitmask = bitmask;
    while (pressed_edge) {
      uint8_t button = __builtin_ctz(pressed_edge);
      pressed_edge &= pressed_edge - 1;
      if (learn_state == learn_state_t::waiting_external) {
        capture_external_button_learn(button);
      } else if (!menu_visible) {
        process_assigned_input(external_button_assign[button], true);
      }
    }
    if (learn_state != learn_state_t::waiting_external && !menu_visible) {
      while (released_edge) {
        uint8_t button = __builtin_ctz(released_edge);
        released_edge &= released_edge - 1;
        process_assigned_input(external_button_assign[button], false);
      }
    }
  }
}

static void process_usb_keyboard_input(void)
{
  uint8_t key;
  bool pressed;
  while (task_midi.getUSBHIDKeyboardEvent(&key, &pressed)) {
    if (!usb_keyboard_enabled || wifi_update_active || wifi_file_server_qr_active) { continue; }
    if (learn_state == learn_state_t::waiting_external) {
      if (pressed) { capture_usb_keyboard_learn(key); }
    } else if (!menu_visible) {
      process_assigned_input(usb_keyboard_assign[key], pressed);
    }
  }
}

static void process_usb_gamepad_input(void)
{
  uint8_t code;
  bool pressed;
  while (task_midi.getUSBHIDGamepadEvent(&code, &pressed)) {
    if (!usb_gamepad_enabled || wifi_update_active || wifi_file_server_qr_active) { continue; }
    if (learn_state == learn_state_t::waiting_external) {
      if (pressed) { capture_usb_gamepad_learn(code); }
    } else if (!menu_visible) {
      process_assigned_input(usb_gamepad_assign[code], pressed);
    }
  }
}

//-------------------------------------------------------------------------
// 入力処理

static int16_t* alloc_recording_buffer(void)
{
  if (recording_buffer != nullptr) { return recording_buffer; }
  size_t bytes = (size_t)recording_buffer_frames * sizeof(int16_t);
#if defined (M5UNIFIED_PC_BUILD)
  recording_buffer = (int16_t*)malloc(bytes);
#else
  recording_buffer = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
  return recording_buffer;
}

static uint32_t recording_max_frames(void)
{
  return recording_sample_rate_current * sampler_pool_t::max_sample_sec;
}

struct auto_crop_result_t {
  uint32_t start = 0;
  uint32_t end = 0;
};

static bool sample_crosses_zero(int16_t a, int16_t b)
{
  return (a <= 0 && b >= 0) || (a >= 0 && b <= 0);
}

static uint32_t find_nearest_zero_crossing(const int16_t* data, uint32_t frames, uint32_t center, uint32_t radius)
{
  if (data == nullptr || frames < 2) { return center; }
  if (center >= frames) { center = frames - 1; }
  radius = std::min<uint32_t>(radius, frames - 1);
  uint32_t best = center;
  int32_t best_abs = data[center];
  if (best_abs < 0) { best_abs = (best_abs == INT16_MIN) ? 32768 : -best_abs; }

  for (uint32_t dist = 0; dist <= radius; ++dist) {
    if (center >= dist) {
      uint32_t i = center - dist;
      if (i > 0 && sample_crosses_zero(data[i - 1], data[i])) { return i; }
      int32_t v = data[i];
      if (v < 0) { v = (v == INT16_MIN) ? 32768 : -v; }
      if (v < best_abs) {
        best_abs = v;
        best = i;
      }
    }
    uint32_t i = center + dist;
    if (i < frames) {
      if (i > 0 && sample_crosses_zero(data[i - 1], data[i])) { return i; }
      int32_t v = data[i];
      if (v < 0) { v = (v == INT16_MIN) ? 32768 : -v; }
      if (v < best_abs) {
        best_abs = v;
        best = i;
      }
    }
  }
  return best;
}

static bool auto_crop_recording(int16_t* data, uint32_t frames, uint32_t sample_rate, auto_crop_result_t& result)
{
  result = {};
  if (data == nullptr || frames < 1024) { return false; }

  uint32_t noise_frames = std::min<uint32_t>(frames / 4, sample_rate / 5);  // max 200ms
  if (noise_frames < 256) { noise_frames = std::min<uint32_t>(frames, 256); }
  int64_t noise_sum = 0;
  int32_t noise_peak = 0;
  int32_t total_peak = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t v = data[i];
    if (v < 0) { v = (v == INT16_MIN) ? 32768 : -v; }
    if (i < noise_frames) {
      noise_sum += v;
      if (noise_peak < v) { noise_peak = v; }
    }
    if (total_peak < v) { total_peak = v; }
  }
  int32_t noise_avg = noise_frames ? (int32_t)(noise_sum / noise_frames) : 0;
  int32_t trim_threshold = std::max<int32_t>(192, std::max<int32_t>(noise_avg * 3, noise_peak + noise_peak / 2));
  if (total_peak > 0 && trim_threshold > total_peak / 5) {
    trim_threshold = std::max<int32_t>(192, total_peak / 5);
  }

  uint32_t first = frames;
  uint32_t last = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t v = data[i];
    if (v < 0) { v = -v; }
    if (v > trim_threshold) {
      if (first == frames) { first = i; }
      last = i;
    }
  }
  if (first == frames || last <= first) { return false; }

  uint32_t pre_roll = sample_rate / 200;  // 5ms
  uint32_t post_roll = sample_rate / 50;  // 20ms
  uint32_t start = (first > pre_roll) ? (first - pre_roll) : 0;
  uint32_t end = std::min<uint32_t>(frames, last + post_roll + 1);
  if (end - start < 1024) { return false; }

  uint32_t zero_radius = std::max<uint32_t>(8, sample_rate / 200);  // 5ms
  start = find_nearest_zero_crossing(data, frames, start, zero_radius);
  uint32_t end_sample = find_nearest_zero_crossing(data, frames, end - 1, zero_radius);
  end = std::min<uint32_t>(frames, end_sample + 1);
  if (end <= start || end - start < 1024) { return false; }

  result.start = start;
  result.end = end;
  return true;
}

static void loop_remove_page_pad_events(performance_page_t page, int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  if (page == performance_page_t::sample) { stop_sample_grid_loop(pad); }
  else { release_synth_trigger(page, (uint8_t)pad); }
  {
    loop_events_guard_t guard;
    loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
      [page, pad](const loop_event_t& e) {
        return e.page == page && !loop_event_is_pitch_bend(e.type) && e.pad == (uint8_t)pad;
      }), loop_events.end());
  }
  loop_mute(page, (uint8_t)pad) = false;
  if (page == performance_page_t::sample) {
    loop_active_layer[pad] = 0;
    loop_deferred_note_on_layer[pad] = 0;
    loop_deferred_live_pad[pad] = false;
    loop_deferred_live_start_frame[pad] = 0;
  } else {
    synth_loop_active_layer[(uint8_t)page][pad] = 0;
    synth_deferred_note_on_layer[(uint8_t)page][pad] = 0;
    synth_sounding_layer[(uint8_t)page][pad] = 0;
  }
  invalidate_loop_timeline_cache();
  if (loop_events.empty()) {
    if (background_loop.isValid()) {
      loop_length_fixed = true;
      loop_length_msec = background_loop_length_ms();
    } else if (!loop_length_fixed && loop_playing) {
      // Deleting the final event must not stop the transport. Freeze the
      // captured duration here so an empty loop remains available to overdub.
      loop_length_msec = std::max<uint32_t>(loop_min_length_ms, loop_display_length_ms(M5.millis()));
      loop_length_fixed = true;
      auto_configure_loop_grid(loop_length_msec);
      // 未確定録音を空にした場合だけ、確定した長さの先頭境界を
      // 次のクロックで処理する。開始時刻そのものは維持する。
      loop_prev_pos_ms = loop_length_msec ? loop_length_msec - 1 : 0;
    }
    // 確定済みLoop/BGMの全イベントを消してもtransportは継続する。
    // loop_start_msecを書き換えるとBGMと残りパートの位相がずれる。
  }
}

static void loop_clear_page_events(performance_page_t page)
{
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    loop_remove_page_pad_events(page, pad);
  }
  {
    loop_events_guard_t guard;
    loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
      [page](const loop_event_t& e) {
        return e.page == page && loop_event_is_pitch_bend(e.type);
      }), loop_events.end());
  }
  if (page == performance_page_t::melody || page == performance_page_t::bass) {
    reset_page_pitch_bend(page, true);
    pitch_bend_record_layer[(uint8_t)page] = 0;
  }
  loop_undo_history[(uint8_t)page].clear();
  invalidate_loop_timeline_cache();
  request_wave_draw();
  request_all_fn_draw();
}

static void loop_remove_pad_events(int pad)
{
  loop_remove_page_pad_events(performance_page_t::sample, pad);
}

static void loop_reset_recording_state(void)
{
  const bool was_loop_playing = loop_playing;
  {
    loop_events_guard_t guard;
    loop_events.clear();
  }
  for (auto& history : loop_undo_history) { history.clear(); }
  std::fill(pitch_bend_record_layer,
            pitch_bend_record_layer + (uint8_t)performance_page_t::max, 0);
  loop_playing = false;
  if (was_loop_playing) { reset_mixer_mix(); }
  loop_prev_pos_ms = 0;
  loop_start_msec = M5.millis();
  loop_capture_zero_until_msec = 0;
  loop_record_enabled = true;
  if (background_loop.isValid()) {
    loop_length_fixed = true;
    loop_length_msec = background_loop_length_ms();
    auto_configure_loop_grid(loop_length_msec);
  } else {
    loop_length_fixed = false;
    loop_length_msec = loop_default_length_ms;
  }
  loop_layer_seq = 1;
  std::fill(loop_page_mute, loop_page_mute + (uint8_t)performance_page_t::max, false);
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    for (uint8_t page = 0; page < (uint8_t)performance_page_t::max; ++page) {
      loop_pad_mute[page][i] = false;
      synth_loop_active_layer[page][i] = 0;
      synth_deferred_note_on_layer[page][i] = 0;
      synth_sounding_layer[page][i] = 0;
      synth_live_min_gate_until[page][i] = 0;
      synth_live_release_pending[page][i] = false;
      synth_live_release_layer[page][i] = 0;
    }
    loop_active_layer[i] = 0;
    loop_deferred_note_on_layer[i] = 0;
    loop_live_min_gate_until[i] = 0;
    loop_live_release_pending[i] = false;
    loop_live_release_layer[i] = 0;
    loop_deferred_live_pad[i] = false;
    loop_deferred_live_pos_ms[i] = 0;
    loop_deferred_live_start_frame[i] = 0;
  }
  loop_live_release_pending_mask = 0;
  std::fill(synth_live_release_pending_mask,
            synth_live_release_pending_mask + (uint8_t)performance_page_t::max, 0);
  clear_synth_runtime();
  sampler_audio_t::stopAll();
  clear_sample_grid_loops();
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
  invalidate_loop_timeline_cache();
}

static void loop_reset_recording_state_if_empty(void)
{
  if (!loop_events.empty()) { return; }

  // Keep an empty loop alive while its transport is running, so an overdub can
  // still use the captured duration. Once stopped with no BGM, the next
  // recording must capture a fresh loop end.
  if (!loop_playing && !background_loop.isValid()) {
    loop_length_fixed = false;
    loop_length_msec = loop_default_length_ms;
    loop_record_enabled = true;
    loop_prev_pos_ms = 0;
    loop_capture_zero_until_msec = 0;
    loop_layer_seq = 1;
    for (auto& history : loop_undo_history) { history.clear(); }
    std::fill(loop_page_mute, loop_page_mute + (uint8_t)performance_page_t::max, false);
    for (uint8_t page = 0; page < (uint8_t)performance_page_t::max; ++page) {
      for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
        loop_pad_mute[page][pad] = false;
      }
    }
    sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
  }
  invalidate_loop_timeline_cache();
}

static bool looks_like_external_input(const int16_t* data, uint32_t frames)
{
  if (data == nullptr || frames < recording_external_sample_rate / 20) { return false; }

  // 外部端子の挿入検出信号がないため、短時間の入力レベルで判定する。
  // 無音の外部マイクは内蔵Micへフォールバックする設計。
  uint32_t skip = std::min<uint32_t>(frames / 4, recording_external_sample_rate / 50);
  int32_t peak = 0;
  uint64_t sum = 0;
  uint32_t count = 0;
  for (uint32_t i = skip; i < frames; ++i) {
    int32_t v = data[i];
    if (v < 0) { v = (v == INT16_MIN) ? 32768 : -v; }
    if (peak < v) { peak = v; }
    sum += (uint32_t)v;
    ++count;
  }
  if (count == 0) { return false; }
  uint32_t avg = sum / count;
  return peak > 1600 && avg > 90;
}

static uint32_t probe_external_input(void)
{
  if (!sampler_audio_t::startRecording(recording_buffer, recording_buffer_frames)) { return 0; }
  uint32_t start = M5.millis();
  while (M5.millis() - start < 220 && sampler_audio_t::recordingFrames() < external_probe_frames) {
    M5.delay(2);
  }
  return sampler_audio_t::stopRecording();
}

static void start_pad_recording(int pad)
{
  if (recording_pad >= 0 || pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  cancel_sample_move();
  int16_t* buf = alloc_recording_buffer();
  if (buf == nullptr) { return; }

  clear_synth_runtime();
  sampler_audio_t::stopAll();
  clear_sample_grid_loops();
  sampler_audio_t::setOutputMuted(true);
  recording_source = recording_source_t::internal_mic;
  recording_sample_rate_current = recording_internal_sample_rate;
  recording_frames = 0;
#if !defined (M5UNIFIED_PC_BUILD)
  if (recording_source_mode == recording_source_mode_t::external_input) {
    recording_source = recording_source_t::external_input;
    recording_sample_rate_current = recording_external_sample_rate;
    recording_frames = 0;
    if (!sampler_audio_t::startRecording(recording_buffer, recording_max_frames(), recording_frames)) {
      sampler_audio_t::setOutputMuted(false);
      return;
    }
  } else if (recording_source_mode == recording_source_mode_t::automatic) {
    uint32_t external_frames = probe_external_input();
    if (looks_like_external_input(recording_buffer, external_frames)) {
      recording_source = recording_source_t::external_input;
      recording_sample_rate_current = recording_external_sample_rate;
      recording_frames = external_frames;
      sampler_audio_t::startRecording(recording_buffer, recording_max_frames(), recording_frames);
    } else {
      if (!M5.Mic.isRunning() && !M5.Mic.begin()) {
        sampler_audio_t::setOutputMuted(false);
        return;
      }
    }
  } else {
    if (!M5.Mic.isRunning() && !M5.Mic.begin()) {
      sampler_audio_t::setOutputMuted(false);
      return;
    }
  }
#else
  if (recording_source_mode == recording_source_mode_t::external_input) {
    recording_source = recording_source_t::external_input;
    recording_sample_rate_current = recording_external_sample_rate;
  }
#endif
  recording_pad = pad;
  rec_wave_pad = pad;
  update_pad_led(pad);
  draw_pad(pad);
  draw_wave();
}

static void finish_pad_recording(void);

static void service_pad_recording(void)
{
  if (recording_pad < 0 || recording_buffer == nullptr) { return; }
  uint32_t max_frames = recording_max_frames();
  if (recording_frames >= max_frames) {
    finish_pad_recording();
    return;
  }

  if (recording_source == recording_source_t::external_input) {
    recording_frames = sampler_audio_t::recordingFrames();
    if (recording_frames >= max_frames || sampler_audio_t::recordingOverflowed()) {
      finish_pad_recording();
    }
    return;
  }

  uint32_t remain = max_frames - recording_frames;
  uint32_t frames = std::min<uint32_t>(recording_chunk_frames, remain);
#if defined (M5UNIFIED_PC_BUILD)
  memset(recording_buffer + recording_frames, 0, (size_t)frames * sizeof(int16_t));
  recording_frames += frames;
#else
  if (M5.Mic.record(recording_buffer + recording_frames, frames, recording_internal_sample_rate)) {
    recording_frames += frames;
  }
#endif
  if (recording_frames >= max_frames) {
    finish_pad_recording();
  }
}

static void finish_pad_recording(void)
{
  if (recording_pad < 0) { return; }

  int pad = recording_pad;
  recording_pad = -1;
  recording_processing_static_drawn = false;
  recording_processing_frame = 0;
  draw_recording_processing_frame("FINALIZING");

#if !defined (M5UNIFIED_PC_BUILD)
  if (recording_source == recording_source_t::external_input) {
    recording_frames = sampler_audio_t::stopRecording();
  } else {
    while (M5.Mic.isRecording()) { M5.delay(1); }
    M5.Mic.end();
  }
#endif
  sampler_audio_t::setOutputMuted(false);
  uint32_t frames = recording_frames;
  uint32_t sample_rate = recording_sample_rate_current;
  bool overflowed = frames >= recording_max_frames() || sampler_audio_t::recordingOverflowed();
  recording_frames = 0;

  // Pad押下の物理ノイズとマイク起動直後の乱れをPad化しない。
  draw_recording_processing_frame("TRIMMING");
  uint32_t start_discard_frames = sample_rate / 10;  // 100ms
  uint32_t end_discard_frames = sample_rate / 10;    // 100ms
  if (frames > start_discard_frames) {
    frames -= start_discard_frames;
    memmove(recording_buffer, recording_buffer + start_discard_frames, (size_t)frames * sizeof(int16_t));
  } else {
    frames = 0;
  }
  if (frames > end_discard_frames) {
    frames -= end_discard_frames;
  } else {
    frames = 0;
  }

  auto_crop_result_t crop;
  if (auto_crop_recording(recording_buffer, frames, sample_rate, crop)) {
    char name[16];
    snprintf(name, sizeof(name), "REC%02u", (unsigned)recording_seq++);
    draw_recording_processing_frame("NORMALIZING");
    prepare_pad_for_new_sample((uint8_t)pad);
    loop_remove_pad_events(pad);
    loop_reset_recording_state_if_empty();
    if (sampler_pool_t::loadRecordedPcm(pad, name, recording_buffer, frames, sample_rate)) {
      auto& slot = sampler_pool_t::slot[pad];
      slot.start_frame = std::min<uint32_t>(crop.start, slot.frames);
      slot.end_frame = std::min<uint32_t>(crop.end, slot.frames);
      draw_recording_processing_frame("ANALYZING");
      sampler_pool_t::analyzeBaseNote((uint8_t)pad);
      sampler_pool_t::analyzeSynthSustain((uint8_t)pad);
      slot.hold_enabled = false;
      slot.loop_enabled = false;
      slot.loop_whole_sample = false;
      rec_wave_pad = pad;
      // 録音PCMは本体フラッシュではなくSDのセッション領域へ即時退避する。
      // 失敗しても現在のRAM上の演奏は維持する。
      draw_recording_processing_frame("SAVING");
      if (save_session_pad((uint8_t)pad)) { save_resume_kit(); }
      // A newly recorded Pad remains in the normal SAMPLE view.  Its first
      // tap is an audition just like every other Pad, rather than dropping
      // the player straight into an editor while the recording result is
      // still being heard and assessed.
      sample_edit_armed_pad = -1;
      sample_edit_pending_pad = -1;
      show_status_message("SAMPLE SAVED", 1600, false);
    }
  }

  draw_all();
  update_all_leds();
  processing_screen_visible = false;
  (void)overflowed;
}

static void preview_edit_pad(void)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  play_sample_once(edit_pad);
}

static bool stop_edit_preview_if_playing(void)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return false; }
  const bool playing = sampler_audio_t::isPlaying((uint8_t)edit_pad)
                    || sample_grid_loop_active[edit_pad]
                    || sample_whole_loop_active[edit_pad];
  if (!playing) { return false; }
  stop_sample_grid_loop(edit_pad);
  // A complete Wave refresh also removes the lightweight playback cursor.
  request_wave_draw();
  return true;
}

static void preview_edit_transport(bool press)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid()) { return; }
  uint32_t sustain_start = 0;
  uint32_t sustain_end = 0;
  uint16_t sustain_crossfade = 0;
  const bool sustain = synth_sustain_parameters(slot, slot.playStart(),
    &sustain_start, &sustain_end, &sustain_crossfade);
  // SYNTH is a sound-design audition: holding the speaker always exposes the
  // Sustain section, and releasing it always demonstrates Release. Outside
  // this sub-page the preview continues to reproduce the Pad's Hold/Repeat
  // performance settings exactly.
  if (edit_synth_page && sustain) {
    if (press) {
      stop_sample_grid_loop(edit_pad);
      play_sample_sustain_voice(edit_pad, false);
    } else {
      sampler_audio_t::release((uint8_t)edit_pad);
      stop_sample_grid_loop(edit_pad, false);
    }
    return;
  }
  if (press) {
    trigger_pad(edit_pad);
    return;
  }
  // One Shot keeps playing after release. Hold and Hold+Repeat follow the
  // physical preview button exactly, so trim and playback-mode edits can be
  // judged without leaving EDIT.
  if (slot.hold_enabled) {
    if (sustain) {
      sampler_audio_t::release((uint8_t)edit_pad);
    } else if (slot.loop_enabled) {
      stop_sample_grid_loop(edit_pad);
    } else {
      sampler_audio_t::stop((uint8_t)edit_pad);
    }
  }
}

static void request_edit_target_draw(void)
{
  // Edit確認のPad発音を描画より優先する。dirty UIはsound priority期間後に
  // まとめて反映されるため、波形の切替でオーディオ開始を待たせない。
  request_wave_draw();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { request_pad_draw(i); }
  request_all_fn_draw();
}

static void update_sustain_after_trim(uint8_t pad)
{
  auto& slot = sampler_pool_t::slot[pad];
  if (slot.synth_sustain_mode == sample_sustain_mode_t::manual) {
    const uint32_t start = slot.playStart();
    const uint32_t end = slot.playEnd();
    const uint32_t minimum = std::min<uint32_t>(32, end > start ? end - start : 0);
    slot.synth_loop_start = std::clamp(slot.synth_loop_start, start, end);
    slot.synth_loop_end = std::clamp(slot.synth_loop_end, start, end);
    if (slot.synth_loop_end <= slot.synth_loop_start + minimum) {
      const uint32_t span = end - start;
      slot.synth_loop_start = start + span * 35u / 100u;
      slot.synth_loop_end = start + span * 70u / 100u;
    }
    return;
  }
  // Automatic sustain detection scans the PCM and may rebuild the internal
  // synth working set. Defer that non-realtime work while the looper owns the
  // audio deadline; edited Start/End values themselves remain ready for the
  // next Pad trigger.
  if (loop_playing) {
    synth_sustain_analysis_pending[pad] = true;
    return;
  }
  sampler_pool_t::analyzeSynthSustain(pad);
}

static void service_pending_sustain_analysis(void)
{
  if (loop_playing) { return; }
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (!synth_sustain_analysis_pending[pad]) { continue; }
    synth_sustain_analysis_pending[pad] = false;
    sampler_pool_t::analyzeSynthSustain(pad);
    request_wave_draw();
    break;  // one short maintenance job per main-loop turn
  }
}

static void initialize_manual_sustain(sample_slot_t& slot)
{
  const uint32_t start = slot.playStart();
  const uint32_t end = slot.playEnd();
  if (end <= start + 32) { return; }
  const bool valid = slot.synth_loop_start >= start
                  && slot.synth_loop_end <= end
                  && slot.synth_loop_end > slot.synth_loop_start + 31;
  if (!valid) {
    const uint32_t span = end - start;
    slot.synth_loop_start = start + span * 35u / 100u;
    slot.synth_loop_end = start + span * 70u / 100u;
  }
  const uint32_t max_crossfade = (slot.synth_loop_end - slot.synth_loop_start) / 4;
  slot.synth_loop_crossfade = (uint16_t)std::min<uint32_t>(
    max_crossfade, std::max<uint32_t>(1, slot.sample_rate * 8u / 1000u));
}

static void enter_edit(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return; }
  // The editor is one coherent surface. Finish any queued performance tile
  // before its waveform and preview cursor begin direct LCD updates.
  reset_live_wave();
  wait_wave_transfer_job();
  wait_ui_dirty_transfers();
  cancel_sample_move();
  // Fn1 belongs exclusively to preview while EDIT is visible. Discard any
  // partially accumulated performance-recorder hold before entering it.
  cancel_hold_progress(hold_progress_kind_t::performance_record);
  performance_record_fn_consumed = false;
  int old = edit_pad;
  if (old >= 0 && old != pad && edit_trim_changed) {
    if (sampler_pool_t::slot[old].base_note_auto) {
      sampler_pool_t::analyzeBaseNote((uint8_t)old);
    }
    update_sustain_after_trim((uint8_t)old);
  }
  edit_pad = pad;
  if (old < 0) {
    edit_param = 0;
    edit_synth_page = false;
    edit_chop_page = false;
  }
  edit_chop_preview_plan_valid = false;
  edit_chop_preview_count = 0;
  edit_chop_preview_index = 0;
  edit_chop_preview_last = -1;
  edit_notice = edit_notice_t::none;
  edit_notice_until_msec = 0;
  if (old < 0 || old != pad) { edit_trim_changed = false; }
  if (old >= 0 && old != pad) {
    sampler_audio_t::stop(old);
    request_pad_draw(old);
  }
  request_edit_target_draw();
}

static void exit_edit(void)
{
  int old = edit_pad;
  edit_pad = -1;
  if (old >= 0 && edit_trim_changed) {
    if (sampler_pool_t::slot[old].base_note_auto) {
      sampler_pool_t::analyzeBaseNote((uint8_t)old);
    }
    update_sustain_after_trim((uint8_t)old);
  }
  edit_trim_changed = false;
  edit_synth_page = false;
  edit_chop_page = false;
  edit_chop_preview_plan_valid = false;
  edit_chop_preview_count = 0;
  edit_chop_preview_index = 0;
  edit_chop_preview_last = -1;
  edit_value_activity_until = 0;
  edit_value_compact_visible = false;
  edit_notice = edit_notice_t::none;
  edit_notice_until_msec = 0;
  if (old >= 0) { stop_sample_grid_loop(old); }
  request_edit_target_draw();
}

static void edit_value_add(int diff)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  if (edit_chop_page) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid()) { return; }
  if (loop_playing && edit_synth_page
   && (edit_param == 7 || edit_param == 8 || edit_param == 10)) {
    edit_notice = edit_notice_t::stop_loop_to_edit_synth;
    edit_notice_until_msec = M5.millis() + edit_notice_duration_msec;
    edit_value_compact_visible = false;
    request_wave_draw();
    return;
  }
  edit_value_activity_until = M5.millis() + 1000;
  edit_value_compact_visible = true;
  edit_notice = edit_notice_t::none;
  edit_notice_until_msec = 0;
  if (edit_param == 4) {
    bool repeat_disabled = false;
    if (slot.synth_sustain_mode != sample_sustain_mode_t::off) {
      // Sustain owns the continuous internal loop. Whole Sample is omitted
      // here, while rhythmic Grid Repeat remains available as a retrigger.
      int index = slot.loop_enabled && !slot.loop_whole_sample
        ? sample_loop_grid_index(slot.loop_grid_half_steps) : -1;
      index = std::clamp(index + (int)diff, -1, (int)loop_repeat_option_count - 1);
      slot.loop_enabled = index >= 0;
      repeat_disabled = index < 0;
      slot.loop_whole_sample = false;
      if (index >= 0) { slot.loop_grid_half_steps = loop_repeat_half_steps[index]; }
    } else {
      int index = sample_repeat_option_index(slot);
      index = std::clamp(index + (int)diff, -1, (int)loop_repeat_option_count);
      slot.loop_enabled = index >= 0;
      repeat_disabled = index < 0;
      slot.loop_whole_sample = index == 0;
      if (index > 0) { slot.loop_grid_half_steps = loop_repeat_half_steps[index - 1]; }
    }
    if (repeat_disabled || sample_grid_loop_active[edit_pad] || sample_whole_loop_active[edit_pad]) {
      stop_sample_grid_loop(edit_pad);
    }
    request_wave_draw();
    request_grid_draw();
    return;
  }
  if (edit_param == 5 || edit_param == 6) {
    const bool enabled = diff > 0;
    bool& setting = edit_param == 5 ? slot.hold_enabled : slot.reverse;
    if (setting != enabled) { setting = enabled; }
    if (edit_param == 6 && enabled) {
      // Reverse and a forward sustain loop are intentionally exclusive.
      slot.synth_sustain_mode = sample_sustain_mode_t::off;
    }
    request_wave_draw();
    request_grid_draw();
    return;
  }
  if (edit_param == 2) {
    int value = (int)slot.volume_q8 + diff * 13; // 約5%
    if (value < 0) { value = 0; }
    if (value > 512) { value = 512; }
    slot.volume_q8 = (uint16_t)value;
    // Volume is the one safe live edit: it changes only the mixer target, so
    // existing voices fade to the new level without replacing their PCM or
    // sustain loop state.
    sampler_audio_t::setVoiceVolumeQ8((uint8_t)edit_pad,
      mixer_scaled_volume_q8(mixer_part_t::sampler, slot.volume_q8));
    if (melody_settings.source == synth_tone_source_t::pad
     && melody_settings.pad == (uint8_t)edit_pad) {
      apply_mixer_part(mixer_part_t::melody);
    }
    if (bass_settings.source == synth_tone_source_t::pad
     && bass_settings.pad == (uint8_t)edit_pad) {
      apply_mixer_part(mixer_part_t::bass);
    }
    if (chord_settings.source == synth_tone_source_t::pad
     && chord_settings.pad == (uint8_t)edit_pad) {
      apply_mixer_part(mixer_part_t::chord);
    }
    // Several encoder samples can arrive in one update.  Defer the expensive
    // waveform redraw so it represents the final value, not each step.
    request_wave_draw();
    request_pad_draw(edit_pad);
    return;
  }
  if (edit_param == 3) {
    int value = (int)slot.pitch_q8 + diff * 13; // 約5%
    if (value < 128) { value = 128; }
    if (value > 512) { value = 512; }
    slot.pitch_q8 = (uint16_t)value;
    request_wave_draw();
    request_pad_draw(edit_pad);
    return;
  }
  if (edit_param == 7 || edit_param == 8) {
    initialize_manual_sustain(slot);
    slot.synth_sustain_mode = sample_sustain_mode_t::manual;
    slot.reverse = false;
    slot.loop_whole_sample = false;
    uint32_t step = std::max<uint32_t>(1, slot.sample_rate / 50); // 20ms
    const int64_t delta = (int64_t)diff * step;
    const uint32_t minimum = std::max<uint32_t>(32, slot.sample_rate / 200); // 5ms
    if (edit_param == 7) {
      int64_t next = (int64_t)slot.synth_loop_start + delta;
      next = std::clamp<int64_t>(next, slot.playStart(),
        (int64_t)slot.synth_loop_end - minimum);
      slot.synth_loop_start = (uint32_t)next;
    } else {
      int64_t next = (int64_t)slot.synth_loop_end + delta;
      next = std::clamp<int64_t>(next,
        (int64_t)slot.synth_loop_start + minimum, slot.playEnd());
      slot.synth_loop_end = (uint32_t)next;
    }
    const uint32_t max_crossfade = (slot.synth_loop_end - slot.synth_loop_start) / 4;
    slot.synth_loop_crossfade = std::min<uint16_t>(slot.synth_loop_crossfade,
                                                   (uint16_t)max_crossfade);
    request_wave_draw();
    request_grid_draw();
    return;
  }
  if (edit_param == 9) {
    static constexpr const uint16_t release_values[] = {
      10, 50, 100, 200, 500, 750, 1000, 1250, 1500
    };
    int index = 0;
    int best_distance = INT_MAX;
    for (int i = 0; i < (int)std::size(release_values); ++i) {
      const int distance = abs((int)slot.synth_release_ms - (int)release_values[i]);
      if (distance < best_distance) { best_distance = distance; index = i; }
    }
    index = std::clamp(index + diff, 0, (int)std::size(release_values) - 1);
    slot.synth_release_ms = release_values[index];
    request_wave_draw();
    return;
  }
  if (edit_param == 10) {
    int mode = std::clamp((int)slot.synth_sustain_mode + diff, 0, 2);
    slot.synth_sustain_mode = (sample_sustain_mode_t)mode;
    if (slot.synth_sustain_mode != sample_sustain_mode_t::off) {
      slot.reverse = false;
      slot.loop_whole_sample = false;
      if (slot.synth_sustain_mode == sample_sustain_mode_t::automatic) {
        sampler_pool_t::analyzeSynthSustain((uint8_t)edit_pad);
      } else {
        initialize_manual_sustain(slot);
      }
    }
    stop_sample_grid_loop(edit_pad);
    request_wave_draw();
    request_grid_draw();
    return;
  }
  uint32_t step = std::max<uint32_t>(1, slot.sample_rate / 50); // 20ms
  int32_t delta = diff * (int32_t)step;
  uint32_t min_frames = std::min<uint32_t>(256, slot.frames);
  uint32_t start = slot.playStart();
  uint32_t end = slot.playEnd();
  if (edit_param == 0) {
    int64_t next = (int64_t)start + delta;
    if (next < 0) { next = 0; }
    if (next > (int64_t)end - (int64_t)min_frames) { next = end - min_frames; }
    slot.start_frame = (uint32_t)next;
  } else {
    int64_t next = (int64_t)end + delta;
    if (next < (int64_t)start + (int64_t)min_frames) { next = start + min_frames; }
    if (next > slot.frames) { next = slot.frames; }
    slot.end_frame = (uint32_t)next;
  }
  edit_trim_changed = true;
  request_wave_draw();
}

static void show_edit_notice(edit_notice_t notice, uint32_t duration_msec)
{
  edit_notice = notice;
  edit_notice_until_msec = M5.millis() + duration_msec;
  edit_value_compact_visible = false;
  request_wave_draw();
}

static void commit_edit(void)
{
  const int pad = edit_pad;
  exit_edit();
  if (pad >= 0 && pad < (int)def::pad::pad_count && sampler_pool_t::slot[pad].isValid()) {
    if (loop_playing) {
      // SD writes can block for milliseconds. Preserve the performance first
      // and persist this otherwise-complete edit once the loop has stopped.
      session_save_pending_pad = pad;
    } else if (save_session_pad((uint8_t)pad)) {
      save_resume_kit();
    }
  }
}

static void service_pending_session_save(void)
{
  if (loop_playing || session_save_pending_pad < 0) { return; }
  const int pad = session_save_pending_pad;
  session_save_pending_pad = -1;
  if (pad < (int)def::pad::pad_count && sampler_pool_t::slot[pad].isValid()
   && save_session_pad((uint8_t)pad)) {
    save_resume_kit();
  }
}

static bool toggle_edit_synth_assignment(performance_page_t page, uint32_t now)
{
  pitched_page_settings_t* settings = nullptr;
  menu_value_t source_value = menu_value_t::melody_source;
  edit_notice_t assign_confirm = edit_notice_t::confirm_melody;
  edit_notice_t unassign_confirm = edit_notice_t::confirm_unassign_melody;
  edit_notice_t assigned = edit_notice_t::assigned_melody;
  edit_notice_t unassigned = edit_notice_t::unassigned_melody;
  if (page == performance_page_t::chord) {
    settings = &chord_settings;
    source_value = menu_value_t::chord_source;
    assign_confirm = edit_notice_t::confirm_chord;
    unassign_confirm = edit_notice_t::confirm_unassign_chord;
    assigned = edit_notice_t::assigned_chord;
    unassigned = edit_notice_t::unassigned_chord;
  } else if (page == performance_page_t::bass) {
    settings = &bass_settings;
    source_value = menu_value_t::bass_source;
    assign_confirm = edit_notice_t::confirm_bass;
    unassign_confirm = edit_notice_t::confirm_unassign_bass;
    assigned = edit_notice_t::assigned_bass;
    unassigned = edit_notice_t::unassigned_bass;
  } else {
    settings = &melody_settings;
  }
  const bool confirmed = [now](edit_notice_t action) {
    return edit_notice == action && (int32_t)(edit_notice_until_msec - now) > 0;
  }(settings->source == synth_tone_source_t::pad && settings->pad == (uint8_t)edit_pad
      ? unassign_confirm : assign_confirm);
  if (settings->source == synth_tone_source_t::pad && settings->pad == (uint8_t)edit_pad) {
    if (!confirmed) {
      show_edit_notice(unassign_confirm, edit_confirm_duration_msec);
      return false;
    }
    menu_value_set(source_value, 0);
    save_resume_kit();
    show_edit_notice(unassigned, edit_notice_duration_msec);
    return true;
  }
  if (!confirmed) {
    show_edit_notice(assign_confirm, edit_confirm_duration_msec);
    return false;
  }
  settings->source = synth_tone_source_t::pad;
  settings->pad = (uint8_t)edit_pad;
  apply_synth_tones(true);
  save_resume_kit();
  show_edit_notice(assigned, edit_notice_duration_msec);
  return true;
}

static uint32_t find_chop_boundary(const sample_slot_t& source, uint32_t ideal,
                                   uint32_t minimum, uint32_t maximum)
{
  if (minimum >= maximum || source.pcm == nullptr) { return ideal; }
  ideal = std::clamp(ideal, minimum, maximum);
  const uint32_t search = std::min<uint32_t>(source.sample_rate / 100u, (maximum - minimum) / 2u);
  uint32_t quietest = ideal;
  int32_t quietest_level = source.pcm[ideal] < 0 ? -(int32_t)source.pcm[ideal] : source.pcm[ideal];
  for (uint32_t offset = 0; offset <= search; ++offset) {
    const uint32_t candidates[2] = { ideal >= minimum + offset ? ideal - offset : minimum,
                                     ideal + offset <= maximum ? ideal + offset : maximum };
    for (uint8_t side = 0; side < (offset == 0 ? 1 : 2); ++side) {
      const uint32_t frame = candidates[side];
      const int32_t value = source.pcm[frame];
      const int32_t level = value < 0 ? -value : value;
      if (level < quietest_level) {
        quietest = frame;
        quietest_level = level;
      }
      if (frame > minimum) {
        const int16_t previous = source.pcm[frame - 1];
        if ((previous <= 0 && value >= 0) || (previous >= 0 && value <= 0)) { return frame; }
      }
    }
  }
  return quietest;
}

static uint32_t find_chop_boundary_pcm(const int16_t* pcm, uint32_t frames,
                                       uint32_t sample_rate, uint32_t ideal,
                                       uint32_t minimum, uint32_t maximum)
{
  if (!pcm || minimum >= maximum || frames == 0) { return ideal; }
  ideal = std::clamp(ideal, minimum, maximum);
  const uint32_t span = maximum - minimum;
  const uint32_t onset_radius = std::min<uint32_t>(span / 2, sample_rate / 5); // +/-200ms
  const uint32_t scan_start = ideal > onset_radius ? std::max(minimum, ideal - onset_radius) : minimum;
  const uint32_t scan_end = std::min(maximum, ideal + onset_radius);
  const uint32_t window = std::max<uint32_t>(4, sample_rate / 500); // 2ms envelope
  const uint32_t history = std::max<uint32_t>(window + 1, sample_rate / 100); // 10ms
  uint32_t best = ideal;
  int64_t best_score = 0;
  for (uint32_t frame = scan_start + history; frame + window < scan_end; frame += window) {
    int64_t before = 0;
    int64_t after = 0;
    for (uint32_t i = 0; i < window; ++i) {
      before += abs((int)pcm[frame - history + i]);
      after += abs((int)pcm[frame + i]);
    }
    const int64_t rise = after - before;
    const uint32_t distance = frame > ideal ? frame - ideal : ideal - frame;
    const int64_t score = rise > 0 ? rise * (int64_t)(onset_radius + 1 - std::min(distance, onset_radius)) : 0;
    if (score > best_score) { best_score = score; best = frame; }
  }
  const uint32_t zero_radius = std::min<uint32_t>(sample_rate / 100, span / 4);
  uint32_t quietest = best;
  int32_t quietest_level = abs((int)pcm[best]);
  for (uint32_t offset = 0; offset <= zero_radius; ++offset) {
    const uint32_t candidates[2] = {
      best >= minimum + offset ? best - offset : minimum,
      best + offset <= maximum ? best + offset : maximum
    };
    for (uint8_t side = 0; side < (offset ? 2 : 1); ++side) {
      const uint32_t frame = candidates[side];
      const int32_t level = abs((int)pcm[frame]);
      if (level < quietest_level) { quietest = frame; quietest_level = level; }
      if (frame > minimum) {
        const int16_t a = pcm[frame - 1];
        const int16_t b = pcm[frame];
        if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) { return frame; }
      }
    }
  }
  return quietest;
}

static uint32_t find_chop_zero_near(const int16_t* pcm, uint32_t frames,
                                    uint32_t target, uint32_t minimum,
                                    uint32_t maximum, uint32_t radius)
{
  if (!pcm || frames == 0 || minimum >= maximum) { return target; }
  maximum = std::min<uint32_t>(maximum, frames - 1);
  target = std::clamp(target, minimum, maximum);
  radius = std::min<uint32_t>(radius, maximum - minimum);
  uint32_t quietest = target;
  int32_t quietest_level = abs((int)pcm[target]);
  for (uint32_t offset = 0; offset <= radius; ++offset) {
    const uint32_t candidates[2] = {
      target >= minimum + offset ? target - offset : minimum,
      target + offset <= maximum ? target + offset : maximum,
    };
    for (uint8_t side = 0; side < (offset ? 2 : 1); ++side) {
      const uint32_t frame = candidates[side];
      const int32_t level = abs((int)pcm[frame]);
      if (level < quietest_level) { quietest = frame; quietest_level = level; }
      if (frame > minimum) {
        const int16_t a = pcm[frame - 1];
        const int16_t b = pcm[frame];
        if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) { return frame; }
      }
    }
  }
  return quietest;
}

static bool build_chop_slice_plan(const int16_t* pcm, uint32_t frames,
                                  uint32_t sample_rate, uint8_t count,
                                  uint32_t* starts, uint32_t* ends,
                                  uint32_t* anchors)
{
  if (!pcm || !frames || !sample_rate || !count || count > 12
   || !starts || !ends || !anchors) { return false; }
  const uint32_t overlap = std::max<uint32_t>(1, sample_rate * 8u / 1000u);
  const uint32_t search = std::max<uint32_t>(1, sample_rate * 2u / 1000u);
  for (uint8_t i = 0; i < count; ++i) {
    const uint32_t anchor = (uint32_t)(((uint64_t)frames * i) / count);
    const uint32_t next_anchor = (uint32_t)(((uint64_t)frames * (i + 1)) / count);
    uint32_t first = 0;
    if (i != 0 && anchor > overlap) {
      const uint32_t target = anchor - overlap;
      first = find_chop_zero_near(pcm, frames, target,
        target > search ? target - search : 0,
        std::min<uint32_t>(anchor - 1, target + search), search);
    }
    uint32_t last = frames;
    if (i + 1 < count && next_anchor + overlap < frames) {
      const uint32_t target = next_anchor + overlap;
      last = find_chop_zero_near(pcm, frames, target,
        std::max<uint32_t>(next_anchor + 1, target > search ? target - search : 0),
        std::min<uint32_t>(frames - 1, target + search), search);
    }
    last = std::max<uint32_t>(first + 16, last);
    starts[i] = first;
    ends[i] = std::min<uint32_t>(frames, last);
    anchors[i] = anchor - first;
  }
  return true;
}

static uint8_t detect_chop_count(const int16_t* pcm, uint32_t frames, uint32_t sample_rate)
{
  if (!pcm || sample_rate == 0 || frames < sample_rate / 2) { return 8; }
  const uint32_t block = std::max<uint32_t>(16, sample_rate / 100); // 10ms
  uint64_t energy_sum = 0;
  uint32_t energy_max = 0;
  uint32_t blocks = 0;
  for (uint32_t pos = 0; pos < frames; pos += block) {
    uint64_t sum = 0;
    const uint32_t end = std::min(frames, pos + block);
    for (uint32_t i = pos; i < end; ++i) { sum += abs((int)pcm[i]); }
    const uint32_t energy = end > pos ? (uint32_t)(sum / (end - pos)) : 0;
    energy_sum += energy;
    energy_max = std::max(energy_max, energy);
    ++blocks;
    if ((blocks & 63u) == 0) { M5.delay(1); }
  }
  if (!blocks || !energy_max) { return 8; }
  const uint32_t mean = (uint32_t)(energy_sum / blocks);
  const uint32_t threshold = mean + (energy_max > mean ? (energy_max - mean) / 3 : 0);
  const uint32_t min_gap_blocks = std::max<uint32_t>(6, blocks / 20);
  uint32_t last_onset = UINT32_MAX;
  uint32_t previous = 0;
  uint8_t attacks = 0;
  uint32_t block_index = 0;
  for (uint32_t pos = 0; pos < frames; pos += block, ++block_index) {
    uint64_t sum = 0;
    const uint32_t end = std::min(frames, pos + block);
    for (uint32_t i = pos; i < end; ++i) { sum += abs((int)pcm[i]); }
    const uint32_t energy = end > pos ? (uint32_t)(sum / (end - pos)) : 0;
    const bool separated = last_onset == UINT32_MAX || block_index - last_onset >= min_gap_blocks;
    const bool strong_rise = energy >= threshold && energy > previous + std::max<uint32_t>(64, previous / 2);
    if (separated && strong_rise) {
      ++attacks;
      last_onset = block_index;
    }
    previous = (previous * 3u + energy) / 4u;
    if ((block_index & 63u) == 0) { M5.delay(1); }
  }
  // Auto is intentionally conservative: unusual counts are used only when
  // the source presents a clear series of separated attacks.
  return attacks >= 4 ? std::min<uint8_t>(12, attacks) : 8;
}

static uint32_t chop_fit_frames(uint32_t source_frames, uint32_t sample_rate,
                                uint32_t reference_ms)
{
  if (!reference_ms || !sample_rate) { return source_frames; }
  const uint64_t reference_frames = ((uint64_t)reference_ms * sample_rate + 500) / 1000;
  const uint32_t candidates[] = {
    (uint32_t)std::max<uint64_t>(1, reference_frames / 4),
    (uint32_t)std::max<uint64_t>(1, reference_frames / 2),
    (uint32_t)std::max<uint64_t>(1, reference_frames),
    (uint32_t)std::min<uint64_t>((uint64_t)sample_rate * sampler_pool_t::max_sample_sec,
                                 reference_frames * 2)
  };
  uint32_t best = candidates[0];
  uint64_t best_error = source_frames > best ? source_frames - best : best - source_frames;
  for (uint32_t candidate : candidates) {
    const uint64_t error = source_frames > candidate ? source_frames - candidate : candidate - source_frames;
    if (error < best_error) { best = candidate; best_error = error; }
  }
  return best;
}

static void reset_chop_preview(void)
{
  edit_chop_preview_plan_valid = false;
  edit_chop_preview_count = 0;
  edit_chop_preview_index = 0;
  edit_chop_preview_last = -1;
}

static bool prepare_chop_preview_plan(void)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return false; }
  const auto& source = sampler_pool_t::slot[edit_pad];
  const uint32_t start = source.playStart();
  const uint32_t end = source.playEnd();
  constexpr uint32_t minimum_frames = 16;
  if (!source.isValid() || end <= start + 4 * minimum_frames) { return false; }

  const uint32_t source_frames = end - start;
  uint8_t count = edit_chop_count_mode == chop_count_mode_t::four ? 4
                : edit_chop_count_mode == chop_count_mode_t::twelve ? 12 : 8;
  if (edit_chop_count_mode == chop_count_mode_t::automatic) {
    count = detect_chop_count(source.pcm + start, source_frames, source.sample_rate);
  }
  if (source_frames <= count * minimum_frames) { return false; }

  uint16_t pitch_q8 = 256;
  if (edit_chop_fit_mode == chop_fit_mode_t::fit_bgm) {
    const uint32_t reference_length = background_loop.isValid()
      ? background_loop_length_ms() : loop_length_fixed ? loop_length_msec : 0;
    if (!reference_length) { return false; }
    const uint32_t target_frames = chop_fit_frames(source_frames, source.sample_rate,
                                                    reference_length);
    if (!target_frames) { return false; }
    pitch_q8 = (uint16_t)std::clamp<uint64_t>(
      ((uint64_t)source_frames * 256u + target_frames / 2u) / target_frames,
      32u, 2048u);
  }

  for (uint8_t i = 0; i <= count; ++i) {
    edit_chop_preview_boundaries[i] = (uint32_t)(((uint64_t)source_frames * i) / count);
  }
  if (!build_chop_slice_plan(source.pcm + start, source_frames, source.sample_rate,
                             count, edit_chop_preview_starts,
                             edit_chop_preview_ends, edit_chop_preview_anchors)) {
    return false;
  }
  edit_chop_preview_count = count;
  edit_chop_preview_pitch_q8 = pitch_q8;
  edit_chop_preview_index = 0;
  edit_chop_preview_last = -1;
  edit_chop_preview_plan_valid = true;
  return true;
}

static bool preview_next_chop_slice(void)
{
  if (!edit_chop_preview_plan_valid && !prepare_chop_preview_plan()) { return false; }
  if (edit_pad < 0 || edit_chop_preview_count == 0) { return false; }
  const auto& source = sampler_pool_t::slot[edit_pad];
  const uint8_t index = edit_chop_preview_index % edit_chop_preview_count;
  const uint32_t first = edit_chop_preview_starts[index];
  const uint32_t last = edit_chop_preview_ends[index];
  if (last <= first) { return false; }
  sampler_audio_t::stop((uint8_t)edit_pad);
  const bool played = sampler_audio_t::play((uint8_t)edit_pad,
    source.pcm + source.playStart() + first, last - first, source.sample_rate,
    false, false, mixer_scaled_volume_q8(mixer_part_t::sampler, source.volume_q8),
    edit_chop_preview_pitch_q8);
  if (!played) { return false; }
  edit_chop_preview_last = (int8_t)index;
  edit_chop_preview_index = (uint8_t)((index + 1) % edit_chop_preview_count);
  request_wave_draw();
  return true;
}

struct chop_key_result_t {
  bool valid = false;
  uint8_t key = 0;
  uint8_t voiced_windows = 0;
};

// Estimate a musical centre from several short regions of the PCM that will
// actually be chopped. The selected Scale remains untouched; only its root
// moves. Percussive or ambiguous material leaves Key unchanged.
static chop_key_result_t detect_chop_key(const int16_t* pcm, uint32_t frames,
                                         uint32_t sample_rate)
{
  chop_key_result_t result;
  if (!pcm || sample_rate < 4000 || frames < sample_rate / 3) { return result; }

  static constexpr uint32_t analysis_rate = 8000;
  static constexpr uint32_t analysis_points = 1024;
  static constexpr uint8_t requested_windows = 9;
  static constexpr uint32_t min_frequency = 65;
  static constexpr uint32_t max_frequency = 1046;
  static constexpr uint16_t confidence_min_q12 = 2450;
  const uint32_t stride = std::max<uint32_t>(1, sample_rate / analysis_rate);
  const uint32_t effective_rate = sample_rate / stride;
  const uint32_t available_points = frames / stride;
  const uint32_t count = std::min<uint32_t>(analysis_points, available_points);
  if (count < 384 || effective_rate <= max_frequency) { return result; }

  const uint32_t source_span = count * stride;
  const uint32_t max_start = frames > source_span ? frames - source_span : 0;
  const uint8_t window_count = max_start == 0 ? 1 : requested_windows;
  const uint32_t min_lag = std::max<uint32_t>(4, effective_rate / max_frequency);
  const uint32_t max_lag = std::min<uint32_t>(count / 2, effective_rate / min_frequency);
  if (max_lag <= min_lag + 2 || max_lag - min_lag >= 192) { return result; }

  uint32_t chroma[12] = {};
  uint32_t total_weight = 0;
  for (uint8_t window = 0; window < window_count; ++window) {
    const uint32_t start = window_count > 1
      ? (uint32_t)(((uint64_t)max_start * window) / (window_count - 1)) : 0;
    int64_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) { sum += pcm[start + i * stride]; }
    const int32_t mean = (int32_t)(sum / (int64_t)count);
    uint64_t signal_energy = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const int32_t value = (int32_t)pcm[start + i * stride] - mean;
      signal_energy += (int64_t)value * value;
    }
    if (signal_energy / count < 192u * 192u) { continue; }

    uint16_t scores[192] = {};
    uint16_t best_score = 0;
    uint32_t best_lag = 0;
    for (uint32_t lag = min_lag; lag <= max_lag; ++lag) {
      int64_t correlation = 0;
      uint64_t energy_a = 0;
      uint64_t energy_b = 0;
      for (uint32_t i = lag; i < count; ++i) {
        const int32_t a = (int32_t)pcm[start + i * stride] - mean;
        const int32_t b = (int32_t)pcm[start + (i - lag) * stride] - mean;
        correlation += (int64_t)a * b;
        energy_a += (int64_t)a * a;
        energy_b += (int64_t)b * b;
      }
      if (correlation <= 0) { continue; }
      const uint64_t energy = std::min(energy_a, energy_b);
      if (energy == 0) { continue; }
      const uint16_t score = (uint16_t)std::min<int64_t>(
        4095, (correlation << 12) / (int64_t)energy);
      scores[lag - min_lag] = score;
      if (score > best_score) { best_score = score; best_lag = lag; }
    }
    if (best_score < confidence_min_q12 || best_lag == 0) { continue; }

    const uint16_t fundamental_threshold = (uint16_t)((best_score * 3u) / 4u);
    for (uint32_t lag = min_lag + 1; lag < max_lag; ++lag) {
      const uint16_t previous = scores[lag - min_lag - 1];
      const uint16_t current = scores[lag - min_lag];
      const uint16_t next = scores[lag - min_lag + 1];
      if (current >= fundamental_threshold && current >= previous && current > next) {
        best_lag = lag;
        best_score = current;
        break;
      }
    }
    const float frequency = (float)effective_rate / (float)best_lag;
    const int midi_note = (int)lroundf(69.0f + 12.0f * log2f(frequency / 440.0f));
    if (midi_note < 24 || midi_note > 96) { continue; }
    uint32_t weight = std::max<uint32_t>(1, best_score - confidence_min_q12 + 1);
    if (window == 0 || window + 1 == window_count) { weight += weight / 3; }
    chroma[(uint8_t)midi_note % 12] += weight;
    total_weight += weight;
    ++result.voiced_windows;
    M5.delay(1);
  }
  if (result.voiced_windows < 2 || total_weight == 0) { return result; }

  uint16_t scale_mask = 0;
  const uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
  for (uint8_t note : sampler_scale_notes[scale]) {
    scale_mask |= (uint16_t)(1u << (note % 12));
  }
  int64_t best = INT64_MIN;
  int64_t second = INT64_MIN;
  uint8_t best_key = 0;
  for (uint8_t key = 0; key < 12; ++key) {
    int64_t score = (int64_t)chroma[key] * 6
                  + (int64_t)chroma[(key + 7) % 12] * 2;
    for (uint8_t pitch_class = 0; pitch_class < 12; ++pitch_class) {
      const uint8_t relative = (pitch_class + 12 - key) % 12;
      score += (scale_mask & (1u << relative))
        ? (int64_t)chroma[pitch_class] * 3
        : -(int64_t)chroma[pitch_class] * 4;
    }
    if (score > best) {
      second = best;
      best = score;
      best_key = key;
    } else if (score > second) {
      second = score;
    }
  }
  const int64_t minimum_margin = std::max<int64_t>(32, total_weight / 16);
  if (best <= 0 || best - second < minimum_margin) { return result; }
  result.valid = true;
  result.key = best_key;
  return result;
}

// Chopped songs are polyphonic, so a single autocorrelation peak is not a
// useful key estimate. Build a compact chromagram with a Goertzel bank and
// score its harmony separately from the low-frequency bass profile.
static chop_key_result_t detect_chop_music_key(const int16_t* pcm, uint32_t frames,
                                               uint32_t sample_rate)
{
  chop_key_result_t result;
  if (!pcm || sample_rate < 8000 || frames < sample_rate / 2) { return result; }

  static constexpr uint32_t analysis_rate = 8000;
  static constexpr uint32_t analysis_points = 1536;
  static constexpr uint8_t requested_windows = 9;
  static constexpr uint8_t first_midi_note = 36;  // C2
  static constexpr uint8_t last_midi_note = 95;   // B6
  static constexpr float two_pi = 6.28318530718f;
  const uint32_t stride = std::max<uint32_t>(1, sample_rate / analysis_rate);
  const uint32_t effective_rate = sample_rate / stride;
  const uint32_t source_span = analysis_points * stride;
  if (frames < source_span) { return result; }

  float* windowed = (float*)malloc(sizeof(float) * analysis_points);
  if (!windowed) { return result; }
  float chroma[12] = {};
  float bass_chroma[12] = {};
  const uint32_t max_start = frames - source_span;
  const uint8_t window_count = max_start ? requested_windows : 1;

  for (uint8_t window = 0; window < window_count; ++window) {
    const uint32_t start = window_count > 1
      ? (uint32_t)(((uint64_t)max_start * window) / (window_count - 1)) : 0;
    float mean = 0.0f;
    for (uint32_t i = 0; i < analysis_points; ++i) {
      mean += pcm[start + i * stride];
    }
    mean /= analysis_points;
    float energy = 0.0f;
    for (uint32_t i = 0; i < analysis_points; ++i) {
      const float phase = (float)i / (analysis_points - 1);
      const float hann = 0.5f - 0.5f * cosf(two_pi * phase);
      const float value = ((float)pcm[start + i * stride] - mean) * hann;
      windowed[i] = value;
      energy += value * value;
    }
    if (energy / analysis_points < 160.0f * 160.0f) { continue; }

    float amplitudes[last_midi_note - first_midi_note + 1] = {};
    float amplitude_sum = 0.0f;
    float amplitude_max = 0.0f;
    for (uint8_t note = first_midi_note; note <= last_midi_note; ++note) {
      const float frequency = 440.0f * powf(2.0f, ((int)note - 69) / 12.0f);
      const float coefficient = 2.0f * cosf(two_pi * frequency / effective_rate);
      float previous = 0.0f;
      float previous2 = 0.0f;
      for (uint32_t i = 0; i < analysis_points; ++i) {
        const float current = windowed[i] + coefficient * previous - previous2;
        previous2 = previous;
        previous = current;
      }
      const float power = std::max<float>(0.0f,
        previous * previous + previous2 * previous2 - coefficient * previous * previous2);
      const float amplitude = sqrtf(power);
      amplitudes[note - first_midi_note] = amplitude;
      amplitude_sum += amplitude;
      amplitude_max = std::max<float>(amplitude_max, amplitude);
    }
    const float mean_amplitude = amplitude_sum /
      (last_midi_note - first_midi_note + 1);
    if (mean_amplitude <= 0.0f || amplitude_max < mean_amplitude * 1.55f) { continue; }

    float useful_sum = 0.0f;
    for (float amplitude : amplitudes) {
      useful_sum += std::max<float>(0.0f, amplitude - mean_amplitude * 0.85f);
    }
    if (useful_sum <= 0.0f) { continue; }
    for (uint8_t note = first_midi_note; note <= last_midi_note; ++note) {
      const float useful = std::max<float>(
        0.0f, amplitudes[note - first_midi_note] - mean_amplitude * 0.85f);
      if (useful == 0.0f) { continue; }
      const float normalized = useful / useful_sum;
      chroma[note % 12] += normalized;
      if (note < 60) {
        // Lower octaves carry more information about the perceived root than
        // upper harmonics. Keep the weighting gentle so bass movement does
        // not override the chord vocabulary of the complete phrase.
        const float octave_weight = note < 48 ? 1.5f : 1.0f;
        bass_chroma[note % 12] += normalized * octave_weight;
      }
    }
    ++result.voiced_windows;
    draw_recording_processing_frame("FINDING KEY");
    M5.delay(1);
  }
  free(windowed);
  if (result.voiced_windows < 3) { return result; }

  uint16_t scale_mask = 0;
  const uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
  for (uint8_t note : sampler_scale_notes[scale]) {
    scale_mask |= (uint16_t)(1u << (note % 12));
  }
  float best = -1.0e30f;
  float second = -1.0e30f;
  uint8_t best_key = 0;
  for (uint8_t key = 0; key < 12; ++key) {
    float score = chroma[key] * 1.8f
                + chroma[(key + 7) % 12] * 0.5f
                + bass_chroma[key] * 4.5f
                + bass_chroma[(key + 7) % 12] * 0.5f;
    for (uint8_t pitch_class = 0; pitch_class < 12; ++pitch_class) {
      const uint8_t relative = (pitch_class + 12 - key) % 12;
      score += (scale_mask & (1u << relative))
        ? chroma[pitch_class] * 1.8f : -chroma[pitch_class] * 1.4f;
    }
    if (score > best) {
      second = best;
      best = score;
      best_key = key;
    } else if (score > second) {
      second = score;
    }
  }

  float chroma_total = 0.0f;
  float chroma_peak = 0.0f;
  uint8_t active_classes = 0;
  for (float value : chroma) {
    chroma_total += value;
    chroma_peak = std::max<float>(chroma_peak, value);
  }
  if (chroma_total > 0.0f) {
    for (float value : chroma) {
      if (value >= chroma_total * 0.055f) { ++active_classes; }
    }
  }
  const bool sustained_tone = chroma_total > 0.0f
    && chroma_peak >= chroma_total * 0.62f && result.voiced_windows >= 5;
  if ((!sustained_tone && active_classes < 2)
   || best <= 0.0f || best - second < 0.08f * result.voiced_windows) {
    return result;
  }
  result.valid = true;
  result.key = best_key;
  return result;
}

static bool chop_edit_sample(void)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return false; }
  const uint8_t source_pad = (uint8_t)edit_pad;
  const auto& source = sampler_pool_t::slot[source_pad];
  const uint32_t start = source.playStart();
  const uint32_t end = source.playEnd();
  uint8_t chop_count = edit_chop_count_mode == chop_count_mode_t::four ? 4
                     : edit_chop_count_mode == chop_count_mode_t::twelve ? 12 : 8;
  constexpr uint32_t minimum_frames = 16;
  if (!source.isValid() || end <= start + 4 * minimum_frames) { return false; }

  const bool fit_to_bgm = edit_chop_fit_mode == chop_fit_mode_t::fit_bgm;
  const uint32_t reference_length = background_loop.isValid()
    ? background_loop_length_ms() : loop_length_fixed ? loop_length_msec : 0;
  if (fit_to_bgm && !reference_length) { return false; }
  const uint32_t source_frames = end - start;
  const uint32_t working_frames = fit_to_bgm
    ? chop_fit_frames(source_frames, source.sample_rate, reference_length) : source_frames;
  const size_t working_bytes = (size_t)working_frames * sizeof(int16_t);
#if defined (M5UNIFIED_PC_BUILD)
  int16_t* working = (int16_t*)malloc(working_bytes);
#else
  int16_t* working = (int16_t*)heap_caps_malloc(working_bytes, MALLOC_CAP_SPIRAM);
#endif
  if (!working) { return false; }
  if (working_frames == source_frames) {
    memcpy(working, source.pcm + start, working_bytes);
  } else {
    for (uint32_t i = 0; i < working_frames; ++i) {
      const uint64_t source_q16 = working_frames > 1
        ? ((uint64_t)i * (source_frames - 1) << 16) / (working_frames - 1) : 0;
      const uint32_t a = (uint32_t)(source_q16 >> 16);
      const uint32_t b = std::min<uint32_t>(source_frames - 1, a + 1);
      const uint32_t fraction = (uint32_t)source_q16 & 0xFFFFu;
      working[i] = (int16_t)(((int64_t)source.pcm[start + a] * (65536u - fraction)
                            + (int64_t)source.pcm[start + b] * fraction) >> 16);
      if ((i & 4095u) == 0) { M5.delay(1); }
    }
  }
  if (edit_chop_count_mode == chop_count_mode_t::automatic) {
    chop_count = detect_chop_count(working, working_frames, source.sample_rate);
  }
  if (working_frames <= chop_count * minimum_frames) { free(working); return false; }

  draw_recording_processing_frame("FINDING KEY");
  const chop_key_result_t detected_key = detect_chop_music_key(
    working, working_frames, source.sample_rate);

  uint32_t slice_starts[12] = {};
  uint32_t slice_ends[12] = {};
  uint32_t slice_anchors[12] = {};
  if (!build_chop_slice_plan(working, working_frames, source.sample_rate, chop_count,
                             slice_starts, slice_ends, slice_anchors)) {
    free(working);
    return false;
  }

  uint8_t targets[12] = {};
  size_t replacing_bytes = 0;
  for (uint8_t i = 0; i < chop_count; ++i) {
    targets[i] = display_order_to_pad(i);
    replacing_bytes += sampler_pool_t::slot[targets[i]].bytes();
  }
  size_t chopped_bytes = 0;
  for (uint8_t i = 0; i < chop_count; ++i) {
    chopped_bytes += (size_t)(slice_ends[i] - slice_starts[i]) * sizeof(int16_t);
  }
  if (chopped_bytes > sampler_pool_t::freeBytes() + replacing_bytes) { free(working); return false; }

  char name[24] = {};
  snprintf(name, sizeof(name), "%s", source.name);
  const uint32_t source_rate = source.sample_rate;
  sampler_audio_t::stopAll();
  for (uint8_t i = 0; i < chop_count; ++i) {
    prepare_pad_for_new_sample(targets[i]);
  }
  exit_edit();
  recording_processing_static_drawn = false;
  recording_processing_frame = 0;
  draw_recording_processing_frame("CHOPPING");
  bool ok = true;
  for (uint8_t i = 0; i < chop_count; ++i) {
    char chop_name[24] = {};
    snprintf(chop_name, sizeof(chop_name), "%.16s %u", name, (unsigned)(i + 1));
    const uint32_t frames = slice_ends[i] - slice_starts[i];
    if (!sampler_pool_t::loadPcmPreserved(targets[i], chop_name,
                                          working + slice_starts[i], frames, source_rate)) {
      ok = false;
      break;
    }
    auto& chopped = sampler_pool_t::slot[targets[i]];
    chopped.beat_anchor_enabled = true;
    chopped.beat_anchor_frame = std::min<uint32_t>(slice_anchors[i], frames - 1);
    draw_recording_processing_frame("CHOPPING");
    M5.delay(1);
  }
  free(working);
  if (!ok) { return false; }

  if (detected_key.valid) {
    chord_settings.key = detected_key.key;
    bass_settings.key = detected_key.key;
    melody_settings.key = detected_key.key;
    refresh_pitched_pad_visuals(performance_page_t::melody);
    refresh_pitched_pad_visuals(performance_page_t::bass);
    request_chord_label_draw();
  }

  for (uint8_t i = 0; i < chop_count; ++i) {
    loop_remove_pad_events(targets[i]);
    loop_mute(performance_page_t::sample, targets[i]) = false;
    if (save_session_pad(targets[i])) { continue; }
  }
  repair_pitched_pad_sources();
  apply_synth_tones(true);
  // FIT uses the current BGM as its musical reference and keeps it playing.
  // KEEP SPEED starts a fresh beat-making surface, so only that mode removes
  // the BGM while preserving the reference loop length and grid.
  if (fit_to_bgm) {
    // Make the retained BGM and the sequencer share the exact reference that
    // was used for the speed conversion.
    loop_length_fixed = reference_length != 0;
    if (reference_length) { loop_length_msec = reference_length; }
    if (loop_length_fixed) { auto_configure_loop_grid(loop_length_msec); }
  } else if (background_loop.isValid()) {
    clear_background_loop();
    loop_length_fixed = reference_length != 0;
    if (reference_length) { loop_length_msec = reference_length; }
    if (loop_length_fixed) { auto_configure_loop_grid(loop_length_msec); }
  } else if (!loop_length_fixed && working_frames && source_rate) {
    loop_length_fixed = true;
    loop_length_msec = std::max<uint32_t>(loop_min_length_ms,
      (uint32_t)(((uint64_t)working_frames * 1000u) / source_rate));
    auto_configure_loop_grid(loop_length_msec);
  }
  save_resume_kit();
  rec_wave_pad = targets[0];
  processing_screen_visible = false;
  char status[32] = {};
  if (detected_key.valid) {
    snprintf(status, sizeof(status), "CHOPPED P1-P%u / KEY %s",
             (unsigned)chop_count, key_names[detected_key.key]);
  } else {
    snprintf(status, sizeof(status), "CHOPPED INTO P1-P%u", (unsigned)chop_count);
  }
  show_status_message(status, 1800, false);
  draw_all();
  update_all_leds();
  return true;
}

static void handle_edit_function_pad(int pad)
{
  if (edit_pad < 0 || pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  const uint8_t number = pad_display_number((uint8_t)pad);
  const uint32_t now = M5.millis();
  auto confirmed = [now](edit_notice_t action) {
    return edit_notice == action && (int32_t)(edit_notice_until_msec - now) > 0;
  };
  if (edit_chop_page) {
    switch (number) {
    case 1:
      if (!background_loop.isValid() && !loop_length_fixed) {
        show_edit_notice(edit_notice_t::chop_needs_bgm, edit_notice_duration_msec);
        return;
      }
      edit_chop_fit_mode = chop_fit_mode_t::fit_bgm;
      break;
    case 2: edit_chop_fit_mode = chop_fit_mode_t::keep_speed; break;
    case 5: edit_chop_count_mode = chop_count_mode_t::four; break;
    case 6: edit_chop_count_mode = chop_count_mode_t::eight; break;
    case 7: edit_chop_count_mode = chop_count_mode_t::twelve; break;
    case 8: edit_chop_count_mode = chop_count_mode_t::automatic; break;
    default: return;
    }
    edit_notice = edit_notice_t::none;
    edit_notice_until_msec = 0;
    sampler_audio_t::stop((uint8_t)edit_pad);
    reset_chop_preview();
    request_wave_draw();
    request_grid_draw();
    update_all_leds();
    return;
  }
  if (edit_synth_page) {
    if (loop_playing && (number == 1 || number == 2 || number == 3
                       || number == 9 || number == 10 || number == 11)) {
      show_edit_notice(edit_notice_t::stop_loop_to_edit_synth, edit_notice_duration_msec);
      return;
    }
    switch (number) {
    case 1: toggle_edit_synth_assignment(performance_page_t::melody, now); return;
    case 2: toggle_edit_synth_assignment(performance_page_t::chord, now); return;
    case 3: toggle_edit_synth_assignment(performance_page_t::bass, now); return;
    case 8:
      edit_synth_page = false;
      edit_param = 0;
      break;
    case 9: edit_param = 10; break;
    case 10: edit_param = 7; break;
    case 11: edit_param = 8; break;
    case 12: edit_param = 9; break;
    default: return;
    }
    edit_notice = edit_notice_t::none;
    edit_notice_until_msec = 0;
    edit_value_compact_visible = false;
    request_wave_draw();
    request_grid_draw();
    request_all_fn_draw();
    return;
  }
  if (loop_playing && number >= 1 && number <= 4) {
    show_edit_notice(edit_notice_t::stop_loop_to_edit_sample, edit_notice_duration_msec);
    return;
  }
  switch (number) {
  case 1:
    edit_chop_page = true;
    edit_chop_fit_mode = (background_loop.isValid() || loop_length_fixed)
      ? chop_fit_mode_t::fit_bgm : chop_fit_mode_t::keep_speed;
    edit_chop_count_mode = chop_count_mode_t::eight;
    reset_chop_preview();
    edit_notice = edit_notice_t::none;
    edit_notice_until_msec = 0;
    request_wave_draw();
    request_grid_draw();
    request_all_fn_draw();
    return;
  case 4:
    if (!confirmed(edit_notice_t::confirm_delete)) {
      show_edit_notice(edit_notice_t::confirm_delete, edit_confirm_duration_msec);
      return;
    }
    {
      const uint8_t target = (uint8_t)edit_pad;
      exit_edit();
      clear_pad_sample(target, true);
    }
    repair_pitched_pad_sources();
    apply_synth_tones(true);
    save_resume_kit();
    edit_notice = edit_notice_t::none;
    edit_notice_until_msec = 0;
    draw_all();
    update_all_leds();
    return;
  case 5:
    if (edit_param == 5) {
      slot.hold_enabled = !slot.hold_enabled;
      show_edit_notice(edit_notice_t::hold, edit_notice_duration_msec);
      request_grid_draw();
      return;
    }
    edit_param = 5;
    break;
  case 6:
    edit_param = 4;
    show_edit_notice(edit_notice_t::repeat, edit_notice_duration_msec);
    request_all_fn_draw();
    return;
  case 7:
    if (edit_param == 6) {
      slot.reverse = !slot.reverse;
      show_edit_notice(edit_notice_t::reverse, edit_notice_duration_msec);
      request_grid_draw();
      return;
    }
    edit_param = 6;
    break;
  case 8:
    edit_synth_page = true;
    edit_param = 10;
    break;
  case 9: edit_param = 0; break;
  case 10: edit_param = 1; break;
  case 11: edit_param = 2; break;
  case 12: edit_param = 3; break;
  default: return;
  }
  edit_notice = edit_notice_t::none;
  edit_notice_until_msec = 0;
  edit_value_compact_visible = false;
  request_wave_draw();
  request_grid_draw();
  request_all_fn_draw();
}

static uint8_t page_midi_channel(performance_page_t page)
{
  if (page == performance_page_t::chord) { return kp::def::midi::channel_2; }
  if (page == performance_page_t::bass) { return kp::def::midi::channel_3; }
  if (page == performance_page_t::drum) { return kp::def::midi::channel_10; }
  return kp::def::midi::channel_1;
}

static uint16_t melody_pitch_bend_scale_q12(performance_page_t page)
{
  // 2^(+/-1/12) in Q12. Linear interpolation across this narrow range is
  // perceptually smooth and avoids floating-point work in the 1ms loop task.
  const int32_t bend = page_pitch_bend[(uint8_t)page].current_q12;
  return (uint16_t)(bend >= 0
    ? 4096 + (bend * 244) / 4096   // 4096 -> 4340 (+1 semitone)
    : 4096 + (bend * 230) / 4096); // -4096 -> 3866 (-1 semitone)
}

static void set_page_pitch_bend_target(performance_page_t page, int16_t target_q12)
{
  if (page != performance_page_t::melody && page != performance_page_t::bass) { return; }
  auto& bend = page_pitch_bend[(uint8_t)page];
  bend.target_q12 = std::clamp<int16_t>(target_q12, -4096, 4096);
  bend.last_msec = M5.millis();
}

static void set_melody_pitch_bend_lever(bool down, bool pressed)
{
  const performance_page_t page = current_page;
  if (page != performance_page_t::melody && page != performance_page_t::bass) { return; }
  auto& bend = page_pitch_bend[(uint8_t)page];
  if (down) { bend.down_held = pressed; }
  else { bend.up_held = pressed; }
  const int16_t target = bend.up_held ? 4096 : bend.down_held ? -4096 : 0;
  set_page_pitch_bend_target(page, target);
  if (current_mode == sampler_mode_t::mode_loop) { record_page_pitch_bend(page, target); }
}

static void cancel_live_pitch_bend_levers(void)
{
  // Do not reset recorded automation here. Only a physically held lever is
  // cancelled, so a looped pitch-bend event remains part of the composition.
  for (performance_page_t page : { performance_page_t::melody, performance_page_t::bass }) {
    auto& bend = page_pitch_bend[(uint8_t)page];
    if (!bend.up_held && !bend.down_held) { continue; }
    bend.up_held = false;
    bend.down_held = false;
    set_page_pitch_bend_target(page, 0);
  }
}

static void apply_page_pitch_bend(performance_page_t page)
{
  auto& bend = page_pitch_bend[(uint8_t)page];
  auto& settings = page_settings(page);
  if (settings.source == synth_tone_source_t::general_midi) {
    // SAM2695 defaults to a +/-2 semitone bend range: 4096 MIDI units equals
    // one semitone, centred at 8192.
    const uint16_t value = (uint16_t)std::clamp<int32_t>(8192 + bend.current_q12, 0, 16383);
    send_sam_midi((uint8_t)kp::def::midi::pitch_bend | page_midi_channel(page),
                  value & 0x7F, value >> 7);
  } else {
    const uint16_t scale = melody_pitch_bend_scale_q12(page);
    const pitched_voice_owner_t owner = page == performance_page_t::bass
      ? pitched_voice_owner_t::bass : pitched_voice_owner_t::melody;
    for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
      if (pitched_voice_state[i].owner == owner) {
        sampler_audio_t::setVoicePitchScaleQ12(external_midi_voice_base + i, scale);
      }
    }
  }
}

static void service_page_pitch_bend(performance_page_t page, uint32_t now)
{
  auto& bend = page_pitch_bend[(uint8_t)page];
  if (bend.current_q12 == bend.target_q12) { return; }
  uint32_t elapsed = now - bend.last_msec;
  if (elapsed == 0) { return; }
  if (elapsed > 32) { elapsed = 32; }
  bend.last_msec = now;
  const uint32_t length = loop_length_fixed ? loop_length_msec : loop_default_length_ms;
  const uint32_t transition_ms = std::clamp<uint32_t>(loop_quantize_step_ms(length), 70, 150);
  const int32_t step = std::max<int32_t>(1, (4096 * (int32_t)elapsed + transition_ms - 1) / transition_ms);
  if (bend.current_q12 < bend.target_q12) {
    bend.current_q12 = std::min<int32_t>(bend.target_q12, bend.current_q12 + step);
  } else {
    bend.current_q12 = std::max<int32_t>(bend.target_q12, bend.current_q12 - step);
  }
  if (now - bend.last_apply_msec < 8 && bend.current_q12 != bend.target_q12) { return; }
  bend.last_apply_msec = now;
  apply_page_pitch_bend(page);
}

static void service_melody_pitch_bend(uint32_t now)
{
  service_page_pitch_bend(performance_page_t::melody, now);
  service_page_pitch_bend(performance_page_t::bass, now);
}

static void reset_page_pitch_bend(performance_page_t page, bool send_midi)
{
  auto& bend = page_pitch_bend[(uint8_t)page];
  bend = {};
  bend.last_msec = M5.millis();
  if (send_midi && page_settings(page).source == synth_tone_source_t::general_midi) {
    send_sam_midi((uint8_t)kp::def::midi::pitch_bend | page_midi_channel(page), 0, 64);
  }
}

static pitched_page_settings_t& page_settings(performance_page_t page)
{
  if (page == performance_page_t::chord) { return chord_settings; }
  if (page == performance_page_t::bass) { return bass_settings; }
  return melody_settings;
}

static uint8_t chord_degree_for_order(uint8_t order)
{
  static constexpr uint8_t degrees[] = { 1, 2, 3, 0, 4, 5, 6, 0, 7, 0, 0, 0 };
  return order < std::size(degrees) ? degrees[order] : 0;
}

static const chord_template_entry_t& chord_template_entry(uint8_t degree)
{
  static constexpr chord_template_entry_t fallback = { 0, chord_quality_t::major };
  if (degree == 0 || degree > 7) { return fallback; }
  return chord_template_for_scale(harmony_scale)[degree - 1];
}

static bool chord_api_default_minor(uint8_t degree)
{
  return degree == 2 || degree == 3 || degree == 6 || degree == 7;
}

static void chord_api_root_for_semitones(int semitones, uint8_t* degree, int* semitone_shift)
{
  static constexpr int root_semitones[] = { 0, 2, 4, 5, 7, 9, 11 };
  int best_degree = 1;
  int best_shift = semitones;
  int best_distance = 128;
  for (int i = 0; i < (int)std::size(root_semitones); ++i) {
    int shift = semitones - root_semitones[i];
    while (shift > 6) { shift -= 12; }
    while (shift < -6) { shift += 12; }
    const int distance = std::abs(shift);
    if (distance < best_distance) {
      best_degree = i + 1;
      best_shift = shift;
      best_distance = distance;
    }
  }
  *degree = (uint8_t)best_degree;
  *semitone_shift = best_shift;
}

static chord_quality_t chord_quality_with_swap(chord_quality_t quality, bool swap)
{
  if (!swap) { return quality; }
  switch (quality) {
  case chord_quality_t::major: return chord_quality_t::minor;
  case chord_quality_t::minor: return chord_quality_t::major;
  case chord_quality_t::diminished: return chord_quality_t::minor;
  case chord_quality_t::dominant7: return chord_quality_t::minor;
  }
  return quality;
}

static const char* chord_quality_suffix(chord_quality_t base, bool swap)
{
  const chord_quality_t quality = chord_quality_with_swap(base, swap);
  if (base == chord_quality_t::dominant7) { return swap ? "m7" : "7"; }
  switch (quality) {
  case chord_quality_t::minor: return "m";
  case chord_quality_t::diminished: return "o";
  default: return "";
  }
}

static int8_t chord_modifier_for_order(uint8_t order)
{
  switch (order) {
  case 3:  return 0;  // Swap major/minor
  case 7:  return 1;  // 7th
  case 9:  return 2;  // sus4
  case 10: return 3;  // Add9
  case 11: return 4;  // M7
  default: return -1;
  }
}

static void request_chord_label_draw(void)
{
  if (current_page != performance_page_t::chord) { return; }
  // Swap only changes the seven degree pads. Queue their full content redraw,
  // which is deferred while an input frame has audio priority.
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (chord_degree_for_order(pad_display_number(pad) - 1) != 0) {
      request_pad_draw(pad);
    }
  }
}

// Bit 0 is Minor Swap. Bits 1-3 store the selected chord modifier index:
// 0=None, 1=7th, 2=sus4, 3=9th, 4=M7. The compact form travels with a
// recorded root Note On event.
static uint8_t active_chord_modifier_index(void)
{
  for (int i = 4; i >= 1; --i) {
    if (chord_modifier_pressed[i]) { return (uint8_t)i; }
  }
  return 0;
}

static uint8_t chord_flags_from_pressed(void)
{
  return (chord_modifier_pressed[0] ? 1 : 0) | (active_chord_modifier_index() << 1);
}

static KANTANMusic_Modifier chord_modifier_from_flags(uint8_t flags)
{
  static constexpr KANTANMusic_Modifier modifiers[] = {
    KANTANMusic_Modifier_7,
    KANTANMusic_Modifier_sus4,
    KANTANMusic_Modifier_Add9,
    KANTANMusic_Modifier_M7,
  };
  const uint8_t index = (flags >> 1) & 0x07;
  return index >= 1 && index <= std::size(modifiers)
    ? modifiers[index - 1] : KANTANMusic_Modifier_None;
}

static KANTANMusic_Modifier chord_modifier_for_template(chord_quality_t base, uint8_t flags)
{
  // A held modifier adds or replaces an extension.  In particular, 7th
  // always ensures a seventh and never removes the Blues I7 / IV7 / V7.
  const KANTANMusic_Modifier held = chord_modifier_from_flags(flags);
  if (held != KANTANMusic_Modifier_None) { return held; }
  if (base == chord_quality_t::diminished && (flags & 1) == 0) {
    return KANTANMusic_Modifier_dim;
  }
  if (base == chord_quality_t::dominant7) { return KANTANMusic_Modifier_7; }
  return KANTANMusic_Modifier_None;
}

static KANTANMusic_Modifier active_chord_modifier(void)
{
  return chord_modifier_from_flags(chord_flags_from_pressed());
}

static uint8_t performance_notes(performance_page_t page, uint8_t pad,
                                 uint8_t* notes, uint8_t capacity,
                                 int chord_flags = -1)
{
  if (!notes || capacity == 0 || pad >= def::pad::pad_count) { return 0; }
  const uint8_t order = pad_display_number(pad) - 1;
  if (page == performance_page_t::melody || page == performance_page_t::bass) {
    const auto& settings = page_settings(page);
    uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
    notes[0] = std::clamp<int>(
      sampler_scale_notes[scale][order] + pitched_page_key(page)
        + pitched_page_octave_semitones(page, settings),
      0, 127);
    return 1;
  }
  if (page == performance_page_t::drum) {
    notes[0] = sampler_drum_notes[order];
    return 1;
  }
  if (page != performance_page_t::chord) { return 0; }
  uint8_t degree = chord_degree_for_order(order);
  if (degree == 0) { return 0; }
  const auto& chord = chord_template_entry(degree);
  KANTANMusic_GetMidiNoteNumberOptions options;
  KANTANMusic_GetMidiNoteNumber_SetDefaultOptions(&options);
  options.voicing = KANTANMusic_Voicing_Close;
  const uint8_t flags = chord_flags < 0 ? chord_flags_from_pressed() : (uint8_t)chord_flags;
  uint8_t api_degree = 1;
  int semitone_shift = 0;
  chord_api_root_for_semitones(chord.root_semitones, &api_degree, &semitone_shift);
  options.semitone_shift = semitone_shift;
  const chord_quality_t quality = chord_quality_with_swap(chord.quality, (flags & 1) != 0);
  // dimはModifierが品質を完全に決めるので、API側のMajor/Minor既定を
  // 不必要に反転させない。その他はTemplateの品質をそのまま指定する。
  const bool want_minor = quality == chord_quality_t::diminished
    ? chord_api_default_minor(api_degree)
    : quality == chord_quality_t::minor;
  options.minor_swap = want_minor != chord_api_default_minor(api_degree);
  options.modifier = chord_modifier_for_template(chord.quality, flags);
  static constexpr uint8_t chord_pitches[] = { 1, 4, 5, 6 };
  uint8_t count = 0;
  for (uint8_t pitch : chord_pitches) {
    if (count >= capacity) { break; }
    uint8_t note = KANTANMusic_GetMidiNoteNumber(pitch, api_degree, harmony_key(), &options);
    if (note != 0) {
      notes[count++] = std::clamp<int>((int)note + chord_settings.octave * 12, 1, 127);
    }
  }
  return count;
}

static bool performance_pad_has_sound(performance_page_t page, uint8_t pad)
{
  if (page == performance_page_t::sample) {
    return pad < def::pad::pad_count && sampler_pool_t::slot[pad].isValid();
  }
  uint8_t notes[4];
  return performance_notes(page, pad, notes, 4) != 0;
}

// Factory kits now leave Pad 9-12 empty. Keep old saved settings and imported
// kits playable when they still refer to one of those former default indexes.
static uint8_t resolved_pad_sound(const pitched_page_settings_t& settings)
{
  if (settings.pad < def::pad::pad_count && sampler_pool_t::slot[settings.pad].isValid()) {
    return settings.pad;
  }
  for (uint8_t order = 0; order < def::pad::pad_count; ++order) {
    const uint8_t pad = display_order_to_pad(order);
    if (sampler_pool_t::slot[pad].isValid()) { return pad; }
  }
  return def::pad::pad_count;
}

static void repair_pitched_pad_sources(void)
{
  const uint8_t melody_pad = resolved_pad_sound(melody_settings);
  const uint8_t chord_pad = resolved_pad_sound(chord_settings);
  const uint8_t bass_pad = resolved_pad_sound(bass_settings);
  if (melody_pad < def::pad::pad_count) { melody_settings.pad = melody_pad; }
  if (chord_pad < def::pad::pad_count) { chord_settings.pad = chord_pad; }
  if (bass_pad < def::pad::pad_count) { bass_settings.pad = bass_pad; }
}

static void detach_pitched_voice(uint8_t index)
{
  if (index >= external_midi_voice_count) { return; }
  const auto previous = pitched_voice_state[index];
  performance_page_t page = performance_page_t::sample;
  switch (previous.owner) {
  case pitched_voice_owner_t::melody: page = performance_page_t::melody; break;
  case pitched_voice_owner_t::chord:  page = performance_page_t::chord;  break;
  case pitched_voice_owner_t::bass:   page = performance_page_t::bass;   break;
  default: break;
  }
  if (page != performance_page_t::sample && previous.trigger < def::pad::pad_count) {
    auto& state = synth_trigger_state[(uint8_t)page][previous.trigger];
    for (uint8_t n = 0; n < state.note_count; ++n) {
      if (state.voices[n] == index && state.voice_generation[n] == previous.generation) {
        state.voices[n] = 0xFF;
        state.voice_generation[n] = 0;
      }
    }
  }
  external_midi_voice_note[index] = -1;
  pitched_voice_state[index] = {};
}

static uint8_t allocate_pitched_voice(pitched_voice_owner_t owner, uint8_t trigger, uint8_t note,
                                      bool live_performance)
{
  (void)live_performance;
  uint8_t selected = 0;
  uint32_t oldest = UINT32_MAX;
  for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
    if (!sampler_audio_t::isPlaying(external_midi_voice_base + i)) {
      selected = i;
      oldest = 0;
      break;
    }
    if (pitched_voice_state[i].age < oldest) {
      oldest = pitched_voice_state[i].age;
      selected = i;
    }
  }
  detach_pitched_voice(selected);
  uint32_t generation = pitched_voice_generation++;
  if (generation == 0) { generation = pitched_voice_generation++; }
  pitched_voice_state[selected] = { owner, trigger, note, pitched_voice_age++, generation };
  return selected;
}

static uint16_t sample_pitch_for_note(const sample_slot_t& slot, uint8_t note)
{
  static constexpr uint16_t ratio_q8[] = {
     64,  68,  72,  76,  81,  85,  91,  96, 102, 108, 114, 121,
    128, 136, 144, 152, 161, 171, 181, 192, 203, 215, 228, 242,
    256, 271, 287, 304, 323, 342, 362, 384, 407, 431, 456, 483,
    512, 542, 575, 609, 645, 684, 724, 767, 813, 861, 912, 967,
   1024
  };
  // Do not fold notes back into one octave. Melody's 12 pads intentionally
  // span roughly two octaves, so preserve their actual distance from the
  // detected Pad Base Note. +/-24 semitones is a practical PCM range.
  const int delta = std::clamp<int>((int)note - (int)slot.base_note, -24, 24);
  uint32_t pitch = ((uint32_t)slot.pitch_q8 * ratio_q8[delta + 24]) >> 8;
  return (uint16_t)std::clamp<uint32_t>(pitch, 32, 2048);
}

static void release_synth_trigger(performance_page_t page, uint8_t pad)
{
  if (pad >= def::pad::pad_count) { return; }
  auto& state = synth_trigger_state[(uint8_t)page][pad];
  synth_sounding_layer[(uint8_t)page][pad] = 0;
  // GM percussion on CH10 is One Shot. Releasing a physical pad must not
  // truncate it, and no Note Off traffic is needed for drum retriggers.
  if (page == performance_page_t::drum) {
    state = {};
    return;
  }
  uint8_t channel = page_midi_channel(page);
  const pitched_voice_owner_t owner = page == performance_page_t::chord
    ? pitched_voice_owner_t::chord
    : page == performance_page_t::bass ? pitched_voice_owner_t::bass
                                       : pitched_voice_owner_t::melody;
  for (uint8_t i = 0; i < state.note_count; ++i) {
    if (!state.midi && state.voices[i] != 0xFF) {
      const uint8_t voice = state.voices[i];
      // A loop event can reuse a PCM slot before its old Note Off is due.
      // Only release the exact allocation that this trigger originally made.
      if (pitched_voice_state[voice].owner == owner
       && pitched_voice_state[voice].trigger == pad
       && pitched_voice_state[voice].generation == state.voice_generation[i]) {
        sampler_audio_t::release(external_midi_voice_base + voice);
        pitched_voice_state[voice] = {};
      }
    } else if (state.midi) {
      send_sam_midi(kp::def::midi::note_off | channel, state.notes[i], 0);
    }
  }
  state = {};
}

// A finger slide is a legato replacement, not a series of released notes.
// Stop the old PCM voice immediately so long releases cannot fill the small
// pitched-voice pool before the newest touch note is started.
static void stop_synth_trigger(performance_page_t page, uint8_t pad)
{
  if (pad >= def::pad::pad_count) { return; }
  auto& state = synth_trigger_state[(uint8_t)page][pad];
  synth_sounding_layer[(uint8_t)page][pad] = 0;
  if (page == performance_page_t::drum) {
    state = {};
    return;
  }
  const uint8_t channel = page_midi_channel(page);
  for (uint8_t i = 0; i < state.note_count; ++i) {
    if (!state.midi && state.voices[i] != 0xFF) {
      sampler_audio_t::stop(external_midi_voice_base + state.voices[i]);
      pitched_voice_state[state.voices[i]] = {};
    } else if (state.midi) {
      send_sam_midi(kp::def::midi::note_off | channel, state.notes[i], 0);
    }
  }
  state = {};
}

static void stop_synth_page(performance_page_t page)
{
  if (page == performance_page_t::sample || page == performance_page_t::drum) { return; }
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    stop_synth_trigger(page, pad);
  }
}

// PCM is owned by each Pad, while live synth and repeat state is owned by the
// performance engine.  Stop both sides before replacing a Pad's PCM so no
// voice/cache can retain a pointer or parameter from the former sound.
static void prepare_pad_for_new_sample(uint8_t pad)
{
  if (pad >= def::pad::pad_count) { return; }
  sampler_audio_t::stop(pad);
  stop_sample_grid_loop(pad, false);
  clear_sampler_sustain_cache_for_pad(pad);
  pads[pad].pressed = false;
  pads[pad].playing_shown = false;

  for (const performance_page_t page : { performance_page_t::melody,
                                         performance_page_t::bass,
                                         performance_page_t::chord }) {
    const auto& settings = page_settings(page);
    if (settings.source == synth_tone_source_t::pad && settings.pad == pad) {
      stop_synth_page(page);
    }
    synth_loop_active_layer[(uint8_t)page][pad] = 0;
    synth_deferred_note_on_layer[(uint8_t)page][pad] = 0;
    synth_sounding_layer[(uint8_t)page][pad] = 0;
    synth_live_min_gate_until[(uint8_t)page][pad] = 0;
    set_synth_live_release_pending((uint8_t)page, pad, false);
    synth_live_release_layer[(uint8_t)page][pad] = 0;
  }
  if (external_midi_sound == external_midi_sound_t::pad && external_midi_pad == pad) {
    for (uint8_t voice = 0; voice < external_midi_voice_count; ++voice) {
      sampler_audio_t::stop(external_midi_voice_base + voice);
      external_midi_voice_note[voice] = -1;
      pitched_voice_state[voice] = {};
    }
  }
}

// Chord is intentionally a single harmonic part. A Pad-sourced chord uses
// four PCM voices, so retaining an older chord's release while a new chord is
// held doubles the expensive pitch-shift/sustain work. Replace the previous
// chord as one instrument; Melody, Bass and external MIDI keep their own
// polyphony and are never stolen here.
static void stop_active_chord_voices()
{
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    auto& state = synth_trigger_state[(uint8_t)performance_page_t::chord][pad];
    if (state.midi) {
      for (uint8_t i = 0; i < state.note_count; ++i) {
        send_sam_midi(kp::def::midi::note_off | page_midi_channel(performance_page_t::chord),
                      state.notes[i], 0);
      }
    } else {
      for (uint8_t i = 0; i < state.note_count; ++i) {
        if (state.voices[i] != 0xFF) {
          sampler_audio_t::stop(external_midi_voice_base + state.voices[i]);
          pitched_voice_state[state.voices[i]] = {};
        }
      }
    }
    state = {};
    synth_sounding_layer[(uint8_t)performance_page_t::chord][pad] = 0;
  }
}

static void trigger_synth_pad(performance_page_t page, uint8_t pad, int chord_flags,
                              bool live_performance)
{
  if (page == performance_page_t::sample || pad >= def::pad::pad_count) { return; }
  if (mixer_part_muted[(uint8_t)mixer_part_for_page(page)]) { return; }
  if (page == performance_page_t::bass) { release_other_bass_notes(pad); }
  if (page == performance_page_t::chord && live_performance) { stop_active_chord_voices(); }
  release_synth_trigger(page, pad);
  synth_sounding_layer[(uint8_t)page][pad] = 0;
  uint8_t notes[4] = {};
  uint8_t note_count = performance_notes(page, pad, notes, 4, chord_flags);
  if (note_count == 0) { return; }

  uint32_t next_epoch = synth_trigger_epoch[(uint8_t)page][pad] + 1u;
  synth_trigger_epoch[(uint8_t)page][pad] = next_epoch ? next_epoch : 1u;

  auto& state = synth_trigger_state[(uint8_t)page][pad];
  state.note_count = note_count;
  uint8_t volume = page == performance_page_t::drum ? drum_volume : page_settings(page).volume;
  if (page == performance_page_t::drum || page_settings(page).source == synth_tone_source_t::general_midi) {
    state.midi = true;
    uint8_t channel = page_midi_channel(page);
    uint8_t velocity = std::max<int>(1, std::min<int>(127, (volume * 127) / 100));
    for (uint8_t i = 0; i < note_count; ++i) {
      state.notes[i] = notes[i];
      state.voices[i] = 0xFF;
      send_sam_midi(kp::def::midi::note_on | channel, notes[i], velocity);
    }
    return;
  }

  auto& settings = page_settings(page);
  const uint8_t source_pad = resolved_pad_sound(settings);
  if (source_pad >= def::pad::pad_count) { state = {}; return; }
  auto& slot = sampler_pool_t::slot[source_pad];
  if (!slot.isValid() || slot.playFrames() == 0) { state = {}; return; }
  pitched_voice_owner_t owner = page == performance_page_t::chord
    ? pitched_voice_owner_t::chord
    : page == performance_page_t::bass
      ? pitched_voice_owner_t::bass : pitched_voice_owner_t::melody;
  uint16_t volume_q8 = (uint16_t)std::min<uint32_t>(512,
    ((uint32_t)slot.volume_q8 * settings.volume) / 100);
  volume_q8 = mixer_scaled_volume_q8(mixer_part_for_page(page), volume_q8);
  uint32_t sustain_start = 0;
  uint32_t sustain_end = 0;
  uint16_t sustain_crossfade = 0;
  const uint32_t source_start = slot.playStart();
  const uint32_t source_frames = slot.playEnd() - source_start;
  bool sustain = synth_sustain_parameters(slot, source_start,
                                           &sustain_start, &sustain_end, &sustain_crossfade);
  // Sustained Pad synthesis is the expensive path: it continuously reads a
  // PSRAM loop, and several recorded parts can be active with BGM. During a
  // running loop, use the same 24kHz internal renderer as Chord for every
  // sustained synth voice. The output envelope stays at 48kHz, so timing and
  // release remain smooth while leaving the I2S task headroom for Note Off.
  const uint8_t render_divider = (page == performance_page_t::chord
                               || (loop_playing && sustain)) ? 2 : 1;
  for (uint8_t i = 0; i < note_count; ++i) {
    uint8_t voice = allocate_pitched_voice(owner, pad, notes[i], live_performance);
    state.notes[i] = notes[i];
    state.voices[i] = voice;
    state.voice_generation[i] = pitched_voice_state[voice].generation;
    sampler_audio_t::playSynth(external_midi_voice_base + voice,
      slot.pcm + source_start, source_frames, slot.sample_rate,
      sustain, slot.reverse, volume_q8,
      sample_pitch_for_note(slot, notes[i]), 0, slot.synth_release_ms,
      sustain_start, sustain_end, sustain_crossfade, 0,
      // Chords are up to four simultaneous Pad voices. Their 48kHz source
      // remains clear with nearest-neighbour pitch stepping, while avoiding
      // four extra PSRAM reads per frame that previously stalled UI changes.
      render_divider == 1,
      render_divider,
      synth_sustain_cache_slot(page));
    if (page == performance_page_t::melody || page == performance_page_t::bass) {
      sampler_audio_t::setVoicePitchScaleQ12(external_midi_voice_base + voice,
                                              melody_pitch_bend_scale_q12(page));
    }
  }
}

static bool touch_play_supported_page(void)
{
  return current_mode == sampler_mode_t::mode_play
      && (current_page == performance_page_t::melody || current_page == performance_page_t::bass);
}

static uint8_t touch_play_note_for_order(performance_page_t page, uint8_t order)
{
  const auto& settings = page_settings(page);
  const uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
  return (uint8_t)std::clamp<int>(sampler_scale_notes[scale][order]
    + pitched_page_key(page) + pitched_page_octave_semitones(page, settings), 0, 127);
}

static void draw_touch_play_row(m5gfx::LovyanGFX& d, performance_page_t page, uint8_t order)
{
  (void)page;
  const int w = d.width();
  const int h = d.height();
  const int row_h = h / (int)def::pad::pad_count;
  const uint32_t accent = performance_page_colors[(uint8_t)performance_page_t::melody];
  static constexpr const char* note_names[] = {
    "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
  };
  const int row = (int)def::pad::pad_count - 1 - order;
  const int y = row * row_h;
  // The touch surface represents shared scale degrees. Melody and Bass still
  // use their own octave when the note is triggered.
  const uint8_t note = touch_play_note_for_order(performance_page_t::melody, order);
  const uint32_t bg = scale_rgb24(accent, 1, 7);
  d.fillRect(0, y, w, row_h - 1, bg);
  d.drawFastHLine(0, y + row_h - 1, w, 0x383840u);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_left);
  d.setTextColor(0xE0E0E8u, bg);
  char label[12];
  snprintf(label, sizeof(label), "%s", note_names[note % 12]);
  d.drawString(label, 8, y + row_h / 2);
}

static uint32_t touch_play_marker_pixels(void)
{
  return 5u * (uint32_t)(M5.Display.height() / (int)def::pad::pad_count);
}

static void draw_touch_play_pad_marker(m5gfx::LovyanGFX& d, performance_page_t page,
                                       int pad, bool active)
{
  (void)page;
  if (pad < 0 || pad >= def::pad::pad_count) { return; }
  const int row_h = d.height() / (int)def::pad::pad_count;
  const uint8_t order = pad_display_number((uint8_t)pad) - 1;
  const int row = (int)def::pad::pad_count - 1 - order;
  const int y = row * row_h;
  const uint32_t accent = performance_page_colors[(uint8_t)performance_page_t::melody];
  d.fillRect(0, y, 5, row_h - 1, active ? accent : scale_rgb24(accent, 1, 7));
}

static int touch_play_tone_x(m5gfx::LovyanGFX& d, uint8_t tone_position)
{
  return ((int)tone_position * (d.width() - 1)) / 127;
}

static void draw_touch_play_tone_base(m5gfx::LovyanGFX& d)
{
  const int w = d.width();
  const int h = d.height();
  d.fillRect(0, h - 8, w, 8, 0x08080Cu);
  d.drawFastVLine(w / 2, h - 8, 8, 0x808088u);
}

static void draw_touch_play_tone_marker(m5gfx::LovyanGFX& d, performance_page_t page,
                                        uint8_t tone_position, bool active)
{
  (void)page;
  const int h = d.height();
  const int x = touch_play_tone_x(d, tone_position);
  const int left = std::max(0, x - 1);
  const int width = std::min<int>((int)d.width() - left, 3);
  d.fillRect(left, h - 7, width, 6,
             active ? performance_page_colors[(uint8_t)performance_page_t::melody] : 0x08080Cu);
  const int center_x = d.width() / 2;
  if (!active && left <= center_x && left + width > center_x) {
    d.drawFastVLine(center_x, h - 8, 8, 0x808088u);
  }
}

static bool ensure_touch_play_surface_cache(void)
{
  if (!touch_play_surface_canvas_ready
   || touch_play_surface_canvas.getBuffer() == nullptr) { return false; }
  const uint8_t key = harmony_key();
  const uint8_t scale = std::min<uint8_t>(harmony_scale, sampler_scale_count - 1);
  if (touch_play_surface_cache_key == key
   && touch_play_surface_cache_scale == scale) { return true; }
  // Keep a matching retained Kaoss surface when it already exists, but never
  // rebuild its full-screen PSRAM image while BLE MIDI is the active source.
  if (ble_midi_cache_guard_active()) { return false; }

  auto& d = touch_play_surface_canvas;
  d.startWrite();
  d.fillScreen(0x101018u);
  for (uint8_t order = 0; order < def::pad::pad_count; ++order) {
    draw_touch_play_row(d, performance_page_t::melody, order);
  }
  draw_touch_play_tone_base(d);
  d.endWrite();
  touch_play_surface_cache_key = key;
  touch_play_surface_cache_scale = scale;
  return true;
}

static void ui_render_metrics_record(uint32_t started_usec, uint32_t pixels)
{
  const uint32_t elapsed = M5.micros() - started_usec;
  ui_render_metrics.last_usec = elapsed;
  ui_render_metrics.last_pixels = pixels;
  if (elapsed > ui_render_metrics.max_usec) { ui_render_metrics.max_usec = elapsed; }
  if (pixels > ui_render_metrics.max_pixels) { ui_render_metrics.max_pixels = pixels; }
  ui_render_metrics.jobs = ui_render_metrics.jobs + 1;
}

static void draw_touch_play_surface(performance_page_t page, int pad, uint8_t tone_position)
{
  auto& d = M5.Display;
  const uint32_t started_usec = M5.micros();
  if (ensure_touch_play_surface_cache()) {
    touch_play_surface_canvas.pushSprite(0, 0);
  } else {
    d.startWrite();
    d.fillScreen(0x101018u);
    for (uint8_t order = 0; order < def::pad::pad_count; ++order) {
      draw_touch_play_row(d, page, order);
    }
    draw_touch_play_tone_base(d);
    d.endWrite();
  }
  d.startWrite();
  draw_touch_play_pad_marker(d, page, pad, true);
  draw_touch_play_tone_marker(d, page, tone_position, true);
  d.endWrite();
  ui_render_metrics_record(started_usec, (uint32_t)d.width() * d.height());
}

#if !defined(M5UNIFIED_PC_BUILD)
static void touch_render_task(void*)
{
  touch_render_state_t state;
  int drawn_pad = -1;
  uint8_t drawn_tone = 0xFF;
  performance_page_t drawn_page = performance_page_t::melody;
  for (;;) {
    const QueueSetMemberHandle_t ready = xQueueSelectFromSet(ui_render_queue_set, portMAX_DELAY);
    if (ready == ui_tile_render_queue) {
      ui_tile_transfer_t transfer;
      if (xQueueReceive(ui_tile_render_queue, &transfer, 0) == pdTRUE
       && transfer.canvas_index < grid_cache_count) {
        const uint32_t started_usec = M5.micros();
        if (transfer.kind == ui_tile_transfer_t::kind_t::grid) {
          // A monolithic PSRAM -> LCD copy can contend with audio samples
          // streaming from the same memory. Transfer short horizontal bands
          // and yield between them so loop/audio work always gets a chance.
          static constexpr int grid_strip_h = 8;
          for (int offset = 0; offset < grid_cache_h
                            && transfer.page_generation == ui_page_generation;
               offset += grid_strip_h) {
            const int height = std::min<int>(grid_strip_h, grid_cache_h - offset);
            M5.Display.setClipRect(transfer.x, transfer.y + offset,
                                   grid_cache_w, height);
            grid_cache_canvas[transfer.canvas_index].pushSprite(transfer.x, transfer.y);
            M5.Display.clearClipRect();
            taskYIELD();
          }
          ui_render_metrics_record(started_usec, (uint32_t)grid_cache_w * grid_cache_h);
          grid_cache_busy[transfer.canvas_index] = false;
        } else if (transfer.canvas_index < 2) {
          if (transfer.page_generation == ui_page_generation) {
            ui_dirty_canvas[transfer.canvas_index].pushSprite(transfer.x, transfer.y);
            ui_render_metrics_record(started_usec, (uint32_t)pad_w * cell_h);
          }
          ui_dirty_canvas_busy[transfer.canvas_index] = false;
        }
      }
      continue;
    }
    if (ready != touch_render_queue
     || xQueueReceive(touch_render_queue, &state, 0) != pdTRUE) { continue; }
    if (state.command == touch_render_command_t::wave_strip) {
      const bool valid = wave_transfer_active
                      && state.wave_generation == wave_transfer_generation
                      && !ui_surface_exclusive && state.wave_h > 0;
      if (valid) {
        const uint32_t started_usec = M5.micros();
        M5.Display.setClipRect(0, wave_y + state.wave_y,
                               wave_canvas.width(), state.wave_h);
        wave_canvas.pushSprite(0, wave_y);
        M5.Display.clearClipRect();
        ui_render_metrics_record(started_usec,
          (uint32_t)wave_canvas.width() * state.wave_h);

        // Only the renderer advances a queued transfer. The producer keeps
        // wave_transfer_active set, so it cannot rewrite the Canvas midway.
        wave_transfer_y += state.wave_h;
        wave_transfer_h -= state.wave_h;
        if (wave_transfer_h <= 0) {
          if (!wave_transfer_full_frame) { draw_live_wave_frame(); }
          wave_transfer_active = false;
          wave_transfer_full_frame = false;
        }
      }
      wave_transfer_job_pending = false;
      continue;
    }
    if (state.command == touch_render_command_t::stop) {
      drawn_pad = -1;
      drawn_tone = 0xFF;
      xSemaphoreGive(touch_render_stopped);
      continue;
    }
    // An update may replace the initial full request before this low-priority
    // task wakes. An uninitialised surface therefore always promotes the
    // newest request to a full draw.
    if (state.command == touch_render_command_t::full
     || drawn_tone == 0xFF || state.page != drawn_page) {
      draw_touch_play_surface(state.page, state.pad, state.tone_position);
      drawn_page = state.page;
      drawn_pad = state.pad;
      drawn_tone = state.tone_position;
      continue;
    }

    auto& d = M5.Display;
    const uint32_t started_usec = micros();
    uint32_t pixels = 0;
    d.startWrite();
    if (drawn_pad != state.pad) {
      draw_touch_play_pad_marker(d, state.page, drawn_pad, false);
      draw_touch_play_pad_marker(d, state.page, state.pad, true);
      pixels += touch_play_marker_pixels() * 2;
      drawn_pad = state.pad;
    }
    if (drawn_tone != state.tone_position) {
      if (drawn_tone != 0xFF) {
        draw_touch_play_tone_marker(d, state.page, drawn_tone, false);
      }
      draw_touch_play_tone_marker(d, state.page, state.tone_position, true);
      pixels += 36;
      drawn_tone = state.tone_position;
    }
    d.endWrite();
    ui_render_metrics_record(started_usec, pixels);
  }
}

static bool queue_touch_render(touch_render_command_t command)
{
  if (touch_render_queue == nullptr || touch_render_task_handle == nullptr) { return false; }
  touch_render_state_t state;
  state.command = command;
  state.page = current_page;
  state.pad = (int8_t)touch_play_pad;
  state.tone_position = touch_play_tone_position;
  if (uxQueueMessagesWaiting(touch_render_queue) != 0) {
    ui_render_metrics.dropped_requests = ui_render_metrics.dropped_requests + 1;
  }
  return xQueueOverwrite(touch_render_queue, &state) == pdTRUE;
}
#endif

static void touch_play_set_tone_target(int x)
{
  const int width = std::max<int>(1, M5.Display.width() - 1);
  x = std::clamp<int>(x, 0, width);
  const int center = width / 2;
  uint8_t cutoff = 127;
  uint8_t resonance = 0;
  uint8_t expression = 127;
  if (x < center) {
    // Centre is the untouched sound.  The left half closes a steep low-pass
    // and gently reduces expression, so dark timbres remain clearly audible.
    const int distance = center - x;
    const int curve = (distance * distance * 125) / std::max<int>(1, center * center);
    cutoff = (uint8_t)std::clamp<int>(127 - curve, 2, 127);
    expression = (uint8_t)std::clamp<int>(127 - (distance * 44) / std::max(1, center), 83, 127);
  } else if (x > center) {
    // The right half starts from the identical original sound, then adds a
    // resonant high-frequency colour.  Keeping some cutoff below 127 gives
    // the resonance a real band to emphasise instead of merely changing a
    // number that cannot be heard.
    const int distance = x - center;
    const int right_width = std::max(1, width - center);
    const int curve = (distance * distance * 34) / (right_width * right_width);
    cutoff = (uint8_t)std::clamp<int>(127 - curve, 93, 127);
    resonance = (uint8_t)std::clamp<int>((distance * 127) / right_width, 0, 127);
  }
  touch_play_target_cutoff = cutoff;
  touch_play_target_resonance = resonance;
  touch_play_target_expression = expression;
  touch_play_target_tone_position = (uint8_t)((x * 127) / width);
}

static void touch_play_apply_tone(uint8_t cutoff, uint8_t resonance, uint8_t expression)
{
  const auto& settings = page_settings(current_page);
  if (settings.source == synth_tone_source_t::general_midi) {
    const uint8_t channel = page_midi_channel(current_page);
    send_sam_midi(0xB0 | channel, 74, cutoff);
    send_sam_midi(0xB0 | channel, 71, resonance);
    send_sam_midi(0xB0 | channel, 11, expression);
  } else if (touch_play_pad >= 0) {
    const auto& state = synth_trigger_state[(uint8_t)current_page][touch_play_pad];
    for (uint8_t i = 0; i < state.note_count; ++i) {
      if (!state.midi && state.voices[i] != 0xFF) {
        sampler_audio_t::setVoiceToneFilter(external_midi_voice_base + state.voices[i],
                                            cutoff, resonance);
      }
    }
  }
}

static void service_touch_play(uint32_t now)
{
  if (!touch_play_active) { return; }
  if (touch_play_pad != touch_play_target_pad
   && now - touch_play_note_msec >= touch_play_note_interval_msec) {
    if (touch_play_pad >= 0) {
      stop_synth_trigger(current_page, (uint8_t)touch_play_pad);
    }
    touch_play_pad = touch_play_target_pad;
    if (touch_play_pad >= 0) {
      trigger_synth_pad(current_page, (uint8_t)touch_play_pad, -1, true);
      touch_play_apply_tone(touch_play_target_cutoff, touch_play_target_resonance,
                            touch_play_target_expression);
      mark_sound_priority(12);
    }
    touch_play_note_msec = now;
  }
  if ((touch_play_cutoff != touch_play_target_cutoff
    || touch_play_resonance != touch_play_target_resonance
    || touch_play_expression != touch_play_target_expression
    || touch_play_tone_position != touch_play_target_tone_position)
   && now - touch_play_filter_msec >= touch_play_filter_interval_msec) {
    touch_play_filter_msec = now;
    touch_play_cutoff = touch_play_target_cutoff;
    touch_play_resonance = touch_play_target_resonance;
    touch_play_expression = touch_play_target_expression;
    touch_play_tone_position = touch_play_target_tone_position;
    touch_play_apply_tone(touch_play_cutoff, touch_play_resonance, touch_play_expression);
  }
  if (now - touch_play_visual_msec < touch_play_visual_interval_msec) { return; }
  if (touch_play_drawn_pad == touch_play_pad
   && touch_play_drawn_tone_position == touch_play_tone_position) { return; }
  touch_play_visual_msec = now;
#if !defined(M5UNIFIED_PC_BUILD)
  // A length-one queue intentionally replaces an old visual request with the
  // newest state. Audio and touch scanning never wait for the LCD bus.
  queue_touch_render(touch_render_command_t::update);
  touch_play_drawn_pad = touch_play_pad;
  touch_play_drawn_tone_position = touch_play_tone_position;
#else
  auto& d = M5.Display;
  const uint32_t started_usec = M5.micros();
  uint32_t pixels = 0;
  d.startWrite();
  if (touch_play_drawn_pad != touch_play_pad) {
    draw_touch_play_pad_marker(d, current_page, touch_play_drawn_pad, false);
    draw_touch_play_pad_marker(d, current_page, touch_play_pad, true);
    pixels += touch_play_marker_pixels() * 2;
    touch_play_drawn_pad = touch_play_pad;
  }
  if (touch_play_drawn_tone_position != touch_play_tone_position) {
    if (touch_play_drawn_tone_position != 0xFF) {
      draw_touch_play_tone_marker(d, current_page, touch_play_drawn_tone_position, false);
    }
    draw_touch_play_tone_marker(d, current_page, touch_play_tone_position, true);
    pixels += 36;
    touch_play_drawn_tone_position = touch_play_tone_position;
  }
  d.endWrite();
  ui_render_metrics_record(started_usec, pixels);
#endif
}

static void handle_touch_play(int x, int y, bool pressed)
{
  if (!touch_play_active) { return; }
  if (!pressed) {
    if (touch_play_pad >= 0) { release_synth_trigger(current_page, (uint8_t)touch_play_pad); }
    touch_play_pad = -1;
    touch_play_target_pad = -1;
    return;
  }
  const int height = std::max<int>(1, M5.Display.height());
  y = std::clamp<int>(y, 0, height - 1);
  touch_play_set_tone_target(x);
  const uint8_t order = (uint8_t)(((height - 1 - y) * def::pad::pad_count) / height);
  const uint8_t pad = display_order_to_pad(std::min<uint8_t>(order, def::pad::pad_count - 1));
  touch_play_target_pad = pad;
  // The first contact keeps the pad-like immediate attack. Subsequent slide
  // points are coalesced by service_touch_play() above.
  if (touch_play_pad < 0) {
    touch_play_pad = pad;
    trigger_synth_pad(current_page, pad, -1, true);
    // A newly allocated Pad voice needs the current tone immediately. This
    // updates audio state only; LCD work remains in service_touch_play().
    touch_play_apply_tone(touch_play_target_cutoff, touch_play_target_resonance,
                          touch_play_target_expression);
    touch_play_note_msec = M5.millis();
    mark_sound_priority(12);
  }
}

static void set_touch_play_active(bool active)
{
  if (active) {
    if (!touch_play_supported_page() || touch_play_active) { return; }
    touch_play_active = true;
    touch_play_pad = -1;
    touch_play_target_pad = -1;
    touch_play_drawn_pad = -1;
    touch_play_cutoff = 127;
    touch_play_resonance = 0;
    touch_play_expression = 127;
    touch_play_tone_position = 64;
    touch_play_target_cutoff = 127;
    touch_play_target_resonance = 0;
    touch_play_target_expression = 127;
    touch_play_target_tone_position = 64;
    touch_play_drawn_tone_position = 0xFF;
    touch_play_filter_msec = M5.millis();
    touch_play_visual_msec = M5.millis();
    touch_play_note_msec = M5.millis();
    // Suspend normal partial UI updates; the full touch surface owns the LCD
    // until Fn3 is released, avoiding waveform redraws over the live panel.
    ui_surface_exclusive = true;
    // Discard a waveform/timeline transfer that may already be in progress.
    // Its remaining strips would otherwise overwrite this surface later.
    wave_transfer_generation = wave_transfer_generation + 1;
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    wave_transfer_job_pending = false;
#if !defined(M5UNIFIED_PC_BUILD)
    if (!queue_touch_render(touch_render_command_t::full)) {
      draw_touch_play_surface(current_page, touch_play_pad, touch_play_tone_position);
    }
#else
    draw_touch_play_surface(current_page, touch_play_pad, touch_play_tone_position);
#endif
    return;
  }
  if (!touch_play_active) { return; }
  if (touch_play_pad >= 0) { release_synth_trigger(current_page, (uint8_t)touch_play_pad); }
  const auto& settings = page_settings(current_page);
  if (settings.source == synth_tone_source_t::general_midi) {
    const uint8_t channel = page_midi_channel(current_page);
    send_sam_midi(0xB0 | channel, 74, 127);
    send_sam_midi(0xB0 | channel, 71, 0);
    send_sam_midi(0xB0 | channel, 11, 127);
  }
  touch_play_active = false;
  touch_play_pad = -1;
  touch_play_target_pad = -1;
  touch_play_drawn_pad = -1;
#if !defined(M5UNIFIED_PC_BUILD)
  if (touch_render_queue != nullptr && touch_render_stopped != nullptr) {
    while (xSemaphoreTake(touch_render_stopped, 0) == pdTRUE) {}
    queue_touch_render(touch_render_command_t::stop);
    // Normal UI must not race the final Core-0 LCD transaction. The timeout
    // preserves recovery if the renderer was not created successfully.
    xSemaphoreTake(touch_render_stopped, pdMS_TO_TICKS(120));
  }
#endif
  ui_surface_exclusive = false;
  restore_performance_surface_from_cache();
  update_all_leds();
}

static void release_other_chord_roots(uint8_t selected_pad)
{
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (pad == selected_pad
     || chord_degree_for_order(pad_display_number(pad) - 1) == 0) { continue; }
    release_synth_trigger(performance_page_t::chord, pad);
  }
}

static void release_other_bass_notes(uint8_t selected_pad)
{
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (pad != selected_pad) { release_synth_trigger(performance_page_t::bass, pad); }
  }
}

static void clear_synth_runtime(void)
{
  for (uint8_t page = (uint8_t)performance_page_t::melody;
       page < (uint8_t)performance_page_t::max; ++page) {
    for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
      release_synth_trigger((performance_page_t)page, pad);
      synth_loop_active_layer[page][pad] = 0;
      synth_deferred_note_on_layer[page][pad] = 0;
      synth_sounding_layer[page][pad] = 0;
      synth_live_min_gate_until[page][pad] = 0;
      synth_live_release_pending[page][pad] = false;
      synth_live_release_layer[page][pad] = 0;
      soft_snap_pending[page][pad] = false;
      soft_snap_released[page][pad] = false;
      soft_snap_release_due[page][pad] = 0;
      soft_snap_release_epoch[page][pad] = 0;
      synth_trigger_epoch[page][pad] = 0;
    }
    synth_live_release_pending_mask[page] = 0;
  }
  for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
    pitched_voice_state[i] = {};
    external_midi_voice_note[i] = -1;
  }
  std::fill(chord_modifier_pressed, chord_modifier_pressed + 5, false);
  request_chord_label_draw();
}

// Padの再生方式に従って発音する
static void trigger_pad(int pad) {
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid()) { return; }
  const int16_t* pcm = slot.pcm + slot.playStart();
  uint32_t frames = slot.playFrames();
  if (frames == 0) { return; }
  if (slot.loop_enabled && slot.loop_whole_sample) {
    if (play_sample_sustain_voice(pad, !slot.hold_enabled)) { return; }
    // Whole Sample is an audio-voice loop, deliberately independent of the
    // loop transport and Note Grid. Toggle mode stops on the next press.
    if (!slot.hold_enabled && sample_whole_loop_active[pad]) {
      stop_sample_grid_loop(pad);
    } else {
      stop_sample_grid_loop(pad, false);
      play_sample_whole_loop(pad);
    }
    return;
  }
  if (slot.loop_enabled && !slot.hold_enabled) {
    // Toggle Loop: 押すとGridリピート開始 / 再度押すと停止。
    if (sample_grid_loop_active[pad]) {
      stop_sample_grid_loop(pad);
    } else {
      start_sample_grid_loop(pad, M5.millis());
    }
    return;
  }
  if (slot.loop_enabled) {
    // Hold Loop: 押下中だけGridリピートする。
    start_sample_grid_loop(pad, M5.millis());
    return;
  }
  stop_sample_grid_loop(pad, false);
  play_sample_once(pad);
}

static bool defer_live_pad_if_early(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return false; }
  uint32_t now = M5.millis();
  uint32_t raw_pos = loop_record_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  if (!loop_should_defer_quantized_note(raw_pos, pos)) { return false; }
  loop_deferred_live_pad[pad] = true;
  loop_deferred_live_pos_ms[pad] = pos;
  loop_deferred_live_start_frame[pad] = 0;
  return true;
}

static bool trigger_anchor_synced_pad(int pad, bool queue_future = true)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return false; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.beatAnchorValid() || slot.loop_enabled || !loop_playing
   || !loop_length_fixed || loop_length_msec == 0) { return false; }

  const uint32_t raw_pos = loop_record_pos_ms(performance_event_time());
  uint32_t target = weighted_quantize_loop_pos_ms(raw_pos, loop_length_msec,
                                                   loop_quantize_steps());
  const uint32_t pre_roll_ms = sample_anchor_preroll_ms(slot);
  uint32_t desired_start = target >= pre_roll_ms
    ? target - pre_roll_ms : loop_length_msec - pre_roll_ms + target;
  uint32_t ahead = loop_forward_distance_ms(raw_pos, desired_start, loop_length_msec);
  uint32_t elapsed = loop_forward_distance_ms(desired_start, raw_pos, loop_length_msec);
  const uint32_t step_ms = std::max<uint32_t>(1, loop_quantize_step_ms(loop_length_msec));

  // A late hit catches up inside the short pre-roll. If it is already too far
  // beyond that window, use the next grid instead of skipping the attack.
  if (elapsed <= std::max<uint32_t>(pre_roll_ms + 24, 32u)) {
    const uint32_t source_frames = (uint32_t)(((uint64_t)elapsed * slot.sample_rate
                                             * slot.pitch_q8) / (1000u * 256u));
    play_sample_once(pad, std::min<uint32_t>(source_frames, slot.playFrames() - 1));
    return true;
  }
  if (ahead > step_ms) {
    target = (target + step_ms) % loop_length_msec;
    desired_start = target >= pre_roll_ms
      ? target - pre_roll_ms : loop_length_msec - pre_roll_ms + target;
  }
  if (queue_future) {
    loop_deferred_live_pad[pad] = true;
    loop_deferred_live_pos_ms[pad] = desired_start;
    loop_deferred_live_start_frame[pad] = 0;
  }
  return true;
}

static bool soft_snap_page(performance_page_t page)
{
  return page == performance_page_t::drum
      || page == performance_page_t::bass
      || page == performance_page_t::chord;
}

static bool defer_live_synth_if_early(performance_page_t page, int pad,
                                      uint8_t chord_flags)
{
  if (!soft_snap_page(page) || pad < 0 || pad >= (int)def::pad::pad_count) { return false; }
  const uint32_t raw_pos = loop_record_pos_ms(performance_event_time());
  const uint32_t pos = quantize_loop_pos_ms(raw_pos, loop_length_msec);
  if (!loop_should_defer_quantized_note(raw_pos, pos)) { return false; }
  const uint8_t page_index = (uint8_t)page;
  if (page == performance_page_t::bass || page == performance_page_t::chord) {
    for (uint8_t other = 0; other < def::pad::pad_count; ++other) {
      soft_snap_pending[page_index][other] = false;
      soft_snap_released[page_index][other] = false;
    }
  }
  soft_snap_pending[page_index][pad] = true;
  soft_snap_released[page_index][pad] = false;
  soft_snap_pos_ms[page_index][pad] = pos;
  soft_snap_chord_flags[page_index][pad] = chord_flags;
  return true;
}

static bool release_deferred_live_synth(performance_page_t page, int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return false; }
  const uint8_t page_index = (uint8_t)page;
  if (!soft_snap_pending[page_index][pad]) { return false; }
  soft_snap_released[page_index][pad] = true;
  return true;
}

static void service_soft_snap_live(uint32_t now, uint32_t prev_pos, uint32_t pos)
{
  for (uint8_t page = 0; page < (uint8_t)performance_page_t::max; ++page) {
    for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
      if (soft_snap_release_due[page][pad]
       && (int32_t)(now - soft_snap_release_due[page][pad]) >= 0) {
        const uint32_t release_epoch = soft_snap_release_epoch[page][pad];
        soft_snap_release_due[page][pad] = 0;
        soft_snap_release_epoch[page][pad] = 0;
        if (release_epoch != 0 && synth_trigger_epoch[page][pad] == release_epoch) {
          release_synth_trigger((performance_page_t)page, pad);
        }
      }
      if (!soft_snap_pending[page][pad]
       || !loop_event_crossed(prev_pos, pos, soft_snap_pos_ms[page][pad])) { continue; }
      soft_snap_pending[page][pad] = false;
      trigger_synth_pad((performance_page_t)page, pad,
                        soft_snap_chord_flags[page][pad], true);
      if (soft_snap_released[page][pad] && page != (uint8_t)performance_page_t::drum) {
        soft_snap_release_due[page][pad] = now + loop_live_min_gate_ms;
        soft_snap_release_epoch[page][pad] = synth_trigger_epoch[page][pad];
      }
      soft_snap_released[page][pad] = false;
    }
  }
}

static void push_loop_event(uint8_t pad, loop_event_type_t type, uint32_t pos_ms, uint16_t layer)
{
  const loop_event_t event { pad, type, pos_ms, layer };
  bool snapshot_appended = false;
  {
    loop_events_guard_t guard;
    if (loop_events.size() >= loop_event_max) {
      loop_events.erase(loop_events.begin());
    } else {
      snapshot_appended = append_loop_playback_snapshot(event);
    }
    loop_events.push_back(event);
  }
  if (type == loop_event_type_t::note_on && layer != 0) {
    auto& history = loop_undo_history[(uint8_t)performance_page_t::sample];
    if (history.empty() || history.back() != layer) { history.push_back(layer); }
  }
  // A newly appended event does not invalidate the retained piano-roll
  // background.  Update its tiny mark in-place when that view is visible;
  // structural edits still use invalidate_loop_timeline_cache().
  advance_loop_events_revision();
  if (!append_loop_timeline_event(event)) {
    loop_timeline_cache_valid = false;
    loop_cursor_prev_x = -1;
    loop_timeline_dirty_span_count = 0;
  }
  if (snapshot_appended) { loop_playback_revision = loop_events_revision; }
}

static void push_loop_event(performance_page_t page, uint8_t pad,
                            loop_event_type_t type, uint32_t pos_ms, uint16_t layer,
                            uint8_t chord_flags = 0)
{
  const loop_event_t event { page, pad, type, pos_ms, layer, chord_flags };
  bool snapshot_appended = false;
  {
    loop_events_guard_t guard;
    if (loop_events.size() >= loop_event_max) { loop_events.erase(loop_events.begin()); }
    else { snapshot_appended = append_loop_playback_snapshot(event); }
    loop_events.push_back(event);
  }
  if ((type == loop_event_type_t::note_on || loop_event_is_pitch_bend(type)) && layer != 0) {
    auto& history = loop_undo_history[(uint8_t)page];
    if (history.empty() || history.back() != layer) { history.push_back(layer); }
  }
  advance_loop_events_revision();
  if (!append_loop_timeline_event(event)) {
    loop_timeline_cache_valid = false;
    loop_cursor_prev_x = -1;
    loop_timeline_dirty_span_count = 0;
  }
  if (snapshot_appended) { loop_playback_revision = loop_events_revision; }
}

static void loop_transport_started_visual(void)
{
  // A stopped piano-roll may have a compact length overlay. Starting from a
  // Pad must clear it just like the PLAY button, rather than leaving it above
  // append-only note updates.
  loop_recording_notice_shown = false;
  // A fixed loop already has a label-free piano-roll base in wave_canvas.
  // Restore only the former overlay rectangle instead of reconstructing that
  // image in the attack path.
  if (loop_length_fixed && loop_timeline_cache_valid && !wave_transfer_active) {
    loop_cursor_prev_x = -1;
    loop_length_label_overlay_pending = false;
    loop_length_label_restore_pending = loop_length_label_overlay_visible;
    return;
  }
  wave_transfer_generation = wave_transfer_generation + 1;
  wave_transfer_active = false;
  wave_transfer_full_frame = false;
  request_wave_draw();
}

static void loop_start_length_capture(uint32_t now)
{
  loop_length_fixed = false;
  loop_prev_pos_ms = 0;
  loop_start_msec = now;
  // 1回の同時打鍵を確実に同じ先頭ステップへ記録する。ここでは
  // 音の発音を遅延させず、記録位置だけを0msに正規化する。
  loop_capture_zero_until_msec = now + 24;
  loop_playing = true;
  loop_transport_started_visual();
}

static void record_page_pitch_bend(performance_page_t page, int16_t target_q12)
{
  if (page != performance_page_t::melody && page != performance_page_t::bass) { return; }
  uint32_t now = M5.millis();
  if (!loop_playing) {
    if (!loop_length_fixed && loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = loop_length_msec ? loop_length_msec - 1 : 0;
      loop_start_msec = now;
      loop_playing = true;
      loop_transport_started_visual();
      play_background_loop_at(0);
    }
  }
  uint32_t raw_pos = loop_record_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  loop_event_type_t type = target_q12 > 0 ? loop_event_type_t::pitch_bend_up
    : target_q12 < 0 ? loop_event_type_t::pitch_bend_down
                     : loop_event_type_t::pitch_bend_center;
  uint16_t& active_layer = pitch_bend_record_layer[(uint8_t)page];
  if (active_layer == 0) {
    if (target_q12 == 0) { return; }
    active_layer = loop_layer_seq++;
  }
  push_loop_event(page, 0, type, pos, active_layer);
  if (target_q12 == 0) { active_layer = 0; }
  // push_loop_event() updates a visible piano-roll with a narrow dirty span.
  // Requesting the full Wave canvas here used to rebuild the entire timeline
  // for every pitch-bend point, exactly while the performer needs audio
  // headroom most. Structural timeline changes still request a full redraw.
}

static void loop_finish_length_capture(uint32_t now)
{
  if (!loop_playing || loop_length_fixed) { return; }
  uint32_t elapsed = loop_display_length_ms(now);
  if (elapsed < loop_min_length_ms) { elapsed = loop_min_length_ms; }
  loop_length_msec = elapsed;
  loop_length_fixed = true;
  auto_configure_loop_grid(loop_length_msec);
  loop_capture_zero_until_msec = 0;
  quantize_loop_events_to_length(loop_length_msec);
  invalidate_loop_timeline_cache();
  loop_prev_pos_ms = loop_length_msec - 1;
  // The captured length follows the physical button edge, while the newly
  // started playback transport begins when audio can actually be restarted.
  loop_start_msec = M5.millis();
  loop_record_enabled = true;
  clear_synth_runtime();
  sampler_audio_t::stopAll();
  clear_sample_grid_loops();
}

static void loop_toggle_play(void);

static void loop_handle_top_button(void)
{
  uint32_t now = performance_event_time();
  if (!loop_length_fixed) {
    loop_finish_length_capture(now);
    if (!loop_length_fixed) { return; }
  } else {
    loop_toggle_play();
    return;
  }
  request_wave_draw();
  request_all_fn_draw();
}

static void trigger_loop_event(const loop_event_t& event, bool live_input = false,
                               bool start_at_anchor = false)
{
  if (loop_event_is_pitch_bend(event.type)) {
    if (event.page != performance_page_t::melody && event.page != performance_page_t::bass) { return; }
    if (performance_page_part_muted(event.page)) { return; }
    int16_t target = event.type == loop_event_type_t::pitch_bend_up ? 4096
      : event.type == loop_event_type_t::pitch_bend_down ? -4096 : 0;
    set_page_pitch_bend_target(event.page, target);
    return;
  }
  int pad = event.pad;
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  if (event.page != performance_page_t::sample) {
    // Older KITs may contain Drum Note Off events. CH10 percussion is always
    // One Shot, so ignore them as well as no longer recording new ones.
    if (event.page == performance_page_t::drum
     && event.type == loop_event_type_t::note_off) { return; }
    if (event.page == performance_page_t::chord) {
      uint8_t order = pad_display_number((uint8_t)pad) - 1;
      // Legacy KITs can contain modifier-button events. Modifiers are now
      // input-only state, never a playable or recorded loop event.
      if (chord_modifier_for_order(order) >= 0) { return; }
      // A physically held chord owns the harmonic part while it is being
      // recorded. Let only its deferred Note On through at the chosen grid;
      // older loop events must not replace it or send a stray Note Off.
      if (!live_input) {
        bool live_chord_held = false;
        for (uint8_t root = 0; root < def::pad::pad_count; ++root) {
          if (synth_loop_active_layer[(uint8_t)performance_page_t::chord][root] != 0) {
            live_chord_held = true;
            break;
          }
        }
        const bool deferred_live_note = event.type == loop_event_type_t::note_on
          && synth_deferred_note_on_layer[(uint8_t)performance_page_t::chord][pad] == event.layer;
        if (live_chord_held && !deferred_live_note) { return; }
      }
    }
    if (event.type == loop_event_type_t::note_off) {
      // Note Off is quantized independently at a finer grid. If a newer layer
      // has already retriggered this pad, the older release must not stop it.
      if (event.layer != 0
       && synth_sounding_layer[(uint8_t)event.page][pad] != event.layer) { return; }
      release_synth_trigger(event.page, (uint8_t)pad);
    } else if (!loop_is_muted(event.page, (uint8_t)pad)) {
      if (event.layer != 0
       && synth_deferred_note_on_layer[(uint8_t)event.page][pad] == event.layer) {
        synth_deferred_note_on_layer[(uint8_t)event.page][pad] = 0;
      }
      if (event.page == performance_page_t::chord) {
        release_other_chord_roots((uint8_t)pad);
      }
      trigger_synth_pad(event.page, (uint8_t)pad, event.chord_flags, live_input);
      synth_sounding_layer[(uint8_t)event.page][pad] = event.layer;
    }
    return;
  }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  if (event.type == loop_event_type_t::note_off) {
    if (event.layer != 0 && loop_deferred_note_on_layer[pad] == event.layer) {
      loop_deferred_note_on_layer[pad] = 0;
    }
    if (slot.hold_enabled) {
      uint32_t sustain_start = 0;
      uint32_t sustain_end = 0;
      uint16_t sustain_crossfade = 0;
      if (synth_sustain_parameters(slot, slot.playStart(),
                                   &sustain_start, &sustain_end, &sustain_crossfade)) {
        sampler_audio_t::release((uint8_t)pad);
        stop_sample_grid_loop(pad, false);
      } else {
        stop_sample_grid_loop(pad);
      }
    }
    return;
  }
  if (event.layer != 0 && loop_deferred_note_on_layer[pad] == event.layer) {
    loop_deferred_note_on_layer[pad] = 0;
  }
  if (loop_is_muted(event.page, (uint8_t)pad)) {
    stop_sample_grid_loop(pad);
    return;
  }

  if (slot.loop_enabled && slot.loop_whole_sample) { play_sample_whole_loop(pad); }
  else if (slot.loop_enabled) { start_sample_grid_loop(pad, M5.millis()); }
  else {
    const uint32_t offset = start_at_anchor && slot.beatAnchorValid()
      ? slot.beat_anchor_frame - slot.playStart() : 0;
    play_sample_once(pad, offset);
  }
}

static void loop_toggle_play(void)
{
  uint32_t now = M5.millis();
  if (loop_playing) {
    finish_performance_recording();
    loop_playing = false;
    loop_prev_pos_ms = 0;
    for (int i = 0; i < (int)def::pad::pad_count; ++i) {
      loop_deferred_live_pad[i] = false;
    }
    clear_synth_runtime();
    reset_page_pitch_bend(performance_page_t::melody, true);
    reset_page_pitch_bend(performance_page_t::bass, true);
    std::fill(pitch_bend_record_layer,
              pitch_bend_record_layer + (uint8_t)performance_page_t::max, 0);
    sampler_audio_t::stopAll();
    clear_sample_grid_loops();
    apply_synth_tones(true);
    reset_mixer_mix();
    loop_reset_recording_state_if_empty();
  } else {
    if (performance_record_armed && !begin_performance_recording()) {
      performance_record_armed = false;
      request_header_draw();
      show_status_message("RECORDING UNAVAILABLE", 1800, false);
    }
    apply_pending_mixer_snapshot();
    // 先頭(0ms)のイベントも、再生開始時の最初の境界通過として必ず発火させる。
    // 0msから始めると event_pos == 0 が既通過と見なされ、先頭の音だけ抜ける。
    loop_prev_pos_ms = loop_length_msec ? loop_length_msec - 1 : 0;
    loop_start_msec = now;
    loop_playing = true;
    loop_transport_started_visual();
    reset_page_pitch_bend(performance_page_t::melody, true);
    reset_page_pitch_bend(performance_page_t::bass, true);
    play_background_loop_at(0);
    apply_synth_tones(true);
    // Anchorのプリロールは本来、前の周回の末尾から始まる。
    // 最初の1周目だけは前周が無いため拍頭から開始し、無音にしない。
    refresh_loop_playback_events();
    for (size_t i = 0; i < loop_playback_event_count; ++i) {
      const auto& event = loop_playback_events[i];
      if (event.page == performance_page_t::sample
       && event.type == loop_event_type_t::note_on && event.pos_ms == 0
       && event.pad < def::pad::pad_count
       && sampler_pool_t::slot[event.pad].beatAnchorValid()) {
        trigger_loop_event(event, false, true);
      }
    }
  }
  request_wave_draw();
  request_fn_draw(0);
}

static void stop_all_audio(bool reset_mixer)
{
  const bool was_loop_playing = loop_playing;
  if (was_loop_playing) { finish_performance_recording(); }
  if (loop_playing) {
    loop_playing = false;
    loop_prev_pos_ms = 0;
  }
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    loop_active_layer[i] = 0;
    loop_deferred_note_on_layer[i] = 0;
    loop_live_min_gate_until[i] = 0;
    loop_live_release_pending[i] = false;
    loop_live_release_layer[i] = 0;
    loop_deferred_live_pad[i] = false;
  }
  loop_live_release_pending_mask = 0;
  clear_synth_runtime();
  reset_page_pitch_bend(performance_page_t::melody, true);
  reset_page_pitch_bend(performance_page_t::bass, true);
  std::fill(pitch_bend_record_layer,
            pitch_bend_record_layer + (uint8_t)performance_page_t::max, 0);
  sampler_audio_t::stopAll();
  if (reset_mixer && was_loop_playing) { reset_mixer_mix(); }
  clear_sample_grid_loops();
  apply_synth_tones(true);
  loop_reset_recording_state_if_empty();
  for (auto& note : external_midi_voice_note) { note = -1; }
  request_wave_draw();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    request_pad_state_draw(i);
  }
  request_all_fn_draw();
}

static bool loop_undo_current_page(void)
{
  auto& history = loop_undo_history[(uint8_t)current_page];
  uint16_t layer = 0;
  while (!history.empty()) {
    uint16_t candidate = history.back();
    history.pop_back();
    bool exists = std::any_of(loop_events.begin(), loop_events.end(),
      [candidate](const loop_event_t& e) {
        return e.page == current_page && e.layer == candidate;
      });
    if (exists) { layer = candidate; break; }
  }
  if (layer == 0) { return false; }
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    if (current_page == performance_page_t::sample) {
      if (loop_active_layer[i] == layer) { loop_active_layer[i] = 0; }
      if (loop_deferred_note_on_layer[i] == layer) { loop_deferred_note_on_layer[i] = 0; }
      continue;
    }
    const uint8_t page = (uint8_t)current_page;
    if (synth_loop_active_layer[page][i] == layer) { synth_loop_active_layer[page][i] = 0; }
    if (synth_deferred_note_on_layer[page][i] == layer) {
      synth_deferred_note_on_layer[page][i] = 0;
    }
    if (synth_sounding_layer[page][i] == layer) {
      release_synth_trigger(current_page, (uint8_t)i);
    }
  }
  {
    loop_events_guard_t guard;
    loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
      [layer](const loop_event_t& e) {
        return e.page == current_page && e.layer == layer;
      }), loop_events.end());
  }
  invalidate_loop_timeline_cache();
  request_wave_draw();
  return true;
}

static void loop_record_pad(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return; }
  // Retriggering this fixed Pad voice replaces the former note. Discard its
  // delayed minimum-gate Release before it can stop the new attack.
  set_loop_live_release_pending((uint8_t)pad, false);
  loop_live_release_layer[pad] = 0;
  loop_live_min_gate_until[pad] = 0;
  uint32_t now = performance_event_time();
  bool restarted_transport = false;
  if (!loop_playing) {
    if (!loop_length_fixed && loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = 0;
      loop_start_msec = M5.millis();
      loop_playing = true;
      loop_transport_started_visual();
      restarted_transport = true;
      play_background_loop_at(0);
    }
  }
  uint32_t raw_pos = restarted_transport ? 0 : loop_record_pos_ms(now);
  const bool anchor_sync = sampler_pool_t::slot[pad].beatAnchorValid()
                        && !sampler_pool_t::slot[pad].loop_enabled
                        && loop_length_fixed;
  bool defer_note_on = false;
  uint32_t pos = anchor_sync
    ? weighted_quantize_loop_pos_ms(raw_pos, loop_length_msec, loop_quantize_steps())
    : loop_record_note_on_pos_ms(raw_pos, true, &defer_note_on);
  const bool missed_deferred_grid = defer_note_on && !loop_deferred_grid_is_still_ahead(pos);
  uint16_t layer = loop_layer_seq++;
  if (sampler_pool_t::slot[pad].hold_enabled) {
    loop_active_layer[pad] = layer;
  }
  if (anchor_sync) {
    if (restarted_transport) {
      // performance_event_time() can precede the freshly assigned
      // loop_start_msec by a few milliseconds. Recomputing transport time in
      // trigger_anchor_synced_pad() would then wrap the unsigned position and
      // may classify this first hit as a future event without queuing it.
      // At transport zero there is no preceding cycle for Anchor pre-roll, so
      // start explicitly at the beat anchor, matching PLAY-button startup.
      trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_on,
                           0, layer }, true, true);
    } else {
      trigger_anchor_synced_pad(pad, false);
    }
  } else if (defer_note_on && !missed_deferred_grid) {
    loop_deferred_note_on_layer[pad] = layer;
  } else {
    trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_on, raw_pos, layer });
    // A short Hold released in the same drained input batch must remain
    // audible until its recorded minimum-gate Note Off.
    if (missed_deferred_grid && sampler_pool_t::slot[pad].hold_enabled) {
      loop_deferred_note_on_layer[pad] = layer;
      loop_live_min_gate_until[pad] = M5.millis() + loop_live_min_gate_ms;
      set_loop_live_release_pending((uint8_t)pad, false);
      loop_live_release_layer[pad] = 0;
    }
  }
  // Live audio must never wait for vector maintenance or the playback task.
  push_loop_event((uint8_t)pad, loop_event_type_t::note_on, pos, layer);
  request_fn_draw(0);  // 再生状態が変わるためPLAY/STOPアイコンを更新
}

static void loop_record_pad_release(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || !slot.hold_enabled || loop_active_layer[pad] == 0) { return; }
  uint32_t now = performance_event_time();
  uint32_t raw_pos = loop_record_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  uint16_t layer = loop_active_layer[pad];
  loop_active_layer[pad] = 0;
  if (loop_deferred_note_on_layer[pad] == layer) {
    loop_deferred_note_on_layer[pad] = 0;
    auto note_on = std::find_if(loop_events.begin(), loop_events.end(), [layer](const loop_event_t& e) {
      return e.layer == layer && e.type == loop_event_type_t::note_on;
    });
    if (note_on != loop_events.end()) {
      uint32_t off_pos = loop_note_off_after_note_on(note_on->pos_ms, loop_length_msec);
      if (loop_live_min_gate_until[pad] != 0) {
        const uint32_t candidate = quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec);
        const uint32_t duration = loop_forward_distance_ms(note_on->pos_ms, candidate, loop_length_msec);
        if (duration > 0 && duration < loop_length_msec / 2) { off_pos = candidate; }
      }
      push_loop_event((uint8_t)pad, loop_event_type_t::note_off, off_pos, layer);
    }
    if (loop_live_min_gate_until[pad] != 0) {
      if ((int32_t)(M5.millis() - loop_live_min_gate_until[pad]) >= 0) {
        trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_off, raw_pos, layer });
        loop_live_min_gate_until[pad] = 0;
      } else {
        set_loop_live_release_pending((uint8_t)pad, true);
        loop_live_release_layer[pad] = layer;
      }
    }
    invalidate_loop_timeline_cache();
    return;
  }
  trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_off, raw_pos, layer });
  push_loop_event((uint8_t)pad, loop_event_type_t::note_off, pos, layer);
}

static void loop_record_synth_pad(performance_page_t page, int pad)
{
  if (page == performance_page_t::sample || pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  // Pitched Pad voices can have a long Release. A pending callback from the
  // preceding tap belongs to that old layer, not the voice reused below.
  set_synth_live_release_pending((uint8_t)page, (uint8_t)pad, false);
  synth_live_release_layer[(uint8_t)page][pad] = 0;
  synth_live_min_gate_until[(uint8_t)page][pad] = 0;
  const bool chord_root = page == performance_page_t::chord
    && chord_degree_for_order(pad_display_number((uint8_t)pad) - 1) != 0;
  if (page == performance_page_t::chord && !chord_root) { return; }
  uint32_t now = performance_event_time();
  bool restarted_transport = false;
  if (!loop_playing) {
    if (!loop_length_fixed && loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = loop_length_msec ? loop_length_msec - 1 : 0;
      loop_start_msec = M5.millis();
      loop_playing = true;
      loop_transport_started_visual();
      restarted_transport = true;
      play_background_loop_at(0);
    }
  }
  uint32_t raw_pos = restarted_transport ? 0 : loop_record_pos_ms(now);
  bool defer_note_on = false;
  uint32_t pos = loop_record_note_on_pos_ms(raw_pos, true, &defer_note_on);
  const bool missed_deferred_grid = defer_note_on && !loop_deferred_grid_is_still_ahead(pos);
  if (chord_root) {
    for (uint8_t other = 0; other < def::pad::pad_count; ++other) {
      if (other == (uint8_t)pad
       || chord_degree_for_order(pad_display_number(other) - 1) == 0) { continue; }
      uint16_t old_layer = synth_loop_active_layer[(uint8_t)page][other];
      if (old_layer == 0) { continue; }
      synth_loop_active_layer[(uint8_t)page][other] = 0;
      if (synth_deferred_note_on_layer[(uint8_t)page][other] == old_layer) {
        synth_deferred_note_on_layer[(uint8_t)page][other] = 0;
        {
          loop_events_guard_t guard;
          loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
            [old_layer](const loop_event_t& e) { return e.layer == old_layer; }), loop_events.end());
        }
        invalidate_loop_timeline_cache();
      } else {
        uint32_t off_pos = loop_length_fixed
          ? quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec) : raw_pos;
        release_synth_trigger(page, other);
        push_loop_event(page, other, loop_event_type_t::note_off, off_pos, old_layer);
      }
    }
  }
  uint16_t layer = loop_layer_seq++;
  uint8_t chord_flags = chord_root ? chord_flags_from_pressed() : 0;
  synth_loop_active_layer[(uint8_t)page][pad] = layer;
  if (defer_note_on && !missed_deferred_grid) {
    synth_deferred_note_on_layer[(uint8_t)page][pad] = layer;
  } else {
    trigger_loop_event({ page, (uint8_t)pad, loop_event_type_t::note_on, raw_pos, layer, chord_flags }, true);
    if (missed_deferred_grid && page != performance_page_t::drum) {
      synth_deferred_note_on_layer[(uint8_t)page][pad] = layer;
      synth_live_min_gate_until[(uint8_t)page][pad] = M5.millis() + loop_live_min_gate_ms;
      set_synth_live_release_pending((uint8_t)page, (uint8_t)pad, false);
      synth_live_release_layer[(uint8_t)page][pad] = 0;
    }
  }
  push_loop_event(page, (uint8_t)pad, loop_event_type_t::note_on, pos, layer, chord_flags);
  request_fn_draw(0);
}

static void loop_record_synth_pad_release(performance_page_t page, int pad)
{
  if (page == performance_page_t::sample || pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  uint16_t layer = synth_loop_active_layer[(uint8_t)page][pad];
  if (page == performance_page_t::drum) {
    // CH10 percussion is a Note On-only event. Keep the layer's Note On and
    // simply clear the live trigger bookkeeping on physical release.
    release_synth_trigger(page, (uint8_t)pad);
    synth_loop_active_layer[(uint8_t)page][pad] = 0;
    return;
  }
  if (layer != 0 && synth_deferred_note_on_layer[(uint8_t)page][pad] == layer) {
    synth_deferred_note_on_layer[(uint8_t)page][pad] = 0;
    auto note_on = std::find_if(loop_events.begin(), loop_events.end(), [layer](const loop_event_t& e) {
      return e.layer == layer && e.type == loop_event_type_t::note_on;
    });
    if (note_on != loop_events.end()) {
      uint32_t off_pos = loop_note_off_after_note_on(note_on->pos_ms, loop_length_msec);
      if (synth_live_min_gate_until[(uint8_t)page][pad] != 0) {
        const uint32_t raw_pos = loop_record_pos_ms(performance_event_time());
        const uint32_t candidate = quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec);
        const uint32_t duration = loop_forward_distance_ms(note_on->pos_ms, candidate, loop_length_msec);
        if (duration > 0 && duration < loop_length_msec / 2) { off_pos = candidate; }
      }
      push_loop_event(page, (uint8_t)pad, loop_event_type_t::note_off, off_pos, layer);
    }
    if (synth_live_min_gate_until[(uint8_t)page][pad] != 0) {
      if ((int32_t)(M5.millis() - synth_live_min_gate_until[(uint8_t)page][pad]) >= 0) {
        trigger_loop_event({ page, (uint8_t)pad, loop_event_type_t::note_off, 0, layer }, true);
        synth_live_min_gate_until[(uint8_t)page][pad] = 0;
      } else {
        set_synth_live_release_pending((uint8_t)page, (uint8_t)pad, true);
        synth_live_release_layer[(uint8_t)page][pad] = layer;
      }
    }
    // Keep the physical layer visible until its deferred release bookkeeping
    // is complete. The loop clock runs concurrently and uses this marker to
    // keep an older recorded chord from replacing a just-released live chord.
    synth_loop_active_layer[(uint8_t)page][pad] = 0;
    invalidate_loop_timeline_cache();
    return;
  }
  if (layer == 0) { release_synth_trigger(page, (uint8_t)pad); return; }
  uint32_t raw_pos = loop_record_pos_ms(performance_event_time());
  uint32_t pos = loop_length_fixed
    ? quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  if (loop_length_fixed) {
    pos = separate_overlapping_note_off(layer, pos, loop_length_msec);
  }
  // Do not clear synth_loop_active_layer before this immediate release. On
  // Pad-sourced chords the audio clock can otherwise start an older loop
  // chord between those two operations, so the physical Note Off releases
  // the wrong four voices and the held chord audibly stops late.
  trigger_loop_event({ page, (uint8_t)pad, loop_event_type_t::note_off, raw_pos, layer }, true);
  synth_loop_active_layer[(uint8_t)page][pad] = 0;
  push_loop_event(page, (uint8_t)pad, loop_event_type_t::note_off, pos, layer);
}

// Pad RepeatはFX Repeatとは別の演奏入力。Padの再トリガを量子化グリッドに
// 沿って生成し、LOOPモードでは生成済みのノートとしてそのまま記録する。
static uint32_t pad_repeat_interval_ms(void)
{
  uint32_t length = loop_length_fixed ? loop_length_msec : loop_default_length_ms;
  uint32_t step = loop_quantize_step_ms(length);
  return pad_repeat_mode == pad_repeat_mode_t::half_grid ? std::max<uint32_t>(1, step / 2) : step;
}

static uint8_t pad_repeat_interval_half_steps(void)
{
  return pad_repeat_mode == pad_repeat_mode_t::half_grid ? 1 : 2;
}

static void arm_pad_repeat_next(int pad, uint32_t now, bool preserve_phase = false)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  if (loop_grid_transport_active()) {
    const uint32_t pos = loop_pos_ms(now);
    if (!preserve_phase || !pad_repeat_transport_locked[pad]) {
      // Lever下はNote Grid、Lever上はその半分を最小スナップ単位にする。
      pad_repeat_phase_half_step[pad] = loop_nearest_grid_phase_half_step(
        pos, pad_repeat_interval_half_steps());
    }
    pad_repeat_transport_locked[pad] = true;
    pad_repeat_next_pos_ms[pad] = loop_next_phase_position_ms(
      pos, pad_repeat_phase_half_step[pad], pad_repeat_interval_half_steps());
    pad_repeat_next_msec[pad] = loop_transport_deadline_msec(now, pad_repeat_next_pos_ms[pad]);
  } else {
    pad_repeat_transport_locked[pad] = false;
    pad_repeat_next_msec[pad] = now + pad_repeat_interval_ms();
  }
}

static void trigger_pad_repeat(performance_page_t page, int pad, int chord_flags = -1)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  if (page != performance_page_t::sample) {
    if (page == performance_page_t::chord) { release_other_chord_roots((uint8_t)pad); }
    trigger_synth_pad(page, (uint8_t)pad, chord_flags, true);
    return;
  }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  play_sample_once(pad);
}

static void loop_record_pad_repeat(performance_page_t page, int pad, uint32_t now)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count
   || !performance_pad_has_sound(page, (uint8_t)pad)) { return; }
  if (!loop_playing) {
    if (!loop_length_fixed && loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = 0;
      loop_start_msec = now;
      loop_playing = true;
      loop_transport_started_visual();
      play_background_loop_at(0);
    }
  }
  uint32_t raw_pos = loop_record_pos_ms(now);
  // Lever Repeatは一打目を必ず即時発音する。未来のグリッドには
  // 記録しないため、同じ周回での重複発音も起こらない。
  uint32_t pos = loop_record_note_on_pos_ms(raw_pos, false, nullptr);
  uint16_t layer = loop_layer_seq++;
  uint8_t chord_flags = page == performance_page_t::chord ? chord_flags_from_pressed() : 0;
  push_loop_event(page, (uint8_t)pad, loop_event_type_t::note_on, pos, layer, chord_flags);
  pad_repeat_last_layer[pad] = layer;
  trigger_pad_repeat(page, pad, chord_flags);  // Leverを倒してから押したPadは即時発音する。
  request_fn_draw(0);
}

static void stop_pad_repeat(int pad, bool record_note_off)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  performance_page_t page = pad_repeat_page[pad];
  auto& slot = sampler_pool_t::slot[pad];
  bool needs_note_off = page == performance_page_t::sample
    ? (slot.isValid() && slot.hold_enabled) : true;
  if (record_note_off && current_mode == sampler_mode_t::mode_loop
   && needs_note_off && pad_repeat_last_layer[pad] != 0) {
    uint32_t now = M5.millis();
    uint32_t raw_pos = loop_record_pos_ms(now);
    uint32_t pos = loop_length_fixed ? quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec) : raw_pos;
    push_loop_event(page, (uint8_t)pad, loop_event_type_t::note_off, pos, pad_repeat_last_layer[pad]);
  }
  if (page == performance_page_t::sample) {
    if (slot.isValid() && (slot.hold_enabled || slot.loop_enabled)) { sampler_audio_t::stop(pad); }
  } else {
    release_synth_trigger(page, (uint8_t)pad);
  }
  pad_repeat_next_msec[pad] = 0;
  pad_repeat_active_mask &= ~(uint16_t)(1u << pad);
  pad_repeat_transport_locked[pad] = false;
  pad_repeat_next_pos_ms[pad] = 0;
  pad_repeat_phase_half_step[pad] = 0;
  pad_repeat_last_layer[pad] = 0;
}

static void trigger_pad_repeat_pulse(int pad, uint32_t now)
{
  performance_page_t page = (pad_repeat_next_msec[pad] || pad_repeat_transport_locked[pad])
    ? pad_repeat_page[pad] : current_page;
  pad_repeat_page[pad] = page;
  if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_pad_repeat(page, pad, now);
  } else {
    trigger_pad_repeat(page, pad);
  }
  mark_sound_priority();
  request_pad_state_draw(pad);
}

static void set_pad_repeat_mode(pad_repeat_mode_t mode)
{
  if (mode == pad_repeat_mode) {
    if (mode != pad_repeat_mode_t::none) { pad_repeat_release_confirm_msec = 0; }
    return;
  }
  if (mode == pad_repeat_mode_t::none) {
    for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
      if (pad_repeat_next_msec[pad]) { stop_pad_repeat(pad, true); }
    }
    pad_repeat_mode = mode;
    pad_repeat_lever_mask = 0;
    pad_repeat_release_confirm_msec = 0;
    return;
  }
  pad_repeat_mode = mode;
  pad_repeat_release_confirm_msec = 0;
  uint32_t now = M5.millis();
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    if (!pads[pad].pressed || !performance_pad_has_sound(current_page, (uint8_t)pad)) { continue; }
    pad_repeat_page[pad] = current_page;
    arm_pad_repeat_next(pad, now);
    pad_repeat_active_mask |= (uint16_t)(1u << pad);
  }
}

static void service_pad_repeat(uint32_t now)
{
  if (pad_repeat_mode == pad_repeat_mode_t::none) { return; }
  if (pad_repeat_release_confirm_msec != 0
   && (int32_t)(now - pad_repeat_release_confirm_msec) >= 0) {
    // Confirm against the latest full button state, rather than trusting the
    // first release edge emitted by the expander.
    if ((prev_bitmask & pad_repeat_lever_mask) == 0) {
      set_pad_repeat_mode(pad_repeat_mode_t::none);
      return;
    }
    pad_repeat_release_confirm_msec = 0;
  }
  uint32_t interval = pad_repeat_interval_ms();
  uint16_t active = pad_repeat_active_mask;
  while (active) {
    const int pad = __builtin_ctz(active);
    active &= active - 1;
    if ((!pad_repeat_next_msec[pad] && !pad_repeat_transport_locked[pad])
     || !pads[pad].pressed || !performance_pad_has_sound(pad_repeat_page[pad], (uint8_t)pad)) { continue; }
    if (pad_repeat_transport_locked[pad]) {
      if (!loop_grid_transport_active()) {
        arm_pad_repeat_next(pad, now);
        continue;
      }
      if ((int32_t)(now - pad_repeat_next_msec[pad]) < 0) { continue; }
      trigger_pad_repeat_pulse(pad, now);
      pad_repeat_next_pos_ms[pad] = loop_next_phase_position_ms(
        pad_repeat_next_pos_ms[pad], pad_repeat_phase_half_step[pad], pad_repeat_interval_half_steps());
      pad_repeat_next_msec[pad] = loop_transport_deadline_msec(now, pad_repeat_next_pos_ms[pad]);
      continue;
    }
    if ((int32_t)(now - pad_repeat_next_msec[pad]) < 0) { continue; }
    trigger_pad_repeat_pulse(pad, now);
    // 描画やSDアクセスの遅れで追いつけない時も、連打を一気に発生させない。
    pad_repeat_next_msec[pad] = now + interval;
  }
}

static uint32_t sample_render_frames(const sample_slot_t& slot, uint32_t out_rate)
{
  if (!slot.isValid() || slot.playFrames() == 0 || slot.sample_rate == 0 || slot.pitch_q8 == 0) { return 0; }
  uint64_t frames = (uint64_t)slot.playFrames() * out_rate * 256u;
  frames /= (uint64_t)slot.sample_rate * slot.pitch_q8;
  if (frames > out_rate * sampler_pool_t::max_sample_sec) { frames = out_rate * sampler_pool_t::max_sample_sec; }
  return frames < 1 ? 1 : (uint32_t)frames;
}

static int32_t sample_render_at(const sample_slot_t& slot, uint32_t out_index, uint32_t out_rate)
{
  uint32_t play_frames = slot.playFrames();
  if (!slot.isValid() || play_frames == 0 || out_rate == 0) { return 0; }
  uint64_t pos_fp = (((uint64_t)out_index * slot.sample_rate * slot.pitch_q8) << 16)
                  / ((uint64_t)out_rate << 8);
  uint32_t idx = (uint32_t)(pos_fp >> 16);
  if (idx >= play_frames) { return 0; }
  uint32_t frac = (uint32_t)pos_fp & 0xFFFFu;
  uint32_t base = slot.playStart();
  uint32_t sample_idx = slot.reverse ? (slot.playEnd() - 1 - idx) : (base + idx);
  int32_t s = slot.pcm[sample_idx];
  if (frac != 0 && idx + 1 < play_frames) {
    uint32_t sample_idx1 = slot.reverse ? (slot.playEnd() - 2 - idx) : (base + idx + 1);
    int32_t s1 = slot.pcm[sample_idx1];
    s += ((s1 - s) * (int32_t)frac) >> 16;
  }
  if (slot.volume_q8 != 256) { s = (int32_t)(((int64_t)s * slot.volume_q8) >> 8); }
  return s;
}

static void move_loop_events_pad(uint8_t from, uint8_t to)
{
  {
    loop_events_guard_t guard;
    for (auto& e : loop_events) {
      if (e.page == performance_page_t::sample && e.pad == from) { e.pad = to; }
    }
  }
  loop_mute(performance_page_t::sample, to) = loop_mute(performance_page_t::sample, to)
                                                || loop_mute(performance_page_t::sample, from);
  loop_mute(performance_page_t::sample, from) = false;
  loop_active_layer[to] = loop_active_layer[from] ? loop_active_layer[from] : loop_active_layer[to];
  loop_deferred_note_on_layer[to] = loop_deferred_note_on_layer[from] ? loop_deferred_note_on_layer[from] : loop_deferred_note_on_layer[to];
  loop_deferred_live_pad[to] = loop_deferred_live_pad[to] || loop_deferred_live_pad[from];
  loop_deferred_live_pos_ms[to] = loop_deferred_live_pos_ms[from] ? loop_deferred_live_pos_ms[from] : loop_deferred_live_pos_ms[to];
  loop_active_layer[from] = 0;
  loop_deferred_note_on_layer[from] = 0;
  loop_deferred_live_pad[from] = false;
  loop_deferred_live_pos_ms[from] = 0;
  invalidate_loop_timeline_cache();
}

static bool sample_move_to_empty(uint8_t from, uint8_t to)
{
  if (from == to || !sampler_pool_t::slot[from].isValid() || sampler_pool_t::slot[to].isValid()) { return false; }
  sampler_audio_t::stop(from);
  sampler_audio_t::stop(to);
  sampler_pool_t::slot[to] = sampler_pool_t::slot[from];
  sampler_pool_t::slot[from] = sample_slot_t{};
  move_loop_events_pad(from, to);
  if (rec_wave_pad == from) { rec_wave_pad = to; }
  return true;
}

static bool sample_mix_to_pad(uint8_t from, uint8_t to)
{
  if (from == to || !sampler_pool_t::slot[from].isValid() || !sampler_pool_t::slot[to].isValid()) { return false; }
  auto& src = sampler_pool_t::slot[from];
  auto& dst = sampler_pool_t::slot[to];
  uint32_t out_rate = std::max<uint32_t>(src.sample_rate, dst.sample_rate);
  if (out_rate == 0 || out_rate > 48000) { out_rate = sampler_audio_t::sample_rate; }
  uint32_t out_frames = std::max(sample_render_frames(src, out_rate), sample_render_frames(dst, out_rate));
  if (out_frames < 16) { return false; }
  size_t out_bytes = (size_t)out_frames * sizeof(int16_t);
  if (sampler_pool_t::freeBytes() + src.bytes() + dst.bytes() < out_bytes) { return false; }
#if defined (M5UNIFIED_PC_BUILD)
  int16_t* mixed = (int16_t*)malloc(out_bytes);
#else
  int16_t* mixed = (int16_t*)heap_caps_malloc(out_bytes, MALLOC_CAP_SPIRAM);
#endif
  if (!mixed) { return false; }

  recording_processing_static_drawn = false;
  recording_processing_frame = 0;
  draw_recording_processing_frame("ANALYZING");
  const uint32_t visual_step = std::max<uint32_t>(1, out_frames / 3);
  uint32_t next_visual = visual_step;
  int32_t peak = 1;
  for (uint32_t i = 0; i < out_frames; ++i) {
    int32_t v = sample_render_at(dst, i, out_rate) + sample_render_at(src, i, out_rate);
    int32_t av = v < 0 ? -v : v;
    if (peak < av) { peak = av; }
    if (i >= next_visual) {
      draw_recording_processing_frame("ANALYZING");
      next_visual += visual_step;
    }
  }
  static constexpr int32_t target_peak = (INT16_MAX * 90) / 100;
  uint32_t gain_q15 = peak > target_peak ? (uint32_t)(((int64_t)target_peak << 15) / peak) : 32768u;
  draw_recording_processing_frame("MIXING");
  next_visual = visual_step;
  for (uint32_t i = 0; i < out_frames; ++i) {
    int32_t v = sample_render_at(dst, i, out_rate) + sample_render_at(src, i, out_rate);
    v = (int32_t)(((int64_t)v * gain_q15) >> 15);
    if (v > INT16_MAX) { v = INT16_MAX; }
    if (v < INT16_MIN) { v = INT16_MIN; }
    mixed[i] = (int16_t)v;
    if (i >= next_visual) {
      draw_recording_processing_frame("MIXING");
      next_visual += visual_step;
    }
  }

  bool dst_hold = dst.hold_enabled;
  bool dst_loop = dst.loop_enabled;
  bool dst_loop_whole = dst.loop_whole_sample;
  uint8_t dst_loop_grid = dst.loop_grid_half_steps;
  sample_sustain_mode_t dst_sustain_mode = dst.synth_sustain_mode;
  uint16_t dst_release_ms = dst.synth_release_ms;
  sampler_audio_t::stop(from);
  sampler_audio_t::stop(to);
  sampler_pool_t::erase(from);
  bool ok = sampler_pool_t::loadPcmOwned(to, "MIX", mixed, out_frames, out_rate);
  if (!ok) {
    free(mixed);
    return false;
  }
  sampler_pool_t::slot[to].hold_enabled = dst_hold;
  sampler_pool_t::slot[to].loop_enabled = dst_loop;
  sampler_pool_t::slot[to].loop_whole_sample = dst_loop_whole;
  sampler_pool_t::slot[to].loop_grid_half_steps = dst_loop_grid;
  sampler_pool_t::slot[to].synth_sustain_mode =
    dst_sustain_mode == sample_sustain_mode_t::manual
      ? sample_sustain_mode_t::automatic : dst_sustain_mode;
  sampler_pool_t::slot[to].synth_release_ms = dst_release_ms;
  move_loop_events_pad(from, to);
  rec_wave_pad = to;
  draw_recording_processing_frame("SAVING");
  if (save_session_pad(to)) { save_resume_kit(); }
  return true;
}

static void cancel_sample_move(void)
{
  int src = sample_move_source_pad;
  int copy_src = sample_move_copy_source_pad;
  int copy_target = sample_move_copy_target_pad;
  sample_move_source_pad = -1;
  sample_move_copy_source_pad = -1;
  sample_move_copy_target_pad = -1;
  sample_edit_armed_pad = -1;
  sample_edit_pending_pad = -1;
  cancel_hold_progress(hold_progress_kind_t::sample_move);
  if (src < 0 && copy_src < 0) { return; }
  request_wave_draw();
  if (src >= 0) { request_pad_state_draw(src); }
  if (copy_src >= 0) { request_pad_state_draw(copy_src); }
  if (copy_target >= 0) { request_pad_state_draw(copy_target); }
}

static void cancel_sample_delete_confirm(void)
{
  if (sample_delete_confirm_pad < 0) { return; }
  const int pad = sample_delete_confirm_pad;
  sample_delete_confirm_pad = -1;
  sample_delete_confirm_until_msec = 0;
  request_pad_state_draw(pad);
  request_wave_draw();
}

static void request_sample_delete(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  const uint32_t now = performance_event_time();
  if (!sampler_pool_t::slot[pad].isValid()) {
    cancel_sample_delete_confirm();
    show_status_message("EMPTY PAD", 900, false);
    return;
  }
  if (sample_delete_confirm_pad == pad
   && (int32_t)(sample_delete_confirm_until_msec - now) > 0) {
    sample_delete_confirm_pad = -1;
    sample_delete_confirm_until_msec = 0;
    clear_pad_sample((uint8_t)pad, true);
    repair_pitched_pad_sources();
    apply_synth_tones(true);
    save_resume_kit();
    show_status_message("SAMPLE DELETED", 1100, false);
    request_all_fn_draw();
    return;
  }
  sample_delete_confirm_pad = pad;
  sample_delete_confirm_until_msec = now + 2800;
  request_pad_state_draw(pad);
  request_wave_draw();
}

static void service_sample_delete_confirm(uint32_t now)
{
  if (sample_delete_confirm_pad < 0) { return; }
  if (!fn_pressed[2] || current_mode != sampler_mode_t::mode_rec
   || current_page != performance_page_t::sample
   || (int32_t)(now - sample_delete_confirm_until_msec) >= 0) {
    cancel_sample_delete_confirm();
  }
}

static bool copy_moved_sample_back(void)
{
  const int source = sample_move_copy_source_pad;
  const int target = sample_move_copy_target_pad;
  if (source < 0 || target < 0 || source >= (int)def::pad::pad_count
   || target >= (int)def::pad::pad_count || !pads[source].pressed
   || sampler_pool_t::slot[source].isValid() || !sampler_pool_t::slot[target].isValid()) {
    return false;
  }
  if (!sampler_pool_t::clone((uint8_t)source, (uint8_t)target)) {
    show_status_message("Not enough memory", 1400, true);
    return false;
  }
  sample_move_copy_source_pad = -1;
  sample_move_copy_target_pad = -1;
  show_status_message("COPIED", 900, false);
  request_header_draw();
  request_wave_draw();
  request_pad_draw(source);
  request_pad_draw(target);
  return true;
}

static bool execute_sample_move_or_mix(int target)
{
  int src = sample_move_source_pad;
  if (src < 0 || target < 0 || target >= (int)def::pad::pad_count || target == src) { return false; }
  const bool target_was_empty = !sampler_pool_t::slot[target].isValid();
  bool ok = target_was_empty ? sample_move_to_empty((uint8_t)src, (uint8_t)target)
                             : sample_mix_to_pad((uint8_t)src, (uint8_t)target);
  if (processing_screen_visible) {
    // ミックスは全画面の待機表示を使うため、完了・失敗どちらでも通常UIを
    // 一度だけ完全復帰する。dirty領域だけではヘッダーが残るためここで戻す。
    draw_all();
    update_all_leds();
    processing_screen_visible = false;
  }
  if (ok) {
    sample_move_source_pad = -1;
    if (target_was_empty && pads[src].pressed) {
      sample_move_copy_source_pad = src;
      sample_move_copy_target_pad = target;
      show_status_message("TAP TARGET AGAIN: COPY", 1800, false);
    }
    request_header_draw();
    request_wave_draw();
    request_pad_draw(src);
    request_pad_draw(target);
  } else {
    show_status_message("Move failed", 1200, true);
  }
  return ok;
}

static void service_sample_move_hold(uint32_t now)
{
  if (sample_move_source_pad >= 0
   || current_mode != sampler_mode_t::mode_rec
   || edit_pad >= 0
   || recording_pad >= 0
   || fn_pressed[0] || fn_pressed[1] || fn_pressed[2]) {
    return;
  }
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    if (sample_edit_pending_pad != pad || !pads[pad].pressed
     || !sampler_pool_t::slot[pad].isValid()) { continue; }
    if (now - pads[pad].press_msec >= sample_move_hold_ms) {
      sample_edit_pending_pad = -1;
      cancel_hold_progress(hold_progress_kind_t::sample_move);
      sample_move_source_pad = pad;
      sampler_audio_t::stop(pad);
      request_wave_draw();
      request_pad_state_draw(pad);
      return;
    }
  }
}

static bool sample_add_available(void)
{
  return !menu_visible && !loop_playing && edit_pad < 0 && recording_pad < 0
      && current_page == performance_page_t::sample
      && (current_mode == sampler_mode_t::mode_rec || current_mode == sampler_mode_t::mode_play);
}

static bool sample_add_armed_active(uint32_t now)
{
  return sample_add_armed_pad >= 0
      && (int32_t)(sample_add_armed_until_msec - now) >= 0;
}

static void cancel_sample_add(void)
{
  const int candidate = sample_add_candidate_pad;
  const int armed = sample_add_armed_pad;
  const int action = sample_add_action_pad;
  sample_add_candidate_pad = -1;
  sample_add_armed_pad = -1;
  sample_add_armed_until_msec = 0;
  sample_add_action_pad = -1;
  cancel_hold_progress(hold_progress_kind_t::sample_add);
  if (candidate >= 0) { request_pad_state_draw(candidate); }
  if (armed >= 0) { request_pad_state_draw(armed); }
  if (action >= 0 && action != armed) { request_pad_state_draw(action); }
}

static void begin_sample_shortcut_import(uint8_t pad)
{
  cancel_sample_add();
  menu_open();
  if (!begin_kit_assign_wav(pad)) {
    // The shared Import view already displays the reason (for example No SD).
    // Returning straight to the performance page would hide that useful error.
    kit_shortcut_target_pad = -1;
  }
}

static void service_sample_add_hold(uint32_t now)
{
  if (!sample_add_available()) {
    cancel_sample_add();
    return;
  }
  if (sample_add_armed_pad >= 0 && !sample_add_armed_active(now)) {
    const int pad = sample_add_armed_pad;
    sample_add_armed_pad = -1;
    sample_add_armed_until_msec = 0;
    request_pad_state_draw(pad);
    request_wave_draw();
  }
  if (sample_add_candidate_pad >= 0) {
    const int pad = sample_add_candidate_pad;
    if (!pads[pad].pressed || sampler_pool_t::slot[pad].isValid()) {
      sample_add_candidate_pad = -1;
    } else if (now - pads[pad].press_msec >= sample_add_arm_hold_ms) {
      sample_add_candidate_pad = -1;
      sample_add_armed_pad = pad;
      sample_add_armed_until_msec = now + sample_add_armed_timeout_ms;
      cancel_hold_progress(hold_progress_kind_t::sample_add);
      show_status_message("TAP: IMPORT / HOLD: RECORD", sample_add_armed_timeout_ms, false);
      request_pad_state_draw(pad);
      request_wave_draw();
    }
  }
  if (sample_add_action_pad >= 0) {
    const int pad = sample_add_action_pad;
    if (!pads[pad].pressed || sampler_pool_t::slot[pad].isValid()) {
      sample_add_action_pad = -1;
      cancel_hold_progress(hold_progress_kind_t::sample_add);
      request_pad_state_draw(pad);
      return;
    }
    if (now - pads[pad].press_msec >= sample_add_action_hold_ms) {
      sample_add_action_pad = -1;
      sample_add_armed_pad = -1;
      sample_add_armed_until_msec = 0;
      clear_status_message(false);
      cancel_hold_progress(hold_progress_kind_t::sample_add);
      start_pad_recording(pad);
      request_pad_state_draw(pad);
    }
  }
}

static void service_fn_modifier_hint(uint32_t now)
{
  bool visible = false;
  if (edit_pad < 0
   && (current_mode == sampler_mode_t::mode_rec
    || current_mode == sampler_mode_t::mode_play)
   && sample_move_source_pad < 0
   && sample_move_copy_source_pad < 0
   && sample_edit_pending_pad < 0) {
    for (const auto& pad : pads) {
      if (pad.pressed && now - pad.press_msec >= fn_modifier_hint_hold_ms) {
        visible = true;
        break;
      }
    }
  }
  if (fn_modifier_hint_visible == visible) { return; }
  fn_modifier_hint_visible = visible;
  request_all_fn_draw();
}

static bool common_fn_mode(void)
{
  return current_mode == sampler_mode_t::mode_rec
      || current_mode == sampler_mode_t::mode_play
      || current_mode == sampler_mode_t::mode_fx;
}

static void toggle_current_page_mute(void)
{
  const mixer_part_t part = mixer_part_for_page(current_page);
  const bool muted = !mixer_part_muted[(uint8_t)part];
  set_mixer_part_muted(part, muted);
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) { request_pad_draw(pad); }
  request_wave_draw();
}

static bool toggle_current_pad_mute(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return false; }
  if (current_page != performance_page_t::sample
   && current_page != performance_page_t::drum) {
    return false;
  }
  bool& muted = loop_mute(current_page, (uint8_t)pad);
  muted = !muted;
  if (muted) {
    if (current_page == performance_page_t::sample) { stop_sample_grid_loop(pad); }
    else { release_synth_trigger(current_page, (uint8_t)pad); }
  }
  request_pad_draw(pad);
  request_wave_draw();
  return true;
}

static void performance_pad_press(int pad)
{
  if (fn_pressed[1] && (common_fn_mode() || current_mode == sampler_mode_t::mode_loop)
   && toggle_current_pad_mute(pad)) {
    return;
  }
  // Chord type pads only define the next root's voicing. They must not start
  // transport, make sound, or leave their own loop events behind.
  if (current_page == performance_page_t::chord) {
    int8_t modifier = chord_modifier_for_order(pad_display_number((uint8_t)pad) - 1);
    if (modifier >= 0) {
      chord_modifier_pressed[modifier] = true;
      if (modifier == 0) { request_chord_label_draw(); }
      request_pad_state_draw(pad);
      return;
    }
  }
  if (pad_repeat_mode != pad_repeat_mode_t::none
   && performance_pad_has_sound(current_page, (uint8_t)pad)) {
    const uint32_t now = M5.millis();
    pad_repeat_page[pad] = current_page;
    trigger_pad_repeat_pulse(pad, now);
    arm_pad_repeat_next(pad, now);
    return;
  }
  if (current_mode == sampler_mode_t::mode_loop && fn_pressed[2]) {
    loop_del_touched_pad = true;
    cancel_hold_progress(hold_progress_kind_t::loop_clear);
    loop_remove_page_pad_events(current_page, pad);
    loop_reset_recording_state_if_empty();
    request_wave_draw();
    request_all_fn_draw();
    request_pad_draw(pad);
    return;
  }
  if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_synth_pad(current_page, pad);
  } else {
    if (current_page == performance_page_t::chord) {
      release_other_chord_roots((uint8_t)pad);
    }
    const uint8_t chord_flags = current_page == performance_page_t::chord
      ? chord_flags_from_pressed() : 0;
    if (!defer_live_synth_if_early(current_page, pad, chord_flags)) {
      trigger_synth_pad(current_page, (uint8_t)pad,
                        current_page == performance_page_t::chord ? chord_flags : -1, true);
    }
  }
  request_pad_state_draw(pad);
}

static void performance_pad_release(int pad)
{
  if (current_page == performance_page_t::chord
   && chord_modifier_for_order(pad_display_number((uint8_t)pad) - 1) >= 0) {
    int8_t modifier = chord_modifier_for_order(pad_display_number((uint8_t)pad) - 1);
    chord_modifier_pressed[modifier] = false;
    if (modifier == 0) { request_chord_label_draw(); }
  } else if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_synth_pad_release(current_page, pad);
  } else {
    if (!release_deferred_live_synth(current_page, pad)) {
      release_synth_trigger(current_page, (uint8_t)pad);
    }
  }
  request_pad_state_draw(pad);
}

static void pad_press(int pad) {
  if (pads[pad].pressed) { return; }
  pads[pad].pressed = true;
  pads[pad].press_msec = performance_event_time();
  mark_sound_priority();
  if (edit_pad >= 0) {
    handle_edit_function_pad(pad);
    // EDIT Pad has persistent focus and enabled states. A frame-only update
    // leaves the old label/background behind, so redraw this small grid as a
    // coherent control surface after every selection.
    request_grid_draw();
    return;
  }
  if (current_mode == sampler_mode_t::mode_rec
   && current_page == performance_page_t::sample && fn_pressed[2]) {
    request_sample_delete(pad);
    return;
  }
  if (fn_pressed[1] && (common_fn_mode() || current_mode == sampler_mode_t::mode_loop)
   && toggle_current_pad_mute(pad)) {
    return;
  }
  if (current_mode == sampler_mode_t::mode_fx) {
    if (mixer_active) { mixer_pad_press(pad); }
    else { fx_pad_press(pad); }
    return;
  }
  if (current_page != performance_page_t::sample) {
    performance_pad_press(pad);
    return;
  }
  if (sample_move_copy_source_pad >= 0) {
    if (pad == sample_move_copy_target_pad) {
      copy_moved_sample_back();
      return;
    }
    // コピー待ち中に別Padへ進んだ場合は、通常演奏へ自然に戻す。
    cancel_sample_move();
  }
  if (sample_move_source_pad >= 0) {
    if (pad != sample_move_source_pad) { execute_sample_move_or_mix(pad); }
    request_pad_draw(pad);
    return;
  }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() && sample_add_available()) {
    const uint32_t now = performance_event_time();
    if (sample_add_armed_active(now) && sample_add_armed_pad == pad) {
      sample_add_action_pad = pad;
      clear_status_message(false);
      begin_hold_progress(hold_progress_kind_t::sample_add, pads[pad].press_msec,
                          sample_add_action_hold_ms, 0x5098F0u,
                          "HOLD TO RECORD", "RECORDING");
    } else {
      cancel_sample_add();
      sample_add_candidate_pad = pad;
      begin_hold_progress(hold_progress_kind_t::sample_add, pads[pad].press_msec,
                          sample_add_arm_hold_ms, 0x5098F0u,
                          "HOLD TO ADD", "READY");
    }
    request_pad_state_draw(pad);
    return;
  }
  if (current_mode == sampler_mode_t::mode_play && slot.isValid()) {
    play_focus_pad = pad;
  }
  bool pad_content_changed = false;

  if (current_mode == sampler_mode_t::mode_loop && fn_pressed[2]) {
    loop_del_touched_pad = true;
    cancel_hold_progress(hold_progress_kind_t::loop_clear);
    loop_remove_pad_events(pad);
    loop_reset_recording_state_if_empty();
    pad_content_changed = true;
    request_wave_draw();
    request_all_fn_draw();
  } else if (current_mode == sampler_mode_t::mode_rec && slot.isValid()) {
    rec_wave_pad = pad;
    const bool edit_confirmed = sample_edit_armed_pad == pad;
    if (!edit_confirmed) {
      sample_edit_armed_pad = pad;
      sample_edit_pending_pad = -1;
      // Sample page uses the first tap as an audition when stopped.  During
      // loop playback it is deliberately silent so the page remains an edit
      // surface and cannot add an accidental extra hit to the loop.
      if (!loop_playing) { play_sample_once(pad); }
      request_wave_draw();
    } else {
      // The second tap confirms EDIT only. Do not start another preview just
      // before the screen transitions, especially for Hold/Sustain samples.
      sample_edit_armed_pad = -1;
      sample_edit_pending_pad = pad;
      begin_hold_progress(hold_progress_kind_t::sample_move, pads[pad].press_msec,
                          sample_move_hold_ms, 0xFFB050u,
                          "HOLD TO MOVE", "SELECT TARGET");
    }
  } else if (pad_repeat_mode != pad_repeat_mode_t::none && slot.isValid()) {
    // Leverを先に倒してからPadを押した場合は、量子化待ちせずここで一発目を鳴らす。
    const uint32_t now = performance_event_time();
    trigger_pad_repeat_pulse(pad, now);
    // 一発目は即時でも、二発目からはBGM/Loopの絶対グリッドへスナップする。
    arm_pad_repeat_next(pad, now);
  } else if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_pad(pad);
  } else if (current_mode == sampler_mode_t::mode_rec && !slot.isValid()) {
    start_pad_recording(pad);
  } else {
    if (!trigger_anchor_synced_pad(pad) && !defer_live_pad_if_early(pad)) { trigger_pad(pad); }
  }
  if (pad_content_changed) { request_pad_draw(pad); }
  else { request_pad_state_draw(pad); }
}

static void pad_release(int pad) {
  if (!pads[pad].pressed) { return; }
  pads[pad].pressed = false;
  mark_sound_priority(60);
  if (edit_pad >= 0) {
    // A filled Sample Pad enters EDIT on its press edge. Its matching release
    // still belongs to that original performance gesture, so a Hold/Sustain
    // preview must receive Note Off instead of being left latched forever.
    if (pad == edit_pad && sampler_pool_t::slot[edit_pad].hold_enabled) {
      preview_edit_transport(false);
    }
    request_grid_draw();
    return;
  }
  if (current_mode == sampler_mode_t::mode_fx) {
    if (mixer_active) { mixer_pad_release(pad); }
    else { fx_pad_release(pad); }
    // FX controls change both their frame and their filled role surface.
    // Do not let the normal Pad-frame path overwrite that palette on release.
    request_pad_draw(pad);
    return;
  }
  if (pad_repeat_next_msec[pad] || pad_repeat_transport_locked[pad]) {
    stop_pad_repeat(pad, true);
    request_pad_state_draw(pad);
    return;
  }
  if (current_page != performance_page_t::sample) {
    performance_pad_release(pad);
    return;
  }
  if (sample_move_source_pad == pad) {
    cancel_sample_move();
    request_pad_state_draw(pad);
    return;
  }
  if (sample_move_copy_source_pad == pad) {
    // 元Padを離したら通常の移動として確定。コピー待ちだけを終了する。
    cancel_sample_move();
    request_pad_state_draw(pad);
    return;
  }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() && sample_add_available()) {
    if (sample_add_candidate_pad == pad) {
      sample_add_candidate_pad = -1;
      cancel_hold_progress(hold_progress_kind_t::sample_add);
      request_pad_state_draw(pad);
      return;
    }
    if (sample_add_action_pad == pad) {
      sample_add_action_pad = -1;
      cancel_hold_progress(hold_progress_kind_t::sample_add);
      if (sample_add_armed_active(performance_event_time()) && sample_add_armed_pad == pad) {
        begin_sample_shortcut_import((uint8_t)pad);
      } else {
        request_pad_state_draw(pad);
      }
      return;
    }
    // Releasing the first long press only leaves the Pad armed. A second
    // deliberate gesture chooses Import or Record, so ordinary missed taps
    // can never open storage or the microphone.
    if (sample_add_armed_pad == pad) {
      request_pad_state_draw(pad);
      return;
    }
    return;
  }
  if (sample_edit_pending_pad == pad) {
    sample_edit_pending_pad = -1;
    cancel_hold_progress(hold_progress_kind_t::sample_move);
    enter_edit(pad);
    // The sound began on the press edge. For Hold samples, this release is
    // their Note Off even though the UI has just transitioned into EDIT.
    if (sampler_pool_t::slot[pad].hold_enabled) {
      preview_edit_transport(false);
    }
    request_grid_draw();
    return;
  }
  if (recording_pad == pad) {
    finish_pad_recording();
    return;
  }
  if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_pad_release(pad);
  }
  if (loop_deferred_live_pad[pad] && slot.isValid() && slot.hold_enabled) {
    loop_deferred_live_pad[pad] = false;
  }
  if (edit_pad < 0 && slot.isValid() && slot.hold_enabled) {
    uint32_t sustain_start = 0;
    uint32_t sustain_end = 0;
    uint16_t sustain_crossfade = 0;
    if (synth_sustain_parameters(slot, slot.playStart(),
                                 &sustain_start, &sustain_end, &sustain_crossfade)) {
      sampler_audio_t::release((uint8_t)pad);
      stop_sample_grid_loop(pad, false);
    } else {
      stop_sample_grid_loop(pad);
    }
  }
  request_pad_state_draw(pad);
}

static void rebase_loop_transport(uint32_t now, uint32_t position_ms)
{
  if (!loop_playing) { return; }
  uint32_t ratio_q8 = loop_speed_ratio_q8();
  loop_start_msec = now - (uint32_t)(((uint64_t)position_ms << 8) / ratio_q8);
  // loop_prev_pos_ms belongs to the 1ms loop clock. Updating it from the UI
  // task turns a one-millisecond rounding correction into a false wrap.
}

static mixer_part_t mixer_part_for_page(performance_page_t page)
{
  switch (page) {
  case performance_page_t::drum:   return mixer_part_t::drum;
  case performance_page_t::sample: return mixer_part_t::sampler;
  case performance_page_t::bass:   return mixer_part_t::bass;
  case performance_page_t::melody: return mixer_part_t::melody;
  case performance_page_t::chord:  return mixer_part_t::chord;
  default:                          return mixer_part_t::sampler;
  }
}

static uint16_t mixer_scaled_volume_q8(mixer_part_t part, uint16_t base_q8)
{
  const uint8_t index = (uint8_t)part;
  if (index >= mixer_part_count || mixer_part_muted[index]) { return 0; }
  return (uint16_t)std::min<uint32_t>(1024,
    ((uint32_t)base_q8 * mixer_part_volume[index] + 50) / 100);
}

static void apply_synth_page_volume(performance_page_t page, bool force)
{
  auto& midi = kp::system_registry->midi_out_control;
  auto midi_volume = [](uint8_t percent) -> uint8_t {
    return (uint8_t)std::min<uint32_t>(127, ((uint32_t)percent * 127 + 50) / 100);
  };
  auto set_volume = [&midi, force](uint8_t channel, uint8_t value) {
    midi.setChannelVolume(channel, value);
    if (force) { send_sam_midi(0xB0 | channel, 7, value); }
  };

  if (page == performance_page_t::drum) {
    uint8_t volume = (uint8_t)((drum_volume
      * mixer_part_volume[(uint8_t)mixer_part_t::drum] + 50) / 100);
    if (mixer_part_muted[(uint8_t)mixer_part_t::drum]) { volume = 0; }
    set_volume(kp::def::midi::channel_10, midi_volume(volume));
    return;
  }

  const auto& settings = page_settings(page);
  if (settings.source != synth_tone_source_t::general_midi) { return; }
  const mixer_part_t part = page == performance_page_t::melody ? mixer_part_t::melody
                          : page == performance_page_t::bass ? mixer_part_t::bass
                          : mixer_part_t::chord;
  const uint8_t channel = page == performance_page_t::melody ? kp::def::midi::channel_1
                        : page == performance_page_t::bass ? kp::def::midi::channel_3
                        : kp::def::midi::channel_2;
  uint8_t volume = (uint8_t)((settings.volume * mixer_part_volume[(uint8_t)part] + 50) / 100);
  if (mixer_part_muted[(uint8_t)part]) { volume = 0; }
  set_volume(channel, midi_volume(volume));
}

static uint8_t synth_sustain_cache_slot(performance_page_t page)
{
  switch (page) {
  case performance_page_t::melody: return 0;
  case performance_page_t::chord:  return 1;
  case performance_page_t::bass:   return 2;
  default:                         return 0xFF;
  }
}

static void prime_synth_sustain_cache(performance_page_t page)
{
  const uint8_t cache_slot = synth_sustain_cache_slot(page);
  if (cache_slot == 0xFF) { return; }
  const auto& settings = page_settings(page);
  if (settings.source != synth_tone_source_t::pad) {
    sampler_audio_t::clearSynthSustainCache(cache_slot);
    return;
  }
  const uint8_t source_pad = resolved_pad_sound(settings);
  if (source_pad >= def::pad::pad_count) {
    sampler_audio_t::clearSynthSustainCache(cache_slot);
    return;
  }
  const auto& slot = sampler_pool_t::slot[source_pad];
  if (!slot.isValid() || slot.playFrames() == 0) {
    sampler_audio_t::clearSynthSustainCache(cache_slot);
    return;
  }
  const uint32_t source_start = slot.playStart();
  uint32_t sustain_start = 0;
  uint32_t sustain_end = 0;
  uint16_t sustain_crossfade = 0;
  if (!synth_sustain_parameters(slot, source_start,
                                &sustain_start, &sustain_end, &sustain_crossfade)) {
    sampler_audio_t::clearSynthSustainCache(cache_slot);
    return;
  }
  sampler_audio_t::primeSynthSustainCache(cache_slot, slot.pcm + source_start,
                                           sustain_start, sustain_end);
}

static void apply_synth_tones(bool force)
{
  auto& midi = kp::system_registry->midi_out_control;
  auto set_program = [&midi, force](uint8_t channel, uint8_t value) {
    midi.setProgramChange(channel, value);
    if (force) { send_sam_midi(0xC0 | channel, value); }
  };
  // Program Change is only sent when an actual sound source/tone changes.
  // Volume changes use apply_synth_page_volume(), keeping looped notes alive.
  apply_synth_page_volume(performance_page_t::melody, force);
  apply_synth_page_volume(performance_page_t::bass, force);
  apply_synth_page_volume(performance_page_t::chord, force);
  apply_synth_page_volume(performance_page_t::drum, force);
  if (melody_settings.source == synth_tone_source_t::general_midi) {
    set_program(kp::def::midi::channel_1, melody_settings.program);
  }
  if (chord_settings.source == synth_tone_source_t::general_midi) {
    set_program(kp::def::midi::channel_2, chord_settings.program);
  }
  if (bass_settings.source == synth_tone_source_t::general_midi) {
    set_program(kp::def::midi::channel_3, bass_settings.program);
  }
  // This does no work while the selected Pad/loop settings are unchanged.
  // On a change it prepares the shared sustain working set before playing.
  prime_synth_sustain_cache(performance_page_t::melody);
  prime_synth_sustain_cache(performance_page_t::bass);
  prime_synth_sustain_cache(performance_page_t::chord);
}

static void apply_mixer_part(mixer_part_t part)
{
  switch (part) {
  case mixer_part_t::bgm:
    sampler_audio_t::setVoiceVolumeQ8(background_loop_voice,
      mixer_scaled_volume_q8(part, background_loop.volume_q8));
    break;
  case mixer_part_t::sampler:
    for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
      sampler_audio_t::setVoiceVolumeQ8(pad,
        mixer_scaled_volume_q8(part, sampler_pool_t::slot[pad].volume_q8));
    }
    break;
  case mixer_part_t::bass:
  case mixer_part_t::melody:
  case mixer_part_t::chord: {
    const pitched_voice_owner_t owner = part == mixer_part_t::bass
      ? pitched_voice_owner_t::bass
      : part == mixer_part_t::melody ? pitched_voice_owner_t::melody
                                     : pitched_voice_owner_t::chord;
    const auto& settings = page_settings(part == mixer_part_t::bass
      ? performance_page_t::bass
      : part == mixer_part_t::melody ? performance_page_t::melody
                                     : performance_page_t::chord);
    const uint8_t source_pad = resolved_pad_sound(settings);
    uint16_t base_q8 = 0;
    if (source_pad < def::pad::pad_count) {
      base_q8 = (uint16_t)std::min<uint32_t>(512,
        ((uint32_t)sampler_pool_t::slot[source_pad].volume_q8 * settings.volume) / 100);
    }
    for (uint8_t i = 0; i < external_midi_voice_count; ++i) {
      if (pitched_voice_state[i].owner == owner) {
        sampler_audio_t::setVoiceVolumeQ8(external_midi_voice_base + i,
          mixer_scaled_volume_q8(part, base_q8));
      }
    }
    apply_synth_page_volume(part == mixer_part_t::bass ? performance_page_t::bass
                            : part == mixer_part_t::melody ? performance_page_t::melody
                            : performance_page_t::chord, true);
    break; }
  case mixer_part_t::drum:
    apply_synth_page_volume(performance_page_t::drum, true);
    break;
  default:
    break;
  }
}

static void mixer_clear_applied_snapshot(void)
{
  const int8_t previous = mixer_applied_snapshot;
  mixer_applied_snapshot = -1;
  if (mixer_active && previous >= 0 && previous < 4) {
    request_pad_draw(display_order_to_pad((uint8_t)(8 + previous)));
  }
}

static void set_mixer_part_muted(mixer_part_t part, bool muted)
{
  const uint8_t index = (uint8_t)part;
  if (index >= mixer_part_count) { return; }
  mixer_clear_applied_snapshot();
  mixer_part_muted[index] = muted;
  if (muted && (part == mixer_part_t::bass
             || part == mixer_part_t::melody
             || part == mixer_part_t::chord)) {
    const performance_page_t page = part == mixer_part_t::bass
      ? performance_page_t::bass
      : part == mixer_part_t::melody ? performance_page_t::melody
                                     : performance_page_t::chord;
    for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
      release_synth_trigger(page, pad);
    }
    if (page == performance_page_t::melody || page == performance_page_t::bass) {
      reset_page_pitch_bend(page, true);
    }
  }
  apply_mixer_part(part);
}

static void apply_all_mixer_parts(void)
{
  for (uint8_t part = 0; part < mixer_part_count; ++part) {
    apply_mixer_part((mixer_part_t)part);
  }
}

// Mixer changes are performance-only. Once transport stops, restore the
// normal Kit balance so another page never starts with an invisible mute or
// reduced part volume. Stored MIX snapshots remain available for recall.
static void reset_mixer_mix(void)
{
  std::fill(mixer_part_volume, mixer_part_volume + mixer_part_count, 100);
  std::fill(mixer_part_muted, mixer_part_muted + mixer_part_count, false);
  mixer_pending_snapshot = -1;
  mixer_applied_snapshot = -1;
  apply_all_mixer_parts();
  if (mixer_active) {
    request_wave_draw();
    request_grid_draw();
  }
}

static void set_mixer_notice(const char* text)
{
  snprintf(mixer_notice, sizeof(mixer_notice), "%s", text ? text : "");
  mixer_notice_until_msec = M5.millis() + 1400;
  request_wave_draw();
}

static void apply_mixer_snapshot(uint8_t snapshot)
{
  if (snapshot >= 4 || !mixer_snapshot[snapshot].valid) { return; }
  memcpy(mixer_part_volume, mixer_snapshot[snapshot].volume, sizeof(mixer_part_volume));
  memcpy(mixer_part_muted, mixer_snapshot[snapshot].muted, sizeof(mixer_part_muted));
  mixer_pending_snapshot = -1;
  for (uint8_t part = 0; part < mixer_part_count; ++part) {
    set_mixer_part_muted((mixer_part_t)part, mixer_part_muted[part]);
  }
  mixer_applied_snapshot = (int8_t)snapshot;
  char message[16];
  snprintf(message, sizeof(message), "MIX %c", 'A' + snapshot);
  set_mixer_notice(message);
  request_grid_draw();
}

static void apply_pending_mixer_snapshot(void)
{
  if (mixer_pending_snapshot < 0 || mixer_pending_snapshot >= 4) { return; }
  apply_mixer_snapshot((uint8_t)mixer_pending_snapshot);
}

static void mixer_set_active(bool active)
{
  if (mixer_active == active) { return; }
  if (active) {
    if (fx_pad_active >= 0) {
      const uint8_t number = pad_display_number((uint8_t)fx_pad_active);
      const int8_t fx = fx_index_for_pad_number(number);
      if (fx >= 0) { fx_set_active((uint8_t)fx, false); }
    }
    fx_pad_active = -1;
  }
  mixer_active = active;
  mixer_held_part = -1;
  std::fill(mixer_pad_armed, mixer_pad_armed + def::pad::pad_count, false);
  std::fill(mixer_pad_adjusted, mixer_pad_adjusted + def::pad::pad_count, false);
  mixer_notice[0] = 0;
  mixer_notice_until_msec = 0;
  request_wave_draw();
  request_grid_draw();
  request_fn_draw(2);
}

static void mixer_pad_press(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  const uint8_t number = pad_display_number((uint8_t)pad);
  const mixer_part_t part = mixer_part_for_pad_number(number);
  if (part != mixer_part_t::count || (number >= 9 && number <= 12)) {
    mixer_pad_armed[pad] = true;
    mixer_pad_adjusted[pad] = false;
    if (part != mixer_part_t::count) { mixer_held_part = (int8_t)part; }
    if (number >= 9 && number <= 12) {
      char waiting[28];
      char ready[28];
      snprintf(waiting, sizeof(waiting), "HOLD TO SAVE MIX %u", (unsigned)(number - 8));
      snprintf(ready, sizeof(ready), "RELEASE TO SAVE MIX %u", (unsigned)(number - 8));
      begin_hold_progress(hold_progress_kind_t::mix_save, pads[pad].press_msec,
                          mixer_snapshot_hold_msec, 0x70C8FFu, waiting, ready);
    }
    request_pad_draw(pad);
  }
}

static void mixer_pad_release(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !mixer_pad_armed[pad]) { return; }
  const uint8_t number = pad_display_number((uint8_t)pad);
  if (number >= 9 && number <= 12) { cancel_hold_progress(hold_progress_kind_t::mix_save); }
  mixer_pad_armed[pad] = false;

  const mixer_part_t mapped_part = mixer_part_for_pad_number(number);
  if (mapped_part != mixer_part_t::count) {
    const uint8_t part = (uint8_t)mapped_part;
    if (!mixer_pad_adjusted[pad]) {
      set_mixer_part_muted((mixer_part_t)part, !mixer_part_muted[part]);
    }
    mixer_held_part = -1;
    // If several parts are held, the most recently found held pad remains
    // available to either encoder after this pad is released.
    for (uint8_t next_part = 0; next_part < mixer_part_count; ++next_part) {
      const uint8_t next_pad = mixer_pad_for_part(next_part);
      if (mixer_pad_armed[next_pad] && pads[next_pad].pressed) {
        mixer_held_part = next_part;
      }
    }
    request_pad_draw(pad);
    request_wave_draw();
    return;
  }

  if (number >= 9 && number <= 12) {
    const uint8_t snapshot = number - 9;
    const uint32_t held_msec = M5.millis() - pads[pad].press_msec;
    if (held_msec >= mixer_snapshot_hold_msec) {
      mixer_snapshot[snapshot].valid = true;
      memcpy(mixer_snapshot[snapshot].volume, mixer_part_volume, sizeof(mixer_part_volume));
      memcpy(mixer_snapshot[snapshot].muted, mixer_part_muted, sizeof(mixer_part_muted));
      char message[20];
      snprintf(message, sizeof(message), "SAVED MIX %c", 'A' + snapshot);
      set_mixer_notice(message);
    } else if (!mixer_snapshot[snapshot].valid) {
      char message[20];
      snprintf(message, sizeof(message), "EMPTY MIX %c", 'A' + snapshot);
      set_mixer_notice(message);
    } else if (loop_playing && loop_length_fixed) {
      mixer_pending_snapshot = snapshot;
      char message[20];
      snprintf(message, sizeof(message), "NEXT MIX %c", 'A' + snapshot);
      set_mixer_notice(message);
    } else {
      apply_mixer_snapshot(snapshot);
    }
    request_pad_draw(pad);
  }
}

static void mixer_volume_add(int diff)
{
  if (mixer_held_part < 0 || mixer_held_part >= mixer_part_count || diff == 0) { return; }
  const uint8_t part = (uint8_t)mixer_held_part;
  const uint8_t pad = mixer_pad_for_part(part);
  if (!mixer_pad_armed[pad] || !pads[pad].pressed) { return; }
  mixer_pad_adjusted[pad] = true;
  if (mixer_part_muted[part]) {
    // A short press restores the stored level immediately. Turning upward
    // instead starts a performance fade-in from silence; turning downward
    // keeps the part muted and must not toggle it when the pad is released.
    if (diff < 0) { return; }
    mixer_part_volume[part] = 0;
    set_mixer_part_muted((mixer_part_t)part, false);
  }
  const int value = std::clamp<int>((int)mixer_part_volume[part] + diff * 5, 0, 100);
  if (value == mixer_part_volume[part]) { return; }
  mixer_part_volume[part] = (uint8_t)value;
  mixer_clear_applied_snapshot();
  apply_mixer_part((mixer_part_t)part);
  request_pad_draw(pad);
  request_wave_draw();
}

static void set_performance_page(performance_page_t page)
{
  if ((uint8_t)page >= (uint8_t)performance_page_t::max || page == current_page) { return; }
  // Part navigation is always an instrument-performance gesture. Leaving
  // SAMPLE's edit/record surface or FX must therefore land on PLAY, including
  // its mode-tab/LED refresh and FX teardown, before composing the next part.
  if (current_mode == sampler_mode_t::mode_rec || current_mode == sampler_mode_t::mode_fx) {
    set_mode(sampler_mode_t::mode_play);
  }
  const performance_page_t previous_page = current_page;
  // Recorded events remain audible across pages, but Undo is deliberately a
  // page-local, immediate-performance operation.
  loop_undo_history[(uint8_t)previous_page].clear();
  if (previous_page == performance_page_t::melody
   || previous_page == performance_page_t::bass) {
    auto& bend = page_pitch_bend[(uint8_t)previous_page];
    bend.down_held = false;
    bend.up_held = false;
    // Recorded automation keeps running in the background. Without a playing
    // loop this is only a live gesture, so returning to centre is appropriate.
    if (!loop_playing) { reset_page_pitch_bend(previous_page, true); }
  }
  if (recording_pad >= 0) { finish_pad_recording(); }
  cancel_sample_move();
  cancel_sample_add();
  if (edit_pad >= 0) { exit_edit(); }
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    if (pads[pad].pressed && previous_page != performance_page_t::sample) {
      release_synth_trigger(previous_page, pad);
    }
    pads[pad].pressed = false;
  }
  current_page = page;
  uint32_t next_generation = ui_page_generation + 1u;
  ui_page_generation = next_generation ? next_generation : 1u;
  reset_live_wave();
  invalidate_loop_timeline_cache();
  repair_pitched_pad_sources();
  apply_synth_tones(true);
  // Header, information canvas, and pad tiles are independent retained
  // regions. Keep the unchanged mode tabs and background in place, and let
  // flush_dirty_ui() compose the new page over several short turns.
  request_header_draw();
  request_wave_draw();
  if (!present_current_grid_cache()) { request_grid_draw(); }
  request_surface_sync();
  update_all_leds();
}

static void move_performance_page(int direction)
{
  int page = (int)performance_page_order_index(current_page) + direction;
  const int count = (int)performance_page_t::max;
  page = std::clamp<int>(page, 0, count - 1);
  set_performance_page(performance_page_order[page]);
}

static uint32_t fx_speed_reference_pos(uint32_t now)
{
  if (!fx_speed_reference_active || fx_speed_reference_length_ms == 0) { return 0; }
  return (uint32_t)(((uint64_t)fx_speed_reference_origin_pos_ms
        + (uint32_t)(now - fx_speed_reference_origin_msec)) % fx_speed_reference_length_ms);
}

static int32_t fx_speed_phase_error_ms(uint32_t target, uint32_t current, uint32_t length)
{
  if (length == 0) { return 0; }
  int32_t error = (int32_t)(target % length) - (int32_t)(current % length);
  const int32_t half = (int32_t)(length / 2u);
  if (error > half) { error -= (int32_t)length; }
  if (error < -half) { error += (int32_t)length; }
  return error;
}

static uint32_t fx_speed_return_duration(int32_t error_ms)
{
  const uint32_t distance = (uint32_t)std::abs(error_ms);
  // This is a performance flourish, not a forensic phase correction. Give a
  // small error a quick flick and cap even a long-held Tempo gesture at one
  // second; the existing faded seek handles the remaining distance.
  const uint32_t physical = error_ms >= 0 ? distance : distance * 2u;
  return std::clamp<uint32_t>(std::max<uint32_t>(240u + distance / 4u,
                                                 physical + 80u),
                              240u, 1000u);
}

static void finish_fx_speed_return(uint32_t now)
{
  uint32_t reference_pos = 0;
  const bool align_transport = fx_speed_reference_active && loop_playing
                            && loop_length_fixed && loop_length_msec != 0
                            && loop_length_msec == fx_speed_reference_length_ms;
  if (align_transport) { reference_pos = fx_speed_reference_pos(now); }

  fx_speed_ratio_current_q8 = 256;
  fx_speed_ratio_target_q8 = 256;
  sampler_audio_t::setFxSpeedRatioQ8(256);
  if (align_transport) {
    rebase_loop_transport(now, reference_pos);
    if (background_loop.isValid() && background_loop.frames) {
      const uint32_t frame = (uint32_t)(((uint64_t)reference_pos
        * background_loop.sample_rate) / 1000u) % background_loop.frames;
      // seek() performs its own 2ms fade, hiding the final sub-frame residue
      // after the audible catch-up/deceleration has done the real work.
      sampler_audio_t::seek(background_loop_voice, frame);
    }
  }
  background_loop_resync_pending = false;
  fx_speed_active = false;
  fx_speed_returning = false;
  fx_speed_reference_active = false;
  sampler_audio_t::setFxActive(0, false);
}

static void fx_set_speed_active(bool active)
{
  fx_speed_pressed = active;
  if (active) {
    // Keep the current ratio as the start of the ramp. A stored parameter can
    // therefore be applied musically when Fn is pressed, rather than jumping.
    fx_speed_active = true;
    fx_speed_returning = false;
    if (!fx_speed_reference_active && loop_playing && loop_length_fixed
     && loop_length_msec != 0) {
      const uint32_t now = M5.millis();
      fx_speed_reference_active = true;
      fx_speed_reference_origin_msec = now;
      fx_speed_reference_origin_pos_ms = loop_pos_ms(now);
      fx_speed_reference_length_ms = loop_length_msec;
    }
    sampler_audio_t::setFx(0, true, fx_param[0]);
  } else if (fx_speed_reference_active && loop_playing && loop_length_fixed
          && loop_length_msec == fx_speed_reference_length_ms) {
    const uint32_t now = M5.millis();
    const int32_t error = fx_speed_phase_error_ms(
      fx_speed_reference_pos(now), loop_pos_ms(now), loop_length_msec);
    fx_speed_returning = true;
    fx_speed_return_started_msec = now;
    fx_speed_return_duration_msec = fx_speed_return_duration(error);
  }
  if (!fx_speed_returning) {
    const int param = active ? fx_param[0] : 0;
    fx_speed_ratio_target_q8 = param >= 0
      ? (uint16_t)(256 + param * 512 / 100)
      : (uint16_t)(256 + param * 256 / 100);
  }
  fx_speed_last_msec = M5.millis();
}

static void service_fx_speed(uint32_t now)
{
  if (!fx_speed_active) { return; }
  if (fx_speed_last_msec == 0) {
    fx_speed_last_msec = now;
    return;
  }
  uint32_t elapsed = now - fx_speed_last_msec;
  if (elapsed == 0) { return; }
  // A stalled UI must not turn the next frame into a sudden speed jump.
  if (elapsed > 24) { elapsed = 24; }
  fx_speed_last_msec = now;

  if (fx_speed_returning) {
    const bool reference_valid = fx_speed_reference_active && loop_playing
                              && loop_length_fixed && loop_length_msec != 0
                              && loop_length_msec == fx_speed_reference_length_ms;
    const uint32_t return_elapsed = now - fx_speed_return_started_msec;
    if (!reference_valid || return_elapsed >= fx_speed_return_duration_msec) {
      finish_fx_speed_return(now);
      return;
    }
    const uint32_t remaining = std::max<uint32_t>(1,
      fx_speed_return_duration_msec - return_elapsed);
    const int32_t error = fx_speed_phase_error_ms(
      fx_speed_reference_pos(now), loop_pos_ms(now), loop_length_msec);
    if (return_elapsed >= 120u && std::abs(error) <= 2
     && std::abs((int)fx_speed_ratio_current_q8 - 256) <= 2) {
      finish_fx_speed_return(now);
      return;
    }
    // rate = 1 + phase_error / remaining_time. This continuously follows the
    // moving normal-speed reference instead of aiming at a stale release point.
    const int32_t correction_q8 = (int32_t)(((int64_t)error * 256ll) / remaining);
    fx_speed_ratio_target_q8 = (uint16_t)std::clamp<int32_t>(256 + correction_q8, 128, 512);
  }

  const int current = fx_speed_ratio_current_q8;
  const int target = fx_speed_ratio_target_q8;
  int next = current;
  if (current != target) {
    const int distance = target - current;
    const uint32_t ramp_msec = fx_speed_returning ? 72u : fx_speed_ramp_msec;
    int step = (std::abs(distance) * (int)elapsed + (int)ramp_msec - 1)
             / (int)ramp_msec;
    if (step < 1) { step = 1; }
    if (step > std::abs(distance)) { step = std::abs(distance); }
    next += distance > 0 ? step : -step;
  }
  if (next != current) {
    // Preserve the old transport position before switching to the next small
    // speed increment. This is a piecewise integration of the DJ-style ramp.
    const uint32_t position = loop_playing ? loop_pos_ms(now) : 0;
    fx_speed_ratio_current_q8 = (uint16_t)next;
    sampler_audio_t::setFxSpeedRatioQ8(fx_speed_ratio_current_q8);
    if (loop_playing) {
      rebase_loop_transport(now, position);
    }
  }
  if (!fx_speed_pressed && !fx_speed_returning && fx_speed_ratio_current_q8 == 256) {
    finish_fx_speed_return(now);
  }
}

static void set_bgm_scratch_lever(int8_t direction, bool pressed)
{
  if (!sampler_audio_t::masterScratchAvailable()
   || loop_repeat_armed || loop_repeat_running) { return; }
  const uint32_t now = M5.millis();
  if (pressed) {
    sampler_audio_t::setTapeStop(false);
    if (fx_pad_active >= 0 && pad_display_number((uint8_t)fx_pad_active) == 7) {
      request_pad_draw(fx_pad_active);
      fx_pad_active = -1;
    }
    // The lever makes a short deck gesture, then holds the record still.
    // Down is a natural forward push; up is a natural pull backwards.
    bgm_scratch_active = true;
    bgm_scratch_return_pending = false;
    bgm_scratch_rejoin_after_stop = false;
    bgm_scratch_target_q8 = direction < 0 ? -384 : 384;
    bgm_scratch_gesture_until_msec = now + bgm_scratch_gesture_msec;
    bgm_scratch_last_msec = now;
    sampler_audio_t::setMasterScratchRateQ8(bgm_scratch_rate_q8);
    sampler_audio_t::setMasterScratch(true);
    return;
  }

  // If the other lever direction is still held, its press edge has already
  // selected the new target speed and must not schedule a return yet.
  namespace bb = kp::def::button_bitmask;
  if (prev_bitmask & (bb::KNOB_L | bb::KNOB_R)) { return; }
  if (!bgm_scratch_active) { return; }
  // Returning the spring-loaded lever is the opposing hand movement. Make
  // one short reverse gesture before parking the record and snapping home.
  bgm_scratch_return_pending = false;
  bgm_scratch_rejoin_after_stop = true;
  bgm_scratch_target_q8 = direction < 0 ? 384 : -384;
  bgm_scratch_gesture_until_msec = now + bgm_scratch_gesture_msec;
  bgm_scratch_last_msec = now;
}

static void service_bgm_scratch(uint32_t now)
{
  if (!bgm_scratch_active) { return; }
  if (current_mode != sampler_mode_t::mode_fx
   || loop_repeat_armed || loop_repeat_running) {
    sampler_audio_t::setMasterScratch(false);
    sampler_audio_t::setMasterScratchRateQ8(256);
    bgm_scratch_active = false;
    bgm_scratch_return_pending = false;
    bgm_scratch_rejoin_after_stop = false;
    return;
  }
  if (bgm_scratch_gesture_until_msec != 0
   && (int32_t)(now - bgm_scratch_gesture_until_msec) >= 0) {
    bgm_scratch_gesture_until_msec = 0;
    bgm_scratch_target_q8 = 0;
  }
  uint32_t elapsed = now - bgm_scratch_last_msec;
  if (elapsed != 0) {
    if (elapsed > 20) { elapsed = 20; }
    bgm_scratch_last_msec = now;
    const int current = bgm_scratch_rate_q8;
    const int target = bgm_scratch_target_q8;
    if (current != target) {
      const int distance = target - current;
      int step = (std::abs(distance) * (int)elapsed + (int)bgm_scratch_ramp_msec - 1)
               / (int)bgm_scratch_ramp_msec;
      if (step < 1) { step = 1; }
      if (step > std::abs(distance)) { step = std::abs(distance); }
      bgm_scratch_rate_q8 = (int16_t)(current + (distance > 0 ? step : -step));
      sampler_audio_t::setMasterScratchRateQ8(bgm_scratch_rate_q8);
    }
  }
  if (bgm_scratch_rejoin_after_stop && bgm_scratch_gesture_until_msec == 0
   && bgm_scratch_rate_q8 == 0) {
    bgm_scratch_rejoin_after_stop = false;
    // The dry mix never stopped. A short audio-task crossfade is enough to
    // return without moving BGM, loop events or synth envelopes.
    sampler_audio_t::setMasterScratch(false);
    sampler_audio_t::setMasterScratchRateQ8(256);
    bgm_scratch_rate_q8 = 256;
    bgm_scratch_target_q8 = 256;
    bgm_scratch_return_pending = false;
    bgm_scratch_active = false;
  }
}

static void set_mode(sampler_mode_t mode) {
  if (current_page != performance_page_t::sample && mode == sampler_mode_t::mode_rec) {
    // SAMPLE is a home/edit button from Melody, Chord and Drum.  The current
    // page does not expose sample recording, so return to the Sample page
    // before entering its dedicated mode.
    set_performance_page(performance_page_t::sample);
  }
  if (mode == current_mode) { return; }
  if (mode == sampler_mode_t::mode_fx) {
    // FX owns the lever for Scratch. Do not leave a Repeat scheduler or a
    // Melody/Bass bend target running after the player changes mode.
    set_pad_repeat_mode(pad_repeat_mode_t::none);
    cancel_live_pitch_bend_levers();
    sampler_audio_t::setDeckBufferEnabled(true);
  }
  cancel_sample_move();
  cancel_sample_add();
  fn_modifier_hint_visible = false;
  if (edit_pad >= 0) { exit_edit(); }
  // Wave Canvas is shared by Edit, piano roll and Play's incremental waveform.
  // A mode change must force Play's first frame to clear the previous view.
  reset_live_wave();
  fn_information_panel_visible = false;
  const bool leaving_fx = current_mode == sampler_mode_t::mode_fx
                       && mode != sampler_mode_t::mode_fx;
  if (leaving_fx) {
    const bool transient_fx_surface = mixer_active || fx_pad_active >= 0;
    for (uint8_t i = 0; i < 3; ++i) {
      if (i == 0) { fx_set_speed_active(false); }
      else { sampler_audio_t::setFxActive(i, false); }
    }
    loop_repeat_set_active(false);
    sampler_audio_t::setMasterDelay(false);
    sampler_audio_t::setMasterScratch(false);
    sampler_audio_t::setTapeStop(false);
    sampler_audio_t::setDeckBufferEnabled(false);
    fx_pad_active = -1;
    mixer_active = false;
    mixer_held_part = -1;
    std::fill(mixer_pad_armed, mixer_pad_armed + def::pad::pad_count, false);
    std::fill(mixer_pad_adjusted, mixer_pad_adjusted + def::pad::pad_count, false);
    if (transient_fx_surface) {
      grid_cache_pad_mask[fx_grid_cache_index] = 0;
      grid_cache_fn_mask[fx_grid_cache_index] = 0;
    }
  }
  current_mode = mode;
  update_mode_leds();
  // A mode press is a performance gesture. Finish any tiny LCD transfer that
  // was already in flight, then update only the mode strip synchronously.
  // The larger information and control surfaces are retained dirty regions
  // and will be composed over subsequent main-loop turns on Core 0.
  wait_wave_transfer_job();
  wait_ui_dirty_transfers();
  draw_tabs();
  request_all_fn_draw();
  request_wave_draw();
  request_header_draw();
  request_surface_sync();
  if (mode == sampler_mode_t::mode_fx || leaving_fx) {
    // FX and performance pages use completely different Pad meanings. Mark
    // all tiles once, but never hold the input loop while transferring them.
    if (!present_current_grid_cache()) { request_grid_draw(); }
  }
}

static void volume_add(int diff) {
  int v = kp::system_registry->user_setting.getMasterVolume() + diff;
  if (v < 0) { v = 0; }
  if (v > 100) { v = 100; }
  kp::system_registry->user_setting.setMasterVolume(v);
  request_header_draw();
}

static uint32_t fx_delay_frames(void)
{
  const uint8_t index = std::min<uint8_t>(
    (uint8_t)fx_param[fx_delay_index], (uint8_t)delay_grid_option_count - 1u);
  const uint32_t step_ms = loop_quantize_step_ms(
    loop_display_length_ms(M5.millis()));
  const uint64_t duration_twice_ms = (uint64_t)step_ms * delay_half_steps[index];
  return (uint32_t)std::max<uint64_t>(1,
    ((uint64_t)sampler_audio_t::sample_rate * duration_twice_ms) / 2000u);
}

static void fx_set_active(uint8_t index, bool active)
{
  if (index > fx_tape_stop_index) { return; }
  if (active && index < 3) { fx_selected = index; }
  if (active && index == fx_delay_index) { fx_selected = fx_delay_index; }
  if (index == fx_tape_stop_index) {
    if (active) {
      // Tape Stop and Scratch share one Deck Buffer. The most recent gesture
      // wins, and the other reader is released before this Pad becomes active.
      sampler_audio_t::setMasterScratch(false);
      bgm_scratch_active = false;
      bgm_scratch_return_pending = false;
      bgm_scratch_rejoin_after_stop = false;
      bgm_scratch_rate_q8 = 256;
      bgm_scratch_target_q8 = 256;
      bgm_scratch_gesture_until_msec = 0;
      // Tape Stop reacts immediately, but its brake length follows two
      // musical Note Grids whenever a real loop/BGM has established one.
      // Free play has no trustworthy grid, so keep the deliberate 660ms feel.
      uint32_t duration_ms = 660;
      if (loop_length_fixed && loop_length_msec != 0) {
        duration_ms = loop_quantize_step_ms(loop_length_msec) * 2u;
      }
      sampler_audio_t::setTapeStopDurationMs(duration_ms);
    }
    sampler_audio_t::setTapeStop(active);
    request_wave_draw();
    return;
  }
  if (index == fx_delay_index) {
    if (active) { sampler_audio_t::setMasterDelayFrames(fx_delay_frames()); }
    sampler_audio_t::setMasterDelay(active);
  } else if (index == fx_repeat_index) {
    sampler_audio_t::setFxActive(index, false);
    loop_repeat_set_active(active);
  } else {
    if (index == fx_tempo_index) { fx_set_speed_active(active); }
    else { sampler_audio_t::setFx(index, active, fx_param[index]); }
  }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  request_wave_draw();
}

static void fx_pad_press(int pad)
{
  const uint8_t number = pad_display_number((uint8_t)pad);
  int fx = -1;
  int repeat_index = -1;
  if (number >= 1 && number <= 4) {
    if (!sampler_audio_t::masterRepeatAvailable()) {
      show_status_message("REPEAT UNAVAILABLE", 1800, false);
      return;
    }
    fx = 2;
    // Repeat options are {8, 4, 2, 1, 0.5}; pads expose 4 through 0.5.
    repeat_index = number;
  } else if (number == 5) {
    fx = 1;
  } else if (number == 6) {
    fx = 0;
  } else if (number == 7) {
    if (!sampler_audio_t::tapeStopAvailable()) {
      show_status_message("TAPE STOP UNAVAILABLE", 1800, false);
      return;
    }
    fx = fx_tape_stop_index;
  } else if (number == 8) {
    if (!sampler_audio_t::masterDelayAvailable()) {
      show_status_message("DELAY UNAVAILABLE", 1800, false);
      return;
    }
    fx = fx_delay_index;
  }
  if (fx < 0) { return; }

  bool preserve_repeat_start = false;
  if (fx_pad_active >= 0) {
    const uint8_t old_number = pad_display_number((uint8_t)fx_pad_active);
    const int8_t old_fx = fx_index_for_pad_number(old_number);
    preserve_repeat_start = fx == 2 && old_fx == 2
      && (loop_repeat_armed || loop_repeat_running);
    if (fx_pad_active != pad) {
      if (!preserve_repeat_start && old_fx >= 0) { fx_set_active((uint8_t)old_fx, false); }
      request_pad_draw(fx_pad_active);
    }
  }
  if (repeat_index >= 0) { fx_param[2] = (int8_t)repeat_index; }
  fx_pad_active = pad;
  if (preserve_repeat_start) {
    // Switching Repeat 4 -> 2 (or similar) changes only its window width.
    // The quantized start position chosen by the first button stays fixed.
    fx_selected = 2;
    loop_repeat_update_width_preserving_start();
    request_wave_draw();
  } else {
    fx_set_active((uint8_t)fx, true);
  }
  request_pad_draw(pad);
}

static void fx_pad_release(int pad)
{
  if (fx_pad_active != pad) { return; }
  const uint8_t number = pad_display_number((uint8_t)pad);
  const int8_t fx = fx_index_for_pad_number(number);
  if (fx >= 0) { fx_set_active((uint8_t)fx, false); }
  fx_pad_active = -1;
  request_pad_draw(pad);
}

// FX Repeat is a hold effect.  The normal release edge handles it first, but
// this also recovers if a very busy audio frame ever loses that edge.
static bool loop_repeat_control_held(uint32_t now)
{
  if (fx_pad_active < 0 || fx_pad_active >= (int)def::pad::pad_count
   || pad_display_number((uint8_t)fx_pad_active) > 4
   || kp::system_registry == nullptr) {
    loop_repeat_release_confirm_msec = 0;
    return false;
  }
  const int button = pad_to_button((uint8_t)fx_pad_active);
  const bool held = button >= 0
    && (kp::system_registry->internal_input.getButtonBitmask() & (1u << button));
  if (held) {
    loop_repeat_release_confirm_msec = 0;
    return true;
  }
  if (loop_repeat_release_confirm_msec == 0) {
    loop_repeat_release_confirm_msec = now + loop_repeat_release_debounce_msec;
    return true;
  }
  return (int32_t)(now - loop_repeat_release_confirm_msec) < 0;
}

static void fx_select_next(void)
{
  fx_selected = (fx_selected + 1) % fx_param_count;
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  draw_wave();
}

static void fx_param_add(int diff)
{
  int index = fx_selected;
  const bool stepped = index == fx_repeat_index || index == fx_delay_index;
  int value = (int)fx_param[index] + (stepped ? diff : diff * 5);
  if (index == fx_repeat_index) {
    int max_value = (int)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0])) - 1;
    if (value < 0) { value = 0; }
    if (value > max_value) { value = max_value; }
  } else if (index == fx_delay_index) {
    if (value < 0) { value = 0; }
    if (value >= (int)delay_grid_option_count) { value = (int)delay_grid_option_count - 1; }
  } else {
    if (value < -50) { value = -50; }
    if (value > 50) { value = 50; }
  }

  // Speed changes are ramped from the current transport position, so changing
  // this value only updates the destination and never jumps the BGM or loop.
  const uint32_t speed_change_now = index == 0 ? M5.millis() : 0;
  fx_param[index] = (int8_t)value;
  if (index == fx_repeat_index) {
    if (fn_pressed[2]) {
      if (loop_repeat_armed || loop_repeat_running) {
        loop_repeat_update_width_preserving_start();
      } else {
        loop_repeat_set_active(true);
      }
    }
  } else if (index == fx_delay_index) {
    sampler_audio_t::setMasterDelayFrames(fx_delay_frames());
  } else {
    if (index == 0) {
      // Parameter changes set a new destination. The actual transport and
      // sample-speed transition is performed gradually by service_fx_speed().
      if (fx_speed_pressed) {
        const int param = fx_param[0];
        fx_speed_ratio_target_q8 = param >= 0
          ? (uint16_t)(256 + param * 512 / 100)
          : (uint16_t)(256 + param * 256 / 100);
        fx_speed_last_msec = speed_change_now;
      }
    } else {
      sampler_audio_t::setFxParam((uint8_t)index, fx_param[index]);
    }
  }
  request_wave_draw();
}

static bool loop_event_crossed(uint32_t prev_pos, uint32_t pos, uint32_t event_pos)
{
  if (prev_pos == pos) { return false; }
  if (prev_pos < pos) { return event_pos > prev_pos && event_pos <= pos; }
  // During a smooth Speed FX, transport rebasing can round the reported
  // position back by a few milliseconds. That is not a cycle wrap and must
  // never retrigger the tail/head events (or seek the BGM). Master Repeat no
  // longer changes this dry transport timeline.
  const uint32_t cycle_length = loop_length_msec;
  if (cycle_length == 0 || prev_pos - pos < cycle_length / 2) { return false; }
  return event_pos > prev_pos || event_pos <= pos;
}

static uint32_t loop_repeat_width_ms(void)
{
  uint8_t index = std::min<uint8_t>((uint8_t)fx_param[2], (uint8_t)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0]) - 1));
  return std::max<uint32_t>(1, ((uint64_t)loop_quantize_step_ms(loop_length_msec) * loop_repeat_half_steps[index]) / 2);
}

static uint32_t loop_repeat_capture_width_ms(void)
{
  // FX pads expose at most 4 Grid. Capture that full range once so switching
  // 4 -> 2 -> 4 changes only the read window and never moves its anchor.
  const uint32_t four_grid = loop_quantize_step_ms(loop_length_msec) * 4u;
  return std::max<uint32_t>(loop_repeat_width_ms(), four_grid);
}

static void apply_master_repeat_width(void)
{
  const uint32_t repeat_frames = (uint32_t)(((uint64_t)loop_repeat_width_ms()
    * sampler_audio_t::sample_rate) / 1000u);
  const uint32_t capture_frames = (uint32_t)(((uint64_t)loop_repeat_capture_width_ms()
    * sampler_audio_t::sample_rate) / 1000u);
  sampler_audio_t::setMasterRepeatFrames(
    std::max<uint32_t>(1, repeat_frames), std::max<uint32_t>(1, capture_frames));
}

static uint32_t loop_next_quantize_pos_ms(uint32_t pos_ms)
{
  uint32_t steps = loop_quantize_steps();
  uint32_t step = ((uint64_t)pos_ms * steps + loop_length_msec - 1) / loop_length_msec;
  return ((uint64_t)(step % steps) * loop_length_msec) / steps;
}

static void loop_repeat_set_active(bool active)
{
  if (!active || !loop_playing || !loop_length_fixed || loop_length_msec == 0) {
    sampler_audio_t::setMasterRepeat(false);
    loop_repeat_armed = false;
    loop_repeat_running = false;
    loop_repeat_release_confirm_msec = 0;
    return;
  }
  uint32_t now = M5.millis();
  loop_repeat_start_pos_ms = loop_next_quantize_pos_ms(loop_pos_ms(now));
  loop_repeat_length_ms = loop_repeat_width_ms();
  apply_master_repeat_width();
  loop_repeat_armed = true;
  loop_repeat_running = false;
  loop_repeat_release_confirm_msec = 0;
}

static void loop_repeat_update_width_preserving_start(void)
{
  if (!loop_repeat_armed && !loop_repeat_running) { return; }
  // Keep loop_repeat_start_pos_ms intact: only the playback window changes.
  loop_repeat_length_ms = loop_repeat_width_ms();
  apply_master_repeat_width();
}

static uint8_t loop_event_dispatch_priority(loop_event_type_t type)
{
  if (type == loop_event_type_t::note_off) { return 0; }
  if (loop_event_is_pitch_bend(type)) { return 1; }
  return 2;
}

// Usually only a few events are due in one 1ms clock tick. Gather them with
// one pass over the loop, then use a tiny stable insertion sort so releases
// still precede bends and attacks at the same position. This avoids scanning
// a dense loop three times while the live input path is waiting on Core 1.
static void order_due_loop_events(size_t count)
{
  for (size_t i = 1; i < count; ++i) {
    const loop_event_t current = loop_due_events[i];
    const uint8_t priority = loop_event_dispatch_priority(current.type);
    size_t j = i;
    while (j > 0
        && loop_event_dispatch_priority(loop_due_events[j - 1].type) > priority) {
      loop_due_events[j] = loop_due_events[j - 1];
      --j;
    }
    loop_due_events[j] = current;
  }
}

static void refresh_loop_playback_events(void)
{
  const uint32_t revision = loop_events_revision;
  if (loop_playback_revision == revision) { return; }
  {
    loop_events_guard_t guard;
    loop_playback_event_count = std::min<size_t>(loop_events.size(), loop_event_max);
    for (size_t i = 0; i < loop_playback_event_count; ++i) {
      loop_playback_events[i] = loop_events[i];
    }
  }
  memset(loop_playback_bucket_mask, 0, sizeof(loop_playback_bucket_mask));
  loop_playback_bucket_length_ms = loop_length_fixed ? loop_length_msec : 0;
  for (size_t i = 0; i < loop_playback_event_count; ++i) {
    add_loop_playback_bucket_event(i, loop_playback_events[i]);
  }
  loop_playback_revision = revision;
}

static size_t collect_due_loop_events(uint32_t prev_pos, uint32_t pos)
{
  size_t due_count = 0;
  auto collect_index = [&](size_t index) {
    if (index >= loop_playback_event_count || due_count >= loop_event_max) { return; }
    const auto& event = loop_playback_events[index];
    if (loop_event_crossed(prev_pos, pos, loop_event_playback_pos(event))) {
      loop_due_events[due_count++] = event;
    }
  };
  auto collect_bucket = [&](uint8_t bucket) {
    for (uint8_t word = 0; word < loop_playback_bucket_words; ++word) {
      uint64_t bits = loop_playback_bucket_mask[bucket][word];
      while (bits) {
        const uint8_t bit = (uint8_t)__builtin_ctzll(bits);
        collect_index((size_t)word * 64 + bit);
        bits &= bits - 1;
      }
    }
  };

  // Small loops are quicker with a linear scan, and FX Repeat has a separate
  // relative timeline.  The indexed path pays off for recorded arrangements
  // with many note and note-off events.
  if (!loop_playback_buckets_ready() || loop_playback_event_count < 24) {
    for (size_t i = 0; i < loop_playback_event_count; ++i) { collect_index(i); }
    return due_count;
  }

  if (prev_pos < pos) {
    const uint8_t first = loop_playback_bucket_for_pos(prev_pos);
    const uint8_t last = loop_playback_bucket_for_pos(pos);
    for (uint8_t bucket = first; bucket <= last; ++bucket) { collect_bucket(bucket); }
  } else if (prev_pos > pos && loop_length_msec != 0
          && prev_pos - pos >= loop_length_msec / 2) {
    const uint8_t first = loop_playback_bucket_for_pos(prev_pos);
    const uint8_t last = loop_playback_bucket_for_pos(pos);
    for (uint8_t bucket = first; bucket < loop_playback_bucket_count; ++bucket) {
      collect_bucket(bucket);
    }
    for (uint8_t bucket = 0; bucket <= last; ++bucket) { collect_bucket(bucket); }
  }
  return due_count;
}

static bool service_loop_repeat(uint32_t now, uint32_t loop_pos)
{
  if ((loop_repeat_armed || loop_repeat_running) && !loop_repeat_control_held(now)) {
    loop_repeat_set_active(false);
    fx_pad_active = -1;
    return false;
  }
  if (loop_repeat_armed) {
    if (loop_pos != loop_repeat_start_pos_ms
     && !loop_event_crossed(loop_prev_pos_ms, loop_pos, loop_repeat_start_pos_ms)) {
      return false;
    }
    loop_repeat_armed = false;
    loop_repeat_running = true;
    apply_master_repeat_width();
    sampler_audio_t::setMasterRepeat(true);
  }
  // The dry BGM, loop events and live notes continue underneath the repeated
  // final-mix PCM, so normal scheduling must never be suppressed here.
  return false;
}

static uint32_t background_expected_frame(uint32_t loop_pos_ms, uint32_t frames)
{
  if (frames == 0 || loop_length_msec == 0) { return 0; }
  // BGM Repeat数を含む1周全体を、transportの現在位置へ線形に対応させる。
  // 最後に素材1周内へ畳み込むため、端数フレームも毎周リセットできる。
  const uint64_t total_frames = (uint64_t)frames * std::max<uint8_t>(1, background_loop.loop_repeats);
  return (uint32_t)(((uint64_t)loop_pos_ms * total_frames / loop_length_msec) % frames);
}

static uint32_t circular_frame_distance(uint32_t a, uint32_t b, uint32_t frames)
{
  if (frames == 0) { return 0; }
  const uint32_t direct = a > b ? a - b : b - a;
  return std::min<uint32_t>(direct, frames - direct);
}

static void resync_background_loop_at_cycle_head(uint32_t loop_pos_ms)
{
  if (!background_loop.isValid()) { return; }

  uint32_t frame = 0;
  uint32_t frames = 0;
  const bool reading = sampler_audio_t::getPlaybackPosition(background_loop_voice, &frame, &frames);
  const bool speed_correction = background_loop_resync_pending;
  background_loop_resync_pending = false;
  if (!reading || frames == 0) {
    play_background_loop_at(loop_pos_ms);
    return;
  }

  if (speed_correction) {
    // Speed FX後だけは、すでに鳴った頭拍を繰り返さないよう少し先へ置く。
    const uint32_t skip = std::min<uint32_t>((background_loop.sample_rate * background_resync_skip_ms) / 1000,
                                             frames - 1);
    const bool already_near_head = frame <= skip || frame >= frames - skip;
    if (!already_near_head) { sampler_audio_t::seek(background_loop_voice, skip); }
    return;
  }

  const uint32_t expected = background_expected_frame(loop_pos_ms, frames);
  const uint32_t tolerance = std::max<uint32_t>(1,
    (background_loop.sample_rate * background_resync_tolerance_ms) / 1000);
  if (circular_frame_distance(frame, expected, frames) > tolerance) {
    // seek()はオーディオタスク内で2msフェードしてから位置を替える。
    sampler_audio_t::seek(background_loop_voice, expected);
  }
}

static void service_live_min_gate_releases(uint32_t now)
{
  uint16_t pending = loop_live_release_pending_mask;
  while (pending) {
    const uint8_t pad = (uint8_t)__builtin_ctz(pending);
    pending &= pending - 1;
    if ((int32_t)(now - loop_live_min_gate_until[pad]) < 0) { continue; }
    const uint16_t layer = loop_live_release_layer[pad];
    set_loop_live_release_pending(pad, false);
    loop_live_release_layer[pad] = 0;
    loop_live_min_gate_until[pad] = 0;
    if (loop_active_layer[pad] == 0 || loop_active_layer[pad] == layer) {
      trigger_loop_event({ pad, loop_event_type_t::note_off, 0, layer });
    }
  }
  for (uint8_t page = 0; page < (uint8_t)performance_page_t::max; ++page) {
    pending = synth_live_release_pending_mask[page];
    while (pending) {
      const uint8_t pad = (uint8_t)__builtin_ctz(pending);
      pending &= pending - 1;
      if ((int32_t)(now - synth_live_min_gate_until[page][pad]) < 0) { continue; }
      const uint16_t layer = synth_live_release_layer[page][pad];
      set_synth_live_release_pending(page, pad, false);
      synth_live_release_layer[page][pad] = 0;
      synth_live_min_gate_until[page][pad] = 0;
      if (synth_loop_active_layer[page][pad] == 0
       || synth_loop_active_layer[page][pad] == layer) {
        release_synth_trigger((performance_page_t)page, pad);
      }
    }
  }
}

static void service_loop(uint32_t now)
{
  service_live_min_gate_releases(now);
  service_melody_pitch_bend(now);
  service_pad_repeat(now);
  service_sample_grid_loops(now);
  if (!loop_playing) { return; }
  if (loop_record_enabled && !loop_length_fixed) { return; }
  uint32_t pos = loop_pos_ms(now);
  const bool loop_wrapped = pos < loop_prev_pos_ms
                         && loop_length_msec != 0
                         && loop_prev_pos_ms - pos >= loop_length_msec / 2;
  refresh_loop_playback_events();
  service_soft_snap_live(now, loop_prev_pos_ms, pos);
  if (loop_wrapped) { apply_pending_mixer_snapshot(); }
  if (loop_wrapped && background_loop.isValid()) {
    resync_background_loop_at_cycle_head(pos);
  }
  bool repeating = service_loop_repeat(now, pos);
  if (!repeating) {
    const size_t due_count = collect_due_loop_events(loop_prev_pos_ms, pos);
    order_due_loop_events(due_count);
    for (size_t i = 0; i < due_count; ++i) { trigger_loop_event(loop_due_events[i]); }
  }
  for (int i = 0; !repeating && i < (int)def::pad::pad_count; ++i) {
    if (loop_deferred_live_pad[i]
     && loop_event_crossed(loop_prev_pos_ms, pos, loop_deferred_live_pos_ms[i])) {
      loop_deferred_live_pad[i] = false;
      const uint32_t start_frame = loop_deferred_live_start_frame[i];
      loop_deferred_live_start_frame[i] = 0;
      if (start_frame) { play_sample_once(i, start_frame); }
      else { trigger_pad(i); }
    }
  }
  loop_prev_pos_ms = pos;
}

#if !defined (M5UNIFIED_PC_BUILD)
// 画面描画とは独立して、1ms周期でループイベントを発火する。
// I2Sより一段低い優先度で同一CPUへ置き、UI処理中の再生ジッターを防ぐ。
static void loop_clock_task(void*)
{
  for (;;) {
    service_loop(M5.millis());
    vTaskDelay(1);
  }
}
#endif

static void render_page_selector(void)
{
  if (!page_selector_canvas_ready) { return; }
  auto& d = page_selector_canvas;
  d.startWrite();
  static constexpr const char* labels[] = {
    "DRUM KIT", "SAMPLER", "BASS", "MELODY", "CHORD"
  };
  static constexpr int normal_h = 17;
  static constexpr int selected_h = 36;
  static constexpr int top = 4;
  const uint32_t background = 0x181820u;

  d.fillSprite(background);
  d.drawRect(0, 0, page_selector_w, page_selector_h, 0xD8D8E0u);
  d.drawRect(1, 1, page_selector_w - 2, page_selector_h - 2, 0x606068u);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  int y = top;
  for (uint8_t index = 0; index < (uint8_t)performance_page_t::max; ++index) {
    const bool selected = index == page_selector_index;
    const int row_h = selected ? selected_h : normal_h;
    const auto page = performance_page_order[index];
    const uint32_t color = performance_page_colors[(uint8_t)page];
    if (selected) {
      d.fillRect(6, y, page_selector_w - 12, row_h, color);
      d.drawRect(6, y, page_selector_w - 12, row_h, 0xFFFFFFu);
    }
    d.setTextSize(selected ? 2 : 1);
    d.setTextColor(selected ? 0x08080Cu : color, selected ? color : background);
    d.drawString(labels[index], page_selector_w / 2, y + row_h / 2);
    y += row_h;
  }
  d.endWrite();
}

static void draw_page_selector(bool slide_in)
{
  if (!page_selector_canvas_ready) { return; }
  render_page_selector();
  // The selector remains available during BLE performance, but its staged
  // slide is cosmetic and should not compete with incoming note bursts.
  if (ble_midi_cache_guard_active() || sound_priority_active(M5.millis())
   || physical_input_pending()) { slide_in = false; }
  if (slide_in) {
    // The selector itself is cached in a sprite. Sliding that one surface is
    // the same low-flicker approach used by the KANTAN Play menu transition.
    for (int frame = 1; frame <= 4; ++frame) {
      const int x = page_selector_x - page_selector_w + (page_selector_w * frame) / 4;
      page_selector_canvas.pushSprite(x, page_selector_y);
      M5.delay(2);
    }
  } else {
    page_selector_canvas.pushSprite(page_selector_x, page_selector_y);
  }
}

static void page_selector_move(int delta)
{
  if (delta == 0 || menu_visible || edit_pad >= 0) { return; }
  if (!page_selector_visible) {
    wave_transfer_generation = wave_transfer_generation + 1;
    wave_transfer_active = false;
    wave_transfer_full_frame = false;
    wait_wave_transfer_job();
    page_selector_restore_pending = false;
    page_selector_visible = true;
    page_selector_index = performance_page_order_index(current_page);
    page_selector_slide_in = true;
  }
  int index = (int)page_selector_index + delta;
  const int count = (int)performance_page_t::max;
  index = std::clamp<int>(index, 0, count - 1);
  page_selector_index = (uint8_t)index;
  page_selector_until_msec = M5.millis() + page_selector_timeout_ms;
  page_selector_dirty = true;
}

static bool page_selector_confirm(bool defer_visual_restore)
{
  if (!page_selector_visible) { return false; }
  page_selector_visible = false;
  page_selector_dirty = false;
  page_selector_restore_pending = false;
  const performance_page_t target = performance_page_order[page_selector_index];
  const bool page_changed = target != current_page;
  set_performance_page(target);
  // A performance input must reach audio before this cosmetic restoration.
  // The retained Wave Canvas already contains the covered surface, so a
  // same-page close never needs an expensive full-screen reconstruction.
  if (!page_changed) {
    if (defer_visual_restore) { page_selector_restore_pending = true; }
    else { push_wave_canvas(); }
  }
  return true;
}

static void service_page_selector(uint32_t now)
{
  if (!page_selector_visible) {
    if (page_selector_restore_pending && !ui_async_display_busy()
     && !physical_input_pending()) {
      page_selector_restore_pending = false;
      push_wave_canvas();
    }
    return;
  }
  if ((int32_t)(now - page_selector_until_msec) >= 0) {
    page_selector_confirm();
    return;
  }
  const uint32_t interval = sound_priority_active(now) ? 50 : 28;
  if (!page_selector_dirty && now - page_selector_last_draw_msec < interval) { return; }
  if (ui_async_display_busy()) { return; }
  draw_page_selector(page_selector_slide_in);
  page_selector_slide_in = false;
  page_selector_dirty = false;
  page_selector_last_draw_msec = now;
}

static void process_encoder_delta(uint8_t encoder, int8_t delta)
{
  if (delta == 0) { return; }
  // ENC3 is the jog dial. It mirrors ENC2 wherever a value can be edited,
  // but keeps its intentionally reversed physical direction.
  if (encoder == 2) { delta = -delta; }
  if (wifi_update_active) { return; }
  if (wifi_file_server_qr_active) {
    // File Editor中も上側エンコーダーの音量操作は利用できる。
    // 下側エンコーダーはWebセッションを終了させず、完全に無視する。
    if (encoder == 0) { volume_add(delta * 5); }
    return;
  }

  if (menu_visible) {
    if (encoder == 1 || encoder == 2) { menu_move(delta); }
    return;
  }

  if (encoder == 0) {
    if (page_selector_visible) { page_selector_confirm(true); }
    volume_add(delta * 5);
    return;
  }

  // Touch Play owns the performance surface. Discard the page/edit encoder
  // deltas here as well as their button pushes below, so a queued jog turn
  // cannot change pages after the player lifts Fn3.
  if (touch_play_active) { return; }

  if (encoder != 1 && encoder != 2) { return; }
  if (edit_pad >= 0) {
    edit_value_add(delta);
  } else if (current_mode == sampler_mode_t::mode_fx) {
    if (mixer_active) { mixer_volume_add(delta); }
    else if (fx_selected != fx_repeat_index) { fx_param_add(delta); }
  } else {
    // ENC2 also opens the selector when it has no context-specific editing
    // task, matching the dedicated ENC3 jog dial for quick page changes.
    page_selector_move(delta);
  }
}

static void process_encoder_value(uint8_t encoder, uint32_t value)
{
  static uint8_t prev_value[3] = { 0, 0, 0 };
  static bool initialized[3] = { false, false, false };
  if (encoder >= 3) { return; }
  uint8_t current = (uint8_t)value;
  if (!initialized[encoder]) {
    prev_value[encoder] = current;
    initialized[encoder] = true;
    return;
  }
  int8_t delta = (int8_t)(current - prev_value[encoder]);
  prev_value[encoder] = current;
  process_encoder_delta(encoder, delta);
}

// I2C入力は1回の更新中にエンコーダー値を複数件届けることがある。
// そのたびに描画すると古い値の描画待ちが残るため、次の非エンコーダー入力まで
// 最新カウントだけを保持し、差分を一度に適用する。加速度ではなく実移動量そのもの。
static bool pending_encoder_value[3] = { false, false, false };
static uint32_t pending_encoder_count[3] = { 0, 0, 0 };

static void queue_encoder_value(uint8_t encoder, uint32_t value)
{
  if (encoder >= 3) { return; }
  pending_encoder_count[encoder] = value;
  pending_encoder_value[encoder] = true;
}

static void flush_encoder_values(void)
{
  for (uint8_t encoder = 0; encoder < 3; ++encoder) {
    if (!pending_encoder_value[encoder]) { continue; }
    pending_encoder_value[encoder] = false;
    process_encoder_value(encoder, pending_encoder_count[encoder]);
  }
}

static void handle_fn_button(int fn, bool press)
{
  if (fn < 0 || fn >= 3) { return; }
  fn_pressed[fn] = press;
  if (press) { fn_press_msec[fn] = performance_event_time(); }
  // Fn is normally a modifier. Show its role on the button edge without
  // adding a continuous drawing cost while the player is performing.
  if (current_mode == sampler_mode_t::mode_rec ||
      current_mode == sampler_mode_t::mode_play ||
      current_mode == sampler_mode_t::mode_loop ||
      current_mode == sampler_mode_t::mode_fx) {
    request_wave_draw();
  }
  if (edit_pad >= 0) {
    if (!press) {
      // Chop preview is a one-shot slice audition. Releasing PLAY must not
      // route through the normal Hold/Repeat preview transport.
      if (fn == 0 && !edit_chop_page) { preview_edit_transport(false); }
      request_fn_draw(fn);
      return;
    }
    if (edit_chop_page && fn == 0) {
      if (!preview_next_chop_slice()) {
        show_edit_notice(edit_chop_fit_mode == chop_fit_mode_t::fit_bgm
          ? edit_notice_t::chop_needs_bgm : edit_notice_t::none,
          edit_notice_duration_msec);
      }
    } else if (edit_chop_page && fn == 1) {
      if (!chop_edit_sample()) {
        processing_screen_visible = false;
        show_status_message(edit_chop_fit_mode == chop_fit_mode_t::fit_bgm
          && !background_loop.isValid() && !loop_length_fixed
            ? "NOTHING TO FIT" : "CHOP FAILED: NO MEMORY", 1800, false);
        draw_all();
        update_all_leds();
      }
    } else if (edit_chop_page) {
      sampler_audio_t::stop((uint8_t)edit_pad);
      edit_chop_page = false;
      reset_chop_preview();
      edit_notice = edit_notice_t::none;
      edit_notice_until_msec = 0;
      request_edit_target_draw();
    } else if (fn == 0) {
      if (!stop_edit_preview_if_playing()) { preview_edit_transport(true); }
    } else if (fn == 1) {
      commit_edit();
    } else {
      exit_edit();
    }
    request_all_fn_draw();
    return;
  }
  if (current_mode == sampler_mode_t::mode_play && fn == 2
   && (current_page == performance_page_t::melody || current_page == performance_page_t::bass)) {
    set_touch_play_active(press);
    return;
  }
  if (current_mode == sampler_mode_t::mode_rec
   && current_page == performance_page_t::sample && fn == 2) {
    if (!press) { cancel_sample_delete_confirm(); }
    request_fn_draw(fn);
    return;
  }
  if (current_mode == sampler_mode_t::mode_fx && fn == 2) {
    if (press) { mixer_set_active(!mixer_active); }
    request_fn_draw(fn);
    return;
  }
  if (common_fn_mode()) {
    if (fn == 0) {
      // A short press remains Play/Stop. Holding while stopped arms the
      // performance WAV recorder, so defer the short action until release.
      // The meter itself is delayed in service_performance_recording(): a
      // normal tap must remain visually silent and feel like a plain Play.
      if (press) {
        performance_record_fn_consumed = false;
      } else {
        cancel_hold_progress(hold_progress_kind_t::performance_record);
      }
      if (!press) {
        if (!performance_record_fn_consumed) { loop_toggle_play(); }
        performance_record_fn_consumed = false;
      }
    } else if (press && fn == 1
            && (current_page == performance_page_t::melody
             || current_page == performance_page_t::bass
             || current_page == performance_page_t::chord)) {
      toggle_current_page_mute();
    }
    request_fn_draw(fn);
    return;
  }
  if (press && current_mode == sampler_mode_t::mode_loop) {
    if (fn == 0) {
      loop_handle_top_button();
    } else if (fn == 1) {
      if (current_page == performance_page_t::melody
       || current_page == performance_page_t::bass
       || current_page == performance_page_t::chord) {
        toggle_current_page_mute();
      }
      request_wave_draw();
    } else if (fn == 2) {
      // Apply Delete only on release. A pad touched while it is held is a
      // deliberate part deletion and must never also consume an Undo step.
      loop_del_touched_pad = false;
      begin_hold_progress(hold_progress_kind_t::loop_clear, fn_press_msec[fn],
                          loop_del_long_press_ms, 0xFF7060u,
                          "HOLD TO CLEAR", "RELEASE TO CLEAR");
    }
  } else if (!press && current_mode == sampler_mode_t::mode_loop && fn == 2) {
    uint32_t held = performance_event_time() - fn_press_msec[fn];
    if (!loop_del_touched_pad && held >= loop_del_long_press_ms) {
      // Long press clears only the part currently displayed. The transport
      // and BGM continue even while loop playback is running.
      loop_clear_page_events(current_page);
    } else if (!loop_del_touched_pad) {
      loop_undo_current_page();
    }
    if (!loop_del_touched_pad) {
      cancel_hold_progress(hold_progress_kind_t::loop_clear);
      request_wave_draw();
      request_all_fn_draw();
    }
  }
  request_fn_draw(fn);
}

static void process_bitmask(uint32_t bitmask, uint32_t event_msec) {
  // A menu command can close the menu on its press edge. Its later release
  // must still belong to the menu; otherwise the shared Fn button can execute
  // its performance action (notably Loop Delete) after the menu has vanished.
  static uint32_t menu_consumed_release_mask = 0;
  struct input_time_scope_t {
    uint32_t previous;
    explicit input_time_scope_t(uint32_t value) : previous(input_event_msec) {
      input_event_msec = value;
    }
    ~input_time_scope_t() { input_event_msec = previous; }
  } input_time_scope(event_msec);
  namespace bb = kp::def::button_bitmask;
  uint32_t pressed_edge  = bitmask & ~prev_bitmask;
  uint32_t released_edge = ~bitmask & prev_bitmask;
  prev_bitmask = bitmask;
  const uint32_t menu_consumed_releases = released_edge & menu_consumed_release_mask;
  menu_consumed_release_mask &= ~released_edge;
  released_edge &= ~menu_consumed_releases;

  if (wifi_update_active) {
    // 接続待ちだけはBack/Exitで中断可能。OTA開始後は入力をすべて無視する。
    if (pressed_edge & ((1u << 4) | (1u << 8) | bb::SIDE_2)) { cancel_wifi_update(); }
    return;
  }
  if (wifi_file_server_qr_active) {
    // 上側エンコーダー押込みは全音停止、下側エンコーダーは無視する。
    // それ以外の本体ボタンを押した時だけFile Editorを終了する。
    if (pressed_edge & bb::ENC1_PUSH) { stop_all_audio(); }
    const uint32_t encoder_pushes = bb::ENC1_PUSH | bb::ENC2_PUSH;
    if (pressed_edge & ~encoder_pushes) { stop_file_server_session(); }
    return;
  }

  // This press confirms the page shown in the jog selector. Handle it before
  // ENC2's normal menu shortcut so no menu is opened accidentally.
  if (page_selector_visible && (pressed_edge & bb::ENC2_PUSH)) {
    page_selector_confirm();
    return;
  }

  const bool menu_was_visible = menu_visible;
  if (menu_handle_input(pressed_edge)) {
    if (menu_was_visible) { menu_consumed_release_mask |= pressed_edge; }
    return;
  }
  if (learn_capture_target(pressed_edge)) { return; }

  if (touch_play_active) {
    // Touch Play owns the performance surface. Only the Fn3 release that
    // opened it is meaningful; physical pads and mode buttons stay inert.
    // Enc1 remains the global volume/stop control, and the lever keeps the
    // Melody/Bass semitone bend available while the touch surface is held.
    for (int btn = 0; btn < 15; ++btn) {
      const uint32_t mask = 1u << btn;
      if ((pressed_edge | released_edge) & mask) {
        if (button_to_pad(btn) == -1 && button_to_fn(btn) == 2) {
          handle_fn_button(2, (pressed_edge & mask) != 0);
        }
      }
    }
    if (current_page == performance_page_t::melody || current_page == performance_page_t::bass) {
      if (pressed_edge & bb::KNOB_L) { set_melody_pitch_bend_lever(true, true); }
      if (pressed_edge & bb::KNOB_R) { set_melody_pitch_bend_lever(false, true); }
      if (released_edge & bb::KNOB_L) { set_melody_pitch_bend_lever(true, false); }
      if (released_edge & bb::KNOB_R) { set_melody_pitch_bend_lever(false, false); }
    }
    if (pressed_edge & bb::ENC1_PUSH) { stop_all_audio(); }
    return;
  }

  // A performance gesture confirms the highlighted part, then continues
  // through this same input event. Audio therefore reacts immediately while
  // the retained information surface is restored later in the UI phase.
  if (page_selector_visible && pressed_edge) {
    page_selector_confirm(true);
  }

  if (released_edge & bb::SIDE_1) {
    move_performance_page(-1);
    return;
  }
  if (released_edge & bb::SIDE_2) {
    move_performance_page(1);
    return;
  }

  // FX lever scratches the cached final mix. The dry transport continues, so
  // this works for BGM, Samples and both Pad/GM synth sources without seeking.
  const bool bgm_scratch_context = current_mode == sampler_mode_t::mode_fx
                                && sampler_audio_t::masterScratchAvailable()
                                && !loop_repeat_armed && !loop_repeat_running;
  if (current_mode == sampler_mode_t::mode_fx) {
    // FX reserves the lever exclusively for Master Scratch. When its buffer
    // is unavailable, swallow the edges instead of falling
    // through to Sample Repeat or Melody/Bass pitch bend.
    if (bgm_scratch_context) {
      if (pressed_edge & bb::KNOB_L) { set_bgm_scratch_lever(1, true); }
      if (pressed_edge & bb::KNOB_R) { set_bgm_scratch_lever(-1, true); }
      if (released_edge & bb::KNOB_L) { set_bgm_scratch_lever(1, false); }
      if (released_edge & bb::KNOB_R) { set_bgm_scratch_lever(-1, false); }
    }
  // Melody/Bass page: lever is a one-semitone pitch bend. Other pages preserve
  // the existing Repeat behaviour (down=1 grid, up=0.5 grid).
  } else if (current_page == performance_page_t::melody
   || current_page == performance_page_t::bass) {
    if (pressed_edge & bb::KNOB_L) { set_melody_pitch_bend_lever(true, true); }
    if (pressed_edge & bb::KNOB_R) { set_melody_pitch_bend_lever(false, true); }
    if (released_edge & bb::KNOB_L) { set_melody_pitch_bend_lever(true, false); }
    if (released_edge & bb::KNOB_R) { set_melody_pitch_bend_lever(false, false); }
  } else {
    if (pressed_edge & bb::KNOB_L) {
      pad_repeat_lever_mask = bb::KNOB_L;
      set_pad_repeat_mode(pad_repeat_mode_t::grid);
    }
    if (pressed_edge & bb::KNOB_R) {
      pad_repeat_lever_mask = bb::KNOB_R;
      set_pad_repeat_mode(pad_repeat_mode_t::half_grid);
    }
    if ((released_edge & bb::KNOB_L) && pad_repeat_mode == pad_repeat_mode_t::grid) {
      pad_repeat_release_confirm_msec = M5.millis() + pad_repeat_lever_release_debounce_msec;
    }
    if ((released_edge & bb::KNOB_R) && pad_repeat_mode == pad_repeat_mode_t::half_grid) {
      pad_repeat_release_confirm_msec = M5.millis() + pad_repeat_lever_release_debounce_msec;
    }
  }

  // メイン15ボタン (Pad 4x3 + Fn列)
  for (int btn = 0; btn < 15; ++btn) {
    uint32_t mask = 1u << btn;
    if (0 == ((pressed_edge | released_edge) & mask)) { continue; }
    bool press = pressed_edge & mask;
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      press ? pad_press(pad) : pad_release(pad);
    } else if (pad == -1) {
      handle_fn_button(button_to_fn(btn), press);
    }
  }

  // 上段4ボタン: モード切替 (REC/PLAY/LOOP/FX)
  for (int i = 0; i < (int)sampler_mode_t::mode_max; ++i) {
    if (pressed_edge & (bb::SUB_1 << i)) {
      set_mode((sampler_mode_t)i);
    }
  }

  if (pressed_edge & bb::ENC1_PUSH) { stop_all_audio(); }

  if (edit_pad < 0 && current_mode == sampler_mode_t::mode_fx && !mixer_active) {
    if (pressed_edge & bb::ENC2_PUSH) { fx_select_next(); }
  }
}

static void process_touch(uint32_t value) {
  bool pressed = value & 1;
  int x = ((int16_t)(value & 0xFFFF)) >> 1;
  int y = ((int16_t)(value >> 16)) >> 1;

  if (wifi_update_active) { return; }
  if (wifi_file_server_qr_active) {
    if (pressed) { stop_file_server_session(); }
    return;
  }

  if (touch_play_active) {
    handle_touch_play(x, y, pressed);
    return;
  }

  if (!pressed) {
    if (touch_pad >= 0) { pad_release(touch_pad); touch_pad = -1; }
    return;
  }

  // モードタブのタップ
  if (y >= tab_y && y < tab_y + tab_h) {
    // メニュー中は上部の4モードタブを操作対象にしない。メニューの
    // 階層表示を開いたまま、誤って演奏ページへ移動するのを防ぐ。
    if (menu_visible) { return; }
    int tab = x / (M5.Display.width() / (int)sampler_mode_t::mode_max);
    if (tab >= 0 && tab < (int)sampler_mode_t::mode_max) {
      set_mode((sampler_mode_t)tab);
    }
    return;
  }

  // Padエリアのタッチ演奏 (シングルタッチのみ)
  if (y >= grid_y && x < fn_x - 2) {
    int col = (x - grid_x) / col_pitch;
    int row = (y - grid_y) / row_pitch;
    if (col >= 0 && col < 4 && row >= 0 && row < 3) {
      int pad = row * 4 + col;
      if (pad != touch_pad) {
        if (touch_pad >= 0) { pad_release(touch_pad); }
        touch_pad = pad;
        pad_press(pad);
      }
      return;
    }
  }
  if (touch_pad >= 0) { pad_release(touch_pad); touch_pad = -1; }
}

//-------------------------------------------------------------------------
// サンプルの読み込み (SDカードのWAV/MP3 → PSRAMプール)

static uint8_t* temp_alloc(size_t bytes) {
#if defined (M5UNIFIED_PC_BUILD)
  return (uint8_t*)malloc(bytes);
#else
  return (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
}

static int16_t* bgm_alloc(size_t bytes) {
#if defined (M5UNIFIED_PC_BUILD)
  return (int16_t*)malloc(bytes);
#else
  return (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
}

static void clear_menu_preview(void)
{
  sampler_audio_t::stop(menu_preview_voice);
  if (synth_menu_preview_note_active) {
    send_sam_midi(0x80 | synth_menu_preview_channel, synth_menu_preview_note, 0);
  }
  synth_menu_preview_note_active = false;
  synth_menu_preview_sample_active = false;
  synth_menu_preview_stop_msec = 0;
  if (menu_preview_pcm) {
    // I2S側が停止フラグを確認してから解放する。DMA 1ブロックより短いと
    // 直前のPCMを参照したまま解放され、次回のプレビューが不安定になる。
    M5.delay(6);
    free(menu_preview_pcm);
  }
  menu_preview_pcm = nullptr;
  menu_preview_frames = 0;
  menu_preview_sample_rate = 44100;
}

static void service_synth_menu_preview(uint32_t now)
{
  if (synth_menu_preview_stop_msec == 0
   || (int32_t)(now - synth_menu_preview_stop_msec) < 0) { return; }
  if (synth_menu_preview_note_active) {
    send_sam_midi(0x80 | synth_menu_preview_channel, synth_menu_preview_note, 0);
  }
  if (synth_menu_preview_sample_active) { sampler_audio_t::stop(menu_preview_voice); }
  synth_menu_preview_note_active = false;
  synth_menu_preview_sample_active = false;
  synth_menu_preview_stop_msec = 0;
}

static void preview_synth_menu_selection(void)
{
  const uint32_t preview_ms = 450;
  const bool bass_preview = synth_sound_select_active
                         && synth_menu_target == performance_page_t::bass;
  if (kit_edit_state == kit_edit_state_t::select_external_tone) {
    const uint8_t program = (uint8_t)std::min<uint16_t>(menu_cursor, 127);
    clear_menu_preview();
    send_sam_midi(0xC0 | synth_menu_preview_channel, program);
    send_sam_midi(0xB0 | synth_menu_preview_channel, 7, 110);
    send_sam_midi(0xB0 | synth_menu_preview_channel, 10, 64);
    synth_menu_preview_note = bass_preview ? 36 : 60;
    send_sam_midi(0x90 | synth_menu_preview_channel, synth_menu_preview_note, 108);
    synth_menu_preview_note_active = true;
    synth_menu_preview_stop_msec = M5.millis() + preview_ms;
    return;
  }
  if (kit_edit_state != kit_edit_state_t::select_external_pad
   || menu_cursor >= def::pad::pad_count) { return; }

  const auto& slot = sampler_pool_t::slot[display_order_to_pad(menu_cursor)];
  if (!slot.isValid()) { return; }
  clear_menu_preview();
  const uint32_t start = slot.reverse && slot.playEnd() > 0
    ? slot.playEnd() - 1 : slot.playStart();
  if (sampler_audio_t::play(menu_preview_voice, slot.pcm, slot.frames, slot.sample_rate,
                            false, slot.reverse, slot.volume_q8,
                            bass_preview ? 64 : 256, start)) {
    synth_menu_preview_sample_active = true;
    synth_menu_preview_stop_msec = M5.millis() + preview_ms;
  }
}

static bool decode_menu_wav_preview(const uint8_t* wav, size_t wav_size, uint32_t max_ms)
{
  wav_info_t info;
  if (!wav || wav_size <= 44 || max_ms == 0 || !parse_wav(wav, wav_size, &info)) { return false; }
  uint32_t preview_frames = std::min<uint32_t>(info.frames, ((uint64_t)info.sample_rate * max_ms) / 1000);
  if (preview_frames == 0) { return false; }
  int16_t* pcm = bgm_alloc((size_t)preview_frames * sizeof(int16_t));
  if (!pcm) { return false; }
  for (uint32_t i = 0; i < preview_frames; ++i) {
    pcm[i] = wav_mono_frame(info, i);
  }
  menu_preview_pcm = pcm;
  menu_preview_frames = preview_frames;
  menu_preview_sample_rate = info.sample_rate;
  if (sampler_audio_t::play(menu_preview_voice, menu_preview_pcm, menu_preview_frames,
                            menu_preview_sample_rate, false, false, 224, 256)) {
    return true;
  }
  clear_menu_preview();
  return false;
}

static bool decode_menu_mp3_preview(const uint8_t* data, size_t size, uint32_t max_ms)
{
  int16_t* pcm = nullptr;
  uint32_t frames = 0;
  uint32_t max_frames = (uint32_t)(((uint64_t)sampler_audio_t::sample_rate * max_ms) / 1000);
  if (decode_mp3_mono_48k(data, size, max_frames, true, &pcm, &frames) != mp3_decode_result_t::ok) {
    return false;
  }
  menu_preview_pcm = pcm;
  menu_preview_frames = frames;
  menu_preview_sample_rate = sampler_audio_t::sample_rate;
  if (sampler_audio_t::play(menu_preview_voice, menu_preview_pcm, menu_preview_frames,
                            menu_preview_sample_rate, false, false, 224, 256)) {
    return true;
  }
  clear_menu_preview();
  return false;
}

static bool play_menu_audio_preview(const char* path, uint32_t max_ms)
{
  static constexpr const size_t max_audio_file_size = 3200 * 1024;
  // プレビューは常に単発。先に前回のPCMを返しておかないと、大きな音源の
  // 一時バッファと新しいプレビューPCMを同時に確保してメモリ不足になり得る。
  clear_menu_preview();
  if (!path || !path[0] || max_ms == 0 || !kp::storage_sd.beginStorage()) { return false; }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 4 || (size_t)size > max_audio_file_size) { return false; }
  uint8_t* tmp = temp_alloc((size_t)size);
  if (!tmp) { return false; }
  int len = kp::storage_sd.loadFromFileToMemory(path, tmp, (size_t)size);
  bool result = false;
  if (len > 4) {
    result = has_lower_suffix(path, ".mp3")
      ? decode_menu_mp3_preview(tmp, (size_t)len, max_ms)
      : decode_menu_wav_preview(tmp, (size_t)len, max_ms);
  }
  free(tmp);
  return result;
}

static const char* canonical_builtin_sample_name(const char* name)
{
  if (!name) { return ""; }
  if (strncmp(name, "builtin:", 8) == 0) { name += 8; }
  // 0.1.6以前の保存Kitに残る旧組み込みIDも復元する。
  if (strcmp(name, "COW") == 0) { return "COWBELL"; }
  if (strcmp(name, "TOML") == 0) { return "TOM"; }
  return name;
}

static const background_source_t* find_builtin_background_loop(const char* builtin_id)
{
  const char* id = builtin_id ? builtin_id : "";
  if (strncmp(id, "builtin:", 8) == 0) { id += 8; }
  // Compatibility with pre-multi-BGM saved kits.
  if (!id[0] || strcmp(id, "BGM_FA.wav") == 0 || strcmp(id, "BGM_HOUSE") == 0) {
    return &builtin_background_loops[0];
  }
  for (const auto& source : builtin_background_loops) {
    if (strcmp(id, source.file) == 0 || strcmp(id, source.source.name) == 0) {
      return &source;
    }
  }
  return nullptr;
}

static bool play_menu_builtin_preview(const char* builtin_id, uint32_t max_ms)
{
  clear_menu_preview();
  if (!builtin_id || !builtin_id[0]) { return false; }
  const char* name = canonical_builtin_sample_name(builtin_id);
  for (const auto& src : builtin_samples) {
    if (strcmp(name, src.name) == 0) {
      return decode_menu_wav_preview(src.data, src.size(), max_ms);
    }
  }
  return false;
}

static void set_error_text(char* error, size_t error_len, const char* msg)
{
  if (error && error_len) { snprintf(error, error_len, "%s", msg ? msg : "Error"); }
}

static bool load_audio_memory_to_pad(uint8_t pad, const char* source_path, const char* display_name,
                                     const uint8_t* data, size_t size, mp3_decode_result_t* mp3_result = nullptr)
{
  if (mp3_result) { *mp3_result = mp3_decode_result_t::ok; }
  prepare_pad_for_new_sample(pad);
  sampler_pool_t::setProgressCallback(draw_busy_status_dots_tick);
  if (has_lower_suffix(source_path ? source_path : "", ".mp3")) {
    int16_t* pcm = nullptr;
    uint32_t frames = 0;
    mp3_decode_result_t result = decode_mp3_mono_48k(data, size,
      sampler_pool_t::max_sample_sec * sampler_audio_t::sample_rate, true, &pcm, &frames);
    if (mp3_result) { *mp3_result = result; }
    bool ok = result == mp3_decode_result_t::ok
      && sampler_pool_t::loadPcmOwned(pad, display_name ? display_name : "", pcm, frames, sampler_audio_t::sample_rate);
    if (!ok && pcm) { free(pcm); }
    sampler_pool_t::setProgressCallback(nullptr);
    return ok;
  }
  bool ok = sampler_pool_t::loadWav(pad, display_name ? display_name : "", data, size);
  sampler_pool_t::setProgressCallback(nullptr);
  return ok;
}

static bool load_audio_to_pad(uint8_t pad, const char* path, const char* display_name, char* error, size_t error_len)
{
  static constexpr const size_t max_audio_file_size = 3200 * 1024;
  if (pad >= def::pad::pad_count || !path || !path[0]) {
    set_error_text(error, error_len, "Bad pad");
    return false;
  }
  if (!kp::storage_sd.beginStorage()) {
    set_error_text(error, error_len, "No SD");
    return false;
  }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 4) {
    set_error_text(error, error_len, "Empty audio");
    return false;
  }
  if ((size_t)size > max_audio_file_size) {
    set_error_text(error, error_len, "Audio too big");
    return false;
  }
  uint8_t* tmp = temp_alloc((size_t)size);
  if (!tmp) {
    set_error_text(error, error_len, "No memory");
    return false;
  }
  int len = kp::storage_sd.loadFromFileToMemory(path, tmp, (size_t)size);
  if (len <= 4) {
    free(tmp);
    set_error_text(error, error_len, "Read failed");
    return false;
  }

  mp3_decode_result_t mp3_result = mp3_decode_result_t::ok;
  bool ok = load_audio_memory_to_pad(pad, path, display_name, tmp, (size_t)len, &mp3_result);
  free(tmp);
  if (!ok) {
    if (!sampler_pool_t::slot[pad].isValid()) {
      loop_remove_pad_events(pad);
      loop_reset_recording_state_if_empty();
      draw_header();
      draw_wave();
      update_pad_led(pad);
      draw_pad(pad);
    }
    set_error_text(error, error_len,
      mp3_result == mp3_decode_result_t::no_memory ? "No MP3 memory" : "Bad audio");
    return false;
  }

  loop_remove_pad_events(pad);
  loop_reset_recording_state_if_empty();
  auto& slot = sampler_pool_t::slot[pad];
  snprintf(slot.file_path, sizeof(slot.file_path), "%s", path);
  rec_wave_pad = pad;
  if (edit_pad == pad) { edit_pad = -1; }
  draw_header();
  draw_wave();
  update_pad_led(pad);
  draw_pad(pad);
  set_error_text(error, error_len, "");
  return true;
}

static void clear_pad_sample(uint8_t pad, bool remove_loop_events)
{
  if (pad >= def::pad::pad_count) { return; }
  sampler_audio_t::stop(pad);
  stop_sample_grid_loop(pad);
  clear_sampler_sustain_cache_for_pad(pad);
  if (remove_loop_events) {
    loop_remove_pad_events(pad);
    loop_reset_recording_state_if_empty();
  }
  if (rec_wave_pad == (int)pad) { rec_wave_pad = -1; }
  if (edit_pad == (int)pad) { edit_pad = -1; }
  pads[pad].pressed = false;
  pads[pad].playing_shown = false;
  sampler_pool_t::erase(pad);
  draw_header();
  draw_wave();
  update_pad_led(pad);
  draw_pad(pad);
}

static void clear_all_pad_samples(void)
{
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    sampler_audio_t::stop(pad);
    stop_sample_grid_loop(pad);
    clear_sampler_sustain_cache_for_pad(pad);
    pads[pad].pressed = false;
    pads[pad].playing_shown = false;
    sampler_pool_t::erase(pad);
  }
  rec_wave_pad = -1;
  edit_pad = -1;
  recording_pad = -1;
  {
    loop_events_guard_t guard;
    loop_events.clear();
  }
  for (auto& history : loop_undo_history) { history.clear(); }
  memset(loop_pad_mute, 0, sizeof(loop_pad_mute));
  memset(loop_active_layer, 0, sizeof(loop_active_layer));
  memset(loop_deferred_note_on_layer, 0, sizeof(loop_deferred_note_on_layer));
  memset(loop_live_min_gate_until, 0, sizeof(loop_live_min_gate_until));
  memset(loop_live_release_pending, 0, sizeof(loop_live_release_pending));
  memset(loop_live_release_layer, 0, sizeof(loop_live_release_layer));
  memset(loop_deferred_live_pad, 0, sizeof(loop_deferred_live_pad));
  memset(synth_loop_active_layer, 0, sizeof(synth_loop_active_layer));
  memset(synth_deferred_note_on_layer, 0, sizeof(synth_deferred_note_on_layer));
  memset(synth_live_min_gate_until, 0, sizeof(synth_live_min_gate_until));
  memset(synth_live_release_pending, 0, sizeof(synth_live_release_pending));
  memset(synth_live_release_layer, 0, sizeof(synth_live_release_layer));
  memset(synth_sounding_layer, 0, sizeof(synth_sounding_layer));
  loop_layer_seq = 1;
  loop_capture_zero_until_msec = 0;
  if (background_loop.isValid()) {
    loop_length_fixed = true;
    loop_length_msec = background_loop_length_ms();
  } else {
    loop_length_fixed = false;
    loop_length_msec = loop_default_length_ms;
  }
  draw_header();
  draw_wave();
  update_all_leds();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { draw_pad(i); }
}

static void set_background_loop_error(const char* msg)
{
  snprintf(background_loop_error, sizeof(background_loop_error), "%s", msg ? msg : "BGM error");
}

static void stop_background_loop(void)
{
  sampler_audio_t::stop(background_loop_voice);
}

static void play_background_loop_at(uint32_t pos_ms)
{
  if (!background_loop.isValid()) { return; }
  uint32_t start_frame = ((uint64_t)pos_ms * background_loop.sample_rate) / 1000;
  if (background_loop.frames) { start_frame %= background_loop.frames; }
  sampler_audio_t::play(background_loop_voice, background_loop.pcm, background_loop.frames,
                        background_loop.sample_rate, true, false,
                        mixer_scaled_volume_q8(mixer_part_t::bgm, background_loop.volume_q8),
                        256, start_frame);
}

static void clear_background_loop(void)
{
  stop_background_loop();
  if (background_loop.pcm) {
    M5.delay(8);
    free(background_loop.pcm);
  }
  background_loop.pcm = nullptr;
  background_loop.frames = 0;
  background_loop.sample_rate = sampler_audio_t::sample_rate;
  background_loop.volume_q8 = volume_q8_from_20_percent_step(4);
  background_loop.loop_repeats = 2;
  background_loop.name[0] = 0;
  background_loop.file_path[0] = 0;
}

static void install_background_loop_pcm(int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                                        const char* display_name, const char* file_path,
                                        uint8_t loop_repeats)
{
  clear_background_loop();
  background_loop.pcm = pcm;
  background_loop.frames = frames;
  background_loop.sample_rate = sample_rate;
  background_loop.loop_repeats = loop_repeats;
  snprintf(background_loop.name, sizeof(background_loop.name), "%s", display_name ? display_name : "BGM");
  snprintf(background_loop.file_path, sizeof(background_loop.file_path), "%s", file_path ? file_path : "");

  {
    loop_events_guard_t guard;
    loop_events.clear();
  }
  for (auto& history : loop_undo_history) { history.clear(); }
  memset(loop_pad_mute, 0, sizeof(loop_pad_mute));
  memset(loop_active_layer, 0, sizeof(loop_active_layer));
  memset(loop_deferred_note_on_layer, 0, sizeof(loop_deferred_note_on_layer));
  memset(loop_live_min_gate_until, 0, sizeof(loop_live_min_gate_until));
  memset(loop_live_release_pending, 0, sizeof(loop_live_release_pending));
  memset(loop_live_release_layer, 0, sizeof(loop_live_release_layer));
  memset(loop_deferred_live_pad, 0, sizeof(loop_deferred_live_pad));
  memset(synth_loop_active_layer, 0, sizeof(synth_loop_active_layer));
  memset(synth_deferred_note_on_layer, 0, sizeof(synth_deferred_note_on_layer));
  memset(synth_live_min_gate_until, 0, sizeof(synth_live_min_gate_until));
  memset(synth_live_release_pending, 0, sizeof(synth_live_release_pending));
  memset(synth_live_release_layer, 0, sizeof(synth_live_release_layer));
  memset(synth_sounding_layer, 0, sizeof(synth_sounding_layer));
  loop_layer_seq = 1;
  loop_length_msec = background_loop_length_ms();
  loop_length_fixed = true;
  loop_playing = false;
  loop_prev_pos_ms = 0;
  loop_start_msec = M5.millis();
  loop_capture_zero_until_msec = 0;
  loop_record_enabled = true;
  auto_configure_loop_grid(loop_length_msec);
  clear_sample_grid_loops();
  if (!startup_loading_active) {
    draw_wave();
    for (int i = 0; i < 3; ++i) { draw_fn(i); }
  }
  set_background_loop_error("");
}

static bool load_background_loop_memory(const uint8_t* data, size_t len, const char* display_name, const char* file_path, uint8_t loop_repeats)
{
  set_background_loop_error("");
  if (!data || len <= 44) {
    set_background_loop_error("Empty BGM");
    return false;
  }
  wav_info_t info;
  if (!parse_wav(data, (size_t)len, &info)) {
    set_background_loop_error("Bad BGM WAV");
    return false;
  }
  const uint32_t target_rate = info.sample_rate == 44100 ? sampler_audio_t::sample_rate : info.sample_rate;
  uint32_t frames = resampled_frame_count(info.frames, info.sample_rate, target_rate);
  if (frames < target_rate / 2) {
    set_background_loop_error("BGM too short");
    return false;
  }
  if (loop_repeats < 1) { loop_repeats = 1; }
  uint32_t max_frames = target_rate * background_loop_max_sec;
  if ((uint64_t)frames * loop_repeats > max_frames) {
    set_background_loop_error("BGM too long");
    return false;
  }
  size_t bytes = (size_t)frames * sizeof(int16_t);
  int16_t* pcm = bgm_alloc(bytes);
  if (!pcm && background_loop.pcm) {
    clear_background_loop();
    pcm = bgm_alloc(bytes);
  }
  if (!pcm) {
    set_background_loop_error("No BGM memory");
    return false;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    pcm[i] = wav_resampled_mono_frame(info, i, target_rate);
    if ((i & 0x07FFu) == 0) { draw_busy_status_dots_tick(); }
  }

  install_background_loop_pcm(pcm, frames, target_rate, display_name, file_path, loop_repeats);
  return true;
}

static bool load_background_loop_file(const char* path, const char* display_name)
{
  set_background_loop_error("");
  if (!path) {
    set_background_loop_error("No BGM path");
    return false;
  }
  if (!kp::storage_sd.beginStorage()) {
    set_background_loop_error("No SD");
    return false;
  }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 4) {
    set_background_loop_error("Empty BGM");
    return false;
  }
  if ((size_t)size > background_loop_max_wav_file_size) {
    set_background_loop_error("BGM file too big");
    return false;
  }
  uint8_t* data = temp_alloc((size_t)size);
  if (!data) {
    set_background_loop_error("No temp memory");
    return false;
  }
  int len = kp::storage_sd.loadFromFileToMemory(path, data, (size_t)size);
  if (len <= 4) {
    free(data);
    set_background_loop_error("BGM read failed");
    return false;
  }
  bool ok = false;
  if (has_lower_suffix(path, ".mp3")) {
    int16_t* pcm = nullptr;
    uint32_t frames = 0;
    mp3_decode_result_t result = decode_mp3_mono_48k(data, (size_t)len,
      sampler_audio_t::sample_rate * background_loop_max_sec, false, &pcm, &frames);
    if (result == mp3_decode_result_t::no_memory && background_loop.pcm) {
      clear_background_loop();
      result = decode_mp3_mono_48k(data, (size_t)len,
        sampler_audio_t::sample_rate * background_loop_max_sec, false, &pcm, &frames);
    }
    if (result == mp3_decode_result_t::ok && frames >= sampler_audio_t::sample_rate / 2) {
      install_background_loop_pcm(pcm, frames, sampler_audio_t::sample_rate, display_name, path, 1);
      ok = true;
    } else {
      free(pcm);
      set_background_loop_error(result == mp3_decode_result_t::too_long ? "BGM too long"
                              : result == mp3_decode_result_t::no_memory ? "No BGM memory"
                              : "Bad BGM MP3");
    }
  } else {
    ok = load_background_loop_memory(data, (size_t)len, display_name, path, 1);
  }
  free(data);
  return ok;
}

static bool ensure_sampler_sd_dirs(void)
{
  if (!kp::storage_sd.beginStorage()) { return false; }
  kp::storage_sd.makeDirectory("/sampler");
  kp::storage_sd.makeDirectory("/sampler/samples");
  kp::storage_sd.makeDirectory("/sampler/loops");
  kp::storage_sd.makeDirectory("/sampler/recordings");
  kp::storage_sd.makeDirectory("/sampler/kits");
  kp::storage_sd.makeDirectory(sampler_default_kit_dir);
  kp::storage_sd.makeDirectory(sampler_session_dir);
  return true;
}

static void reset_sampler_sd_folder_selection(void)
{
  static constexpr const char* roots[] = {
    "/sampler/samples", "/sampler/loops", "/sampler/kits"
  };
  for (size_t i = 0; i < 3; ++i) {
    snprintf(sampler_sd_folders[i], sizeof(sampler_sd_folders[i]), "%s", roots[i]);
  }
  save_sampler_folder_settings();
}

static void load_sampler_folder_settings(void)
{
  if (!kp::storage_littlefs.beginStorage()) { return; }
  int size = kp::storage_littlefs.getFileSize(sampler_folder_settings_path);
  if (size <= 2 || size > 512) { return; }
  std::vector<uint8_t> data((size_t)size + 1, 0);
  if (kp::storage_littlefs.loadFromFileToMemory(sampler_folder_settings_path, data.data(), (size_t)size) != size) { return; }
  JsonDocument doc;
  if (deserializeJson(doc, data.data(), size)) { return; }
  const char* roots[] = { "/sampler/samples", "/sampler/loops", "/sampler/kits" };
  const char* keys[] = { "samples", "loops", "kits" };
  for (size_t i = 0; i < 3; ++i) {
    const char* value = doc[keys[i]] | roots[i];
    if (strncmp(value, roots[i], strlen(roots[i])) == 0
     && (value[strlen(roots[i])] == 0 || value[strlen(roots[i])] == '/')) {
      snprintf(sampler_sd_folders[i], sizeof(sampler_sd_folders[i]), "%s", value);
    }
  }
  wifi_auto_update_check = doc["autoUpdate"] | false;
  menu_sound_enabled = doc["menuSound"] | true;
}

static void save_sampler_folder_settings(void)
{
  if (!kp::storage_littlefs.beginStorage()) { return; }
  JsonDocument doc;
  doc["samples"] = sampler_sd_folders[0];
  doc["loops"] = sampler_sd_folders[1];
  doc["kits"] = sampler_sd_folders[2];
  doc["autoUpdate"] = wifi_auto_update_check;
  doc["menuSound"] = menu_sound_enabled;
  std::string out;
  serializeJson(doc, out);
  kp::storage_littlefs.saveFromMemoryToFile(sampler_folder_settings_path, (const uint8_t*)out.data(), out.size());
}

static void load_builtin_samples(void)
{
  for (size_t i = 0; i < builtin_default_sample_count && i < def::pad::pad_count; ++i) {
    draw_startup_loading_frame("LOADING PRESET");
    uint8_t pad = display_order_to_pad((uint8_t)i);
    sampler_pool_t::setProgressCallback(draw_busy_status_dots_tick);
    const bool loaded = sampler_pool_t::loadWav(pad, builtin_samples[i].name,
                                                builtin_samples[i].data, builtin_samples[i].size());
    sampler_pool_t::setProgressCallback(nullptr);
    if (loaded) {
      snprintf(sampler_pool_t::slot[pad].file_path, sizeof(sampler_pool_t::slot[pad].file_path),
               "builtin:%s", builtin_samples[i].name);
    }
  }
  load_builtin_background_loop();
}

static bool load_builtin_sample_to_pad(uint8_t pad, const char* builtin_id)
{
  if (pad >= def::pad::pad_count || !builtin_id) { return false; }
  const char* name = canonical_builtin_sample_name(builtin_id);
  for (const auto& src : builtin_samples) {
    if (strcmp(name, src.name) == 0) {
      prepare_pad_for_new_sample(pad);
      sampler_pool_t::setProgressCallback(draw_busy_status_dots_tick);
      const bool loaded = sampler_pool_t::loadWav(pad, src.name, src.data, src.size());
      sampler_pool_t::setProgressCallback(nullptr);
      if (!loaded) { return false; }
      snprintf(sampler_pool_t::slot[pad].file_path, sizeof(sampler_pool_t::slot[pad].file_path),
               "builtin:%s", src.name);
      return true;
    }
  }
  return false;
}

static bool load_builtin_background_loop(const char* builtin_id)
{
  const auto* source = find_builtin_background_loop(builtin_id);
  if (!source) { return false; }
  draw_startup_loading_frame("LOADING BGM");
  return load_background_loop_memory(source->source.data,
                                     source->source.size(),
                                     source->source.name,
                                     (std::string("builtin:") + source->file).c_str(),
                                     2);
}

static int load_sd_samples(void) {
  static constexpr const size_t max_audio_file_size = 3200 * 1024;

  int loaded_count = 0;
  if (ensure_sampler_sd_dirs()) {
    std::vector<kp::file_info_string_t> list;
    kp::storage_sd.getFileList(list, "/sampler/samples", "");

    // WAV / MP3のみ抽出 (大文字小文字を問わない)
    list.erase(std::remove_if(list.begin(), list.end(), [](const kp::file_info_string_t& f) {
      return !is_audio_file_name(f.filename);
    }), list.end());

    // ファイル名の若い順で最大12個を、左下Pad 1から順に割り当てる。
    std::sort(list.begin(), list.end(),
      [](const kp::file_info_string_t& a, const kp::file_info_string_t& b) {
        return a.filename < b.filename;
      });

    for (const auto& f : list) {
      if (loaded_count >= (int)def::pad::pad_count) { break; }
      std::string full = std::string("/sampler/samples/") + f.filename;
      size_t fsize = f.filesize;
      if (fsize == 0) {
        int sz = kp::storage_sd.getFileSize(full.c_str());
        if (sz <= 0) { continue; }
        fsize = sz;
      }
      if (fsize > max_audio_file_size) { continue; }

      uint8_t* tmp = temp_alloc(fsize);
      if (tmp == nullptr) { break; }
      int len = kp::storage_sd.loadFromFileToMemory(full.c_str(), tmp, fsize);
      if (len > 4) {
        std::string name = f.filename.substr(0, f.filename.size() - 4);
        uint8_t pad = display_order_to_pad((uint8_t)loaded_count);
        if (load_audio_memory_to_pad(pad, full.c_str(), name.c_str(), tmp, len)) {
          snprintf(sampler_pool_t::slot[pad].file_path, sizeof(sampler_pool_t::slot[pad].file_path), "%s", full.c_str());
          ++loaded_count;
        }
      }
      free(tmp);
    }
  }
  return loaded_count;
}

static void clear_kit(void)
{
  clear_synth_runtime();
  sampler_audio_t::stopAll();
  clear_sample_grid_loops();
  clear_background_loop();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    sampler_pool_t::erase(i);
    pads[i].pressed = false;
    pads[i].playing_shown = false;
  }
  recording_pad = -1;
  rec_wave_pad = -1;
  edit_pad = -1;
  std::fill(mixer_part_volume, mixer_part_volume + mixer_part_count, 100);
  std::fill(mixer_part_muted, mixer_part_muted + mixer_part_count, false);
  for (auto& snapshot : mixer_snapshot) { snapshot = mixer_snapshot_t{}; }
  mixer_pending_snapshot = -1;
  mixer_applied_snapshot = -1;
  mixer_active = false;
  mixer_held_part = -1;
  loop_reset_recording_state();
  if (!startup_loading_active) {
    draw_all();
    update_all_leds();
  }
}

static void reset_builtin_kit(void)
{
  clear_kit();
  current_kit_path[0] = 0;
  if (menu_visible) { show_loading_message(); }
  load_builtin_samples();
  draw_all();
  update_all_leds();
}

// A saved Default Kit is a user-owned startup point. It is intentionally not
// assigned to current_kit_path, so Reset Kit can never make later Save update
// Default_Kit.json by accident. A bad/missing default always falls back to the
// immutable embedded factory kit.
static bool reset_default_or_builtin_kit(void)
{
  bool loaded_default = false;
  if (ensure_sampler_sd_dirs()
   && kp::storage_sd.getFileSize(sampler_default_kit_path) > 0) {
    loaded_default = load_kit_file(sampler_default_kit_path);
  }
  if (loaded_default) {
    current_kit_path[0] = 0;
    return true;
  }
  reset_builtin_kit();
  return false;
}

static bool save_current_kit(const char* path)
{
  if (!path || !ensure_sampler_sd_dirs()) { return false; }
  return save_kit_to_storage(kp::storage_sd, path);
}

static void write_u16_le(uint8_t* out, uint16_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
}

static void write_u32_le(uint8_t* out, uint32_t value)
{
  out[0] = (uint8_t)value;
  out[1] = (uint8_t)(value >> 8);
  out[2] = (uint8_t)(value >> 16);
  out[3] = (uint8_t)(value >> 24);
}

static bool save_pcm_as_wav(kp::storage_base_t& storage, const char* path,
                            const int16_t* pcm, uint32_t frames, uint32_t sample_rate)
{
  if (!path || !pcm || frames == 0 || sample_rate == 0 || frames > UINT32_MAX / sizeof(int16_t)) {
    return false;
  }

  const uint32_t data_bytes = frames * sizeof(int16_t);
  uint8_t header[44] = {};
  memcpy(header, "RIFF", 4);
  write_u32_le(header + 4, 36 + data_bytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  write_u32_le(header + 16, 16);
  write_u16_le(header + 20, 1);
  write_u16_le(header + 22, 1);
  write_u32_le(header + 24, sample_rate);
  write_u32_le(header + 28, sample_rate * sizeof(int16_t));
  write_u16_le(header + 32, sizeof(int16_t));
  write_u16_le(header + 34, 16);
  memcpy(header + 36, "data", 4);
  write_u32_le(header + 40, data_bytes);
  if (storage.saveFromMemoryToFile(path, header, sizeof(header)) != (int)sizeof(header)) {
    return false;
  }

  // SDへ少量ずつ送ることで、PSRAM上の演奏用PCMを複製しない。
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm);
  size_t remaining = data_bytes;
  size_t next_animation_bytes = 64 * 1024;
  size_t written = 0;
  while (remaining) {
    size_t chunk = std::min<size_t>(remaining, 8192);
    if (storage.appendFromMemoryToFile(path, bytes, chunk) != (int)chunk) {
      return false;
    }
    bytes += chunk;
    remaining -= chunk;
    written += chunk;
    if (processing_screen_visible && written >= next_animation_bytes) {
      draw_recording_processing_frame("SAVING");
      next_animation_bytes += 64 * 1024;
    }
  }
  return true;
}

static bool write_performance_wav_header(const char* path, uint32_t data_bytes)
{
  uint8_t header[44] = {};
  memcpy(header, "RIFF", 4);
  write_u32_le(header + 4, 36 + data_bytes);
  memcpy(header + 8, "WAVEfmt ", 8);
  write_u32_le(header + 16, 16);
  write_u16_le(header + 20, 1);
  write_u16_le(header + 22, 1);
  write_u32_le(header + 24, sampler_audio_t::sample_rate);
  write_u32_le(header + 28, sampler_audio_t::sample_rate * sizeof(int16_t));
  write_u16_le(header + 32, sizeof(int16_t));
  write_u16_le(header + 34, 16);
  memcpy(header + 36, "data", 4);
  write_u32_le(header + 40, data_bytes);
  if (data_bytes == 0) {
    return kp::storage_sd.saveFromMemoryToFile(path, header, sizeof(header)) == (int)sizeof(header);
  }
  return kp::storage_sd.patchFromMemoryToFile(path, 0, header, sizeof(header)) == (int)sizeof(header);
}

static bool next_performance_record_path(char* path, size_t path_len)
{
  if (!path || path_len < 40 || !ensure_sampler_sd_dirs()) { return false; }
  for (uint16_t index = 1; index < 1000; ++index) {
    snprintf(path, path_len, "/sampler/recordings/Performance_%03u.wav", (unsigned)index);
    if (kp::storage_sd.getFileSize(path) <= 0) { return true; }
  }
  return false;
}

static bool performance_record_writer_step(void)
{
  if (!performance_record_active && !performance_record_finishing) { return false; }
  const uint32_t frames = sampler_audio_t::readOutputStreamCapture(
    performance_record_write_buffer, performance_record_write_frames);
  if (frames != 0) {
    const size_t bytes = (size_t)frames * sizeof(int16_t);
    if (kp::storage_sd.appendFromMemoryToFile(performance_record_path,
          reinterpret_cast<const uint8_t*>(performance_record_write_buffer), bytes) != (int)bytes) {
      performance_record_failed = true;
    } else {
      performance_record_data_bytes += bytes;
    }
    return true;
  }
  if (!performance_record_finishing) { return false; }

  const bool overflow = sampler_audio_t::outputStreamCaptureOverflowed();
  const bool ok = !performance_record_failed && !overflow
               && performance_record_data_bytes != 0
               && write_performance_wav_header(performance_record_path, performance_record_data_bytes);
  if (!ok) {
    performance_record_failed = true;
    kp::storage_sd.removeFile(performance_record_path);
  }
  performance_record_active = false;
  performance_record_finishing = false;
  performance_record_done = true;
  return true;
}

#if !defined(M5UNIFIED_PC_BUILD)
static void performance_record_writer_task(void*)
{
  for (;;) {
    const bool wrote = performance_record_writer_step();
    vTaskDelay(pdMS_TO_TICKS(wrote ? 1 : 4));
  }
}
#endif

static bool begin_performance_recording(void)
{
  if (performance_record_active || performance_record_finishing || !ensure_sampler_sd_dirs()
   || !next_performance_record_path(performance_record_path, sizeof(performance_record_path))) { return false; }
  if (performance_record_ring == nullptr) {
#if defined(M5UNIFIED_PC_BUILD)
    performance_record_ring = (int16_t*)malloc((size_t)performance_record_ring_frames * sizeof(int16_t));
    performance_record_write_buffer = (int16_t*)malloc((size_t)performance_record_write_frames * sizeof(int16_t));
#else
    performance_record_ring = (int16_t*)heap_caps_malloc(
      (size_t)performance_record_ring_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    performance_record_write_buffer = (int16_t*)heap_caps_malloc(
      (size_t)performance_record_write_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
#endif
  }
  if (!performance_record_ring || !performance_record_write_buffer
   || !write_performance_wav_header(performance_record_path, 0)) { return false; }
#if !defined(M5UNIFIED_PC_BUILD)
  if (performance_record_writer_task_handle == nullptr) {
    xTaskCreatePinnedToCore(performance_record_writer_task, "perf_rec", 1024 * 3, nullptr,
                            1, &performance_record_writer_task_handle, 0);
  }
  if (performance_record_writer_task_handle == nullptr) { return false; }
#endif
  performance_record_data_bytes = 0;
  performance_record_failed = false;
  performance_record_done = false;
  performance_record_finishing = false;
  if (!sampler_audio_t::startOutputStreamCapture(performance_record_ring, performance_record_ring_frames)) {
    return false;
  }
  performance_record_active = true;
  request_header_draw();
  return true;
}

static void finish_performance_recording(void)
{
  if (!performance_record_active) { return; }
  sampler_audio_t::stopOutputStreamCapture();
  performance_record_finishing = true;
  request_header_draw();
}

static void service_performance_recording(void)
{
#if defined(M5UNIFIED_PC_BUILD)
  performance_record_writer_step();
#endif
  // The record-arm gesture is available wherever Fn1 is the common Play/Stop
  // control. It only runs while stopped, so stopping a loop stays immediate.
  if (edit_pad < 0 && fn_pressed[0] && common_fn_mode() && !loop_playing
   && !performance_record_fn_consumed) {
    const uint32_t held_msec = M5.millis() - fn_press_msec[0];
    if (held_msec >= performance_record_hold_hint_delay_ms
     && hold_progress_kind != hold_progress_kind_t::performance_record) {
      begin_hold_progress(hold_progress_kind_t::performance_record,
                          fn_press_msec[0] + performance_record_hold_hint_delay_ms,
                          performance_record_toggle_hold_ms, 0xF04040u,
                          performance_record_armed ? "HOLD: RECORD OFF" : "HOLD: RECORD MODE",
                          performance_record_armed ? "RECORDING OFF" : "RECORD MODE");
    }
    if (held_msec >= performance_record_hold_hint_delay_ms + performance_record_toggle_hold_ms) {
      performance_record_fn_consumed = true;
      cancel_hold_progress(hold_progress_kind_t::performance_record);
      if (!performance_record_armed && !ensure_sampler_sd_dirs()) {
        show_status_message("NO SD FOR RECORDING", 1800, false);
      } else {
        performance_record_armed = !performance_record_armed;
        request_header_draw();
        request_fn_draw(0);
        show_status_message(performance_record_armed ? "RECORDING ARMED" : "RECORDING OFF", 1400, false);
      }
    }
  }
  if (!performance_record_done) { return; }
  performance_record_done = false;
  performance_record_armed = false;
  request_header_draw();
  show_status_message(performance_record_failed ? "RECORDING FAILED" : "RECORDING STOPPED", 1800, false);
}

static bool next_loop_bgm_path(char* path, size_t path_len)
{
  if (!path || path_len < 24 || !ensure_sampler_sd_dirs()) { return false; }
  for (uint16_t index = 1; index < 1000; ++index) {
    snprintf(path, path_len, "/sampler/loops/Loop_%03u.wav", (unsigned)index);
    if (kp::storage_sd.getFileSize(path) <= 0) { return true; }
  }
  return false;
}

static void save_loop_as_bgm(void)
{
  if (loop_bgm_save_active) { return; }
  if (!loop_length_fixed || loop_length_msec < 250) {
    show_status_message("Loop length not set", 1800, false);
    return;
  }
  if (!ensure_sampler_sd_dirs()) {
    show_status_message("No SD", 1800, false);
    return;
  }

  const uint32_t frames = (uint32_t)(((uint64_t)loop_length_msec
                                    * sampler_audio_t::sample_rate) / 1000);
  int16_t* pcm = bgm_alloc((size_t)frames * sizeof(int16_t));
  if (!pcm) {
    show_status_message("No memory", 1800, false);
    return;
  }
  char path[64] = {};
  if (!next_loop_bgm_path(path, sizeof(path))) {
    free(pcm);
    show_status_message("No file name", 1800, false);
    return;
  }

  // The menu action intentionally becomes a stopped, one-pass bounce. The
  // original loop events and the loaded BGM remain in memory unchanged.
  menu_close();
  loop_bgm_save_active = true;
  loop_bgm_save_pcm = pcm;
  loop_bgm_save_frames = frames;
  stop_all_audio(false);
  loop_record_enabled = false;
  loop_length_fixed = true;
  loop_start_msec = M5.millis();
  loop_prev_pos_ms = loop_length_msec - 1;
  loop_playing = true;
  sampler_audio_t::startOutputCapture(pcm, frames);
  // A saved BGM is a complete backing track. Include the currently loaded
  // BGM at its configured level; the exported file becomes the replacement
  // BGM, so this does not create a duplicate on later playback.
  if (background_loop.isValid()) { play_background_loop_at(0); }

  recording_processing_static_drawn = false;
  recording_processing_frame = 0;
  processing_screen_visible = true;
  draw_recording_processing_frame("BOUNCING");
  const uint32_t wait_start = M5.millis();
  while (sampler_audio_t::outputCaptureFrames() < frames
      && M5.millis() - wait_start < loop_length_msec + 1000) {
#if defined(M5UNIFIED_PC_BUILD)
    service_loop(M5.millis());
#endif
    const uint32_t captured = sampler_audio_t::outputCaptureFrames();
    if (frames != 0) {
      const uint8_t percent = (uint8_t)std::min<uint32_t>(100, (captured * 100) / frames);
      if ((percent & 7u) == 0) { draw_recording_processing_frame("BOUNCING"); }
    }
    M5.delay(8);
  }
  const uint32_t captured = sampler_audio_t::stopOutputCapture();
  loop_playing = false;
  loop_prev_pos_ms = 0;
  loop_record_enabled = true;
  sampler_audio_t::stopAll();
  processing_screen_visible = false;
  loop_bgm_save_active = false;
  loop_bgm_save_pcm = nullptr;
  loop_bgm_save_frames = 0;

  bool ok = captured >= frames * 9 / 10
         && save_pcm_as_wav(kp::storage_sd, path, pcm, frames, sampler_audio_t::sample_rate);
  free(pcm);
  draw_all();
  show_status_message(ok ? "BGM saved" : "BGM save failed", 2000, false);
}

static bool save_session_pad(uint8_t pad)
{
  if (pad >= def::pad::pad_count) { return false; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || !ensure_sampler_sd_dirs()) { return false; }
  char path[80];
  snprintf(path, sizeof(path), "%s/pad%02u.wav", sampler_session_dir, (unsigned)(pad + 1));
  if (!save_pcm_as_wav(kp::storage_sd, path, slot.pcm, slot.frames, slot.sample_rate)) {
    return false;
  }
  snprintf(slot.file_path, sizeof(slot.file_path), "%s", path);
  return true;
}

static bool make_kit_asset_directory(kp::storage_base_t& storage, const char* kit_path, std::string& asset_dir)
{
  if (!kit_path) { return false; }
  std::string path = kit_path;
  if (path.size() <= 5 || path.compare(path.size() - 5, 5, ".json") != 0) { return false; }
  asset_dir = path.substr(0, path.size() - 5) + "_assets";
  // 既に作成済みの場合も false を返す実装があるため、作成後にサイズ問い合わせを
  // 行わず、そのまま保存を試みる。各WAVは常に上書きする。
  storage.makeDirectory(asset_dir.c_str());
  return true;
}

static bool save_kit_to_storage(kp::storage_base_t& storage, const char* path)
{
  if (!path || !storage.beginStorage()) { return false; }
  const bool is_resume = strcmp(path, sampler_resume_path) == 0;
  std::string asset_dir;
  if (!is_resume && !make_kit_asset_directory(storage, path, asset_dir)) { return false; }
  JsonDocument doc;
  doc["version"] = 7;
  doc["resume"] = is_resume;
  if (!is_resume) { doc["assets"] = asset_dir; }
  JsonArray samples = doc["samples"].to<JsonArray>();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    auto& slot = sampler_pool_t::slot[i];
    if (!slot.isValid()) { continue; }
    std::string file = slot.file_path;
    if (!is_resume) {
      char asset_path[128];
      snprintf(asset_path, sizeof(asset_path), "%s/pad%02d.wav", asset_dir.c_str(), i + 1);
      if (!save_pcm_as_wav(storage, asset_path, slot.pcm, slot.frames, slot.sample_rate)) { return false; }
      file = asset_path;
    }
    JsonObject s = samples.add<JsonObject>();
    s["pad"] = pad_display_number((uint8_t)i);
    s["internalPad"] = i;
    s["name"] = slot.name;
    s["file"] = file;
    s["start"] = slot.start_frame;
    s["end"] = slot.end_frame;
    s["volume"] = slot.volume_q8;
    s["pitch"] = slot.pitch_q8;
    s["baseNote"] = slot.base_note;
    s["baseNoteAuto"] = slot.base_note_auto;
    s["synthSustainAuto"] = slot.synth_sustain_auto;
    s["synthSustainConfidence"] = slot.synth_sustain_confidence;
    s["synthLoopStart"] = slot.synth_loop_start;
    s["synthLoopEnd"] = slot.synth_loop_end;
    s["synthLoopCrossfade"] = slot.synth_loop_crossfade;
    s["synthSustainMode"] = (uint8_t)slot.synth_sustain_mode;
    s["synthReleaseMs"] = slot.synth_release_ms;
    s["reverse"] = slot.reverse;
    s["hold"] = slot.hold_enabled;
    s["loop"] = slot.loop_enabled;
    s["loopWholeSample"] = slot.loop_whole_sample;
    s["loopGridHalfSteps"] = slot.loop_grid_half_steps;
    s["beatAnchorEnabled"] = slot.beat_anchor_enabled;
    s["beatAnchorFrame"] = slot.beat_anchor_frame;
  }
  JsonObject loop = doc["loop"].to<JsonObject>();
  loop["lengthMs"] = loop_length_msec;
  loop["lengthFixed"] = loop_length_fixed;
  loop["quantize"] = loop_quantize_enabled;
  loop["noteGridIndex"] = loop_quantize_option_index;
  loop["noteOffGridIndex"] = loop_note_off_quantize_option_index;
  JsonObject bgm = loop["background"].to<JsonObject>();
  bgm["name"] = background_loop.name;
  std::string bgm_file = background_loop.file_path;
  if (!is_resume && background_loop.isValid()) {
    char asset_path[128];
    snprintf(asset_path, sizeof(asset_path), "%s/background.wav", asset_dir.c_str());
    if (!save_pcm_as_wav(storage, asset_path, background_loop.pcm,
                         background_loop.frames, background_loop.sample_rate)) { return false; }
    bgm_file = asset_path;
  }
  bgm["file"] = bgm_file;
  bgm["volume"] = background_loop.volume_q8;
  bgm["repeats"] = background_loop.loop_repeats;
  JsonArray events = loop["events"].to<JsonArray>();
  for (const auto& e : loop_events) {
    JsonObject item = events.add<JsonObject>();
    item["page"] = (uint8_t)e.page;
    item["pad"] = e.pad;
    item["type"] = loop_event_type_name(e.type);
    item["pos"] = e.pos_ms;
    item["layer"] = e.layer;
    item["chordFlags"] = e.chord_flags;
  }
  JsonObject fx = doc["fx"].to<JsonObject>();
  fx["pitch"] = fx_param[0];
  fx["filter"] = fx_param[1];
  fx["repeat"] = fx_param[2];
  fx["delay"] = fx_param[fx_delay_index];
  JsonObject mixer = doc["mixer"].to<JsonObject>();
  JsonArray mixer_volume = mixer["volume"].to<JsonArray>();
  JsonArray mixer_mute = mixer["mute"].to<JsonArray>();
  for (uint8_t part = 0; part < mixer_part_count; ++part) {
    mixer_volume.add(mixer_part_volume[part]);
    mixer_mute.add(mixer_part_muted[part]);
  }
  JsonArray mixer_scenes = mixer["scenes"].to<JsonArray>();
  for (const auto& snapshot : mixer_snapshot) {
    JsonObject scene = mixer_scenes.add<JsonObject>();
    scene["valid"] = snapshot.valid;
    JsonArray volume = scene["volume"].to<JsonArray>();
    JsonArray mute = scene["mute"].to<JsonArray>();
    for (uint8_t part = 0; part < mixer_part_count; ++part) {
      volume.add(snapshot.volume[part]);
      mute.add(snapshot.muted[part]);
    }
  }
  JsonObject synth = doc["synth"].to<JsonObject>();
  synth["key"] = harmony_key();
  synth["scale"] = harmony_scale;
  JsonObject melody = synth["melody"].to<JsonObject>();
  melody["source"] = (uint8_t)melody_settings.source;
  melody["program"] = melody_settings.program;
  melody["pad"] = melody_settings.pad;
  melody["key"] = melody_settings.key;
  melody["scale"] = melody_settings.scale;
  melody["octave"] = melody_settings.octave;
  melody["volume"] = melody_settings.volume;
  melody["followHarmonyKey"] = melody_follow_harmony_key;
  JsonObject chord = synth["chord"].to<JsonObject>();
  chord["source"] = (uint8_t)chord_settings.source;
  chord["program"] = chord_settings.program;
  chord["pad"] = chord_settings.pad;
  chord["key"] = chord_settings.key;
  chord["octave"] = chord_settings.octave;
  chord["volume"] = chord_settings.volume;
  JsonObject bass = synth["bass"].to<JsonObject>();
  bass["source"] = (uint8_t)bass_settings.source;
  bass["program"] = bass_settings.program;
  bass["pad"] = bass_settings.pad;
  bass["key"] = chord_settings.key;
  bass["scale"] = bass_settings.scale;
  bass["octave"] = bass_settings.octave;
  bass["volume"] = bass_settings.volume;
  synth["drumVolume"] = drum_volume;
  JsonArray assigns = doc["midiAssign"].to<JsonArray>();
  for (uint8_t note = 0; note < 128; ++note) {
    if (midi_note_assign[note] != (int16_t)midi_assign_target_t::none) {
      JsonObject assign = assigns.add<JsonObject>();
      assign["note"] = note;
      assign["target"] = midi_note_assign[note];
    }
  }
  JsonArray cc_assigns = doc["midiCcAssign"].to<JsonArray>();
  for (uint8_t controller = 0; controller < 128; ++controller) {
    if (midi_cc_assign[controller] != (int16_t)midi_assign_target_t::none) {
      JsonObject assign = cc_assigns.add<JsonObject>();
      assign["cc"] = controller;
      assign["target"] = midi_cc_assign[controller];
    }
  }
  JsonArray external_assigns = doc["externalAssign"].to<JsonArray>();
  for (uint8_t button = 0; button < 32; ++button) {
    if (external_button_assign[button] != (int16_t)midi_assign_target_t::none) {
      JsonObject assign = external_assigns.add<JsonObject>();
      assign["button"] = button;
      assign["target"] = external_button_assign[button];
    }
  }
  doc["usbKeyboard"] = usb_keyboard_enabled;
  if (is_resume) {
    doc["performancePage"] = (uint8_t)current_page;
    doc["inputSource"] = (uint8_t)external_input_mode;
    doc["bleMidiAddress"] = ble_preferred_address;
    doc["bleMidiName"] = ble_preferred_name;
    doc["externalMidiCh1Program"] = external_midi_ch1_program;
    doc["externalMidiSound"] = (uint8_t)external_midi_sound;
    doc["externalMidiPad"] = external_midi_pad;
    doc["midiNoteAction"] = (uint8_t)midi_note_action;
  }
  JsonArray keyboard_assigns = doc["usbKeyboardAssign"].to<JsonArray>();
  for (uint16_t key = 0; key < 256; ++key) {
    if (usb_keyboard_assign[key] != (int16_t)midi_assign_target_t::none) {
      JsonObject assign = keyboard_assigns.add<JsonObject>();
      assign["key"] = key;
      assign["target"] = usb_keyboard_assign[key];
    }
  }
  JsonArray gamepad_assigns = doc["usbGamepadAssign"].to<JsonArray>();
  for (uint16_t code = 0; code < 256; ++code) {
    if (usb_gamepad_assign[code] != (int16_t)midi_assign_target_t::none) {
      JsonObject assign = gamepad_assigns.add<JsonObject>();
      assign["code"] = code;
      assign["target"] = usb_gamepad_assign[code];
    }
  }

  std::string out;
  serializeJson(doc, out);
  return storage.saveFromMemoryToFile(path, (const uint8_t*)out.c_str(), out.size()) >= 0;
}

static bool load_kit_file(const char* path)
{
  return load_kit_from_storage(kp::storage_sd, path);
}

static bool load_kit_from_storage(kp::storage_base_t& storage, const char* path, bool allow_sd_assets)
{
  if (!path || !storage.beginStorage()) { return false; }
  int size = storage.getFileSize(path);
  if (size <= 0 || size > 128 * 1024) { return false; }
  uint8_t* data = temp_alloc((size_t)size + 1);
  if (!data) { return false; }
  int len = storage.loadFromFileToMemory(path, data, (size_t)size);
  if (len <= 0) {
    free(data);
    return false;
  }
  data[len] = 0;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  free(data);
  if (err) { return false; }

  const bool is_resume = strcmp(path, sampler_resume_path) == 0;
  clear_kit();
  if (menu_visible) { show_loading_message(); }
  draw_startup_loading_frame(is_resume ? "RESTORING KIT" : "LOADING KIT");
  bool skipped_sd_assets = false;
  auto restore_synth_settings = [](uint8_t pad, sample_slot_t& slot, JsonObject s) {
    sampler_pool_t::analyzeSynthSustain(pad);
    const uint8_t mode = std::min<uint8_t>(
      s["synthSustainMode"] | (uint8_t)sample_sustain_mode_t::automatic,
      (uint8_t)sample_sustain_mode_t::manual);
    slot.synth_sustain_mode = (sample_sustain_mode_t)mode;
    slot.synth_release_ms = std::clamp<uint16_t>(
      s["synthReleaseMs"] | 120, 10, 2000);
    if (slot.synth_sustain_mode == sample_sustain_mode_t::manual) {
      const uint32_t loop_start = s["synthLoopStart"] | slot.synth_loop_start;
      const uint32_t loop_end = s["synthLoopEnd"] | slot.synth_loop_end;
      if (loop_start >= slot.playStart() && loop_end <= slot.playEnd()
       && loop_end > loop_start + 31) {
        slot.synth_loop_start = loop_start;
        slot.synth_loop_end = loop_end;
        slot.synth_loop_crossfade = std::min<uint16_t>(
          s["synthLoopCrossfade"] | slot.synth_loop_crossfade,
          (uint16_t)((loop_end - loop_start) / 4));
      } else {
        initialize_manual_sustain(slot);
      }
    }
    if (slot.reverse) { slot.synth_sustain_mode = sample_sustain_mode_t::off; }
    if (slot.synth_sustain_mode != sample_sustain_mode_t::off) {
      slot.loop_whole_sample = false;
    }
  };
  JsonArray samples = doc["samples"].as<JsonArray>();
  for (JsonObject s : samples) {
    draw_startup_loading_frame(is_resume ? "RESTORING SOUNDS" : "LOADING SOUNDS");
    int pad = s["internalPad"] | -1;
    if (pad < 0) {
      int display_pad = s["pad"] | 0;
      if (display_pad >= 1 && display_pad <= (int)def::pad::pad_count) {
        pad = display_order_to_pad((uint8_t)(display_pad - 1));
      }
    }
    const char* file = s["file"] | "";
    if (pad < 0 || pad >= (int)def::pad::pad_count || file[0] == 0) { continue; }
    if (strncmp(file, "builtin:", 8) == 0) {
      if (load_builtin_sample_to_pad((uint8_t)pad, file)) {
        auto& slot = sampler_pool_t::slot[pad];
        slot.start_frame = std::min<uint32_t>((uint32_t)(s["start"] | 0), slot.frames);
        slot.end_frame = std::min<uint32_t>((uint32_t)(s["end"] | slot.frames), slot.frames);
        slot.volume_q8 = s["volume"] | 256;
        slot.pitch_q8 = s["pitch"] | 256;
        if (!s["baseNote"].isNull()) { slot.base_note = s["baseNote"] | 60; }
        slot.base_note_auto = s["baseNoteAuto"] | true;
        if (slot.base_note_auto && (slot.start_frame != 0 || slot.end_frame != slot.frames)) {
          sampler_pool_t::analyzeBaseNote((uint8_t)pad);
        }
        slot.reverse = s["reverse"] | false;
        slot.hold_enabled = s["hold"] | false;
        slot.loop_enabled = s["loop"] | false;
        slot.loop_whole_sample = s["loopWholeSample"] | false;
        slot.loop_grid_half_steps = loop_repeat_half_steps[
          sample_loop_grid_index(s["loopGridHalfSteps"] | 8)];
        slot.beat_anchor_enabled = s["beatAnchorEnabled"] | false;
        slot.beat_anchor_frame = std::min<uint32_t>(s["beatAnchorFrame"] | 0u,
                                                     slot.frames ? slot.frames - 1 : 0);
        restore_synth_settings((uint8_t)pad, slot, s);
      }
      continue;
    }
    // 起動時のresume復元ではSDを一切開かない。外部キットはメニューから
    // 明示的に読み込むため、SD上のファイル数・容量に起動時間が左右されない。
    if (!allow_sd_assets) {
      skipped_sd_assets = true;
      continue;
    }
    if (!kp::storage_sd.beginStorage()) { continue; }
    int audio_size = kp::storage_sd.getFileSize(file);
    if (audio_size <= 4 || audio_size > 3200 * 1024) { continue; }
    uint8_t* audio_data = temp_alloc((size_t)audio_size);
    if (!audio_data) { continue; }
    int audio_len = kp::storage_sd.loadFromFileToMemory(file, audio_data, (size_t)audio_size);
    if (audio_len > 4 && load_audio_memory_to_pad((uint8_t)pad, file, s["name"] | "", audio_data, audio_len)) {
      auto& slot = sampler_pool_t::slot[pad];
      snprintf(slot.file_path, sizeof(slot.file_path), "%s", file);
      slot.start_frame = std::min<uint32_t>((uint32_t)(s["start"] | 0), slot.frames);
      slot.end_frame = std::min<uint32_t>((uint32_t)(s["end"] | slot.frames), slot.frames);
      slot.volume_q8 = s["volume"] | 256;
      slot.pitch_q8 = s["pitch"] | 256;
      if (!s["baseNote"].isNull()) { slot.base_note = s["baseNote"] | 60; }
      slot.base_note_auto = s["baseNoteAuto"] | true;
      if (slot.base_note_auto && (slot.start_frame != 0 || slot.end_frame != slot.frames)) {
        sampler_pool_t::analyzeBaseNote((uint8_t)pad);
      }
      slot.reverse = s["reverse"] | false;
      slot.hold_enabled = s["hold"] | false;
      slot.loop_enabled = s["loop"] | false;
      slot.loop_whole_sample = s["loopWholeSample"] | false;
      slot.loop_grid_half_steps = loop_repeat_half_steps[
        sample_loop_grid_index(s["loopGridHalfSteps"] | 8)];
      slot.beat_anchor_enabled = s["beatAnchorEnabled"] | false;
      slot.beat_anchor_frame = std::min<uint32_t>(s["beatAnchorFrame"] | 0u,
                                                   slot.frames ? slot.frames - 1 : 0);
      restore_synth_settings((uint8_t)pad, slot, s);
    }
    free(audio_data);
  }

  JsonObject loop = doc["loop"].as<JsonObject>();
  JsonObject bgm = loop["background"].as<JsonObject>();
  const char* bgm_file = bgm["file"] | "";
  if (bgm_file[0]) {
    if (find_builtin_background_loop(bgm_file)) {
      load_builtin_background_loop(bgm_file);
    } else if (allow_sd_assets) {
      load_background_loop_file(bgm_file, bgm["name"] | "BGM");
    } else {
      skipped_sd_assets = true;
    }
    background_loop.volume_q8 = volume_q8_from_20_percent_step(
      volume_20_percent_step_from_q8(bgm["volume"] | background_loop.volume_q8));
    uint8_t repeats = bgm["repeats"] | background_loop.loop_repeats;
    background_loop.loop_repeats = repeats <= 1 ? 1 : (repeats <= 2 ? 2 : 4);
  }
  if (background_loop.isValid()) {
    // BGM WAV may be replaced without changing its built-in ID. Its current PCM
    // duration and repeat count are authoritative; a saved length can be stale.
    loop_length_msec = background_loop_length_ms();
    loop_length_fixed = true;
  } else {
    loop_length_msec = loop["lengthMs"] | loop_default_length_ms;
    loop_length_fixed = loop["lengthFixed"] | false;
  }
  loop_quantize_enabled = loop["quantize"] | loop_quantize_enabled;
  loop_quantize_option_index = loop["noteGridIndex"] | loop_quantize_option_index;
  loop_note_off_quantize_option_index = loop["noteOffGridIndex"] | loop_note_off_quantize_option_index;
  for (auto& history : loop_undo_history) { history.clear(); }
  uint16_t max_layer = 0;
  {
    loop_events_guard_t guard;
    loop_events.clear();
    for (JsonObject item : loop["events"].as<JsonArray>()) {
      loop_event_t e;
      uint8_t page = item["page"] | (uint8_t)performance_page_t::sample;
      if (page >= (uint8_t)performance_page_t::max) { page = (uint8_t)performance_page_t::sample; }
      e.page = (performance_page_t)page;
      e.pad = item["pad"] | 0;
      e.type = parse_loop_event_type(item["type"] | "on");
      e.pos_ms = (item["pos"] | 0) % std::max<uint32_t>(1, loop_length_msec);
      e.layer = item["layer"] | 0;
      e.chord_flags = item["chordFlags"] | 0;
      if (e.pad < def::pad::pad_count) {
        loop_events.push_back(e);
        if (max_layer < e.layer) { max_layer = e.layer; }
      }
    }
  }
  loop_layer_seq = max_layer + 1;
  loop_playing = false;
  loop_prev_pos_ms = 0;
  fx_param[0] = doc["fx"]["pitch"] | fx_param[0];
  fx_param[1] = doc["fx"]["filter"] | fx_param[1];
  fx_param[2] = doc["fx"]["repeat"] | fx_param[2];
  fx_param[fx_delay_index] = (int8_t)std::clamp<int>(
    doc["fx"]["delay"] | (int)fx_param[fx_delay_index],
    0, (int)delay_grid_option_count - 1);
  std::fill(mixer_part_volume, mixer_part_volume + mixer_part_count, 100);
  std::fill(mixer_part_muted, mixer_part_muted + mixer_part_count, false);
  for (auto& snapshot : mixer_snapshot) { snapshot = mixer_snapshot_t{}; }
  JsonObject mixer = doc["mixer"].as<JsonObject>();
  if (!mixer.isNull()) {
    JsonArray volume = mixer["volume"].as<JsonArray>();
    JsonArray mute = mixer["mute"].as<JsonArray>();
    for (uint8_t part = 0; part < mixer_part_count; ++part) {
      if (part < volume.size()) {
        mixer_part_volume[part] = std::min<int>(100, volume[part] | 100);
      }
      if (part < mute.size()) { mixer_part_muted[part] = mute[part] | false; }
    }
    JsonArray scenes = mixer["scenes"].as<JsonArray>();
    for (uint8_t index = 0; index < 4 && index < scenes.size(); ++index) {
      JsonObject scene = scenes[index].as<JsonObject>();
      mixer_snapshot[index].valid = scene["valid"] | false;
      JsonArray scene_volume = scene["volume"].as<JsonArray>();
      JsonArray scene_mute = scene["mute"].as<JsonArray>();
      for (uint8_t part = 0; part < mixer_part_count; ++part) {
        if (part < scene_volume.size()) {
          mixer_snapshot[index].volume[part] = std::min<int>(100, scene_volume[part] | 100);
        }
        if (part < scene_mute.size()) {
          mixer_snapshot[index].muted[part] = scene_mute[part] | false;
        }
      }
    }
  }
  mixer_pending_snapshot = -1;
  mixer_applied_snapshot = -1;
  mixer_active = false;
  mixer_held_part = -1;
  JsonObject synth = doc["synth"].as<JsonObject>();
  JsonObject melody = synth["melody"].as<JsonObject>();
  melody_follow_harmony_key = true;
  if (!melody.isNull()) {
    melody_settings.source = (melody["source"] | 0) == 1
      ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    melody_settings.program = std::min<int>(127, melody["program"] | melody_settings.program);
    melody_settings.pad = std::min<int>(def::pad::pad_count - 1, melody["pad"] | melody_settings.pad);
    melody_settings.key = std::min<int>(11, melody["key"] | melody_settings.key);
    melody_settings.scale = std::min<int>((int)sampler_scale_count - 1,
                                          melody["scale"] | melody_settings.scale);
    melody_settings.octave = std::clamp<int>(melody["octave"] | (int)melody_settings.octave, -2, 2);
    melody_settings.volume = synth_volume_percent_from_step(
      synth_volume_step_from_percent(std::min<int>(100, melody["volume"] | melody_settings.volume)));
    melody_follow_harmony_key = melody["followHarmonyKey"] | true;
  }
  JsonObject chord = synth["chord"].as<JsonObject>();
  if (!chord.isNull()) {
    chord_settings.source = (chord["source"] | 0) == 1
      ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    chord_settings.program = std::min<int>(127, chord["program"] | chord_settings.program);
    chord_settings.pad = std::min<int>(def::pad::pad_count - 1, chord["pad"] | chord_settings.pad);
    chord_settings.key = std::min<int>(11, chord["key"] | chord_settings.key);
    chord_settings.octave = std::clamp<int>(chord["octave"] | (int)chord_settings.octave, -2, 2);
    chord_settings.volume = synth_volume_percent_from_step(
      synth_volume_step_from_percent(std::min<int>(100, chord["volume"] | chord_settings.volume)));
  }
  JsonObject bass = synth["bass"].as<JsonObject>();
  bass_settings = {
    synth_tone_source_t::general_midi, 38, factory_pad_sound_pad, 0, 0, 0, 80
  };
  if (!bass.isNull()) {
    bass_settings.source = (bass["source"] | 0) == 1
      ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
    bass_settings.program = std::min<int>(127, bass["program"] | bass_settings.program);
    bass_settings.pad = std::min<int>(def::pad::pad_count - 1, bass["pad"] | bass_settings.pad);
    bass_settings.scale = std::min<int>((int)sampler_scale_count - 1,
                                        bass["scale"] | bass_settings.scale);
    int restored_bass_octave = bass["octave"] | (int)bass_settings.octave;
    // v4 and earlier stored Bass's one-octave-lower base as user value -1.
    // Shift the display value up while preserving the audible note range.
    if ((doc["version"] | 1) < 5) { ++restored_bass_octave; }
    bass_settings.octave = std::clamp<int>(restored_bass_octave, -2, 2);
    bass_settings.volume = synth_volume_percent_from_step(
      synth_volume_step_from_percent(std::min<int>(100, bass["volume"] | bass_settings.volume)));
  }
  // New Kits keep Key / Scale at synth root.  Older Kits are migrated from
  // their Melody and Chord settings, then all three performance pages share
  // the resulting global harmony state.
  const uint8_t restored_key = std::min<int>(11, synth["key"] | chord_settings.key);
  const uint8_t restored_scale = std::min<int>((int)sampler_scale_count - 1,
    synth["scale"] | melody_settings.scale);
  chord_settings.key = restored_key;
  melody_settings.key = restored_key;
  bass_settings.key = restored_key;
  harmony_scale = restored_scale;
  melody_settings.scale = restored_scale;
  bass_settings.scale = restored_scale;
  melody_follow_harmony_key = true;
  drum_volume = synth_volume_percent_from_step(
    synth_volume_step_from_percent(std::min<int>(100, synth["drumVolume"] | drum_volume)));
    std::fill(midi_note_assign, midi_note_assign + 128, (int16_t)midi_assign_target_t::none);
    std::fill(midi_cc_assign, midi_cc_assign + 128, (int16_t)midi_assign_target_t::none);
  std::fill(external_button_assign, external_button_assign + 32, (int16_t)midi_assign_target_t::none);
  std::fill(usb_keyboard_assign, usb_keyboard_assign + 256, (int16_t)midi_assign_target_t::none);
  std::fill(usb_gamepad_assign, usb_gamepad_assign + 256, (int16_t)midi_assign_target_t::none);
  for (JsonObject assign : doc["midiAssign"].as<JsonArray>()) {
    int note = assign["note"] | -1;
    int target = assign["target"] | (int)midi_assign_target_t::none;
    if (note >= 0 && note < 128
     && target >= (int)midi_assign_target_t::pad_base
     && target < (int)midi_assign_target_t::fn_base + 3) {
      midi_note_assign[note] = target;
    }
  }
  for (JsonObject assign : doc["midiCcAssign"].as<JsonArray>()) {
    int controller = assign["cc"] | -1;
    int target = assign["target"] | (int)midi_assign_target_t::none;
    if (controller >= 0 && controller < 128
     && target >= (int)midi_assign_target_t::pad_base
     && target < (int)midi_assign_target_t::fn_base + 3) {
      midi_cc_assign[controller] = target;
    }
  }
  for (JsonObject assign : doc["externalAssign"].as<JsonArray>()) {
    int button = assign["button"] | -1;
    int target = assign["target"] | (int)midi_assign_target_t::none;
    if (button >= 0 && button < 32
     && target >= (int)midi_assign_target_t::pad_base
     && target < (int)midi_assign_target_t::fn_base + 3) {
      external_button_assign[button] = target;
    }
  }
  for (JsonObject assign : doc["usbKeyboardAssign"].as<JsonArray>()) {
    int key = assign["key"] | -1;
    int target = assign["target"] | (int)midi_assign_target_t::none;
    if (key >= 0 && key < 256
     && target >= (int)midi_assign_target_t::pad_base
     && target < (int)midi_assign_target_t::fn_base + 3) {
      usb_keyboard_assign[key] = target;
    }
  }
  for (JsonObject assign : doc["usbGamepadAssign"].as<JsonArray>()) {
    int code = assign["code"] | -1;
    int target = assign["target"] | (int)midi_assign_target_t::none;
    if (code >= 0 && code < 256
     && target >= (int)midi_assign_target_t::pad_base
     && target < (int)midi_assign_target_t::fn_base + 3) {
      usb_gamepad_assign[code] = target;
    }
  }
  if (is_resume) {
    uint8_t page = doc["performancePage"] | (uint8_t)performance_page_t::sample;
    if (page < (uint8_t)performance_page_t::max) { current_page = (performance_page_t)page; }
    snprintf(ble_preferred_address, sizeof(ble_preferred_address), "%s",
             doc["bleMidiAddress"] | "");
    snprintf(ble_preferred_name, sizeof(ble_preferred_name), "%s",
             doc["bleMidiName"] | "");
    task_midi.setBLEMidiPreferredDevice(ble_preferred_address, ble_preferred_name);
    uint8_t source = doc["inputSource"] | (uint8_t)external_input_mode_t::off;
    if (source >= (uint8_t)external_input_mode_t::max) { source = (uint8_t)external_input_mode_t::off; }
    external_input_mode = (external_input_mode_t)source;
    uint8_t program = doc["externalMidiCh1Program"] | external_midi_ch1_program;
    if (program < 128) { external_midi_ch1_program = program; }
    uint8_t sound = doc["externalMidiSound"] | (uint8_t)external_midi_sound_t::general_midi;
    if (sound <= (uint8_t)external_midi_sound_t::pad) {
      external_midi_sound = (external_midi_sound_t)sound;
    }
    uint8_t pad = doc["externalMidiPad"] | external_midi_pad;
    if (pad < def::pad::pad_count) { external_midi_pad = pad; }
    uint8_t note_action = doc["midiNoteAction"] | (uint8_t)midi_note_action_t::automatic;
    if (note_action <= (uint8_t)midi_note_action_t::control) {
      midi_note_action = (midi_note_action_t)note_action;
    }
    if (synth["melody"].isNull()) {
      melody_settings.program = external_midi_ch1_program;
      melody_settings.source = external_midi_sound == external_midi_sound_t::pad
        ? synth_tone_source_t::pad : synth_tone_source_t::general_midi;
      melody_settings.pad = external_midi_pad;
    }
  }
  // inputSource導入前のresumeは従来のUSB Keyboard設定を引き継ぐ。
  if (is_resume && doc["inputSource"].isNull() && (doc["usbKeyboard"] | false)) {
    external_input_mode = external_input_mode_t::usb_keyboard;
  }
  update_midi_assign_count();
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_display_length_ms(M5.millis())));
  refresh_sample_grid_loop_intervals();
  repair_pitched_pad_sources();
  apply_synth_tones(true);
  apply_all_mixer_parts();
  if (!startup_loading_active) {
    draw_all();
    update_all_leds();
  }
  // 外部素材を含むresumeは、呼び出し元で完全な内蔵プリセットへ戻す。
  // MIDI/FXなどLittleFS内の設定はここまでで復元済み。
  if (!is_resume && &storage == &kp::storage_sd && !skipped_sd_assets) {
    snprintf(current_kit_path, sizeof(current_kit_path), "%s", path);
  }
  return !skipped_sd_assets;
}

static bool load_resume_kit(void)
{
  // ResumeはSD全体を列挙しない。直前状態に記録されたWAVだけを直接開くため、
  // セッション素材を残したまま起動時間を予測可能に保てる。
  return load_kit_from_storage(kp::storage_littlefs, sampler_resume_path, true);
}

static void save_resume_kit(void)
{
  clear_synth_runtime();
  sampler_audio_t::stopAll();
  clear_sample_grid_loops();
  clear_menu_preview();
  save_kit_to_storage(kp::storage_littlefs, sampler_resume_path);
}

//-------------------------------------------------------------------------
// Web editor bridge

static bool sampler_web_path_is_in(const char* path, const char* directory, const char* suffix)
{
  if (!path || !directory || !suffix) { return false; }
  size_t dir_len = strlen(directory);
  size_t path_len = strlen(path);
  size_t suffix_len = strlen(suffix);
  if (strncmp(path, directory, dir_len) != 0 || path[dir_len] != '/') { return false; }
  if (strstr(path + dir_len + 1, "..") != nullptr) { return false; }
  return path_len > dir_len + suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static bool sampler_web_audio_path_is_in(const char* path, const char* directory)
{
  return sampler_web_path_is_in(path, directory, "")
      && is_audio_file_name(path);
}

bool sampler_web_enqueue_command(const uint8_t* data, size_t size)
{
  if (data == nullptr || size == 0 || size > 32 * 1024) { return false; }
#if !defined(M5UNIFIED_PC_BUILD)
  if (sampler_web_command_mutex == nullptr
   || xSemaphoreTake(sampler_web_command_mutex, pdMS_TO_TICKS(20)) != pdTRUE) { return false; }
#endif
  bool accepted = sampler_web_pending_command.empty();
  if (accepted) {
    sampler_web_pending_command.assign((const char*)data, size);
  }
#if !defined(M5UNIFIED_PC_BUILD)
  xSemaphoreGive(sampler_web_command_mutex);
#endif
  return accepted;
}

bool sampler_web_prepare_storage_operation(bool remount)
{
  sampler_web_storage_operation_ok = false;
  sampler_web_storage_stop_done = false;
  sampler_web_storage_remount_requested = remount;
  sampler_web_storage_stop_requested = true;
  // WAVの読込み・セッション保存は主ループ内で完了するまでSDを占有する。
  // Assign直後のWeb UI自動更新も、その完了を待ってから一覧を返す。
  const uint32_t deadline = M5.millis() + 5000;
  while (!sampler_web_storage_stop_done && (int32_t)(M5.millis() - deadline) < 0) {
    M5.delay(2);
  }
  return sampler_web_storage_stop_done && sampler_web_storage_operation_ok;
}

void sampler_web_note_client_access(void)
{
  wifi_file_server_client_connected = true;
}

static void service_sampler_web_storage_stop(void)
{
  if (!sampler_web_storage_stop_requested) { return; }
  sampler_web_storage_stop_requested = false;
  stop_all_audio();
  bool ok = true;
  if (sampler_web_storage_remount_requested) {
    // HTTPタスクでSDを終了すると、メインループ上のKIT/セッション処理と
    // 競合してWebサーバーごと停止することがある。再接続はここへ集約する。
    kp::storage_sd.endStorage();
    M5.delay(4);
    ok = kp::storage_sd.beginStorage();
  }
  sampler_web_storage_remount_requested = false;
  sampler_web_storage_operation_ok = ok;
  sampler_web_storage_stop_done = true;
}

bool sampler_web_export_state(std::string& out)
{
  JsonDocument doc;
  doc["version"] = 1;
  doc["sampleRate"] = sampler_audio_t::sample_rate;
  JsonObject folders = doc["folders"].to<JsonObject>();
  folders["samples"] = sampler_sd_folders[0];
  folders["loops"] = sampler_sd_folders[1];
  folders["kits"] = sampler_sd_folders[2];
  JsonArray builtin_samples_json = doc["builtinSamples"].to<JsonArray>();
  for (const auto& source : builtin_samples) {
    JsonObject item = builtin_samples_json.add<JsonObject>();
    item["name"] = source.name;
    item["file"] = std::string("builtin:") + source.name;
  }
  JsonArray builtin_bgm_json = doc["builtinBackgrounds"].to<JsonArray>();
  for (const auto& source : builtin_background_loops) {
    JsonObject builtin_bgm = builtin_bgm_json.add<JsonObject>();
    builtin_bgm["name"] = source.source.name;
    builtin_bgm["file"] = std::string("builtin:") + source.file;
  }
  JsonArray pads_json = doc["pads"].to<JsonArray>();
  for (uint8_t pad = 0; pad < def::pad::pad_count; ++pad) {
    const auto& slot = sampler_pool_t::slot[pad];
    JsonObject item = pads_json.add<JsonObject>();
    item["pad"] = pad;
    item["label"] = pad_display_number(pad);
    item["name"] = slot.name;
    item["file"] = slot.file_path;
    item["frames"] = slot.frames;
    item["sampleRate"] = slot.sample_rate;
    item["start"] = slot.playStart();
    item["end"] = slot.playEnd();
    item["volume"] = slot.volume_q8;
    item["pitch"] = slot.pitch_q8;
    item["baseNote"] = slot.base_note;
    item["baseNoteAuto"] = slot.base_note_auto;
    item["synthSustainMode"] = (uint8_t)slot.synth_sustain_mode;
    item["synthReleaseMs"] = slot.synth_release_ms;
    item["synthLoopStart"] = slot.synth_loop_start;
    item["synthLoopEnd"] = slot.synth_loop_end;
    item["reverse"] = slot.reverse;
    item["hold"] = slot.hold_enabled;
    item["loop"] = slot.loop_enabled;
    item["loopWholeSample"] = slot.loop_whole_sample;
    item["loopGridHalfSteps"] = slot.loop_grid_half_steps;
    item["beatAnchorEnabled"] = slot.beat_anchor_enabled;
    item["beatAnchorFrame"] = slot.beat_anchor_frame;
    JsonArray wave = item["wave"].to<JsonArray>();
    for (uint8_t bin = 0; bin < sample_slot_t::waveform_bins; ++bin) {
      JsonArray point = wave.add<JsonArray>();
      point.add(slot.waveform_min[bin]);
      point.add(slot.waveform_max[bin]);
    }
  }
  JsonObject loop = doc["loop"].to<JsonObject>();
  loop["lengthMs"] = loop_length_msec;
  loop["lengthFixed"] = loop_length_fixed;
  loop["quantize"] = loop_quantize_enabled;
  loop["noteGridIndex"] = loop_quantize_option_index;
  loop["noteOffGridIndex"] = loop_note_off_quantize_option_index;
  loop["playing"] = loop_playing;
  JsonObject bgm = loop["background"].to<JsonObject>();
  bgm["name"] = background_loop.name;
  bgm["file"] = background_loop.file_path;
  bgm["volume"] = background_loop.volume_q8;
  bgm["frames"] = background_loop.frames;
  bgm["sampleRate"] = background_loop.sample_rate;
  JsonArray events = loop["events"].to<JsonArray>();
  for (const auto& e : loop_events) {
    JsonObject item = events.add<JsonObject>();
    item["page"] = (uint8_t)e.page;
    item["pad"] = e.pad;
    item["type"] = loop_event_type_name(e.type);
    item["pos"] = e.pos_ms;
    item["layer"] = e.layer;
    item["chordFlags"] = e.chord_flags;
  }
  serializeJson(doc, out);
  return true;
}

bool sampler_web_get_audio(bool background, uint8_t pad, sampler_web_audio_t& out)
{
  out = {};
  if (background) {
    out.pcm = background_loop.pcm;
    out.frames = background_loop.frames;
    out.sample_rate = background_loop.sample_rate;
  } else if (pad < def::pad::pad_count) {
    const auto& slot = sampler_pool_t::slot[pad];
    out.pcm = slot.pcm;
    out.frames = slot.frames;
    out.sample_rate = slot.sample_rate;
  }
  return out.pcm != nullptr && out.frames != 0 && out.sample_rate != 0;
}

static void service_sampler_web_command(void)
{
  std::string command;
#if !defined(M5UNIFIED_PC_BUILD)
  if (sampler_web_command_mutex == nullptr
   || xSemaphoreTake(sampler_web_command_mutex, 0) != pdTRUE) { return; }
#endif
  command.swap(sampler_web_pending_command);
#if !defined(M5UNIFIED_PC_BUILD)
  xSemaphoreGive(sampler_web_command_mutex);
#endif
  if (command.empty()) { return; }

  JsonDocument doc;
  if (deserializeJson(doc, command)) { return; }
  const char* action = doc["action"] | "";
  if (strcmp(action, "playPad") == 0) {
    int pad = doc["pad"] | -1;
    if (pad >= 0 && pad < def::pad::pad_count) { trigger_pad(pad); }
    return;
  }
  if (strcmp(action, "stopAudio") == 0) {
    stop_all_audio();
    return;
  }
  if (strcmp(action, "previewWav") == 0) {
    const char* path = doc["file"] | "";
    if (strncmp(path, "builtin:", 8) == 0) {
      if (const auto* bgm = find_builtin_background_loop(path)) {
        clear_menu_preview();
        decode_menu_wav_preview(bgm->source.data, bgm->source.size(),
                                std::clamp<uint32_t>(doc["maxMs"] | 2000, 250, 2000));
      } else {
        play_menu_builtin_preview(path, std::clamp<uint32_t>(doc["maxMs"] | 2000, 250, 2000));
      }
    } else if (sampler_web_audio_path_is_in(path, "/sampler/samples")
     || sampler_web_audio_path_is_in(path, "/sampler/loops")) {
      // アサイン前のSD上音源を専用プレビューVoiceへ短時間だけ展開する。
      // Padプールと設定は変更しない。
      uint32_t max_ms = std::clamp<uint32_t>(doc["maxMs"] | 2000, 250, 2000);
      play_menu_audio_preview(path, max_ms);
    }
    return;
  }
  if (strcmp(action, "playBgm") == 0) {
    // File editor preview must not trigger recorded pad events.  Start only
    // the background voice and leave the loop transport stopped.
    loop_playing = false;
    loop_prev_pos_ms = 0;
    clear_synth_runtime();
    sampler_audio_t::stopAll();
    clear_sample_grid_loops();
    play_background_loop_at(0);
    return;
  }
  if (strcmp(action, "stopBgm") == 0) {
    loop_playing = false;
    loop_prev_pos_ms = 0;
    stop_background_loop();
    return;
  }
  if (strcmp(action, "setFolder") == 0) {
    const char* kind = doc["kind"] | "";
    const char* path = doc["path"] | "";
    const char* kinds[] = { "samples", "loops", "kits" };
    const char* roots[] = { "/sampler/samples", "/sampler/loops", "/sampler/kits" };
    for (size_t i = 0; i < 3; ++i) {
      size_t root_len = strlen(roots[i]);
      if (strcmp(kind, kinds[i]) == 0 && strncmp(path, roots[i], root_len) == 0
       && (path[root_len] == 0 || path[root_len] == '/') && strstr(path, "..") == nullptr
       && strlen(path) < sizeof(sampler_sd_folders[i])) {
        snprintf(sampler_sd_folders[i], sizeof(sampler_sd_folders[i]), "%s", path);
        save_sampler_folder_settings();
      }
    }
    return;
  }
  if (strcmp(action, "loadKit") == 0) {
    const char* path = doc["file"] | "";
    if (sampler_web_path_is_in(path, "/sampler/kits", ".json")) { load_kit_file(path); }
    return;
  }
  if (strcmp(action, "saveKit") == 0) {
    const char* path = doc["file"] | "";
    if (sampler_web_path_is_in(path, "/sampler/kits", ".json") && kp::storage_sd.beginStorage()) {
      ensure_sampler_sd_dirs();
      if (save_kit_to_storage(kp::storage_sd, path)) {
        snprintf(current_kit_path, sizeof(current_kit_path), "%s", path);
      }
    }
    return;
  }
  if (strcmp(action, "assignSample") == 0) {
    int pad = doc["pad"] | -1;
    const char* path = doc["file"] | "";
    if (pad >= 0 && pad < def::pad::pad_count && strncmp(path, "builtin:", 8) == 0) {
      if (load_builtin_sample_to_pad((uint8_t)pad, path)) {
        loop_remove_pad_events((uint8_t)pad);
        loop_reset_recording_state_if_empty();
        rec_wave_pad = pad;
        request_wave_draw();
        request_pad_draw((uint8_t)pad);
      }
    } else if (pad >= 0 && pad < def::pad::pad_count && sampler_web_audio_path_is_in(path, "/sampler/samples")) {
      const char* name = strrchr(path, '/');
      char error[32];
      load_audio_to_pad((uint8_t)pad, path, name ? name + 1 : path, error, sizeof(error));
    }
    return;
  }
  if (strcmp(action, "clearPad") == 0) {
    int pad = doc["pad"] | -1;
    if (pad >= 0 && pad < def::pad::pad_count) { clear_pad_sample((uint8_t)pad, true); }
    return;
  }
  if (strcmp(action, "setPad") == 0) {
    int pad = doc["pad"] | -1;
    if (pad < 0 || pad >= def::pad::pad_count) { return; }
    auto& slot = sampler_pool_t::slot[pad];
    if (!slot.isValid()) { return; }
    if (!doc["start"].isNull()) { slot.start_frame = std::min<uint32_t>(doc["start"].as<uint32_t>(), slot.frames); }
    if (!doc["end"].isNull()) { slot.end_frame = std::min<uint32_t>(doc["end"].as<uint32_t>(), slot.frames); }
    if (slot.end_frame <= slot.start_frame) { slot.end_frame = std::min<uint32_t>(slot.frames, slot.start_frame + 1); }
    if (!doc["volume"].isNull()) { slot.volume_q8 = std::clamp<uint16_t>(doc["volume"].as<uint16_t>(), 0, 512); }
    if (!doc["pitch"].isNull()) { slot.pitch_q8 = std::clamp<uint16_t>(doc["pitch"].as<uint16_t>(), 128, 512); }
    if (!doc["baseNote"].isNull()) {
      slot.base_note = std::min<uint8_t>(127, doc["baseNote"].as<uint8_t>());
      slot.base_note_auto = false;
    }
    if (!doc["reverse"].isNull()) {
      slot.reverse = doc["reverse"].as<bool>();
      if (slot.reverse) { slot.synth_sustain_mode = sample_sustain_mode_t::off; }
    }
    if (!doc["hold"].isNull()) { slot.hold_enabled = doc["hold"].as<bool>(); }
    if (!doc["loop"].isNull()) {
      slot.loop_enabled = doc["loop"].as<bool>();
      if (!slot.loop_enabled) {
        slot.loop_whole_sample = false;
        stop_sample_grid_loop(pad);
      }
    }
    if (!doc["loopWholeSample"].isNull()) {
      slot.loop_whole_sample = doc["loopWholeSample"].as<bool>();
      if (slot.loop_whole_sample) {
        slot.loop_enabled = true;
        slot.synth_sustain_mode = sample_sustain_mode_t::off;
      }
      stop_sample_grid_loop(pad);
    }
    if (!doc["loopGridHalfSteps"].isNull()) {
      slot.loop_whole_sample = false;
      slot.loop_grid_half_steps = loop_repeat_half_steps[
        sample_loop_grid_index(doc["loopGridHalfSteps"].as<uint8_t>())];
      if (sample_grid_loop_active[pad]) {
        arm_sample_grid_loop_next(pad, M5.millis());
      }
    }
    if (!doc["synthReleaseMs"].isNull()) {
      slot.synth_release_ms = std::clamp<uint16_t>(
        doc["synthReleaseMs"].as<uint16_t>(), 10, 2000);
    }
    if (!doc["synthSustainMode"].isNull()) {
      const uint8_t mode = std::min<uint8_t>(
        doc["synthSustainMode"].as<uint8_t>(),
        (uint8_t)sample_sustain_mode_t::manual);
      slot.synth_sustain_mode = (sample_sustain_mode_t)mode;
      if (slot.synth_sustain_mode != sample_sustain_mode_t::off) {
        slot.reverse = false;
        slot.loop_whole_sample = false;
        if (slot.synth_sustain_mode == sample_sustain_mode_t::automatic) {
          sampler_pool_t::analyzeSynthSustain((uint8_t)pad);
        } else {
          if (!doc["synthLoopStart"].isNull()) {
            slot.synth_loop_start = std::clamp<uint32_t>(
              doc["synthLoopStart"].as<uint32_t>(), slot.playStart(), slot.playEnd());
          }
          if (!doc["synthLoopEnd"].isNull()) {
            slot.synth_loop_end = std::clamp<uint32_t>(
              doc["synthLoopEnd"].as<uint32_t>(), slot.playStart(), slot.playEnd());
          }
          initialize_manual_sustain(slot);
        }
      }
    }
    request_wave_draw();
    request_pad_draw((uint8_t)pad);
    return;
  }
  if (strcmp(action, "loadBgm") == 0) {
    const char* path = doc["file"] | "";
    if (find_builtin_background_loop(path)) {
      load_builtin_background_loop(path);
    } else if (sampler_web_audio_path_is_in(path, "/sampler/loops")) {
      const char* name = strrchr(path, '/');
      load_background_loop_file(path, name ? name + 1 : path);
    }
    return;
  }
  if (strcmp(action, "clearBgm") == 0) {
    clear_background_loop();
    return;
  }
  if (strcmp(action, "setLoop") == 0) {
    uint32_t length = doc["lengthMs"] | loop_length_msec;
    loop_length_msec = std::clamp<uint32_t>(length, 250, background_loop_max_sec * 1000);
    if (!doc["lengthFixed"].isNull()) { loop_length_fixed = doc["lengthFixed"].as<bool>(); }
    if (!doc["quantize"].isNull()) { loop_quantize_enabled = doc["quantize"].as<bool>(); }
    if (!doc["noteGridIndex"].isNull()) { loop_quantize_option_index = std::min<uint8_t>(doc["noteGridIndex"].as<uint8_t>(), loop_quantize_option_count() - 1); }
    if (!doc["noteOffGridIndex"].isNull()) { loop_note_off_quantize_option_index = std::min<uint8_t>(doc["noteOffGridIndex"].as<uint8_t>(), loop_quantize_option_count() - 1); }
    if (!doc["backgroundVolume"].isNull()) {
      background_loop.volume_q8 = volume_q8_from_20_percent_step(
        volume_20_percent_step_from_q8(std::clamp<uint16_t>(doc["backgroundVolume"].as<uint16_t>(), 0, 256)));
      if (loop_playing && background_loop.isValid()) { play_background_loop_at(loop_pos_ms(M5.millis())); }
    }
    sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
    refresh_sample_grid_loop_intervals();
    invalidate_loop_timeline_cache();
    request_wave_draw();
    return;
  }
  if (strcmp(action, "setEvents") == 0) {
    for (auto& history : loop_undo_history) { history.clear(); }
    uint16_t max_layer = 0;
    {
      loop_events_guard_t guard;
      loop_events.clear();
      for (JsonObject item : doc["events"].as<JsonArray>()) {
        if (loop_events.size() >= loop_event_max) { break; }
        loop_event_t e;
        uint8_t page = item["page"] | (uint8_t)performance_page_t::sample;
        e.page = page < (uint8_t)performance_page_t::max
          ? (performance_page_t)page : performance_page_t::sample;
        e.pad = item["pad"] | def::pad::pad_count;
        e.type = parse_loop_event_type(item["type"] | "on");
        e.pos_ms = (item["pos"] | 0) % std::max<uint32_t>(1, loop_length_msec);
        e.layer = item["layer"] | 0;
        e.chord_flags = item["chordFlags"] | 0;
        if (e.pad < def::pad::pad_count) {
          loop_events.push_back(e);
          max_layer = std::max(max_layer, e.layer);
        }
      }
    }
    loop_layer_seq = max_layer + 1;
    invalidate_loop_timeline_cache();
    request_wave_draw();
  }
}

//-------------------------------------------------------------------------
// KANTAN Play base への電源投入 (main/main.cpp の起動手順と共通)

static void power_on_base(void)
{
#if !defined (M5UNIFIED_PC_BUILD)
  static constexpr const uint8_t aw9523_i2c_addr = 0x58;
  static constexpr const uint8_t port0_reg = 0x02;
  static constexpr const uint8_t port1_reg = 0x03;
  static constexpr const uint32_t port0_bitmask_bus_en = 0b00000010; // BUS EN
  static constexpr const uint32_t port1_bitmask_boost = 0b10000000; // BOOST_EN

  uint8_t buf[2];
  M5.In_I2C.readRegister(aw9523_i2c_addr, port0_reg, buf, sizeof(buf), 100000);
  uint8_t bus_en_on = buf[0] | port0_bitmask_bus_en;
  uint8_t boost_on = buf[1] | port1_bitmask_boost;
  uint8_t bus_en_off = bus_en_on & ~port0_bitmask_bus_en;

  M5.In_I2C.writeRegister8(aw9523_i2c_addr, port0_reg, bus_en_off, 100000);
  M5.In_I2C.writeRegister8(aw9523_i2c_addr, port1_reg, boost_on, 100000);

  // BUS_ENをオンにした直後に短時間に電流が大量に流れるのを避けるため、
  // 高速にオンオフを繰り返して、電流の立ち上がりを抑える。
  // これをしないとバッテリ電圧が低い状況下では起動に失敗することがある。
  for (int i = 0; i < 256; ++i) {
    M5.In_I2C.writeRegister8(aw9523_i2c_addr, port0_reg, bus_en_off, 400000);
    M5.In_I2C.writeRegister8(aw9523_i2c_addr, port0_reg, bus_en_on, 400000);
    m5gfx::delayMicroseconds(i);
  }
#endif
}

//-------------------------------------------------------------------------

static void init(void)
{
  // Keep loop recording allocation-free.  These vectors are touched from the
  // performance path, where a first-time heap growth can postpone a dense
  // synth hit by an audible amount.
  loop_events.reserve(loop_event_max);
  for (auto& history : loop_undo_history) { history.reserve(loop_event_max); }

  auto cfg = M5.config();
  cfg.output_power = false;
  cfg.internal_spk = false;
  M5.begin(cfg);

  M5.Display.setRotation(0);

#if defined (CONFIG_IDF_TARGET_ESP32S3)
  // CoreS3内蔵音源AW88298への電力供給を止める
  M5.Power.Axp2101.setALDO1(0);
  M5.Power.Axp2101.setBLDO2(0);

  // CoreS3内蔵カメラへの電力供給を止める
  M5.Power.Axp2101.setALDO3(0);
  M5.Power.Axp2101.setBLDO1(0);
#endif

  power_on_base();

#if !defined(M5UNIFIED_PC_BUILD)
  // Host利用後のソフトリセットでは、電源IC側のVBUS出力ラッチが残る機体がある。
  // USBタスクやPC接続を始める前に必ず出力を落とし、USB-Cをデバイス側へ安定復帰させる。
  M5.Power.setUsbOutput(false);
  M5.delay(220);
#endif

  M5.Power.setChargeCurrent(200);

  kp::system_registry = new kp::system_registry_t();
  kp::system_registry->init();
  // 演奏開始後のpush_backで再確保を起こさず、最初の録音音を優先する。
  loop_events.reserve(loop_event_max);
  // 共通アプリの既定値はUSBホスト給電オンだが、サンプラーは外部入力を
  // 明示的に選択するまで給電しない。起動中の不要なVBUS切替を避ける。
  kp::system_registry->midi_port_setting.setUSBPowerEnabled(false);
  M5.Power.setUsbOutput(false);
  audio.start();
  sampler_audio_t::setOutputGainPercent(fixed_output_gain_percent);
  for (uint8_t i = 0; i < 3; ++i) {
    sampler_audio_t::setFx(i, false, fx_param[i]);
  }
  sampler_audio_t::setMasterDelay(false);
  sampler_audio_t::setMasterDelayFrames(fx_delay_frames());
  sampler_audio_t::setTapeStop(false);
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_default_length_ms));
  if (!task_i2c.start()) {
    M5.Display.print("\nhardware not found.\n");
    M5.delay(4096);
    M5.Power.powerOff();
  }
  task_port_a.start();
  task_midi.start();
#if !defined(M5UNIFIED_PC_BUILD)
  sampler_web_command_mutex = xSemaphoreCreateMutex();
  touch_render_queue = xQueueCreate(1, sizeof(touch_render_state_t));
  ui_tile_render_queue = xQueueCreate(2, sizeof(ui_tile_transfer_t));
  ui_render_queue_set = xQueueCreateSet(3);
  touch_render_stopped = xSemaphoreCreateBinary();
  const bool ui_render_queues_ready = touch_render_queue != nullptr
                                  && ui_tile_render_queue != nullptr
                                  && ui_render_queue_set != nullptr
                                  && xQueueAddToSet(touch_render_queue, ui_render_queue_set) == pdPASS
                                  && xQueueAddToSet(ui_tile_render_queue, ui_render_queue_set) == pdPASS;
  if (ui_render_queues_ready && touch_render_stopped != nullptr) {
    if (xTaskCreatePinnedToCore(touch_render_task, "sampler_ui", 4096, nullptr,
                                kp::def::system::task_priority_spi,
                                &touch_render_task_handle,
                                kp::def::system::task_cpu_spi) != pdPASS) {
      touch_render_task_handle = nullptr;
    }
  }
  if (touch_render_task_handle == nullptr) {
    if (touch_render_queue != nullptr) { vQueueDelete(touch_render_queue); }
    if (ui_tile_render_queue != nullptr) { vQueueDelete(ui_tile_render_queue); }
    if (ui_render_queue_set != nullptr) { vQueueDelete(ui_render_queue_set); }
    if (touch_render_stopped != nullptr) { vSemaphoreDelete(touch_render_stopped); }
    touch_render_queue = nullptr;
    ui_tile_render_queue = nullptr;
    ui_render_queue_set = nullptr;
    touch_render_stopped = nullptr;
  }
#endif
  task_wifi.start();

  wave_canvas.setColorDepth(16);
  wave_canvas.createSprite(M5.Display.width(), wave_h);
  // The waveform canvas remains in internal RAM for responsive performance.
  // Menus are not rendered during Wi-Fi operations, so keeping their retained
  // surface in PSRAM leaves enough internal heap for TLS and OTA buffers.
  menu_canvas.setPsram(true);
  menu_canvas.setColorDepth(16);
  menu_canvas.createSprite(M5.Display.width(), menu_area_h);
  // ページ遷移専用の第二面はPSRAMへ置く。確保できない機体では従来の単面描画へ
  // 自動的にフォールバックするため、通常のメニュー操作は維持される。
  menu_transition_canvas.setPsram(true);
  menu_transition_canvas.setColorDepth(16);
  menu_transition_canvas.createSprite(M5.Display.width(), menu_area_h);
  menu_transition_canvas_ready = menu_transition_canvas.getBuffer() != nullptr;
  page_selector_canvas.setPsram(true);
  page_selector_canvas.setColorDepth(16);
  page_selector_canvas.createSprite(page_selector_w, page_selector_h);
  page_selector_canvas_ready = page_selector_canvas.getBuffer() != nullptr;
  touch_play_surface_canvas.setPsram(true);
  touch_play_surface_canvas.setColorDepth(16);
  touch_play_surface_canvas.createSprite(M5.Display.width(), M5.Display.height());
  touch_play_surface_canvas_ready = touch_play_surface_canvas.getBuffer() != nullptr;
  touch_play_surface_cache_key = 0xFF;
  touch_play_surface_cache_scale = 0xFF;
  create_ui_dirty_canvases();
  for (uint8_t i = 0; i < grid_cache_count; ++i) {
    grid_cache_canvas[i].setPsram(true);
    grid_cache_canvas[i].setColorDepth(16);
    grid_cache_canvas[i].createSprite(grid_cache_w, grid_cache_h);
    grid_cache_ready[i] = grid_cache_canvas[i].getBuffer() != nullptr;
    if (grid_cache_ready[i]) { reset_grid_cache(i); }
  }

  startup_loading_active = true;
  startup_loading_static_drawn = false;
  startup_loading_frame = 0;
  draw_startup_loading_frame("STARTING");
  std::fill(midi_note_assign, midi_note_assign + 128, (int16_t)midi_assign_target_t::none);
  std::fill(midi_cc_assign, midi_cc_assign + 128, (int16_t)midi_assign_target_t::none);
  std::fill(external_button_assign, external_button_assign + 32, (int16_t)midi_assign_target_t::none);
  load_sampler_folder_settings();
  if (!load_resume_kit()) {
    load_builtin_samples();
  }
  const bool applying_input_change_restart = consume_external_input_restart();
  if (!applying_input_change_restart
   && (external_input_mode == external_input_mode_t::usb_midi_host
    || external_input_mode == external_input_mode_t::usb_keyboard
    || external_input_mode == external_input_mode_t::usb_gamepad)) {
    // 通常起動ではUSB-CをPC接続・充電へ確実に戻す。メニュー変更による
    // 自動再起動だけは上のRTCマーカーで選択したHostモードを一度適用する。
    external_input_mode = external_input_mode_t::off;
    usb_host_disabled_on_boot = true;
    save_resume_kit();
  }
  startup_loading_active = false;
  apply_external_input_mode();
  // Input routing is the final startup stage that can touch MIDI state.
  // Reassert every performance tone only after the Kit and route are stable.
  repair_pitched_pad_sources();
  apply_synth_tones(true);
  startup_synth_restore_pending = true;
  startup_synth_restore_not_before = M5.millis() + 350;
  // The audio task started muted before I2S/codec setup.  Only release the
  // physical output after the restored kit and external-input route are
  // stable; sampler_audio performs the one-second fade from silence.
  sampler_audio_t::releaseStartupMute();
#if !defined(M5UNIFIED_PC_BUILD)
  // BLE MIDIを選んだ起動では自動確認を行わない。演奏用BLEを数秒後に切断して
  // Wi-Fiを起動する挙動と、無線初期化のメモリピークを避ける。更新確認は
  // メニューのUpdateから明示的に実行でき、その場合は排他的にBLEを停止する。
  startup_update_check_pending = wifi_auto_update_check
    && kp::task_wifi_t::hasSavedSTAConfig()
    && external_input_mode != external_input_mode_t::ble_midi;
  startup_update_check_active = false;
  startup_update_check_not_before = M5.millis() + 12000;
#endif

  input_history_code = kp::system_registry->internal_input.getHistoryCode();
  external_input_history_code = kp::system_registry->external_input.getHistoryCode();
  external_input_prev_bitmask = kp::system_registry->external_input.getPortAButtonBitmask();
  prev_bitmask = kp::system_registry->internal_input.getButtonBitmask();
  process_encoder_value(0, kp::system_registry->internal_input.getEncValue(0));
  process_encoder_value(1, kp::system_registry->internal_input.getEncValue(1));
  process_encoder_value(2, kp::system_registry->internal_input.getEncValue(2));

  draw_all();
  update_all_leds();
  if (usb_host_disabled_on_boot) {
    show_status_message("USB Host reset to Off", 2400, true);
  }

#if !defined (M5UNIFIED_PC_BUILD)
  // One clock tick may dispatch several voices plus pitch-bend MIDI. The
  // former 2KB stack was marginal for these nested C++ paths and could fail
  // only under dense loop automation, making the crash appear intermittent.
  xTaskCreatePinnedToCore(loop_clock_task, "loopclk", 1024 * 4, nullptr,
                          kp::def::system::task_priority_i2s - 1, nullptr,
                          kp::def::system::task_cpu_i2s);
#endif
}

static void service_startup_synth_restore(uint32_t now)
{
  if (!startup_synth_restore_pending
   || (int32_t)(now - startup_synth_restore_not_before) < 0) { return; }
  startup_synth_restore_pending = false;
  repair_pitched_pad_sources();
  apply_synth_tones(true);
}

static void service_startup_update_check(uint32_t now)
{
#if !defined(M5UNIFIED_PC_BUILD)
  if (!startup_update_check_pending || (int32_t)(now - startup_update_check_not_before) < 0) {
    return;
  }
  if (external_input_mode == external_input_mode_t::ble_midi) {
    startup_update_check_pending = false;
    return;
  }
  // 演奏やメニュー操作を始めた場合は、その瞬間を避けて静かなタイミングまで待つ。
  if (menu_visible || loop_playing || sound_priority_active(now)
   || wifi_update_active || wifi_file_server_qr_active) { return; }

  startup_update_check_pending = false;
  startup_update_check_active = true;
  begin_wifi_radio_request(wifi_radio_request_t::update_check);
#else
  (void)now;
#endif
}

static void service_startup_update_check_finish(void)
{
#if !defined(M5UNIFIED_PC_BUILD)
  if (!startup_update_check_active || kp::system_registry == nullptr) { return; }
  using namespace kp::def::command;
  auto reg = kp::system_registry;
  const uint8_t state = reg->runtime_info.getWiFiOtaProgress();
  const bool complete = state == (uint8_t)wifi_ota_state_t::ota_update_available
                     || state == (uint8_t)wifi_ota_state_t::ota_already_up_to_date
                     || state == (uint8_t)wifi_ota_state_t::ota_connection_error
                     || state == (uint8_t)wifi_ota_state_t::ota_wifi_connection_error
                     || state == (uint8_t)wifi_ota_state_t::ota_catalog_error
                     || state == (uint8_t)wifi_ota_state_t::ota_no_matching_firmware
                     || state == (uint8_t)wifi_ota_state_t::ota_update_failed;
  if (!complete || reg->wifi_control.getOperation() != wifi_operation_t::wfop_disable) { return; }
  startup_update_check_active = false;
  disable_wifi_and_clear_indicator();
#endif
}

static void update(void)
{
  // 外部MIDIは画面・ボタンの処理より先にSAMへ渡す。重いUI更新中でも
  // Note On/Offの順序を保ち、キーボード演奏の遅れを最小化する。
  process_external_midi_input();
  process_external_button_input();
  process_usb_keyboard_input();
  process_usb_gamepad_input();

  // 本体ボタン・タッチ入力の履歴を処理
  auto& input = kp::system_registry->internal_input;
  const kp::registry_base_t::history_t* h;
  while ((h = input.getHistory(input_history_code)) != nullptr) {
    const bool is_encoder = h->index == kp::system_registry_t::reg_internal_input_t::ENC1_VALUE
                         || h->index == kp::system_registry_t::reg_internal_input_t::ENC2_VALUE
                         || h->index == kp::system_registry_t::reg_internal_input_t::ENC3_VALUE;
    // モード切替やボタン押下の前に、それまでの回転だけは確定する。
    // これにより入力の意味上の順序を保ったまま、連続回転の描画を間引ける。
    if (!is_encoder) { flush_encoder_values(); }
    switch (h->index) {
    case kp::system_registry_t::reg_internal_input_t::BUTTON_BITMASK:
      process_bitmask(h->value, h->msec);
      break;
    case kp::system_registry_t::reg_internal_input_t::ENC1_VALUE:
      queue_encoder_value(0, h->value);
      break;
    case kp::system_registry_t::reg_internal_input_t::ENC2_VALUE:
      queue_encoder_value(1, h->value);
      break;
    case kp::system_registry_t::reg_internal_input_t::ENC3_VALUE:
      queue_encoder_value(2, h->value);
      break;
    case kp::system_registry_t::reg_internal_input_t::TOUCH_VALUE:
      process_touch(h->value);
      break;
    default:
      break;
    }
  }
  // 履歴キューの読み出しよりエンコーダーが先に進んでいる場合がある。
  // 現在カウントで上書きし、未反映の実移動量を1回で適用する。
  // これは加速度ではなく、メニューと各パラメーターで共通の追随処理。
  queue_encoder_value(0, input.getEncValue(0));
  queue_encoder_value(1, input.getEncValue(1));
  queue_encoder_value(2, input.getEncValue(2));
  flush_encoder_values();

  // オペレーターコマンド (電源オフ等) の処理
  kp::def::command::command_param_t cp;
  bool is_pressed;
  while (kp::system_registry->operator_command.getQueue(&opcmd_history_code, &cp, &is_pressed)) {
    if (cp.command == kp::def::command::system_control && is_pressed) {
      auto sc = (kp::def::command::system_control_t)cp.param;
      if (sc == kp::def::command::system_control_t::sc_power_off
       || sc == kp::def::command::system_control_t::sc_reset) {
        static bool resume_saved = false;
        if (!resume_saved) {
          resume_saved = true;
          save_resume_kit();
        }
        kp::system_registry->runtime_info.setPowerOff(sc);
      }
    }
  }

  uint32_t msec = M5.millis();

  // The audio engine uses this only for already-sustaining Pad voices. Keep
  // its lifetime in lockstep with the existing LCD/input priority window.
  sampler_audio_t::setPerformancePriority(sound_priority_active(msec));
  // The raw output-wave ring exists solely for the Sampler Play live scope.
  // Loop playback draws the piano roll instead, so stop producing cross-core
  // visual data there and return that small but continuous budget to audio.
  sampler_audio_t::setLiveWaveCapture(!menu_visible
    && learn_state == learn_state_t::idle
    && uses_incremental_wave_transfer());

  service_startup_synth_restore(msec);
  service_touch_play(msec);
  service_fx_speed(msec);
  service_bgm_scratch(msec);
  service_menu_feedback(msec);
  service_menu_settings_save(msec);
  service_synth_menu_preview(msec);
  service_learn_target_timeout(msec);
  service_usb_host_after_pc_disconnect(msec);
  service_usb_host_vbus_power();
  service_ble_connection_reset(msec);
  service_ble_device_ui(msec);
  service_wifi_radio_start();
  service_wifi_update();
  service_startup_update_check_finish();
  service_wifi_ble_resume(msec);
  service_performance_ui_arena(msec);
  if (wifi_update_active) { return; }
  service_sampler_web_storage_stop();
  service_sampler_web_command();
  service_performance_recording();
  service_startup_update_check(msec);
  service_pad_recording();
  service_sample_move_hold(msec);
  service_sample_add_hold(msec);
  service_sample_delete_confirm(msec);
  service_pending_sustain_analysis();
  service_sampler_sustain_cache();
  service_pending_session_save();
  service_fn_modifier_hint(msec);
  service_surface_sync(msec);
#if defined (M5UNIFIED_PC_BUILD)
  service_loop(msec);
#endif

  if (menu_visible || learn_state != learn_state_t::idle) {
    service_wifi_setup_result();
    service_wifi_setup_qr();
    static uint32_t prev_status_anim_msec = 0;
    if (menu_visible && status_message_busy && status_message_visible(msec) && msec - prev_status_anim_msec >= 120) {
      prev_status_anim_msec = msec;
      // The menu canvas is cached. Update only the popup border so the
      // loading animation remains visible without repainting the menu text.
      draw_busy_status_dots_tick();
      return;
    }
    if (menu_visible && status_message[0] && status_message_until && (int32_t)(msec - status_message_until) >= 0) {
      clear_status_message(true);
    }
    return;
  }

  if (edit_value_compact_visible && (int32_t)(msec - edit_value_activity_until) >= 0) {
    edit_value_compact_visible = false;
    request_wave_draw();
  }
  if (edit_notice != edit_notice_t::none
   && (int32_t)(msec - edit_notice_until_msec) >= 0) {
    edit_notice = edit_notice_t::none;
    edit_notice_until_msec = 0;
    request_wave_draw();
  }
  if (mixer_notice[0] && (int32_t)(msec - mixer_notice_until_msec) >= 0) {
    mixer_notice[0] = 0;
    mixer_notice_until_msec = 0;
    if (mixer_active) { request_wave_draw(); }
  }
  // A pending Mix uses a small white outline blink. Refresh only that one pad
  // while the Mixer is visible, leaving normal performance drawing untouched.
  static uint32_t mixer_pending_blink_msec = 0;
  if (mixer_active && mixer_pending_snapshot >= 0
   && msec - mixer_pending_blink_msec >= 250) {
    mixer_pending_blink_msec = msec;
    request_pad_draw(display_order_to_pad((uint8_t)(8 + mixer_pending_snapshot)));
  }

  // バッテリー残量/充電状態はI2Cタスクで更新されるため、ヘッダーも定期更新する。
  static uint32_t prev_header_msec = 0;
  if (msec - prev_header_msec >= 1000) {
    prev_header_msec = msec;
    if (!sound_priority_active(msec) && !ui_async_display_busy()) { draw_header(); }
    else { request_header_draw(); }
  }

  flush_dirty_ui(false);

  // 再生中Padのハイライト表示更新。ループ再生中は発音を優先し、Padの点滅追従を省く。
  static uint32_t prev_play_msec = 0;
  if (!loop_playing && msec - prev_play_msec >= 120 && !sound_priority_active(msec)) {
    prev_play_msec = msec;
    for (int i = 0; i < (int)def::pad::pad_count; ++i) {
      bool playing = sampler_audio_t::isPlaying(i);
      if (pads[i].playing_shown != playing) {
        pads[i].playing_shown = playing;
        update_pad_led(i);
        if (ui_async_display_busy()) { request_pad_state_draw(i); }
        else { draw_pad_frame(i); }
      }
    }
  }

  // REC/EDITのサンプル波形はイベント時のみ更新し、Playのリアルタイム波形とLoopカーソルだけ定期更新する。
  static uint32_t prev_wave_msec = 0;
  bool recording_notice = loop_recording_notice_active();
  if (recording_notice) {
    if (!loop_recording_notice_shown) {
      prev_wave_msec = msec;
      draw_wave();
    }
  } else {
    loop_recording_notice_shown = false;
  }
  uint32_t wave_interval = 0;
  if (!recording_notice
   && (current_mode == sampler_mode_t::mode_loop || current_mode == sampler_mode_t::mode_play)
   && loop_playing) {
    wave_interval = (sound_priority_active(msec) || physical_input_pending()) ? 0 : 50;
  } else if (current_mode == sampler_mode_t::mode_play
          && current_page == performance_page_t::sample) {
    // The old 90ms attack suppression made repeated playing look frozen.
    // During dense performance use a stable ~30fps producer; at rest retain
    // the smoother 60fps target. LCD transfer itself runs on Core 0.
    wave_interval = sound_attack_guard_active(msec) ? 0
                  : sound_priority_active(msec) ? 33 : 16;
  }
  if (!ui_surface_exclusive && !page_selector_visible
   && hold_progress_kind != hold_progress_kind_t::loop_clear
   && wave_interval != 0 && !wave_transfer_active
   && msec - prev_wave_msec >= wave_interval) {
    prev_wave_msec = msec;
    if ((current_mode == sampler_mode_t::mode_loop || current_mode == sampler_mode_t::mode_play) && loop_playing) {
      draw_loop_timeline(true);
    } else if (!sound_attack_guard_active(msec) && !wave_transfer_active) {
      draw_wave();
    }
  }
  flush_dirty_ui(false);
  // Sampleモードの試聴時だけ、選択Pad波形に再生位置を重ねる。
  service_sample_preview_cursor(msec);
  // Draw this last: the selector temporarily covers the wave area and must
  // remain above its incremental playback redraw.
  service_page_selector(msec);
  // Long-press feedback owns only a compact chip and is deliberately last,
  // so the loop cursor cannot paint over it.
  service_hold_progress(msec);
  // Queue at the very end of the UI phase. Core 0 performs the short transfer
  // while Core 1 returns to its input/audio scheduling loop.
  service_wave_transfer();
  // The length is a stopped-state overlay. Restore its small base-image area
  // after transport starts, or draw it after a stopped timeline finishes.
  service_loop_length_label_overlay();
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

void setup() {
  sampler_ns::init();
}

void loop() {
  sampler_ns::update();
  M5.delay(2);
}

#if !defined (M5UNIFIED_PC_BUILD) && !defined (ARDUINO)
extern "C" void app_main(void)
{
  setup();
  for (;;) {
    loop();
  }
}
#endif

#endif // defined (KANPLAY_SAMPLER)
