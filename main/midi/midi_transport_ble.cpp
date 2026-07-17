// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#if 1

#include "midi_transport_ble.hpp"

#if __has_include(<esp_bt.h>)

#include "../system_registry.hpp"

#include <M5Unified.h>

#include <BLEDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <deque>
#include <vector>

#include <esp_bt.h>
#include <esp32-hal-bt.h>
#include <mutex>

#define MIDI_SERVICE_UUID         "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MIDI_CHARACTERISTIC_UUID  "7772e5db-3868-4112-a1a9-f2669d106bf3"

namespace midi_driver {

static std::mutex mutex_rx;

//----------------------------------------------------------------

static MIDI_Transport_BLE* _instance = nullptr;

// peripheral BLE client
static BLEClient* _pClient = nullptr;

static BLEServer *pServer = nullptr;
static BLEService *pService = nullptr;
static BLEAdvertising *pAdvertising = nullptr;
static BLECharacteristic *pCharacteristic = nullptr;
static int _conn_id = -1;
// static std::deque<std::vector<uint8_t> > _rx_queue;
static std::vector<uint8_t> _rx_data;
static uint8_t _rx_running_status = 0;
static volatile uint32_t _rx_packet_count = 0;
static volatile uint8_t _last_rx_length = 0;
static uint8_t _last_rx_data[6] = {};
static char _central_device_name[24] = {};
static char _central_device_address[18] = {};
static char _peripheral_device_address[18] = {};
static uint8_t _central_midi_properties = 0;
static bool _central_is_m_vave = false;
static uint8_t _subscription_attempts = 0;
static uint32_t _subscription_next_retry_msec = 0;
static uint32_t _subscription_rx_packet_base = 0;
static volatile uint8_t _m_vave_auth_state = 0;
static uint8_t _m_vave_cccd_value = 0xFF;
static volatile uint8_t _local_notify_registration_status = 0xFF;

#if defined(CONFIG_BLUEDROID_ENABLED)
class SamplerSecurityCallbacks final : public BLESecurityCallbacks {
public:
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
};

static SamplerSecurityCallbacks sampler_security_callbacks;
static BLESecurity sampler_security;

static void configure_ble_bonding(void)
{
  // CoreS3 has no numeric-entry UI.  Just Works bonding is appropriate for a
  // local MIDI controller and lets controllers retain KANTAN as an authorized peer.
  sampler_security.setAuthenticationMode(ESP_LE_AUTH_BOND);
  sampler_security.setCapability(ESP_IO_CAP_NONE);
  sampler_security.setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  sampler_security.setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setSecurityCallbacks(&sampler_security_callbacks);
}
#endif
static uint8_t _central_subscription = 0;

static void note_received_packet(const uint8_t* data, size_t length)
{
  ++_rx_packet_count;
  const size_t copied = std::min<size_t>(length, sizeof(_last_rx_data));
  for (size_t i = 0; i < copied; ++i) { _last_rx_data[i] = data[i]; }
  _last_rx_length = copied;
}

static bool is_m_vave_name(const String& name)
{
  String upper_name = name;
  upper_name.toUpperCase();
  return upper_name.indexOf("M-VAVE") >= 0
      || upper_name.indexOf("SMC-PAD") >= 0
      || upper_name.indexOf("SMC PAD") >= 0;
}

// InstaChordと直結時のCharacteristic
static BLERemoteCharacteristic* remotecharacteristic = nullptr;
static uint16_t _mtu_size = 23;

// static constexpr const size_t _tx_queue_size = 4;
// static int _tx_queue_index = 0;
// static std::vector<uint8_t> _tx_queue[_tx_queue_size];

#if defined (CONFIG_BLUEDROID_ENABLED)
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
    _conn_id = pServer->getConnId();
    snprintf(_peripheral_device_address, sizeof(_peripheral_device_address), "%s",
             BLEAddress(param->connect.remote_bda).toString().c_str());
    _instance->setPeripheralConnected(true);
// printf("BLE MIDI Connected.\n");
// pServer->updatePeerMTU(_conn_id, _mtu_size);
    // M5.Lcd.printf("BLE MIDI Connected. MTU:%d\n", _mtu_size);
  };
  void onDisconnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
    _conn_id = -1;
    _peripheral_device_address[0] = 0;
    _instance->setPeripheralConnected(false);
// printf("BLE MIDI Disconnect.\n");
  }
  void onMtuChanged(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
// printf("BLE onMtuChanged : %d\n", param->mtu.mtu);
  }
};

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic, esp_ble_gatts_cb_param_t *param) override {
    note_received_packet(pCharacteristic->getData(), pCharacteristic->getLength());
    _instance->decodeReceive(pCharacteristic->getData(), pCharacteristic->getLength());
    // GATT callback is invoked from the Bluetooth host task, not an ISR.
    _instance->execTaskNotify();
    // printf("onWrite called.\n");
    // fflush(stdout);
  }
  // void onRead(BLECharacteristic *pCharacteristic) override {
  //   printf("onRead called.\n");
  // }
  // void onNotify(BLECharacteristic *pCharacteristic) override {
  //   printf("onNotify called.\n");
  // }
  // void onStatus(BLECharacteristic *pCharacteristic, Status s, uint32_t code) override {
  //   if (s == SUCCESS_NOTIFY || s == SUCCESS_INDICATE) {
  //     // ESP_LOGV("BLE", "onStatus: success");
  //     printf("onStatus: success\n");
  //   } else {
  //     // ESP_LOGE("BLE", "onStatus: error %d, code %d", s, code);
  //     printf("onStatus: error %d, code %d", s, code);
  //   }
  //   fflush(stdout);
  // }
};
#endif

#if defined (CONFIG_NIMBLE_ENABLED)
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
    _conn_id = pServer->getConnId();
    _instance->setPeripheralConnected(true);
  };
  void onDisconnect(BLEServer *pServer, ble_gap_conn_desc *desc) override {
    _conn_id = -1;
    _instance->setPeripheralConnected(false);
  }
  void onMtuChanged(BLEServer *pServer, ble_gap_conn_desc *desc, uint16_t mtu) override {
  }
};

class MyCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic, ble_gap_conn_desc *desc) override {
    note_received_packet(pCharacteristic->getData(), pCharacteristic->getLength());
    _instance->decodeReceive(pCharacteristic->getData(), pCharacteristic->getLength());
    // GATT callback is invoked from the Bluetooth host task, not an ISR.
    _instance->execTaskNotify();
  }
};
#endif

static MyServerCallbacks myServerCallbacks;

void MIDI_Transport_BLE::decodeReceive(const uint8_t* data, size_t length)
{
  if (length < 2) { return; }
  // BLE-MIDIは先頭のtimestamp highに続き、各メッセージのtimestamp lowと
  // MIDIデータが並ぶ。Running Statusを含む標準パケットに加え、timestampを
  // 省略する機器も受け入れる。後者は一般的なBLE MIDIホストが寛容に扱うため、
  // コントローラ互換性のためにここでもフォールバックする。
  std::vector<uint8_t> rxtmp;
  size_t i = 1; // 先頭はtimestamp high
  while (i < length) {
    // BLE-MIDI timestamp-low is any byte with bit 7 set (80-FF), not only
    // 80-BF.  A timestamp is unambiguous when followed by a status byte, or
    // by data while a running status is active.  This also keeps compatibility
    // with controllers that omit timestamp-low before an explicit status.
    const bool high_bit = (data[i] & 0x80) != 0;
    const bool next_is_status = i + 1 < length && (data[i + 1] & 0x80) != 0;
    const bool timestamp_before_running = high_bit && _rx_running_status >= 0x80
      && i + 1 < length && !next_is_status;
    if (high_bit && (next_is_status || timestamp_before_running)) { ++i; }
    if (i >= length) { break; }
    uint8_t status = _rx_running_status;
    if (data[i] & 0x80) {
      status = data[i++];
      if (status < 0xF0) { _rx_running_status = status; }
    }
    if (status < 0x80) { break; }

    uint8_t data_count = ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) ? 1 : 2;
    // リアルタイムメッセージは演奏入力へ渡さず、Running Statusも維持する。
    if (status >= 0xF8) { continue; }
    if (status >= 0xF0 || i + data_count > length) { break; }
    if ((data[i] & 0x80) || (data_count == 2 && (data[i + 1] & 0x80))) { break; }
    rxtmp.push_back(status);
    rxtmp.insert(rxtmp.end(), data + i, data + i + data_count);
    i += data_count;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_rx);
    _rx_data.insert(_rx_data.end(), rxtmp.begin(), rxtmp.end());
  }
}

static std::vector<BLEAdvertisedDevice> ble_scan(void)
{
  std::vector<BLEAdvertisedDevice> foundMidiDevices;
  BLEScan* pBLEScan = BLEDevice::getScan();
  if (pBLEScan == nullptr) {
    return foundMidiDevices;
  }

  BLEUUID serviceUUID(MIDI_SERVICE_UUID);

  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->clearResults();
  auto foundDevices = pBLEScan->start(1);
  ESP_LOGV("BLE", "Found %d BLE device(s)", foundDevices->getCount());
  for (int i=0; i < foundDevices->getCount(); i++) {
    BLEAdvertisedDevice device = foundDevices->getDevice(i);
    auto deviceStr = "name = \"" + device.getName() + "\", address = "  + device.getAddress().toString();
    String name = device.getName();
    // M-VAVE SMC-PAD variants omit the MIDI UUID from advertising on some
    // firmware revisions.  Prefer their advertised name before probing other
    // generic BLE devices in the room.
    const bool m_vave = is_m_vave_name(name);
    if (m_vave) {
      foundMidiDevices.insert(foundMidiDevices.begin(), device);
      continue;
    }
    if (device.haveServiceUUID() && device.isAdvertisingService(serviceUUID)) {
      ESP_LOGV("BLE", " - BLE MIDI device : %s", deviceStr.c_str());
      // Prefer peripherals that explicitly advertise the MIDI service.
      foundMidiDevices.insert(foundMidiDevices.begin(), device);
    }
    else {
      ESP_LOGV("BLE", " - Other type of BLE device : %s", deviceStr.c_str());
      // Several BLE MIDI controllers (including some M-VAVE firmware) omit
      // the MIDI service UUID from advertising data.  They are still valid
      // candidates; the GATT service check after connection is authoritative.
      if (device.getName().length()) {
        foundMidiDevices.push_back(device);
      }
    }
  }
  ESP_LOGV("BLE", "Total of BLE MIDI devices : %d", foundMidiDevices.size());
  pBLEScan->clearResults();
  return foundMidiDevices;
;
/*
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);
  pBLEScan->clearResults();
  foundMidiDevices.clear();
  BLEScanResults foundDevices = pBLEScan->start(3);
  pBLEScan->clearResults();
  pBLEScan->setAdvertisedDeviceCallbacks(&adv_cb);
  pBLEScan->start(60, true);
*/
}



MIDI_Transport_BLE::~MIDI_Transport_BLE()
{
  end();
}

bool MIDI_Transport_BLE::begin(void)
{
  _is_begin = false;
  return true;
}

void MIDI_Transport_BLE::end(void)
{
  if (_is_begin) {
    _is_begin = false;
    if (_conn_id >= 0) {
      pServer->disconnect(_conn_id);
      _conn_id = -1;
    }
  }
}

void MIDI_Transport_BLE::addMessage(const uint8_t* data, size_t length)
{
  uint32_t msec = M5.millis();
  int len = length + (_tx_runningStatus != data[0] ? 2 : 0);
  if (_tx_data.size() + len >= _mtu_size - 1) {
    // If the tx_data size exceeds the buffer size, send it immediately
    sendFlush();
  }
  if (_tx_data.empty()) {
    uint32_t msec_high = 0x3F & (msec >> 7);
    _tx_data.push_back(0x80 | msec_high);
    _tx_runningStatus = 0;
  }

  if (_tx_runningStatus != data[0])
  {
    uint32_t msec_low = (msec & 0x7F);
    _tx_data.push_back(0x80 | msec_low);
    _tx_data.push_back(data[0]); // status byte
    _tx_runningStatus = data[0];
  }
  _tx_data.insert(_tx_data.end(), data + 1, data + length);
  if (_tx_data.size() + 4 >= _mtu_size - 3) {
    // If the tx_data size exceeds the buffer size, send it immediately
    sendFlush();
  }
}

bool MIDI_Transport_BLE::sendFlush(void)
{
  bool result = false;
ESP_LOGV("BLE", "sendFlush called, tx_data size: %d", _tx_data.size());
// printf("sendFlush called, tx_data size: %d\n", _tx_data.size());
  auto remote = remotecharacteristic;
  if (remote)
  {
    result = true;
    if (!_tx_data.empty()) {
      remote->writeValue(_tx_data.data(), _tx_data.size(), false);
    }
  } else if (pCharacteristic && _conn_id >= 0) {
    result = true;
    if (!_tx_data.empty()) {
      pCharacteristic->setValue( _tx_data.data(), _tx_data.size());
      pCharacteristic->notify();
    }
  }
  _tx_data.clear();
  _tx_runningStatus = 0;
  return result;
}

std::vector<uint8_t> MIDI_Transport_BLE::read(void)
{
  std::lock_guard<std::mutex> lock(mutex_rx);
  auto rxValueVec = _rx_data;
  _rx_data.clear();
  return rxValueVec;
}

uint32_t MIDI_Transport_BLE::getReceivedPacketCount(void) const
{
  return _rx_packet_count;
}

void MIDI_Transport_BLE::getConnectionDiagnostic(bool* central, bool* peripheral, uint8_t* subscription) const
{
  if (central != nullptr) { *central = _central_connected; }
  if (peripheral != nullptr) { *peripheral = _peripheral_connected; }
  if (subscription != nullptr) { *subscription = _central_subscription; }
}

void MIDI_Transport_BLE::getCentralDeviceName(char* name, size_t size) const
{
  if (name == nullptr || size == 0) { return; }
  snprintf(name, size, "%s", _central_device_name);
}

void MIDI_Transport_BLE::getPeerAddresses(char* central, size_t central_size, char* peripheral, size_t peripheral_size) const
{
  if (central != nullptr && central_size != 0) {
    snprintf(central, central_size, "%s", _central_device_address);
  }
  if (peripheral != nullptr && peripheral_size != 0) {
    snprintf(peripheral, peripheral_size, "%s", _peripheral_device_address);
  }
}

uint8_t MIDI_Transport_BLE::getCentralMIDIProperties(void) const
{
  return _central_midi_properties;
}

void MIDI_Transport_BLE::getSecurityDiagnostic(uint8_t* auth_state, uint8_t* cccd_value,
                                               uint8_t* registration_status) const
{
  if (auth_state != nullptr) { *auth_state = _m_vave_auth_state; }
  if (cccd_value != nullptr) { *cccd_value = _m_vave_cccd_value; }
  if (registration_status != nullptr) { *registration_status = _local_notify_registration_status; }
}

bool MIDI_Transport_BLE::clearCentralBond(void)
{
#if defined(CONFIG_BLUEDROID_ENABLED)
  if (_central_device_address[0] == 0) { return false; }
  BLEAddress address{String(_central_device_address)};
  return esp_ble_remove_bond_device(*address.getNative()) == ESP_OK;
#else
  return false;
#endif
}

void MIDI_Transport_BLE::getLastReceivedPacket(uint8_t* data, size_t* length) const
{
  const size_t copied = std::min<size_t>(_last_rx_length, sizeof(_last_rx_data));
  if (data != nullptr) {
    for (size_t i = 0; i < copied; ++i) { data[i] = _last_rx_data[i]; }
  }
  if (length != nullptr) { *length = copied; }
}
/*
size_t MIDI_Transport_BLE::read(uint8_t* data, size_t length)
{
  if (_use_rx == false) { return 0; }
  if (_conn_id < 0) { return 0; }
  size_t result = 0;

  while (!_rx_queue.empty())
  {
    std::vector<uint8_t> rxValueVec = _rx_queue.front();
    _rx_queue.pop_front();
    size_t copy_length = std::min(length, rxValueVec.size());
    std::copy(rxValueVec.begin(), rxValueVec.begin() + copy_length, data);
    result += copy_length;
    length -= copy_length;
    data += copy_length;
    if (length == 0) { break; }
  }
  return result;
}
//*/

static void notifyCallback(BLERemoteCharacteristic* pBLERemoteCharacteristic, uint8_t* pData, size_t length, bool isNotify) {
  // printf("Notify callback for characteristic %d ", isNotify ? 1 : 0);
  // printf(pBLERemoteCharacteristic->getUUID().toString().c_str());
  // printf(" of data length : %d  data:", length);
  // for (int i = 0; i < length; i++) {
  //   printf("%02x ", pData[i]);
  // }
  // printf("\n");
  // fflush(stdout);
#if !defined(CONFIG_BLUEDROID_ENABLED)
  if (_instance != nullptr && length) {
    note_received_packet(pData, length);
    _instance->decodeReceive(pData, length);
    // Notifications are delivered on the Bluetooth host task as well.
    _instance->execTaskNotify();
  }
#else
  (void)pBLERemoteCharacteristic;
  (void)pData;
  (void)length;
  (void)isNotify;
#endif
}

#if defined(CONFIG_BLUEDROID_ENABLED)
static void sampler_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t,
                                        esp_ble_gattc_cb_param_t* param)
{
  if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT) {
    _local_notify_registration_status = (uint8_t)param->reg_for_notify.status;
    return;
  }
  if (event != ESP_GATTC_NOTIFY_EVT || _instance == nullptr
   || param->notify.value == nullptr || param->notify.value_len == 0) { return; }

  // Arduino BLEのBLERemoteCharacteristicはhandleマップの照合に失敗すると
  // 通知自体が届いていてもコールバックを呼ばない。購読済みの中央接続は
  // BLE MIDIのみなので、GATTC通知をここで直接受け取る。
  note_received_packet(param->notify.value, param->notify.value_len);
  _instance->decodeReceive(param->notify.value, param->notify.value_len);
  _instance->execTaskNotify();
}
#endif

static void service_m_vave_subscription(void)
{
  if (!_central_is_m_vave || remotecharacteristic == nullptr
   || _subscription_attempts >= 4
   || _rx_packet_count != _subscription_rx_packet_base) {
    return;
  }
  const uint32_t now = M5.millis();
  if ((int32_t)(now - _subscription_next_retry_msec) < 0) { return; }

  // registerForNotify()のローカルコールバック登録は維持したまま、M-VAVE側の
  // CCCDだけを再度有効化する。暗号化確立が最初の書込みより遅れた場合に効く。
  auto descriptor = remotecharacteristic->getDescriptor(BLEUUID((uint16_t)0x2902));
  if (descriptor != nullptr) {
    uint8_t notify_enabled[] = { 0x01, 0x00 };
    descriptor->writeValue(notify_enabled, sizeof(notify_enabled), true);
    const String value = descriptor->readValue();
    _m_vave_cccd_value = value.length() ? (uint8_t)value[0] : 0xFF;
  }
  ++_subscription_attempts;
  _central_subscription = _subscription_attempts;
  _subscription_next_retry_msec = now + 650 + 250 * _subscription_attempts;
}

void MIDI_Transport_BLE::service(void)
{
  if (_central_connected) {
    service_m_vave_subscription();
    return;
  }
  if (!(_use_tx || _use_rx) || _central_connected || _peripheral_connected
   || _pClient != nullptr || _connecting) { return; }
  const uint32_t now = M5.millis();
  if (now - _last_central_scan_msec < 2000) { return; }
  _last_central_scan_msec = now;

  // Reuse the established connection setup path without tearing down the BLE
  // controller or its peripheral advertisement.
  const bool use_tx = _use_tx;
  const bool use_rx = _use_rx;
  _use_tx = false;
  _use_rx = false;
  setUseTxRx(use_tx, use_rx);
}

class MyClientCallback : public BLEClientCallbacks {
  void onConnect(BLEClient* pclient) {
    ESP_LOGV("BLE", "ble client: onConnect");
    // printf("ble client: onConnect\n");
    // fflush(stdout);
    // _instance->setCentralConnected(true);
  }

  void onDisconnect(BLEClient* pclient) {
    // connected = false;
    ESP_LOGV("BLE", "ble client: onDisconnect\n");
    // printf("ble client: onDisconnect\n");
    // fflush(stdout);
    remotecharacteristic = nullptr;
    _central_device_name[0] = 0;
    _central_device_address[0] = 0;
    _central_midi_properties = 0;
    _central_subscription = 0;
    _central_is_m_vave = false;
    _subscription_attempts = 0;
    _subscription_next_retry_msec = 0;
    _m_vave_auth_state = 0;
    _m_vave_cccd_value = 0xFF;
    _local_notify_registration_status = 0xFF;
    if (pclient == _pClient) {
      _pClient = nullptr;
      delete pclient;
    }
    _instance->setCentralConnected(false);
  }
};
static MyClientCallback myClientCallback;

void MIDI_Transport_BLE::updateState(void)
{
  _connected = _central_connected || _peripheral_connected;

  auto midiport_info = kanplay_ns::def::command::midiport_info_t::mp_off;

  if (_connecting) {
    midiport_info = kanplay_ns::def::command::midiport_info_t::mp_connecting;
  } else if (_connected) {
    midiport_info = kanplay_ns::def::command::midiport_info_t::mp_connected;
  } else if (_use_tx || _use_rx) {
    midiport_info = kanplay_ns::def::command::midiport_info_t::mp_enabled;
  }
  kanplay_ns::system_registry->runtime_info.setMidiPortStateBLE(midiport_info);
}

void MIDI_Transport_BLE::setCentralConnected(bool connected)
{
  _central_connected = connected;
  updateState();
}

void MIDI_Transport_BLE::setPeripheralConnected(bool connected)
{
  _peripheral_connected = connected;
  updateState();
  auto adv = pAdvertising;
  if (adv != nullptr) {
    if (connected) {
      adv->stop();
    } else {
      adv->start();
    }
  }
}

void MIDI_Transport_BLE::setUseTxRx(bool use_tx, bool use_rx)
{
  _instance = this;
  if (_use_tx == use_tx && _use_rx == use_rx) { return; }

  auto midi_service_uuid = BLEUUID(MIDI_SERVICE_UUID);
  auto midi_characteristic_uuid = BLEUUID(MIDI_CHARACTERISTIC_UUID);

  bool prev_en = _use_tx || _use_rx;
  bool new_en = use_tx || use_rx;
  if (prev_en != new_en) {
    _rx_data.clear();
    if (new_en) {
      if (!_is_begin) {
        _is_begin = true;
        // BLEDevice::setMTU(_mtu_size);
        BLEDevice::init(_config.device_name);
#if defined(CONFIG_BLUEDROID_ENABLED)
        configure_ble_bonding();
        BLEDevice::setCustomGattcHandler(sampler_gattc_event_handler);
#endif
        // BLEDevice::setMTU(_mtu_size);
        pServer = BLEDevice::createServer();
        pServer->setCallbacks(&myServerCallbacks);

        pService = pServer->createService(midi_service_uuid);
        pCharacteristic = pService->createCharacteristic(
                            midi_characteristic_uuid,
                            BLECharacteristic::PROPERTY_READ   |
                            BLECharacteristic::PROPERTY_WRITE_NR|
                            BLECharacteristic::PROPERTY_NOTIFY
                          );
        if (pCharacteristic != nullptr) {
          pCharacteristic->setCallbacks(new MyCallbacks());
          pCharacteristic->addDescriptor(new BLE2902());
          pCharacteristic->setNotifyProperty(true);

          BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
          oAdvertisementData.setFlags(ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
          oAdvertisementData.setCompleteServices(midi_service_uuid);
          oAdvertisementData.setName(_config.device_name);
          pAdvertising = pServer->getAdvertising();
          pAdvertising->setMinPreferred(0x06); // 7.5msec  (6 x 1.25msec)
          pAdvertising->setMaxPreferred(0x0C); // 15.0msec (12 x 1.25msec)
          pAdvertising->setAdvertisementData(oAdvertisementData);
        }
      }
      pService->start();
      pAdvertising->start();
    } else {
      if (_conn_id >= 0 && pServer != nullptr) {
        pServer->disconnect(_conn_id);
        _conn_id = -1;
      }
      if (pAdvertising != nullptr) { pAdvertising->stop(); }
      if (pService != nullptr) { pService->stop(); }
    }

    _instance->setCentralConnected(false);
    if (_pClient != nullptr) {
      remotecharacteristic = nullptr;
      _pClient->disconnect();
      // delete _pClient;
      // _pClient = nullptr;
    }

    if (new_en) {
      // M5.delay(128); // Wait for advertising to start
  // BLEDevice::setMTU(_mtu_size);
      auto foundMidiDevices = ble_scan();
      if (!foundMidiDevices.empty()) {
        _connecting = true;
        // Advertising data is not reliable enough to identify every BLE MIDI
        // controller.  Probe candidates one by one and accept only the device
        // that exposes the standard MIDI GATT service/characteristic.
        for (auto& device : foundMidiDevices) {
          const bool m_vave = is_m_vave_name(device.getName());
          // SMC-PAD Pocketはボンド済み接続の再開時に、GATT接続とCCCD設定は
          // 成功してもMIDI通知を送らない固体がある。M-VAVEだけは保存鍵を
          // 使わず、標準BLE MIDIの非ボンド接続として扱う。
#if defined(CONFIG_BLUEDROID_ENABLED)
          if (m_vave) {
            esp_ble_remove_bond_device(*device.getAddress().getNative());
            _m_vave_auth_state = 0;
            M5.delay(40);
          }
#endif
          BLEClient* pClient = BLEDevice::createClient();
          if (pClient == nullptr) { break; }
          pClient->setClientCallbacks(&myClientCallback);
          if (!pClient->connect(&device)) {
            delete pClient;
            continue;
          }
          M5.delay(m_vave ? 120 : 16);
          auto remoteservice = pClient->getService(midi_service_uuid);
          BLERemoteCharacteristic* rc = nullptr;
          std::vector<BLERemoteCharacteristic*> midi_characteristics;
          if (remoteservice != nullptr) {
            auto characteristics = remoteservice->getCharacteristicsByHandle();
            for (const auto& entry : *characteristics) {
              auto candidate = entry.second;
              if (candidate != nullptr && candidate->getUUID().equals(midi_characteristic_uuid)
               && (candidate->canNotify() || candidate->canIndicate())) {
                midi_characteristics.push_back(candidate);
                if (rc == nullptr) { rc = candidate; }
              }
            }
          }
          if (rc != nullptr && (rc->canNotify() || rc->canIndicate())) {
            _central_midi_properties = (rc->canNotify() ? 0x01 : 0)
                                     | (rc->canIndicate() ? 0x02 : 0)
                                     | (rc->canWrite() ? 0x04 : 0)
                                     | (rc->canWriteNoResponse() ? 0x08 : 0)
                                     | (rc->getDescriptor(BLEUUID((uint16_t)0x2902)) != nullptr ? 0x10 : 0);
            _central_is_m_vave = m_vave;
            _subscription_rx_packet_base = _rx_packet_count;
            for (auto midi_characteristic : midi_characteristics) {
              midi_characteristic->registerForNotify(notifyCallback,
                                                      midi_characteristic->canNotify());
            }
            if (m_vave) {
              auto cccd = rc->getDescriptor(BLEUUID((uint16_t)0x2902));
              if (cccd != nullptr) {
                const String value = cccd->readValue();
                _m_vave_cccd_value = value.length() ? (uint8_t)value[0] : 0xFF;
              }
            }
            remotecharacteristic = rc;
            _pClient = pClient;
            _subscription_attempts = 1;
            _central_subscription = 1;
            _subscription_next_retry_msec = m_vave ? M5.millis() + 500 : 0;
            snprintf(_central_device_name, sizeof(_central_device_name), "%s",
                     device.getName().length() ? device.getName().c_str() : device.getAddress().toString().c_str());
            snprintf(_central_device_address, sizeof(_central_device_address), "%s",
                     device.getAddress().toString().c_str());
            _instance->setCentralConnected(true);
            break;
          }
          if (pClient->isConnected()) {
            pClient->disconnect();
          }
          delete pClient;
        }
      }
    } else if (_is_begin) {
      // Wi-Fi AP/STAの開始前にBLEコントローラを完全停止する。単に広告を止める
      // だけでは無線・内部RAMを保持し、Wi-Fi初期化が失敗する個体がある。
      BLEDevice::deinit(false);
      pCharacteristic = nullptr;
      pAdvertising = nullptr;
      pService = nullptr;
      pServer = nullptr;
      remotecharacteristic = nullptr;
      _pClient = nullptr;
      _conn_id = -1;
      _central_is_m_vave = false;
      _subscription_attempts = 0;
      _subscription_next_retry_msec = 0;
      _m_vave_auth_state = 0;
      _m_vave_cccd_value = 0xFF;
      _local_notify_registration_status = 0xFF;
#if defined(CONFIG_BLUEDROID_ENABLED)
      BLEDevice::setCustomGattcHandler(nullptr);
#endif
      _is_begin = false;
      _tx_data.clear();
      _tx_runningStatus = 0;
    }
  }
  _connecting = false;
  _use_tx = use_tx;
  _use_rx = use_rx;
  updateState();
}

//----------------------------------------------------------------

} // namespace midi_driver

#endif

#endif
