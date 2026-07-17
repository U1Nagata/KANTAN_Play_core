// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_TASK_MIDI_HPP
#define KANPLAY_TASK_MIDI_HPP

#include "common_define.hpp"

namespace kanplay_ns {
//-------------------------------------------------------------------------
class task_midi_t {
public:
  void start(void);
  bool startUSBHIDKeyboard(void);
  bool getUSBHIDKeyboardEvent(uint8_t* usage, bool* pressed);
  bool isUSBStarted(void);
  bool isUSBStackReady(void);
  def::command::usb_mode_t getUSBMode(void);
  void getUSBHostDiagnostic(uint16_t* vendor_id, uint16_t* product_id,
                            uint8_t* interface_class, uint8_t* interface_subclass,
                            uint8_t* endpoint_count, bool* device_seen,
                            bool* midi_interface, int* open_result,
                            int* descriptor_result, int* claim_result) const;
  uint32_t getBLEMidiPacketCount(void) const;
  void getBLEMidiLastPacket(uint8_t* data, size_t* length) const;
  void getBLEMidiConnectionDiagnostic(bool* central, bool* peripheral, uint8_t* subscription) const;
  void getBLEMidiCentralDeviceName(char* name, size_t size) const;
  void getBLEMidiPeerAddresses(char* central, size_t central_size, char* peripheral, size_t peripheral_size) const;
  uint8_t getBLEMidiCentralProperties(void) const;
  void getBLEMidiSecurityDiagnostic(uint8_t* auth_state, uint8_t* cccd_value,
                                    uint8_t* registration_status) const;
  bool clearBLEMidiCentralBond(void);
protected:
  static void task_func(task_midi_t* me);
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
