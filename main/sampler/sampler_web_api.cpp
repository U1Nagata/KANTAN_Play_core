// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#if defined(KANPLAY_SAMPLER) && !defined(M5UNIFIED_PC_BUILD)

#include <M5Unified.h>

#include <esp_heap_caps.h>
#include <esp_http_server.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../file_manage.hpp"
#include "sampler_web.hpp"
#include "sampler_web_api.hpp"

namespace sampler_ns {
namespace {

struct web_dir_t {
  const char* token;
  const char* path;
  const char* suffix;
  size_t max_bytes;
  const char* content_type;
};

static constexpr const web_dir_t web_dirs[] = {
  { "samples", "/sampler/samples", ".wav", 3200 * 1024, "audio/wav" },
  { "loops",   "/sampler/loops",   ".wav", 1600 * 1024, "audio/wav" },
  { "kits",    "/sampler/kits",    ".json", 128 * 1024, "application/json" },
};

static esp_err_t send_error(httpd_req_t* req, const char* status, const char* message)
{
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  char body[96];
  snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message ? message : "error");
  return httpd_resp_sendstr(req, body);
}

static std::string url_decode(const char* text, size_t length)
{
  std::string out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    char c = text[i];
    if (c == '%' && i + 2 < length) {
      unsigned int value = 0;
      if (sscanf(text + i + 1, "%2x", &value) == 1) {
        out.push_back((char)value);
        i += 2;
        continue;
      }
    }
    out.push_back(c == '+' ? ' ' : c);
  }
  return out;
}

static bool valid_relative_path(const std::string& name, const char* suffix, bool allow_empty = false)
{
  if (name.empty()) { return allow_empty; }
  if (name.size() > 120 || name[0] == '/' || name.find('\\') != std::string::npos || name.find("..") != std::string::npos) { return false; }
  for (size_t start = 0; start < name.size();) {
    size_t end = name.find('/', start);
    if (end == std::string::npos) { end = name.size(); }
    if (end == start || name[start] == '.') { return false; }
    start = end + 1;
  }
  if (!suffix || !suffix[0]) { return true; }
  size_t sl = strlen(suffix);
  return name.size() > sl && strcasecmp(name.c_str() + name.size() - sl, suffix) == 0;
}

static const web_dir_t* parse_dir_and_name(httpd_req_t* req, std::string* name)
{
  static constexpr const char prefix[] = "/api/sampler/files/";
  const char* path = req->uri;
  if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) { return nullptr; }
  path += sizeof(prefix) - 1;
  const char* end = strchr(path, '?');
  size_t path_len = end ? (size_t)(end - path) : strlen(path);
  for (const auto& dir : web_dirs) {
    size_t token_len = strlen(dir.token);
    if (path_len < token_len || memcmp(path, dir.token, token_len) != 0) { continue; }
    if (path_len == token_len) {
      if (name) { name->clear(); }
      return &dir;
    }
    if (path[token_len] != '/') { continue; }
    std::string decoded = url_decode(path + token_len + 1, path_len - token_len - 1);
    if (!valid_relative_path(decoded, dir.suffix)) { return nullptr; }
    if (name) { *name = decoded; }
    return &dir;
  }
  return nullptr;
}

static bool ensure_dirs()
{
  if (!kanplay_ns::storage_sd.beginStorage()) { return false; }
  kanplay_ns::storage_sd.makeDirectory("/sampler");
  for (const auto& dir : web_dirs) { kanplay_ns::storage_sd.makeDirectory(dir.path); }
  return true;
}

static std::string full_path(const web_dir_t& dir, const std::string& name)
{
  return std::string(dir.path) + "/" + name;
}

static bool query_path(httpd_req_t* req, std::string& out)
{
  out.clear();
  size_t length = httpd_req_get_url_query_len(req);
  if (length == 0) { return true; }
  std::vector<char> query(length + 1, 0);
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) { return false; }
  char raw[128] = {};
  if (httpd_query_key_value(query.data(), "path", raw, sizeof(raw)) != ESP_OK) { return true; }
  out = url_decode(raw, strlen(raw));
  return valid_relative_path(out, nullptr, true);
}

static esp_err_t list_files(httpd_req_t* req, const web_dir_t& dir)
{
  if (!ensure_dirs()) { return send_error(req, "503 Service Unavailable", "SD card unavailable"); }
  std::string relative;
  if (!query_path(req, relative)) { return send_error(req, "400 Bad Request", "invalid folder path"); }
  std::string current = full_path(dir, relative);
  std::vector<kanplay_ns::file_info_string_t> files;
  kanplay_ns::storage_sd.getFileList(files, current.c_str(), "");
  httpd_resp_set_type(req, "application/json");
  char header[180];
  snprintf(header, sizeof(header), "{\"path\":\"%s\",\"files\":[", relative.c_str());
  httpd_resp_sendstr_chunk(req, header);
  bool first = true;
  for (const auto& file : files) {
    if (!valid_relative_path(file.filename, dir.suffix)) { continue; }
    char item[180];
    snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"size\":%u}", first ? "" : ",", file.filename.c_str(), (unsigned)file.filesize);
    httpd_resp_sendstr_chunk(req, item);
    first = false;
  }
  httpd_resp_sendstr_chunk(req, "]}");
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static const web_dir_t* parse_folder_dir(httpd_req_t* req)
{
  static constexpr const char prefix[] = "/api/sampler/folders/";
  const char* token = req->uri;
  if (strncmp(token, prefix, sizeof(prefix) - 1) != 0) { return nullptr; }
  token += sizeof(prefix) - 1;
  const char* end = strchr(token, '?');
  size_t len = end ? (size_t)(end - token) : strlen(token);
  for (const auto& dir : web_dirs) {
    if (strlen(dir.token) == len && memcmp(dir.token, token, len) == 0) { return &dir; }
  }
  return nullptr;
}

static esp_err_t response_folders(httpd_req_t* req)
{
  sampler_web_note_client_access();
  if (!sampler_web_prepare_storage_operation()) {
    return send_error(req, "503 Service Unavailable", "audio stop timeout");
  }
  const auto* dir = parse_folder_dir(req);
  if (!dir) { return send_error(req, "404 Not Found", "unknown sampler directory"); }
  if (!ensure_dirs()) { return send_error(req, "503 Service Unavailable", "SD card unavailable"); }
  std::string relative;
  if (!query_path(req, relative)) { return send_error(req, "400 Bad Request", "invalid folder path"); }
  if (req->method == HTTP_POST) {
    size_t length = httpd_req_get_url_query_len(req);
    std::vector<char> query(length + 1, 0);
    char raw[96] = {};
    if (length == 0 || httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK
     || httpd_query_key_value(query.data(), "name", raw, sizeof(raw)) != ESP_OK) { return send_error(req, "400 Bad Request", "folder name required"); }
    std::string name = url_decode(raw, strlen(raw));
    if (!valid_relative_path(name, nullptr) || name.find('/') != std::string::npos) { return send_error(req, "400 Bad Request", "invalid folder name"); }
    std::string created = full_path(*dir, relative);
    created += "/" + name;
    if (!kanplay_ns::storage_sd.makeDirectory(created.c_str())) { return send_error(req, "409 Conflict", "folder create failed"); }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
  }
  std::vector<kanplay_ns::file_info_string_t> folders;
  std::string current = full_path(*dir, relative);
  kanplay_ns::storage_sd.getDirectoryList(folders, current.c_str());
  httpd_resp_set_type(req, "application/json");
  char header[180];
  snprintf(header, sizeof(header), "{\"path\":\"%s\",\"folders\":[", relative.c_str());
  httpd_resp_sendstr_chunk(req, header);
  for (size_t i = 0; i < folders.size(); ++i) {
    char item[140];
    snprintf(item, sizeof(item), "%s\"%s\"", i ? "," : "", folders[i].filename.c_str());
    httpd_resp_sendstr_chunk(req, item);
  }
  httpd_resp_sendstr_chunk(req, "]}");
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t get_file(httpd_req_t* req, const web_dir_t& dir, const std::string& name)
{
  if (!ensure_dirs()) { return send_error(req, "503 Service Unavailable", "SD card unavailable"); }
  std::string path = full_path(dir, name);
  int size = kanplay_ns::storage_sd.getFileSize(path.c_str());
  if (size <= 0 || (size_t)size > dir.max_bytes) { return send_error(req, "404 Not Found", "file not found"); }
  auto* data = (uint8_t*)heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
  if (!data) { return send_error(req, "500 Internal Server Error", "memory unavailable"); }
  int len = kanplay_ns::storage_sd.loadFromFileToMemory(path.c_str(), data, (size_t)size);
  if (len != size) { free(data); return send_error(req, "500 Internal Server Error", "read failed"); }
  httpd_resp_set_type(req, dir.content_type);
  httpd_resp_set_hdr(req, "Content-Disposition", name.c_str());
  esp_err_t result = httpd_resp_send(req, (const char*)data, len);
  free(data);
  return result;
}

static esp_err_t put_file(httpd_req_t* req, const web_dir_t& dir, const std::string& name)
{
  if (!ensure_dirs()) { return send_error(req, "503 Service Unavailable", "SD card unavailable"); }
  if (req->content_len == 0 || (size_t)req->content_len > dir.max_bytes) { return send_error(req, "413 Payload Too Large", "file too large"); }
  auto* data = (uint8_t*)heap_caps_malloc(req->content_len, MALLOC_CAP_SPIRAM);
  if (!data) { return send_error(req, "500 Internal Server Error", "memory unavailable"); }
  size_t read = 0;
  while (read < (size_t)req->content_len) {
    int got = httpd_req_recv(req, (char*)data + read, req->content_len - read);
    if (got <= 0) { free(data); return send_error(req, "400 Bad Request", "upload failed"); }
    read += got;
  }
  if ((strcmp(dir.suffix, ".wav") == 0 && (read < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0))
   || (strcmp(dir.suffix, ".json") == 0 && data[0] != '{')) {
    free(data);
    return send_error(req, "400 Bad Request", "invalid file format");
  }
  std::string path = full_path(dir, name);
  int written = kanplay_ns::storage_sd.saveFromMemoryToFile(path.c_str(), data, read);
  free(data);
  if (written != (int)read) { return send_error(req, "500 Internal Server Error", "save failed"); }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

static esp_err_t delete_file(httpd_req_t* req, const web_dir_t& dir, const std::string& name)
{
  if (!ensure_dirs()) { return send_error(req, "503 Service Unavailable", "SD card unavailable"); }
  std::string path = full_path(dir, name);
  if (!kanplay_ns::storage_sd.removeFile(path.c_str())) { return send_error(req, "404 Not Found", "delete failed"); }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

static esp_err_t rename_file(httpd_req_t* req, const web_dir_t& dir, const std::string& name)
{
  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len == 0 || query_len > 256) { return send_error(req, "400 Bad Request", "new name required"); }
  std::vector<char> query(query_len + 1, 0);
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) { return send_error(req, "400 Bad Request", "invalid query"); }
  char target_raw[128] = {};
  if (httpd_query_key_value(query.data(), "to", target_raw, sizeof(target_raw)) != ESP_OK) { return send_error(req, "400 Bad Request", "new name required"); }
  std::string target = url_decode(target_raw, strlen(target_raw));
  if (!valid_relative_path(target, dir.suffix) || target == name || !ensure_dirs()) { return send_error(req, "400 Bad Request", "invalid file name"); }
  std::string source_path = full_path(dir, name);
  std::string target_path = full_path(dir, target);
  if (kanplay_ns::storage_sd.getFileSize(target_path.c_str()) >= 0) { return send_error(req, "409 Conflict", "file already exists"); }
  if (!kanplay_ns::storage_sd.renameFile(source_path.c_str(), target_path.c_str())) { return send_error(req, "500 Internal Server Error", "rename failed"); }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"ok\"}");
}

static esp_err_t response_files(httpd_req_t* req)
{
  sampler_web_note_client_access();
  if (!sampler_web_prepare_storage_operation()) {
    return send_error(req, "503 Service Unavailable", "audio stop timeout");
  }
  std::string name;
  const auto* dir = parse_dir_and_name(req, &name);
  if (!dir) { return send_error(req, "404 Not Found", "unknown sampler directory"); }
  if (name.empty()) { return req->method == HTTP_GET ? list_files(req, *dir) : send_error(req, "405 Method Not Allowed", "file name required"); }
  if (req->method == HTTP_GET) { return get_file(req, *dir, name); }
  if (req->method == HTTP_PUT) { return put_file(req, *dir, name); }
  if (req->method == HTTP_DELETE) { return delete_file(req, *dir, name); }
  if (req->method == HTTP_POST) { return rename_file(req, *dir, name); }
  return send_error(req, "405 Method Not Allowed", "method not allowed");
}

static esp_err_t response_state(httpd_req_t* req)
{
  sampler_web_note_client_access();
  std::string json;
  if (!sampler_web_export_state(json)) { return send_error(req, "500 Internal Server Error", "state unavailable"); }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), json.size());
}

static esp_err_t response_audio(httpd_req_t* req)
{
  sampler_web_note_client_access();
  static constexpr const char prefix[] = "/api/sampler/audio/";
  const char* key = req->uri + sizeof(prefix) - 1;
  bool background = strcmp(key, "background.wav") == 0;
  uint8_t pad = 0;
  if (!background && (sscanf(key, "pad/%hhu.wav", &pad) != 1 || pad >= 12)) {
    return send_error(req, "404 Not Found", "unknown audio");
  }
  sampler_web_audio_t audio;
  if (!sampler_web_get_audio(background, pad, audio)) { return send_error(req, "404 Not Found", "audio unavailable"); }
  struct wav_header_t {
    char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmt_size;
    uint16_t format; uint16_t channels; uint32_t rate; uint32_t byte_rate;
    uint16_t align; uint16_t bits; char data[4]; uint32_t data_size;
  } __attribute__((packed));
  const uint32_t bytes = audio.frames * sizeof(int16_t);
  wav_header_t header = {{'R','I','F','F'}, 36 + bytes, {'W','A','V','E'}, {'f','m','t',' '}, 16,
    1, 1, audio.sample_rate, audio.sample_rate * 2, 2, 16, {'d','a','t','a'}, bytes};
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_send_chunk(req, (const char*)&header, sizeof(header));
  const uint8_t* data = (const uint8_t*)audio.pcm;
  for (uint32_t offset = 0; offset < bytes; ) {
    size_t chunk = std::min<uint32_t>(4096, bytes - offset);
    if (httpd_resp_send_chunk(req, (const char*)data + offset, chunk) != ESP_OK) { break; }
    offset += chunk;
  }
  return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t response_command(httpd_req_t* req)
{
  sampler_web_note_client_access();
  if (req->content_len == 0 || req->content_len > 32 * 1024) { return send_error(req, "413 Payload Too Large", "command too large"); }
  std::vector<uint8_t> body(req->content_len);
  size_t read = 0;
  while (read < body.size()) {
    int got = httpd_req_recv(req, (char*)body.data() + read, body.size() - read);
    if (got <= 0) { return send_error(req, "400 Bad Request", "request failed"); }
    read += got;
  }
  if (!sampler_web_enqueue_command(body.data(), body.size())) { return send_error(req, "409 Conflict", "sampler is busy"); }
  httpd_resp_set_status(req, "202 Accepted");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"result\":\"queued\"}");
}

static constexpr const httpd_uri uri_table[] = {
  { "/api/sampler/files/*", HTTP_GET,    response_files,   nullptr, false, false, nullptr },
  { "/api/sampler/files/*", HTTP_PUT,    response_files,   nullptr, false, false, nullptr },
  { "/api/sampler/files/*", HTTP_DELETE, response_files,   nullptr, false, false, nullptr },
  { "/api/sampler/files/*", HTTP_POST,   response_files,   nullptr, false, false, nullptr },
  { "/api/sampler/folders/*", HTTP_GET,  response_folders, nullptr, false, false, nullptr },
  { "/api/sampler/folders/*", HTTP_POST, response_folders, nullptr, false, false, nullptr },
  { "/api/sampler/state",   HTTP_GET,    response_state,   nullptr, false, false, nullptr },
  { "/api/sampler/audio/*", HTTP_GET,    response_audio,   nullptr, false, false, nullptr },
  { "/api/sampler/command", HTTP_POST,   response_command, nullptr, false, false, nullptr },
};

} // namespace

void sampler_web_api_register_uris(httpd_handle_t server)
{
  if (!server) { return; }
  for (const auto& uri : uri_table) { httpd_register_uri_handler(server, &uri); }
}

void sampler_web_api_unregister_uris(httpd_handle_t server)
{
  if (!server) { return; }
  for (const auto& uri : uri_table) { httpd_unregister_uri_handler(server, uri.uri, uri.method); }
}

} // namespace sampler_ns

#endif
