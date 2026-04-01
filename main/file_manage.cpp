// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.


#include <M5Unified.h>

#if __has_include(<LittleFS.h>)
 #include <LittleFS.h>
#endif

#if __has_include(<SdFat.h>)
 #define DISABLE_FS_H_WARNING
 #include <SdFat.h>
 SdFat SD;
#elif __has_include(<SD.h>)
// #include <SD.h>
#else
 #include <filesystem>
 #include <stdio.h>
#endif


#include "file_manage.hpp"

#include "system_registry.hpp"

#include <set>


// ファイルインポートマクロ

// m1mac用のインポートマクロ
#if defined (__APPLE__) && defined (__MACH__) && defined (__arm64__)

#define IMPORT_FILE(section, path, filename, symbol) \
static constexpr const char* filename_##symbol = filename; \
extern const uint8_t symbol[], sizeof_##symbol[]; \
asm (\
  ".section __DATA,__data \n"\
  ".balign 4\n_"\
  #symbol ":\n"\
  ".incbin \"incbin/preset/" path filename "\"\n"\
  ".global _sizeof_" #symbol "\n"\
  ".set _sizeof_" #symbol ", . - _" #symbol "\n"\
  ".global _" #symbol "\n"\
  ".balign 4\n"\
  ".text \n")

#else

#define IMPORT_FILE(section, path, filename, symbol) \
static constexpr const char* filename_##symbol = filename; \
extern const uint8_t symbol[], sizeof_##symbol[]; \
asm (\
  ".section " #section "\n"\
  ".balign 4\n"\
  ".global " #symbol "\n"\
  #symbol ":\n"\
  ".incbin \"incbin/preset/" path filename "\"\n"\
  ".global sizeof_" #symbol "\n"\
  ".set sizeof_" #symbol ", . - " #symbol "\n"\
  ".balign 4\n"\
  ".section \".text\"\n")
#endif

// ソングプリセット: ジャンル別パターン
#define ENTRY(idx, filename) IMPORT_FILE(.rodata, "song_genre/", filename, sg_##idx);
#include "../incbin/preset/song_genre/_list.inl"
#undef ENTRY

// ソングプリセット: 楽曲データ
#define ENTRY(idx, filename) IMPORT_FILE(.rodata, "song_song/", filename, ss_##idx);
#include "../incbin/preset/song_song/_list.inl"
#undef ENTRY

namespace kanplay_ns {

  
void spi_lock(void);
void spi_unlock(void);


// ソングプリセット: ジャンル別パターン
#define ENTRY(idx, filename) { filename_sg_##idx, sg_##idx, (size_t)sizeof_sg_##idx },
static const incbin_file_t incbin_song_genre[] = {
#include "../incbin/preset/song_genre/_list.inl"
};
#undef ENTRY

// ソングプリセット: 楽曲データ
#define ENTRY(idx, filename) { filename_ss_##idx, ss_##idx, (size_t)sizeof_ss_##idx },
static const incbin_file_t incbin_song_song[] = {
#include "../incbin/preset/song_song/_list.inl"
};
#undef ENTRY

// extern instance
storage_sd_t storage_sd;
storage_littlefs_t storage_littlefs;
file_manage_t file_manage;

static storage_incbin_t storage_incbin_song_genre  { incbin_song_genre, sizeof(incbin_song_genre) / sizeof(incbin_song_genre[0]) };
static storage_incbin_t storage_incbin_song_song   { incbin_song_song,  sizeof(incbin_song_song)  / sizeof(incbin_song_song[0]) };
static storage_incbin_t storage_incbin_arp_empty    { nullptr, 0 };

using dt = def::app::data_type_t;
static dir_manage_t dir_manage[dt::data_type_max] =
{ { nullptr                    ,                             "" }, // data_unknown
  { &storage_sd                , def::app::data_path[dt::data_song_users         ] }, // data_song_users
  { &storage_sd                , def::app::data_path[dt::data_song_extra         ] }, // data_song_extra
  { &storage_incbin_song_genre , def::app::data_path[dt::data_song_preset_genre  ] }, // data_song_preset_genre
  { &storage_incbin_song_song  , def::app::data_path[dt::data_song_preset_song   ] }, // data_song_preset_song
  { &storage_littlefs          , def::app::data_path[dt::data_system             ] }, // data_system
  { &storage_sd                , def::app::data_path[dt::data_progression_users  ] }, // data_progression_users
  { &storage_sd                , def::app::data_path[dt::data_arpeggio_users     ] }, // data_arpeggio_user
  { &storage_incbin_arp_empty  , def::app::data_path[dt::data_arpeggio_drum      ] }, // data_arpeggio_drum   (データ未追加)
  { &storage_incbin_arp_empty  , def::app::data_path[dt::data_arpeggio_bass      ] }, // data_arpeggio_bass   (データ未追加)
  { &storage_incbin_arp_empty  , def::app::data_path[dt::data_arpeggio_guitar    ] }, // data_arpeggio_guitar (データ未追加)
  { &storage_incbin_arp_empty  , def::app::data_path[dt::data_arpeggio_piano     ] }, // data_arpeggio_piano  (データ未追加)
  { &storage_incbin_arp_empty  , def::app::data_path[dt::data_arpeggio_other     ] }, // data_arpeggio_other  (データ未追加)
};

static std::string trimExtension(const std::string& filename)
{
  auto pos = filename.find_last_of('.');
  if (pos != std::string::npos) {
    return filename.substr(0, pos);
  }
  return filename;
}

void memory_info_t::release(void) {
  filename.clear();
  size = 0;
  if (data) {
    m5gfx::heap_free(data);
    data = nullptr;
  }
}

//-------------------------------------------------------------------------

bool storage_sd_t::beginStorage(void)
{
  if (_is_begin) { return true; }

  spi_lock();

#if __has_include(<SdFat.h>)
  SdSpiConfig spiConfig(M5.getPin(m5::pin_name_t::sd_spi_cs), SHARED_SPI, SD_SCK_MHZ(25), &SPI);
  _is_begin = SD.begin(spiConfig);
#elif __has_include(<SD.h>)
  _is_begin = SD.begin(M5.getPin(m5::pin_name_t::sd_spi_cs));
#else
  _is_begin = true;
#endif

  spi_unlock();

  if (!_is_begin) {
    system_registry->popup_notify.setPopup(false, def::notify_type_t::NOTIFY_STORAGE_OPEN);
    M5_LOGE("SD card mount failed");
  } else {
    for (auto& dm : dir_manage) {
      if (dm.getStorage() == this) {
        makeDirectory(dm.makeFullPath("").c_str());
      }
    }
  }

  return _is_begin;
}
void storage_sd_t::endStorage(void)
{
  if (!_is_begin) { return; }

  spi_lock();

  _is_begin = false;
#if __has_include(<SdFat.h>)
  SD.end();
#elif __has_include(<SD.h>)
  SD.end();
#else
#endif
  spi_unlock();
}

int storage_sd_t::getFileSize(const char* path)
{
  int res = -1;
  spi_lock();
#if __has_include(<SdFat.h>)
  if (SD.exists(path)) {
    auto file = SD.open(path, O_READ);
    if (file) {
      res = file.fileSize();
      file.close();
    }
  }
#elif __has_include (<SD.h>)
  if (SD.exists(path)) {
    auto file = SD.open(path, FILE_READ);
    if (file) {
      res = file.size();
      file.close();
    }
  }
#else
  if (path[0] == '/') { ++path; }
  if (std::filesystem::exists(path)) {
    res = std::filesystem::file_size(path);
  }
#endif
  spi_unlock();
  return res;
}

int storage_sd_t::loadFromFileToMemory(const char* path, uint8_t* dst, size_t max_length)
{
  if (!_is_begin) { return -1; }

  spi_lock();

  int len = -1;
#if __has_include(<SdFat.h>)
  auto file = SD.open(path, O_READ);
  if (file != false) {
    len = file.dataLength();
    if (len) {
      if (len > max_length) {
        len = max_length;
      }
      len = file.read(dst, len);
    }
    file.close();
  }

#elif __has_include (<SD.h>)
  auto file = SD.open(path, FILE_READ);
  if (file) {
    len = file.size();
    if (len) {
      if (len > max_length) {
        len = max_length;
      }
      len = file.read(dst, len);
    }
    file.close();
  }

#else
  if (path[0] == '/') { ++path; }
  auto FP = fopen(path, "r");
M5_LOGV("sd:loadFromFileToMemory : %s  open:%d\n", path, FP != nullptr);
  if (FP) {
    fseek(FP, 0, SEEK_END);
    len = ftell(FP);
    if (len) {
      if (len > max_length) {
        len = max_length;
      }
      fseek(FP, 0, SEEK_SET);
      len = fread(dst, 1, len, FP);
    }
    fclose(FP);
  }
#endif
  spi_unlock();
  return len;
}

int storage_sd_t::saveFromMemoryToFile(const char* path, const uint8_t* data, size_t length)
{
  if (!_is_begin) { return -1; }

  int result = -1;
  spi_lock();
#if __has_include(<SdFat.h>)
  auto file = SD.open(path, O_CREAT | O_WRITE | O_TRUNC);
  if (file) {
    result = file.write(data, length);
  
    auto now = time(nullptr);
    auto tm = gmtime(&now);
    file.timestamp(T_CREATE|T_WRITE, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
  
    file.close();
  }

#elif __has_include (<SD.h>)
  auto file = SD.open(path, FILE_WRITE);
  if (file) {
    result = file.write(data, length);
    file.close();
  }

#else
  if (path[0] == '/') { ++path; }
  auto FP = fopen(path, "w");
  if (FP) {
    result = fwrite(data, 1, length, FP);
    fclose(FP);
  }
#endif
  spi_unlock();
  return result;
}

int storage_sd_t::getFileList(std::vector<file_info_string_t>& list, const char* path, const char* suffix)
{
  if (!_is_begin) { return -1; }

  file_info_string_t info;

  int result = -1;

  const size_t len_suffix = suffix ? strlen(suffix) : 0;

  spi_lock();
#if __has_include(<SdFat.h>)
  auto dir = SD.open(path);
  if (false != dir) {
    if (dir.isDirectory()) {
      FsFile file;
      while (false != (file = dir.openNextFile())) {
        char filename[256];
        file.getName(filename, sizeof(filename));
        // mac系不可視ファイル無視
        if (memcmp(filename, "._", 2) == 0) { continue; }
        auto len_name = strlen(filename);
        if ( len_name < len_suffix) continue;
        if (len_suffix > 0) {
          if (strcmp(&filename[len_name - len_suffix], suffix) != 0) {
            continue;
          }
        }
        info.filename = filename;
        info.filesize = file.fileSize();
        list.push_back( info );
  //M5_LOGV("file %s %d", filename, info.filesize);
      }
      dir.close();
    } else {
      info.filename = "";
      info.filesize = dir.fileSize();
      list.push_back( info );
    }
    result = list.size();
  }
#elif __has_include (<SD.h>)
  auto dir = SD.open(path);
  if (dir) {
    if (dir.isDirectory()) {
      fs::File file;
      while (false != (file = dir.openNextFile())) {
        info.filename = file.name();
        info.filesize = file.size();
        list.push_back( info );
      }
      dir.close();
    }
    result = list.size();
  }
#else

  if (path[0] == '/') { ++path; }
  bool hit = std::filesystem::exists(path);
  M5_LOGD("exists:%d , %s", hit, path);
  if (hit) {
    hit = std::filesystem::is_directory(path);
    M5_LOGD("is_dir:%d , %s", hit, path);
    if (hit) {
      for (const auto& file : std::filesystem::directory_iterator(path)) {
        // list.push_back({file.path().filename().u8string().c_str(), file.file_size()});
        auto name = file.path().filename().string();
        auto len_name = name.length();
        if ( len_name < len_suffix) continue;
        if (len_suffix > 0) {
          if (strcmp(&name[len_name - len_suffix], suffix) != 0) {
            continue;
          }
        }
        size_t size = file.file_size();
        list.push_back({file.path().filename().string(), size});
      }
    } else {
      size_t size = std::filesystem::file_size(path);
      list.push_back({ "", size });
M5_LOGD("file size:%d , %s", size, path);
    }
    result = list.size();
  }
#endif

  spi_unlock();
  if (!list.empty()) {
    std::sort(list.begin(), list.end(), [](const file_info_string_t& a, const file_info_string_t& b) { return a.filename < b.filename; });
  }
  return result;
}

bool storage_sd_t::makeDirectory(const char* path)
{
  bool res = false;
  spi_lock();
#if __has_include (<SdFat.h>)
  res = SD.mkdir(path);
#elif __has_include (<SD.h>)
  res = SD.mkdir(path);
#else
  if (path[0] == '/') { ++path; }
  res = std::filesystem::create_directory(path);
#endif
  spi_unlock();
  return res;
}

bool storage_sd_t::removeFile(const char* path)
{
  bool res = false;
  spi_lock();
#if __has_include (<SdFat.h>)
  res = SD.remove(path);
#elif __has_include (<SD.h>)
  res = SD.remove(path);
#else
  if (path[0] == '/') { ++path; }
  res = std::filesystem::remove(path);
#endif
  spi_unlock();
  return res;
}

bool storage_sd_t::renameFile(const char* path, const char* newpath)
{
  return false;
}

//-------------------------------------------------------------------------

bool storage_littlefs_t::beginStorage(void)
{
  if (_is_begin) { return true; }

#if __has_include(<LittleFS.h>)
  _is_begin = LittleFS.begin(true);
#else
  _is_begin = true;
#endif

  if (!_is_begin) {
    system_registry->popup_notify.setPopup(false, def::notify_type_t::NOTIFY_STORAGE_OPEN);
    M5_LOGE("LittleFS mount failed");
  }

  return _is_begin;
}
void storage_littlefs_t::endStorage(void)
{
  if (!_is_begin) { return; }
  _is_begin = false;
#if __has_include(<LittleFS.h>)
  LittleFS.end();
#else
#endif
}

int storage_littlefs_t::getFileSize(const char* path)
{
  int res = -1;
#if __has_include(<LittleFS.h>)
  if (LittleFS.exists(path)) {
    auto file = LittleFS.open(path);
    if (file) {
      res = file.size();
      file.close();
    }
  }
#else
  if (path[0] == '/') { ++path; }
  if (std::filesystem::exists(path)) {
    res = std::filesystem::file_size(path);
  }
#endif
  return res;
}

int storage_littlefs_t::loadFromFileToMemory(const char* path, uint8_t* dst, size_t max_length)
{
  if (!_is_begin) { return -1; }

  int len = -1;
#if __has_include(<LittleFS.h>)
  bool exists = LittleFS.exists(path);
  M5_LOGD("LittleFS open:%s exists:%d", path, exists);
  if (!LittleFS.exists(path)) { return len; }
  auto file = LittleFS.open(path);
  if (!file) { return len; }
  len = file.size();
  if (len) {
    if (len > max_length) {
      len = max_length;
    }
    len = file.read(dst, len);
  }
  file.close();
#else
  if (path[0] == '/') { ++path; }
  auto FP = fopen(path, "r");
M5_LOGV("littlefs:loadFromFileToMemory : %s  open:%d", path, FP != nullptr);
  if (!FP) { return len; }
  fseek(FP, 0, SEEK_END);
  len = ftell(FP);
  if (len) {
    if (len > max_length) {
      len = max_length;
    }
    fseek(FP, 0, SEEK_SET);
    len = fread(dst, 1, len, FP);
  }
  fclose(FP);
#endif

  return len;
}

int storage_littlefs_t::saveFromMemoryToFile(const char* path, const uint8_t* data, size_t length)
{
  if (!_is_begin) { return -1; }

  const char* tmpfile = "/.tmpsave.tmp";
  size_t writelen = 0;
#if __has_include(<LittleFS.h>)
  // 一旦テンポラリファイルに保存する。
  auto file = LittleFS.open(tmpfile, FILE_WRITE, true);
  if (!file) {
    return -1;
  }
  writelen = file.write(data, length);
  file.close();
  taskYIELD();
  // 元のファイルを削除してリネームする。
  LittleFS.remove(path);
  LittleFS.rename(tmpfile, path);

#else
  if (path[0] == '/') { ++path; }
  auto FP = fopen(path, "w");
  if (!FP) { return -1; }
  writelen = fwrite(data, 1, length, FP);
  fclose(FP);
#endif

  return writelen;
}

int storage_littlefs_t::getFileList(std::vector<file_info_string_t>& list, const char* path, const char* suffix)
{
  if (!_is_begin) { return -1; }

  file_info_string_t info;

  const size_t len_suffix = suffix ? strlen(suffix) : 0;

#if __has_include(<LittleFS.h>)
  if (LittleFS.exists(path)) {
M5_LOGV("LittleFS check exists:%s found", path);
    auto dir = LittleFS.open(path);
    if (false == dir) { return -1; }
    if (dir.isDirectory()) {
      fs::File file;
      while (false != (file = dir.openNextFile())) {
        info.filename = file.name();
        info.filesize = file.size();
        auto len_name = info.filename.length();
        if (len_name < len_suffix) continue;
        if (len_suffix > 0) {
          if (strcmp(&info.filename[len_name - len_suffix], suffix) != 0) {
            continue;
          }
        }
        list.push_back( info );
M5_LOGV("file %s %d", info.filename.c_str(), info.filesize);
      }
      dir.close();
    } else {
      info.filename = "";
      info.filesize = dir.size();
      list.push_back( info );
    }
  } else {
    M5_LOGV("LittleFS check exists:%s not found", path);
  }
#else

if (path[0] == '/' && path[1] != '\0') { ++path; }
bool result = std::filesystem::exists(path);
M5_LOGD("exists:%d , %s", result, path);
  if (result) {
    result = std::filesystem::is_directory(path);
M5_LOGD("is_dir:%d , %s", result, path);
    if (result) {
      const char* p = (path[0] == '/') ? "./" : path;
      for (const auto& file : std::filesystem::directory_iterator(p)) {
        if (std::filesystem::is_directory(file)) {
          M5_LOGD(" skip dir:%s", file.path().filename().string().c_str());
          continue;
        }
        // list.push_back({file.path().filename().u8string().c_str(), file.file_size()});

        auto name = file.path().filename().string();
        auto len_name = name.length();
        if ( len_name < len_suffix) continue;
        if (len_suffix > 0) {
          if (strcmp(&name[len_name - len_suffix], suffix) != 0) {
            continue;
          }
        }
M5_LOGD("file found:%s", file.path().filename().string().c_str());

        size_t size = file.file_size();
        list.push_back({file.path().filename().string(), size});
      }
    } else {
      size_t size = std::filesystem::file_size(path);
      list.push_back({ "", size });
M5_LOGD("file size:%d , %s", size, path);
    }
  }
#endif

  return list.size();
}

bool storage_littlefs_t::makeDirectory(const char* path)
{
  return false;
}

bool storage_littlefs_t::removeFile(const char* path)
{
  bool res = false;
#if __has_include(<LittleFS.h>)
  res = LittleFS.remove(path);
#else
  if (path[0] == '/') { ++path; }
  res = std::filesystem::remove(path);
#endif
  return res;
}

bool storage_littlefs_t::renameFile(const char* path, const char* newpath)
{
  return false;
}

//-------------------------------------------------------------------------

bool storage_incbin_t::beginStorage(void)
{
  return true;
}

void storage_incbin_t::endStorage(void)
{
}

int storage_incbin_t::getFileSize(const char* path)
{
  for (size_t i = 0; i < _file_count; ++i) {
    if (strcmp(_files[i].filename, path) == 0) {
      return _files[i].size;
    }
  }
  return -1;
}

int storage_incbin_t::loadFromFileToMemory(const char* path, uint8_t* dst, size_t max_length)
{
  for (size_t i = 0; i < _file_count; ++i) {
    if (strcmp(_files[i].filename, path) == 0) {
      auto size = _files[i].size;
      if (size > max_length) { size = max_length; }
      memcpy(dst, _files[i].data, size);
      return size;
    }
  }
  return -1;
}

int storage_incbin_t::saveFromMemoryToFile(const char* path, const uint8_t* data, size_t length)
{
  return -1;
}

int storage_incbin_t::getFileList(std::vector<file_info_string_t>& list, const char* path, const char* suffix)
{
  for (size_t i = 0; i < _file_count; ++i) {
    file_info_string_t info;
    info.filename = _files[i].filename;
    info.filesize = _files[i].size;
    list.push_back(info);
  }
  return list.size();
}

//-------------------------------------------------------------------------

bool dir_manage_t::updateFileList(void)
{
  _file_list_count = 0;
  if (_storage == nullptr) { return false; }
  std::vector<file_info_string_t> list;
  int result = _storage->getFileList(list, _path, def::app::fileext_song);
  if (result < 0) {
    _storage->endStorage();
    _storage->beginStorage();
    result = _storage->getFileList(list, _path, def::app::fileext_song);
  }

  if (result <= 0) {
    if (_file_list) {
      m5gfx::heap_free(_file_list);
      _file_list = nullptr;
    }
    if (_all_filenames) {
      m5gfx::heap_free(_all_filenames);
      _all_filenames = nullptr;
    }
    // マイナス値の場合はエラーとみなしてfalseを返す。
    return result == 0;
  }

  // 以下、得られたデータを std::vector から PSRAM 上のバッファに変換して格納する。
  {
    // ファイルリスト構造体配列をPSRAMに確保する。
    auto file_list = (file_info_t*)m5gfx::heap_alloc_psram(sizeof(file_info_t) * list.size());
    if (file_list == nullptr) {
      M5_LOGE("dir_manage_t::update:heap_alloc_psram failed. size:%d", sizeof(file_info_t) * list.size());
      return false;
    }

    // ファイル名をヌル区切りで連結したバッファを作成する。
    std::vector<char> all_filenames;
    file_info_t* p = file_list;
    for (auto& file : list) {
      p->filename = (char*)all_filenames.size();
      p->filesize = file.filesize;
      ++p;
      all_filenames.insert(all_filenames.end(), file.filename.begin(), file.filename.end());
      all_filenames.insert(all_filenames.end(), 2, '\0'); // ヌル終端
M5_LOGV("dir_manage_t::update file:%s size:%d", file.filename.c_str(), file.filesize);
    }

    // バッファをPSRAMにコピーする。
    char* psram_buffer = (char*)m5gfx::heap_alloc_psram(all_filenames.size());
    if (psram_buffer == nullptr) {
      M5_LOGE("dir_manage_t::update:heap_alloc_psram failed. size:%d", all_filenames.size());
      m5gfx::heap_free(file_list);
      return false;
    }
    memcpy(psram_buffer, all_filenames.data(), all_filenames.size());
    // ファイル名ポインタをPSRAM上のアドレスに変換する。
    p = file_list;
    for (size_t i = 0; i < list.size(); ++i) {
      p->filename = psram_buffer + (size_t)(p->filename);
      ++p;
    }

    auto prev_file_list = _file_list;
    _file_list = file_list;
    _file_list_count = list.size();
    auto prev_all_filenames = _all_filenames;
    _all_filenames = psram_buffer;
    if (prev_file_list) {
      m5gfx::heap_free(prev_file_list);
    }
    if (prev_all_filenames) {
      m5gfx::heap_free(prev_all_filenames);
    }
  }

  return true;
}

int dir_manage_t::search(const char* filename) const
{
  for (size_t i = 0; i < _file_list_count; ++i) {
    if (strcmp(_file_list[i].filename, filename) == 0) {
      return i;
    }
  }
  return -1;
}

std::string dir_manage_t::getFullPath(size_t index)
{
  if (index >= _file_list_count) { return ""; }
  return std::string(_path) + _file_list[index].filename;
}

std::string dir_manage_t::makeFullPath(const char* filename) const
{
  return std::string(_path) + filename;
}

//-------------------------------------------------------------------------
dir_manage_t* file_manage_t::getDirManage(def::app::data_type_t dir_type)
{
  assert(dir_type < def::app::data_type_t::data_type_max && "dir_type is out of range");
  return &dir_manage[dir_type];
}

const file_info_t* file_manage_t::getFileInfo(def::app::data_type_t dir_type, size_t index)
{
  return dir_manage[dir_type].getInfo(index);
}

bool file_manage_t::updateFileList(def::app::data_type_t dir_type)
{
  M5_LOGV("updateFileList");
  auto dir = getDirManage(dir_type);
  if (dir == nullptr) { return false; }
  return dir->updateFileList();
}

void file_manage_t::setLatestFileInfo(def::app::data_type_t data_type, const char* filename)
{
  if (data_type == def::app::data_type_t::data_song_preset_genre
   || data_type == def::app::data_type_t::data_song_preset_song
   || data_type == def::app::data_type_t::data_song_users
   || data_type == def::app::data_type_t::data_song_extra) {
    _latest_data_type = data_type;
    _latest_file_name = (filename != nullptr) ? filename : "";
    _display_file_name = trimExtension(_latest_file_name);
    _latest_file_index = -1;
    auto dir = getDirManage(data_type);
    if (dir != nullptr) {
      if (dir->isEmpty()) {
        updateFileList(data_type);
      }
      _latest_file_index = dir->search(_latest_file_name.c_str());
    }
  }
}

memory_info_t* file_manage_t::createMemoryInfo(size_t length)
{
  if ((int32_t)length < 0) { return nullptr; }
  auto new_queue_index = (_load_queue_index + 1) % max_memory_info;

  _memory_info[new_queue_index].release();

  auto tmp = (uint8_t*)m5gfx::heap_alloc_psram(length);
  if (tmp == nullptr) {
    M5_LOGE("getLoadMemory:heap_alloc_psram failed. size:%d", length);
    return nullptr;
  }
  _memory_info[new_queue_index].data = tmp;
  _memory_info[new_queue_index].size = length;
  _load_queue_index = new_queue_index;
  return &_memory_info[new_queue_index];
}

memory_info_t* file_manage_t::loadFile(def::app::data_type_t dir_type, size_t index)
{
  auto dir = getDirManage(dir_type);
  if (dir == nullptr) { return nullptr; }
  if (index >= dir->getCount()) {
    dir->updateFileList();
  }
  if ((int16_t)index < 0) { // マイナス指定されている場合は末尾側として扱えるようにindexを加算する
    index = ((int16_t)index) + dir->getCount();
  }
  if (index < dir->getCount())
  {
    auto info = dir->getInfo(index);
    // M5_LOGV("file_manage_t::loadFile : index:%d %s", index, info->filename);
    return loadFile(dir_type, info->filename);
  }
  return nullptr;
}

memory_info_t* file_manage_t::loadFile(def::app::data_type_t dir_type, const char* file_name)
{
  auto dir = getDirManage(dir_type);
  if (dir == nullptr) { return nullptr; }
  auto storage = dir->getStorage();
  if (storage == nullptr) { return nullptr; }

  if (storage->isBegin() == false) { storage->beginStorage(); }
  auto fullpath = dir->makeFullPath(file_name);
  auto filesize = storage->getFileSize(fullpath.c_str());
// M5_LOGV("file_manage_t::loadFile : %s size:%d", fullpath.c_str(), filesize);
  if (filesize > 0) {
    auto memory = createMemoryInfo(filesize);
// M5_LOGV("memory: create size:%d  result:%p", filesize, memory);
    if (memory != nullptr) {
      memory->dir_type = dir_type;
      memory->filename = file_name;
      if (0 <= storage->loadFromFileToMemory(fullpath.c_str(), memory->data, memory->size)) {
M5_LOGV(" loaded:%s size:%d", fullpath.c_str(), memory->size);
        return memory;
      }
      memory->release();
    }
  }
M5_LOGE(" load failed:%s", fullpath.c_str());
  return nullptr;
}

bool file_manage_t::saveFile(def::app::data_type_t dir_type, size_t memory_index)
{
  auto mem = getMemoryInfoByIndex(memory_index);
  auto dir = getDirManage(dir_type);
  auto st = dir->getStorage();

  if (mem == nullptr) {
    return false;
  }
  if (mem->data[0] != '{') {
    return false;
  }

  auto path = dir->makeFullPath(mem->filename.c_str());
  auto result = st->saveFromMemoryToFile(path.c_str(), mem->data, mem->size);
  if (result != mem->size) {
    st->endStorage();
    st->beginStorage();
    result = st->saveFromMemoryToFile(path.c_str(), mem->data, mem->size);
  }
// M5_LOGV("save:%s size:%d result:%d", path.c_str(), mem->size, result);

  if (result != mem->size) {
    return false;
  }

  return true;
}

bool file_manage_t::removeFile(def::app::data_type_t dir_type, const char* filename)
{
  auto dir = getDirManage(dir_type);
  if (dir == nullptr) { return false; }
  auto storage = dir->getStorage();
  if (storage == nullptr) { return false; }
  auto fullpath = dir->makeFullPath(filename);
  return storage->removeFile(fullpath.c_str());
}
//-------------------------------------------------------------------------
}; // namespace kanplay_ns
