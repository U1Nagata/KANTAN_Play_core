// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct mi_selector_t : public mi_normal_t {
  constexpr mi_selector_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, const text_array_t* names)
  : mi_normal_t { cate, menu_id, level, title }
  , _names { names }
  {}

  const char* getSelectorText(size_t index) const override { return _names->at(index)->get(); }
  size_t getSelectorCount(void) const override { return _names->size(); }

  const char* getValueText(void) const override
  {
    return _names->at(getValue() - getMinValue())->get();
  }

protected:
  const text_array_t* _names;
};

struct mi_language_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "English", "English" },
    { "日本語", "日本語" },
  }};

public:
  constexpr mi_language_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  int getValue(void) const override
  {
    return getMinValue() + static_cast<uint8_t>(system_registry->user_setting.getLanguage());
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto lang = static_cast<def::lang::language_t>(value - getMinValue());
    system_registry->user_setting.setLanguage(lang);
    return true;
  }
};


struct mi_imu_velocity_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 3, (const localize_text_t[]){
    { "Disable", "無効" },
    { "Normal",  "標準" },
    { "Strong",  "強め" },
  }};

public:
  constexpr mi_imu_velocity_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  int getValue(void) const override
  {
    return getMinValue() + system_registry->user_setting.getImuVelocityLevel();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setImuVelocityLevel(value);
    return true;
  }
};

struct mi_ext_midi_velocity_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Disabled", "無効" },
    { "Enabled",  "有効"},
  }};

public:
  constexpr mi_ext_midi_velocity_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->user_setting.getExtMidiVelocity();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setExtMidiVelocity(value);
    return true;
  }
};

struct mi_brightness_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 5, (const localize_text_t[]){
    { "Very Low",  "最暗" },     // Level 1
    { "Low",      "暗め" },     // Level 2
    { "Medium",   "標準" },     // Level 3
    { "High",     "明るめ" },   // Level 4
    { "Very High", "最明" },     // Level 5
  }};

public:
  constexpr mi_brightness_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
};

struct mi_lcd_backlight_t : public mi_brightness_t {
public:
  constexpr mi_lcd_backlight_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_brightness_t { cate, menu_id, level, title } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->user_setting.getDisplayBrightness();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setDisplayBrightness(value);
    return true;
  }
};

struct mi_led_brightness_t : public mi_brightness_t {
public:
  constexpr mi_led_brightness_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_brightness_t { cate, menu_id, level, title } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->user_setting.getLedBrightness();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setLedBrightness(value);
    system_registry->rgbled_control.refresh();
    return true;
  }
};

struct mi_vol_midi_t : public mi_normal_t {
  constexpr mi_vol_midi_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  int getMinValue(void) const override { return 10; }
  int getMaxValue(void) const override { return 127; }

  int getValue(void) const override
  {
    return system_registry->user_setting.getMIDIMasterVolume();
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    system_registry->user_setting.setMIDIMasterVolume(value);
    return true;
  }
  const char* getSelectorText(size_t index) const override {
    int tmp = index + getMinValue();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tmp);
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
  const char* getValueText(void) const override
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", getValue());
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
};

struct mi_vol_adcmic_t : public mi_normal_t {
  constexpr mi_vol_adcmic_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  int getMinValue(void) const override { return 0; }
  int getMaxValue(void) const override { return 11; }

  int getValue(void) const override
  {
    return system_registry->user_setting.getADCMicAmp();
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    system_registry->user_setting.setADCMicAmp(value);
    return true;
  }
  const char* getSelectorText(size_t index) const override {
    int tmp = index + getMinValue();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", tmp);
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
  const char* getValueText(void) const override
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", getValue());
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
};

struct mi_detail_view_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "icon view"  , "アイコン表示" },
    { "detail view", "詳細表示"     },
  }};

public:
  constexpr mi_detail_view_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  int getValue(void) const override
  {
    return getMinValue() + system_registry->user_setting.getGuiDetailMode();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setGuiDetailMode(value);
    return true;
  }
};

struct mi_enable_selector_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Off",     "オフ"  },
    { "On",      "オン"  },
  }};

public:
  constexpr mi_enable_selector_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
};

struct mi_wave_view_t : public mi_enable_selector_t {
public:
  constexpr mi_wave_view_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_enable_selector_t { cate, menu_id, level, title } {}

  int getValue(void) const override
  {
    return getMinValue() + static_cast<uint8_t>(system_registry->user_setting.getGuiWaveView());
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->user_setting.setGuiWaveView(value);
    return true;
  }
};


struct mi_webserver_t : public mi_enable_selector_t {
public:
  constexpr mi_webserver_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_enable_selector_t { cate, menu_id, level, title } {}

  int getValue(void) const override
  {
    return getMinValue() + static_cast<uint8_t>(system_registry->wifi_control.getWebServerMode());
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->wifi_control.setWebServerMode(static_cast<def::command::webserver_mode_t>(value));
    return true;
  }
};

/*
struct mi_usewifi_t : public mi_enable_selector_t {
public:
  constexpr mi_usewifi_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_enable_selector_t { cate, menu_id, level, title } {}

  int getValue(void) const override
  {
    return getMinValue() + static_cast<uint8_t>(system_registry->wifi_control.getMode());
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->wifi_control.setMode(static_cast<def::command::wifi_mode_t>(value));
    return true;
  }
};
//*/

struct mi_system_info_t : public mi_normal_t {
  constexpr mi_system_info_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title } {}

  const char* getValueText(void) const override {
    static char buf[16];
    snprintf(buf, sizeof(buf), "v%d.%d.%d", (int)def::app::app_version_major, (int)def::app::app_version_minor, (int)def::app::app_version_patch);
    return buf;
  }
  static constexpr const localize_text_t _selector_text = { "Show QR code", "QRコードを表示" };
  const char* getSelectorText(size_t index) const override { return _selector_text.get(); }

  size_t getSelectorCount(void) const override { return 1; }

  bool execute(void) const override
  {
    system_registry->popup_qr.setQRCodeType(def::qrcode_type_t::QRCODE_URL_SYSTEM_INFO);
    return false;
  }

  bool exit(void) const override
  {
    system_registry->popup_qr.setQRCodeType(def::qrcode_type_t::QRCODE_NONE);
    return mi_normal_t::exit();
  }
};

struct mi_manual_qr_t : public mi_normal_t {
  constexpr mi_manual_qr_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title } {}

  const char* getValueText(void) const override { return "..."; }
  static constexpr const localize_text_t _selector_text_manual = { "Show QR code", "QRコードを表示" };
  const char* getSelectorText(size_t index) const override { return _selector_text_manual.get(); }

  size_t getSelectorCount(void) const override { return 1; }

  bool execute(void) const override
  {
    system_registry->popup_qr.setQRCodeType(def::qrcode_type_t::QRCODE_URL_MANUAL);
    return false;
  }

  bool exit(void) const override
  {
    system_registry->popup_qr.setQRCodeType(def::qrcode_type_t::QRCODE_NONE);
    return mi_normal_t::exit();
  }
};

struct mi_all_reset_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel", "キャンセル" },
    { "Reset",  "リセット"   },
  }};

public:
  constexpr mi_all_reset_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  const char* getValueText(void) const override { return "..."; }

  int getValue(void) const override
  {
    return getMinValue();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      system_registry->reset();
      system_registry->save();
      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_ALL_RESET);
    }
    return true;
  }
};


#if 0
struct mi_intvalue_t : public mi_normal_t {
  constexpr mi_intvalue_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, const int16_t min_value, const int16_t max_value, const int16_t step)
  : mi_normal_t { cate, menu_id, level, title }
  , _min_value { min_value }
  , _max_value { max_value }
  , _step { step }
  {}
  virtual menu_item_type_t getType(void) const { return menu_item_type_t::input_number; }

  const char* getValueText(void) const override
  {
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d", getValue());
    return buf;
  }

  bool inputUpDown(int updown) const override
  {
    int value = getValue();
    value += updown * _step;
    if (value < _min_value) { value = _min_value; }
    if (value > _max_value) { value = _max_value; }
    return setValue(value);
  }

  bool inputNumber(uint8_t value) const override
  {
    if (value < _min_value) { value = _min_value; }
    if (value > _max_value) { value = _max_value; }
    return setValue(value);
  }

protected:
  int16_t _min_value;
  int16_t _max_value;
  int16_t _step;
};
#endif