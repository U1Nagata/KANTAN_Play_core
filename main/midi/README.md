# MIDI Transport Layer

このフォルダは、KANTAN Play core と KANTAN Sampler が共用するMIDI通信層です。

- `midi_driver.*`: トランスポート共通のMIDIメッセージ処理
- `midi_transport_ble.*`: BLE MIDI
- `midi_transport_uart.*`: UART MIDI
- `midi_transport_usb.*`: USB MIDI、HIDキーボード、デジタルゲームパッド
- `usb_host_probe.*`: USB Host列挙の診断と旧式機器向け互換処理

アプリ側は原則として `main/task_midi.*` を経由し、このフォルダを直接操作しません。

詳細は [MIDIモジュール解説](../../docs/development/core/midi-module.md) を参照してください。
