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
//   SDカード /sampler/samples/*.wav (PCM16 mono/stereo 〜48kHz) を起動時に名前順で最大12個
//   PSRAMプールへ読み込む。SDに無い場合は組み込みサンプルを使用。
//   演奏中は SD にアクセスしない (レスポンス保証のため)。

#if defined (KANPLAY_SAMPLER)

#include <ArduinoJson.h>
#include <M5Unified.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../common_define.hpp"
#include "../system_registry.hpp"
#include "../task_i2c.hpp"
#include "../task_midi.hpp"
#include "../task_wifi.hpp"
#include "../file_manage.hpp"

#include "sampler_define.hpp"
#include "sampler_audio.hpp"
#include "sampler_pool.hpp"
#include "sampler_samples.hpp"
#include "sampler_wav.hpp"

namespace sampler_ns {
namespace kp = kanplay_ns;
using def::mode::sampler_mode_t;

//-------------------------------------------------------------------------
// 状態

struct pad_state_t {
  bool pressed = false;
  bool playing_shown = false;  // 再生中ハイライトの表示状態 (再描画判定用)
};

static sampler_audio_t audio;
static kp::task_i2c_t task_i2c;
static kp::task_midi_t task_midi;
static kp::task_wifi_t task_wifi;

static sampler_mode_t current_mode = sampler_mode_t::mode_play;
static pad_state_t pads[def::pad::pad_count];
static bool fn_pressed[3] = { false, false, false };
static uint32_t fn_press_msec[3] = { 0, 0, 0 };
static int recording_pad = -1;
static int rec_wave_pad = -1;
static int16_t* recording_buffer = nullptr;
static uint32_t recording_frames = 0;
static uint16_t recording_seq = 1;
static int edit_pad = -1;
static uint8_t edit_param = 0;  // 0=Start, 1=End, 2=Volume, 3=Pitch
static uint32_t edit_value_activity_until = 0;
static bool edit_value_compact_visible = false;

enum class loop_event_type_t : uint8_t {
  note_on,
  note_off,
};

struct loop_event_t {
  uint8_t pad = 0;
  loop_event_type_t type = loop_event_type_t::note_on;
  uint32_t pos_ms = 0;
  uint16_t layer = 0;
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
static kp::registry_base_t::history_code_t input_history_code = 0;
static kp::registry_base_t::history_code_t opcmd_history_code = 0;
static int touch_pad = -1;  // タッチ演奏中のPad番号 (-1=なし)
static std::vector<loop_event_t> loop_events;
static bool loop_playing = false;
static uint32_t loop_start_msec = 0;
static uint32_t loop_prev_pos_ms = 0;
static uint16_t loop_layer_seq = 1;
static bool loop_record_enabled = true;
static bool loop_length_fixed = false;
static uint32_t loop_length_msec = 4000;
static bool loop_quantize_enabled = true;
static uint8_t loop_quantize_option_index = 2;  // 32分割
static uint8_t loop_note_off_quantize_option_index = 3;  // 64分割
static bool loop_pad_mute[def::pad::pad_count] = { false };
static uint16_t loop_active_layer[def::pad::pad_count] = { 0 };
static uint16_t loop_deferred_note_on_layer[def::pad::pad_count] = { 0 };
static bool loop_deferred_live_pad[def::pad::pad_count] = { false };
static uint32_t loop_deferred_live_pos_ms[def::pad::pad_count] = { 0 };
static bool loop_del_touched_pad = false;
static bool loop_recording_notice_shown = false;
static uint8_t fx_selected = 0;
static int8_t fx_param[3] = { 0, 0, 0 };
static bool loop_repeat_armed = false;
static bool loop_repeat_running = false;
static uint32_t loop_repeat_start_pos_ms = 0;
static uint32_t loop_repeat_length_ms = 0;
static uint32_t loop_repeat_started_msec = 0;
static uint32_t loop_repeat_prev_pos_ms = 0;
static constexpr const uint8_t loop_repeat_half_steps[] = { 16, 8, 4, 2, 1 };  // 8, 4, 2, 1, 0.5 step
static constexpr const char* loop_repeat_labels[] = { "8", "4", "2", "1", "0.5" };
static constexpr const uint8_t background_loop_voice = def::pad::pad_count;
static constexpr const uint8_t menu_preview_voice = def::pad::pad_count + 1;
struct background_loop_t {
  int16_t* pcm = nullptr;
  uint32_t frames = 0;
  uint32_t sample_rate = 44100;
  uint16_t volume_q8 = 192;
  uint8_t loop_repeats = 1;
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

static M5Canvas wave_canvas(&M5.Display);
static M5Canvas menu_canvas(&M5.Display);

static void loop_repeat_set_active(bool active);
static void draw_all(void);
static void draw_wave(void);
static bool ensure_sampler_sd_dirs(void);
static void load_builtin_samples(void);
static void load_builtin_background_loop(void);
static int load_sd_samples(void);
static void clear_kit(void);
static bool save_current_kit(void);
static bool load_kit_file(const char* path);
static void reload_samples_from_sd(void);
static bool load_wav_to_pad(uint8_t pad, const char* path, const char* display_name, char* error, size_t error_len);
static void clear_pad_sample(uint8_t pad, bool remove_loop_events);
static void clear_all_pad_samples(void);
static bool load_background_loop_file(const char* path, const char* display_name);
static bool load_background_loop_memory(const uint8_t* data, size_t len, const char* display_name, const char* file_path, uint8_t loop_repeats);
static void clear_menu_preview(void);
static bool play_menu_wav_preview(const char* path, uint32_t max_ms);
static void set_background_loop_error(const char* msg);
static void clear_background_loop(void);
static void play_background_loop_at(uint32_t pos_ms);
static void stop_background_loop(void);
static void loop_reset_recording_state(void);
static void stop_all_audio(void);

//-------------------------------------------------------------------------
// レイアウト定数 (240x320 縦画面)

static constexpr const int32_t header_h   = 24;
static constexpr const int32_t wave_y     = 25;
static constexpr const int32_t wave_h     = 112;
static constexpr const int32_t tab_y      = 143;
static constexpr const int32_t tab_h      = 30;
static constexpr const int32_t menu_area_y = wave_y;
static constexpr const int32_t menu_area_h = tab_y + tab_h - wave_y;
static constexpr const int32_t grid_y     = 180;
static constexpr const int32_t cell_h     = 44;
static constexpr const int32_t row_pitch  = 47;
static constexpr const int32_t pad_w      = 44;
static constexpr const int32_t col_pitch  = 47;
static constexpr const int32_t grid_x     = 2;
static constexpr const int32_t fn_x       = 194;
static constexpr const int32_t fn_w       = 44;
static constexpr const uint32_t recording_internal_sample_rate = 16000;  // CoreS3内蔵MicはM5Unified標準例に合わせる
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
static constexpr const uint32_t loop_del_long_press_ms = 800;
static constexpr const size_t loop_event_max = 96;

//-------------------------------------------------------------------------
// 色定義

static constexpr uint32_t led_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (r << 16) | (g << 8) | b;
}

static constexpr uint32_t led_from_rgb24(uint32_t rgb) {
  return led_rgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

struct mode_info_t {
  const char* name;
  uint32_t screen_color;
  uint32_t led_color;
};

static constexpr const mode_info_t mode_info[] = {
  { "REC",  0xE04040u, led_rgb(255,  32,  32) },
  { "PLAY", 0x40C040u, led_rgb( 32, 255,  32) },
  { "LOOP", 0x4080E0u, led_rgb( 32,  96, 255) },
  { "FX",   0xC040C0u, led_rgb(255,  32, 255) },
};

// モードごとのFnボタン機能名 (上から順)
static constexpr const char* const fn_labels[][3] = {
  { "EDIT", "REV",  "DEL"  },  // REC
  { "PLAY", "HOLD", "LOOP" },  // PLAY
  { "END",  "MUTE", "DEL"  },  // LOOP
  { "PITCH", "FILTER", "REPEAT" },  // FX
};
static constexpr const char* const edit_param_labels[4] = { "START", "END", "VOLUME", "PITCH" };

// Pad配色 { 画面通常, 画面押下, LED通常, LED押下 } : Pad位置で5色を巡回
struct pad_color_t { uint32_t bg; uint32_t bg_hi; uint32_t led; uint32_t led_hi; };
static constexpr const pad_color_t sample_colors[] = {
  { 0x703020u, 0xFF8040u, led_from_rgb24(0x703020u), led_from_rgb24(0xFF8040u) },
  { 0x707020u, 0xFFFF40u, led_from_rgb24(0x707020u), led_from_rgb24(0xFFFF40u) },
  { 0x207030u, 0x40FF70u, led_from_rgb24(0x207030u), led_from_rgb24(0x40FF70u) },
  { 0x207070u, 0x40FFFFu, led_from_rgb24(0x207070u), led_from_rgb24(0x40FFFFu) },
  { 0x602080u, 0xC040FFu, led_from_rgb24(0x602080u), led_from_rgb24(0xC040FFu) },
};
static constexpr const pad_color_t empty_color =
  { 0x282830u, 0x484858u, led_from_rgb24(0x282830u), led_from_rgb24(0x484858u) };
static constexpr const pad_color_t fn_color =
  { 0x303048u, 0x6060A0u, led_rgb(6, 6, 16), led_rgb(64, 64, 160) };

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
  if (!sampler_pool_t::slot[pad].isValid()) { return empty_color; }
  return sample_colors[pad % 5];
}

static bool pad_highlighted(int pad) {
  return pads[pad].pressed || pads[pad].playing_shown || recording_pad == pad || edit_pad == pad;
}

static bool any_pad_pressed(void)
{
  for (const auto& p : pads) {
    if (p.pressed) { return true; }
  }
  return false;
}

static bool fn_modifier_hint(int fn)
{
  if (edit_pad >= 0 || !any_pad_pressed()) { return false; }
  if (current_mode == sampler_mode_t::mode_rec) { return true; }
  return current_mode == sampler_mode_t::mode_play && fn != 0;
}

static void update_pad_led(int pad) {
  auto& c = pad_colors(pad);
  kp::system_registry->rgbled_control.setColor(pad_to_button(pad), pad_highlighted(pad) ? c.led_hi : c.led);
}

static void update_fn_led(int fn) {
  kp::system_registry->rgbled_control.setColor(fn_to_button(fn), fn_pressed[fn] ? fn_color.led_hi : fn_color.led);
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

//-------------------------------------------------------------------------
// 画面描画

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

static void draw_header(bool force = false) {
  auto& d = M5.Display;
  struct header_cache_t {
    bool valid = false;
    uint8_t battery = 255;
    bool charging = false;
    uint8_t volume = 255;
    size_t used = 0;
  };
  static header_cache_t cache;

  uint8_t battery = kp::system_registry->runtime_info.getBatteryLevel();
  if (battery > 100) { battery = 100; }
  battery = (battery + 2) / 5 * 5;  // 細かな揺れでアイコンが点滅しないように丸める
  bool charging = kp::system_registry->runtime_info.getBatteryCharging();
  uint8_t volume = kp::system_registry->user_setting.getMasterVolume();
  size_t used = sampler_pool_t::usedBytes();
  if (!force && cache.valid
   && cache.battery == battery
   && cache.charging == charging
   && cache.volume == volume
   && cache.used == used) {
    return;
  }
  cache.valid = true;
  cache.battery = battery;
  cache.charging = charging;
  cache.volume = volume;
  cache.used = used;

  d.startWrite();
  d.fillRect(0, 0, d.width(), header_h, 0x000000u);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::middle_left);
  d.setTextColor(0xFFFFFFu, 0x000000u);
  d.drawString(def::app::app_name, 4, header_h / 2);

  static constexpr const int32_t battery_icon_w = 14;
  static constexpr const int32_t icon_gap = 2;
  static constexpr const int32_t volume_icon_w = header_h;
  int32_t right_w = battery_icon_w + icon_gap + volume_icon_w;
  int32_t right_x = d.width() - right_w;

  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%01uMB"
    , (unsigned)(used >> 20)
    , (unsigned)(((used & 0xFFFFF) * 10) >> 20));
  d.setTextDatum(m5gfx::textdatum_t::middle_right);
  d.setTextColor(0xA0A0B0u, 0x000000u);
  d.drawString(buf, right_x - 4, header_h / 2);

  draw_battery_icon(right_x, 0, battery_icon_w, header_h, battery, charging);
  draw_volume_icon(right_x + battery_icon_w + icon_gap, 0, volume_icon_w, header_h, volume);
  d.endWrite();
}

static inline int32_t apply_wave_volume(int16_t value, uint16_t volume_q8)
{
  int32_t scaled = ((int32_t)value * volume_q8) >> 8;
  if (scaled > INT16_MAX) { return INT16_MAX; }
  if (scaled < INT16_MIN) { return INT16_MIN; }
  return scaled;
}

static uint32_t loop_display_length_ms(uint32_t now)
{
  if (loop_playing && loop_record_enabled && !loop_length_fixed) {
    uint32_t elapsed = now - loop_start_msec;
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
    return now - loop_start_msec;
  }
  return (now - loop_start_msec) % length_ms;
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

static bool loop_recording_notice_active(void)
{
  return current_mode == sampler_mode_t::mode_loop
      && loop_playing
      && loop_record_enabled
      && !loop_length_fixed;
}

static void push_wave_canvas(void)
{
  auto& c = wave_canvas;
  uint32_t color = mode_info[(int)current_mode].screen_color;
  c.drawRect(0, 0, c.width(), c.height(), color);
  c.drawRect(1, 1, c.width() - 2, c.height() - 2, color);
  c.pushSprite(0, wave_y);
}

// 32分グリッドを保ちながら、4分/8分の音楽的に強い位置へ少しだけ寄せる。
// scoreが小さい候補を選ぶため、強拍ほど通常の中間点より広く吸着する。
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

    // Q10重み: 4分 > 8分 > 16分 > 通常の32分位置。
    uint32_t weight = 1024;
    if (steps >= 4 && index % (steps / 4) == 0) {
      weight = 560;
    } else if (steps >= 8 && index % (steps / 8) == 0) {
      weight = 760;
    } else if (steps >= 16 && index % (steps / 16) == 0) {
      weight = 920;
    }
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
  if (requantize_existing && loop_quantize_enabled && loop_length_fixed) {
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
  // 強拍へ寄せた結果も実際の発音を待たせ、即時音とループ音の二重発音を防ぐ。
  uint32_t early_window_ms = std::min<uint32_t>(90, std::max<uint32_t>(25, (step_ms * 3) / 4));
  uint32_t ahead_ms = loop_forward_distance_ms(raw_pos, quantized_pos, loop_length_msec);
  return ahead_ms > 0 && ahead_ms <= early_window_ms;
}

static void quantize_loop_events_to_length(uint32_t length_ms)
{
  for (auto& e : loop_events) {
    e.pos_ms = e.type == loop_event_type_t::note_off
      ? quantize_loop_note_off_pos_ms(e.pos_ms, length_ms)
      : quantize_loop_pos_ms(e.pos_ms, length_ms);
  }
}

static void draw_loop_timeline(void)
{
  auto& c = wave_canvas;
  const int w = c.width();
  const int h = c.height();
  c.fillScreen(0x080810u);
  if (loop_recording_notice_active()) {
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(2);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextColor(0xFF7070u, 0x080810u);
    c.drawString("RECORDING", w / 2, h / 2);
    if (background_loop.isValid()) {
      c.setTextSize(1);
      c.setTextDatum(m5gfx::textdatum_t::top_left);
      c.setTextColor(0xB0E0FFu, 0x080810u);
      char bgm[40];
      snprintf(bgm, sizeof(bgm), "BGM %.1fs", (double)background_loop_length_ms() / 1000.0);
      c.drawString(bgm, 3, 2);
    }
    loop_recording_notice_shown = true;
    push_wave_canvas();
    return;
  }
  c.drawFastHLine(0, h / 2, w, 0x204060u);
  uint32_t length_ms = loop_display_length_ms(M5.millis());
  for (int step = 0; step <= 16; ++step) {
    int x = (step * (w - 1)) / 16;
    bool major = (step % 4) == 0;
    c.drawFastVLine(x, 0, h, step == 0 ? 0x5070B0u : (major ? 0x304870u : 0x1A2438u));
  }
  int lane_h = std::max<int>(1, (h - 16) / def::pad::pad_count);
  int mark_h = std::min<int>(6, std::max<int>(3, lane_h - 1));
  auto lane_y_for_pad = [lane_h](uint8_t pad) {
    int display_order = (int)pad_display_number(pad) - 1;
    return 8 + ((int)def::pad::pad_count - 1 - display_order) * lane_h;
  };
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    if (loop_pad_mute[pad]) {
      int y = lane_y_for_pad((uint8_t)pad) + mark_h / 2;
      c.drawFastHLine(0, y, w, 0x303038u);
    }
  }
  for (const auto& e : loop_events) {
    if (e.pad >= def::pad::pad_count) { continue; }
    int x = ((uint64_t)e.pos_ms * (w - 1)) / length_ms;
    int y = lane_y_for_pad(e.pad);
    uint32_t color = loop_pad_mute[e.pad]
      ? 0x606068u
      : sample_colors[e.pad % (sizeof(sample_colors) / sizeof(sample_colors[0]))].bg_hi;
    if (e.type == loop_event_type_t::note_off) {
      c.drawFastHLine(x - 2, y + mark_h / 2, 5, color);
    } else {
      c.fillRect(x - 1, y, 3, mark_h, color);
    }
  }
  int play_x = ((uint64_t)loop_pos_ms(M5.millis()) * (w - 1)) / length_ms;
  c.drawFastVLine(play_x, 0, h, loop_playing ? 0xFFFFFFu : 0x808090u);
  if (background_loop.isValid()) {
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(0xB0E0FFu, 0x080810u);
    char bgm[40];
    snprintf(bgm, sizeof(bgm), "BGM %.1fs", (double)background_loop_length_ms() / 1000.0);
    c.drawString(bgm, 3, 2);
  }
  push_wave_canvas();
}

static void draw_fx_panel(void)
{
  auto& c = wave_canvas;
  const int w = c.width();
  const int h = c.height();
  c.fillScreen(0x100818u);
  for (int i = 0; i < 3; ++i) {
    int row_h = h / 3;
    int y = i * row_h + 5;
    uint32_t color = i == 0 ? 0x40B0FFu : i == 1 ? 0xFFC040u : 0x80FF80u;
    bool held = fn_pressed[i];
    if (!held) { color = 0x606060u; }
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(i == fx_selected ? 0xFFFFFFu : 0xB0B0C0u, 0x100818u);
    c.drawString(fn_labels[(int)sampler_mode_t::mode_fx][i], 4, y);
    int bx = 58;
    int bw = w - bx - 42;
    int by = y + 4;
    int bh = 24;
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
    } else {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0]) - 1));
      c.fillRect(bx + 1, by + 4, ((bw - 2) * (index + 1)) / (sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0])), bh - 7, color);
    }
    char value[12];
    if (i == 2) {
      uint8_t index = std::min<uint8_t>((uint8_t)fx_param[i], (uint8_t)(sizeof(loop_repeat_labels) / sizeof(loop_repeat_labels[0]) - 1));
      snprintf(value, sizeof(value), "%s", loop_repeat_labels[index]);
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

static inline uint32_t display_wave_sample_index(const sample_slot_t& slot, uint32_t index)
{
  if (index >= slot.frames) { index = slot.frames - 1; }
  return slot.reverse ? (slot.frames - 1 - index) : index;
}

static void draw_wave(void) {
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
      for (uint32_t i = a; i < b; ++i) {
        int16_t v = (int16_t)apply_wave_volume(slot.pcm[display_wave_sample_index(slot, i)], slot.volume_q8);
        if (mn > v) { mn = v; }
        if (mx < v) { mx = v; }
      }
      int y0 = (h / 2) - ((int)mx * (h / 2 - 3)) / 32768;
      int y1 = (h / 2) - ((int)mn * (h / 2 - 3)) / 32768;
      if (y1 <= y0) { y1 = y0 + 1; }
      c.drawFastVLine(x, y0, y1 - y0, 0xD0B050u);
    }
    draw_sample_points(c, slot, true);
    uint32_t accent = edit_param == 0 ? 0xFF7050u : edit_param == 1 ? 0x50A0FFu
                    : edit_param == 2 ? 0x60E080u : 0xB080FFu;
    char value[16];
    if (edit_param == 0) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playStart() / slot.sample_rate : 0.0f);
    } else if (edit_param == 1) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playEnd() / slot.sample_rate : 0.0f);
    } else if (edit_param == 2) {
      snprintf(value, sizeof(value), "%u%%", (unsigned)((slot.volume_q8 * 100u) / 256u));
    } else {
      snprintf(value, sizeof(value), "%u%%", (unsigned)((slot.pitch_q8 * 100u) / 256u));
    }
    bool compact_chip = edit_value_compact_visible
      && (int32_t)(edit_value_activity_until - M5.millis()) > 0;
    if (!compact_chip) { edit_value_compact_visible = false; }
    const int chip_w = compact_chip ? std::max<int>(46, (int)strlen(value) * 10 + 18) : 80;
    const int chip_h = compact_chip ? 24 : 42;
    const int chip_x = (w - chip_w) / 2;
    const int chip_y = (h - chip_h) / 2;
    c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x040408u);
    c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x303038u);
    c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, accent);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setFont(&fonts::efontJA_16_b);
    c.setTextSize(1);
    if (compact_chip) {
      c.setTextColor(accent);
      c.drawString(value, w / 2, chip_y + chip_h / 2);
    } else {
      c.setTextColor(0xFFFFFFu);
      c.drawString(edit_param_labels[edit_param], w / 2, chip_y + 12);
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
    push_wave_canvas();
    return;
  }
  if (current_mode == sampler_mode_t::mode_rec) {
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
        for (uint32_t i = a; i < b; ++i) {
          int16_t v = (int16_t)apply_wave_volume(slot.pcm[display_wave_sample_index(slot, i)], slot.volume_q8);
          if (mn > v) { mn = v; }
          if (mx < v) { mx = v; }
        }
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

static void draw_pad(int pad) {
  auto& d = M5.Display;
  auto& c = pad_colors(pad);
  auto& slot = sampler_pool_t::slot[pad];
  const int x = grid_x + (pad % 4) * col_pitch;
  const int y = grid_y + (pad / 4) * row_pitch;
  const bool hi = pad_highlighted(pad);
  d.startWrite();
  d.fillRoundRect(x, y, pad_w, cell_h, 6, hi ? c.bg_hi : c.bg);
  if (recording_pad == pad) {
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
    uint32_t start = slot.playStart();
    uint32_t frames = slot.playFrames();
    // ミュート中は波形を減光してひと目で分かるようにする
    bool muted = loop_pad_mute[pad];
    uint32_t wave_color = muted ? (hi ? 0x303030u : 0x686868u) : (hi ? 0x101010u : 0xF0D060u);
    uint32_t center_color = muted ? 0x404040u : (hi ? 0x404040u : 0x805020u);
    d.drawFastHLine(wx, cy, ww, center_color);
    for (int px = 0; px < ww; ++px) {
      uint32_t a = start + ((uint64_t)px * frames) / ww;
      uint32_t b = start + ((uint64_t)(px + 1) * frames) / ww;
      if (b <= a) { b = std::min<uint32_t>(a + 1, slot.playEnd()); }
      int16_t mn = INT16_MAX;
      int16_t mx = INT16_MIN;
      uint32_t first_bin = ((uint64_t)a * sample_slot_t::waveform_bins) / slot.frames;
      uint32_t last_bin = ((uint64_t)(b ? b - 1 : 0) * sample_slot_t::waveform_bins) / slot.frames;
      if (first_bin >= sample_slot_t::waveform_bins) { first_bin = sample_slot_t::waveform_bins - 1; }
      if (last_bin >= sample_slot_t::waveform_bins) { last_bin = sample_slot_t::waveform_bins - 1; }
      for (uint32_t bin = first_bin; bin <= last_bin; ++bin) {
        int16_t min_value = (int16_t)apply_wave_volume(slot.waveform_min[bin], slot.volume_q8);
        int16_t max_value = (int16_t)apply_wave_volume(slot.waveform_max[bin], slot.volume_q8);
        if (mn > min_value) { mn = min_value; }
        if (mx < max_value) { mx = max_value; }
      }
      int y0 = cy - ((int)mx * (wh / 2 - 1)) / 32768;
      int y1 = cy - ((int)mn * (wh / 2 - 1)) / 32768;
      if (y1 <= y0) { y1 = y0 + 1; }
      d.drawFastVLine(wx + px, y0, y1 - y0, wave_color);
    }
    // 再生方式/ミュートのアイコンバッジ。縁取りを避け、単色面に大きめに描く。
    int bx = x + pad_w - 10;
    int by = y + 10;
    uint32_t plate = hi ? 0x202028u : 0x08080Cu;
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
  d.endWrite();
}

static constexpr const uint32_t edit_start_color = 0xFF7050u;
static constexpr const uint32_t edit_end_color   = 0x50A0FFu;
static constexpr const uint32_t edit_vol_color   = 0x60E080u;
static constexpr const uint32_t edit_pitch_color = 0xB080FFu;

static void draw_fn(int fn) {
  auto& d = M5.Display;
  const int y = grid_y + fn * row_pitch;
  bool fx_mode = edit_pad < 0 && current_mode == sampler_mode_t::mode_fx;
  bool active = fn_pressed[fn];
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
  if (fx_mode && fx_selected == fn && !active) { bg = 0x503060u; }
  d.startWrite();
  d.fillRoundRect(fn_x, y, fn_w, cell_h, 6, bg);
  if (modifier_hint) {
    d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, 0x585878u);
  }

  // FXモードは文字表示のまま
  if (fx_mode) {
    if (fn_pressed[fn]) {
      d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, 0xF0D060u);
    }
    d.setFont(&fonts::efontJA_16_b);
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(active ? 0xFFFFFFu : 0x9090C0u);
    int tx = fn_x + fn_w / 2;
    int ty = y + cell_h / 2;
    d.drawString(fn_labels[(int)current_mode][fn], tx, ty);
    d.endWrite();
    return;
  }

  const int cx = fn_x + fn_w / 2;
  const int cy = y + cell_h / 2;
  const int s = 9;
  uint32_t color = active ? 0xFFFFFFu : modifier_hint ? 0xB0B0D0u : 0x9090C0u;

  if (edit_pad >= 0) {
    // EDIT中: [START/ENDトグル] [VOLUME] [EXIT]
    if (fn == 0) {
      uint32_t accent = edit_param == 0 ? edit_start_color
                      : edit_param == 1 ? edit_end_color : color;
      if (edit_param <= 1) { d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, accent); }
      draw_icon(d, icon_t::pencil, cx, cy, s, active ? 0xFFFFFFu : accent, bg);
      // 編集対象が始点か終点かをバーの位置で示す
      if (edit_param == 0) { d.fillRect(fn_x + 4, y + 6, 2, cell_h - 12, edit_start_color); }
      if (edit_param == 1) { d.fillRect(fn_x + fn_w - 6, y + 6, 2, cell_h - 12, edit_end_color); }
    } else if (fn == 1) {
      uint32_t accent = edit_param == 3 ? edit_pitch_color : edit_vol_color;
      if (edit_param >= 2) { d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, accent); }
      draw_icon(d, icon_t::volume, cx, cy, s, active ? 0xFFFFFFu : (edit_param >= 2 ? accent : color), bg);
      if (edit_param == 3) {
        d.setFont(&fonts::efontJA_16_b);
        d.setTextSize(1);
        d.setTextDatum(m5gfx::textdatum_t::bottom_right);
        d.setTextColor(active ? 0xFFFFFFu : edit_pitch_color);
        d.drawString("P", fn_x + fn_w - 4, y + cell_h - 3);
      }
    } else {
      draw_icon(d, icon_t::exit_door, cx, cy, s, color, bg);
    }
    d.endWrite();
    return;
  }

  switch (current_mode) {
  case sampler_mode_t::mode_rec: {
    static constexpr const icon_t icons[] = { icon_t::pencil, icon_t::reverse, icon_t::trash };
    draw_icon(d, icons[fn], cx, cy, s, color, bg);
    break; }
  case sampler_mode_t::mode_play: {
    if (fn == 0) {
      draw_icon(d, loop_playing ? icon_t::stop : icon_t::play, cx, cy, s,
                active ? 0xFFFFFFu : loop_playing ? 0xFF7070u : 0x70E080u, bg);
    } else if (fn == 1) {
      draw_icon(d, icon_t::hold_gate, cx, cy, s, color, bg);
    } else {
      draw_icon(d, icon_t::loop_arrow, cx, cy, s, color, bg);
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
  d.endWrite();
}

static void draw_grid(void) {
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { draw_pad(i); }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static void draw_all(void) {
  M5.Display.fillScreen(0x101018u);
  draw_header(true);
  draw_wave();
  draw_tabs();
  draw_grid();
}

//-------------------------------------------------------------------------
// 簡易メニュー

enum class menu_page_t : uint8_t {
  root,
  kit,
  kit_edit,
  loop,
  input_assign,
  connections,
  wifi,
  audio,
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
  midi_input,
  usb_mode,
  usb_host_power,
  wifi_file_server,
  audio_input_source,
  display_brightness,
  led_brightness,
  language,
};

enum class menu_action_t : uint8_t {
  none,
  kit_load,
  kit_save,
  kit_new,
  kit_reload_samples,
  kit_assign_wav,
  kit_clear_pad,
  kit_clear_all_pads,
  kit_pad_list,
  background_load,
  background_clear,
  loop_clear,
  loop_stop,
  input_learn,
  input_assign_list,
  input_clear_all,
  wifi_info,
  system_info,
  reset_all_settings,
};

struct sampler_menu_item_t {
  const char* label;
  menu_item_kind_t kind;
  menu_page_t child;
  menu_value_t value;
  menu_action_t action;
};

static constexpr const sampler_menu_item_t menu_root_items[] = {
  { "Kit",          menu_item_kind_t::submenu, menu_page_t::kit,          menu_value_t::none, menu_action_t::none },
  { "Loop",         menu_item_kind_t::submenu, menu_page_t::loop,         menu_value_t::none, menu_action_t::none },
  { "Input Assign", menu_item_kind_t::submenu, menu_page_t::input_assign, menu_value_t::none, menu_action_t::none },
  { "Connections",  menu_item_kind_t::submenu, menu_page_t::connections,  menu_value_t::none, menu_action_t::none },
  { "Wi-Fi",        menu_item_kind_t::submenu, menu_page_t::wifi,         menu_value_t::none, menu_action_t::none },
  { "Audio",        menu_item_kind_t::submenu, menu_page_t::audio,        menu_value_t::none, menu_action_t::none },
  { "System",       menu_item_kind_t::submenu, menu_page_t::system,       menu_value_t::none, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_kit_items[] = {
  { "Load Kit",       menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_load },
  { "Save Kit",       menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_save },
  { "Import Sample",  menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_assign_wav },
  { "New Kit",        menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_new },
  { "Reload Samples", menu_item_kind_t::action,  menu_page_t::root, menu_value_t::none, menu_action_t::kit_reload_samples },
};

static constexpr const sampler_menu_item_t menu_kit_edit_items[] = {
  { "Import Sample",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_assign_wav },
  { "Clear Pad",      menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_clear_pad },
  { "Clear All Pads", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_clear_all_pads },
  { "Pad List",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::kit_pad_list },
};

static constexpr const sampler_menu_item_t menu_loop_items[] = {
  { "Load BGM",      menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::background_load },
  { "Clear BGM",     menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::background_clear },
  { "BGM Volume",    menu_item_kind_t::value,  menu_page_t::root, menu_value_t::background_volume,  menu_action_t::none },
  { "Quantize",      menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_quantize,      menu_action_t::none },
  { "Note Grid",     menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_note_grid,     menu_action_t::none },
  { "Note Off Grid", menu_item_kind_t::value,  menu_page_t::root, menu_value_t::loop_note_off_grid, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_input_items[] = {
  { "Learn",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_learn },
  { "Assign List", menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_assign_list },
  { "Clear All",   menu_item_kind_t::action, menu_page_t::root, menu_value_t::none, menu_action_t::input_clear_all },
};

static constexpr const sampler_menu_item_t menu_connections_items[] = {
  { "MIDI Input",     menu_item_kind_t::value, menu_page_t::root, menu_value_t::midi_input,     menu_action_t::none },
  { "USB Mode",       menu_item_kind_t::value, menu_page_t::root, menu_value_t::usb_mode,       menu_action_t::none },
  { "USB Host Power", menu_item_kind_t::value, menu_page_t::root, menu_value_t::usb_host_power, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_wifi_items[] = {
  { "File Server", menu_item_kind_t::value,  menu_page_t::root, menu_value_t::wifi_file_server, menu_action_t::none },
  { "Wi-Fi Info",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,             menu_action_t::wifi_info },
};

static constexpr const sampler_menu_item_t menu_audio_items[] = {
  { "Input Source", menu_item_kind_t::value, menu_page_t::root, menu_value_t::audio_input_source, menu_action_t::none },
};

static constexpr const sampler_menu_item_t menu_system_items[] = {
  { "Display",    menu_item_kind_t::value,  menu_page_t::root, menu_value_t::display_brightness, menu_action_t::none },
  { "LED",        menu_item_kind_t::value,  menu_page_t::root, menu_value_t::led_brightness,     menu_action_t::none },
  { "Language",   menu_item_kind_t::value,  menu_page_t::root, menu_value_t::language,           menu_action_t::none },
  { "Info",       menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::system_info },
  { "Reset All",  menu_item_kind_t::action, menu_page_t::root, menu_value_t::none,               menu_action_t::reset_all_settings },
};

static bool menu_visible = false;
static menu_page_t menu_page = menu_page_t::root;
static uint8_t menu_cursor = 0;
static uint8_t menu_depth = 0;
static char status_message[96] = { 0 };
static uint32_t status_message_until = 0;  // 0なら明示的に消すまで表示
static bool status_message_busy = false;
static uint32_t status_message_anim_msec = 0;

enum class kit_edit_state_t : uint8_t {
  idle,
  select_kit_file,
  select_wav,
  select_bgm_wav,
  assign_wait_pad,
  clear_wait_pad,
  pad_list,
};
static kit_edit_state_t kit_edit_state = kit_edit_state_t::idle;
static std::vector<kp::file_info_string_t> kit_wav_list;
static char kit_wav_dir[24] = { 0 };
static char kit_pending_wav_path[96] = { 0 };
static char kit_pending_wav_name[40] = { 0 };

enum class learn_state_t : uint8_t {
  idle,
  waiting_target,
  waiting_external,
};
static learn_state_t learn_state = learn_state_t::idle;
static char learn_target_label[16] = { 0 };

static void menu_sound_cursor(uint8_t display_number)
{
  auto reg = kp::system_registry;
  if (!reg->user_setting.getGuideSound()) { return; }
  int value = display_number >= 200 ? display_number / 10 : display_number;
  reg->player_command.addQueue(
    kp::def::command::command_param_t(uint16_t((uint16_t(value) << 8) | kp::def::command::menu_cursor_sound)));
}

static void menu_sound_navigate(uint8_t type)
{
  auto reg = kp::system_registry;
  if (!reg->user_setting.getGuideSound()) { return; }
  uint8_t level = menu_depth;
  if (type == 0 || type == 3) { level = 1; }
  if (level < 1) { level = 1; }
  if (level > 6) { level = 6; }
  reg->player_command.addQueue({ kp::def::command::menu_navigate_sound, uint8_t(((type & 0x0F) << 4) | (level & 0x0F)) });
}

static const sampler_menu_item_t* menu_items(menu_page_t page, size_t* count)
{
  switch (page) {
  default:
  case menu_page_t::root:         *count = sizeof(menu_root_items) / sizeof(menu_root_items[0]); return menu_root_items;
  case menu_page_t::kit:          *count = sizeof(menu_kit_items) / sizeof(menu_kit_items[0]); return menu_kit_items;
  case menu_page_t::kit_edit:     *count = sizeof(menu_kit_edit_items) / sizeof(menu_kit_edit_items[0]); return menu_kit_edit_items;
  case menu_page_t::loop:         *count = sizeof(menu_loop_items) / sizeof(menu_loop_items[0]); return menu_loop_items;
  case menu_page_t::input_assign: *count = sizeof(menu_input_items) / sizeof(menu_input_items[0]); return menu_input_items;
  case menu_page_t::connections:  *count = sizeof(menu_connections_items) / sizeof(menu_connections_items[0]); return menu_connections_items;
  case menu_page_t::wifi:         *count = sizeof(menu_wifi_items) / sizeof(menu_wifi_items[0]); return menu_wifi_items;
  case menu_page_t::audio:        *count = sizeof(menu_audio_items) / sizeof(menu_audio_items[0]); return menu_audio_items;
  case menu_page_t::system:       *count = sizeof(menu_system_items) / sizeof(menu_system_items[0]); return menu_system_items;
  }
}

static const char* menu_page_title(menu_page_t page)
{
  switch (page) {
  default:
  case menu_page_t::root: return "Menu";
  case menu_page_t::kit: return "Kit";
  case menu_page_t::kit_edit: return "Edit Pad";
  case menu_page_t::loop: return "Loop";
  case menu_page_t::input_assign: return "Input Assign";
  case menu_page_t::connections: return "Connections";
  case menu_page_t::wifi: return "Wi-Fi";
  case menu_page_t::audio: return "Audio";
  case menu_page_t::system: return "System";
  }
}

static menu_page_t menu_parent_page(menu_page_t page)
{
  switch (page) {
  case menu_page_t::kit_edit: return menu_page_t::kit;
  case menu_page_t::kit:
  case menu_page_t::loop:
  case menu_page_t::input_assign:
  case menu_page_t::connections:
  case menu_page_t::wifi:
  case menu_page_t::audio:
  case menu_page_t::system:
  default:
    return menu_page_t::root;
  }
}

static uint8_t menu_parent_cursor(menu_page_t page)
{
  switch (page) {
  case menu_page_t::kit: return 0;
  case menu_page_t::loop: return 1;
  case menu_page_t::input_assign: return 2;
  case menu_page_t::connections: return 3;
  case menu_page_t::wifi: return 4;
  case menu_page_t::audio: return 5;
  case menu_page_t::system: return 6;
  case menu_page_t::kit_edit: return 2;
  default: return 0;
  }
}

static uint8_t menu_page_depth(menu_page_t page)
{
  switch (page) {
  case menu_page_t::root: return 0;
  case menu_page_t::kit_edit: return 2;
  default: return 1;
  }
}

static uint8_t menu_dynamic_depth(void)
{
  switch (kit_edit_state) {
  case kit_edit_state_t::select_wav:
  case kit_edit_state_t::assign_wait_pad:
    return 2;
  case kit_edit_state_t::clear_wait_pad:
  case kit_edit_state_t::pad_list:
    return 3;
  case kit_edit_state_t::select_kit_file:
  case kit_edit_state_t::select_bgm_wav:
    return 2;
  default:
    return menu_page_depth(menu_page);
  }
}

static int menu_value_count(menu_value_t value)
{
  switch (value) {
  case menu_value_t::loop_quantize:
  case menu_value_t::usb_host_power:
  case menu_value_t::wifi_file_server:
  case menu_value_t::language:
    return 2;
  case menu_value_t::loop_note_grid:
  case menu_value_t::loop_note_off_grid:
  case menu_value_t::background_volume:
  case menu_value_t::midi_input:
  case menu_value_t::display_brightness:
  case menu_value_t::led_brightness:
    return 5;
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
  case menu_value_t::loop_note_grid: return loop_quantize_option_index;
  case menu_value_t::loop_note_off_grid: return loop_note_off_quantize_option_index;
  case menu_value_t::background_volume: {
    static constexpr uint16_t volumes[] = { 0, 64, 128, 192, 256 };
    int best = 0;
    int best_diff = 9999;
    for (int i = 0; i < 5; ++i) {
      int diff = abs((int)background_loop.volume_q8 - (int)volumes[i]);
      if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best; }
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
  case menu_value_t::wifi_file_server: return reg->wifi_control.getWebServerMode() == kp::def::command::webserver_mode_t::ws_enable ? 1 : 0;
  case menu_value_t::audio_input_source: return (int)recording_source_mode;
  case menu_value_t::display_brightness: return reg->user_setting.getDisplayBrightness();
  case menu_value_t::led_brightness: return reg->user_setting.getLedBrightness();
  case menu_value_t::language: return reg->user_setting.getLanguage() == kp::def::lang::language_t::ja ? 1 : 0;
  default: return 0;
  }
}

static const char* menu_value_text(menu_value_t value, int index)
{
  static char buf[16];
  static constexpr const char* off_on[] = { "Off", "On" };
  static constexpr const char* grids[] = { "8", "16", "32", "64", "128" };
  static constexpr const char* bgm_volumes[] = { "0", "25", "50", "75", "100" };
  static constexpr const char* midi_inputs[] = { "Off", "USB", "BLE", "PortC", "All" };
  static constexpr const char* usb_modes[] = { "Host", "Device" };
  static constexpr const char* input_sources[] = { "Auto", "Internal", "External" };
  static constexpr const char* langs[] = { "EN", "JP" };
  switch (value) {
  case menu_value_t::loop_quantize:
  case menu_value_t::usb_host_power:
  case menu_value_t::wifi_file_server:
    return off_on[index ? 1 : 0];
  case menu_value_t::loop_note_grid:
  case menu_value_t::loop_note_off_grid:
    return grids[std::min<int>(index, 4)];
  case menu_value_t::background_volume:
    return bgm_volumes[std::min<int>(index, 4)];
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

static void menu_value_set(menu_value_t value, int index)
{
  auto reg = kp::system_registry;
  int count = menu_value_count(value);
  if (count <= 0) { return; }
  if (index < 0) { index = count - 1; }
  if (index >= count) { index = 0; }
  switch (value) {
  case menu_value_t::loop_quantize:
    set_loop_quantize_enabled(index != 0);
    break;
  case menu_value_t::loop_note_grid:
    set_loop_quantize_option((uint8_t)index, false);
    break;
  case menu_value_t::loop_note_off_grid:
    set_loop_note_off_quantize_option((uint8_t)index, false);
    break;
  case menu_value_t::background_volume: {
    static constexpr uint16_t volumes[] = { 0, 64, 128, 192, 256 };
    background_loop.volume_q8 = volumes[std::min<int>(index, 4)];
    if (loop_playing && background_loop.isValid()) {
      uint32_t start_frame = ((uint64_t)loop_pos_ms(M5.millis()) * background_loop.sample_rate) / 1000;
      if (background_loop.frames) { start_frame %= background_loop.frames; }
      sampler_audio_t::play(background_loop_voice, background_loop.pcm, background_loop.frames,
                            background_loop.sample_rate, true, false, background_loop.volume_q8, 256, start_frame);
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
  case menu_value_t::wifi_file_server:
    if (index) {
      reg->wifi_control.setWebServerMode(kp::def::command::webserver_mode_t::ws_enable);
      reg->wifi_control.setOperation(kp::def::command::wifi_operation_t::wfop_web_filer);
    } else {
      reg->wifi_control.setWebServerMode(kp::def::command::webserver_mode_t::ws_disable);
      reg->wifi_control.setOperation(kp::def::command::wifi_operation_t::wfop_disable);
      reg->wifi_control.setWifiMode(kp::def::command::wifi_mode_t::wifi_disable);
    }
    break;
  case menu_value_t::audio_input_source:
    recording_source_mode = (recording_source_mode_t)index;
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
  reg->save();
}

static void clear_status_message(bool redraw = true);
static void draw_menu(bool redraw_keypad = false);

static bool status_message_visible(uint32_t now = M5.millis())
{
  return status_message[0] && (status_message_until == 0 || (int32_t)(status_message_until - now) > 0);
}

static uint32_t status_busy_frame_color(void)
{
  uint32_t phase = ((M5.millis() - status_message_anim_msec) / 120) & 0x07;
  if (phase > 4) { phase = 8 - phase; }
  uint8_t v = 110 + phase * 32;
  return ((uint32_t)v << 16) | ((uint32_t)v << 8) | 0xFFu;
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
    d.drawString("Press target", 120, wave_y + 64);
  } else if (learn_state == learn_state_t::waiting_external) {
    d.setTextColor(0xC0C0D0u, 0x08080Cu);
    d.drawString("Press external input", 120, wave_y + 58);
    d.setTextColor(0x80D0FFu, 0x08080Cu);
    d.drawString(learn_target_label, 120, wave_y + 86);
  }
  d.endWrite();
}

static const char* menu_button_label(int btn)
{
  static char wait_pad_labels[15][4];
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    if (btn == 4) { return "Back"; }
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      snprintf(wait_pad_labels[btn], sizeof(wait_pad_labels[btn]), "%u", (unsigned)pad_display_number((uint8_t)pad));
      return wait_pad_labels[btn];
    }
    return "";
  }
  static constexpr const char* labels[15] = {
    "1", "2", "3", "0", "Exit",
    "4", "5", "6", "Back", "OK",
    "7", "8", "9", "", "",
  };
  return (btn >= 0 && btn < 15) ? labels[btn] : "";
}

static void draw_menu_keypad(void)
{
  auto& d = M5.Display;
  d.startWrite();
  for (int btn = 0; btn < 15; ++btn) {
    int row = btn / 5;
    int col = btn % 5;
    int x = (col == 4) ? fn_x : grid_x + col * col_pitch;
    int y = grid_y + (2 - row) * row_pitch;
    bool wait_pad = kit_edit_state == kit_edit_state_t::assign_wait_pad || kit_edit_state == kit_edit_state_t::clear_wait_pad;
    bool command = wait_pad ? (btn == 4) : (btn == 4 || btn == 8 || btn == 9);
    uint32_t bg = command ? 0x263048u : 0x202028u;
    uint32_t fg = command ? 0xC8D8FFu : 0xFFFFFFu;
    if (!wait_pad && btn == 9) {
      bg = 0x304838u;
      fg = 0xB0FFD0u;
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
}

static int menu_first_visible(uint8_t cursor)
{
  return cursor >= 4 ? cursor - 3 : 0;
}

static const char* kit_dynamic_title(void)
{
  switch (kit_edit_state) {
  case kit_edit_state_t::select_kit_file: return "Load Kit";
  case kit_edit_state_t::select_wav: return "Import Sample";
  case kit_edit_state_t::select_bgm_wav: return "Load BGM";
  case kit_edit_state_t::assign_wait_pad: return "Select Pad";
  case kit_edit_state_t::clear_wait_pad: return "Clear Pad";
  case kit_edit_state_t::pad_list: return "Pad List";
  default: return menu_page_title(menu_page);
  }
}

static size_t kit_dynamic_count(void)
{
  switch (kit_edit_state) {
  case kit_edit_state_t::select_kit_file: return kit_wav_list.size();
  case kit_edit_state_t::select_wav: return kit_wav_list.size();
  case kit_edit_state_t::select_bgm_wav: return kit_wav_list.size();
  case kit_edit_state_t::pad_list: return def::pad::pad_count;
  default: return 0;
  }
}

static bool kit_dynamic_list_active(void)
{
  return kit_edit_state == kit_edit_state_t::select_wav
      || kit_edit_state == kit_edit_state_t::select_kit_file
      || kit_edit_state == kit_edit_state_t::select_bgm_wav
      || kit_edit_state == kit_edit_state_t::pad_list;
}

static void kit_dynamic_label(size_t index, char* out, size_t out_len)
{
  if (out_len == 0) { return; }
  out[0] = 0;
  if (kit_edit_state == kit_edit_state_t::select_wav
   || kit_edit_state == kit_edit_state_t::select_bgm_wav
   || kit_edit_state == kit_edit_state_t::select_kit_file) {
    if (index >= kit_wav_list.size()) { return; }
    std::string name = kit_wav_list[index].filename;
    if (kit_edit_state == kit_edit_state_t::select_kit_file) {
      if (name.size() > 5) { name.resize(name.size() - 5); }
    } else if (name.size() > 4) {
      name.resize(name.size() - 4);
    }
    snprintf(out, out_len, "%s", name.c_str());
    return;
  }
  if (kit_edit_state == kit_edit_state_t::pad_list && index < def::pad::pad_count) {
    uint8_t pad = display_order_to_pad((uint8_t)index);
    auto& slot = sampler_pool_t::slot[pad];
    snprintf(out, out_len, "P%u %s", (unsigned)(index + 1), slot.isValid() ? slot.name : "-");
  }
}

static void draw_menu_wait_pad(const char* title, const char* line1, const char* line2)
{
  auto& d = menu_canvas;
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

static void draw_menu_content(int scroll_px = 0, int x_offset = 0)
{
  if (learn_state != learn_state_t::idle) {
    draw_all();
    draw_learn_overlay();
    return;
  }
  if (!menu_visible) { return; }

  auto& d = menu_canvas;
  size_t count = kit_dynamic_list_active() ? kit_dynamic_count() : 0;
  const auto* items = kit_dynamic_list_active() ? nullptr : menu_items(menu_page, &count);
  // メニュー画面ではモード色の外枠は表示しない
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad) {
    d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
    draw_menu_wait_pad("Import Sample", "Press Pad", kit_pending_wav_name);
    d.pushSprite(x_offset, menu_area_y);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::clear_wait_pad) {
    d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
    draw_menu_wait_pad("Clear Pad", "Press Pad", "sample will be removed");
    d.pushSprite(x_offset, menu_area_y);
    return;
  }
  if (count == 0) { return; }
  if (menu_cursor >= count) { menu_cursor = count - 1; }
  d.fillRect(0, 0, M5.Display.width(), menu_area_h, 0x08080Cu);
  d.setFont(&fonts::efontJA_16_b);
  d.setTextSize(1);
  d.setTextDatum(m5gfx::textdatum_t::top_left);
  d.setTextColor(0xFFFFFFu, 0x08080Cu);
  d.drawString(kit_dynamic_title(), 8, 5);
  d.setTextDatum(m5gfx::textdatum_t::top_right);
  d.setTextColor(0x9090B0u, 0x08080Cu);
  d.drawString("Back / OK", M5.Display.width() - 8, 5);
  d.drawFastHLine(6, 25, M5.Display.width() - 12, 0x303048u);

  int first = menu_first_visible(menu_cursor);
  for (int row = -1; row < 5; ++row) {
    int index = first + row;
    if (index < 0 || index >= (int)count) { continue; }
    int y = 42 + row * 25 + scroll_px;
    if (y < 28 || y > menu_area_h - 10) { continue; }
    bool selected = index == menu_cursor;
    if (selected) { d.fillRoundRect(5, y - 3, 230, 23, 4, 0x303058u); }
    d.setTextColor(selected ? 0xFFFFFFu : 0xC0C0D0u, selected ? 0x303058u : 0x08080Cu);
    d.setTextDatum(m5gfx::textdatum_t::middle_left);
    char label[48];
    if (kit_dynamic_list_active()) {
      char item_label[40];
      kit_dynamic_label(index, item_label, sizeof(item_label));
      snprintf(label, sizeof(label), "%u %s", (unsigned)(index + 1), item_label);
    } else {
      const auto& item = items[index];
      snprintf(label, sizeof(label), "%u %s", (unsigned)(index + 1), item.label);
    }
    d.drawString(label, 10, y + 9);
    if (kit_dynamic_list_active()) {
      if (kit_edit_state == kit_edit_state_t::select_wav
       || kit_edit_state == kit_edit_state_t::select_bgm_wav
       || kit_edit_state == kit_edit_state_t::select_kit_file) {
        d.setTextDatum(m5gfx::textdatum_t::middle_right);
        d.setTextColor(0x80D0FFu, selected ? 0x303058u : 0x08080Cu);
        d.drawString(kit_edit_state == kit_edit_state_t::select_kit_file ? "kit" : "wav", 230, y + 9);
      }
    } else if (items[index].kind == menu_item_kind_t::submenu) {
      d.setTextDatum(m5gfx::textdatum_t::middle_right);
      d.drawString(">", 230, y + 9);
    } else if (items[index].kind == menu_item_kind_t::value) {
      d.setTextDatum(m5gfx::textdatum_t::middle_right);
      d.setTextColor(0x80D0FFu, selected ? 0x303058u : 0x08080Cu);
      int value = menu_value_get(items[index].value);
      d.drawString(menu_value_text(items[index].value, value), 230, y + 9);
    }
  }
  if (status_message_visible()) {
    uint32_t frame = status_message_busy ? status_busy_frame_color() : 0x606080u;
    d.fillRoundRect(8, tab_y - menu_area_y + 1, 224, tab_h - 2, 5, 0x202030u);
    d.drawRoundRect(8, tab_y - menu_area_y + 1, 224, tab_h - 2, 5, frame);
    d.setTextColor(0xFFFFFFu, 0x202030u);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.drawString(status_message, 120, tab_y - menu_area_y + tab_h / 2);
  }
  d.pushSprite(x_offset, menu_area_y);
}

static void draw_menu(bool redraw_keypad)
{
  draw_menu_content();
  if (redraw_keypad) { draw_menu_keypad(); }
}

static void draw_menu_scroll(int old_cursor, int new_cursor)
{
  if (!menu_visible || old_cursor == new_cursor) {
    draw_menu();
    return;
  }
  int old_first = menu_first_visible((uint8_t)old_cursor);
  int new_first = menu_first_visible((uint8_t)new_cursor);
  int delta_rows = new_first - old_first;
  // 表示ウィンドウが1行だけ動く時、旧位置→新位置へ一方向に滑らかにスクロールする。
  // (行き過ぎて戻るバウンスを避けるため、オフセットは単調に0へ近づける)
  if (delta_rows == 1 || delta_rows == -1) {
    static constexpr const int row_h = 25;
    // ドットピッチを2倍にして描画回数を半減 (中間1フレーム + 最終)
    draw_menu_content(delta_rows * row_h / 2);
    M5.delay(6);
    draw_menu();
  } else {
    draw_menu();
  }
}

static void draw_menu_page_transition(int direction)
{
  if (!menu_visible) {
    draw_menu();
    return;
  }
  const int w = M5.Display.width();
  const int dir = direction >= 0 ? 1 : -1;
  static constexpr const int frames = 6;  // スクロールピッチ(1フレームの移動量)を半分にして視認しやすく
  // 高速化: 新ページのレンダリング(日本語フォント描画)は最初の1フレームだけ行い、
  // 以降は同じ menu_canvas スプライトをオフセット違いで貼るだけにする。
  // (かんぷれappのオフスクリーン描画資産の再利用と同じ発想)
  // 全面黒塗りはしない: 覆われない部分は前ページが残るため、黒画面を挟まず
  // 新ページが上にスライドインし、点滅(ブラックアウト)しない。
  for (int i = frames - 1; i >= 0; --i) {
    int x = dir * i * w / frames;
    if (i == frames - 1) {
      draw_menu_content(0, x);  // ここで一度だけ実レンダリング
    } else {
      menu_canvas.pushSprite(x, menu_area_y);  // 以降は貼るだけ
    }
    M5.delay(3);
  }
}

static void menu_open(void)
{
  clear_menu_preview();
  menu_visible = true;
  menu_page = menu_page_t::root;
  menu_cursor = 0;
  menu_depth = 0;
  kit_edit_state = kit_edit_state_t::idle;
  clear_status_message(false);
  menu_sound_navigate(0);
  menu_sound_cursor(1);
  draw_menu(true);
}

static void menu_close(void)
{
  clear_menu_preview();
  menu_visible = false;
  clear_status_message(false);
  menu_depth = 0;
  kit_edit_state = kit_edit_state_t::idle;
  menu_sound_navigate(3);
  draw_all();
}

static void menu_back(void)
{
  if (learn_state != learn_state_t::idle) {
    learn_state = learn_state_t::idle;
    draw_all();
    return;
  }
  if (!menu_visible) { return; }
  if (kit_edit_state == kit_edit_state_t::assign_wait_pad) {
    clear_menu_preview();
    kit_edit_state = kit_edit_state_t::select_wav;
    menu_depth = menu_dynamic_depth();
    menu_sound_navigate(2);
    draw_menu_page_transition(-1);
    return;
  }
  if (kit_edit_state == kit_edit_state_t::select_kit_file
   || kit_edit_state == kit_edit_state_t::select_wav
   || kit_edit_state == kit_edit_state_t::clear_wait_pad
   || kit_edit_state == kit_edit_state_t::pad_list) {
    kit_edit_state_t prev_state = kit_edit_state;
    kit_edit_state = kit_edit_state_t::idle;
    menu_page = (prev_state == kit_edit_state_t::select_kit_file
              || prev_state == kit_edit_state_t::select_wav)
      ? menu_page_t::kit
      : menu_page_t::kit_edit;
    switch (prev_state) {
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
    menu_page = menu_page_t::loop;
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

static bool begin_kit_assign_wav(void)
{
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  ensure_sampler_sd_dirs();
  load_menu_file_list_from("/sampler/samples", ".wav");
  if (kit_wav_list.empty()) {
    show_status_message("No wav", 1600, true);
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
  load_menu_file_list_from("/sampler/kits", ".json");
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

static bool begin_background_wav_select(void)
{
  set_background_loop_error("");
  if (!kp::storage_sd.beginStorage()) {
    show_status_message("No SD", 1600, true);
    return false;
  }
  ensure_sampler_sd_dirs();
  load_menu_file_list_from("/sampler/loops", ".wav");
  if (kit_wav_list.empty()) {
    show_status_message("No BGM wav", 1600, true);
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
  if (name.size() > 4) { name.resize(name.size() - 4); }
  std::string path = std::string(kit_wav_dir) + "/" + f.filename;
  snprintf(kit_pending_wav_path, sizeof(kit_pending_wav_path), "%s", path.c_str());
  snprintf(kit_pending_wav_name, sizeof(kit_pending_wav_name), "%s", name.c_str());
  play_menu_wav_preview(kit_pending_wav_path, 2000);
  kit_edit_state = kit_edit_state_t::assign_wait_pad;
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
  if (name.size() > 4) { name.resize(name.size() - 4); }
  std::string path = std::string(kit_wav_dir) + "/" + f.filename;
  clear_menu_preview();
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::loop;
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
  menu_cursor = 2;
  menu_depth = menu_page_depth(menu_page);
  menu_sound_navigate(1);
  show_loading_message();
  bool ok = load_kit_file(path.c_str());
  show_status_message(ok ? "Kit loaded" : "Load failed", 1600, false);
  draw_menu(true);
}

static void assign_pending_wav_to_pad(uint8_t pad)
{
  char error[32] = { 0 };
  clear_menu_preview();
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::kit;
  menu_cursor = 0;
  show_loading_message();
  bool ok = load_wav_to_pad(pad, kit_pending_wav_path, kit_pending_wav_name, error, sizeof(error));
  char msg[48];
  if (ok) {
    snprintf(msg, sizeof(msg), "Assigned P%u", (unsigned)pad_display_number(pad));
  } else {
    snprintf(msg, sizeof(msg), "%s", error[0] ? error : "Assign failed");
  }
  show_status_message(msg, 1600, false);
  draw_menu(true);
}

static void clear_selected_pad_sample(uint8_t pad)
{
  clear_pad_sample(pad, true);
  kit_edit_state = kit_edit_state_t::idle;
  menu_page = menu_page_t::kit_edit;
  menu_cursor = 0;
  char msg[32];
  snprintf(msg, sizeof(msg), "Cleared P%u", (unsigned)pad_display_number(pad));
  show_status_message(msg, 1600, false);
  draw_menu(true);
}

static void menu_execute_action(menu_action_t action)
{
  switch (action) {
  case menu_action_t::kit_load: {
    begin_kit_file_select();
    return; }
  case menu_action_t::kit_save:
    show_status_message(save_current_kit() ? "Kit saved" : "Save failed", 1600, false);
    break;
  case menu_action_t::kit_new:
    clear_kit();
    show_status_message("New kit", 1600, false);
    break;
  case menu_action_t::kit_reload_samples:
    show_loading_message();
    reload_samples_from_sd();
    show_status_message("Samples reloaded", 1600, false);
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
    loop_reset_recording_state();
    show_status_message("Loop cleared", 1600, false);
    break;
  case menu_action_t::loop_stop:
    stop_all_audio();
    show_status_message("Stopped", 1600, false);
    break;
  case menu_action_t::input_learn:
    menu_visible = false;
    learn_state = learn_state_t::waiting_target;
    learn_target_label[0] = 0;
    draw_learn_overlay();
    return;
  case menu_action_t::input_assign_list:
    show_status_message("No assigns yet", 1600, false);
    break;
  case menu_action_t::input_clear_all:
    show_status_message("Assigns cleared", 1600, false);
    break;
  case menu_action_t::wifi_info: {
    auto sta = (int)kp::system_registry->runtime_info.getWiFiSTAInfo();
    auto ap = (int)kp::system_registry->runtime_info.getWiFiAPInfo();
    char msg[48];
    snprintf(msg, sizeof(msg), "STA %d / AP %d", sta, ap);
    show_status_message(msg, 1600, false);
    break; }
  case menu_action_t::system_info: {
    char msg[64];
    snprintf(msg, sizeof(msg), "v%d.%d.%d RAM %u%%"
      , (int)def::app::app_version_major, (int)def::app::app_version_minor, (int)def::app::app_version_patch
      , (unsigned)((sampler_pool_t::usedBytes() * 100) / sampler_pool_t::pool_budget_bytes));
    show_status_message(msg, 1600, false);
    break; }
  case menu_action_t::reset_all_settings:
    kp::system_registry->reset();
    show_status_message("Settings reset", 1600, false);
    break;
  default:
    break;
  }
  draw_menu(true);
}

static void menu_select(void)
{
  if (!menu_visible) { return; }
  if (kit_edit_state == kit_edit_state_t::select_kit_file) {
    select_kit_file();
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
  if (kit_edit_state == kit_edit_state_t::pad_list) {
    return;
  }
  size_t count = 0;
  const auto* items = menu_items(menu_page, &count);
  if (menu_cursor >= count) { return; }
  const auto& item = items[menu_cursor];
  if (item.kind == menu_item_kind_t::submenu) {
    menu_page = item.child;
    menu_cursor = 0;
    menu_depth = menu_page_depth(menu_page);
    menu_sound_navigate(1);
    menu_sound_cursor(1);
    draw_menu_page_transition(1);
  } else if (item.kind == menu_item_kind_t::value) {
    int next = menu_value_get(item.value) + 1;
    menu_value_set(item.value, next);
    menu_sound_navigate(1);
    draw_menu();
  } else {
    menu_sound_navigate(1);
    menu_execute_action(item.action);
  }
}

static void menu_move(int diff)
{
  if (!menu_visible) { return; }
  size_t count = kit_dynamic_list_active() ? kit_dynamic_count() : 0;
  if (!kit_dynamic_list_active()) {
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
  draw_menu_scroll(old, menu_cursor);
}

static void menu_input_number(uint8_t number)
{
  if (!menu_visible) { return; }
  size_t count = kit_dynamic_list_active() ? kit_dynamic_count() : 0;
  if (!kit_dynamic_list_active()) {
    menu_items(menu_page, &count);
  }
  uint8_t display_number = (number == 0) ? 10 : number;
  if (display_number < 1 || display_number > count) { return; }
  int old = menu_cursor;
  menu_cursor = display_number - 1;
  menu_sound_cursor(display_number);
  draw_menu_scroll(old, menu_cursor);
}

static bool menu_handle_button(int btn)
{
  if (!menu_visible || btn < 0 || btn >= 15) { return false; }
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
  if (pressed_edge & bb::SIDE_2) {
    if (learn_state != learn_state_t::idle) {
      learn_state = learn_state_t::idle;
      draw_all();
    } else if (menu_visible) {
      menu_close();
    } else {
      menu_open();
    }
    return true;
  }
  if (learn_state != learn_state_t::idle) {
    if (pressed_edge & (bb::ENC1_PUSH | bb::ENC2_PUSH)) {
      menu_back();
      return true;
    }
    return false;
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
  if (pressed_edge & bb::ENC2_UP) { menu_move(+1); return true; }
  if (pressed_edge & bb::ENC2_DOWN) { menu_move(-1); return true; }
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
    } else if (pad == -1) {
      snprintf(learn_target_label, sizeof(learn_target_label), "FN%d", button_to_fn(btn) + 1);
    }
    learn_state = learn_state_t::waiting_external;
    draw_learn_overlay();
    return true;
  }
  for (int i = 0; i < (int)sampler_mode_t::mode_max; ++i) {
    if (pressed_edge & (bb::SUB_1 << i)) {
      snprintf(learn_target_label, sizeof(learn_target_label), "%s", mode_info[i].name);
      learn_state = learn_state_t::waiting_external;
      draw_learn_overlay();
      return true;
    }
  }
  if (pressed_edge & bb::ENC1_PUSH) {
    snprintf(learn_target_label, sizeof(learn_target_label), "STOP ALL");
    learn_state = learn_state_t::waiting_external;
    draw_learn_overlay();
    return true;
  }
  if (pressed_edge & bb::ENC2_PUSH) {
    snprintf(learn_target_label, sizeof(learn_target_label), "ENC2");
    learn_state = learn_state_t::waiting_external;
    draw_learn_overlay();
    return true;
  }
  return false;
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

static bool auto_crop_and_normalize(int16_t* data, uint32_t frames, uint32_t sample_rate, auto_crop_result_t& result)
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

  int32_t peak = 1;
  for (uint32_t i = start; i < end; ++i) {
    int32_t v = data[i];
    if (v < 0) { v = (v == INT16_MIN) ? 32768 : -v; }
    if (peak < v) { peak = v; }
  }

  static constexpr const int32_t target_peak = 30000;
  for (uint32_t i = 0; i < frames; ++i) {
    int32_t v = ((int32_t)data[i] * target_peak) / peak;
    if (v > INT16_MAX) { v = INT16_MAX; }
    if (v < INT16_MIN) { v = INT16_MIN; }
    data[i] = (int16_t)v;
  }
  result.start = start;
  result.end = end;
  return true;
}

static void loop_remove_pad_events(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
    [pad](const loop_event_t& e) { return e.pad == (uint8_t)pad; }), loop_events.end());
  loop_pad_mute[pad] = false;
  loop_active_layer[pad] = 0;
  loop_deferred_note_on_layer[pad] = 0;
  loop_deferred_live_pad[pad] = false;
  if (loop_events.empty()) {
    if (background_loop.isValid()) {
      loop_length_fixed = true;
      loop_length_msec = background_loop_length_ms();
    } else {
      loop_length_fixed = false;
      loop_length_msec = loop_default_length_ms;
    }
    loop_prev_pos_ms = 0;
  }
}

static void loop_reset_recording_state(void)
{
  loop_events.clear();
  loop_playing = false;
  loop_prev_pos_ms = 0;
  loop_start_msec = M5.millis();
  loop_record_enabled = true;
  if (background_loop.isValid()) {
    loop_length_fixed = true;
    loop_length_msec = background_loop_length_ms();
  } else {
    loop_length_fixed = false;
    loop_length_msec = loop_default_length_ms;
  }
  loop_layer_seq = 1;
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    loop_pad_mute[i] = false;
    loop_active_layer[i] = 0;
    loop_deferred_note_on_layer[i] = 0;
    loop_deferred_live_pad[i] = false;
    loop_deferred_live_pos_ms[i] = 0;
  }
  sampler_audio_t::stopAll();
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
}

static void loop_reset_recording_state_if_empty(void)
{
  if (loop_events.empty()) { loop_reset_recording_state(); }
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
  int16_t* buf = alloc_recording_buffer();
  if (buf == nullptr) { return; }

  sampler_audio_t::stopAll();
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
  if (auto_crop_and_normalize(recording_buffer, frames, sample_rate, crop)) {
    char name[16];
    snprintf(name, sizeof(name), "REC%02u", (unsigned)recording_seq++);
    sampler_audio_t::stop(pad);
    loop_remove_pad_events(pad);
    loop_reset_recording_state_if_empty();
    if (sampler_pool_t::loadPcm(pad, name, recording_buffer, frames, sample_rate)) {
      auto& slot = sampler_pool_t::slot[pad];
      slot.start_frame = std::min<uint32_t>(crop.start, slot.frames);
      slot.end_frame = std::min<uint32_t>(crop.end, slot.frames);
      slot.hold_enabled = false;
      slot.loop_enabled = false;
      rec_wave_pad = pad;
    }
  }

  draw_header();
  update_pad_led(pad);
  draw_pad(pad);
  (void)overflowed;
  draw_wave();
}

static void preview_edit_pad(void)
{
  if (loop_playing) { return; }
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  sampler_audio_t::play(edit_pad, slot.pcm + slot.playStart(), slot.playFrames(), slot.sample_rate,
                        false, slot.reverse, slot.volume_q8, slot.pitch_q8);
}

static void draw_edit_target(void)
{
  draw_wave();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { draw_pad(i); }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static void enter_edit(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return; }
  int old = edit_pad;
  edit_pad = pad;
  if (old < 0) { edit_param = 0; }
  if (old >= 0 && old != pad) {
    sampler_audio_t::stop(old);
    draw_pad(old);
  }
  draw_edit_target();
}

static void exit_edit(void)
{
  int old = edit_pad;
  edit_pad = -1;
  if (old >= 0) { sampler_audio_t::stop(old); }
  draw_edit_target();
}

static void edit_value_add(int diff)
{
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid()) { return; }
  edit_value_activity_until = M5.millis() + 1000;
  edit_value_compact_visible = true;
  if (edit_param == 2) {
    int value = (int)slot.volume_q8 + diff * 13; // 約5%
    if (value < 0) { value = 0; }
    if (value > 512) { value = 512; }
    slot.volume_q8 = (uint16_t)value;
    draw_wave();
    draw_pad(edit_pad);
    return;
  }
  if (edit_param == 3) {
    int value = (int)slot.pitch_q8 + diff * 13; // 約5%
    if (value < 128) { value = 128; }
    if (value > 512) { value = 512; }
    slot.pitch_q8 = (uint16_t)value;
    draw_wave();
    draw_pad(edit_pad);
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
  draw_wave();
}

// Padの再生方式に従って発音する
static void trigger_pad(int pad) {
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid()) { return; }
  const int16_t* pcm = slot.pcm + slot.playStart();
  uint32_t frames = slot.playFrames();
  if (frames == 0) { return; }
  if (slot.loop_enabled && !slot.hold_enabled) {
    // Toggle Loop: 押すとループ開始 / 再度押すと停止
    if (sampler_audio_t::isPlaying(pad)) {
      sampler_audio_t::stop(pad);
    } else {
      sampler_audio_t::play(pad, pcm, frames, slot.sample_rate, true, slot.reverse, slot.volume_q8, slot.pitch_q8);
    }
    return;
  }
  // One Shot / Hold / Hold Loop
  sampler_audio_t::play(pad, pcm, frames, slot.sample_rate, slot.loop_enabled, slot.reverse, slot.volume_q8, slot.pitch_q8);
}

static bool defer_live_pad_if_early(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return false; }
  uint32_t now = M5.millis();
  uint32_t raw_pos = loop_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  if (!loop_should_defer_quantized_note(raw_pos, pos)) { return false; }
  loop_deferred_live_pad[pad] = true;
  loop_deferred_live_pos_ms[pad] = pos;
  return true;
}

static void push_loop_event(uint8_t pad, loop_event_type_t type, uint32_t pos_ms, uint16_t layer)
{
  if (loop_events.size() >= loop_event_max) {
    loop_events.erase(loop_events.begin());
  }
  loop_events.push_back({ pad, type, pos_ms, layer });
}

static void loop_start_length_capture(uint32_t now)
{
  loop_length_fixed = false;
  loop_prev_pos_ms = 0;
  loop_start_msec = now;
  loop_playing = true;
}

static void loop_finish_length_capture(uint32_t now)
{
  if (!loop_playing || loop_length_fixed) { return; }
  uint32_t elapsed = now - loop_start_msec;
  if (elapsed < loop_min_length_ms) { elapsed = loop_min_length_ms; }
  loop_length_msec = elapsed;
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
  loop_length_fixed = true;
  quantize_loop_events_to_length(loop_length_msec);
  loop_prev_pos_ms = loop_length_msec - 1;
  loop_start_msec = now;
  loop_record_enabled = true;
  sampler_audio_t::stopAll();
}

static void loop_toggle_play(void);

static void loop_handle_top_button(void)
{
  uint32_t now = M5.millis();
  if (!loop_length_fixed) {
    loop_finish_length_capture(now);
    if (!loop_length_fixed) { return; }
  } else {
    loop_toggle_play();
    return;
  }
  draw_wave();
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static void trigger_loop_event(const loop_event_t& event)
{
  int pad = event.pad;
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  if (event.type == loop_event_type_t::note_off) {
    if (event.layer != 0 && loop_deferred_note_on_layer[pad] == event.layer) {
      loop_deferred_note_on_layer[pad] = 0;
    }
    if (slot.hold_enabled) {
      sampler_audio_t::stop(pad);
    }
    return;
  }
  if (event.layer != 0 && loop_deferred_note_on_layer[pad] == event.layer) {
    loop_deferred_note_on_layer[pad] = 0;
  }
  if (loop_pad_mute[pad]) { return; }

  sampler_audio_t::play(pad, slot.pcm + slot.playStart(), slot.playFrames(), slot.sample_rate,
                        slot.loop_enabled, slot.reverse, slot.volume_q8, slot.pitch_q8);
}

static void loop_toggle_play(void)
{
  uint32_t now = M5.millis();
  if (loop_playing) {
    loop_playing = false;
    loop_prev_pos_ms = 0;
    for (int i = 0; i < (int)def::pad::pad_count; ++i) {
      loop_deferred_live_pad[i] = false;
    }
    sampler_audio_t::stopAll();
  } else {
    loop_prev_pos_ms = 0;
    loop_start_msec = now;
    loop_playing = true;
    play_background_loop_at(0);
  }
  draw_wave();
}

static void stop_all_audio(void)
{
  if (loop_playing) {
    loop_playing = false;
    loop_prev_pos_ms = 0;
  }
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    loop_active_layer[i] = 0;
    loop_deferred_note_on_layer[i] = 0;
    loop_deferred_live_pad[i] = false;
  }
  sampler_audio_t::stopAll();
  draw_wave();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    update_pad_led(i);
    draw_pad(i);
  }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static void loop_undo(void)
{
  if (loop_events.empty()) { return; }
  uint16_t layer = loop_events.back().layer;
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    if (loop_active_layer[i] == layer) { loop_active_layer[i] = 0; }
    if (loop_deferred_note_on_layer[i] == layer) { loop_deferred_note_on_layer[i] = 0; }
  }
  loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
    [layer](const loop_event_t& e) { return e.layer == layer; }), loop_events.end());
  draw_wave();
}

static void loop_record_pad(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count || !sampler_pool_t::slot[pad].isValid()) { return; }
  uint32_t now = M5.millis();
  if (!loop_playing) {
    if (!loop_length_fixed && loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = 0;
      loop_start_msec = now;
      loop_playing = true;
      play_background_loop_at(0);
    }
  }
  uint32_t raw_pos = loop_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  uint16_t layer = loop_layer_seq++;
  bool defer_note_on = loop_should_defer_quantized_note(raw_pos, pos);
  push_loop_event((uint8_t)pad, loop_event_type_t::note_on, pos, layer);
  if (sampler_pool_t::slot[pad].hold_enabled) {
    loop_active_layer[pad] = layer;
  }
  if (defer_note_on) {
    loop_deferred_note_on_layer[pad] = layer;
  } else {
    trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_on, raw_pos, layer });
  }
  loop_prev_pos_ms = raw_pos;
  draw_fn(0);  // 再生状態が変わるためPLAY/STOPアイコンを更新
}

static void loop_record_pad_release(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || !slot.hold_enabled || loop_active_layer[pad] == 0) { return; }
  uint32_t now = M5.millis();
  uint32_t raw_pos = loop_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_note_off_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  uint16_t layer = loop_active_layer[pad];
  loop_active_layer[pad] = 0;
  if (loop_deferred_note_on_layer[pad] == layer) {
    loop_deferred_note_on_layer[pad] = 0;
    loop_events.erase(std::remove_if(loop_events.begin(), loop_events.end(),
      [layer](const loop_event_t& e) { return e.layer == layer; }), loop_events.end());
    loop_prev_pos_ms = raw_pos;
    return;
  }
  push_loop_event((uint8_t)pad, loop_event_type_t::note_off, pos, layer);
  trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_off, raw_pos, layer });
  loop_prev_pos_ms = raw_pos;
}

static void apply_play_fn_to_pad(int fn, int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  if (fn == 0) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid()) { return; }
  if (fn == 1) {
    slot.hold_enabled = !slot.hold_enabled;
  } else if (fn == 2) {
    slot.loop_enabled = !slot.loop_enabled;
  }
  draw_pad(pad);
}

static void apply_rec_fn_to_pad(int fn, int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (fn == 0) {
    if (slot.isValid()) {
      enter_edit(pad);
      preview_edit_pad();
    }
    return;
  }
  if (fn == 1) {
    if (slot.isValid()) {
      slot.reverse = !slot.reverse;
      rec_wave_pad = pad;
      if (!loop_playing) { trigger_pad(pad); }
      draw_wave();
      draw_pad(pad);
    }
    return;
  }
  if (fn == 2) {
    if (slot.isValid()) {
      sampler_audio_t::stop(pad);
      if (rec_wave_pad == pad) { rec_wave_pad = -1; }
      loop_remove_pad_events(pad);
      loop_reset_recording_state_if_empty();
      sampler_pool_t::erase(pad);
      draw_header();
      draw_wave();
      update_pad_led(pad);
      draw_pad(pad);
    }
  }
}

static bool apply_fn_to_pressed_pads(int fn)
{
  if (edit_pad >= 0) { return false; }
  if (current_mode != sampler_mode_t::mode_rec && current_mode != sampler_mode_t::mode_play) { return false; }
  if (current_mode == sampler_mode_t::mode_play && fn == 0) { return false; }
  bool applied = false;
  for (int pad = 0; pad < (int)def::pad::pad_count; ++pad) {
    if (!pads[pad].pressed) { continue; }
    if (current_mode == sampler_mode_t::mode_rec) {
      apply_rec_fn_to_pad(fn, pad);
    } else {
      apply_play_fn_to_pad(fn, pad);
    }
    applied = true;
  }
  if (applied) {
    for (int i = 0; i < 3; ++i) {
      update_fn_led(i);
      draw_fn(i);
    }
  }
  return applied;
}

static void pad_press(int pad) {
  if (pads[pad].pressed) { return; }
  pads[pad].pressed = true;
  auto& slot = sampler_pool_t::slot[pad];

  if (current_mode == sampler_mode_t::mode_play && (fn_pressed[1] || fn_pressed[2])) {
    // PLAYモード互換操作: Fn(HOLD/LOOP)を押しながらPadで各フラグをトグル
    if (slot.isValid()) {
      if (fn_pressed[1]) { slot.hold_enabled = !slot.hold_enabled; }
      if (fn_pressed[2]) { slot.loop_enabled = !slot.loop_enabled; }
    }
  } else if (edit_pad < 0 && current_mode == sampler_mode_t::mode_rec && fn_pressed[0]) {
    // RECモード互換操作: EDIT + Pad で編集対象にする
    if (slot.isValid()) {
      enter_edit(pad);
      preview_edit_pad();
    }
  } else if (edit_pad < 0 && current_mode == sampler_mode_t::mode_rec && fn_pressed[2]) {
    // RECモード: DEL + Pad でサンプル削除
    if (slot.isValid()) {
      sampler_audio_t::stop(pad);
      if (rec_wave_pad == pad) { rec_wave_pad = -1; }
      loop_remove_pad_events(pad);
      loop_reset_recording_state_if_empty();
      sampler_pool_t::erase(pad);
      draw_header();  // プール使用量の表示を更新
      draw_wave();
    }
  } else if (edit_pad < 0 && current_mode == sampler_mode_t::mode_rec && fn_pressed[1]) {
    // RECモード: REV + Pad で逆再生を切り替える
    if (slot.isValid()) {
      slot.reverse = !slot.reverse;
      rec_wave_pad = pad;
      if (!loop_playing) { trigger_pad(pad); }
      draw_wave();
    }
  } else if (current_mode == sampler_mode_t::mode_loop && fn_pressed[1]) {
    loop_pad_mute[pad] = !loop_pad_mute[pad];
    draw_wave();
  } else if (current_mode == sampler_mode_t::mode_loop && fn_pressed[2]) {
    loop_del_touched_pad = true;
    loop_remove_pad_events(pad);
    loop_reset_recording_state_if_empty();
    draw_wave();
    for (int i = 0; i < 3; ++i) { draw_fn(i); }
  } else if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_pad(pad);
  } else if (edit_pad >= 0 && slot.isValid()) {
    enter_edit(pad);
    preview_edit_pad();
  } else if (edit_pad < 0 && current_mode == sampler_mode_t::mode_rec && !slot.isValid()) {
    start_pad_recording(pad);
  } else if (edit_pad < 0 && current_mode == sampler_mode_t::mode_rec && slot.isValid()) {
    rec_wave_pad = pad;
    if (!loop_playing) { trigger_pad(pad); }
    draw_wave();
  } else {
    if (!defer_live_pad_if_early(pad)) { trigger_pad(pad); }
  }
  update_pad_led(pad);
  draw_pad(pad);
  if (edit_pad < 0 && (current_mode == sampler_mode_t::mode_rec || current_mode == sampler_mode_t::mode_play)) {
    for (int i = 0; i < 3; ++i) {
      update_fn_led(i);
      draw_fn(i);
    }
  }
}

static void pad_release(int pad) {
  if (!pads[pad].pressed) { return; }
  pads[pad].pressed = false;
  if (recording_pad == pad) {
    finish_pad_recording();
    return;
  }
  auto& slot = sampler_pool_t::slot[pad];
  if (current_mode == sampler_mode_t::mode_loop) {
    loop_record_pad_release(pad);
  }
  if (loop_deferred_live_pad[pad] && slot.isValid() && slot.hold_enabled) {
    loop_deferred_live_pad[pad] = false;
  }
  if (edit_pad < 0 && slot.isValid() && slot.hold_enabled) {
    sampler_audio_t::stop(pad);
  }
  update_pad_led(pad);
  draw_pad(pad);
  if (edit_pad < 0 && (current_mode == sampler_mode_t::mode_rec || current_mode == sampler_mode_t::mode_play)) {
    for (int i = 0; i < 3; ++i) {
      update_fn_led(i);
      draw_fn(i);
    }
  }
}

static void set_mode(sampler_mode_t mode) {
  if (mode == current_mode) { return; }
  if (edit_pad >= 0) { exit_edit(); }
  if (current_mode == sampler_mode_t::mode_fx && mode != sampler_mode_t::mode_fx) {
    for (uint8_t i = 0; i < 3; ++i) {
      sampler_audio_t::setFxActive(i, false);
    }
    loop_repeat_set_active(false);
  }
  current_mode = mode;
  update_mode_leds();
  draw_tabs();
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  draw_wave();
}

static void volume_add(int diff) {
  int v = kp::system_registry->user_setting.getMasterVolume() + diff;
  if (v < 0) { v = 0; }
  if (v > 100) { v = 100; }
  kp::system_registry->user_setting.setMasterVolume(v);
  draw_header();
}

static void fx_set_active(uint8_t index, bool active)
{
  if (index >= 3) { return; }
  if (active) { fx_selected = index; }
  if (index == 2) {
    sampler_audio_t::setFxActive(index, false);
    loop_repeat_set_active(active);
  } else {
    sampler_audio_t::setFx(index, active, fx_param[index]);
  }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  draw_wave();
}

static void fx_select_next(void)
{
  fx_selected = (fx_selected + 1) % 3;
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  draw_wave();
}

static void fx_param_add(int diff)
{
  int index = fx_selected;
  int value = (int)fx_param[index] + (index == 2 ? diff : diff * 5);
  if (index == 2) {
    int max_value = (int)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0])) - 1;
    if (value < 0) { value = 0; }
    if (value > max_value) { value = max_value; }
  } else {
    if (value < -50) { value = -50; }
    if (value > 50) { value = 50; }
  }
  fx_param[index] = (int8_t)value;
  if (index == 2) {
    if (fn_pressed[2]) { loop_repeat_set_active(true); }
  } else {
    sampler_audio_t::setFxParam((uint8_t)index, fx_param[index]);
  }
  draw_wave();
}

static bool loop_event_crossed(uint32_t prev_pos, uint32_t pos, uint32_t event_pos)
{
  if (prev_pos == pos) { return false; }
  if (prev_pos < pos) { return event_pos > prev_pos && event_pos <= pos; }
  return event_pos > prev_pos || event_pos <= pos;
}

static uint32_t loop_repeat_width_ms(void)
{
  uint8_t index = std::min<uint8_t>((uint8_t)fx_param[2], (uint8_t)(sizeof(loop_repeat_half_steps) / sizeof(loop_repeat_half_steps[0]) - 1));
  return std::max<uint32_t>(1, ((uint64_t)loop_quantize_step_ms(loop_length_msec) * loop_repeat_half_steps[index]) / 2);
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
    if (loop_repeat_armed || loop_repeat_running) { sampler_audio_t::stopAll(); }
    loop_repeat_armed = false;
    loop_repeat_running = false;
    return;
  }
  if (loop_repeat_running) { sampler_audio_t::stopAll(); }
  uint32_t now = M5.millis();
  loop_repeat_start_pos_ms = loop_next_quantize_pos_ms(loop_pos_ms(now));
  loop_repeat_length_ms = loop_repeat_width_ms();
  loop_repeat_armed = true;
  loop_repeat_running = false;
}

static bool loop_repeat_event_relative(const loop_event_t& event, uint32_t* relative)
{
  uint32_t pos = event.pos_ms >= loop_repeat_start_pos_ms
    ? event.pos_ms - loop_repeat_start_pos_ms
    : loop_length_msec - loop_repeat_start_pos_ms + event.pos_ms;
  if (pos >= loop_repeat_length_ms) { return false; }
  *relative = pos;
  return true;
}

static bool service_loop_repeat(uint32_t now, uint32_t loop_pos)
{
  if (loop_repeat_armed) {
    if (loop_pos != loop_repeat_start_pos_ms
     && !loop_event_crossed(loop_prev_pos_ms, loop_pos, loop_repeat_start_pos_ms)) {
      return false;
    }
    loop_repeat_armed = false;
    loop_repeat_running = true;
    loop_repeat_started_msec = now;
    loop_repeat_prev_pos_ms = loop_repeat_length_ms - 1;
    sampler_audio_t::stopAll();
  }
  if (!loop_repeat_running) { return false; }

  uint32_t repeat_pos = (now - loop_repeat_started_msec) % loop_repeat_length_ms;
  for (const auto& event : loop_events) {
    uint32_t event_pos = 0;
    if (loop_repeat_event_relative(event, &event_pos)
     && loop_event_crossed(loop_repeat_prev_pos_ms, repeat_pos, event_pos)) {
      trigger_loop_event(event);
    }
  }
  loop_repeat_prev_pos_ms = repeat_pos;
  return true;
}

static void service_loop(uint32_t now)
{
  if (!loop_playing) { return; }
  if (loop_record_enabled && !loop_length_fixed) { return; }
  uint32_t pos = loop_pos_ms(now);
  bool repeating = service_loop_repeat(now, pos);
  if (!repeating && !loop_events.empty()) {
    for (const auto& e : loop_events) {
      if (loop_event_crossed(loop_prev_pos_ms, pos, e.pos_ms)) {
        trigger_loop_event(e);
      }
    }
  }
  for (int i = 0; !repeating && i < (int)def::pad::pad_count; ++i) {
    if (loop_deferred_live_pad[i]
     && loop_event_crossed(loop_prev_pos_ms, pos, loop_deferred_live_pos_ms[i])) {
      loop_deferred_live_pad[i] = false;
      trigger_pad(i);
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

static void process_bitmask(uint32_t bitmask) {
  namespace bb = kp::def::button_bitmask;
  uint32_t pressed_edge  = bitmask & ~prev_bitmask;
  uint32_t released_edge = ~bitmask & prev_bitmask;
  prev_bitmask = bitmask;

  if (menu_handle_input(pressed_edge)) { return; }
  if (learn_capture_target(pressed_edge)) { return; }

  // メイン15ボタン (Pad 4x3 + Fn列)
  for (int btn = 0; btn < 15; ++btn) {
    uint32_t mask = 1u << btn;
    if (0 == ((pressed_edge | released_edge) & mask)) { continue; }
    bool press = pressed_edge & mask;
    int pad = button_to_pad(btn);
    if (pad >= 0) {
      press ? pad_press(pad) : pad_release(pad);
    } else if (pad == -1) {
      int fn = button_to_fn(btn);
      fn_pressed[fn] = press;
      if (press) { fn_press_msec[fn] = M5.millis(); }
      if (press && current_mode == sampler_mode_t::mode_play && edit_pad < 0 && fn == 0) {
        loop_toggle_play();
        update_fn_led(fn);
        draw_fn(fn);
        continue;
      }
      if (press && apply_fn_to_pressed_pads(fn)) {
        update_fn_led(fn);
        draw_fn(fn);
        continue;
      }
      if (press && edit_pad >= 0) {
        if (fn == 0) {
          // EDITボタンで Start / End をトグル (Volume選択中はStartへ戻る)
          edit_param = (edit_param == 0) ? 1 : 0;
          draw_wave();
        } else if (fn == 1) {
          // Fn2はVolume/Pitch編集をトグル
          edit_param = (edit_param == 2) ? 3 : 2;
          draw_wave();
        } else {
          exit_edit();
        }
        if (edit_pad >= 0) {
          for (int i = 0; i < 3; ++i) { draw_fn(i); }
        }
      } else if (press && current_mode == sampler_mode_t::mode_loop) {
        if (fn == 0) {
          loop_handle_top_button();
        } else if (fn == 1) {
          draw_wave();
        } else if (fn == 2) {
          if (loop_record_enabled && !loop_length_fixed && loop_playing) {
            loop_reset_recording_state();
            loop_del_touched_pad = true;
            for (int i = 0; i < 3; ++i) { draw_fn(i); }
          } else {
            loop_del_touched_pad = false;
          }
          draw_wave();
        }
      } else if (!press && current_mode == sampler_mode_t::mode_loop && fn == 2) {
        uint32_t held = M5.millis() - fn_press_msec[fn];
        if (!loop_del_touched_pad && !loop_playing && held >= loop_del_long_press_ms) {
          loop_reset_recording_state();
          draw_wave();
          for (int i = 0; i < 3; ++i) { draw_fn(i); }
        }
      } else if (current_mode == sampler_mode_t::mode_fx) {
        fx_set_active((uint8_t)fn, press);
      }
      update_fn_led(fn);
      draw_fn(fn);
    }
  }

  // 上段4ボタン: モード切替 (REC/PLAY/LOOP/FX)
  for (int i = 0; i < (int)sampler_mode_t::mode_max; ++i) {
    if (pressed_edge & (bb::SUB_1 << i)) {
      set_mode((sampler_mode_t)i);
    }
  }

  // 上エンコーダ: マスターボリューム
  if (pressed_edge & bb::ENC1_UP)   { volume_add(+5); }
  if (pressed_edge & bb::ENC1_DOWN) { volume_add(-5); }
  if (pressed_edge & bb::ENC1_PUSH) { stop_all_audio(); }

  if (edit_pad >= 0) {
    if (pressed_edge & bb::ENC2_UP)   { edit_value_add(+1); }
    if (pressed_edge & bb::ENC2_DOWN) { edit_value_add(-1); }
    if (pressed_edge & bb::ENC2_PUSH) { preview_edit_pad(); }
  } else if (current_mode == sampler_mode_t::mode_fx) {
    if (pressed_edge & bb::ENC2_UP)   { fx_param_add(+1); }
    if (pressed_edge & bb::ENC2_DOWN) { fx_param_add(-1); }
    if (pressed_edge & bb::ENC2_PUSH) { fx_select_next(); }
  }
}

static void process_touch(uint32_t value) {
  bool pressed = value & 1;
  int x = ((int16_t)(value & 0xFFFF)) >> 1;
  int y = ((int16_t)(value >> 16)) >> 1;

  if (!pressed) {
    if (touch_pad >= 0) { pad_release(touch_pad); touch_pad = -1; }
    return;
  }

  // モードタブのタップ
  if (y >= tab_y && y < tab_y + tab_h) {
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
// サンプルの読み込み (SDカード /sampler/samples/*.wav → PSRAMプール)

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
  if (menu_preview_pcm) {
    M5.delay(2);
    free(menu_preview_pcm);
  }
  menu_preview_pcm = nullptr;
  menu_preview_frames = 0;
  menu_preview_sample_rate = 44100;
}

static bool play_menu_wav_preview(const char* path, uint32_t max_ms)
{
  static constexpr const size_t max_wav_file_size = 3200 * 1024;
  if (!path || !path[0] || max_ms == 0) { return false; }
  if (!kp::storage_sd.beginStorage()) { return false; }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 44 || (size_t)size > max_wav_file_size) { return false; }
  uint8_t* tmp = temp_alloc((size_t)size);
  if (!tmp) { return false; }
  int len = kp::storage_sd.loadFromFileToMemory(path, tmp, (size_t)size);
  if (len <= 44) {
    free(tmp);
    return false;
  }
  wav_info_t info;
  if (!parse_wav(tmp, (size_t)len, &info)) {
    free(tmp);
    return false;
  }
  uint32_t preview_frames = std::min<uint32_t>(info.frames, ((uint64_t)info.sample_rate * max_ms) / 1000);
  if (preview_frames == 0) {
    free(tmp);
    return false;
  }
  int16_t* pcm = bgm_alloc((size_t)preview_frames * sizeof(int16_t));
  if (!pcm) {
    free(tmp);
    return false;
  }
  if (info.channels == 2) {
    for (uint32_t i = 0; i < preview_frames; ++i) {
      pcm[i] = (int16_t)(((int32_t)info.pcm[i * 2] + info.pcm[i * 2 + 1]) >> 1);
    }
  } else {
    memcpy(pcm, info.pcm, (size_t)preview_frames * sizeof(int16_t));
  }
  free(tmp);
  clear_menu_preview();
  menu_preview_pcm = pcm;
  menu_preview_frames = preview_frames;
  menu_preview_sample_rate = info.sample_rate;
  return sampler_audio_t::play(menu_preview_voice, menu_preview_pcm, menu_preview_frames,
                               menu_preview_sample_rate, false, false, 224, 256);
}

static void set_error_text(char* error, size_t error_len, const char* msg)
{
  if (error && error_len) { snprintf(error, error_len, "%s", msg ? msg : "Error"); }
}

static bool load_wav_to_pad(uint8_t pad, const char* path, const char* display_name, char* error, size_t error_len)
{
  static constexpr const size_t max_wav_file_size = 3200 * 1024;
  if (pad >= def::pad::pad_count || !path || !path[0]) {
    set_error_text(error, error_len, "Bad pad");
    return false;
  }
  if (!kp::storage_sd.beginStorage()) {
    set_error_text(error, error_len, "No SD");
    return false;
  }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 44) {
    set_error_text(error, error_len, "Empty WAV");
    return false;
  }
  if ((size_t)size > max_wav_file_size) {
    set_error_text(error, error_len, "WAV too big");
    return false;
  }
  uint8_t* tmp = temp_alloc((size_t)size);
  if (!tmp) {
    set_error_text(error, error_len, "No memory");
    return false;
  }
  int len = kp::storage_sd.loadFromFileToMemory(path, tmp, (size_t)size);
  if (len <= 44) {
    free(tmp);
    set_error_text(error, error_len, "Read failed");
    return false;
  }

  sampler_audio_t::stop(pad);
  bool ok = sampler_pool_t::loadWav(pad, display_name ? display_name : "", tmp, (size_t)len);
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
    set_error_text(error, error_len, "Bad WAV");
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
    pads[pad].pressed = false;
    pads[pad].playing_shown = false;
    sampler_pool_t::erase(pad);
  }
  rec_wave_pad = -1;
  edit_pad = -1;
  recording_pad = -1;
  loop_events.clear();
  memset(loop_pad_mute, 0, sizeof(loop_pad_mute));
  memset(loop_active_layer, 0, sizeof(loop_active_layer));
  memset(loop_deferred_note_on_layer, 0, sizeof(loop_deferred_note_on_layer));
  memset(loop_deferred_live_pad, 0, sizeof(loop_deferred_live_pad));
  loop_layer_seq = 1;
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
                        background_loop.sample_rate, true, false, background_loop.volume_q8, 256, start_frame);
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
  background_loop.sample_rate = 44100;
  background_loop.volume_q8 = 192;
  background_loop.loop_repeats = 1;
  background_loop.name[0] = 0;
  background_loop.file_path[0] = 0;
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
  uint32_t frames = info.frames;
  if (frames < info.sample_rate / 2) {
    set_background_loop_error("BGM too short");
    return false;
  }
  if (loop_repeats < 1) { loop_repeats = 1; }
  uint32_t max_frames = info.sample_rate * background_loop_max_sec;
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
  if (info.channels == 2) {
    for (uint32_t i = 0; i < frames; ++i) {
      pcm[i] = (int16_t)(((int32_t)info.pcm[i * 2] + info.pcm[i * 2 + 1]) >> 1);
    }
  } else {
    memcpy(pcm, info.pcm, bytes);
  }

  clear_background_loop();
  background_loop.pcm = pcm;
  background_loop.frames = frames;
  background_loop.sample_rate = info.sample_rate;
  background_loop.loop_repeats = loop_repeats;
  snprintf(background_loop.name, sizeof(background_loop.name), "%s", display_name ? display_name : "BGM");
  snprintf(background_loop.file_path, sizeof(background_loop.file_path), "%s", file_path ? file_path : "");

  loop_events.clear();
  memset(loop_pad_mute, 0, sizeof(loop_pad_mute));
  memset(loop_active_layer, 0, sizeof(loop_active_layer));
  memset(loop_deferred_note_on_layer, 0, sizeof(loop_deferred_note_on_layer));
  memset(loop_deferred_live_pad, 0, sizeof(loop_deferred_live_pad));
  loop_layer_seq = 1;
  loop_length_msec = background_loop_length_ms();
  loop_length_fixed = true;
  loop_playing = false;
  loop_prev_pos_ms = 0;
  loop_start_msec = M5.millis();
  loop_record_enabled = true;
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_length_msec));
  draw_wave();
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
  set_background_loop_error("");
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
  if (size <= 44) {
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
  if (len <= 44) {
    free(data);
    set_background_loop_error("BGM read failed");
    return false;
  }
  bool ok = load_background_loop_memory(data, (size_t)len, display_name, path, 1);
  free(data);
  return ok;
}

static bool ensure_sampler_sd_dirs(void)
{
  if (!kp::storage_sd.beginStorage()) { return false; }
  kp::storage_sd.makeDirectory("/sampler");
  kp::storage_sd.makeDirectory("/sampler/samples");
  kp::storage_sd.makeDirectory("/sampler/loops");
  kp::storage_sd.makeDirectory("/sampler/kits");
  return true;
}

static void load_builtin_samples(void)
{
  M5.Display.print("\nbuiltin");
  for (size_t i = 0; i < builtin_sample_count && i < def::pad::pad_count; ++i) {
    uint8_t pad = display_order_to_pad((uint8_t)i);
    sampler_pool_t::loadWav(pad, builtin_samples[i].name, builtin_samples[i].data, builtin_samples[i].size());
  }
  load_builtin_background_loop();
}

static void load_builtin_background_loop(void)
{
  M5.Display.print(" bgm");
  load_background_loop_memory(builtin_background_loop.data,
                              builtin_background_loop.size(),
                              builtin_background_loop.name,
                              "builtin:BGM_FA.wav",
                              2);
}

static int load_sd_samples(void) {
  // 読み込み中のWAVファイル一時バッファ上限 (16秒/48kHz/stereo + ヘッダ余裕)
  static constexpr const size_t max_wav_file_size = 3200 * 1024;

  int loaded_count = 0;
  M5.Display.print("\nSD");
  if (ensure_sampler_sd_dirs()) {
    std::vector<kp::file_info_string_t> list;
    kp::storage_sd.getFileList(list, "/sampler/samples", "");

    // .wav のみ抽出 (大文字小文字を問わない)
    list.erase(std::remove_if(list.begin(), list.end(), [](const kp::file_info_string_t& f) {
      const auto& n = f.filename;
      if (n.size() < 5) { return true; }
      std::string ext = n.substr(n.size() - 4);
      for (auto& ch : ext) { ch = tolower(ch); }
      return ext != ".wav";
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
      if (fsize > max_wav_file_size) { fsize = max_wav_file_size; }

      uint8_t* tmp = temp_alloc(fsize);
      if (tmp == nullptr) { break; }
      int len = kp::storage_sd.loadFromFileToMemory(full.c_str(), tmp, fsize);
      if (len > 44) {
        std::string name = f.filename.substr(0, f.filename.size() - 4);
        uint8_t pad = display_order_to_pad((uint8_t)loaded_count);
        if (sampler_pool_t::loadWav(pad, name.c_str(), tmp, len)) {
          snprintf(sampler_pool_t::slot[pad].file_path, sizeof(sampler_pool_t::slot[pad].file_path), "%s", full.c_str());
          ++loaded_count;
          M5.Display.print(".");
        }
      }
      free(tmp);
    }
  }
  return loaded_count;
}

static void clear_kit(void)
{
  sampler_audio_t::stopAll();
  clear_background_loop();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    sampler_pool_t::erase(i);
    pads[i].pressed = false;
    pads[i].playing_shown = false;
  }
  recording_pad = -1;
  rec_wave_pad = -1;
  edit_pad = -1;
  loop_reset_recording_state();
  draw_all();
  update_all_leds();
}

static void reload_samples_from_sd(void)
{
  clear_kit();
  if (menu_visible) { show_loading_message(); }
  if (load_sd_samples() == 0) {
    show_status_message("No SD samples", 1600, true);
    load_builtin_samples();
  }
  draw_all();
  update_all_leds();
}

static bool save_current_kit(void)
{
  if (!kp::storage_sd.beginStorage()) { return false; }
  kp::storage_sd.makeDirectory("/sampler");
  kp::storage_sd.makeDirectory("/sampler/kits");

  JsonDocument doc;
  doc["version"] = 1;
  JsonArray samples = doc["samples"].to<JsonArray>();
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    auto& slot = sampler_pool_t::slot[i];
    if (!slot.isValid()) { continue; }
    JsonObject s = samples.add<JsonObject>();
    s["pad"] = pad_display_number((uint8_t)i);
    s["internalPad"] = i;
    s["name"] = slot.name;
    s["file"] = slot.file_path;
    s["start"] = slot.start_frame;
    s["end"] = slot.end_frame;
    s["volume"] = slot.volume_q8;
    s["pitch"] = slot.pitch_q8;
    s["reverse"] = slot.reverse;
    s["hold"] = slot.hold_enabled;
    s["loop"] = slot.loop_enabled;
  }
  JsonObject loop = doc["loop"].to<JsonObject>();
  loop["lengthMs"] = loop_length_msec;
  loop["lengthFixed"] = loop_length_fixed;
  loop["quantize"] = loop_quantize_enabled;
  loop["noteGridIndex"] = loop_quantize_option_index;
  loop["noteOffGridIndex"] = loop_note_off_quantize_option_index;
  JsonObject bgm = loop["background"].to<JsonObject>();
  bgm["name"] = background_loop.name;
  bgm["file"] = background_loop.file_path;
  bgm["volume"] = background_loop.volume_q8;
  bgm["repeats"] = background_loop.loop_repeats;
  JsonArray events = loop["events"].to<JsonArray>();
  for (const auto& e : loop_events) {
    JsonObject item = events.add<JsonObject>();
    item["pad"] = e.pad;
    item["type"] = e.type == loop_event_type_t::note_on ? "on" : "off";
    item["pos"] = e.pos_ms;
    item["layer"] = e.layer;
  }
  JsonObject fx = doc["fx"].to<JsonObject>();
  fx["pitch"] = fx_param[0];
  fx["filter"] = fx_param[1];
  fx["repeat"] = fx_param[2];

  std::string out;
  serializeJson(doc, out);
  return kp::storage_sd.saveFromMemoryToFile("/sampler/kits/current.json", (const uint8_t*)out.c_str(), out.size()) >= 0;
}

static bool load_kit_file(const char* path)
{
  if (!path || !kp::storage_sd.beginStorage()) { return false; }
  int size = kp::storage_sd.getFileSize(path);
  if (size <= 0 || size > 128 * 1024) { return false; }
  uint8_t* data = temp_alloc((size_t)size + 1);
  if (!data) { return false; }
  int len = kp::storage_sd.loadFromFileToMemory(path, data, (size_t)size);
  if (len <= 0) {
    free(data);
    return false;
  }
  data[len] = 0;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, data, len);
  free(data);
  if (err) { return false; }

  clear_kit();
  if (menu_visible) { show_loading_message(); }
  JsonArray samples = doc["samples"].as<JsonArray>();
  for (JsonObject s : samples) {
    int pad = s["internalPad"] | -1;
    if (pad < 0) {
      int display_pad = s["pad"] | 0;
      if (display_pad >= 1 && display_pad <= (int)def::pad::pad_count) {
        pad = display_order_to_pad((uint8_t)(display_pad - 1));
      }
    }
    const char* file = s["file"] | "";
    if (pad < 0 || pad >= (int)def::pad::pad_count || file[0] == 0) { continue; }
    int wav_size = kp::storage_sd.getFileSize(file);
    if (wav_size <= 44 || wav_size > 3200 * 1024) { continue; }
    uint8_t* wav = temp_alloc((size_t)wav_size);
    if (!wav) { continue; }
    int wav_len = kp::storage_sd.loadFromFileToMemory(file, wav, (size_t)wav_size);
    if (wav_len > 44 && sampler_pool_t::loadWav((uint8_t)pad, s["name"] | "", wav, wav_len)) {
      auto& slot = sampler_pool_t::slot[pad];
      snprintf(slot.file_path, sizeof(slot.file_path), "%s", file);
      slot.start_frame = std::min<uint32_t>((uint32_t)(s["start"] | 0), slot.frames);
      slot.end_frame = std::min<uint32_t>((uint32_t)(s["end"] | slot.frames), slot.frames);
      slot.volume_q8 = s["volume"] | 256;
      slot.pitch_q8 = s["pitch"] | 256;
      slot.reverse = s["reverse"] | false;
      slot.hold_enabled = s["hold"] | false;
      slot.loop_enabled = s["loop"] | false;
    }
    free(wav);
  }

  JsonObject loop = doc["loop"].as<JsonObject>();
  JsonObject bgm = loop["background"].as<JsonObject>();
  const char* bgm_file = bgm["file"] | "";
  if (bgm_file[0]) {
    if (strcmp(bgm_file, "builtin:BGM_FA.wav") == 0) {
      load_builtin_background_loop();
    } else {
      load_background_loop_file(bgm_file, bgm["name"] | "BGM");
    }
    background_loop.volume_q8 = bgm["volume"] | background_loop.volume_q8;
    background_loop.loop_repeats = bgm["repeats"] | background_loop.loop_repeats;
  }
  loop_length_msec = loop["lengthMs"] | loop_default_length_ms;
  loop_length_fixed = loop["lengthFixed"] | false;
  loop_quantize_enabled = loop["quantize"] | loop_quantize_enabled;
  loop_quantize_option_index = loop["noteGridIndex"] | loop_quantize_option_index;
  loop_note_off_quantize_option_index = loop["noteOffGridIndex"] | loop_note_off_quantize_option_index;
  loop_events.clear();
  uint16_t max_layer = 0;
  for (JsonObject item : loop["events"].as<JsonArray>()) {
    loop_event_t e;
    e.pad = item["pad"] | 0;
    e.type = strcmp(item["type"] | "on", "off") == 0 ? loop_event_type_t::note_off : loop_event_type_t::note_on;
    e.pos_ms = item["pos"] | 0;
    e.layer = item["layer"] | 0;
    if (e.pad < def::pad::pad_count) {
      loop_events.push_back(e);
      if (max_layer < e.layer) { max_layer = e.layer; }
    }
  }
  loop_layer_seq = max_layer + 1;
  loop_playing = false;
  loop_prev_pos_ms = 0;
  fx_param[0] = doc["fx"]["pitch"] | fx_param[0];
  fx_param[1] = doc["fx"]["filter"] | fx_param[1];
  fx_param[2] = doc["fx"]["repeat"] | fx_param[2];
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_display_length_ms(M5.millis())));
  draw_all();
  update_all_leds();
  return true;
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

  M5.Display.setFont(&fonts::efontJA_16_b);
  M5.Display.setTextSize(1, 2);
  M5.Display.printf("%s\nver%d.%d.%d\n\nboot"
    , def::app::app_name
    , (int)def::app::app_version_major, (int)def::app::app_version_minor, (int)def::app::app_version_patch);

  power_on_base();

  M5.Power.setChargeCurrent(200);

  kp::system_registry = new kp::system_registry_t();
  M5.Display.print("."); kp::system_registry->init();
  M5.Display.print("."); audio.start();
  for (uint8_t i = 0; i < 3; ++i) {
    sampler_audio_t::setFx(i, false, fx_param[i]);
  }
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_default_length_ms));
  M5.Display.print(".");
  if (!task_i2c.start()) {
    M5.Display.print("\nhardware not found.\n");
    M5.delay(4096);
    M5.Power.powerOff();
  }
  task_midi.start();
  task_wifi.start();

  load_builtin_samples();

  wave_canvas.setColorDepth(16);
  wave_canvas.createSprite(M5.Display.width(), wave_h);
  menu_canvas.setColorDepth(16);
  menu_canvas.createSprite(M5.Display.width(), menu_area_h);

  input_history_code = kp::system_registry->internal_input.getHistoryCode();
  prev_bitmask = kp::system_registry->internal_input.getButtonBitmask();

  draw_all();
  update_all_leds();

#if !defined (M5UNIFIED_PC_BUILD)
  xTaskCreatePinnedToCore(loop_clock_task, "loopclk", 1024 * 2, nullptr,
                          kp::def::system::task_priority_i2s - 1, nullptr,
                          kp::def::system::task_cpu_i2s);
#endif
}

static void update(void)
{
  // 本体ボタン・タッチ入力の履歴を処理
  auto& input = kp::system_registry->internal_input;
  const kp::registry_base_t::history_t* h;
  while ((h = input.getHistory(input_history_code)) != nullptr) {
    switch (h->index) {
    case kp::system_registry_t::reg_internal_input_t::BUTTON_BITMASK:
      process_bitmask(h->value);
      break;
    case kp::system_registry_t::reg_internal_input_t::TOUCH_VALUE:
      process_touch(h->value);
      break;
    default:
      break;
    }
  }

  // オペレーターコマンド (電源オフ等) の処理
  kp::def::command::command_param_t cp;
  bool is_pressed;
  while (kp::system_registry->operator_command.getQueue(&opcmd_history_code, &cp, &is_pressed)) {
    if (cp.command == kp::def::command::system_control && is_pressed) {
      auto sc = (kp::def::command::system_control_t)cp.param;
      if (sc == kp::def::command::system_control_t::sc_power_off
       || sc == kp::def::command::system_control_t::sc_reset) {
        kp::system_registry->runtime_info.setPowerOff(sc);
      }
    }
  }

  uint32_t msec = M5.millis();

  service_pad_recording();
#if defined (M5UNIFIED_PC_BUILD)
  service_loop(msec);
#endif

  if (menu_visible || learn_state != learn_state_t::idle) {
    static uint32_t prev_status_anim_msec = 0;
    if (menu_visible && status_message_busy && status_message_visible(msec) && msec - prev_status_anim_msec >= 120) {
      prev_status_anim_msec = msec;
      draw_menu();
      return;
    }
    if (menu_visible && status_message[0] && status_message_until && (int32_t)(msec - status_message_until) >= 0) {
      clear_status_message(true);
    }
    return;
  }

  if (edit_value_compact_visible && (int32_t)(msec - edit_value_activity_until) >= 0) {
    edit_value_compact_visible = false;
    draw_wave();
  }

  // バッテリー残量/充電状態はI2Cタスクで更新されるため、ヘッダーも定期更新する。
  static uint32_t prev_header_msec = 0;
  if (msec - prev_header_msec >= 1000) {
    prev_header_msec = msec;
    draw_header();
  }

  // 再生中Padのハイライト表示更新
  static uint32_t prev_play_msec = 0;
  if (msec - prev_play_msec >= 100) {
    prev_play_msec = msec;
    for (int i = 0; i < (int)def::pad::pad_count; ++i) {
      bool playing = sampler_audio_t::isPlaying(i);
      if (pads[i].playing_shown != playing) {
        pads[i].playing_shown = playing;
        update_pad_led(i);
        draw_pad(i);
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
    wave_interval = 80;
  } else if (current_mode == sampler_mode_t::mode_play) {
    wave_interval = 50;
  }
  if (wave_interval != 0 && msec - prev_wave_msec >= wave_interval) {
    prev_wave_msec = msec;
    draw_wave();
  }
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
