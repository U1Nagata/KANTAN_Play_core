// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#include <M5Unified.h>

#if !defined(M5UNIFIED_PC_BUILD)

#include <string.h>
#include <string>
#include <vector>

#include <esp_http_server.h>

#include "task_wifi/task_wifi_api.hpp"

#include "system_registry.hpp"
#include "file_manage.hpp"

// =====================================================================
// /api/... エンドポイント
// L1: /api/files/<dir>[/<filename>]   ファイルCRUD
// L2: /api/song                       現在のライブソング GET/PUT
//     /api/song/save?dir=&name=       現在のライブソングをファイル保存
//     /api/progression                現在のライブ進行データ GET/PUT
//     /api/progression/save?...       現在の進行データをファイル保存
//
// 注意:
// - 書き込み可能ディレクトリは api_dir_table のホワイトリストに限る
// - ファイル名は ".." やスラッシュを禁止し、必ず拡張子チェック
// - file_manage は内部で PSRAM をローテーションするため、レスポンス送信
//   完了までバッファが解放されないことを前提にしている
// - SDカードへのI/Oは task_operator など他タスクと競合しうる。今は許容
// =====================================================================

namespace kanplay_ns {
namespace {

// %エンコードを戻す。task_wifi.cpp 側にも同名の関数があるが、内部結合のため複製する。
std::string url_decode(const std::string& str) {
  std::string decoded;
  for (size_t i = 0; i < str.length(); ++i) {
    char ch = str[i];
    if (ch == '%' && i + 2 < str.length()) {
      int v = 0;
      sscanf(str.substr(i + 1, 2).c_str(), "%x", &v);
      ch = static_cast<char>(v);
      i += 2;
    } else if (ch == '+') {
      ch = ' ';
    }
    decoded += ch;
  }
  return decoded;
}

struct api_dir_entry_t {
  const char* token;                  // URLトークン (例: "songs/user")
  def::app::data_type_t dir_type;
  const char* required_ext;           // 必須拡張子 (例: ".json")
  bool writable;                      // PUT/DELETE 可否
};
constexpr const api_dir_entry_t api_dir_table[] = {
  { "songs/user",       def::app::data_type_t::data_song_users,        ".json", true  },
  { "songs/extra",      def::app::data_type_t::data_song_extra,        ".json", true  },
  { "arpeggio/user",    def::app::data_type_t::data_arpeggio_users,    ".json", true  },
  { "progression/user", def::app::data_type_t::data_progression_users, ".json", true  },
};

// パスから dir エントリを longest match する。一致した場合の token 長を out_token_len に書く。
const api_dir_entry_t* api_match_dir(const char* path, size_t path_len, size_t& out_token_len)
{
  const api_dir_entry_t* best = nullptr;
  size_t best_len = 0;
  for (auto& e : api_dir_table) {
    size_t tl = strlen(e.token);
    if (path_len >= tl
     && memcmp(path, e.token, tl) == 0
     && (path_len == tl || path[tl] == '/')) {
      if (tl > best_len) {
        best = &e;
        best_len = tl;
      }
    }
  }
  out_token_len = best_len;
  return best;
}

// ファイル名のバリデーション。拡張子の一致を要求する。
bool api_is_valid_filename(const char* name, const char* required_ext)
{
  if (name == nullptr || name[0] == 0) return false;
  size_t n = strlen(name);
  // FAT LFN 上限 255 に対し安全マージンを取る (data_path のディレクトリ分を見越す)
  if (n > 240) return false;
  if (name[0] == '.' || name[0] == '/') return false;
  for (size_t i = 0; i < n; ++i) {
    char c = name[i];
    if (c == '/' || c == '\\') return false;
    if (c == '.' && i + 1 < n && name[i + 1] == '.') return false;
  }
  if (required_ext != nullptr) {
    size_t el = strlen(required_ext);
    if (n < el) return false;
    if (strcasecmp(name + n - el, required_ext) != 0) return false;
  }
  return true;
}

esp_err_t api_send_json_error(httpd_req_t* req, const char* status, const char* message)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"error\":\"");
  if (message) httpd_resp_sendstr_chunk(req, message);
  httpd_resp_sendstr_chunk(req, "\"}");
  httpd_resp_sendstr_chunk(req, nullptr);
  return ESP_OK;
}

// JSON エスケープして送る (制御文字/" /\ のみ。SDのファイル名想定で十分)
void api_send_json_string(httpd_req_t* req, const char* s)
{
  httpd_resp_sendstr_chunk(req, "\"");
  const char* p = s;
  const char* start = s;
  for (; *p; ++p) {
    char c = *p;
    if (c == '"' || c == '\\' || (uint8_t)c < 0x20) {
      if (p > start) httpd_resp_send_chunk(req, start, p - start);
      char buf[8];
      if (c == '"' || c == '\\') {
        buf[0] = '\\'; buf[1] = c; buf[2] = 0;
      } else {
        snprintf(buf, sizeof(buf), "\\u%04x", c);
      }
      httpd_resp_sendstr_chunk(req, buf);
      start = p + 1;
    }
  }
  if (p > start) httpd_resp_send_chunk(req, start, p - start);
  httpd_resp_sendstr_chunk(req, "\"");
}

// content_len バイトを PSRAM 上の memory_info に読み込む。
// 成功時は memory_info_t* を返し、mem->size に実際の読み込みバイト数を入れる。
// 失敗時は nullptr を返し、リソース解放と HTTP エラー応答を済ませる。
memory_info_t* api_recv_body(httpd_req_t* req)
{
  size_t total = req->content_len;
  if (total == 0 || total > def::app::max_file_len) {
    api_send_json_error(req, "413 Payload Too Large", "size out of range");
    return nullptr;
  }
  auto mem = file_manage.createMemoryInfo(total + 1);
  if (mem == nullptr || mem->data == nullptr) {
    api_send_json_error(req, "500 Internal Server Error", "alloc failed");
    return nullptr;
  }
  size_t read = 0;
  while (read < total) {
    int ret = httpd_req_recv(req, (char*)mem->data + read, total - read);
    if (ret <= 0) {
      mem->release();
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        api_send_json_error(req, "408 Request Timeout", "recv timeout");
      } else {
        api_send_json_error(req, "400 Bad Request", "recv failed");
      }
      return nullptr;
    }
    read += (size_t)ret;
  }
  mem->data[total] = 0;
  mem->size = total;
  return mem;
}

// GET /api/files/<dir>  → 一覧
esp_err_t api_files_list(httpd_req_t* req, const api_dir_entry_t* dir)
{
  file_manage.updateFileList(dir->dir_type);
  auto* dm = file_manage.getDirManage(dir->dir_type);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"dir\":\"");
  httpd_resp_sendstr_chunk(req, dir->token);
  httpd_resp_sendstr_chunk(req, "\",\"files\":[");
  if (dm != nullptr) {
    size_t count = dm->getCount();
    for (size_t i = 0; i < count; ++i) {
      auto info = dm->getInfo(i);
      if (info == nullptr || info->filename == nullptr) continue;
      httpd_resp_sendstr_chunk(req, (i == 0) ? "{\"name\":" : ",{\"name\":");
      api_send_json_string(req, info->filename);
      char numbuf[32];
      snprintf(numbuf, sizeof(numbuf), ",\"size\":%u}", (unsigned)info->filesize);
      httpd_resp_sendstr_chunk(req, numbuf);
    }
  }
  httpd_resp_sendstr_chunk(req, "]}");
  httpd_resp_sendstr_chunk(req, nullptr);
  return ESP_OK;
}

// GET /api/files/<dir>/<name>  → ファイル中身
esp_err_t api_files_get(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
{
  auto mem = file_manage.loadFile(dir->dir_type, filename);
  if (mem == nullptr) {
    return api_send_json_error(req, "404 Not Found", "file not found");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)mem->data, mem->size);
}

// PUT /api/files/<dir>/<name>  → 保存
esp_err_t api_files_put(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
{
  if (!dir->writable) {
    return api_send_json_error(req, "403 Forbidden", "directory is read-only");
  }
  auto mem = api_recv_body(req);
  if (mem == nullptr) return ESP_OK; // recv 内でエラー応答済み

  // 拡張子に応じた最低限のフォーマット検査 (.json は '{' で始まること)
  if (strcasecmp(dir->required_ext, ".json") == 0) {
    if (mem->data[0] != '{') {
      mem->release();
      return api_send_json_error(req, "400 Bad Request", "invalid json");
    }
  }
  mem->dir_type = dir->dir_type;
  mem->filename = filename;
  bool ok = file_manage.saveFile(dir->dir_type, mem->index);
  if (!ok) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "save failed");
  }
  // 一覧キャッシュを更新しておく
  file_manage.updateFileList(dir->dir_type);
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// POST /api/files/<dir>/<old>/rename?to=<new>
//   同一 dir 内のリネーム。to が既存なら 409。to の拡張子は old と同じ要件 (=dir->required_ext)。
esp_err_t api_files_rename(httpd_req_t* req, const api_dir_entry_t* dir, const char* old_name)
{
  if (!dir->writable) {
    return api_send_json_error(req, "403 Forbidden", "directory is read-only");
  }
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen > 1024) {
    return api_send_json_error(req, "400 Bad Request", "missing query");
  }
  std::vector<char> qbuf(qlen + 1, 0);
  if (httpd_req_get_url_query_str(req, qbuf.data(), qbuf.size()) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "bad query");
  }
  char to_buf[256] = {};
  if (httpd_query_key_value(qbuf.data(), "to", to_buf, sizeof(to_buf)) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "missing 'to'");
  }
  std::string new_name = url_decode(to_buf);
  if (!api_is_valid_filename(new_name.c_str(), dir->required_ext)) {
    return api_send_json_error(req, "400 Bad Request", "invalid 'to' filename");
  }
  if (new_name == old_name) {
    return api_send_json_error(req, "400 Bad Request", "same name");
  }
  // 上書き禁止: to が既存なら 409
  file_manage.updateFileList(dir->dir_type);
  auto* dm = file_manage.getDirManage(dir->dir_type);
  if (dm != nullptr && dm->search(new_name.c_str()) >= 0) {
    return api_send_json_error(req, "409 Conflict", "destination exists");
  }
  // 元ファイルが無い場合のチェック (renameFile は失敗時に false を返すが、原因の切り分け用)
  if (dm != nullptr && dm->search(old_name) < 0) {
    return api_send_json_error(req, "404 Not Found", "source not found");
  }
  bool ok = file_manage.renameFile(dir->dir_type, old_name, new_name.c_str());
  if (!ok) {
    return api_send_json_error(req, "500 Internal Server Error", "rename failed");
  }
  // ライブで参照中のファイルなら latest 情報を新名に追従
  if (file_manage.getLatestDataType() == dir->dir_type
   && file_manage.getLatestFileName() == old_name) {
    file_manage.setLatestFileInfo(dir->dir_type, new_name.c_str());
  }
  file_manage.updateFileList(dir->dir_type);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// DELETE /api/files/<dir>/<name>
esp_err_t api_files_delete(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
{
  if (!dir->writable) {
    return api_send_json_error(req, "403 Forbidden", "directory is read-only");
  }
  bool ok = file_manage.removeFile(dir->dir_type, filename);
  if (!ok) {
    return api_send_json_error(req, "404 Not Found", "delete failed");
  }
  file_manage.updateFileList(dir->dir_type);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// /api/files/* 共通ディスパッチ
esp_err_t response_api_files_handler(httpd_req_t* req)
{
  static constexpr const char prefix[] = "/api/files/";
  static constexpr const size_t prefix_len = sizeof(prefix) - 1;
  if (strncmp(req->uri, prefix, prefix_len) != 0) {
    return api_send_json_error(req, "404 Not Found", "bad path");
  }
  const char* path = req->uri + prefix_len;
  const char* qmark = strchr(path, '?');
  size_t path_len = qmark ? (size_t)(qmark - path) : strlen(path);

  size_t token_len = 0;
  auto dir = api_match_dir(path, path_len, token_len);
  if (dir == nullptr) {
    return api_send_json_error(req, "404 Not Found", "unknown directory");
  }

  // <token> 部のみか、<token>/<filename> か
  if (path_len == token_len) {
    if (req->method != HTTP_GET) {
      return api_send_json_error(req, "405 Method Not Allowed", "use GET on directory");
    }
    return api_files_list(req, dir);
  }

  // path[token_len] は '/' であることが api_match_dir で保証されている
  const char* fname_start = path + token_len + 1;
  size_t fname_len = path_len - token_len - 1;

  // POST + 末尾 "/rename" はリネームアクション扱い
  static constexpr const char rename_suffix[] = "/rename";
  static constexpr const size_t rename_suffix_len = sizeof(rename_suffix) - 1;
  bool is_rename = (req->method == HTTP_POST)
                && (fname_len > rename_suffix_len)
                && (memcmp(fname_start + fname_len - rename_suffix_len,
                           rename_suffix, rename_suffix_len) == 0);
  if (is_rename) {
    fname_len -= rename_suffix_len;
  }

  std::string fname_raw(fname_start, fname_len);
  std::string fname = url_decode(fname_raw);
  if (!api_is_valid_filename(fname.c_str(), dir->required_ext)) {
    return api_send_json_error(req, "400 Bad Request", "invalid filename");
  }
  if (is_rename) {
    return api_files_rename(req, dir, fname.c_str());
  }
  switch (req->method) {
  case HTTP_GET:    return api_files_get(req, dir, fname.c_str());
  case HTTP_PUT:    return api_files_put(req, dir, fname.c_str());
  case HTTP_DELETE: return api_files_delete(req, dir, fname.c_str());
  default:
    return api_send_json_error(req, "405 Method Not Allowed", "method not allowed");
  }
}

// GET /api/song  現在のライブソングを JSON で返す
esp_err_t response_api_song_get_handler(httpd_req_t* req)
{
  auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
  if (mem == nullptr || mem->data == nullptr) {
    return api_send_json_error(req, "500 Internal Server Error", "alloc failed");
  }
  size_t len = system_registry->song_data.saveSongJSON(mem->data, def::app::max_file_len);
  if (len == 0) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "serialize failed");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)mem->data, len);
}

// PUT /api/song  受け取ったソングデータをライブに反映する
//                file_load_notify と同じ経路でtask_operatorに委譲し、
//                安全に backup_song_data → song_data へ差し替える
esp_err_t response_api_song_put_handler(httpd_req_t* req)
{
  auto mem = api_recv_body(req);
  if (mem == nullptr) return ESP_OK;

  if (mem->data[0] != '{') {
    mem->release();
    return api_send_json_error(req, "400 Bad Request", "invalid json");
  }
  // data_song_blank として扱うことで、既存のロード処理に乗せつつ
  // ファイル名を「new_xxx」にリセットする (匿名アップロード扱い)
  mem->dir_type = def::app::data_type_t::data_song_blank;
  mem->filename = "";
  system_registry->operator_command.addQueue(
    { def::command::file_load_notify, (int)mem->index } );

  httpd_resp_set_status(req, "202 Accepted");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"queued\"}");
}

// GET /api/progression  現在のライブ進行データを Progression JSON で返す
esp_err_t response_api_progression_get_handler(httpd_req_t* req)
{
  auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
  if (mem == nullptr || mem->data == nullptr) {
    return api_send_json_error(req, "500 Internal Server Error", "alloc failed");
  }
  size_t len = system_registry->saveProgressionJSON(mem->data, def::app::max_file_len);
  if (len == 0) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "serialize failed");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)mem->data, len);
}

// PUT /api/progression  受け取った Progression JSON でライブの進行データを差し替える
//   menu_data の load 経路と同じく loadProgressionJSON を直接呼ぶ
//   (副作用として再生位置が 0 にリセットされる)
esp_err_t response_api_progression_put_handler(httpd_req_t* req)
{
  auto mem = api_recv_body(req);
  if (mem == nullptr) return ESP_OK;

  if (mem->data[0] != '{') {
    mem->release();
    return api_send_json_error(req, "400 Bad Request", "invalid json");
  }
  bool ok = system_registry->loadProgressionJSON(mem->data, mem->size);
  mem->release();
  if (!ok) {
    return api_send_json_error(req, "400 Bad Request", "load failed");
  }
  system_registry->checkSongModified();
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// POST /api/progression/save?dir=progression/user&name=foo.json
//   現在の進行データをファイルに保存する
esp_err_t response_api_progression_save_handler(httpd_req_t* req)
{
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen > 1024) {
    return api_send_json_error(req, "400 Bad Request", "missing query");
  }
  std::vector<char> qbuf(qlen + 1, 0);
  if (httpd_req_get_url_query_str(req, qbuf.data(), qbuf.size()) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "bad query");
  }
  char dir_buf[32] = {};
  char name_buf[256] = {};
  if (httpd_query_key_value(qbuf.data(), "dir",  dir_buf,  sizeof(dir_buf))  != ESP_OK
   || httpd_query_key_value(qbuf.data(), "name", name_buf, sizeof(name_buf)) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "missing dir/name");
  }
  std::string dir_token = url_decode(dir_buf);
  std::string name      = url_decode(name_buf);

  size_t token_len = 0;
  auto dir = api_match_dir(dir_token.c_str(), dir_token.size(), token_len);
  if (dir == nullptr || token_len != dir_token.size() || !dir->writable) {
    return api_send_json_error(req, "400 Bad Request", "invalid dir");
  }
  // 進行データ専用ディレクトリのみ許可
  if (dir->dir_type != def::app::data_type_t::data_progression_users) {
    return api_send_json_error(req, "400 Bad Request", "dir is not for progression");
  }
  if (!api_is_valid_filename(name.c_str(), dir->required_ext)) {
    return api_send_json_error(req, "400 Bad Request", "invalid filename");
  }

  auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
  if (mem == nullptr || mem->data == nullptr) {
    return api_send_json_error(req, "500 Internal Server Error", "alloc failed");
  }
  size_t len = system_registry->saveProgressionJSON(mem->data, def::app::max_file_len);
  if (len == 0) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "serialize failed");
  }
  mem->size = len;
  mem->dir_type = dir->dir_type;
  mem->filename = name;
  bool ok = file_manage.saveFile(dir->dir_type, mem->index);
  if (!ok) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "save failed");
  }
  file_manage.updateFileList(dir->dir_type);

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

// POST /api/song/save?dir=songs/user&name=foo.json
//   現在のライブソングをファイルに保存する
esp_err_t response_api_song_save_handler(httpd_req_t* req)
{
  // クエリ文字列を抽出
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen > 1024) {
    return api_send_json_error(req, "400 Bad Request", "missing query");
  }
  std::vector<char> qbuf(qlen + 1, 0);
  if (httpd_req_get_url_query_str(req, qbuf.data(), qbuf.size()) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "bad query");
  }
  char dir_buf[32] = {};
  char name_buf[256] = {};
  if (httpd_query_key_value(qbuf.data(), "dir",  dir_buf,  sizeof(dir_buf))  != ESP_OK
   || httpd_query_key_value(qbuf.data(), "name", name_buf, sizeof(name_buf)) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "missing dir/name");
  }
  std::string dir_token = url_decode(dir_buf);
  std::string name      = url_decode(name_buf);

  size_t token_len = 0;
  auto dir = api_match_dir(dir_token.c_str(), dir_token.size(), token_len);
  if (dir == nullptr || token_len != dir_token.size() || !dir->writable) {
    return api_send_json_error(req, "400 Bad Request", "invalid dir");
  }
  // ソング系ディレクトリのみ許可
  if (dir->dir_type != def::app::data_type_t::data_song_users
   && dir->dir_type != def::app::data_type_t::data_song_extra) {
    return api_send_json_error(req, "400 Bad Request", "dir is not for songs");
  }
  if (!api_is_valid_filename(name.c_str(), dir->required_ext)) {
    return api_send_json_error(req, "400 Bad Request", "invalid filename");
  }

  auto mem = file_manage.createMemoryInfo(def::app::max_file_len);
  if (mem == nullptr || mem->data == nullptr) {
    return api_send_json_error(req, "500 Internal Server Error", "alloc failed");
  }
  size_t len = system_registry->song_data.saveSongJSON(mem->data, def::app::max_file_len);
  if (len == 0) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "serialize failed");
  }
  mem->size = len;
  mem->dir_type = dir->dir_type;
  mem->filename = name;
  bool ok = file_manage.saveFile(dir->dir_type, mem->index);
  if (!ok) {
    mem->release();
    return api_send_json_error(req, "500 Internal Server Error", "save failed");
  }
  file_manage.setLatestFileInfo(dir->dir_type, name.c_str());
  file_manage.updateFileList(dir->dir_type);
  system_registry->updateUnchangedSongCRC32();
  system_registry->checkSongModified();

  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

constexpr const httpd_uri api_uri_table[] = {
  { "/api/files/*", HTTP_GET   , response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/files/*", HTTP_PUT   , response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/files/*", HTTP_DELETE, response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/files/*", HTTP_POST  , response_api_files_handler   , nullptr, false, false, nullptr }, // /rename サフィックス用
  { "/api/song",      HTTP_GET , response_api_song_get_handler , nullptr, false, false, nullptr },
  { "/api/song",      HTTP_PUT , response_api_song_put_handler , nullptr, false, false, nullptr },
  { "/api/song/save", HTTP_POST, response_api_song_save_handler, nullptr, false, false, nullptr },
  { "/api/progression",      HTTP_GET , response_api_progression_get_handler , nullptr, false, false, nullptr },
  { "/api/progression",      HTTP_PUT , response_api_progression_put_handler , nullptr, false, false, nullptr },
  { "/api/progression/save", HTTP_POST, response_api_progression_save_handler, nullptr, false, false, nullptr },
};

} // anonymous namespace

void task_wifi_api_register_uris(httpd_handle_t server)
{
  if (server == nullptr) return;
  for (auto& uri : api_uri_table) {
    httpd_register_uri_handler(server, &uri);
  }
}

void task_wifi_api_unregister_uris(httpd_handle_t server)
{
  if (server == nullptr) return;
  for (auto& uri : api_uri_table) {
    httpd_unregister_uri_handler(server, uri.uri, uri.method);
  }
}

} // namespace kanplay_ns

#endif // !M5UNIFIED_PC_BUILD
