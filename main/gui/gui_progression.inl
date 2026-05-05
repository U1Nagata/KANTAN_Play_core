// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct ui_chord_part_container_t : public ui_container_t
{
  static constexpr int16_t slot_display_duration = 2100; // 操作後の表示持続時間(ms)
  static constexpr float slot_number_text_size = 2.4f;
  static constexpr int16_t slot_number_rect_w = 168;
  static constexpr int16_t slot_number_rect_h = 120;

protected:
  def::gui_mode_t _prev_mode;
  uint8_t _prev_slot_index = 255;
  int16_t _slot_display_remain = INT16_MAX; // 起動時は操作があるまで表示し続ける
  uint32_t _prev_change_counter = 0;
  bool _waiting_next_input = true; // 起動時は最初の操作を待つ

  rect_t getSlotNumberRect(int32_t offset_x, int32_t offset_y) const
  {
    int32_t cx = offset_x + (_client_rect.w >> 1);
    int32_t cy = offset_y + (_client_rect.h >> 1);
    return { cx - (slot_number_rect_w >> 1), cy - (slot_number_rect_h >> 1), slot_number_rect_w, slot_number_rect_h };
  }

  void update_impl(draw_param_t *param, int offset_x, int offset_y) override
  {
    auto mode = system_registry->runtime_info.getGuiMode();
    if (_prev_mode != mode) {
M5_LOGV("ui_chord_part_container_t::update_impl: mode changed %d -> %d", (int)_prev_mode, (int)mode);
      _prev_mode = mode;
      switch (mode) {
      default:
        setTargetRect({0, header_height, main_area_width, main_area_height}); // TODO: 仮の値
        break;

      case def::gui_mode_t::gm_menu:
        setTargetRect({0, header_height, main_area_width, main_area_height});
        break;
      }
    }
    // スロット番号が変化したら即座に表示
    auto slot_index = system_registry->runtime_info.getPlaySlot();
    if (_prev_slot_index != slot_index) {
      _prev_slot_index = slot_index;
      _slot_display_remain = INT16_MAX;
      _waiting_next_input = true;
      _prev_change_counter = system_registry->working_command.getChangeCounter();
      param->addInvalidatedRect(getSlotNumberRect(offset_x, offset_y));
    }
    // スロット変化後、次のボタン操作でカウントダウン開始
    if (_waiting_next_input) {
      auto counter = system_registry->working_command.getChangeCounter();
      if (counter != _prev_change_counter) {
        _waiting_next_input = false;
        _slot_display_remain = slot_display_duration;
        _prev_change_counter = counter;
      }
    }
    // カウントダウン中：0になったタイミングで再描画して消す
    if (!_waiting_next_input && _slot_display_remain > 0) {
      _slot_display_remain -= param->smooth_step;
      if (_slot_display_remain <= 0) {
        _slot_display_remain = 0;
        param->addInvalidatedRect(getSlotNumberRect(offset_x, offset_y));
      }
    }
    ui_container_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t* param, M5Canvas* canvas, int32_t offset_x,
      int32_t offset_y, const rect_t* clip_rect) override
  {
    // 子ウィジェット（パートセル）を先に描画
    ui_container_t::draw_impl(param, canvas, offset_x, offset_y, clip_rect);

    // パート編集中・表示期間外はオーバーレイ非表示
    if (system_registry->runtime_info.getGuiMode() == def::gui_mode_t::gm_part_edit) { return; }
    if (_slot_display_remain <= 0) { return; }
    if (_client_rect.empty()) { return; }

    int32_t cx = offset_x + (_client_rect.w >> 1);
    int32_t cy = offset_y + (_client_rect.h >> 1);

    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
    canvas->setFont(&fonts::lv_font_montserrat_48);
    canvas->setTextSize(slot_number_text_size);
    canvas->setTextColor(0x0A1830u);

    char buf[4];
    snprintf(buf, sizeof(buf), "%d", _prev_slot_index + 1);
    canvas->drawString(buf, cx, cy);

    canvas->setFont(&fonts::efontJA_16_b);
    canvas->setTextSize(1);
    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
    canvas->setTextColor(TFT_WHITE);
  }
};
static ui_chord_part_container_t ui_chord_part_container;

struct ui_progression_timeline_t : public ui_base_t
{
protected:
  static constexpr const int32_t max_visible_step = 5;
  static constexpr const int32_t step_icon_width = main_area_width / max_visible_step;
  int16_t _current_step_index = 0;
  int32_t _x_scroll_current = 0;
  int32_t _x_scroll_target = 0;
  int32_t _x_scroll_offset = 0;
  uint8_t _offset_step = 0;
  uint8_t _highlight_index = 0;
  bool _no_data = false;
  def::gui_mode_t _prev_mode = def::gui_mode_t::gm_unknown;

  progression_desc_t _desc[max_visible_step + 2];
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto mode = system_registry->runtime_info.getGuiMode();
    bool visible = false;
    switch (mode) {
    case def::gui_mode_t::gm_song_play:
    case def::gui_mode_t::gm_song_recording:
      visible = true;
      break;
    default:
      break;
    }
    bool need_init = false;
    if (_prev_mode != mode) {
      need_init = visible;
      _prev_mode = mode;
      auto r = getTargetRect();
      if (visible) {
        if (mode == def::gui_mode_t::gm_song_recording) {
          _offset_step = 2;
        } else {
          _offset_step = 0;
        }
        // r.x = 0;
        // r.w = disp_width;
        r.y = disp_height - main_btns_height;
        r.h = main_btns_height;
      } else {
        // r.x = disp_width;
        // r.w = 0;
        r.y = disp_height;
        r.h = 0;
      }
      setTargetRect(r);
    }
    // 表示データが無い場合の判定
    _no_data = (_offset_step == 0)
            && (system_registry->current_progression->info.getLength() == 0);

    ui_base_t::update_impl(param, offset_x, offset_y);

    if (_client_rect.empty()) { return; }

    int32_t current_stepindex = (int32_t)system_registry->runtime_info.getProgressionPosition();
    _x_scroll_target = current_stepindex * step_icon_width * 256;
    if (need_init && visible) {
      _x_scroll_current = _x_scroll_target;
    } else {
      _x_scroll_current = smooth_move(_x_scroll_target, _x_scroll_current, (param->smooth_step + 2) >> 2);
    }
    int32_t visible_stepindex = _x_scroll_current / (step_icon_width * 256);
    int32_t visible_scroll = visible_stepindex * step_icon_width * 256;
    int32_t offset = (visible_scroll - _x_scroll_current + 128) >> 8;
    visible_stepindex -= _offset_step;

    _highlight_index = current_stepindex - visible_stepindex;
    visible_stepindex -= 1;

    if (_current_step_index != visible_stepindex || _x_scroll_offset != offset || need_init) {
      _current_step_index = visible_stepindex;
      _x_scroll_offset = offset;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
      for (int32_t i = 0; i < max_visible_step+2; ++i) {
        _desc[i] = system_registry->current_progression->getStepDescriptor(visible_stepindex + i);
      }
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    image_dark_shift(canvas, offset_x, offset_y, _client_rect.w, _client_rect.h, 2);

    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
    canvas->setTextColor(TFT_WHITE);
    int32_t y_degree = offset_y + (main_btns_height >> 1) - 1;
    int32_t xe = offset_x + _x_scroll_offset;

    // image_dark_shift(canvas, offset_x + _offset_step * step_icon_width, offset_y, step_icon_width, main_btns_height);
    // image_color_or(canvas, offset_x + _offset_step * step_icon_width, offset_y, step_icon_width - 1, main_btns_height, 0x4200u);
    // image_color_or(canvas, offset_x + _offset_step * step_icon_width, offset_y, 1, main_btns_height, 0x8410u);
    // image_color_or(canvas, offset_x + (_offset_step+1) * step_icon_width-1, offset_y, 1, main_btns_height, 0x8410u);
    for (int32_t i = 0; i <= max_visible_step; ++i) {
      int32_t x = xe;
      xe += step_icon_width;
      if (xe < clip_rect->left() || clip_rect->right() < x) {
        continue;
      }
      if (i == _highlight_index) {
        int xtmp = x < clip_rect->left() ? clip_rect->left() : x;
        int xetmp = xe > clip_rect->right() ? clip_rect->right() : xe;
        image_dark_shift(canvas, xtmp, offset_y, xetmp - xtmp, main_btns_height);
        image_color_or(canvas, xtmp, offset_y, xetmp - xtmp, main_btns_height, 0x4200);
      }
      if (xe > clip_rect->left()) {
        image_color_or(canvas, xe - 1, offset_y, 1, main_btns_height, 0x8410);
      }
      // canvas->drawFastVLine(xe - 1, offset_y, main_btns_height, TFT_LIGHTGRAY);

      const auto &prev_desc = _desc[i];
      const auto &desc = _desc[i+1];
      const auto degree = desc.getDegree();
   // if (degree && prev_desc != desc)
      if (degree)
      {
        int32_t x_center = x + (step_icon_width >> 1);

        int32_t y_mod = y_degree + (main_btns_height / 3);
        if (y_mod+12 > clip_rect->top() && y_mod-12 < clip_rect->bottom())
        {
          auto modifier = desc.getModifier();
          auto name_tbl = def::command::command_name_table[def::command::command_t::chord_modifier];
          int32_t x_mod = x + (step_icon_width >> 1);
          const char* mod_str = name_tbl[modifier];
          canvas->setTextSize(1, 2);
          canvas->drawString(mod_str, x_mod, y_mod);
        }

        if (y_degree+12 > clip_rect->top() && y_degree-12 < clip_rect->bottom()) {
          auto x_degree = x_center;
          auto semitone = desc.getSemitoneShift();
          auto minor_swap = desc.getMinorSwap();
          if (semitone || minor_swap) {
            canvas->setTextSize(1, 1);
            x_degree -= 8;
            if (semitone) {
              auto text = (semitone < 0) ? "♭" : "♯";
              canvas->drawString(text, x_degree + 16, y_degree - 6);
            }
            if (minor_swap) {
              canvas->drawString("〜", x_degree + 16, y_degree + 6);
            }
          }
          canvas->setTextSize(2, 2);
          canvas->drawNumber(degree, x_degree, y_degree);
        }

        if (prev_desc.getPartBits() != desc.getPartBits() || prev_desc.getSlotIndex() != desc.getSlotIndex())
        {
          // パート矩形: 1行12px、2行で合計25px（+3px拡張）
          static constexpr int32_t part_row_h  = 12;  // 1行の高さ
          static constexpr int32_t part_top    = 4;   // エリア上端マージン
          static constexpr int32_t part_gap    = 1;   // 行間
          static constexpr int32_t part_cell_w_div = 4;  // セル幅 = step_icon_width / 4

          uint32_t color_enable  = system_registry->color_setting.getEnablePartColor();
          uint32_t color_disable = system_registry->color_setting.getDisablePartColor();

          for (int part = 0; part < def::app::max_chord_part; ++part)
          {
            auto color = desc.getPartEnable(part) ? color_enable : color_disable;
            int32_t row   = part < 3 ? 0 : 1;
            int32_t y_part = offset_y + part_top + row * (part_row_h + part_gap);
            int32_t x_part = x + (part % 3) * (step_icon_width / part_cell_w_div) + (step_icon_width / 8);
            canvas->fillRect(x_part, y_part, (step_icon_width / part_cell_w_div) - 1, part_row_h, color);
          }

          // スロット番号をパートエリア中央に黄色太字で重ねて表示（1始まりUI）
          {
            auto slotindex = desc.getSlotIndex();
            int32_t part_area_h = part_row_h * 2 + part_gap;
            int32_t y_slot = offset_y + part_top + (part_area_h >> 1);
            int32_t x_slot = x + (step_icon_width >> 1);
            canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
            canvas->setTextColor(0xE7E7E7u);  // #E7E7E7（RGB888）
            canvas->setFont(&fonts::FreeSansBold9pt7b);
            canvas->setTextSize(1);
            char slot_buf[4];
            snprintf(slot_buf, sizeof(slot_buf), "%d", (int)(slotindex + 1));
            canvas->drawString(slot_buf, x_slot, y_slot);
            canvas->setFont(&fonts::efontJA_16_b);  // canvasのデフォルトフォントに戻す
            canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
            canvas->setTextColor(TFT_WHITE);
          }
        }
      }
    }
    for (int i = 1; i < 3; ++i) {
      image_color_or(canvas, offset_x, offset_y + (main_btns_height/3) * i, _client_rect.w, 1, 0x8410u);
//      canvas->drawFastHLine(offset_x, offset_y + (main_btns_height/3) * i, _client_rect.w, TFT_DARKGRAY);
    }
    if (_no_data) {
      canvas->setTextSize(2, 2);
      canvas->drawString("No Data", offset_x + (_client_rect.w >> 1), y_degree);
      return;
    }
  }
};
static ui_progression_timeline_t ui_progression_timeline;

// タイムライン領域の上下2pxにステータスラインを描画するウィジェット
// スクロール再描画とは独立してモード変化時のみ更新する
struct ui_status_line_t : public ui_base_t
{
  // レコーディング: 赤+緑少し  0xF400 = R:11111 G:00100 B:00000
  // オートソング:   緑+青少し  0x07E3 = R:00000 G:11111 B:00011
  static constexpr uint16_t color_recording  = 0xF400;
  static constexpr uint16_t color_auto_song  = 0x07E3;

  def::gui_mode_t _prev_mode = def::gui_mode_t::gm_unknown;
  uint16_t _line_color = 0;

protected:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto mode = system_registry->runtime_info.getGuiMode();
    uint16_t new_color = 0;
    if (mode == def::gui_mode_t::gm_song_recording) {
      new_color = color_recording;
    } else if (mode == def::gui_mode_t::gm_song_play) {
      if (system_registry->runtime_info.getPlayMode() == def::playmode::pm_auto_song) {
        new_color = color_auto_song;
      }
    }
    if (_prev_mode != mode) {
      _prev_mode = mode;
      bool visible = (new_color != 0);
      auto r = getTargetRect();
      r.y = visible ? (disp_height - main_btns_height) : disp_height;
      r.h = visible ? main_btns_height : 0;
      setTargetRect(r);
    }
    if (_line_color != new_color) {
      _line_color = new_color;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override {
    if (_line_color == 0 || _client_rect.empty()) { return; }
    image_color_or(canvas, offset_x, offset_y,                              _client_rect.w, 2, _line_color);
    image_color_or(canvas, offset_x, offset_y + _client_rect.h - 2, _client_rect.w, 2, _line_color);
  }
};
static ui_status_line_t ui_status_line;

// ガイド表示中にステップ位置を右上に表示するウィジェット
struct ui_step_counter_t : public ui_base_t
{
  static constexpr int32_t w = 48;                      // ガイド表示1枠と同幅
  static constexpr int32_t h = main_btns_height / 3;    // パート・スロット領域と同高さ（32px）

  uint16_t _prev_pos = 0xFFFF;
  uint16_t _prev_len = 0xFFFF;
  def::gui_mode_t _prev_mode = def::gui_mode_t::gm_unknown;

protected:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto mode = system_registry->runtime_info.getGuiMode();
    bool visible = (mode == def::gui_mode_t::gm_song_play || mode == def::gui_mode_t::gm_song_recording);
    if (_prev_mode != mode) {
      _prev_mode = mode;
      auto r = getTargetRect();
      r.y = visible ? (disp_height - main_btns_height) : disp_height;
      setTargetRect(r);
    }
    if (!visible) { ui_base_t::update_impl(param, offset_x, offset_y); return; }

    auto pos = system_registry->runtime_info.getProgressionPosition();
    auto len = system_registry->current_progression->info.getLength();
    if (_prev_pos != pos || _prev_len != len) {
      _prev_pos = pos;
      _prev_len = len;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override {
    if (_client_rect.empty()) { return; }
    int cx = offset_x + (_client_rect.w >> 1);
    int cy = offset_y + (_client_rect.h >> 1);  // h=32 → cy = offset_y+16
    canvas->fillRect(offset_x, offset_y, _client_rect.w, _client_rect.h, TFT_DARKGRAY);
    canvas->drawRect(offset_x, offset_y, _client_rect.w, _client_rect.h, TFT_BLUE);
    // 中央の区切り線
    canvas->drawFastHLine(offset_x + 2, cy, _client_rect.w - 4, TFT_BLACK);

    canvas->setFont(&fonts::FreeSansBold9pt7b);
    canvas->setTextSize(1);
    canvas->setTextColor(TFT_BLACK);
    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);

    char buf[8];
    // 上半分（16px）：現在位置
    snprintf(buf, sizeof(buf), "%d", (int)_prev_pos + 1);
    canvas->drawString(buf, cx, offset_y + (_client_rect.h >> 2) + 2);
    // 下半分（16px）：総数
    snprintf(buf, sizeof(buf), "%d", (int)_prev_len);
    canvas->drawString(buf, cx, cy + (_client_rect.h >> 2) + 1);

    canvas->setFont(&fonts::efontJA_16_b);
    canvas->setTextSize(1);
    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
  }
};
static ui_step_counter_t ui_step_counter;
