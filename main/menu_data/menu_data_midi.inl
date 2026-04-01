// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct mi_song_tempo_t : public mi_normal_t {
  constexpr mi_song_tempo_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  int getMinValue(void) const override { return def::app::tempo_bpm_min; }
  int getMaxValue(void) const override { return def::app::tempo_bpm_max; }
  // size_t getSelectorCount(void) const override { return def::app::tempo_bpm_max - def::app::tempo_bpm_min + 1; }

  int getValue(void) const override
  {
    return system_registry->song_data.song_info.getTempo();
    // return system_registry->current_slot->slot_info.getTempo();
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    system_registry->song_data.song_info.setTempo(value);
/*
    system_registry->current_slot->slot_info.setTempo(value);
    for (int i = 0; i < def::app::max_slot; ++i) {
      system_registry->song_data.slot[i].slot_info.setTempo(value);
    }
*/
    return true;
  }
  const char* getSelectorText(size_t index) const override {
    int tempo = index + getMinValue();
    char buf[16];
    snprintf(buf, sizeof(buf), "%d bpm", tempo);
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
  const char* getValueText(void) const override
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d bpm", getValue());
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
};

struct mi_song_swing_t : public mi_normal_t {
  constexpr mi_song_swing_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  int getMinValue(void) const override { return def::app::swing_percent_min; }
  int getMaxValue(void) const override { return def::app::swing_percent_max / 10; }

  int getValue(void) const override
  {
    return system_registry->song_data.song_info.getSwing() / 10;
    // return system_registry->current_slot->slot_info.getSwing();
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    system_registry->song_data.song_info.setSwing(value * 10);
/*
    system_registry->current_slot->slot_info.setSwing(value);
    for (int i = 0; i < def::app::max_slot; ++i) {
      system_registry->song_data.slot[i].slot_info.setSwing(value);
    }
*/
    return true;
  }
  const char* getSelectorText(size_t index) const override {
    int sw = index * 10;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %%", sw);
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
  const char* getValueText(void) const override
  {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d %%", getValue() * 10);
    _title_text_buffer = buf;
    return _title_text_buffer.c_str();
  }
};

struct mi_drum_note_t : public mi_selector_t {
  constexpr mi_drum_note_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, uint8_t pitch_number )
  : mi_selector_t { cate, menu_id, level, title, &def::midi::drum_note_name_tbl } // 35 = Acoustic Bass Drum, 81 = Open Triangle
  , _pitch_number { pitch_number }
  {}
protected:
  const uint8_t _pitch_number;

  // 設定可能な最小値を取得する
  int getMinValue(void) const override { return def::midi::drum_note_name_min; }

  int getValue(void) const override
  {
    int part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->song_data.chord_part_drum[part_index].getDrumNoteNumber(_pitch_number);
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    int part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->song_data.chord_part_drum[part_index].setDrumNoteNumber(_pitch_number, value);
    return true;
  }
};

struct mi_ctrl_assign_t : public mi_normal_t {
  constexpr mi_ctrl_assign_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, const def::ctrl_assign::control_assignment_t table[], uint16_t size, def::mapping::target_t map_target)
  : mi_normal_t { cate, menu_id, level, title }
  , _table { table }
  , _size { size }
  , _map_target { map_target }
  {}

  const char* getSelectorText(size_t index) const override { return _table[index].text.get(); }
  size_t getSelectorCount(void) const override { return _size; }

  const char* getValueText(void) const override
  {
    return _table[getValue() - getMinValue()].text.get();
  }

  bool exit(void) const override
  {
    system_registry->updateControlMapping();
    return mi_normal_t::exit();
  }

protected:
  const def::ctrl_assign::control_assignment_t* _table;
  const uint16_t _size;
  const def::mapping::target_t _map_target;
};

struct mi_cmap_copy_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel", "キャンセル" },
    { "Copy",   "コピー"   },
  }};

public:
  constexpr mi_cmap_copy_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::mapping::target_t map_target )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  , _map_target { map_target }
  {
  }

  const char* getValueText(void) const override { return "..."; }
  int getValue(void) const override { return getMinValue(); }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      auto dst_mapping = &system_registry->control_mapping[(int)_map_target];
      auto src_mapping = &system_registry->control_mapping[1-(int)_map_target];
      dst_mapping->internal.assign(src_mapping->internal);
      dst_mapping->external.assign(src_mapping->external);
      dst_mapping->midinote.assign(src_mapping->midinote);

      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_COPY_CONTROL_MAPPING);
      system_registry->updateControlMapping();
    }
    return true;
  }

protected:
  const def::mapping::target_t _map_target;
};

struct mi_cmap_delete_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel", "キャンセル" },
    { "Delete", "削除"   },
  }};

public:
  constexpr mi_cmap_delete_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::mapping::target_t map_target )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  , _map_target { map_target }
  {
  }

  const char* getValueText(void) const override { return "..."; }
  int getValue(void) const override { return getMinValue(); }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      auto dst_mapping = &system_registry->control_mapping[(int)_map_target];
      dst_mapping->reset();
      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_DELETE_CONTROL_MAPPING);
    }
    return true;
  }

protected:
  const def::mapping::target_t _map_target;
};

// control assignment for internal
struct mi_ca_internal_t : public mi_ctrl_assign_t {
public:
  constexpr mi_ca_internal_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, uint8_t button_index, def::mapping::target_t map_target)
  : mi_ctrl_assign_t { cate, menu_id, level, title, def::ctrl_assign::playbutton_table, sizeof(def::ctrl_assign::playbutton_table) / sizeof(def::ctrl_assign::playbutton_table[0])-1, map_target }
  , _button_index { button_index } {}

  system_registry_t::reg_command_mapping_t* target(void) const { return &system_registry->control_mapping[(int)_map_target].internal; }

  int getValue(void) const override
  {
    auto cmd = target()->getCommandParamArray(_button_index);
    int index = def::ctrl_assign::get_index_from_command(_table, cmd);
    if (index < 0) {
      index = 0;
    }
    return getMinValue() + index;
  }
  bool setValue(int value) const override
  {
    if (mi_ctrl_assign_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    target()->setCommandParamArray(_button_index, _table[value].command);
    return true;
  }

protected:
  const uint8_t _button_index;
};

struct mi_ca_external_t : public mi_ctrl_assign_t {
public:
  constexpr mi_ca_external_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, uint8_t button_index, def::mapping::target_t map_target)
  : mi_ctrl_assign_t { cate, menu_id, level, title, def::ctrl_assign::external_table, sizeof(def::ctrl_assign::external_table) / sizeof(def::ctrl_assign::external_table[0])-1, map_target }
  , _button_index { button_index } {}

  system_registry_t::reg_command_mapping_t* target(void) const { return &system_registry->control_mapping[(int)_map_target].external; }

  int getValue(void) const override
  {
    auto cmd = target()->getCommandParamArray(_button_index);
    int index = def::ctrl_assign::get_index_from_command(_table, cmd);
    if (index < 0) {
      index = 0;
    }
    return getMinValue() + index;
  }
  bool setValue(int value) const override
  {
    if (mi_ctrl_assign_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    target()->setCommandParamArray(_button_index, _table[value].command);
    return true;
  }

protected:
  const uint8_t _button_index;
};

struct mi_ca_midinote_t : public mi_ctrl_assign_t {
public:
  constexpr mi_ca_midinote_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, uint8_t button_index, def::mapping::target_t map_target)
  : mi_ctrl_assign_t { cate, menu_id, level, title, def::ctrl_assign::external_table, sizeof(def::ctrl_assign::external_table) / sizeof(def::ctrl_assign::external_table[0])-1, map_target }
  , _button_index { button_index } {}

  system_registry_t::reg_command_mapping_t* target(void) const { return &system_registry->control_mapping[(int)_map_target].midinote; }

  int getValue(void) const override
  {
    auto cmd = target()->getCommandParamArray(_button_index);
    int index = def::ctrl_assign::get_index_from_command(_table, cmd);
    if (index < 0) {
      index = 0;
    }
    return getMinValue() + index;
  }
  bool setValue(int value) const override
  {
    if (mi_ctrl_assign_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    target()->setCommandParamArray(_button_index, _table[value].command);
    return true;
  }

protected:
  const uint8_t _button_index;
};

struct mi_midi_selector_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 4, (const localize_text_t[]){
    { "Off",      "オフ" },
    { "Output",   "出力" },
    { "Input",    "入力" },
    { "In + Out", "入出力" },
  }};

public:
  constexpr mi_midi_selector_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
};

struct mi_portc_midi_t : public mi_midi_selector_t {
  constexpr mi_portc_midi_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_midi_selector_t { cate, menu_id, level, title } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getPortCMIDI();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setPortCMIDI( static_cast<def::command::ex_midi_mode_t>(value));
    return true;
  }
};

struct mi_ble_midi_t : public mi_midi_selector_t {
  constexpr mi_ble_midi_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_midi_selector_t { cate, menu_id, level, title } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getBLEMIDI();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setBLEMIDI( static_cast<def::command::ex_midi_mode_t>(value));
    return true;
  }
};

struct mi_usb_midi_t : public mi_midi_selector_t {
  constexpr mi_usb_midi_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_midi_selector_t { cate, menu_id, level, title } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getUSBMIDI();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setUSBMIDI( static_cast<def::command::ex_midi_mode_t>(value));
    return true;
  }
};

struct mi_usb_mode_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Host",            "ホスト" },
    { "Device (to PC)" , "デバイス(→PC)"   },
  }};

public:
  constexpr mi_usb_mode_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getUSBMode();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setUSBMode( static_cast<def::command::usb_mode_t>(value));
    return true;
  }
};

struct mi_usb_power_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Off", "給電しない" },
    { "On" , "給電する"   },
  }};

public:
  constexpr mi_usb_power_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getUSBPowerEnabled();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setUSBPowerEnabled( static_cast<bool>(value));
    return true;
  }
};

struct mi_iclink_port_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 3, (const localize_text_t[]){
    { "Off",   "オフ" },
    { "BLE", nullptr },
    { "USB", nullptr },
  }};

public:
  constexpr mi_iclink_port_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getInstaChordLinkPort();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setInstaChordLinkPort( static_cast<def::command::instachord_link_port_t>(value));
    return true;
  }
};

struct mi_iclink_dev_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "KANTAN Play", "かんぷれ" },
    { "InstaChord",  "インスタコード"},
  }};

public:
  constexpr mi_iclink_dev_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getInstaChordLinkDev();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setInstaChordLinkDev( static_cast<def::command::instachord_link_dev_t>(value));
    return true;
  }
};

struct mi_iclink_style_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Button", "ボタン" },
    { "Pad",  "パッド"},
  }};

public:
  constexpr mi_iclink_style_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->midi_port_setting.getInstaChordLinkStyle();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->midi_port_setting.setInstaChordLinkStyle( static_cast<def::command::instachord_link_style_t>(value));
    return true;
  }
};

struct mi_otaupdate_t : public mi_normal_t {
  constexpr mi_otaupdate_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title } {}
  menu_item_type_t getType(void) const override { return menu_item_type_t::show_progress; }

  bool setSelectingValue(int value) const override { return false; }
  bool execute(void) const override { return false; }
  bool inputUpDown(int updown) const override { return false; }
  bool inputNumber(uint8_t number) const override { return false; }

  bool enter(void) const override
  {
    // OTAを実施する際にオートプレイは無効にする
    system_registry->runtime_info.setAutoplayState(def::play::auto_play_state_t::auto_play_none);

    system_registry->runtime_info.setWiFiOtaProgress(def::command::wifi_ota_state_t::ota_connecting);
    system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_ota_begin);
    return mi_normal_t::enter();
  }
  bool exit(void) const override
  {
    auto v = getSelectingValue();
    if (0 < v && v <= 100) {
      // OTAの途中でメニューを閉じることはできない
      return true;
    }
    system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_disable);
    system_registry->runtime_info.setWiFiOtaProgress(0);
    return mi_normal_t::exit();
  }

  std::string getString(void) const override {
    char buf[32];
    std::string result;
    auto v = getSelectingValue();
    switch (v) {
    case (uint8_t)def::command::wifi_ota_state_t::ota_connecting:         snprintf(buf, sizeof(buf), "Connecting.");           break;
    case (uint8_t)def::command::wifi_ota_state_t::ota_connection_error:   snprintf(buf, sizeof(buf), "Connection error.");     break;
    case (uint8_t)def::command::wifi_ota_state_t::ota_update_available:   snprintf(buf, sizeof(buf), "Download.");             break;
    case (uint8_t)def::command::wifi_ota_state_t::ota_already_up_to_date: snprintf(buf, sizeof(buf), "Already up to date.");   break;
    case (uint8_t)def::command::wifi_ota_state_t::ota_update_failed:      snprintf(buf, sizeof(buf), "Update failed.");        break;
    case (uint8_t)def::command::wifi_ota_state_t::ota_update_done:        snprintf(buf, sizeof(buf), "Update Done.");          break;
    default:
      snprintf(buf, sizeof(buf), "Download :% 3d %%", v);
      break;
    }
    return std::string(buf);
  }

  int getSelectingValue(void) const override
  {
    return system_registry->runtime_info.getWiFiOtaProgress();
  }
};

struct mi_wifiap_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Use Smartphone", "スマホで設定" },
    { "WPS"           , "WPSで設定"   },
  }};

public:
  constexpr mi_wifiap_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  const char* getValueText(void) const override { return "..."; }

  int getSelectingValue(void) const override
  {
    auto qrtype = def::qrcode_type_t::QRCODE_NONE;

    auto result = mi_selector_t::getSelectingValue();

    if (result == 1) {
      if (system_registry->wifi_control.getOperation() == def::command::wifi_operation_t::wfop_setup_ap) {
        qrtype = system_registry->runtime_info.getWiFiStationCount()
                    ? def::qrcode_type_t::QRCODE_URL_DEVICE
                    : def::qrcode_type_t::QRCODE_AP_SSID;
      }
    }
    if (system_registry->popup_qr.getQRCodeType() != qrtype) {
      system_registry->popup_qr.setQRCodeType(qrtype);
      if (result == 1 && qrtype == def::qrcode_type_t::QRCODE_NONE) {
        exit();
      }
    }
    return result;
  }
  bool execute(void) const override
  {
    if (getSelectingValue() == 1) {
      system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_setup_ap);
    } else {
      system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_setup_wps);
    }
    return false;
  }

  bool exit(void) const override
  {
    system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_disable);
    system_registry->popup_qr.setQRCodeType(def::qrcode_type_t::QRCODE_NONE);
    return mi_normal_t::exit();
  }
};

static std::string _tmp_filename;
struct mi_filelist_t : public mi_normal_t {
  constexpr mi_filelist_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::app::data_type_t dir_type )
  : mi_normal_t { cate, menu_id, level, title }
  , _dir_type { dir_type }
  {}
protected:
  def::app::data_type_t _dir_type;

  const char* getSelectorText(size_t index) const override {
    auto fileinfo = file_manage.getFileInfo(_dir_type, index);
    _tmp_filename = fileinfo->filename;

    // 末尾の拡張子 .json を削除
    auto pos = _tmp_filename.rfind(".json");
    if (pos != std::string::npos) {
      _tmp_filename = _tmp_filename.substr(0, pos);
    }

    return _tmp_filename.c_str();
  }

  size_t getSelectorCount(void) const override { return file_manage.getDirManage(_dir_type)->getCount(); }

  const char* getValueText(void) const override
  {
    return "...";
  }

  int getValue(void) const override
  {
    if (_dir_type == file_manage.getLatestDataType()) {
      return file_manage.getLatestFileIndex() + getMinValue();
    }
    return -1;
  }

  bool exit(void) const override
  {
    // ファイルメニューから抜ける時はオートプレイは無効にする
    system_registry->runtime_info.setAutoplayState(def::play::auto_play_state_t::auto_play_none);
    system_registry->runtime_info.setProgressionPosition(0);
    return mi_normal_t::exit();
  }

};

struct mi_load_file_t : public mi_filelist_t {
  constexpr mi_load_file_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::app::data_type_t dir_type, size_t top_index = 1 )
  : mi_filelist_t { cate, menu_id, level, title, dir_type }
  , _top_index { top_index }
  {
  }
protected:
  const size_t _top_index;
  int getMinValue(void) const override { return _top_index; }

  bool enter(void) const override
  {
    system_registry->backup_song_data.assign(system_registry->song_data);
    file_manage.updateFileList(_dir_type);

    return mi_filelist_t::enter();
  }
  bool execute(void) const override
  {
    auto fileinfo = file_manage.getFileInfo(_dir_type, _selecting_value - getMinValue());
    auto mem = file_manage.loadFile(_dir_type, fileinfo->filename);
    if (mem != nullptr) {
      system_registry->operator_command.addQueue( { def::command::file_load_notify, mem->index } );
      std::string filename = fileinfo->filename;

      system_registry->control_mapping[1].reset();
      system_registry->updateUnchangedKmapCRC32();

      // 拡張子を探す (末尾から . を探す)
      auto pos = filename.rfind(".");
      // 拡張子が見つかったら削除
      if (pos != std::string::npos) { filename = filename.substr(0, pos); }
      // 拡張子を追加する
      filename += def::app::fileext_kmap;

      auto mem_kmap = file_manage.loadFile(_dir_type, filename.c_str());
      if (mem_kmap != nullptr) {
        mem_kmap->dir_type = def::app::data_type_t::data_kmap;
        system_registry->operator_command.addQueue( { def::command::file_load_notify, mem_kmap->index } );
      }
    } else {
      system_registry->popup_notify.setPopup(false, def::notify_type_t::NOTIFY_FILE_LOAD);
    }
    return mi_filelist_t::execute();
  }
};

struct mi_save_t : public mi_normal_t {
  constexpr mi_save_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::app::data_type_t dir_type )
  : mi_normal_t { cate, menu_id, level, title }
  , _dir_type { dir_type }
  {}
  static constexpr const size_t max_filenames = 4;
  def::app::data_type_t _dir_type;
protected:
  const char* getSelectorText(size_t index) const override {
    return _filenames[index].c_str();
  }

  size_t getSelectorCount(void) const override { return max_filenames; }

  const char* getValueText(void) const override
  {
    return "...";
  }

  bool enter(void) const override
  {
    auto fn = file_manage.getDisplayFileName();
    if (fn.empty()) {
      fn = "new_song";
    }
    _filenames[0] = fn + ".json";
    _filenames[1] = fn + "_.json";
    _filenames[2] = "_" + fn + ".json";

    auto t = time(nullptr);
    auto tm = localtime(&t);
    char buf[64];

    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d.json",
          tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
          tm->tm_hour, tm->tm_min, tm->tm_sec);
    _filenames[3] = buf;

    _selecting_value = getMinValue();

    return mi_normal_t::enter();
  }

  bool execute(void) const override
  {
    auto index = _selecting_value - getMinValue();
    bool result = false;
    {
      auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
      if (mem) {
        mem->filename = _filenames[index];
        mem->dir_type = _dir_type;

        auto len = system_registry->song_data.saveSongJSON(mem->data, def::app::max_file_len);
        if (len > 0 && mem->data[0] == '{') {
          mem->size = len;
          result = file_manage.saveFile(_dir_type, mem->index);
        }
        mem->release();
      }
    }
    if (result)
    { // コントロールマッピング .kmap も保存する
      std::string filename = _filenames[index];
      // 拡張子を探す (末尾から . を探す)
      auto pos = filename.rfind(".");
      // 拡張子が見つかったら削除
      if (pos != std::string::npos) { filename = filename.substr(0, pos); }
      filename += def::app::fileext_kmap;

      if (system_registry->control_mapping[1].empty()) {
        // 保存するデータが無い場合は既存KMAPファイルを削除する
        file_manage.removeFile(_dir_type, filename.c_str());
      } else {
        auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
        if (mem) {
          // 拡張子を追加する
          mem->filename = filename;
          mem->dir_type = _dir_type;

          auto len = system_registry->control_mapping[1].saveJSON(mem->data, def::app::max_file_len);
          if (len > 0 && mem->data[0] == '{') {
            mem->size = len;
            result = file_manage.saveFile(_dir_type, mem->index) && result;
          }
          mem->release();
        }
      }
    }
    system_registry->popup_notify.setPopup(result, def::notify_type_t::NOTIFY_FILE_SAVE);
    if (result) {
      system_registry->updateUnchangedSongCRC32();
      system_registry->updateUnchangedKmapCRC32();
      file_manage.setLatestFileInfo(_dir_type, _filenames[index].c_str());
      // レジュームの状態に影響があるのでここで保存しておく
      system_registry->save();
    }
    file_manage.updateFileList(_dir_type);
    // // 未保存の編集の警告表示を更新する
    system_registry->checkSongModified();

    return mi_normal_t::execute();
  }
protected:
  static std::string _filenames[max_filenames];
};
std::string mi_save_t::_filenames[max_filenames];

struct mi_song_autorepeat_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Off", "オフ" },
    { "On",  "オン" },
  }};

public:
  constexpr mi_song_autorepeat_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->runtime_info.getSongAutoRepeat();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->runtime_info.setSongAutoRepeat(value);
    return true;
  }
};

struct mi_song_part_operation_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Auto",   "自動" },
    { "Manual", "手動" },
  }};

public:
  constexpr mi_song_part_operation_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}
  int getValue(void) const override
  {
    return getMinValue() + system_registry->runtime_info.getSongPartOperation();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    system_registry->runtime_info.setSongPartOperation(value);
    return true;
  }
};

struct mi_back_to_freeplay_t : public mi_normal_t {
  constexpr mi_back_to_freeplay_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title } {}

  menu_item_type_t getType(void) const override { return menu_item_type_t::mt_tree; }

  bool enter(void) const override {
    // フリープレイに切り替える
    system_registry->operator_command.addQueue({ def::command::play_mode_set, def::playmode::pm_free_play });
    // メニューを閉じる
    system_registry->operator_command.addQueue({ def::command::menu_function, def::command::mf_exit });
    return false;
  }
};
