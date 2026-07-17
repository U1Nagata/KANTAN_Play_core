# USB host compatibility object

`libusb.a` is rebuilt from ESP-IDF 5.4.2 with
`CONFIG_USB_HOST_RESET_RECOVERY_MS=100` and
`CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE=1024`.

The stock Arduino ESP32 3.2.1 library uses 30 ms. Some older USB 1.1 MIDI
devices need at least 80 ms after a bus reset before enumeration continues.
Composite USB audio/MIDI devices can also expose configuration descriptors
larger than the stock 256-byte control-transfer buffer.
Only the USB host component is replaced. TinyUSB and all other ESP-IDF
components remain the stock PlatformIO versions.

`platformio.ini` applies this library and the enumeration wrappers through
the shared `usb_host_compat` flags. Both KANTAN Play (`release_s3`) and
KANTAN Sampler (`sampler_s3`) must use that shared block.
