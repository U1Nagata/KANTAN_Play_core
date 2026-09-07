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
  // GitHub Pages follows the development branch without raw-content cache
  // lag, which keeps the device catalog in sync with newly published OTA data.
  static constexpr const char url_ota_catalog[] =
    "https://u1nagata.github.io/KANTAN_Play_core/firmware/catalog.json";
}

// 上段4モードボタンに対応する動作モード (仕様書「各モード」参照)
namespace def::mode {
  enum class sampler_mode_t : uint8_t {
    mode_sound = 0,  // 音源の選択・Sample Recording・編集
    mode_play,       // 自由演奏（Loopへ記録しない）
    mode_rec,        // Pad演奏をLoopへ記録する
    mode_fx,         // リアルタイムエフェクト
    mode_max,
  };
}

namespace def::pad {
  static constexpr const uint8_t pad_count = 12;  // 4x3 メインPad
}
//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
