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
    return system_registry->current_slot->chord_part_drum[part_index].getDrumNoteNumber(_pitch_number);
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    int part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part_drum[part_index].setDrumNoteNumber(_pitch_number, value);
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
