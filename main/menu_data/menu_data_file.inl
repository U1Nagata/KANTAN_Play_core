// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

// ファイル読み書き関連のメニュー項目

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

    // "." が2つ以上ある場合、最初の "." より前は分類記号なので非表示にする
    // 例: "G1-01.Pop_Basic1.json" → "Pop_Basic1.json" → 拡張子除去 → "Pop_Basic1"
    auto dot1 = _tmp_filename.find('.');
    if (dot1 != std::string::npos && _tmp_filename.find('.', dot1 + 1) != std::string::npos) {
      _tmp_filename = _tmp_filename.substr(dot1 + 1);
    }

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

// Blank プリセットをロードしてソングデータをリセットする項目
// Cancel / Reset の選択肢を表示してユーザーに確認する
struct mi_reset_song_t : public mi_cancel_exec_t {
protected:
  static constexpr const localize_text_array_t name_array = { 2, (const localize_text_t[]){
    { "Cancel", "キャンセル" },
    { "Reset",  "リセット"  },
  }};

public:
  constexpr mi_reset_song_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title )
  : mi_cancel_exec_t { cate, menu_id, level, title, &name_array } {}

  const char* getValueText(void) const override { return "..."; }
  int getValue(void) const override { return getMinValue(); }
  bool setValue(int value) const override
  {
    if (mi_selector_t::setValue(value) == false) { return false; }
    value -= getMinValue();
    if (value == 1) {
      system_registry->backup_song_data.assign(system_registry->song_data);
      auto mem = file_manage.loadFile(def::app::data_type_t::data_song_blank, (int)0);
      if (mem != nullptr) {
        system_registry->operator_command.addQueue( { def::command::file_load_notify, mem->index } );
      } else {
        system_registry->popup_notify.setPopup(false, def::notify_type_t::NOTIFY_FILE_LOAD);
      }
    }
    return true;
  }
};

// コード進行データのみをSDカードに保存する項目
struct mi_save_progression_t : public mi_normal_t {
  constexpr mi_save_progression_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::app::data_type_t dir_type )
  : mi_normal_t { cate, menu_id, level, title }
  , _dir_type { dir_type }
  {}
  static constexpr const size_t max_filenames = 2;
  def::app::data_type_t _dir_type;
protected:
  const char* getSelectorText(size_t index) const override {
    return _filenames[index].c_str();
  }
  size_t getSelectorCount(void) const override { return max_filenames; }
  const char* getValueText(void) const override { return "..."; }

  bool enter(void) const override
  {
    auto fn = file_manage.getDisplayFileName();
    if (fn.empty()) { fn = "progression"; }
    _filenames[0] = fn + "_prog.json";

    auto t = time(nullptr);
    auto tm = localtime(&t);
    char buf[64];
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d.json",
          tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
          tm->tm_hour, tm->tm_min, tm->tm_sec);
    _filenames[1] = buf;

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

        auto len = system_registry->saveProgressionJSON(mem->data, def::app::max_file_len);
        if (len > 0 && mem->data[0] == '{') {
          mem->size = len;
          result = file_manage.saveFile(_dir_type, mem->index);
        }
        mem->release();
      }
    }
    system_registry->popup_notify.setPopup(result, def::notify_type_t::NOTIFY_FILE_SAVE);
    file_manage.updateFileList(_dir_type);
    return mi_normal_t::execute();
  }
protected:
  static std::string _filenames[max_filenames];
};
std::string mi_save_progression_t::_filenames[max_filenames];

// コード進行データをSDカードから読み込む項目
struct mi_load_progression_t : public mi_normal_t {
  constexpr mi_load_progression_t( def::menu_category_t cate, uint16_t menu_id, uint8_t level, const localize_text_t& title, def::app::data_type_t dir_type )
  : mi_normal_t { cate, menu_id, level, title }, _dir_type { dir_type } {}

  menu_item_type_t getType(void) const override { return menu_item_type_t::mt_normal; }
  const char* getValueText(void) const override { return "..."; }
  size_t getSelectorCount(void) const override { return _file_count; }
  const char* getSelectorText(size_t index) const override {
    auto info = file_manage.getFileInfo(_dir_type, index);
    if (info == nullptr) { return ""; }
    _tmp_filename = info->filename;
    auto dot1 = _tmp_filename.find('.');
    if (dot1 != std::string::npos && _tmp_filename.find('.', dot1 + 1) != std::string::npos) {
      _tmp_filename = _tmp_filename.substr(dot1 + 1);
    }
    auto pos = _tmp_filename.rfind(".json");
    if (pos != std::string::npos) {
      _tmp_filename = _tmp_filename.substr(0, pos);
    }
    return _tmp_filename.c_str();
  }
  int getMinValue(void) const override { return 1; }
  int getMaxValue(void) const override { return _file_count; }

  bool enter(void) const override {
    file_manage.updateFileList(_dir_type);
    auto dir = file_manage.getDirManage(_dir_type);
    _file_count = (dir != nullptr) ? dir->getCount() : 0;
    _selecting_value = getMinValue();
    return (_file_count > 0) ? mi_normal_t::enter() : false;
  }

  bool execute(void) const override
  {
    auto file_index = _selecting_value - getMinValue();
    auto mem = file_manage.loadFile(_dir_type, file_index);
    bool result = false;
    if (mem) {
      result = system_registry->loadProgressionJSON(mem->data, mem->size);
      mem->release();
    }
    system_registry->popup_notify.setPopup(result, def::notify_type_t::NOTIFY_FILE_LOAD);
    if (result) {
      system_registry->clearProvisionalProgression();
      system_registry->player_command.addQueue({ def::command::chord_step_reset_request, 1 });
      const auto playmode = system_registry->runtime_info.getPlayMode();
      const bool is_guide_mode = (playmode == def::playmode::pm_guide_play
                                || playmode == def::playmode::pm_free_guide
                                || playmode == def::playmode::pm_auto_song);
      if (!is_guide_mode) {
        system_registry->operator_command.addQueue({ def::command::play_mode_set, def::playmode::pm_guide_play });
      }
    }
    return mi_normal_t::execute();
  }

protected:
  static size_t _file_count;
  def::app::data_type_t _dir_type;
};
size_t mi_load_progression_t::_file_count = 0;
