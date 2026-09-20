// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace kanplay_ns {

// Keep the CoreS3 OTG switch off while an externally powered hub/Y-cable has
// a chance to present VBUS.  If no external VBUS appears after the USB Host
// stack is ready, the CoreS3 supplies the bus-powered MIDI device itself.
class usb_host_power_handoff_t {
public:
  enum class phase_t : uint8_t {
    inactive,
    waiting_for_stack,
    observing_external_vbus,
    externally_powered,
    supplying_vbus,
  };

  static constexpr uint32_t observation_msec = 1500;
  static constexpr uint8_t stable_samples_required = 3;

  void begin(bool usb_host_selected) {
    _phase = usb_host_selected ? phase_t::waiting_for_stack : phase_t::inactive;
    _deadline = 0;
    _stable_samples = 0;
  }

  bool step(uint32_t now, bool stack_ready, bool external_vbus_present) {
    if (_phase == phase_t::inactive || _phase == phase_t::externally_powered) {
      return false;
    }
    if (_phase == phase_t::supplying_vbus) { return true; }

    if (_phase == phase_t::waiting_for_stack) {
      if (!stack_ready) { return false; }
      _phase = phase_t::observing_external_vbus;
      _deadline = now + observation_msec;
      _stable_samples = external_vbus_present ? 1 : 0;
      return false;
    }

    _stable_samples = external_vbus_present
        ? uint8_t(_stable_samples < stable_samples_required
                      ? _stable_samples + 1
                      : stable_samples_required)
        : 0;
    if (_stable_samples >= stable_samples_required) {
      _phase = phase_t::externally_powered;
      return false;
    }
    if (int32_t(now - _deadline) < 0) { return false; }

    // Never turn on the OTG output while VBUS is present, even if it appeared
    // only at the end of the observation window.
    _phase = external_vbus_present
        ? phase_t::externally_powered
        : phase_t::supplying_vbus;
    return _phase == phase_t::supplying_vbus;
  }

  phase_t phase() const { return _phase; }

private:
  phase_t _phase = phase_t::inactive;
  uint32_t _deadline = 0;
  uint8_t _stable_samples = 0;
};

} // namespace kanplay_ns
