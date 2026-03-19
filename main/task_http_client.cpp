// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#include <M5Unified.h>

#include "task_http_client.hpp"

#include "system_registry.hpp"

#if defined (M5UNIFIED_PC_BUILD)
namespace kanplay_ns {

void task_http_client_t::task_func(task_http_client_t* me)
{}

};
#else

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_event.h>
#include <esp_ota_ops.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <string.h>

#define HASH_LEN 32

namespace kanplay_ns {
//-------------------------------------------------------------------------

static char* _http_dst = nullptr;
static size_t _http_dst_remain = 0;
static esp_err_t _http_client_event_handler(esp_http_client_event_t *evt)
{
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (evt->data_len) {
      if (_http_dst_remain < evt->data_len) {
        evt->data_len = _http_dst_remain;
      }
      memcpy(_http_dst, evt->data, evt->data_len);
      _http_dst += evt->data_len;
      _http_dst_remain -= evt->data_len;
    }
  }
  return ESP_OK;
}

static esp_err_t execHttpClient(const char* url, char* data, const size_t length)
{
  _http_dst_remain = length;
  _http_dst = data;

  esp_http_client_config_t config;
  memset(&config, 0, sizeof(esp_http_client_config_t));
  config.url = url;
  config.event_handler = _http_client_event_handler;
  config.keep_alive_enable = true;
  config.buffer_size = 1024;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.skip_cert_common_name_check = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    M5_LOGE("HTTP client perform failed: %s (0x%x)", esp_err_to_name(err), err);
  } else {
    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);
    M5_LOGI("HTTP status=%d, content_length=%d, received=%d", status, content_len, (int)(_http_dst - data));
  }
  esp_http_client_close(client);
  return err;
}

// 初期値として3MBを設定
static size_t ota_content_length = 1024*1024*3; // 3MB
static size_t ota_received_length = 0;

static esp_err_t _http_ota_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        M5_LOGD("HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        M5_LOGD("HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        M5_LOGD("HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        M5_LOGD("HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        if (0 == strncmp(evt->header_key, "Content-Length", 14))
        {
          ota_content_length = atoi(evt->header_value);
          M5_LOGV("Content-Length: %d", ota_content_length);
          ota_received_length = 0;
        }
        break;
    case HTTP_EVENT_ON_DATA:
        // M5_LOGD("HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        ota_received_length += evt->data_len;
        if (ota_content_length < ota_received_length) {
          ota_content_length = ota_received_length;
        }
        system_registry->runtime_info.setWiFiOtaProgress(ota_content_length ? ota_received_length * 100 / ota_content_length : 0);
        break;
    case HTTP_EVENT_ON_FINISH:
        M5_LOGD("HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        M5_LOGD("HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        M5_LOGD("HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

static TaskHandle_t _httpcl_task_handle = nullptr;

void task_http_client_t::start(void)
{
  if (_httpcl_task_handle == nullptr) {
    xTaskCreatePinnedToCore((TaskFunction_t)task_func, "httpcl", 4096, this, def::system::task_priority_wifi, &_httpcl_task_handle, def::system::task_cpu_wifi);
  }
}

// リダイレクトを手動解決して最終URLを取得する（ヘッダバッファ蓄積を防止）
static bool resolve_redirects(char* url, size_t url_length)
{
  for (int retry = 0; retry < 5; ++retry) {
    esp_http_client_config_t config;
    memset(&config, 0, sizeof(esp_http_client_config_t));
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = true;
    config.disable_auto_redirect = true;
    config.method = HTTP_METHOD_HEAD;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) { return false; }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && (status == 301 || status == 302)) {
      // Locationヘッダから新しいURLを取得
      esp_http_client_set_redirection(client);
      err = esp_http_client_get_url(client, url, (int)url_length);
      esp_http_client_cleanup(client);
      if (err != ESP_OK) { return false; }
      M5_LOGI("Redirect to: %s", url);
      continue;
    }

    esp_http_client_cleanup(client);
    return (err == ESP_OK && status == 200);
  }
  return false;
}

static esp_err_t exec_http_ota(const char* binary_url)
{
  M5_LOGI("Starting OTA example task");
  esp_http_client_config_t config;
  memset(&config, 0, sizeof(esp_http_client_config_t));

  config.url = binary_url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.event_handler = _http_ota_event_handler;
  config.keep_alive_enable = true;
  config.buffer_size = 1024;
  config.skip_cert_common_name_check = true;

  esp_https_ota_config_t ota_config;
  memset(&ota_config, 0, sizeof(esp_https_ota_config_t));
  ota_received_length = 0;

  ota_config.http_config = &config;

  M5_LOGI("Attempting to download update from %s", config.url);
  return esp_https_ota(&ota_config);
}

static def::command::wifi_ota_state_t exec_get_binary_url(const char* json_url, char* data, const size_t length)
{
  _http_dst = data;
  _http_dst_remain = length;
  auto http_err = execHttpClient(json_url, data, length);
  if (ESP_OK != http_err) {
    M5_LOGE("execHttpClient failed: %s (0x%x)", esp_err_to_name(http_err), http_err);
    return def::command::wifi_ota_state_t::ota_connection_error;
  }

  size_t received = length - _http_dst_remain;
  M5_LOGI("HTTP response received: %d bytes", (int)received);
  if (received < length) {
    data[received] = 0;  // null terminate
  }

  ArduinoJson::JsonDocument json;
  auto error = deserializeJson(json, data);
  data[0] = 0;

  if (error) {
    M5_LOGE("JSON parse failed: %s", error.c_str());
    return def::command::wifi_ota_state_t::ota_connection_error;
  }

  auto firmware_array = json["firmware"].as<JsonArray>();
  auto array_size = firmware_array.size();
  M5_LOGI("firmware count:%d", array_size);
  if (array_size == 0) {
    M5_LOGE("firmware array is empty");
    return def::command::wifi_ota_state_t::ota_connection_error;
  }

  const char* target_type = "release";
  if (system_registry->runtime_info.getDeveloperMode()) {
    target_type = "develop";
  }

#if defined ( CONFIG_IDF_TARGET_ESP32S3 )
  const char* board_name = "cores3";
#else
  const char* board_name = "core2";
#endif
  M5_LOGI("target_type=%s, board=%s", target_type, board_name);

  for (int i = 0; i < array_size; ++i) {
    auto type = firmware_array[i]["type"].as<const char*>();
    auto ver = firmware_array[i]["ver"].as<const char*>();
    auto url_list = firmware_array[i]["url"].as<JsonObject>();

    M5_LOGI("[%d] type=%s, ver=%s", i, type ? type : "(null)", ver ? ver : "(null)");

    // ターゲットタイプが同じか確認
    bool target_check = (type != nullptr && 0 == strcmp(target_type, type));
    if (target_check) {
      auto url = url_list[board_name].as<const char*>();
      M5_LOGI("matched: url=%s", url ? url : "(null)");
      if (url != nullptr) {
        strncpy(data, url, length);
        // バージョンが今と一致しているか確認
        bool version_check = (0 == strcmp(def::app::app_version_string, ver));
        M5_LOGI("version: current=%s, server=%s, match=%d", def::app::app_version_string, ver, version_check);
        if (version_check) {
          return def::command::wifi_ota_state_t::ota_already_up_to_date;
        }
        return def::command::wifi_ota_state_t::ota_update_available;
      }
    }
  }
  M5_LOGE("No matching firmware found for target=%s board=%s", target_type, board_name);
  return def::command::wifi_ota_state_t::ota_connection_error;
}

static void exec_ota_inner(const char* json_url)
{
  static constexpr const size_t MAX_HTTP_OUTPUT_BUFFER = 1024 * 4;
  auto local_response_buffer = (char*)m5gfx::heap_alloc_psram(MAX_HTTP_OUTPUT_BUFFER + 1);
  if (local_response_buffer == nullptr) {
    M5_LOGE("Failed to allocate PSRAM buffer for OTA");
    system_registry->runtime_info.setWiFiOtaProgress(def::command::wifi_ota_state_t::ota_connection_error);
    return;
  }
  {
    auto state = exec_get_binary_url(json_url, local_response_buffer, MAX_HTTP_OUTPUT_BUFFER);
    system_registry->runtime_info.setWiFiOtaProgress(state);
  
    if (state == def::command::wifi_ota_state_t::ota_update_available) {
      // リダイレクトを事前解決して最終URLを取得（ヘッダバッファ蓄積を防止）
      resolve_redirects(local_response_buffer, MAX_HTTP_OUTPUT_BUFFER);
      auto ret = exec_http_ota(local_response_buffer);
      system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_disable);
      if (ret == ESP_OK) {
        system_registry->runtime_info.setWiFiOtaProgress(def::command::wifi_ota_state_t::ota_update_done);
        // M5.delay(1024);
        // OTA完了後に本体リセット
        system_registry->operator_command.addQueue( { def::command::system_control, def::command::system_control_t::sc_reset } );
      } else {
        system_registry->runtime_info.setWiFiOtaProgress(def::command::wifi_ota_state_t::ota_connection_error);
        M5_LOGE("Firmware upgrade failed");
      }
    }
    m5gfx::heap_free(local_response_buffer);
  }
  }

void task_http_client_t::exec_ota(const char* json_url)
{
  _ota_json_url = json_url;
  start();
  _request = request_ota;
  xTaskNotify(_httpcl_task_handle, request_ota, eSetValueWithOverwrite);
}

void task_http_client_t::task_func(task_http_client_t* me)
{
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    auto request = me->_request;
    me->_request = request_none;
    switch (request) {
    case request_ota:
      exec_ota_inner(me->_ota_json_url);
      break;

    default:
      break;
    }
  }
}

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
