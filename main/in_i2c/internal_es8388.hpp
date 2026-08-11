// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_INTERNAL_ES8388_HPP
#define KANPLAY_INTERNAL_ES8388_HPP

#include <stdint.h>

namespace kanplay_ns {
//-------------------------------------------------------------------------
class internal_es8388_t {
  uint8_t _out_volume = 0xFF;
  uint8_t _in_volume = 0xFF;
public:
  void init(void);
  void mute(void);
  void unmute(void);

  // 出力ボリューム変更指示 有効レンジは 0 ~ 33 (0x21  4.5dB)
  void setOutVolume(uint8_t volume);
  uint8_t getOutVolume(void) { return _out_volume; }

  // 入力ボリューム変更指示 有効レンジは 0 ~ 15
  void setInVolume(uint8_t volume);
  uint8_t getInVolume(void) { return _in_volume; }

  // INPUT1 carries the normal internal sound-engine path. INPUT2 is the
  // headset/external microphone path selected only while sampling it.
  void setExternalInput(bool enabled);
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
