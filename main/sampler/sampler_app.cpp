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
//   SDカード /sampler/*.wav (PCM16 mono/stereo 〜48kHz) を起動時に名前順で最大12個
//   PSRAMプールへ読み込む。SDに無い場合は組み込みサンプルを使用。
//   演奏中は SD にアクセスしない (レスポンス保証のため)。

#if defined (KANPLAY_SAMPLER)

#include <M5Unified.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../common_define.hpp"
#include "../system_registry.hpp"
#include "../task_i2c.hpp"
#include "../file_manage.hpp"

#include "sampler_define.hpp"
#include "sampler_audio.hpp"
#include "sampler_pool.hpp"
#include "sampler_samples.hpp"

namespace sampler_ns {
namespace kp = kanplay_ns;
using def::mode::sampler_mode_t;
using def::pad::play_type_t;

//-------------------------------------------------------------------------
// 状態

struct pad_state_t {
  bool pressed = false;
  bool playing_shown = false;  // 再生中ハイライトの表示状態 (再描画判定用)
};

static sampler_audio_t audio;
static kp::task_i2c_t task_i2c;

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
static uint8_t edit_param = 0;  // 0=Start, 1=End, 2=Volume

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
static recording_source_t recording_source = recording_source_t::internal_mic;
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

static M5Canvas wave_canvas(&M5.Display);

static void loop_repeat_set_active(bool active);

//-------------------------------------------------------------------------
// レイアウト定数 (240x320 縦画面)

static constexpr const int32_t header_h   = 24;
static constexpr const int32_t wave_y     = 25;
static constexpr const int32_t wave_h     = 112;
static constexpr const int32_t tab_y      = 143;
static constexpr const int32_t tab_h      = 30;
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
  { "EDIT", "CHOP", "DEL"  },  // REC
  { "ONE",  "HOLD", "LOOP" },  // PLAY
  { "END",  "MUTE", "DEL"  },  // LOOP
  { "PITCH", "FILTER", "REPEAT" },  // FX
};
static constexpr const char* const edit_fn_labels[3] = { "PARAM", "REV", "EXIT" };
static constexpr const char* const edit_param_labels[3] = { "START", "END", "VOLUME" };

// 再生方式バッジ (One/Hold/Loop)
static constexpr const char* const play_type_badge[] = { "O", "H", "L" };

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

static void draw_battery_icon(int x, int y, int w, int h)
{
  auto& d = M5.Display;
  uint8_t battery = kp::system_registry->runtime_info.getBatteryLevel();
  if (battery > 100) { battery = 100; }
  bool charging = kp::system_registry->runtime_info.getBatteryCharging();

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

static void draw_volume_icon(int x, int y, int w, int h)
{
  auto& d = M5.Display;
  uint8_t volume = kp::system_registry->user_setting.getMasterVolume();
  int r = 10;
  int cx = x + w - 12;
  int cy = y + 11;
  int arc = volume * 36 / 10;
  d.fillCircle(cx, cy, r, 0x303030u);
  d.drawCircle(cx, cy, r, TFT_LIGHTGRAY);
  d.setColor(TFT_WHITE);
  d.fillArc(cx, cy, r, 0, 90, 90 + arc);
}

static void draw_header(void) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillRect(0, 0, d.width(), header_h, 0x000000u);
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
    , (unsigned)(sampler_pool_t::usedBytes() >> 20)
    , (unsigned)(((sampler_pool_t::usedBytes() & 0xFFFFF) * 10) >> 20));
  d.setTextDatum(m5gfx::textdatum_t::middle_right);
  d.setTextColor(0xA0A0B0u, 0x000000u);
  d.drawString(buf, right_x - 4, header_h / 2);

  draw_battery_icon(right_x, 0, battery_icon_w, header_h);
  draw_volume_icon(right_x + battery_icon_w + icon_gap, 0, volume_icon_w, header_h);
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
  c.drawFastHLine(0, h / 2, w, 0x204060u);
  uint32_t length_ms = loop_display_length_ms(M5.millis());
  for (int beat = 0; beat <= 4; ++beat) {
    int x = (beat * (w - 1)) / 4;
    c.drawFastVLine(x, 0, h, beat == 0 ? 0x4060A0u : 0x203050u);
  }
  for (const auto& e : loop_events) {
    if (e.pad >= def::pad::pad_count || loop_pad_mute[e.pad]) { continue; }
    int x = ((uint64_t)e.pos_ms * (w - 1)) / length_ms;
    int lane_h = std::max<int>(1, (h - 16) / def::pad::pad_count);
    int mark_h = std::min<int>(6, std::max<int>(3, lane_h - 1));
    int y = 8 + e.pad * lane_h;
    uint32_t color = sample_colors[e.pad % (sizeof(sample_colors) / sizeof(sample_colors[0]))].bg_hi;
    if (e.type == loop_event_type_t::note_off) {
      c.drawFastHLine(x - 2, y + mark_h / 2, 5, color);
    } else {
      c.fillRect(x - 1, y, 3, mark_h, color);
    }
  }
  int play_x = ((uint64_t)loop_pos_ms(M5.millis()) * (w - 1)) / length_ms;
  c.drawFastVLine(play_x, 0, h, loop_playing ? 0xFFFFFFu : 0x808090u);
  c.setTextSize(1);
  c.setTextDatum(m5gfx::textdatum_t::top_left);
  c.setTextColor(0xFFFFFFu, 0x080810u);
  char info[40];
  snprintf(info, sizeof(info), "%s %s %u %.1fs"
    , loop_playing ? "LOOP" : "STOP"
    , loop_record_enabled ? "REC" : "OFF"
    , (unsigned)loop_events.size()
    , (double)length_ms / 1000.0);
  c.drawString(info, 3, 3);
  c.pushSprite(0, wave_y);
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
    c.setTextSize(1, 2);
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
      int fill = ((bw / 2 - 1) * (int)fx_param[i]) / 100;
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
  c.pushSprite(0, wave_y);
}

static void draw_sample_points(M5Canvas& c, const sample_slot_t& slot, bool show_active_param)
{
  if (slot.frames == 0) { return; }
  const int w = c.width();
  const int h = c.height();
  int sx = ((uint64_t)slot.playStart() * w) / slot.frames;
  int ex = ((uint64_t)slot.playEnd() * w) / slot.frames;
  if (sx < 0) { sx = 0; }
  if (sx >= w) { sx = w - 1; }
  if (ex < 0) { ex = 0; }
  if (ex >= w) { ex = w - 1; }
  c.fillRect(0, 0, sx, h, 0x181820u);
  c.fillRect(ex + 1, 0, w - ex - 1, h, 0x181820u);
  uint32_t start_color = (show_active_param && edit_param == 0) ? 0xFF7050u : 0xB05040u;
  uint32_t end_color = (show_active_param && edit_param == 1) ? 0x50A0FFu : 0x4070B0u;
  c.drawFastVLine(sx, 0, h, start_color);
  c.drawFastVLine(sx + 1 < w ? sx + 1 : sx, 0, h, start_color);
  c.drawFastVLine(ex, 0, h, end_color);
  c.drawFastVLine(ex > 0 ? ex - 1 : ex, 0, h, end_color);
  c.fillTriangle(sx, 0, sx + 5, 0, sx, 5, start_color);
  c.fillTriangle(ex, h - 1, ex - 5, h - 1, ex, h - 6, end_color);
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
        int16_t v = (int16_t)apply_wave_volume(slot.pcm[i], slot.volume_q8);
        if (mn > v) { mn = v; }
        if (mx < v) { mx = v; }
      }
      int y0 = (h / 2) - ((int)mx * (h / 2 - 3)) / 32768;
      int y1 = (h / 2) - ((int)mn * (h / 2 - 3)) / 32768;
      if (y1 <= y0) { y1 = y0 + 1; }
      c.drawFastVLine(x, y0, y1 - y0, 0xD0B050u);
    }
    draw_sample_points(c, slot, true);
    c.setTextSize(1);
    c.setTextDatum(m5gfx::textdatum_t::top_left);
    c.setTextColor(0xFFFFFFu, 0x080810u);
    char info[48];
    snprintf(info, sizeof(info), "%s P%d %.2fs V%u%s"
      , edit_param_labels[edit_param]
      , edit_pad + 1
      , slot.sample_rate ? (float)slot.playFrames() / slot.sample_rate : 0.0f
      , (unsigned)((slot.volume_q8 * 100u) / 256u)
      , slot.reverse ? " R" : "");
    c.drawString(info, 3, 3);

    uint32_t accent = edit_param == 0 ? 0xFF7050u : edit_param == 1 ? 0x50A0FFu : 0x60E080u;
    char value[16];
    if (edit_param == 0) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playStart() / slot.sample_rate : 0.0f);
    } else if (edit_param == 1) {
      snprintf(value, sizeof(value), "%.2fs", slot.sample_rate ? (float)slot.playEnd() / slot.sample_rate : 0.0f);
    } else {
      snprintf(value, sizeof(value), "%u%%", (unsigned)((slot.volume_q8 * 100u) / 256u));
    }
    const int chip_w = 92;
    const int chip_h = 36;
    const int chip_x = (w - chip_w) / 2;
    const int chip_y = (h - chip_h) / 2;
    c.fillRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x040408u);
    c.drawRoundRect(chip_x, chip_y, chip_w, chip_h, 5, 0x303038u);
    c.drawRoundRect(chip_x + 1, chip_y + 1, chip_w - 2, chip_h - 2, 4, accent);
    c.setTextDatum(m5gfx::textdatum_t::middle_center);
    c.setTextSize(2, 2);
    c.setTextColor(0x000000u);
    c.drawString(edit_param_labels[edit_param], w / 2 + 1, chip_y + 14);
    c.setTextColor(0xFFFFFFu);
    c.drawString(edit_param_labels[edit_param], w / 2, chip_y + 13);
    c.setTextSize(1);
    c.setTextColor(0x000000u);
    c.drawString(value, w / 2 + 1, chip_y + 29);
    c.setTextColor(accent);
    c.drawString(value, w / 2, chip_y + 28);
    c.pushSprite(0, wave_y);
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
          int16_t v = (int16_t)apply_wave_volume(slot.pcm[i], slot.volume_q8);
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
      char info[40];
      snprintf(info, sizeof(info), "REC P%d %.2fs V%u"
        , rec_wave_pad + 1
        , slot.sample_rate ? (float)slot.playFrames() / slot.sample_rate : 0.0f
        , (unsigned)((slot.volume_q8 * 100u) / 256u));
      c.drawString(info, 3, 3);
    } else {
      c.setTextSize(1, 2);
      c.setTextDatum(m5gfx::textdatum_t::middle_center);
      c.setTextColor(0x808090u, 0x080810u);
      c.drawString(recording_pad >= 0 ? "REC" : "SELECT PAD", w / 2, h / 2);
    }
    c.pushSprite(0, wave_y);
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
    c.pushSprite(0, wave_y);
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
  c.pushSprite(0, wave_y);
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
    d.setTextSize(1, 2);
    d.setTextDatum(m5gfx::textdatum_t::middle_center);
    d.setTextColor(active ? 0x000000u : 0x9090A0u);
    int tx = x + tab_w / 2;
    int ty = tab_y + tab_h / 2;
    d.drawString(info.name, tx, ty);
    d.drawString(info.name, tx + 1, ty);
  }
  d.endWrite();
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
    uint32_t wave_color = hi ? 0x101010u : 0xF0D060u;
    uint32_t center_color = hi ? 0x404040u : 0x805020u;
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
    // 再生方式バッジ
    d.setTextSize(1);
    d.setTextDatum(m5gfx::textdatum_t::top_right);
    if (loop_pad_mute[pad]) {
      d.setTextColor(hi ? 0x600000u : 0xFF6060u);
      d.drawString("M", x + pad_w - 3, y + 3);
    } else {
      d.setTextColor(hi ? 0x303030u : 0x9090A0u);
      d.drawString(play_type_badge[(int)slot.play_type], x + pad_w - 3, y + 3);
    }
  }
  d.endWrite();
}

static void draw_fn(int fn) {
  auto& d = M5.Display;
  const int y = grid_y + fn * row_pitch;
  bool fx_mode = edit_pad < 0 && current_mode == sampler_mode_t::mode_fx;
  bool active = fn_pressed[fn];
  uint32_t bg = active ? fn_color.bg_hi : fn_color.bg;
  if (fx_mode && fx_selected == fn && !active) { bg = 0x503060u; }
  d.startWrite();
  d.fillRoundRect(fn_x, y, fn_w, cell_h, 6, bg);
  if (fx_mode && fn_pressed[fn]) {
    d.drawRoundRect(fn_x, y, fn_w, cell_h, 6, 0xF0D060u);
  }
  d.setTextSize(1, 2);
  d.setTextDatum(m5gfx::textdatum_t::middle_center);
  d.setTextColor(active ? 0xFFFFFFu : 0x9090C0u);
  const char* label = edit_pad >= 0 ? edit_fn_labels[fn] : fn_labels[(int)current_mode][fn];
  if (edit_pad < 0 && current_mode == sampler_mode_t::mode_loop && fn == 0) {
    label = loop_length_fixed ? (loop_playing ? "STOP" : "PLAY") : "END";
  }
  int tx = fn_x + fn_w / 2;
  int ty = y + cell_h / 2;
  d.drawString(label, tx, ty);
  d.drawString(label, tx + 1, ty);
  d.endWrite();
}

static void draw_grid(void) {
  for (int i = 0; i < (int)def::pad::pad_count; ++i) { draw_pad(i); }
  for (int i = 0; i < 3; ++i) { draw_fn(i); }
}

static void draw_all(void) {
  M5.Display.fillScreen(0x101018u);
  draw_header();
  draw_wave();
  draw_tabs();
  draw_grid();
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
    loop_length_fixed = false;
    loop_length_msec = loop_default_length_ms;
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
  loop_length_fixed = false;
  loop_length_msec = loop_default_length_ms;
  loop_layer_seq = 1;
  for (int i = 0; i < (int)def::pad::pad_count; ++i) {
    loop_pad_mute[i] = false;
    loop_active_layer[i] = 0;
    loop_deferred_note_on_layer[i] = 0;
    loop_deferred_live_pad[i] = false;
    loop_deferred_live_pos_ms[i] = 0;
  }
  sampler_audio_t::stopAll();
  sampler_audio_t::setFxQuantizeStepMs(loop_quantize_step_ms(loop_default_length_ms));
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
      slot.play_type = play_type_t::play_one;
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
  if (edit_pad < 0 || edit_pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[edit_pad];
  if (!slot.isValid() || slot.playFrames() == 0) { return; }
  sampler_audio_t::play(edit_pad, slot.pcm + slot.playStart(), slot.playFrames(), slot.sample_rate,
                        false, slot.reverse, slot.volume_q8);
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
  if (edit_param == 2) {
    int value = (int)slot.volume_q8 + diff * 13; // 約5%
    if (value < 0) { value = 0; }
    if (value > 512) { value = 512; }
    slot.volume_q8 = (uint16_t)value;
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
  switch (slot.play_type) {
  default:
  case play_type_t::play_one:   // 押すと最後まで再生
  case play_type_t::play_hold:  // 押している間だけ再生 (releaseで停止)
    sampler_audio_t::play(pad, pcm, frames, slot.sample_rate, false, slot.reverse, slot.volume_q8);
    break;
  case play_type_t::play_loop:  // 押すとループ開始 / 再度押すと停止
    if (sampler_audio_t::isPlaying(pad)) {
      sampler_audio_t::stop(pad);
    } else {
      sampler_audio_t::play(pad, pcm, frames, slot.sample_rate, true, slot.reverse, slot.volume_q8);
    }
    break;
  }
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
    if (slot.play_type == play_type_t::play_hold || slot.play_type == play_type_t::play_loop) {
      sampler_audio_t::stop(pad);
    }
    return;
  }
  if (event.layer != 0 && loop_deferred_note_on_layer[pad] == event.layer) {
    loop_deferred_note_on_layer[pad] = 0;
  }
  if (loop_pad_mute[pad]) { return; }

  bool loop = slot.play_type == play_type_t::play_loop;
  sampler_audio_t::play(pad, slot.pcm + slot.playStart(), slot.playFrames(), slot.sample_rate,
                        loop, slot.reverse, slot.volume_q8);
}

static void loop_toggle_play(void)
{
  uint32_t now = M5.millis();
  if (loop_playing) {
    loop_prev_pos_ms = loop_pos_ms(now);
    loop_playing = false;
    for (int i = 0; i < (int)def::pad::pad_count; ++i) {
      loop_deferred_live_pad[i] = false;
    }
    sampler_audio_t::stopAll();
  } else {
    loop_start_msec = now - loop_prev_pos_ms;
    loop_playing = true;
  }
  draw_wave();
}

static void stop_all_audio(void)
{
  uint32_t now = M5.millis();
  if (loop_playing) {
    loop_prev_pos_ms = loop_pos_ms(now);
    loop_playing = false;
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
    if (loop_events.empty() && loop_record_enabled) {
      loop_start_length_capture(now);
    } else {
      loop_prev_pos_ms = 0;
      loop_start_msec = now;
      loop_playing = true;
    }
  }
  uint32_t raw_pos = loop_pos_ms(now);
  uint32_t pos = loop_length_fixed ? quantize_loop_pos_ms(raw_pos, loop_length_msec) : raw_pos;
  uint16_t layer = loop_layer_seq++;
  bool defer_note_on = loop_should_defer_quantized_note(raw_pos, pos);
  push_loop_event((uint8_t)pad, loop_event_type_t::note_on, pos, layer);
  if (sampler_pool_t::slot[pad].play_type == play_type_t::play_hold) {
    loop_active_layer[pad] = layer;
  }
  if (defer_note_on) {
    loop_deferred_note_on_layer[pad] = layer;
  } else {
    trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_on, raw_pos, layer });
  }
  loop_prev_pos_ms = raw_pos;
  draw_wave();
}

static void loop_record_pad_release(int pad)
{
  if (pad < 0 || pad >= (int)def::pad::pad_count) { return; }
  auto& slot = sampler_pool_t::slot[pad];
  if (!slot.isValid() || slot.play_type != play_type_t::play_hold || loop_active_layer[pad] == 0) { return; }
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
    draw_wave();
    return;
  }
  push_loop_event((uint8_t)pad, loop_event_type_t::note_off, pos, layer);
  trigger_loop_event({ (uint8_t)pad, loop_event_type_t::note_off, raw_pos, layer });
  loop_prev_pos_ms = raw_pos;
  draw_wave();
}

static void pad_press(int pad) {
  if (pads[pad].pressed) { return; }
  pads[pad].pressed = true;
  auto& slot = sampler_pool_t::slot[pad];

  if (current_mode == sampler_mode_t::mode_play
   && (fn_pressed[0] || fn_pressed[1] || fn_pressed[2])) {
    // PLAYモード: Fn(ONE/HOLD/LOOP)を押しながらPadで再生方式を設定
    if (slot.isValid()) {
      slot.play_type = fn_pressed[0] ? play_type_t::play_one
                     : fn_pressed[1] ? play_type_t::play_hold
                                     : play_type_t::play_loop;
    }
  } else if (current_mode == sampler_mode_t::mode_rec && fn_pressed[2]) {
    // RECモード: DEL + Pad でサンプル削除
    if (slot.isValid()) {
      sampler_audio_t::stop(pad);
      if (edit_pad == pad) { edit_pad = -1; }
      if (rec_wave_pad == pad) { rec_wave_pad = -1; }
      loop_remove_pad_events(pad);
      loop_reset_recording_state_if_empty();
      sampler_pool_t::erase(pad);
      draw_header();  // プール使用量の表示を更新
      draw_wave();
    }
  } else if (current_mode == sampler_mode_t::mode_rec && fn_pressed[0]) {
    // RECモード: EDIT + Pad で編集対象を選択
    enter_edit(pad);
    preview_edit_pad();
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
  } else if (current_mode == sampler_mode_t::mode_rec && !slot.isValid()) {
    start_pad_recording(pad);
  } else if (current_mode == sampler_mode_t::mode_rec && slot.isValid()) {
    rec_wave_pad = pad;
    trigger_pad(pad);
    draw_wave();
  } else {
    if (!defer_live_pad_if_early(pad)) { trigger_pad(pad); }
  }
  update_pad_led(pad);
  draw_pad(pad);
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
  if (loop_deferred_live_pad[pad] && slot.isValid() && slot.play_type == play_type_t::play_hold) {
    loop_deferred_live_pad[pad] = false;
  }
  if (edit_pad < 0 && slot.isValid() && slot.play_type == play_type_t::play_hold) {
    sampler_audio_t::stop(pad);
  }
  update_pad_led(pad);
  draw_pad(pad);
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
    if (value < -100) { value = -100; }
    if (value > 100) { value = 100; }
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
      if (press && edit_pad >= 0) {
        if (fn == 0) {
          edit_param = (edit_param + 1) % 3;
          draw_wave();
        } else if (fn == 1) {
          auto& slot = sampler_pool_t::slot[edit_pad];
          if (slot.isValid()) {
            slot.reverse = !slot.reverse;
            draw_wave();
            preview_edit_pad();
          }
        } else {
          exit_edit();
        }
      } else if (press && current_mode == sampler_mode_t::mode_loop) {
        if (fn == 0) {
          loop_handle_top_button();
        } else if (fn == 1) {
          draw_wave();
        } else if (fn == 2) {
          loop_del_touched_pad = false;
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
// サンプルの読み込み (SDカード /sampler/*.wav → PSRAMプール)

static uint8_t* temp_alloc(size_t bytes) {
#if defined (M5UNIFIED_PC_BUILD)
  return (uint8_t*)malloc(bytes);
#else
  return (uint8_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
}

static void load_kit(void) {
  // 読み込み中のWAVファイル一時バッファ上限 (16秒/48kHz/stereo + ヘッダ余裕)
  static constexpr const size_t max_wav_file_size = 3200 * 1024;

  int next_pad = 0;
  M5.Display.print("\nSD");
  if (kp::storage_sd.beginStorage()) {
    kp::storage_sd.makeDirectory("/sampler");

    std::vector<kp::file_info_string_t> list;
    kp::storage_sd.getFileList(list, "/sampler", "");

    // .wav のみ抽出 (大文字小文字を問わない)
    list.erase(std::remove_if(list.begin(), list.end(), [](const kp::file_info_string_t& f) {
      const auto& n = f.filename;
      if (n.size() < 5) { return true; }
      std::string ext = n.substr(n.size() - 4);
      for (auto& ch : ext) { ch = tolower(ch); }
      return ext != ".wav";
    }), list.end());

    // 名前順で最大12個をPadへ割り当て
    std::sort(list.begin(), list.end(),
      [](const kp::file_info_string_t& a, const kp::file_info_string_t& b) {
        return a.filename < b.filename;
      });

    for (const auto& f : list) {
      if (next_pad >= (int)def::pad::pad_count) { break; }
      std::string full = std::string("/sampler/") + f.filename;
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
        if (sampler_pool_t::loadWav(next_pad, name.c_str(), tmp, len)) {
          ++next_pad;
          M5.Display.print(".");
        }
      }
      free(tmp);
    }
  }

  if (next_pad == 0) {
    // SDにサンプルが無い場合は組み込みサンプルを使用
    M5.Display.print(" builtin");
    for (size_t i = 0; i < builtin_sample_count && i < def::pad::pad_count; ++i) {
      sampler_pool_t::loadWav(i, builtin_samples[i].name, builtin_samples[i].data, builtin_samples[i].size());
    }
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

  M5.Display.setTextSize(2);
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

  load_kit();

  wave_canvas.setColorDepth(16);
  wave_canvas.createSprite(M5.Display.width(), wave_h);

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
  uint32_t wave_interval = 0;
  if ((current_mode == sampler_mode_t::mode_loop || current_mode == sampler_mode_t::mode_play) && loop_playing) {
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
