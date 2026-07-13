// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_WEB_API_HPP
#define KANTAN_SAMPLER_WEB_API_HPP

#if defined(KANPLAY_SAMPLER) && !defined(M5UNIFIED_PC_BUILD)

#include <esp_http_server.h>

namespace sampler_ns {
void sampler_web_api_register_uris(httpd_handle_t server);
void sampler_web_api_unregister_uris(httpd_handle_t server);
} // namespace sampler_ns

#endif
#endif
