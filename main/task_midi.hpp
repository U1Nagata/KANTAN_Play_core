// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_TASK_MIDI_HPP
#define KANPLAY_TASK_MIDI_HPP

namespace kanplay_ns {
//-------------------------------------------------------------------------
class task_midi_t {
public:
  void start(void);
  bool startUSBHIDKeyboard(void);
  bool getUSBHIDKeyboardEvent(uint8_t* usage, bool* pressed);
protected:
  static void task_func(task_midi_t* me);
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
