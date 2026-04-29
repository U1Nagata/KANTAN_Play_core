// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_TASK_WIFI_API_HPP
#define KANPLAY_TASK_WIFI_API_HPP

// /api/* エンドポイントの登録/解除を task_wifi.cpp から呼び出すための公開インタフェース。
// task_wifi.cpp はこのヘッダだけをインクルードし、URI 一覧やハンドラ実装は知らない。

#if !defined(M5UNIFIED_PC_BUILD)

#include <esp_http_server.h>

namespace kanplay_ns {

void task_wifi_api_register_uris(httpd_handle_t server);
void task_wifi_api_unregister_uris(httpd_handle_t server);

} // namespace kanplay_ns

#endif // !M5UNIFIED_PC_BUILD

#endif // KANPLAY_TASK_WIFI_API_HPP
