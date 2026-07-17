// SPDX-License-Identifier: MIT
#pragma once

#include <stdint.h>

namespace midi_driver {

struct usb_host_probe_t {
  uint16_t connection_events;
  uint16_t disconnection_events;
  uint16_t port_error_events;
  uint16_t overcurrent_events;
  uint16_t reset_commands;
  uint16_t reset_failures;
  uint16_t enum_process_calls;
  uint16_t enum_process_failures;
  uint16_t control_submissions;
  uint16_t control_completions;
  uint16_t control_submit_failures;
  uint16_t last_request_length;
  uint8_t last_descriptor_type;
};

usb_host_probe_t get_usb_host_probe(void);

} // namespace midi_driver
