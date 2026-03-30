// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

struct mi_program_t : public mi_selector_t {
  constexpr mi_program_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &def::midi::program_name_table }
  {}
protected:
  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getTone() + getMinValue();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setTone(value);
    return true;
  }
};

struct mi_octave_t : public mi_normal_t {
  constexpr mi_octave_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  const char* getSelectorText(size_t index) const override {
    return def::app::position_name_table.at(index * 4)->get();
  }
  size_t getSelectorCount(void) const override { return (def::app::position_name_table.size() >> 2) + 1; }

  const char* getValueText(void) const override
  {
    return def::app::position_name_table.at( (getValue() - getMinValue()) << 2 )->get();
  }

  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return (system_registry->current_slot->chord_part[part_index].part_info.getPosition() >> 2) + 10;
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    int v = (value - 10) << 2;
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setPosition(v);
    return true;
  }
};

struct mi_voicing_t : public mi_normal_t {
  constexpr mi_voicing_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  const char* getSelectorText(size_t index) const override {
    return def::play::GetVoicingName(static_cast<KANTANMusic_Voicing>(index));
  }
  size_t getSelectorCount(void) const override { return KANTANMusic_Voicing::KANTANMusic_MAX_VOICING; }

  const char* getValueText(void) const override
  {
    return def::play::GetVoicingName(static_cast<KANTANMusic_Voicing>(getValue() - getMinValue()));
  }

  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getVoicing() + getMinValue();
  }
  bool setValue(int value) const override
  {
    if (mi_normal_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setVoicing(value);
    return true;
  }
};

struct mi_clear_notes_t : public mi_normal_t {
  constexpr mi_clear_notes_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  const char* getValueText(void) const override { return "..."; }
  const char* getSelectorText(size_t index) const override { return "Clear All Notes"; }

  bool execute(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].arpeggio.reset();
    system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_CLEAR_ALL_NOTES);
    return mi_normal_t::execute();
  }

  size_t getSelectorCount(void) const override { return 1; }
  int getValue(void) const override { return 0; }
  bool setValue(int value) const override { return true;}
};

struct mi_sequence_mode_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 5, (const localize_text_t[]){
    { "Free Play",  "フリープレイ" },
    { "Beat Play",  "ビートプレイ" },
    { "Guide Play", "ガイドプレイ" },
    { "Free + Guide",  "フリー + ガイド" },
    { "Auto Song",  "オートソング" },
  }};

  constexpr mi_sequence_mode_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    uint32_t res = system_registry->runtime_info.getSequenceMode();
    if (res >= def::seqmode::seqmode_max) {
      res = 0;
    }
    return static_cast<int>(res) + getMinValue();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value < 0) {
      value = 0;
    }
    if (value >= def::seqmode::seqmode_t::seqmode_max) {
      value = 0;
    }
    static constexpr def::seqmode::seqmode_t modes[] = {
      def::seqmode::seqmode_t::seq_free_play,
      def::seqmode::seqmode_t::seq_beat_play,
      def::seqmode::seqmode_t::seq_guide_play,
      def::seqmode::seqmode_t::seq_free_guide,
      def::seqmode::seqmode_t::seq_auto_song,
    };
    auto mode = modes[value];

    system_registry->operator_command.addQueue({ def::command::sequence_mode_set, mode });
    return true;
  }
};

struct mi_recording_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "On",  "オン" },
    { "Off", "オフ" },
  }};

  constexpr mi_recording_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    uint32_t res = system_registry->runtime_info.getGuiFlag_SongRecording()
                 ? 0 : 1;
    return static_cast<int>(res) + getMinValue();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    auto recording = (value == 0)
                   ? def::command::recording_control_t::rec_start
                   : def::command::recording_control_t::rec_stop;
    // system_registry->runtime_info.setGuiFlag_SongRecording(recording);
    system_registry->operator_command.addQueue({ def::command::recording_control, recording });
    return true;
  }
};

struct mi_seq_index_t : public mi_normal_t {
  constexpr mi_seq_index_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, int target_step )
  : mi_normal_t { cate, menu_id, level, title }
  , _target_step { static_cast<int8_t>(target_step) }
  {}
protected:
  // const char* getValueText(void) const override { return ""; }

  bool enter(void) const override
  {
    if (_target_step < 0) {
      system_registry->runtime_info.setSequenceStepIndex(system_registry->current_sequence->info.getLength());
    } else {
      system_registry->runtime_info.setSequenceStepIndex(0);
    }
    system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_SEQ_CURSOR_MOVE);
    return false;
  }

  // size_t getSelectorCount(void) const override { return 1; }
  // int getValue(void) const override { return 0; }
  // bool setValue(int value) const override { return true;}

  int8_t _target_step;
};

struct mi_seq_resize_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel",  "キャンセル" },
    { "Execute", "実行"      },
  }};

public:
  constexpr mi_seq_resize_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, int8_t mode )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  , _mode { mode }
  {}

  const char* getValueText(void) const override { return "..."; }
  int getValue(void) const override { return getMinValue(); }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      bool result;
      if (_mode > 1) {
        result = system_registry->current_sequence->stretch();
        system_registry->popup_notify.setPopup(result, def::notify_type_t::NOTIFY_SEQ_STRETCH);
      } else {
        result = system_registry->current_sequence->compress();
        system_registry->popup_notify.setPopup(result, def::notify_type_t::NOTIFY_SEQ_COMPRESS);
      }
    }
    return true;
  }

  int8_t _mode; // >1: stretch, <=1: compress
};

struct mi_clear_seq_t : public mi_selector_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel", "キャンセル" },
    { "Clear",  "クリア"   },
  }};

public:
  constexpr mi_clear_seq_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array } {}

  const char* getValueText(void) const override { return "..."; }
  int getValue(void) const override { return getMinValue(); }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      system_registry->current_sequence->deleteAfter(system_registry->runtime_info.getSequenceStepIndex());
      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_CLEAR_AFTER_CURSOR);
    }
    return true;
  }
/*
  bool execute(void) const override
  {
    if (!setValue(_selecting_value)) { return false; }
    // 値を確定したときに親階層に戻る場合はここでexit
    exit();
    return true;
  }
//*/
};


struct mi_percent_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 20, (const simple_text_t[]){
     "5%",  "10%",  "15%",  "20%",
    "25%",  "30%",  "35%",  "40%",
    "45%",  "50%",  "55%",  "60%",
    "65%",  "70%",  "75%",  "80%",
    "85%",  "90%",  "95%", "100%",
  }};

  constexpr mi_percent_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}
};

struct mi_partvolume_t : public mi_percent_t {
  constexpr mi_partvolume_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_percent_t { cate, menu_id, level, title}
  {}
protected:
  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getVolume() / 5;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setVolume(value * 5);
    return true;
  }
};

struct mi_velocity_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 21, (const simple_text_t[]){
    "mute",
      "5%",  "10%",  "15%",  "20%",
    "25%",  "30%",  "35%",  "40%",
    "45%",  "50%",  "55%",  "60%",
    "65%",  "70%",  "75%",  "80%",
    "85%",  "90%",  "95%", "100%",
  }};

  constexpr mi_velocity_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

protected:
  int getValue(void) const override
  {
    int velo = system_registry->runtime_info.getEditVelocity();
    if (velo < 0) return 1;
    return 1 + (velo / 5);
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    int velo = (value == 1) ? -5 : (value - 1) * 5;
    system_registry->runtime_info.setEditVelocity(velo);
    return true;
  }
};

struct mi_arpeggio_step_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 32, (const simple_text_t[]){
    "1",  "2",  "3",  "4",  "5",  "6",  "7",  "8",
    "9",  "10", "11", "12", "13", "14", "15", "16",
    "17", "18", "19", "20", "21", "22", "23", "24",
    "25", "26", "27", "28", "29", "30", "31", "32",
  }};

  constexpr mi_arpeggio_step_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}
};

struct mi_loop_length_t : public mi_arpeggio_step_t {
  constexpr mi_loop_length_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_arpeggio_step_t { cate, menu_id, level, title }
  {}
protected:
  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getLoopStep() / 2 + 1;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setLoopStep(value * 2 - 1);
    return true;
  }
};

struct mi_anchor_step_t : public mi_arpeggio_step_t {
  constexpr mi_anchor_step_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_arpeggio_step_t { cate, menu_id, level, title }
  {}
protected:
  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getAnchorStep() / 2 + 1;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setAnchorStep(value * 2 - 2);
    return true;
  }
};

struct mi_stroke_speed_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 10, (const simple_text_t[]){
    "5 msec",  "10 msec",  "15 msec",  "20 msec",
    "25 msec", "30 msec",  "35 msec",  "40 msec",
    "45 msec", "50 msec"
  }};

  constexpr mi_stroke_speed_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}
protected:
  int getValue(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    return system_registry->current_slot->chord_part[part_index].part_info.getStrokeSpeed() / 5;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto part_index = system_registry->chord_play.getEditTargetPart();
    system_registry->current_slot->chord_part[part_index].part_info.setStrokeSpeed(value * 5);
    return true;
  }
};

struct mi_offbeat_style_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Auto", "自動" },
    { "Self", "手動" },
  }};

  constexpr mi_offbeat_style_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    return system_registry->user_setting.getOffbeatStyle();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto style = def::play::offbeat_style_t::offbeat_auto;
    switch (value) {
    default: break;
    case 2: style = def::play::offbeat_style_t::offbeat_self; break;
    }
    system_registry->user_setting.setOffbeatStyle(style);
    return true;
  }
};

struct mi_slot_perform_style_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 3, (const localize_text_t[]){
    { "Chord Mode", "コード" },
    { "Note Mode",  "ノート" },
    { "Drum Mode",  "ドラム" },
  }};

  constexpr mi_slot_perform_style_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    switch (system_registry->runtime_info.getGui_PerformStyle()) {
    default:
    case def::perform_style_t::ps_chord: return 1;
    case def::perform_style_t::ps_note:  return 2;
    case def::perform_style_t::ps_drum:  return 3;
    }
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto mode = def::perform_style_t::ps_chord;
    switch (value) {
    default:
    case 1: break;
    case 2: mode = def::perform_style_t::ps_note; break;
    case 3: mode = def::perform_style_t::ps_drum; break;
    }
    system_registry->operator_command.addQueue({ def::command::perform_style_set, (int)mode });
    return true;
  }
};

struct mi_slot_key_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 23, (const simple_text_t[]){
    "- 11","- 10","- 9", " -8",
    "- 7", "- 6", "- 5", " -4",
    "- 3", "- 2", "- 1", "± 0",
    "+ 1", "+ 2", "+ 3", "+ 4",
    "+ 5", "+ 6", "+ 7", "+ 8",
    "+ 9", "+ 10","+ 11"
  }};

  constexpr mi_slot_key_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    auto key_offset = system_registry->current_slot->slot_info.getKeyOffset();
    return key_offset + 12;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    auto key_offset = value - 12;
    system_registry->current_slot->slot_info.setKeyOffset(key_offset);
    return true;
  }
};

// スロット別のステップ/ビート
struct mi_slot_step_beat_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 4, (const simple_text_t[]){
    "1", "2", "3", "4"
  }};

  constexpr mi_slot_step_beat_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    return system_registry->current_slot->slot_info.getStepPerBeat();
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    system_registry->current_slot->slot_info.setStepPerBeat(value);
    return true;
  }
};

// 全スロット一括のステップ/ビート
struct mi_song_step_beat_t : public mi_selector_t {
  static constexpr const simple_text_array_t name_array = { 5, (const simple_text_t[]){
    "1", "2", "3", "4", "Each"
  }};

  constexpr mi_song_step_beat_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  int getValue(void) const override
  {
    auto step_per_beat = system_registry->current_slot->slot_info.getStepPerBeat();
    for (size_t i = 0; i < def::app::max_slot; ++i) {
      if (system_registry->song_data.slot[i].slot_info.getStepPerBeat() != step_per_beat) {
        return 5; // "Each"
      }
    }
    return step_per_beat;
  }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    if (value == 5) { // "Each"
      return true;
    }
    auto prev_value = getValue();
    for (size_t i = 0; i < def::app::max_slot; ++i) {
      system_registry->song_data.slot[i].slot_info.setStepPerBeat(value);
    }
    if ((prev_value != value) && (prev_value <= 4)) {
      auto tempo = system_registry->song_data.song_info.getTempo();

      // ステップ/ビートが変更された場合、テンポを調整する
      uint16_t new_tempo = (tempo * prev_value) / value;
      system_registry->song_data.song_info.setTempo(new_tempo);
    }

    return true;
  }
};

struct mi_slot_clipboard_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Copy Setting"  , "設定コピー" },
    { "Paste Setting" , "設定ペースト" },
  }};

  const char* getValueText(void) const override { return "..."; }

  constexpr mi_slot_clipboard_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  bool execute(void) const override
  {
    // auto part_index = system_registry->chord_play.getEditTargetPart();
    // auto slot_index = system_registry->runtime_info.getPlaySlot();
    // auto slot = &system_registry->song_data.slot[slot_index];
    switch (getSelectingValue()) {
    case 1:
      system_registry->clipboard_slot.assign(*system_registry->current_slot);
      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_COPY_SLOT_SETTING);
      system_registry->clipboard_content = system_registry_t::clipboard_contetn_t::CLIPBOARD_CONTENT_SLOT;
      // M5_LOGV("mi_slot_clipboard_t: Copy Setting");
      break;

    case 2:
      {
        bool flg = (system_registry->clipboard_content == system_registry_t::clipboard_contetn_t::CLIPBOARD_CONTENT_SLOT);
        if (flg) {
          system_registry->current_slot->assign(system_registry->clipboard_slot);
        }
        system_registry->popup_notify.setPopup(flg, def::notify_type_t::NOTIFY_PASTE_SLOT_SETTING);
      }
      break;

    default:
      // M5_LOGV("mi_slot_clipboard_t: unknown: %d", getValue());
      return false;
    }
    return mi_selector_t::execute();
  }
};

struct mi_slot_clear_notes_t : public mi_normal_t {
  constexpr mi_slot_clear_notes_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title }
  {}
protected:
  const char* getValueText(void) const override { return "..."; }
  const char* getSelectorText(size_t index) const override { return "Clear All Notes"; }

  bool execute(void) const override
  {
    for (int i = 0; i < def::app::max_chord_part; ++i) {
      system_registry->current_slot->chord_part[i].arpeggio.reset();
    }
    system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_CLEAR_ALL_NOTES);
    return mi_normal_t::execute();
  }

  size_t getSelectorCount(void) const override { return 1; }
  int getValue(void) const override { return 0; }
  bool setValue(int value) const override { return true;}
};

struct mi_part_clipboard_t : public mi_selector_t {
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Copy Part"    , "パートコピー" },
    { "Paste Part"   , "パートペースト" },
  }};

  const char* getValueText(void) const override { return "..."; }

  constexpr mi_part_clipboard_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_selector_t { cate, menu_id, level, title, &name_array }
  {}

  bool execute(void) const override
  {
    auto part_index = system_registry->chord_play.getEditTargetPart();
    switch (getSelectingValue()) {
    case 1:
      system_registry->clipboard_slot.chord_part[0].assign(system_registry->current_slot->chord_part[part_index]);
      system_registry->popup_notify.setPopup(true, def::notify_type_t::NOTIFY_COPY_PART_SETTING);
      system_registry->clipboard_content = system_registry_t::clipboard_contetn_t::CLIPBOARD_CONTENT_PART;
      break;

    case 2:
      {
        bool flg = (system_registry->clipboard_content == system_registry_t::clipboard_contetn_t::CLIPBOARD_CONTENT_PART);
        if (flg) {
          system_registry->current_slot->chord_part[part_index].assign(system_registry->clipboard_slot.chord_part[0]);
        }
        system_registry->popup_notify.setPopup(flg, def::notify_type_t::NOTIFY_PASTE_PART_SETTING);
      }
      break;

    default:
      // M5_LOGV("mi_part_clipboard_t: unknown: %d", getValue());
      return false;
    }
    return mi_selector_t::execute();
  }
};

// パート簡易編集メニューからアルペジオ編集（従来のパート編集モード）に遷移する項目
struct mi_arpeggio_edit_t : public mi_normal_t {
  constexpr mi_arpeggio_edit_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_normal_t { cate, menu_id, level, title } {}

  menu_item_type_t getType(void) const override { return menu_item_type_t::mt_tree; }

  bool enter(void) const override {
    // アルペジオ編集コマンドを発行（対象パート番号を1始まりで渡す）
    system_registry->operator_command.addQueue({ def::command::part_edit_enter, system_registry->chord_play.getEditTargetPart() + 1 });
    // メニューを閉じる
    system_registry->operator_command.addQueue({ def::command::menu_function, def::command::mf_exit });
    return false;
  }
};
