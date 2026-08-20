// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_TEXT_HPP
#define KANTAN_SAMPLER_TEXT_HPP

#include <stdint.h>
#include <string>
#include <vector>

namespace sampler_ns {

// macOS commonly stores Japanese filenames in decomposed UTF-8. The bundled
// font contains precomposed kana, but not standalone combining sound marks.
inline uint32_t compose_kana_sound_mark(uint32_t base, uint32_t mark)
{
  const bool dakuten = mark == 0x3099 || mark == 0x309B;
  const bool handakuten = mark == 0x309A || mark == 0x309C;
  if (dakuten) {
    switch (base) {
    case 0x304B: case 0x304D: case 0x304F: case 0x3051: case 0x3053:
    case 0x3055: case 0x3057: case 0x3059: case 0x305B: case 0x305D:
    case 0x305F: case 0x3061: case 0x3064: case 0x3066: case 0x3068:
    case 0x306F: case 0x3072: case 0x3075: case 0x3078: case 0x307B:
    case 0x30AB: case 0x30AD: case 0x30AF: case 0x30B1: case 0x30B3:
    case 0x30B5: case 0x30B7: case 0x30B9: case 0x30BB: case 0x30BD:
    case 0x30BF: case 0x30C1: case 0x30C4: case 0x30C6: case 0x30C8:
    case 0x30CF: case 0x30D2: case 0x30D5: case 0x30D8: case 0x30DB:
      return base + 1;
    case 0x3046: return 0x3094;
    case 0x30A6: return 0x30F4;
    case 0x30EF: case 0x30F0: case 0x30F1: case 0x30F2:
      return base + 8;
    default: break;
    }
  } else if (handakuten) {
    switch (base) {
    case 0x306F: case 0x3072: case 0x3075: case 0x3078: case 0x307B:
    case 0x30CF: case 0x30D2: case 0x30D5: case 0x30D8: case 0x30DB:
      return base + 2;
    default: break;
    }
  }
  return 0;
}

inline std::string normalize_japanese_display_text(const std::string& source)
{
  std::vector<uint32_t> codepoints;
  codepoints.reserve(source.size());
  for (size_t i = 0; i < source.size();) {
    const uint8_t lead = (uint8_t)source[i++];
    uint32_t cp = lead;
    uint8_t continuation = 0;
    if ((lead & 0xE0) == 0xC0) { cp = lead & 0x1F; continuation = 1; }
    else if ((lead & 0xF0) == 0xE0) { cp = lead & 0x0F; continuation = 2; }
    else if ((lead & 0xF8) == 0xF0) { cp = lead & 0x07; continuation = 3; }
    for (uint8_t n = 0; n < continuation && i < source.size(); ++n) {
      cp = (cp << 6) | ((uint8_t)source[i++] & 0x3F);
    }
    if (!codepoints.empty()) {
      const uint32_t composed = compose_kana_sound_mark(codepoints.back(), cp);
      if (composed) {
        codepoints.back() = composed;
        continue;
      }
    }
    codepoints.push_back(cp);
  }

  std::string result;
  result.reserve(source.size());
  for (const uint32_t cp : codepoints) {
    if (cp <= 0x7F) {
      result.push_back((char)cp);
    } else if (cp <= 0x7FF) {
      result.push_back((char)(0xC0 | (cp >> 6)));
      result.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      result.push_back((char)(0xE0 | (cp >> 12)));
      result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      result.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      result.push_back((char)(0xF0 | (cp >> 18)));
      result.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      result.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return result;
}

} // namespace sampler_ns

#endif
