// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_DEFINE_HPP
#define KANTAN_SAMPLER_DEFINE_HPP

#include "sampler_version.hpp"

#include <stdint.h>

namespace sampler_ns {
//-------------------------------------------------------------------------
namespace def::app {
  static constexpr const uint8_t app_version_major = SAMPLER_VERSION_MAJOR;
  static constexpr const uint8_t app_version_minor = SAMPLER_VERSION_MINOR;
  static constexpr const uint8_t app_version_patch = SAMPLER_VERSION_PATCH;
  static constexpr const char app_name[] = "KANTAN Sampler";
  // Sampler beta is delivered from the development repository.  Use the raw
  // file endpoint so catalog and binary use the same TLS/CDN path.
  static constexpr const char url_ota_catalog[] =
    "https://raw.githubusercontent.com/U1Nagata/KANTAN_Play_core/main/docs/firmware/catalog.json";
}

// 上段4モードボタンに対応する動作モード (仕様書「各モード」参照)
namespace def::mode {
  enum class sampler_mode_t : uint8_t {
    mode_rec = 0,  // 音を捕まえる (録音・素材管理)
    mode_play,     // 自由演奏 (Loopへ記録されない)
    mode_loop,     // 時間へ記録する (ルーパー)
    mode_fx,       // 音を壊す (エフェクト)
    mode_max,
  };
}

namespace def::pad {
  static constexpr const uint8_t pad_count = 12;  // 4x3 メインPad
}
//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
