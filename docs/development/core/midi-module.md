# MIDI モジュール解説

`main/midi/` は、KANTAN Play core と KANTAN Sampler が共用するMIDI通信層です。
アプリ側は原則として `main/task_midi.*` を経由し、このフォルダはバイト列の送受信、
メッセージ復号、USB/BLE機器の管理を担当します。

## 構成

| ファイル | 役割 |
|---|---|
| `midi_driver.hpp` | `MIDI_Message`、抽象トランスポート `MIDI_Transport`、ストリームデコーダ、アプリ向け `MIDIDriver` を定義。 |
| `midi_driver.cpp` | MIDIメッセージ長の判定、送信、ランニングステータス対応のストリーム復号を実装。 |
| `midi_transport_ble.hpp` | BLE MIDIの公開API。スキャン結果、選択中機器、診断、ボンディング、Central / Peripheral接続状態を定義。 |
| `midi_transport_ble.cpp` | BLE MIDI GATTのクライアント／サーバー実装。受信復号、探索・接続・再接続、購読、ペアリング、M-VAVE互換処理を担当。 |
| `midi_transport_uart.hpp` | UART MIDIの設定と公開API。標準速度は31,250 bps。 |
| `midi_transport_uart.cpp` | UART初期化、RXタスク、バイトバッファ、送信時のランニングステータス圧縮、FreeRTOS通知を実装。 |
| `midi_transport_usb.hpp` | USB MIDIのDevice / Host API、USB HIDキーボード・デジタルゲームパッド入力、Host診断APIを定義。 |
| `midi_transport_usb.cpp` | TinyUSB DeviceモードとESP-IDF USB Hostモードを実装。MIDI/HID列挙、Host給電、イベント受信、USB MIDI送信を担当。 |
| `usb_host_probe.hpp` | Device Info画面用の、USB Host列挙診断データ構造を定義。 |
| `usb_host_probe.cpp` | ESP-IDF USB Hostの診断ラッパーと、旧式機器の列挙互換処理を実装。 |

## データフロー

```text
task_midi / sampler input handling
  -> MIDIDriver
    -> MIDI_Transport (BLE, UART, or USB)
      -> physical MIDI device

incoming bytes
  -> MIDI_Transport::read()
    -> MIDI_Decoder
      -> MIDI_Message
```

`MIDI_Transport` が物理通信を受け持ち、`MIDIDriver` がバイトストリームを通常の
MIDIメッセージに変換します。`sendNoteOn`、`sendNoteOff`、`sendControlChange`、
`sendProgramChange` などは `MIDIDriver` のヘルパーです。

## ファイル別の補足

### `midi_driver.hpp` / `midi_driver.cpp`

- MIDI解析はここに集約し、BLE/UART/USB固有の分岐をドライバに入れない。
- `MIDI_Decoder` は `read()` をまたいでデータを保持し、ランニングステータスに対応する。
  不完全な受信パケットをトランスポート側で捨てないこと。
- `MIDI_Transport::service()` は任意の定期処理。BLEでは遅延接続処理に使い、
  他のトランスポートは軽量なままにできる。

### `midi_transport_ble.hpp` / `midi_transport_ble.cpp`

- CoreS3がスマートフォン／PC向けのPeripheralとして振る舞う経路と、外部BLE MIDI
  コントローラーへ接続するCentralの両方に対応する。
- 探索一覧は `max_scan_devices` 件まで。名称、アドレス、RSSI、広告内のMIDI宣言の有無を保持する。
- Central側はMIDI UUIDを広告しない機器も候補に含める。これは接続後にMIDIサービスを公開する
  M-VAVEのような機器に必要。
- ペアリング／ボンド、CCCD購読の再試行、受信パケット診断はBLE/GATT固有なのでここに置く。
- MIDIタスクから `service()` を定期的に呼ぶ。探索、再接続、購読復帰をオーディオ処理を止めずに進める。

### `midi_transport_uart.hpp` / `midi_transport_uart.cpp`

- ハードウェアMIDIシリアルポート用。
- RXは専用FreeRTOSタスクで動き、トランスポートの通知ハンドルでMIDIタスクを起こす。
- TXはメッセージをバッファし、安全な範囲で重複したチャンネルステータスを省略する。

### `midi_transport_usb.hpp` / `midi_transport_usb.cpp`

- 接続したPC向けのUSB MIDI **Device**モードと、外部コントローラー向けのUSB MIDI
  **Host**モードに対応する。同じUSBポートを使うため、両者は排他的。
- HostモードではHIDキーボードとWindows互換デジタルゲームパッドも扱う。アナログ軸は意図的に無視する。
- `getHostDiagnostic()` は列挙失敗をUIで確認するためのもの。演奏の高速経路では使わない。
- USB Hostの給電・モード切替には再初期化が必要な場合がある。再起動判断はアプリ／メニュー側で行い、
  USBスタックのライフサイクルはこのトランスポートに任せる。

### `usb_host_probe.hpp` / `usb_host_probe.cpp`

- 接続、リセット、ディスクリプタ、コントロール転送の結果を記録し、未対応USB機器を
  Device Info画面から調査できるようにする。
- 一部の旧式MIDIコントローラー向けに、列挙中の短い待機時間と、誤った8バイトではなく
  完全な9バイトのConfiguration Descriptor Headerを要求する互換処理を入れている。
- ESP-IDF内部に依存するラッパーなので、変更は最小限にし、ESP-IDF更新後は標準機器と旧式機器の
  両方でUSB MIDI Hostを確認する。

## 変更時の確認事項

1. MIDI解析は `midi_driver` に集約し、トランスポート非依存を保つ。
2. オーディオ・ボタンの高速経路では、メモリ確保、探索、診断を避ける。
3. 新しい外部トランスポートは、呼び出し側に条件を増やさず `MIDI_Transport` を実装する。
4. 入力経路を変えた後は、USB Host / Device切替、BLE再接続、UART入力、複数ノートを含むBLEパケットを確認する。
