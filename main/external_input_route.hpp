// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace kanplay_ns {

// Keep the persisted numeric values stable.  This small, dependency-free
// model is shared by the firmware registry and the host regression test.
enum class external_input_route_source_t : uint8_t {
  off = 0,
  usb_midi_host,
  usb_midi_device,
  ble_midi,
  uart_midi,
  max,
};

struct external_input_route_plan_t {
  bool uart_input = false;
  bool ble_input = false;
  bool usb_input = false;
  bool usb_output = false;
  bool usb_host = false;
  bool usb_power = false;

  constexpr uint8_t inputCount(void) const {
    return uint8_t(uart_input) + uint8_t(ble_input) + uint8_t(usb_input);
  }
  constexpr bool isExclusive(void) const { return inputCount() <= 1; }
};

constexpr external_input_route_source_t sanitizeExternalInputRoute(uint8_t raw) {
  return raw < uint8_t(external_input_route_source_t::max)
      ? static_cast<external_input_route_source_t>(raw)
      : external_input_route_source_t::off;
}

constexpr external_input_route_plan_t externalInputRoutePlan(
    external_input_route_source_t source) {
  switch (source) {
  case external_input_route_source_t::usb_midi_host:
    return {false, false, true, false, true, true};
  case external_input_route_source_t::usb_midi_device:
    // A computer connection is a bidirectional performance/control link.
    return {false, false, true, true, false, false};
  case external_input_route_source_t::ble_midi:
    return {false, true, false, false, false, false};
  case external_input_route_source_t::uart_midi:
    return {true, false, false, false, false, false};
  case external_input_route_source_t::off:
  default:
    return {};
  }
}

// USB-C can be connected either to a computer (Device role) or to a MIDI
// controller (Host role).  Never claim Host while another source is already
// driving VBUS.  Keep the saved selection intact and use the bidirectional
// computer route for this boot; the application restarts into Host after VBUS
// disappears.  This policy is shared by Sequencer and Sampler.
constexpr external_input_route_source_t externalInputBootSource(
    external_input_route_source_t saved_source, bool external_vbus_present) {
  return saved_source == external_input_route_source_t::usb_midi_host
      && external_vbus_present
      ? external_input_route_source_t::usb_midi_device
      : saved_source;
}

constexpr bool externalInputWaitsForUsbHostDisconnect(
    external_input_route_source_t saved_source, bool external_vbus_present) {
  return externalInputBootSource(saved_source, external_vbus_present) != saved_source;
}

// A powered OTG hub/Y-cable can provide VBUS while the ESP32-S3 remains the
// USB data host.  In that case the CoreS3 must keep its OTG power switch off
// so the same connector can feed the charger instead of driving two supplies
// against each other.
constexpr bool externalInputUsbPowerEnabled(
    external_input_route_source_t source, bool external_vbus_present) {
  return externalInputRoutePlan(source).usb_power && !external_vbus_present;
}

} // namespace kanplay_ns
