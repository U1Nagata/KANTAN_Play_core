// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct ui_volume_info_t : public ui_base_t
{
protected:
    uint8_t _volume = 0;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    auto volume = system_registry->user_setting.getMasterVolume();
    if (_volume != volume) {
      _volume = volume;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    int r = 10;
    int x = offset_x + _client_rect.w - 12;
    {
      int y = offset_y + 11;
      int v = _volume * 36 / 10;
      canvas->fillCircle(x, y, r, 0x303030u);
      canvas->drawCircle(x, y, r, TFT_LIGHTGRAY);
      canvas->setColor(TFT_WHITE);
      canvas->fillArc(x, y, r, 0, 90, 90 + v);
    }
  }
};
static ui_volume_info_t ui_volume_info;

struct ui_battery_info_t : public ui_base_t
{
protected:
    uint8_t _battery = 0;
    bool _is_charging = false;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    auto battery = system_registry->runtime_info.getBatteryLevel();
    auto is_charging = system_registry->runtime_info.getBatteryCharging();

    if (_is_charging != is_charging || _battery != battery) {
      _is_charging = is_charging;
      _battery = battery;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  { // バッテリーアイコン
    int x = offset_x;
    int w = _client_rect.w;

    int y = offset_y + 2;
    int h = _client_rect.h - 4;
    canvas->fillRect(x + (w>>2), y, w - (w>>1), -2, TFT_WHITE);
    canvas->drawRect(x, y, w, h, TFT_WHITE);
    x += 2;
    w -= 4;
    y += 2;
    h -= 4;
    int v = h * _battery / 100;
    canvas->fillRect(x, y + h - v, w, v, _is_charging ? TFT_GREEN : TFT_WHITE);
  }
};
static ui_battery_info_t ui_battery_info;

struct ui_wifi_sta_info_t : public ui_base_t
{
protected:
    def::command::wifi_sta_info_t _wifi_status = (def::command::wifi_sta_info_t)0xFF;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto wifi_status = system_registry->runtime_info.getWiFiSTAInfo();

    if (_wifi_status != wifi_status) {
      _wifi_status = wifi_status;
      int w = (wifi_status == def::command::wifi_sta_info_t::wsi_off)
            ? 0 : 23;
      if (w != _target_rect.w) {
        _target_rect.x -= w - _target_rect.w;
        _target_rect.w = w;
      }
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  { // WiFi STAアイコン
// canvas->fillScreen(rand());
// canvas->fillRect(offset_x, offset_y, _client_rect.w, _client_rect.h, rand());
    int level = -1;
    switch (_wifi_status) {
    case def::command::wifi_sta_info_t::wsi_off:
      level = 0;
      break;
    case def::command::wifi_sta_info_t::wsi_waiting:
      level = 1;
      break;
    case def::command::wifi_sta_info_t::wsi_signal_1:
      level = 2;
      break;
    case def::command::wifi_sta_info_t::wsi_signal_2:
      level = 3;
      break;
    case def::command::wifi_sta_info_t::wsi_signal_3:
      level = 4;
      break;
    case def::command::wifi_sta_info_t::wsi_signal_4:
      level = 5;
      break;
    default:
      break;
    }

    int x = offset_x;
    int w = _client_rect.w;

    int y = offset_y;
    int h = _client_rect.h;

    int xc = x + (w >> 1);
    int yc = y + h - 5;
    canvas->fillCircle(xc, yc, 2, level > 0 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc,  7,  6, 230, 310, level > 1 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc, 11, 10, 230, 310, level > 2 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc, 15, 14, 230, 310, level > 3 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc, 19, 18, 230, 310, level > 4 ? 0xFFFFFFu : 0x3F3F3Fu);
    if (level < 0) {
      canvas->drawLine(offset_x, offset_y, offset_x+w, offset_y+h, TFT_RED);
      canvas->drawLine(offset_x, offset_y+h, offset_x+w, offset_y, TFT_RED);
    }
/*
    canvas->setTextSize(1);
    canvas->setTextDatum(top_left);
    canvas->setTextColor(TFT_WHITE);
    canvas->drawNumber((uint8_t)_wifi_status, x, y);
*/
  }
};
static ui_wifi_sta_info_t ui_wifi_sta_info;

struct ui_wifi_ap_info_t : public ui_base_t
{
protected:
    def::command::wifi_ap_info_t _wifi_status = (def::command::wifi_ap_info_t)0xFF;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto wifi_status = system_registry->runtime_info.getWiFiAPInfo();
    if (_wifi_status != wifi_status) {
      _wifi_status = wifi_status;
      int w = (wifi_status == def::command::wifi_ap_info_t::wai_off)
            ? 0 : 23;
      if (w != _target_rect.w) {
        _target_rect.x -= w - _target_rect.w;
        _target_rect.w = w;
      }
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  { // WiFi APアイコン
    int level = -1;
    switch (_wifi_status) {
    case def::command::wifi_ap_info_t::wai_off:
      level = 0;
      break;
    case def::command::wifi_ap_info_t::wai_waiting:
      level = 1;
      break;
    case def::command::wifi_ap_info_t::wai_enabled:
      level = 4;
      break;
    default:
      break;
    }

    int x = offset_x;
    int w = _client_rect.w;

    int y = offset_y;
    int h = _client_rect.h;

    int xc = x + (w >> 1);
    int yc = y + (h >> 1) - 2;
    canvas->fillCircle(xc, yc, 2, level > 0 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillRoundRect(xc - 2, yc + 4, 5, 8, 2, level > 0 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc,  5,  6, 150, 390, level > 1 ? 0xFFFFFFu : 0x3F3F3Fu);
    canvas->fillArc(xc, yc,  9, 10, 130, 410, level > 2 ? 0xFFFFFFu : 0x3F3F3Fu);
    if (level < 0) {
      canvas->drawLine(offset_x, offset_y, offset_x+w, offset_y+h, TFT_RED);
      canvas->drawLine(offset_x, offset_y+h, offset_x+w, offset_y, TFT_RED);
    }
/*
    canvas->setTextSize(1);
    canvas->setTextDatum(top_left);
    canvas->setTextColor(TFT_WHITE);
    canvas->drawNumber((uint8_t)_wifi_status, x, y);
*/
  }
};
static ui_wifi_ap_info_t ui_wifi_ap_info;

struct ui_midiport_info_t : public ui_base_t
{
protected:
    static constexpr uint32_t images[3][5] = {
   // { 0b11111110000000001111111100000000
      { __builtin_bswap32(0b11110000000000000000011111000000)
      , __builtin_bswap32(0b11010000000000001000110001100000)
      , __builtin_bswap32(0b11110111101111011110110000000000)
      , __builtin_bswap32(0b10000100101000001000110001100000)
      , __builtin_bswap32(0b10000111101000001110011111000000)
      },
      { __builtin_bswap32(0b01111110001100000001111111000000)
      , __builtin_bswap32(0b01100011001100000001100000000000)
      , __builtin_bswap32(0b01111110001100000001111110000000)
      , __builtin_bswap32(0b01100011001100000001100000000000)
      , __builtin_bswap32(0b01111110001111111001111111000000)
      },
      { __builtin_bswap32(0b01100011000111111001111110000000)
      , __builtin_bswap32(0b01100011001100000001100011000000)
      , __builtin_bswap32(0b01100011000111110001111110000000)
      , __builtin_bswap32(0b01100011000000011001100011000000)
      , __builtin_bswap32(0b00111110001111110001111110000000)
      },
    };
    uint32_t _color[3] = { 0, 0, 0 };
    uint8_t _tx_couunt[3] = { 0, 0, 0 };
    uint8_t _rx_couunt[3] = { 0, 0, 0 };
    uint16_t _tx_dots[3];
    uint16_t _rx_dots[3];
    bool _visible = false;

public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    bool updated = false;

    def::command::midiport_info_t info[3];
    info[0] = system_registry->runtime_info.getMidiPortStatePC();
    info[1] = system_registry->runtime_info.getMidiPortStateBLE();
    info[2] = system_registry->runtime_info.getMidiPortStateUSB();

    bool visible = false;
    for (int i = 0; i < 3; ++i) {
      uint32_t color = 0x303030u;
      if (info[i] != def::command::midiport_info_t::mp_off) {
        visible = true;
        switch (info[i]) {
        case def::command::midiport_info_t::mp_enabled:
          color = 0x606060u; break;
        case def::command::midiport_info_t::mp_connected:
          color = 0xFFFFFFu; break;
        case def::command::midiport_info_t::mp_connecting:
          color = ((param->current_msec >> 8)&1) ? 0xFFFFFFu : 0x606060u; break;
        default: break;
        }
      }
      if (_color[i] != color) {
        _color[i] = color;
        updated = true;
      }
    }
    if (_visible != visible) {
      _visible = visible;
      int w = (visible)
            ? 26 : 0;
      if (w != _target_rect.w) {
        _target_rect.x -= w - _target_rect.w;
        _target_rect.w = w;
      }
    }
    uint8_t tx_couunt[3] = {
      system_registry->runtime_info.getMidiTxCountPC(),
      system_registry->runtime_info.getMidiTxCountBLE(),
      system_registry->runtime_info.getMidiTxCountUSB(),
    };
    uint8_t rx_couunt[3] = {
      system_registry->runtime_info.getMidiRxCountPC(),
      system_registry->runtime_info.getMidiRxCountBLE(),
      system_registry->runtime_info.getMidiRxCountUSB(),
    };

    // 通信カウンタのドット移動増分を求める
    uint8_t shift = (param->current_msec >> 4) - (param->prev_msec >> 4);
    if (shift) {
      for (int i = 0; i < 3; ++i) {
        auto tx = _tx_dots[i];
        if (tx) {
          _tx_dots[i] = tx << shift;
          updated = true;
        }
        auto rx = _rx_dots[i];
        if (rx) {
          _rx_dots[i] = rx << shift;
          updated = true;
        }
      }
    }
    for (int i = 0; i < 3; ++i) {
      if (_tx_couunt[i] != tx_couunt[i]) {
        _tx_couunt[i] = tx_couunt[i];
        _tx_dots[i] |= 1;
        updated = true;
      }
      if (_rx_couunt[i] != rx_couunt[i]) {
        _rx_couunt[i] = rx_couunt[i];
        _rx_dots[i] |= 1;
        updated = true;
      }
    }
    if (updated) {
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    int x = offset_x;
    int w = _client_rect.w;
    for (int i = 0; i < 3; ++i) {
      int y = offset_y + (i * 8);
      canvas->drawBitmap(x, y+2, (const uint8_t*)images[i], 32, 5, _color[i]);
      canvas->setColor(0xFFFF00u);
      for (int j = 0; j < 13; ++j) {
        if (_tx_dots[i] & (1 << j)) {
          canvas->writePixel(x +     (12 - j), y);
          canvas->writePixel(x +w-1- (12 - j), y);
        }
        if (_rx_dots[i] & (1 << j)) {
          canvas->writePixel(x +     j, y + 1);
          canvas->writePixel(x +w-1- j, y + 1);
        }
      }
    }
  }
};
static ui_midiport_info_t ui_midiport_info;

struct ui_icon_auto_play_t : public ui_base_t
{
protected:
  def::play::auto_play_state_t _autoplay_state;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto mode = system_registry->runtime_info.getGuiAutoplayState();
    if (_autoplay_state != mode) {
      _autoplay_state = mode;
      if (mode == def::play::auto_play_state_t::auto_play_none) {
        _target_rect.w = 0;
      } else {
        _target_rect.w = 11;
      }
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  { // オートプレイアイコン
    int x = offset_x;
    int w = _client_rect.w;

    int y = offset_y;
    int h = _client_rect.h;

    x = x + (w >> 1);
    y = y + (h >> 1);
    switch (_autoplay_state) {
    default:
      canvas->fillTriangle(x - 5, y - 5, x - 5, y + 5, x + 5, y, TFT_GREEN);
      break;
    case def::play::auto_play_state_t::auto_play_none:
    case def::play::auto_play_state_t::auto_play_waiting:
      canvas->fillRect(x - 5, y - 5, 11, 11, TFT_DARKGRAY);
      break;
    case def::play::auto_play_state_t::auto_play_paused:
      canvas->fillRect(x - 5, y - 5,  3, 11, TFT_DARKGRAY);
      canvas->fillRect(x + 5, y - 5, -3, 11, TFT_DARKGRAY);
      break;
    case def::play::auto_play_state_t::auto_play_beatmode:
      canvas->drawFastHLine(x - 4, y,  2, TFT_YELLOW);
      canvas->drawFastHLine(x + 3, y,  2, TFT_YELLOW);
      canvas->drawLine(x - 3, y    , x - 1, y - 6, TFT_YELLOW);
      canvas->drawLine(x - 1, y - 5, x + 1, y + 6, TFT_YELLOW);
      canvas->drawLine(x + 3, y    , x + 1, y + 6, TFT_YELLOW);
      break;
    }
  }
};
static ui_icon_auto_play_t ui_icon_auto_play;

struct ui_song_modified_t : public ui_base_t
{
protected:
  bool _modified = false;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    auto modified = system_registry->runtime_info.getSongModified();
    if (_modified != modified) {
      _modified = modified;
      if (!modified) {
        _target_rect.x += _target_rect.w;
        _target_rect.w = 0;
      } else {
        _target_rect.w = 9;
        _target_rect.x -= _target_rect.w;
      }
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
    ui_base_t::update_impl(param, offset_x, offset_y);
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  { // 未保存の変更ありアイコン
    int x = offset_x;
    int w = _client_rect.w;

    int y = offset_y;
    int h = _client_rect.h;

    int xc = x + (w >> 1);
    int yc = y + (h >> 1) + 4;

    canvas->drawLine(xc - 3, yc - 3, xc + 3, yc + 3, TFT_YELLOW);
    canvas->drawLine(xc - 3, yc + 3, xc + 3, yc - 3, TFT_YELLOW);
    canvas->drawFastHLine(xc - 4, yc,  9, TFT_YELLOW);
    canvas->drawFastVLine(xc, yc - 5, 11, TFT_YELLOW);
  }
};
static ui_song_modified_t ui_song_modified;

struct ui_playkey_info_t : public ui_base_t
{
protected:
    uint8_t _master_key = 0;
    int8_t _slot_key = 0;
public:
  void update_impl(draw_param_t *param, int offset_x, int offset_y) override {
    ui_base_t::update_impl(param, offset_x, offset_y);
    auto master_key = system_registry->runtime_info.getMasterKey();
    int slot_key = system_registry->current_slot->slot_info.getKeyOffset();
    if (_master_key != master_key || _slot_key != slot_key) {
      _master_key = master_key;
      _slot_key = slot_key;
      param->addInvalidatedRect({offset_x, offset_y, _client_rect.w, _client_rect.h});
    }
  }

  void draw_impl(draw_param_t *param, M5Canvas *canvas, int32_t offset_x,
                          int32_t offset_y, const rect_t *clip_rect) override
  {
    int x = offset_x + (_client_rect.w >> 1);
    int y = offset_y - (_slot_key ? 3 : 6);
    canvas->setTextSize(1, _slot_key ? 1 : 2);
    canvas->setTextDatum(m5gfx::datum_t::top_center);
    canvas->setTextColor(0xFFFFFFu);
    auto text = def::app::key_name_table[_master_key];
    canvas->drawString(text, x, y);
    if (_slot_key) {
      char buf[8];
      snprintf(buf, sizeof(buf), "%+d", _slot_key);
      canvas->drawString(buf, x, y + (_client_rect.h >> 1));
    }
  }
};
static ui_playkey_info_t ui_playkey_info;