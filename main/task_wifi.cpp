// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#include <M5Unified.h>

#include <string.h>

#include "task_wifi.hpp"

#include "task_http_client.hpp"

#include "system_registry.hpp"

#include "file_manage.hpp"

#if defined (M5UNIFIED_PC_BUILD)
namespace kanplay_ns {

void task_wifi_t::task_func(task_wifi_t* me)
{
  uint8_t counter;
  for (;;) {
    ++counter;
    M5.delay(256);
    auto mode = system_registry->wifi_control.getWifiMode();
    auto op = system_registry->wifi_control.getOperation();

    auto sta_info = def::command::wifi_sta_info_t::wsi_off;
    auto ap_info = def::command::wifi_ap_info_t::wai_off;

    bool sta = false;
    bool ap = false;
    if (mode == def::command::wifi_mode_t::wifi_enable_sta) {
      sta_info = (def::command::wifi_sta_info_t)((3 & (counter >> 3))+1);
    }
    if (op == def::command::wifi_operation_t::wfop_setup_ap) {
      sta_info = def::command::wifi_sta_info_t::wsi_waiting;
      ap_info = (counter & 0x10)
              ? def::command::wifi_ap_info_t::wai_enabled
              : def::command::wifi_ap_info_t::wai_waiting;
      system_registry->runtime_info.setWiFiStationCount((counter & 0x10) ? 1 : 0);
    }
    if (op == def::command::wifi_operation_t::wfop_setup_wps) {
      sta_info = def::command::wifi_sta_info_t::wsi_waiting;
    }
    if (op == def::command::wifi_operation_t::wfop_ota_begin) {
      system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_ota_progress);
    }
    if (op == def::command::wifi_operation_t::wfop_ota_progress) {
      sta_info = (def::command::wifi_sta_info_t)((3 & (counter >> 3))+1);
      int progress = counter & 0x7f;
      if (progress > 101) { progress = 101; }
      system_registry->runtime_info.setWiFiOtaProgress(progress);
    }
    system_registry->runtime_info.setWiFiSTAInfo(sta_info);
    system_registry->runtime_info.setWiFiAPInfo(ap_info);
  }
}

void task_wifi_t::start(void) {
  auto thread = SDL_CreateThread((SDL_ThreadFunction)task_func, "wifi", this);
}
};
#else

#include <esp_wifi.h>
#include <esp_wps.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_netif.h>
#include <esp_event.h>

#include <esp_crt_bundle.h>

#include <mdns.h>
#include <lwip/sockets.h>

#if __has_include (<esp_sntp.h>)
  #include <esp_sntp.h>
  #define SNTP_ENABLED 1
#elif __has_include (<sntp.h>)
  #include <sntp.h>
  #define SNTP_ENABLED 1
#endif



// ファイルインポートマクロ
#define IMPORT_FILE(section, filename, symbol) \
extern const char symbol[], sizeof_##symbol[]; \
asm (\
  ".section " #section "\n"\
  ".balign 4\n"\
  ".global " #symbol "\n"\
  #symbol ":\n"\
  ".incbin \"" filename "\"\n"\
  ".global sizeof_" #symbol "\n"\
  ".set sizeof_" #symbol ", . - " #symbol "\n"\
  ".balign 4\n"\
  ".section \".text\"\n")

IMPORT_FILE(.rodata, "incbin/html/wifi.html", html_wifi);
IMPORT_FILE(.rodata, "incbin/html/main.html", html_main);

namespace kanplay_ns {
//-------------------------------------------------------------------------

// --- WiFi state tracking ---
// イベントハンドラが他タスクから書く軽量フラグ類は wifi_state_t に入れず
// ファイルスコープ static のまま残す（task_wifi_info との競合回避のため）。
enum wifi_sta_state_t : uint8_t {
  STA_STOPPED,
  STA_IDLE,
  STA_CONNECTED,
  STA_DISCONNECTED,
};
static volatile wifi_sta_state_t _sta_state = STA_STOPPED;
static volatile bool _ap_started = false;
static volatile int _ap_station_count = 0;
// -2=idle, -1=scanning, >=0=results ready to be consumed by task loop
static volatile int _scan_status = -2;
// POST /wifi で接続試行を開始した時刻(ms)。0 は未試行。
// grace 期間内の STA_DISCONNECTED は "connecting" として扱い、誤った「接続失敗」表示を抑止する。
static volatile uint32_t _connect_start_ms = 0;
static constexpr uint32_t CONNECT_GRACE_MS = 10000;
// 直近の STA_DISCONNECTED イベントの reason コード (0 = 未発生)
static volatile uint16_t _last_disconnect_reason = 0;

// Wi-Fi 起動時刻からの経過時間を計測するための基準時刻 (タイミングログ用)
static volatile uint32_t _setup_t0_ms = 0;


// パスワード間違い・SSID不一致など、ほぼ確定的な失敗とみなせる reason か判定する。
// これらは grace 期間を待たずに即 "failed" を返す。
static bool is_fatal_disconnect_reason(uint16_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
      return true;
    default:
      return false;
  }
}

// --- SSID scan cache ---
static constexpr uint8_t SSID_CACHE_MAX = 16;
static constexpr uint32_t SSID_CACHE_TTL_MS = 90000; // 90秒見えなかったエントリは捨てる
struct ssid_cache_entry_t {
  char ssid[33];
  int8_t rssi;
  uint32_t last_seen_ms;
};

// --- Dynamically allocated Wi-Fi runtime state ---
// Wi-Fi が有効な間だけ確保し、完全無効化時に解放する。
// これにより WiFi ドライバ本体 (esp_wifi_deinit) と付随リソースが返却される。
struct wifi_state_t {
  // SSID cache
  ssid_cache_entry_t ssid_cache[SSID_CACHE_MAX] = {};
  volatile uint8_t ssid_cache_count = 0;
  SemaphoreHandle_t ssid_cache_mutex = nullptr;
  volatile uint32_t last_scan_done_ms = 0;

  // Wi-Fi driver / netif
  esp_netif_t* sta_netif = nullptr;
  esp_netif_t* ap_netif = nullptr;
  bool wifi_started = false;
  bool wps_enabled = false;

  // Captive portal DNS
  int dns_sock = -1;
  uint32_t dns_target_ip = 0;

  // HTTP server
  httpd_handle_t http_server = nullptr;
};
static wifi_state_t* _ws = nullptr;

static void dns_server_stop() {
  if (!_ws) return;
  if (_ws->dns_sock >= 0) {
    close(_ws->dns_sock);
    _ws->dns_sock = -1;
  }
}

static void dns_server_start(uint32_t ip) {
  if (!_ws) return;
  dns_server_stop();
  _ws->dns_target_ip = ip;
  _ws->dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (_ws->dns_sock < 0) return;
  int flags = fcntl(_ws->dns_sock, F_GETFL, 0);
  fcntl(_ws->dns_sock, F_SETFL, flags | O_NONBLOCK);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = INADDR_ANY;
  bind(_ws->dns_sock, (struct sockaddr*)&addr, sizeof(addr));
}

static void dns_server_process() {
  if (!_ws || _ws->dns_sock < 0) return;
  static constexpr int DNS_MAX_QUERY = 512;
  static constexpr int DNS_ANSWER_SIZE = 16; // A record answer: 2+2+2+4+2+4=16 bytes
  uint8_t buf[DNS_MAX_QUERY + DNS_ANSWER_SIZE];
  struct sockaddr_in client = {};
  socklen_t client_len = sizeof(client);
  int len = recvfrom(_ws->dns_sock, buf, DNS_MAX_QUERY, 0, (struct sockaddr*)&client, &client_len);
  if (len < 12 || len > DNS_MAX_QUERY) return;
  // Build DNS response: set QR=1, keep RD, set ANCOUNT=1
  buf[2] = 0x80 | (buf[2] & 0x01);
  buf[3] = 0x00;
  buf[6] = 0; buf[7] = 1;  // ANCOUNT = 1
  buf[8] = 0; buf[9] = 0;  // NSCOUNT = 0
  buf[10] = 0; buf[11] = 0; // ARCOUNT = 0
  // Append A record answer
  int pos = len;
  buf[pos++] = 0xC0; buf[pos++] = 0x0C; // name pointer to question
  buf[pos++] = 0x00; buf[pos++] = 0x01; // type A
  buf[pos++] = 0x00; buf[pos++] = 0x01; // class IN
  buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x00; buf[pos++] = 0x3C; // TTL 60s
  buf[pos++] = 0x00; buf[pos++] = 0x04; // RDLENGTH 4
  memcpy(&buf[pos], &_ws->dns_target_ip, 4); pos += 4;
  sendto(_ws->dns_sock, buf, pos, 0, (struct sockaddr*)&client, client_len);
}

// --- WiFi initialization ---
static TaskHandle_t _wifi_task_handle = nullptr;
static TaskHandle_t _wifi_info_task_handle = nullptr;

static bool wpsStart() {
  if (!_ws) return false;
  esp_wps_config_t config = {};
  config.wps_type = WPS_TYPE_PBC;
  strncpy(config.factory_info.manufacturer, "ESPRESSIF", sizeof(config.factory_info.manufacturer) - 1);
  strncpy(config.factory_info.model_number, CONFIG_IDF_TARGET, sizeof(config.factory_info.model_number) - 1);
  strncpy(config.factory_info.model_name, "ESPRESSIF IOT", sizeof(config.factory_info.model_name) - 1);
  strncpy(config.factory_info.device_name, "ESP DEVICE", sizeof(config.factory_info.device_name) - 1);
  strncpy(config.pin, "00000000", sizeof(config.pin) - 1);

  esp_err_t err = esp_wifi_wps_enable(&config);
  if (err != ESP_OK) {
    M5_LOGE("WPS Enable Failed: 0x%x: %s", err, esp_err_to_name(err));
    return false;
  }
  err = esp_wifi_wps_start(0);
  if (err != ESP_OK) {
    M5_LOGE("WPS Start Failed: 0x%x: %s", err, esp_err_to_name(err));
    return false;
  }
  _ws->wps_enabled = true;
  return true;
}

static void wpsStop() {
  if (!_ws) return;
  _ws->wps_enabled = false;
  esp_err_t err = esp_wifi_wps_disable();
  if (err != ESP_OK) {
    M5_LOGE("WPS Disable Failed: 0x%x: %s", err, esp_err_to_name(err));
  }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
      _sta_state = STA_IDLE;
      break;
    case WIFI_EVENT_STA_STOP:
      _sta_state = STA_STOPPED;
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      {
        auto* evt = (wifi_event_sta_disconnected_t*)event_data;
        _last_disconnect_reason = evt ? evt->reason : 0;
        _sta_state = STA_DISCONNECTED;
      }
      break;
    case WIFI_EVENT_AP_START:
      _ap_started = true;
      M5_LOGI("[wifi-timing] t=+%lu WIFI_EVENT_AP_START",
              (unsigned long)(M5.millis() - _setup_t0_ms));
      break;
    case WIFI_EVENT_AP_STOP:
      _ap_started = false;
      _ap_station_count = 0;
      break;
    case WIFI_EVENT_AP_STACONNECTED:
      _ap_station_count = _ap_station_count + 1;
      M5_LOGI("[wifi-timing] t=+%lu WIFI_EVENT_AP_STACONNECTED (count=%d)",
              (unsigned long)(M5.millis() - _setup_t0_ms), _ap_station_count);
      break;
    case WIFI_EVENT_AP_STADISCONNECTED:
      if (_ap_station_count > 0) _ap_station_count = _ap_station_count - 1;
      break;
    case WIFI_EVENT_SCAN_DONE:
      {
        uint16_t count = 0;
        esp_wifi_scan_get_ap_num(&count);
        _scan_status = (int)count;
        if (_wifi_task_handle) {
          // タスク側で records 取得＆キャッシュ更新させる
          xTaskNotifyGive(_wifi_task_handle);
        }
      }
      break;
    case WIFI_EVENT_STA_WPS_ER_SUCCESS:
      {
        auto* evt = (wifi_event_sta_wps_er_success_t*)event_data;
        if (evt && evt->ap_cred_cnt > 0) {
          wifi_config_t sta_config = {};
          memcpy(sta_config.sta.ssid, evt->ap_cred[0].ssid, sizeof(sta_config.sta.ssid));
          memcpy(sta_config.sta.password, evt->ap_cred[0].passphrase, sizeof(sta_config.sta.password));
          esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        }
        wpsStop();
        system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_disable);
        system_registry->wifi_control.setWifiMode(def::command::wifi_mode_t::wifi_enable_sta);
      }
      break;
    case WIFI_EVENT_STA_WPS_ER_FAILED:
    case WIFI_EVENT_STA_WPS_ER_TIMEOUT:
      wpsStop();
      if (system_registry->wifi_control.getOperation() == def::command::wifi_operation_t::wfop_setup_wps) {
        wpsStart();
      }
      break;
    default:
      break;
    }
  } else if (event_base == IP_EVENT) {
    if (event_id == IP_EVENT_STA_GOT_IP) {
      _sta_state = STA_CONNECTED;
    }
  }

  if (_wifi_info_task_handle) {
    xTaskNotify(_wifi_info_task_handle, event_id, eSetValueWithOverwrite);
  }
}

// esp_netif_init / esp_event_loop_create_default はプロセスで一度だけ行う。
// (esp_netif_deinit は IDF 側で未サポートのため再 init 不可)
static bool _netif_inited = false;
static void ensure_netif_subsystem() {
  if (_netif_inited) return;
  _netif_inited = true;
  esp_netif_init();
  esp_event_loop_create_default();
}

// Wi-Fi 有効化に伴い wifi_state_t を確保し、ドライバを初期化する。
static void wifi_state_create() {
  if (_ws) return;
  _setup_t0_ms = M5.millis();
  M5_LOGI("[wifi-timing] t=0 wifi_state_create begin");
  ensure_netif_subsystem();

  _ws = new wifi_state_t();
  _ws->ssid_cache_mutex = xSemaphoreCreateMutex();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  // KANTAN Playでは高帯域WiFiは不要(OTAと設定UIのみ)なのでバッファを削減
  cfg.static_rx_buf_num = 4;    // default 8 → 4 (DMA内部RAM、1.6KB/個 → 約6KB節約)
  cfg.dynamic_rx_buf_num = 16;  // default 32 → 16 (PSRAM)
  cfg.rx_ba_win = 4;            // default 6 → 4 (内部RAM、約3KB節約)
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_FLASH);

  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr);

  M5_LOGI("[wifi-timing] t=+%lu wifi_state_create end (driver initialized)",
          (unsigned long)(M5.millis() - _setup_t0_ms));
}

// Wi-Fi 完全無効化。ドライバと netif・イベント購読・オブジェクト本体を解放する。
// 呼び出し元(task_func)は事前に HTTP/mDNS/DNS サーバを停止してから呼ぶこと。
static void wifi_state_destroy() {
  if (!_ws) return;

  // 1) 以後イベントハンドラが動かないようにする
  //    (in-flight ハンドラの終了を内部で待機する)
  esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);

  // 2) Wi-Fi 停止 & ドライバ解放
  if (_ws->wifi_started) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    _ws->wifi_started = false;
  }
  esp_wifi_deinit();

  // 3) netif 破棄
  if (_ws->sta_netif) {
    esp_netif_destroy_default_wifi(_ws->sta_netif);
    _ws->sta_netif = nullptr;
  }
  if (_ws->ap_netif) {
    esp_netif_destroy_default_wifi(_ws->ap_netif);
    _ws->ap_netif = nullptr;
  }

  // 4) ミューテックスとオブジェクト本体の解放
  if (_ws->ssid_cache_mutex) {
    vSemaphoreDelete(_ws->ssid_cache_mutex);
    _ws->ssid_cache_mutex = nullptr;
  }
  delete _ws;
  _ws = nullptr;

  // 参照されうる軽量 static の初期化
  _sta_state = STA_STOPPED;
  _ap_started = false;
  _ap_station_count = 0;
  _scan_status = -2;
}

// netifは使用するモードのものだけ作成する
static void wifi_ensure_sta_netif() {
  if (_ws && !_ws->sta_netif) {
    _ws->sta_netif = esp_netif_create_default_wifi_sta();
  }
}
static void wifi_ensure_ap_netif() {
  if (_ws && !_ws->ap_netif) {
    _ws->ap_netif = esp_netif_create_default_wifi_ap();
  }
}


static esp_err_t response_redirect(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t response_top_handler(httpd_req_t *req)
{
  auto operation = system_registry->wifi_control.getOperation();
  if (operation == def::command::wifi_operation_t::wfop_setup_ap) {
    return response_redirect(req, "/wifi");
  }
  return response_redirect(req, "/main");
}

static esp_err_t response_wifi_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html_wifi, (uint32_t)sizeof_html_wifi);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t response_main_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, html_main, (uint32_t)sizeof_html_main);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

static esp_err_t response_ctrl_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");

  struct btnctrl_t {
    def::command::command_param_t command;
    const char* keycode_array;
  };
  using namespace def::command;
  static constexpr const btnctrl_t btnctrl_table[] = {
    {{internal_button,11}, "qQ7"}, {{internal_button,12}, "wW8"}, {{internal_button,13}, "eE9"}, {{internal_button,14}, "rR" }, {{internal_button,15}, "tT"},
    {{internal_button, 6}, "aA4"}, {{internal_button, 7}, "sS5"}, {{internal_button, 8}, "dD6"}, {{internal_button, 9}, "fF" }, {{internal_button,10}, "gG"},
    {{internal_button, 1}, "zZ1"}, {{internal_button, 2}, "xX2"}, {{internal_button, 3}, "cC3"}, {{internal_button, 4}, "vV0"}, {{internal_button, 5}, "bB"},
  };

  httpd_resp_sendstr_chunk(req,
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">\n"
    "<script>\n"
    "let ct={"
  );

  char linebuf[16];
  for (auto& btn : btnctrl_table) {
    int i = 0;
    do {
      snprintf(linebuf, sizeof(linebuf), "'%c':%d,", btn.keycode_array[i], btn.command.raw);
      httpd_resp_sendstr_chunk(req, linebuf);
    } while (btn.keycode_array[++i]);
    httpd_resp_sendstr_chunk(req, "\n");
  }
  httpd_resp_sendstr_chunk(req,
    "};\n"
    "const ws=new WebSocket('/ws');"
    "document.addEventListener('keydown',function(e){ if(!e.repeat&&e.key in ct){ ws.send('cmd=p'+ct[e.key]); } });\n"
    "document.addEventListener('keyup',function(e){ if(!e.repeat&&e.key in ct){ ws.send('cmd=r'+ct[e.key]); } });\n"
    "</script>\n"
    "</head><body>KEYBOARD CONTROL</body></html>\n"
  );
  httpd_resp_sendstr_chunk(req, nullptr);
  return ESP_OK;
}

// スキャン結果（wifi_ap_record_t）を SSID キャッシュへマージする。
// ・キャッシュはクリアせず、既存エントリに追記・更新する
// ・同一 SSID は最新の RSSI と last_seen_ms に更新
// ・TTL を超えたエントリは捨てる
// ・溢れた場合は (1) 期限切れ (2) 最弱 RSSI の順に捨てる
// ・最後に RSSI 降順で整列
static void ssid_cache_merge(const wifi_ap_record_t* recs, uint16_t count)
{
  if (!_ws || !_ws->ssid_cache_mutex) return;
  uint32_t now = M5.millis();
  if (now == 0) now = 1;
  xSemaphoreTake(_ws->ssid_cache_mutex, portMAX_DELAY);
  auto* cache = _ws->ssid_cache;

  // 1) TTL 経過エントリを除去（前詰め）
  uint8_t w = 0;
  for (uint8_t r = 0; r < _ws->ssid_cache_count; ++r) {
    if ((now - cache[r].last_seen_ms) < SSID_CACHE_TTL_MS) {
      if (w != r) cache[w] = cache[r];
      ++w;
    }
  }
  _ws->ssid_cache_count = w;

  // 2) 新規スキャン結果をマージ
  for (uint16_t i = 0; i < count; ++i) {
    const char* ssid = (const char*)recs[i].ssid;
    if (ssid[0] == 0) continue; // 非公開SSIDはスキップ
    int8_t rssi = recs[i].rssi;

    bool dup = false;
    for (uint8_t j = 0; j < _ws->ssid_cache_count; ++j) {
      if (strncmp(cache[j].ssid, ssid, 32) == 0) {
        cache[j].rssi = rssi;
        cache[j].last_seen_ms = now;
        dup = true;
        break;
      }
    }
    if (dup) continue;

    if (_ws->ssid_cache_count < SSID_CACHE_MAX) {
      auto& e = cache[_ws->ssid_cache_count];
      strncpy(e.ssid, ssid, 32);
      e.ssid[32] = 0;
      e.rssi = rssi;
      e.last_seen_ms = now;
      _ws->ssid_cache_count = _ws->ssid_cache_count + 1;
    } else {
      // 最弱 RSSI のエントリを探して、より強ければ置換
      uint8_t min_idx = 0;
      for (uint8_t j = 1; j < SSID_CACHE_MAX; ++j) {
        if (cache[j].rssi < cache[min_idx].rssi) min_idx = j;
      }
      if (rssi > cache[min_idx].rssi) {
        auto& e = cache[min_idx];
        strncpy(e.ssid, ssid, 32);
        e.ssid[32] = 0;
        e.rssi = rssi;
        e.last_seen_ms = now;
      }
    }
  }

  // 3) RSSI 降順に挿入ソート
  for (uint8_t i = 1; i < _ws->ssid_cache_count; ++i) {
    ssid_cache_entry_t key = cache[i];
    int j = (int)i - 1;
    while (j >= 0 && cache[j].rssi < key.rssi) {
      cache[j + 1] = cache[j];
      --j;
    }
    cache[j + 1] = key;
  }
  xSemaphoreGive(_ws->ssid_cache_mutex);
}

static void ssid_cache_clear(void)
{
  if (!_ws || !_ws->ssid_cache_mutex) return;
  xSemaphoreTake(_ws->ssid_cache_mutex, portMAX_DELAY);
  _ws->ssid_cache_count = 0;
  xSemaphoreGive(_ws->ssid_cache_mutex);
}

static esp_err_t response_ssid_handler(httpd_req_t *req) {
  // その時点で持っているキャッシュを即時返却する（待機しない）。
  // 継続的な再スキャンはタスク側で走っているので、呼び出すたびに最新が返る。
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr_chunk(req, "{\"ssids\":[");

  if (_ws && _ws->ssid_cache_mutex) {
    xSemaphoreTake(_ws->ssid_cache_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < _ws->ssid_cache_count; ++i) {
      // JSON 用に " と \ のみ最低限エスケープ
      const char* s = _ws->ssid_cache[i].ssid;
      httpd_resp_sendstr_chunk(req, (i == 0) ? "\"" : ",\"");
      const char* p = s;
      const char* start = p;
      while (*p) {
        if (*p == '"' || *p == '\\') {
          if (p > start) httpd_resp_send_chunk(req, start, p - start);
          httpd_resp_sendstr_chunk(req, "\\");
          httpd_resp_send_chunk(req, p, 1);
          start = p + 1;
        }
        ++p;
      }
      if (p > start) httpd_resp_send_chunk(req, start, p - start);
      httpd_resp_sendstr_chunk(req, "\"");
    }
    xSemaphoreGive(_ws->ssid_cache_mutex);
  }

  httpd_resp_sendstr_chunk(req, "]}");
  httpd_resp_sendstr_chunk(req, nullptr);
  return ESP_OK;
}

static std::string url_decode(const std::string& str) {
  std::string decoded;
  for (int i = 0; i < str.length(); i++) {
    char ch = str[i];
    if (ch == '%') {
      int ii;
      sscanf(str.substr(i + 1, 2).c_str(), "%x", &ii);
      ch = static_cast<char>(ii);
      i += 2;
    } else if (ch == '+') {
      ch = ' ';
    }
    decoded += ch;
  }
  return decoded;
}

static esp_err_t response_post_wifi_handler(httpd_req_t *req) {
  int ret, len = req->content_len;
  std::string ssid, password;
  esp_err_t res;
  {
    std::vector<char> res_buf (len+1, 0);
    if ((ret = httpd_req_recv(req, res_buf.data(), len)) <= 0) {
      return ESP_FAIL;
    }
    std::vector<char> buf (len+1, 0);
    memset(buf.data(), 0, buf.size());
    res = httpd_query_key_value(res_buf.data(), "s", buf.data(), buf.size());
    if (res == ESP_OK) {
      ssid = url_decode(buf.data());
      memset(buf.data(), 0, buf.size());
      res = httpd_query_key_value(res_buf.data(), "p", buf.data(), buf.size());
      if (res == ESP_OK) {
        password = url_decode(buf.data());
      }
    }
  }

  if (res == ESP_OK) {
    wifi_config_t sta_config = {};
    strncpy((char*)sta_config.sta.ssid, ssid.c_str(), sizeof(sta_config.sta.ssid) - 1);
    strncpy((char*)sta_config.sta.password, password.c_str(), sizeof(sta_config.sta.password) - 1);
    // mode/op は wfop_setup_ap 維持のまま、STA のみ接続試行を開始する。
    // これにより AP/HTTP サーバが落ちず、接続失敗時にユーザが再設定できる。
    uint32_t now = M5.millis();
    _connect_start_ms = now ? now : 1;
    _last_disconnect_reason = 0;
    _sta_state = STA_IDLE;
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();
    return response_redirect(req, "/wifi");
  }
  return ESP_OK;
}

static esp_err_t response_status_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");

  const char* state_str = "unknown";
  switch (_sta_state) {
    case STA_STOPPED:   state_str = "stopped"; break;
    case STA_IDLE:      state_str = "connecting"; break;
    case STA_CONNECTED: state_str = "connected"; break;
    case STA_DISCONNECTED:
    default: {
      // reason が確定的失敗系なら grace を待たず即 failed
      uint16_t reason = _last_disconnect_reason;
      if (is_fatal_disconnect_reason(reason)) {
        state_str = "failed";
      } else {
        uint32_t start = _connect_start_ms;
        uint32_t now = M5.millis();
        if (start != 0 && (now - start) < CONNECT_GRACE_MS) {
          state_str = "connecting";
        } else {
          state_str = "failed";
        }
      }
      break;
    }
  }

  char ssid_buf[66] = {}; // 32文字 × エスケープ最大 2倍 + 終端
  {
    wifi_config_t cfg = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
      const uint8_t* src = cfg.sta.ssid;
      size_t j = 0;
      for (size_t i = 0; i < 32 && src[i] && j + 2 < sizeof(ssid_buf); ++i) {
        char c = (char)src[i];
        if (c == '"' || c == '\\') ssid_buf[j++] = '\\';
        ssid_buf[j++] = c;
      }
      ssid_buf[j] = 0;
    }
  }

  char ip_buf[16] = "";
  if (_ws && _ws->sta_netif) {
    esp_netif_ip_info_t ip_info = {};
    if (esp_netif_get_ip_info(_ws->sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
    }
  }

  // attempted: 一度でも POST /wifi で接続試行を開始したかどうか
  const char* attempted = (_connect_start_ms != 0) ? "true" : "false";

  char out[192];
  int n = snprintf(out, sizeof(out),
    "{\"attempted\":%s,\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\"}",
    attempted, state_str, ssid_buf, ip_buf);
  if (n < 0) n = 0;
  httpd_resp_send(req, out, n);
  return ESP_OK;
}

static esp_err_t response_done_handler(httpd_req_t *req) {
  // サーバ有効化設定をオンにして Setup を終了する
  system_registry->wifi_control.setWebServerMode(def::command::webserver_mode_t::ws_enable);
  system_registry->wifi_control.setOperation(def::command::wifi_operation_t::wfop_disable);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req,
    "<!DOCTYPE html><html><head><meta charset=UTF-8></head>"
    "<body style=\"font-family:sans-serif;text-align:center;padding:10vw\">"
    "<h2>Setup finished</h2></body></html>");
  return ESP_OK;
}


static esp_err_t response_ws_handler(httpd_req_t *req)
{
  if (req->method == HTTP_GET) {
    return ESP_OK;
  }
  httpd_ws_frame_t ws_pkt;
  uint8_t *buf = nullptr;
  memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;

  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK) {
    return ret;
  }
  if (ws_pkt.len) {
    buf = (uint8_t*)calloc(1, ws_pkt.len + 1);
    if (buf == nullptr) {
      return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
      free(buf);
      return ret;
    }
    if (memcmp(buf, "cmd=", 4) == 0) {
      bool press = (buf[4] == 'p');
      def::command::command_param_t cmd;
      cmd.raw = atoi((const char*)&buf[5]);
      system_registry->operator_command.addQueue(cmd, press);
    }
  }
  free(buf);
  return ret;
}

// =====================================================================
// /api/... エンドポイント
// L1: /api/files/<dir>[/<filename>]   ファイルCRUD
// L2: /api/song                       現在のライブソング GET/PUT
//     /api/song/save?dir=&name=       現在のライブソングをファイルに保存
//
// 注意:
// - 書き込み可能ディレクトリは api_dir_table のホワイトリストに限る
// - ファイル名は ".." やスラッシュを禁止し、必ず拡張子チェック
// - file_manage は内部で PSRAM をローテーションするため、レスポンス送信
//   完了までバッファが解放されないことを前提にしている
// - SDカードへのI/Oは task_operator など他タスクと競合しうる。今は許容
// =====================================================================

struct api_dir_entry_t {
  const char* token;                  // URLトークン (例: "songs/user")
  def::app::data_type_t dir_type;
  const char* required_ext;           // 必須拡張子 (例: ".json")
  bool writable;                      // PUT/DELETE 可否
};
static constexpr const api_dir_entry_t api_dir_table[] = {
  { "songs/user",       def::app::data_type_t::data_song_users,        ".json", true  },
  { "songs/extra",      def::app::data_type_t::data_song_extra,        ".json", true  },
  { "arpeggio/user",    def::app::data_type_t::data_arpeggio_users,    ".json", true  },
  { "progression/user", def::app::data_type_t::data_progression_users, ".json", true  },
};

// パスから dir エントリを longest match する。一致した場合の token 長を out_token_len に書く。
static const api_dir_entry_t* api_match_dir(const char* path, size_t path_len, size_t& out_token_len)
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
static bool api_is_valid_filename(const char* name, const char* required_ext)
{
  if (name == nullptr || name[0] == 0) return false;
  size_t n = strlen(name);
  if (n > 64) return false;
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

static esp_err_t api_send_json_error(httpd_req_t* req, const char* status, const char* message)
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
static void api_send_json_string(httpd_req_t* req, const char* s)
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
static memory_info_t* api_recv_body(httpd_req_t* req)
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
static esp_err_t api_files_list(httpd_req_t* req, const api_dir_entry_t* dir)
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
static esp_err_t api_files_get(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
{
  auto mem = file_manage.loadFile(dir->dir_type, filename);
  if (mem == nullptr) {
    return api_send_json_error(req, "404 Not Found", "file not found");
  }
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, (const char*)mem->data, mem->size);
}

// PUT /api/files/<dir>/<name>  → 保存
static esp_err_t api_files_put(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
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

// DELETE /api/files/<dir>/<name>
static esp_err_t api_files_delete(httpd_req_t* req, const api_dir_entry_t* dir, const char* filename)
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
static esp_err_t response_api_files_handler(httpd_req_t* req)
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
  std::string fname_raw(path + token_len + 1, path_len - token_len - 1);
  std::string fname = url_decode(fname_raw);
  if (!api_is_valid_filename(fname.c_str(), dir->required_ext)) {
    return api_send_json_error(req, "400 Bad Request", "invalid filename");
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
static esp_err_t response_api_song_get_handler(httpd_req_t* req)
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
static esp_err_t response_api_song_put_handler(httpd_req_t* req)
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
static esp_err_t response_api_progression_get_handler(httpd_req_t* req)
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
static esp_err_t response_api_progression_put_handler(httpd_req_t* req)
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
static esp_err_t response_api_progression_save_handler(httpd_req_t* req)
{
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen > 256) {
    return api_send_json_error(req, "400 Bad Request", "missing query");
  }
  std::vector<char> qbuf(qlen + 1, 0);
  if (httpd_req_get_url_query_str(req, qbuf.data(), qbuf.size()) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "bad query");
  }
  char dir_buf[32] = {};
  char name_buf[80] = {};
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
static esp_err_t response_api_song_save_handler(httpd_req_t* req)
{
  // クエリ文字列を抽出
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen > 256) {
    return api_send_json_error(req, "400 Bad Request", "missing query");
  }
  std::vector<char> qbuf(qlen + 1, 0);
  if (httpd_req_get_url_query_str(req, qbuf.data(), qbuf.size()) != ESP_OK) {
    return api_send_json_error(req, "400 Bad Request", "bad query");
  }
  char dir_buf[32] = {};
  char name_buf[80] = {};
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

static constexpr const httpd_uri uri_table[] = {
  { "/"    , HTTP_GET , response_top_handler      , nullptr, false, false, nullptr },
  { "/wifi", HTTP_GET , response_wifi_handler     , nullptr, false, false, nullptr },
  { "/main", HTTP_GET , response_main_handler     , nullptr, false, false, nullptr },
  { "/ctrl", HTTP_GET , response_ctrl_handler     , nullptr, false, false, nullptr },
  { "/ssid"   , HTTP_GET , response_ssid_handler     , nullptr, false, false, nullptr },
  { "/status" , HTTP_GET , response_status_handler   , nullptr, false, false, nullptr },
  { "/wifi"   , HTTP_POST, response_post_wifi_handler, nullptr, false, false, nullptr },
  { "/done"   , HTTP_POST, response_done_handler     , nullptr, false, false, nullptr },
  { "/ws"     , HTTP_GET , response_ws_handler       , nullptr,  true, false, nullptr },
  { "/api/files/*", HTTP_GET   , response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/files/*", HTTP_PUT   , response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/files/*", HTTP_DELETE, response_api_files_handler   , nullptr, false, false, nullptr },
  { "/api/song",      HTTP_GET , response_api_song_get_handler , nullptr, false, false, nullptr },
  { "/api/song",      HTTP_PUT , response_api_song_put_handler , nullptr, false, false, nullptr },
  { "/api/song/save", HTTP_POST, response_api_song_save_handler, nullptr, false, false, nullptr },
  { "/api/progression",      HTTP_GET , response_api_progression_get_handler , nullptr, false, false, nullptr },
  { "/api/progression",      HTTP_PUT , response_api_progression_put_handler , nullptr, false, false, nullptr },
  { "/api/progression/save", HTTP_POST, response_api_progression_save_handler, nullptr, false, false, nullptr },
};

static httpd_handle_t start_webserver(void)
{
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // /api/files/* など `*` を含むパターンを使うためにワイルドカード一致を有効化
  config.uri_match_fn = httpd_uri_match_wildcard;
  // uri_table[] の数に応じてハンドラ上限を引き上げる
  config.max_uri_handlers = 24;

  M5_LOGI("Starting server on port: '%d'", config.server_port);
  if (httpd_start(&server, &config) == ESP_OK) {
    M5_LOGI("Registering URI handlers");

    for (auto& uri : uri_table) {
      httpd_register_uri_handler(server, &uri);
    }

    return server;
  }

  M5_LOGI("Error starting server!");
  return nullptr;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
  if (server) {
    for (auto& uri : uri_table) {
      httpd_unregister_uri(server, uri.uri);
    }
    return httpd_stop(server);
  }
  return ESP_OK;
}

static constexpr const size_t http_port = 80;

static void task_wifi_info(void*) {
  bool ntp_sync = false;
  bool sntp_inited = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, 1000);
    {
      def::command::wifi_sta_info_t wifi_sta_info = def::command::wifi_sta_info_t::wsi_error;
      switch (_sta_state) {
      case STA_CONNECTED:
        {
          // SNTP初期化はWiFi接続確立後に行う（TCP/IPスタックの初期化が必要なため）
          if (!sntp_inited) {
            sntp_inited = true;
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, def::ntp::server1);
            esp_sntp_setservername(1, def::ntp::server2);
            esp_sntp_setservername(2, def::ntp::server3);
            esp_sntp_init();
          }
          wifi_ap_record_t ap_info;
          if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            int rssi = ((ap_info.rssi + 127) >> 5) + 1;
            if (rssi < 0) rssi = 0;
            wifi_sta_info = (def::command::wifi_sta_info_t)rssi;
          }
          if (!ntp_sync) {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
              ntp_sync = true;
              system_registry->runtime_info.setSntpSync(true);
            }
          }
        }
        break;
      case STA_STOPPED:
        wifi_sta_info = def::command::wifi_sta_info_t::wsi_off;
        break;
      case STA_IDLE:
      case STA_DISCONNECTED:
        wifi_sta_info = def::command::wifi_sta_info_t::wsi_waiting;
        break;
      }
      system_registry->runtime_info.setWiFiSTAInfo(wifi_sta_info);
    }
    {
      def::command::wifi_ap_info_t wifi_ap_info = def::command::wifi_ap_info_t::wai_off;
      if (_ap_started) {
        if (_ap_station_count > 0) {
          wifi_ap_info = def::command::wifi_ap_info_t::wai_enabled;
        } else {
          wifi_ap_info = def::command::wifi_ap_info_t::wai_waiting;
        }
        system_registry->runtime_info.setWiFiStationCount(_ap_station_count);
      }
      system_registry->runtime_info.setWiFiAPInfo(wifi_ap_info);
    }
  }
}

void task_wifi_t::start(void)
{
#if defined (M5UNIFIED_PC_BUILD)
  auto thread = SDL_CreateThread((SDL_ThreadFunction)task_func, "wifi", this);
#else

  xTaskCreatePinnedToCore(task_wifi_info, "wi", 2048, this, 0, &_wifi_info_task_handle, def::system::task_cpu_wifi);

  xTaskCreatePinnedToCore((TaskFunction_t)task_func, "wifi", 4096, this, def::system::task_priority_wifi, &_wifi_task_handle, def::system::task_cpu_wifi);
  system_registry->wifi_control.setNotifyTaskHandle(_wifi_task_handle);

#endif
}

// -----------------------------------------------------------------------------
// task_func 内部で使う低レベルヘルパー群
// -----------------------------------------------------------------------------

// AP にステーションが接続した後、SSID スキャンを有効にするために
// AP-only モードで立ち上がっている Wi-Fi を APSTA モードへ昇格させる。
// 昇格は 1 度だけ発生する想定で、以降の呼び出しは何もしない。
static void maybe_upgrade_to_apsta_for_scan()
{
  if (!_ws || !_ws->wifi_started) return;
  wifi_mode_t cur_mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&cur_mode) != ESP_OK) return;
  if (cur_mode == WIFI_MODE_APSTA || cur_mode == WIFI_MODE_STA) return;

  wifi_ensure_sta_netif();
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  M5_LOGI("[wifi] upgrade AP->APSTA err=0x%x (t=+%lu)",
          err, (unsigned long)(M5.millis() - _setup_t0_ms));
}

// 完了したスキャン結果をキャッシュに取り込み、必要なら次のスキャンをキックする。
// 呼び出し側は「スキャンすべき状況か」の判定 (goal + station count 等) を済ませてから呼ぶ。
static void scan_tick()
{
  if (!_ws) return;

  // (1) 完了したスキャンの取り込み
  if (_scan_status >= 0) {
    uint16_t count = (uint16_t)_scan_status;
    if (count > 0) {
      wifi_ap_record_t* recs = (wifi_ap_record_t*)malloc(count * sizeof(wifi_ap_record_t));
      if (recs) {
        if (esp_wifi_scan_get_ap_records(&count, recs) == ESP_OK) {
          ssid_cache_merge(recs, count);
        }
        free(recs);
      }
    } else {
      // 0件完了: キャッシュは温存（前回結果を捨てない）
      esp_wifi_clear_ap_list();
    }
    _scan_status = -2;
    uint32_t now = M5.millis();
    _ws->last_scan_done_ms = now ? now : 1; // 0 は「未実施」を意味するので避ける
  }

  // (2) 次のスキャンを間隔に従ってキック
  if (_scan_status == -2) {
    uint32_t now = M5.millis();
    uint32_t interval = (_ws->ssid_cache_count == 0) ? 2500 : 12000;
    bool due = (_ws->last_scan_done_ms == 0)
             || ((now - _ws->last_scan_done_ms) >= interval);
    if (due) {
      esp_err_t err = esp_wifi_scan_start(nullptr, false);
      if (err == ESP_OK) {
        _scan_status = -1;
      } else {
        // 失敗時は次 tick で再試行。last_scan_done_ms を進めて連打を防ぐ。
        _ws->last_scan_done_ms = now ? now : 1;
      }
    }
  }
}

// setup_ap (AP モード) の AP 設定を反映する。呼び出しは esp_wifi_start の前。
static void apply_ap_config()
{
  wifi_config_t ap_config = {};
  strncpy((char*)ap_config.ap.ssid, def::app::wifi_ap_ssid, sizeof(ap_config.ap.ssid) - 1);
  strncpy((char*)ap_config.ap.password, def::app::wifi_ap_pass, sizeof(ap_config.ap.password) - 1);
  ap_config.ap.ssid_len = strlen(def::app::wifi_ap_ssid);
  ap_config.ap.channel = 1;
  ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap_config.ap.max_connection = 4;
  esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

void task_wifi_t::task_func(task_wifi_t* me)
{
  task_http_client_t task_http_client;

  // Wi-Fi サブシステムの「望む状態」を 1 つの値で表現するビットフラグ集合。
  // システムレジストリ (wifi_mode / operation / webserver_mode) の組み合わせから
  // compute_from_registry() でこの構造体が計算され、task_func はこれと直前の値の
  // 差分に基づいて必要な起動/停止アクションを発行する。
  struct wifi_goal_t {
    union {
      struct {
        uint8_t ap_enabled   : 1; // AP インタフェースを立ち上げる
        uint8_t sta_enabled  : 1; // STA インタフェースを立ち上げ外部 AP へ接続する
        uint8_t http_server  : 1; // HTTP サーバ (設定 UI) を起動する
        uint8_t ssid_scan    : 1; // setup UI 向けに SSID スキャンリストを保持する
        uint8_t wps          : 1; // WPS プッシュボタン接続モード
      };
      uint8_t raw = 0;
    };
    // レジストリの 3 つの入力値から望む状態を組み立てる。
    // 各入力値は独立に足し算されるため、上位の指定 (operation や webserver_mode) は
    // 下位 (wifi_mode) が指定したフラグを解除しない。
    void compute_from_registry(def::command::wifi_mode_t m,
                               def::command::wifi_operation_t o,
                               def::command::webserver_mode_t s) {
      raw = 0;

      // (1) 基本のインタフェース構成
      switch (m) {
      default:
      case def::command::wifi_mode_t::wifi_disable:                               break;
      case def::command::wifi_mode_t::wifi_enable_sta:    sta_enabled = 1;        break;
      case def::command::wifi_mode_t::wifi_enable_ap:     ap_enabled = 1;         break;
      case def::command::wifi_mode_t::wifi_enable_sta_ap: ap_enabled = sta_enabled = 1; break;
      }

      // (2) 現在走っているオペレーション (セットアップ / OTA 等) に必要な構成を追加
      switch (o) {
      default:
      case def::command::wifi_operation_t::wfop_disable:
        break;
      case def::command::wifi_operation_t::wfop_setup_ap:
        // スマホ向けの設定モード: AP を立て、SSID 一覧を提供し、HTTP UI を出す
        ap_enabled = 1;
        ssid_scan  = 1;
        http_server = 1;
        break;
      case def::command::wifi_operation_t::wfop_setup_wps:
        wps = 1;
        break;
      case def::command::wifi_operation_t::wfop_ota_begin:
      case def::command::wifi_operation_t::wfop_ota_progress:
        // OTA 取得のため STA 接続が必要
        sta_enabled = 1;
        break;
      }

      // (3) 独立した Web サーバ希望 (デバッグ/コントロール UI など)
      switch (s) {
      default: break;
      case def::command::webserver_mode_t::ws_enable:
        ap_enabled = sta_enabled = http_server = 1;
        break;
      }
    }
  };
  wifi_goal_t goal;

  for (;;) {
#if defined (M5UNIFIED_PC_BUILD)
    M5.delay(2048);
#else

    // -------- ウェイト待ち: AP/HTTPサーバ動作中は DNS を細かく捌くため短周期で回す --------
    int wait = 1024;
    if (goal.ap_enabled || goal.http_server) {
      dns_server_process();
      wait = 4;
    }
    taskYIELD();
    ulTaskNotifyTake(pdTRUE, wait);

    // -------- レジストリから新しい goal を計算 --------
    auto mode = system_registry->wifi_control.getWifiMode();
    auto op = system_registry->wifi_control.getOperation();
    auto webserver_mode = system_registry->wifi_control.getWebServerMode();
    wifi_goal_t prev_goal = goal;
    goal.compute_from_registry(mode, op, webserver_mode);

    // =============================================================================
    // goal が変化した場合、差分に沿って Wi-Fi サブシステムの構成を切り替える。
    // 3 段階に分けて実行する:
    //   Phase 1: 消失するサブシステムの停止 (WPS/HTTP サーバ/スキャン)
    //   Phase 2: ラジオ (AP/STA/WPS) の再構成 or 完全無効化
    //   Phase 3: 新たに必要になったサブシステムの起動 (HTTP サーバ/DNS/mDNS 等)
    // =============================================================================
    if (prev_goal.raw != goal.raw) {

      // ---- Phase 1: 消失するサブシステムの停止 ------------------------------
      if (!goal.wps && _ws && _ws->wps_enabled) {
        wpsStop();
      }
      if (prev_goal.http_server && !goal.http_server) {
        if (_ws && _ws->http_server) {
          stop_webserver(_ws->http_server);
          _ws->http_server = nullptr;
        }
        mdns_free();
      }
      if (prev_goal.ssid_scan && !goal.ssid_scan) {
        esp_wifi_clear_ap_list();
        _scan_status = -2;
        if (_ws) _ws->last_scan_done_ms = 0;
        ssid_cache_clear();
      }

      // ---- Phase 2: ラジオ (AP/STA/WPS) の再構成 ------------------------------
      // AP/STA/WPS のいずれかのフラグが変化した場合のみ Wi-Fi ドライバを作り直す。
      if (prev_goal.ap_enabled  != goal.ap_enabled
       || prev_goal.sta_enabled != goal.sta_enabled
       || prev_goal.wps         != goal.wps) {

        // ソフト遷移: AP は継続、WPS も変化なしで、STA フラグだけが変化したケース。
        // この場合は esp_wifi_stop/start を挟まず、AP ビーコンを途切れさせないまま
        // STA インタフェースの on/off だけを行う。(主な用途: setup_ap で端末が
        // ESP の AP に繋がった後、STA 接続が成立してそのまま webserver_mode に
        // 遷移する場合に、スマホの AP 接続を切らないため。)
        bool soft_sta_transition =
             _ws && _ws->wifi_started
          && prev_goal.ap_enabled && goal.ap_enabled
          && !prev_goal.wps && !goal.wps
          && prev_goal.sta_enabled != goal.sta_enabled;

        if (soft_sta_transition) {
          if (goal.sta_enabled) {
            // STA 有効化: 必要なら APSTA モードに昇格し、未接続なら connect
            wifi_ensure_sta_netif();
            wifi_mode_t cur_mode = WIFI_MODE_NULL;
            if (esp_wifi_get_mode(&cur_mode) == ESP_OK && cur_mode != WIFI_MODE_APSTA) {
              esp_wifi_set_mode(WIFI_MODE_APSTA);
            }
            if (_sta_state != STA_CONNECTED) {
              esp_wifi_connect();
            }
          } else {
            // STA 無効化: disconnect のみ。AP を維持したいのでモード (APSTA) は
            // そのまま残す。次回 AP を落とす遷移が来たときに完全再構成する。
            esp_wifi_disconnect();
          }
        } else {
          // 通常遷移: stop/start を伴う完全再構成
          if (prev_goal.sta_enabled && !goal.sta_enabled) {
            esp_wifi_disconnect();
          }
          if (prev_goal.ap_enabled && !goal.ap_enabled) {
            dns_server_stop();
          }
          M5.delay(16);

          const bool radio_off = !goal.ap_enabled && !goal.sta_enabled && !goal.wps;
          if (radio_off) {
          // 完全無効化: ドライバと wifi_state_t を解放して heap を返却する
          wifi_state_destroy();
          system_registry->task_status.setSuspend(
            system_registry_t::reg_task_status_t::bitindex_t::TASK_WIFI);
        } else {
          system_registry->task_status.setWorking(
            system_registry_t::reg_task_status_t::bitindex_t::TASK_WIFI);

          // Wi-Fi 有効化: 必要なら wifi_state_t を作成してドライバ初期化
          wifi_state_create();

          if (_ws->wifi_started) {
            esp_wifi_stop();
            _ws->wifi_started = false;
          }

          // 起動時のモード決定ルール:
          //   AP が必要で STA も必要 → APSTA
          //   AP だけ必要 (SSIDスキャンも) → AP-only で立ち上げ、後でステーション
          //     接続を受けてから APSTA へ昇格する (maybe_upgrade_to_apsta_for_scan)
          //   STA のみ → STA
          wifi_mode_t new_mode;
          if (goal.ap_enabled) {
            new_mode = goal.sta_enabled ? WIFI_MODE_APSTA : WIFI_MODE_AP;
          } else {
            new_mode = WIFI_MODE_STA;
          }

          // 必要な netif だけ作成 (不要な netif のメモリ確保を避ける)
          if (new_mode == WIFI_MODE_STA || new_mode == WIFI_MODE_APSTA) {
            wifi_ensure_sta_netif();
          }
          if (new_mode == WIFI_MODE_AP || new_mode == WIFI_MODE_APSTA) {
            wifi_ensure_ap_netif();
          }
          esp_wifi_set_mode(new_mode);

          if (goal.ap_enabled) {
            apply_ap_config();
          }

          esp_wifi_start();
          _ws->wifi_started = true;
          M5_LOGI("[wifi-timing] t=+%lu esp_wifi_start (mode=%d)",
                  (unsigned long)(M5.millis() - _setup_t0_ms), (int)new_mode);

          if (goal.wps && !_ws->wps_enabled) {
            M5.delay(16);
            wpsStart();
          }
          if (goal.sta_enabled) {
            M5.delay(16);
            esp_wifi_connect();
          }
        }
        } // end else (通常遷移)
      }

      // ---- Phase 3: 新しく必要になったサブシステムの起動 ----------------------
      if (!prev_goal.ssid_scan && goal.ssid_scan) {
        // 初回スキャンはこの後の scan_tick() に任せる (直前のドライバ再起動と競合しない)
        _scan_status = -2;
        if (_ws) _ws->last_scan_done_ms = 0;
      }
      if (!prev_goal.http_server && goal.http_server && _ws) {
        M5.delay(16);
        _ws->http_server = start_webserver();
        mdns_init();
        mdns_hostname_set(def::app::wifi_mdns);
        mdns_service_add(nullptr, "_http", "_tcp", http_port, nullptr, 0);
        if (goal.ap_enabled && _ws->ap_netif) {
          esp_netif_ip_info_t ip_info;
          if (esp_netif_get_ip_info(_ws->ap_netif, &ip_info) == ESP_OK) {
            dns_server_start(ip_info.ip.addr);
          }
        }
      }
    }

    // =============================================================================
    // 定常時の処理 (goal 変化の有無に関わらず毎 tick 実行する必要があるもの)
    // =============================================================================

    // setup_ap 中に STA 接続が成立したら、自動的に webserver_mode を有効化して
    // setup を終了させる。以降は AP/HTTP サーバを維持したまま通常動作に移行する。
    // (ユーザが /done を押さなくても自動で遷移する)
    if (op == def::command::wifi_operation_t::wfop_setup_ap
        && _sta_state == STA_CONNECTED) {
      system_registry->wifi_control.setWebServerMode(
        def::command::webserver_mode_t::ws_enable);
      system_registry->wifi_control.setOperation(
        def::command::wifi_operation_t::wfop_disable);
    }

    // OTA 開始要求 → STA 接続が確立次第 http client で取得開始
    if (op == def::command::wifi_operation_t::wfop_ota_begin) {
      system_registry->runtime_info.setWiFiOtaProgress(
        def::command::wifi_ota_state_t::ota_connecting);
      if (_sta_state == STA_CONNECTED) {
        system_registry->wifi_control.setOperation(
          def::command::wifi_operation_t::wfop_ota_progress);
        task_http_client.exec_ota(def::app::url_ota_info);
      }
    }

    // SSID スキャン: ステーションが接続してから初めて動かす。
    //   - ステーション接続までは AP の radio を占有させない (接続高速化)
    //   - ステーション接続を受けたら AP-only → APSTA に昇格して scan 解禁
    if (goal.ssid_scan && _ws && _ws->wifi_started && _ap_station_count > 0) {
      maybe_upgrade_to_apsta_for_scan();
      scan_tick();
    }

#endif
  }
}

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
