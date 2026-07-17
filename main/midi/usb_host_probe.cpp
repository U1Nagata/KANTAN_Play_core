// SPDX-License-Identifier: MIT
#include "usb_host_probe.hpp"

#if __has_include(<usb/usb_host.h>)

#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/queue.h>
#include <usb/usb_host.h>

namespace {

volatile midi_driver::usb_host_probe_t probe = {};
volatile uint16_t enum_calls_since_connection = 0;

static void increment(volatile uint16_t& value)
{
  if (value != UINT16_MAX) { ++value; }
}

} // namespace

// Layout used internally by ESP-IDF's USB host stack. The public transfer is
// the final member, so it can be safely inspected before forwarding the URB.
struct usb_host_urb_compat_t {
  TAILQ_ENTRY(usb_host_urb_compat_t) tailq_entry;
  void* hcd_ptr;
  uint32_t hcd_var;
  void* usb_host_client;
  bool usb_host_inflight;
  usb_transfer_t transfer;
};

// These wrappers observe the ESP-IDF host stack without changing its timing or
// return values. The private HCD enums use the C ABI and have int-sized values.
extern "C" int __real_hcd_port_handle_event(void* port_handle);
extern "C" int __wrap_hcd_port_handle_event(void* port_handle)
{
  const int event = __real_hcd_port_handle_event(port_handle);
  switch (event) {
  case 1:
    increment(probe.connection_events);
    enum_calls_since_connection = 0;
    break;
  case 2: increment(probe.disconnection_events); break;
  case 3: increment(probe.port_error_events); break;
  case 4: increment(probe.overcurrent_events); break;
  default: break;
  }
  return event;
}

extern "C" esp_err_t __real_hcd_port_command(void* port_handle, int command);
extern "C" esp_err_t __wrap_hcd_port_command(void* port_handle, int command)
{
  const esp_err_t result = __real_hcd_port_command(port_handle, command);
  if (command == 2) { // HCD_PORT_CMD_RESET
    increment(probe.reset_commands);
    if (result != ESP_OK) { increment(probe.reset_failures); }
  }
  return result;
}

extern "C" esp_err_t __real_enum_process(void);
extern "C" esp_err_t __wrap_enum_process(void)
{
  increment(probe.enum_process_calls);
  increment(enum_calls_since_connection);
  // ESP-IDF 5.4 proceeds directly from the full device descriptor to the
  // configuration descriptor request. Some legacy USB 1.1 MIDI devices keep
  // NAKing that request unless they get a short settling interval here. In the
  // current enumeration FSM the 11th process call is GET_SHORT_CONFIG_DESC.
  if (enum_calls_since_connection == 11) {
    vTaskDelay(pdMS_TO_TICKS(120));
  }
  const esp_err_t result = __real_enum_process();
  if (result != ESP_OK) { increment(probe.enum_process_failures); }
  return result;
}

extern "C" esp_err_t __real_usbh_dev_submit_ctrl_urb(void* device_handle, void* urb);
extern "C" esp_err_t __wrap_usbh_dev_submit_ctrl_urb(void* device_handle, void* urb_ptr)
{
  auto* urb = static_cast<usb_host_urb_compat_t*>(urb_ptr);
  auto* setup = reinterpret_cast<usb_setup_packet_t*>(urb->transfer.data_buffer);
  const uint8_t descriptor_type = setup->wValue >> 8;
  probe.last_descriptor_type = descriptor_type;
  probe.last_request_length = setup->wLength;
  if (setup->bRequest == USB_B_REQUEST_GET_DESCRIPTOR
      && descriptor_type == USB_B_DESCRIPTOR_TYPE_CONFIGURATION
      && setup->wLength == 8) {
    // A configuration descriptor header is 9 bytes. ESP-IDF 5.4 requests only
    // 8 bytes here; some legacy devices NAK that nonstandard short request
    // forever. Request the complete header and provide a 64-byte-aligned IN
    // buffer, valid for both 8-byte and 64-byte EP0 maximum packet sizes.
    setup->wLength = sizeof(usb_config_desc_t);
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + 64;
    probe.last_request_length = setup->wLength;
  }
  increment(probe.control_submissions);
  const esp_err_t result = __real_usbh_dev_submit_ctrl_urb(device_handle, urb_ptr);
  if (result != ESP_OK) { increment(probe.control_submit_failures); }
  return result;
}

extern "C" void* __real_hcd_urb_dequeue(void* pipe_handle);
extern "C" void* __wrap_hcd_urb_dequeue(void* pipe_handle)
{
  void* urb = __real_hcd_urb_dequeue(pipe_handle);
  if (urb != nullptr) { increment(probe.control_completions); }
  return urb;
}

namespace midi_driver {

usb_host_probe_t get_usb_host_probe(void)
{
  return {
    probe.connection_events,
    probe.disconnection_events,
    probe.port_error_events,
    probe.overcurrent_events,
    probe.reset_commands,
    probe.reset_failures,
    probe.enum_process_calls,
    probe.enum_process_failures,
    probe.control_submissions,
    probe.control_completions,
    probe.control_submit_failures,
    probe.last_request_length,
    probe.last_descriptor_type,
  };
}

} // namespace midi_driver

#else

namespace midi_driver {
usb_host_probe_t get_usb_host_probe(void) { return {}; }
} // namespace midi_driver

#endif
