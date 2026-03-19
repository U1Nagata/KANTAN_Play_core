// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct ui_partinfo_t : public ui_base_t
{
protected:
  uint32_t _backcolor;
  uint32_t _color_hit;
  int16_t _effect_remain = 0;
  registry_t::history_code_t _partinfo_history_code;
  registry_t::history_code_t _arpeggio_history_code;
  uint8_t _slot_index;
  uint8_t _hit_effect_index;
  bool _isEnabled; // パートが有効かどうか
  bool _isEmpty;   // ベロシティパターンが空欄かどうか
  bool _isDetailMode;
  bool _need_draw = true;

  // ハイライトに追従するフラグ
  bool _is_follow_highlight = true;

public:
  static constexpr const uint8_t icon_width = 64;
  static constexpr const uint8_t icon_height = 64;

  void update_inner(draw_param_t *param, int offset_x, int offset_y) {
    auto part = &system_registry->current_slot->chord_part[_part_index];
    auto partinfo = &part->part_info;
    // auto partinfo = &param->multipart->channel_info_list[part_index];

    // partinfoに変更があれば再描画対象とする
    auto partinfo_history_code = partinfo->getHistoryCode();
    auto arpeggio_history_code = part->arpeggio.getHistoryCode();
    auto slot_index = system_registry->runtime_info.getPlaySlot();
    bool flg_update = (_partinfo_history_code != partinfo_history_code
                    || _arpeggio_history_code != arpeggio_history_code
                   || _slot_index != slot_index);
    if (flg_update) {
      _partinfo_history_code = partinfo_history_code;
      _arpeggio_history_code = arpeggio_history_code;
      _slot_index = slot_index;
      _isEmpty = system_registry->current_slot->chord_part[_part_index].arpeggio.isEmpty();
    }

    int highlight_target = system_registry->chord_play.getPartStep(_part_index);
    if (highlight_target < 0) { highlight_target = 0; }
    highlight_target <<= 8;
    if (x_highlight_256 != highlight_target) {
      if (_isDetailMode) {
        auto before_rect = getHighlightRect(offset_x, offset_y);
        x_highlight_256 = smooth_move(highlight_target, x_highlight_256, param->smooth_step);
        auto after_rect = getHighlightRect(offset_x, offset_y);
        param->addInvalidatedRect(rect_or(before_rect, after_rect));
      } else {
        x_highlight_256 = highlight_target;
      }
      if (_is_follow_highlight) {
        int x256 = highlight_target < 0 ? 0 : highlight_target;
        if (x_scroll_target > x256) {
          changePage(getCurrentPage() - 1);
        } else
        if (x_scroll_target < x256 - (256 * 7))
        {
          changePage(getCurrentPage() + 1);
        }
      }
    }
    if (x_scroll_current != x_scroll_target) {
      if (_isDetailMode) {
        x_scroll_current = smooth_move(x_scroll_target, x_scroll_current, param->smooth_step);
        flg_update = true;
      } else {
        x_scroll_current = x_scroll_target;
      }
    }
    bool isEnabled = partinfo->getEnabled();
    if (_isEnabled != isEnabled) {
      _isEnabled = isEnabled;
      flg_update = true;
    }
    _backcolor = (isEnabled)
                ? system_registry->color_setting.getEnablePartColor()
                : system_registry->color_setting.getDisablePartColor();
    auto color = system_registry->color_setting.getArpeggioNoteForeColor();
    // auto color = param->color_set->arpeggio_note_fore;
    if (!isEnabled) { color = (color >> 1) & 0x7F7F7Fu; }

    auto effect_remain = _effect_remain;
    if (_isDetailMode) {
      effect_remain = 0;
    } else {
      auto hit_effect_index = system_registry->runtime_info.getPartEffect(_part_index);
      if (_hit_effect_index != hit_effect_index) {
        _hit_effect_index = hit_effect_index;
        if (effect_remain == 0) {
          flg_update = true;
        }
        effect_remain = 127;
      }
      if (effect_remain) {
        effect_remain -= param->smooth_step;
        if (effect_remain <= 0) {
          effect_remain = 0;
          flg_update = true;
        }
      }
      if (flg_update) {
        color = add_color(color, (effect_remain * 0x010100u) | (uint8_t)(-effect_remain));
      }
    }
    _effect_remain = effect_remain;
    _color_hit = color;

    if (flg_update) {
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }

  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    if (_parent->getClientRect().empty()) { return; }
    if (_need_draw == false) { return; }
    if (system_registry->runtime_info.getGuiMode() != def::gui_mode_t::gm_part_edit) {
      _isDetailMode = system_registry->user_setting.getGuiDetailMode();
      update_inner(param, offset_x, offset_y);
    }
  }

  void fill_background(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect)
  {
    auto partinfo = &system_registry->current_slot->chord_part[_part_index].part_info;
    // auto partinfo = &param->multipart->channel_info_list[part_index];
    bool isEnabled = _isEnabled;
    canvas->setColor(_backcolor);
    canvas->fillRect(offset_x+1, offset_y+1, _client_rect.w-2, _client_rect.h-2);
    canvas->setColor(isEnabled ? 0x88AACCu : 0x224466u);
    canvas->writeFastHLine(offset_x, offset_y  , _client_rect.w-1);
    canvas->writeFastVLine(offset_x, offset_y+1, _client_rect.h-2);

    canvas->setColor(0x000033u);
    canvas->writeFastHLine(offset_x, offset_y+_client_rect.h-1, _client_rect.w-1);
    canvas->writeFastVLine(offset_x+_client_rect.w-1, offset_y+1, _client_rect.h-2);

    int y = offset_y + getY((7 << 8) + 128)-2;
    if (y + 16 > clip_rect->top() && y < clip_rect->bottom())
    {/// 楽器名の表示
      auto name = def::midi::program_name_table.at(partinfo->getTone())->get();
      // auto name = draw_param_t::program_name_tbl[partinfo->program_number];
      // canvas->setFont(&fonts::efontJA_16_b);
      float fx = _client_rect.w / 232.0f;
      if (fx < 0.5f) { fx = 0.5f; }
      float fy = _client_rect.h / 152.0f;
      if (fy < 1.0f) { fy = 1.0f; }
      canvas->setTextSize(fx, fy);
      canvas->setTextDatum(m5gfx::textdatum_t::top_center);
      canvas->setTextColor(isEnabled ? 0xFFFFFFu : 0x7F7F7Fu);
      canvas->drawString(name, offset_x + 1 + (_client_rect.w >> 1), y);
    }
  }

  void draw_easymode(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect)
  {
    auto partinfo = &system_registry->current_slot->chord_part[_part_index].part_info;
    // auto partinfo = &param->multipart->channel_info_list[part_index];
    int x = offset_x + ((_client_rect.w - 64) >> 1);
    int y = offset_y + ((_client_rect.h - 64) >> 1) - getY(128);
    if (_isEmpty) {
      // パートの中身が空の場合はグレー塗りにしてパートが存在しないように見せる
      canvas->fillRect(offset_x + 1, offset_y + 1, _client_rect.w - 2, _client_rect.h - 2, 0x808080u);
      return;
    }
    if ((clip_rect->right() < x) || (clip_rect->left()  > (x + icon_width)))
    { return; }
    canvas->pushGrayscaleImage( x
                      , y
                      , icon_width
                      , icon_height
                      , &resource_icon_instrument_64x64x36[resource_program_icon_table[partinfo->getTone()] * (icon_width * icon_height)]
                      , m5gfx::color_depth_t::grayscale_8bit
                      , _color_hit
                      , _backcolor
                      );
  }

  rect_t getHighlightRect(int offset_x, int offset_y)
  {
    int x = getX(x_highlight_256);
    int w = getX(x_highlight_256 + 256) - x;
    int y = getY(-128);
    int h = getY((7 << 8) + 128) - y;
    x -= w >> 1;
    return { x + offset_x, y + offset_y, w, h };
  }

  void draw_normalmode(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect)
  {
    auto partinfo = &system_registry->current_slot->chord_part[_part_index].part_info;
    // auto partinfo = &param->multipart->channel_info_list[part_index];

    bool clear_confirm = system_registry->chord_play.getConfirm_AllClear();

    bool isEnabled = partinfo->getEnabled();

    int r = (std::min(_client_rect.w , _client_rect.h) + 12) / 24;
    { // 背景ドット描画
      canvas->setColor(system_registry->color_setting.getArpeggioNoteBackColor());
      // canvas->setColor(param->color_set->arpeggio_note_back);
      const int dr = r >> 1;
      for (int j = 0; j < def::app::max_arpeggio_step; j += 2)
      // for (int j = 0; j < arpeggio_step_number_max; j += 2)
      {
        int x = offset_x + getX(j << 8);
        if (x+dr < clip_rect->left()) { continue; }
        if (x-dr > clip_rect->right()) { break; }
        for (int i = 7; i > 0; --i) {
          int y = offset_y + getY(i << 8);
          if (y-dr > clip_rect->bottom()) { continue; }
          if (y+dr < clip_rect->top()) { break; }
          canvas->fillCircle(x, y, dr);
        }
      }
    }

    {
      auto color = system_registry->color_setting.getArpeggioStepColor();
      if (!isEnabled) { color = (color >> 1) & 0x7F7F7Fu; }
      auto rect = getHighlightRect(offset_x, offset_y);
      canvas->fillRect(rect.x, rect.y, rect.w, rect.h, color);
    }

    // canvas->setFont(&fonts::TomThumb);
    // canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
    // canvas->setTextSize(1, 1);
    int j = getIndexByX(clip_rect->x - r - offset_x + _client_rect.x);
    int je = getIndexByX(clip_rect->right() + r - offset_x + _client_rect.x);
    if (j < 0) { j = 0; }
    if (je >= def::app::max_arpeggio_step) { je = def::app::max_arpeggio_step - 1; }
    for (; j <= je; ++j) {
      int x = getX(j << 8) + offset_x;
      if (x + r < clip_rect->left()) { continue; }
      if (x - r > clip_rect->right()) { break; }

      {
        int y = offset_y + getY(0);
        int xt = x + 1;
        if (j == partinfo->getAnchorStep()) {
          bool last = false;
          for (int yp = 0; !last; ++yp) {
            last = (yp == (r << 1) + 1);
            canvas->setColor(0x000080u);
            int w = r + (yp >> 2);
            if (r > 5) {
              canvas->drawPixel(xt - w-1, y-r+yp, TFT_LIGHTGRAY);
              canvas->drawPixel(xt + w-1, y-r+yp, TFT_LIGHTGRAY);
              if (yp != 0 && !last) {
                canvas->setColor(0x000080u);
              }
            }
            canvas->fillRect(xt -w, y-r+yp, (w << 1) - 1, 1);
          }
        }
        if (j == partinfo->getLoopStep()) {
          canvas->fillTriangle(xt-(r*3>>1), y, xt+r, y-r, xt+r,y+r, 0x800000u);
          if (r > 5)
          canvas->drawTriangle(xt-(r*3>>1), y, xt+r, y-r, xt+r,y+r, TFT_LIGHTGRAY);
        }
        // 列番号の描画
        // canvas->drawNumber(j+1, x+4, y+1);
      }

      bool highlight = (j == system_registry->chord_play.getPartStep(_part_index));
      // bool highlight = (j == partinfo->current_step);
      int lighting = (isEnabled ? 1 : 0) + (highlight ? 0 : -1);

      if (!partinfo->isDrumPart())
      {
        int y = offset_y + getY(7 << 8);
        auto style = system_registry->current_slot->chord_part[_part_index].arpeggio.getStyle(j);
        // auto style = partinfo->arpeggio_pattern.step[j].arpeggio_style;
        static constexpr const uint32_t style_color_tbl[] = {
          0x339933u,  // arpeggio_pattern_t::arpeggio_style_t::same_time
          0xFF6666u,  // arpeggio_pattern_t::arpeggio_style_t::low_to_high
          0x6666FFu,  // arpeggio_pattern_t::arpeggio_style_t::high_to_low
          0x999999u,  // arpeggio_pattern_t::arpeggio_style_t::mute
          0x111111u,  // arpeggio_pattern_t::arpeggio_style_t::return_to_start
        };
        uint32_t color = 0;
        if (style < (sizeof(style_color_tbl) / sizeof(uint32_t))) {
          color = style_color_tbl[style];
        }
        if (j > partinfo->getLoopStep()) { --lighting; }
        if (lighting) {
          uint32_t add = (lighting < 0) ? 0xE0E0E0u : 0x202020u;
          color = add_color(color, add);
        }

        switch (style) {
        case def::play::arpeggio_style_t::high_to_low:
          canvas->fillTriangle(x, y+r, x-r, y-r, x+r,y-r, color);
          canvas->drawTriangle(x, y+r, x-r, y-r, x+r,y-r, TFT_LIGHTGRAY);
          break;
        case def::play::arpeggio_style_t::low_to_high:
          canvas->fillTriangle(x, y-r, x-r, y+r, x+r,y+r, color);
          canvas->drawTriangle(x, y-r, x-r, y+r, x+r,y+r, TFT_LIGHTGRAY);
          break;
        case def::play::arpeggio_style_t::mute:
          canvas->fillRect(x-(r), y-(r), r<<1, r<<1, color);
          canvas->drawRect(x-(r), y-(r), r<<1, r<<1, TFT_LIGHTGRAY);
          break;
        case def::play::arpeggio_style_t::same_time:
          canvas->fillCircle(x, y, r>>1, color);
          canvas->drawCircle(x, y, r>>1, TFT_LIGHTGRAY);
          break;
        default: break;
        }
      }
      uint32_t fore_color = system_registry->color_setting.getArpeggioNoteForeColor();
      uint32_t back_color = system_registry->color_setting.getArpeggioNoteBackColor();
      // uint32_t fore_color = param->color_set->arpeggio_note_fore;
      // uint32_t back_color = param->color_set->arpeggio_note_back;
      if (lighting) {
        uint32_t add = (lighting < 0) ? 0xE0E0E0u : 0x202020u;
        fore_color = add_color(fore_color, add);
        back_color = add_color(back_color, add);
      }
      // auto step = &(partinfo->arpeggio_pattern.step[j]);
      canvas->setColor(fore_color);
      for (int i = 5 + partinfo->isDrumPart(); i >= 0; --i) {
        int y = offset_y + getY((i + 1) << 8);

        if (y - r > clip_rect->bottom()) { continue; }
        if (y + r < clip_rect->top()) { break; }

        auto v = system_registry->current_slot->chord_part[_part_index].arpeggio.getVelocity(j, i);
        if (v) {
          if (v < 0) {
            if (!clear_confirm) {
              canvas->fillRect(x - (r >> 1), y - (r >> 1), r, r, 0x999999u);
            }
            canvas->drawRect(x - (r >> 1), y - (r >> 1), r, r, fore_color);
          } else {
            if (clear_confirm) {
              canvas->drawArc(x, y, r, 0, 90, 90 + v * 36 / 10, fore_color);
            } else {
              canvas->fillArc(x, y, r, 0, 90, 90 + v * 36 / 10, fore_color);
            }
          }
        }
      }
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    if (!_need_draw) { return; }
    fill_background(param, canvas, offset_x, offset_y, clip_rect);

    if (_isDetailMode) {
      draw_normalmode(param, canvas, offset_x, offset_y, clip_rect);
    } else {
      draw_easymode(param, canvas, offset_x, offset_y, clip_rect);
    }
  }
  void changePage(int pageindex) {
    pageindex = (pageindex < 0) ? 0 : (pageindex > ((def::app::max_arpeggio_step >> 3)-1) ) ? ((def::app::max_arpeggio_step >> 3)-1) : pageindex;
    // pageindex = (pageindex < 0) ? 0 : (pageindex > ((arpeggio_step_number_max>>3)-1) ) ? ((arpeggio_step_number_max>>3)-1) : pageindex;
    x_scroll_target = pageindex * 256 * 8;
  }
  int getCurrentPage(void) const {
    return x_scroll_target >> 11;
  }
  int getX(int x256) {
    return ((x256 - x_scroll_current + 128) * _client_rect.w) / (256 * 8);
  }
  int getY(int y256) {
    return ((y256 + 128) * _client_rect.h) / (256 * 10);
  }
  int getIndexByX(int x) {
    x -= _client_rect.x;
    return (((x * 256 * 8) / _client_rect.w) + x_scroll_current) >> 8;
  }
  int getIndexByY(int y) {
    y -= _client_rect.y;
    return ((y * 10) / (_client_rect.h));
  }

  int16_t x_highlight_256 = -16;
  int16_t x_scroll_current = 0;
  int16_t x_scroll_target = 0;
  void setPartIndex(uint8_t part_index) { _part_index = part_index; }
  void setNeedDraw(bool need_draw) { _need_draw = need_draw; }
protected:
  uint8_t _part_index;
};
static ui_partinfo_t ui_partinfo_list[def::app::max_chord_part];

static void setPartInfoNeedDraw(bool need_draw) {
M5_LOGD("setPartInfoNeedDraw(%d)\n", need_draw);
  for (int i = 0; i < def::app::max_chord_part; ++i) {
    ui_partinfo_list[i].setNeedDraw(need_draw);
  }
}

struct ui_arpeggio_edit_t : public ui_partinfo_t
{
protected:
  int16_t _cursor_target_x_256 = 0;
  int16_t _cursor_target_y_256 = 0;
  int16_t _cursor_current_x_256 = 0;
  int16_t _cursor_current_y_256 = 0;
  uint8_t _cursor_x = 0x80;
  uint8_t _cursor_y = 0x80;
  int16_t _cursor_display_x = 0;
  int16_t _cursor_display_y = 0;

  def::command::command_param_t _command_param[4];
  def::command::edit_enc2_target_t _enc2_target;

  uint8_t _part_volume = 0;
  uint8_t _program = 0;
  uint8_t _end_point = -1;
  uint8_t _anchor_step = -1;
  uint8_t _displacement = -1;
  int8_t _position = 0;
  KANTANMusic_Voicing _voicing = KANTANMusic_Voicing::KANTANMusic_MAX_VOICING;
  const char* _voicing_name = "";
  int _edit_velocity = 0;
  bool _is_enabled = false;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    if (_parent->getClientRect().empty()) { return; }
    bool visible = !_client_rect.empty();
    bool moving = _client_rect != _target_rect;

    ui_base_t::update_impl(param, offset_x, offset_y);
    update_inner(param, offset_x, offset_y);
    bool flg_update = false;

    const bool is_partedit = system_registry->runtime_info.getGuiFlag_PartEdit();

    auto target_index = system_registry->chord_play.getEditTargetPart();

    if (!moving) {
      if (!visible && is_partedit) {
        _isDetailMode = true;
        _is_follow_highlight = true;
        _part_index = target_index;
        flg_update = true;
        _is_enabled = true;
        setClientRect(ui_partinfo_list[target_index].getClientRect());
        setTargetRect({0, 0, main_area_width, main_area_height});
        _cursor_x = 0x80;
        _cursor_y = 0x80;
      } else
      if (visible && (!is_partedit || _part_index != target_index)) {
        flg_update = true;
        _is_enabled = false;
        setTargetRect(ui_partinfo_list[_part_index].getTargetRect());
        setPartInfoNeedDraw(true);
      }
    }

    if (visible && moving && getClientRect() == getTargetRect()) {
      setPartInfoNeedDraw(!_is_enabled);
      if (!_is_enabled) {
        setTargetRect({0, 0, 0, 0});
        setClientRect({0, 0, 0, 0});
      }
    }

  
    auto part = &system_registry->current_slot->chord_part[_part_index];
    auto part_info = &part->part_info;

    if (!_target_rect.empty()) {
      // サブボタン群の表示更新があるか確認
      bool subbutton_update = false;
      auto enc2_target = (def::command::edit_enc2_target_t)(system_registry->chord_play.getEditEnc2Target());
      if (_enc2_target != enc2_target) {
        _enc2_target = enc2_target;
        subbutton_update = true;
      }
      for (int i = 0; i < def::hw::max_sub_button; ++i) {
        auto sub_button_index = i;
        auto pair0 = system_registry->sub_button.getCommandParamArray(sub_button_index);
        auto pair1 = system_registry->sub_button.getCommandParamArray(sub_button_index + def::hw::max_sub_button);
        auto command_param_0 = pair0.array[0].getParam();
        auto command_param_1 = pair1.array[0].getParam();

        if (enc2_target == command_param_0) {
          sub_button_index = i;
        } else
        if (enc2_target == command_param_1) {
          sub_button_index = i + def::hw::max_sub_button;
        } else {
          // サブボタンのスワップを考慮
          bool sub_button_swap = system_registry->runtime_info.getSubButtonSwap();
          if (sub_button_swap) {
            sub_button_index = i + def::hw::max_sub_button;
          }
        }
        auto pair = system_registry->sub_button.getCommandParamArray(sub_button_index);
        auto command_param = pair.array[0];
        if (_command_param[i] != command_param) {
          _command_param[i] = command_param;
          subbutton_update = true;
        }
      }
      uint8_t part_volume = part_info->getVolume();
      if (_part_volume != part_volume) {
        _part_volume = part_volume;
        subbutton_update = true;
      }
      uint8_t program = part_info->getTone();
      if (_program != program) {
        _program = program;
        subbutton_update = true;
      }
      int8_t position = part_info->getPosition();
      if (_position != position) {
        _position = position;
        subbutton_update = true;
      }
      auto voicing = part_info->getVoicing();
      if (_voicing != voicing) {
        _voicing = voicing;
        _voicing_name = def::play::GetVoicingName(voicing);
        subbutton_update = true;
      }
      int velo = system_registry->runtime_info.getEditVelocity();
      if (_edit_velocity != velo) {
        _edit_velocity = velo;
        subbutton_update = true;
      }
      uint8_t end_point = part_info->getLoopStep();
      if (_end_point != end_point) {
        _end_point = end_point;
        subbutton_update = true;
      }
      uint8_t anchor_step = part_info->getAnchorStep();
      if (_anchor_step != anchor_step) {
        _anchor_step = anchor_step;
        subbutton_update = true;
      }
      uint8_t displacement = part_info->getStrokeSpeed();
      if (_displacement != displacement) {
        _displacement = displacement;
        subbutton_update = true;
      }
      if (subbutton_update) {
        int top = getY(128 + (7 << 8));
        int bottom = getY(128 + (9 << 8));
        int h = bottom - top;
        int y = offset_y + top;
        int x = offset_x;
        int w = _client_rect.w;
        param->addInvalidatedRect({x, y, w, h});
      }

      bool cursor_update = (param->prev_msec >> 8) != (param->current_msec >> 8);
      if (cursor_update) { // TODO:暫定処置。これを無くしても良い状態に差分更新を実装したい
        param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
      }

      int cx = system_registry->chord_play.getPartStep(_part_index);
      if (cx < 0) { cx = 0; }
      auto cy = system_registry->chord_play.getCursorY() + 1;
      changePage(cx >> 3);
      if (_cursor_x != cx || _cursor_y != cy || (getClientRect() != getTargetRect())) {
        _cursor_x = cx;
        _cursor_y = cy;
        _cursor_target_x_256 = cx << 8;
        _cursor_target_y_256 = cy << 8;
        flg_update = true;
      }
      if (_cursor_current_x_256 != _cursor_target_x_256
       || _cursor_current_y_256 != _cursor_target_y_256) {
        _cursor_current_x_256 = smooth_move(_cursor_target_x_256, _cursor_current_x_256, param->smooth_step);
        _cursor_current_y_256 = smooth_move(_cursor_target_y_256, _cursor_current_y_256, param->smooth_step);
      }
      int new_display_x = getX(_cursor_current_x_256);
      int new_display_y = getY(_cursor_current_y_256);
      if (_cursor_display_x != new_display_x || _cursor_display_y != new_display_y) {
        param->addInvalidatedRect({_cursor_display_x + offset_x - 24, _cursor_display_y + offset_y - 24, 48, 48});
        _cursor_display_x = new_display_x;
        _cursor_display_y = new_display_y;
        cursor_update = true;
      }
      if (cursor_update) {
        param->addInvalidatedRect({_cursor_display_x + offset_x - 24, _cursor_display_y + offset_y - 24, 48, 48});
        // flg_update = true;
      }
    }
    if (flg_update) {
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }
  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    fill_background(param, canvas, offset_x, offset_y, clip_rect);
    draw_normalmode(param, canvas, offset_x, offset_y, clip_rect);

    canvas->setTextColor(0xFFFFFFu);
    canvas->setTextSize(1);
    canvas->setTextDatum(m5gfx::textdatum_t::middle_center);
    for (int j = 0; j < def::app::max_arpeggio_step; j += 2) {
      int x = getX(j << 8);
      if (x < 0) { continue; }
      if (x > _client_rect.w) { break; }
      x += offset_x + 4;
      int y = offset_y + getY(0);
      canvas->drawNumber((j>>1)+1, x, y);
    }

    const int r = (std::min(_client_rect.w , _client_rect.h) + 12) / 24;

    bool invert = 1 & (param->current_msec >> 8);
    {
      // int r = getX(128) - getX(0);
      int x = _cursor_display_x + offset_x;
      int y = _cursor_display_y + offset_y;
      // カーソルを描画
      canvas->drawCircle(x, y, r + 5, invert ? TFT_YELLOW : TFT_RED);
      canvas->drawCircle(x, y, r + 7, invert ? TFT_RED : TFT_YELLOW);
    }

    if (system_registry->chord_play.getConfirm_Paste())
    { // コピー/ペースト(クリップボード)のパターンを描画
      auto part = &system_registry->current_slot->chord_part[_part_index];
      auto part_info = &part->part_info;

      int range_w = system_registry->chord_play.getRangeWidth();
      for (int j = 0; j < range_w; ++j) {
        int step = _cursor_x + j;
        int x = getX(step << 8) + offset_x;
        if (x + r < 0) { continue; }
        if (x - r > _client_rect.w) { break; }

        canvas->setColor(TFT_YELLOW);
        for (int i = 5 + part_info->isDrumPart(); i >= 0; --i) {
          auto v = system_registry->clipboard_arpeggio.getVelocity(j, i);
          if (v) {
            int y = offset_y + getY((i + 1) << 8);
            if (v < 0) {
              canvas->drawRect(x - (r >> 1), y - (r >> 1), r, r);
            } else {
              canvas->drawArc(x, y, r, 0, 90, 90 + v * 36 / 10);
            }
          }
        }
      }
    }

    const int y = offset_y + getY(9 << 8);
    if (y + 8 > clip_rect->top() && y - 8 < clip_rect->bottom()) {
      const uint32_t fore_color = 0xFFFF44u;
      const uint32_t unfocus_color = 0xCCDDEEu;

      canvas->setTextSize(1);
      for (int i = 0; i < def::hw::max_sub_button; ++i)
      { // サブボタン機能の描画
        auto command_param = _command_param[i];
        auto param = command_param.getParam();

        uint32_t color = (param == _enc2_target) ? fore_color : unfocus_color;
        canvas->setColor(color);
        canvas->setTextColor(color);
        canvas->setTextDatum(textdatum_t::middle_center);

        // int x = offset_x + getX(x_scroll_current + ((i*2) << 8));
        int left = (i * _client_rect.w) >> 2;
        int right = ((i+1) * _client_rect.w) >> 2;
        int x = offset_x + left;
        int w = right - left;
        int x1 = x + (w>>2);
        int x2 = x + (w>>1);
        int x3 = x + (w*3>>2);

        switch (param) {
        case def::command::edit_enc2_target_t::part_vol:
          {
            int v = _part_volume;
            canvas->fillArc(x1, y, r, 0, 90, 90 + v * 36 / 10);
            canvas->drawNumber(v, x3, y);
          }
          break;

        case def::command::edit_enc2_target_t::velocity:
          {
            int v = _edit_velocity;
            if (v < 0) {
              canvas->fillRect(x1 - (r >> 1), y - (r >> 1), r, r, color);
              canvas->drawString("mute", x3, y);
            } else {
              canvas->fillArc(x1, y, r, 0, 90, 90 + v * 36 / 10, color);
              canvas->drawNumber(v, x3, y);
            }
          }
          break;

        case def::command::edit_enc2_target_t::program:
          {
            // プログラムナンバーは 1~128 として表示する
            int p = _program + 1;
            canvas->drawNumber(p, x2, y);
          }
          break;

        case def::command::edit_enc2_target_t::endpoint:
          {
            int p = (_end_point>>1) + 1;
            canvas->drawNumber(p, x2, y);
          }
          break;

        case def::command::edit_enc2_target_t::banlift:
          {
            int p = (_anchor_step>>1) + 1;
            canvas->drawNumber(p, x2, y);
          }
          break;

        case def::command::edit_enc2_target_t::displacement:
          {
            int p = _displacement;
            canvas->drawNumber(p, x2, y);
          }
          break;

        case def::command::edit_enc2_target_t::position:
          {
            int pos = _position + def::app::position_table_offset;
            canvas->drawString(def::app::position_name_table.at(pos)->get(), x2, y);
          }
          break;

        case def::command::edit_enc2_target_t::voicing:
          {
            canvas->drawString(_voicing_name, x2, y);
          }
          break;

        default: break;

        }
      }
    }
  }
};
static ui_arpeggio_edit_t ui_arpeggio_edit;