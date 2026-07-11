// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

// サンプル再生エンジン (KANPLAY_SAMPLER ビルド時のみ有効)
// I2S 初期化部は main/task_i2s.cpp と同一構成 (ES8388がI2Sマスター、48kHz/32bit/stereo)

#if defined (KANPLAY_SAMPLER)

#include <M5Unified.h>

#include "sampler_audio.hpp"

#include "../common_define.hpp"
#include "../system_registry.hpp"

#if !defined (M5UNIFIED_PC_BUILD)

#if __has_include(<driver/i2s_std.h>)
 #include <driver/i2s_std.h>
#else
 #include <driver/i2s.h>
#endif

#endif

namespace sampler_ns {
namespace kp = kanplay_ns;
//-------------------------------------------------------------------------

static constexpr const uint16_t i2s_dma_frame_num = 96;
static constexpr const uint32_t output_sample_rate = sampler_audio_t::sample_rate;

//-------------------------------------------------------------------------
// ボイス管理

struct voice_t {
  const int16_t* pcm = nullptr;  // モノラルPCMデータ先頭
  uint32_t frames = 0;           // 総フレーム数
  uint64_t pos_fp = 0;           // 再生位置 (16.16固定小数、フレーム単位)
  uint32_t base_step_fp = 0;     // FXなしの再生ステップ
  uint32_t step_fp = 0;          // 現在の再生ステップ
  bool loop = false;
  bool reverse = false;
  uint16_t volume_q8 = 256;
  volatile bool active = false;
};

static voice_t voices[sampler_audio_t::max_voice];
static volatile bool output_muted = false;

struct fx_state_t {
  volatile bool active = false;
  volatile int8_t param = 0;
};

static fx_state_t fx[3];
static int32_t filter_l = 0;
static int32_t filter_r = 0;

struct recorder_t {
  volatile bool active = false;
  volatile bool overflow = false;
  int16_t* buffer = nullptr;
  volatile uint32_t frames = 0;
  uint32_t capacity = 0;
};

static recorder_t recorder;

static inline uint32_t pitch_step_fp(uint32_t base_step)
{
  if (!fx[0].active || fx[0].param == 0) { return base_step; }
  int param = fx[0].param * 2;
  if (param > 100) { param = 100; }
  if (param < -100) { param = -100; }
  uint32_t ratio_q8 = param > 0
    ? (uint32_t)(256 + (param * 256) / 100)   // +100 = 2x
    : (uint32_t)(256 + (param * 128) / 100);  // -100 = 0.5x
  if (ratio_q8 < 64) { ratio_q8 = 64; }
  return (uint32_t)(((uint64_t)base_step * ratio_q8) >> 8);
}

static void update_voice_steps(void)
{
  for (auto& voice : voices) {
    if (voice.active) { voice.step_fp = pitch_step_fp(voice.base_step_fp); }
  }
}

bool sampler_audio_t::play(uint8_t voice, const int16_t* pcm, uint32_t frames, uint32_t sample_rate,
                           bool loop, bool reverse, uint16_t volume_q8)
{
  if (voice >= max_voice || pcm == nullptr || frames == 0 || sample_rate == 0) { return false; }

  auto& v = voices[voice];
  v.active = false;  // 再生中の再トリガに備え一旦停止してから書き換える
  v.pcm = pcm;
  v.frames = frames;
  v.base_step_fp = (uint32_t)(((uint64_t)sample_rate << 16) / output_sample_rate);
  v.step_fp = pitch_step_fp(v.base_step_fp);
  v.pos_fp = 0;
  v.loop = loop;
  v.reverse = reverse;
  v.volume_q8 = volume_q8;
  v.active = true;
  return true;
}

void sampler_audio_t::stop(uint8_t voice)
{
  if (voice < max_voice) { voices[voice].active = false; }
}

void sampler_audio_t::stopAll(void)
{
  for (auto& v : voices) { v.active = false; }
}

bool sampler_audio_t::isPlaying(uint8_t voice)
{
  return (voice < max_voice) && voices[voice].active;
}

void sampler_audio_t::setOutputMuted(bool muted)
{
  output_muted = muted;
}

static int8_t clamp_fx_param(int value)
{
  if (value < -100) { value = -100; }
  if (value > 100) { value = 100; }
  return (int8_t)value;
}

void sampler_audio_t::setFx(uint8_t index, bool active, int8_t param)
{
  if (index >= 3) { return; }
  fx[index].param = clamp_fx_param(param);
  fx[index].active = active;
  if (index == 0) { update_voice_steps(); }
}

void sampler_audio_t::setFxActive(uint8_t index, bool active)
{
  if (index < 3) {
    fx[index].active = active;
    if (index == 0) { update_voice_steps(); }
  }
}

void sampler_audio_t::setFxParam(uint8_t index, int8_t param)
{
  if (index >= 3) { return; }
  fx[index].param = clamp_fx_param(param);
  if (index == 0) { update_voice_steps(); }
}

void sampler_audio_t::setFxQuantizeStepMs(uint32_t step_ms)
{
  (void)step_ms;  // Repeatはsampler_app側のLOOPイベント再生で処理する。
}

bool sampler_audio_t::startRecording(int16_t* buffer, uint32_t capacity_frames, uint32_t initial_frames)
{
  if (buffer == nullptr || capacity_frames == 0 || initial_frames >= capacity_frames) { return false; }
  recorder.active = false;
  recorder.buffer = buffer;
  recorder.capacity = capacity_frames;
  recorder.frames = initial_frames;
  recorder.overflow = false;
  recorder.active = true;
  return true;
}

uint32_t sampler_audio_t::stopRecording(void)
{
  recorder.active = false;
  return recorder.frames;
}

uint32_t sampler_audio_t::recordingFrames(void)
{
  return recorder.frames;
}

bool sampler_audio_t::isRecording(void)
{
  return recorder.active;
}

bool sampler_audio_t::recordingOverflowed(void)
{
  return recorder.overflow;
}

// 1フレーム分のボイス合成値を求めて加算する (16bit値を32bitフルスケールに拡張して加算)
static inline int64_t mix_voices(void)
{
  int64_t mixed = 0;
  for (size_t n = 0; n < sampler_audio_t::max_voice; ++n) {
    auto& v = voices[n];
    if (!v.active) { continue; }
    uint32_t idx = (uint32_t)(v.pos_fp >> 16);
    if (idx >= v.frames) {
      if (v.loop && v.frames) { v.pos_fp = 0; idx = 0; }
      else { v.active = false; continue; }
    }
    uint32_t frac = v.pos_fp & 0xFFFF;
    uint32_t sample_idx = v.reverse ? (v.frames - 1 - idx) : idx;
    int32_t s0 = v.pcm[sample_idx ];
    int32_t s = s0;
    if (frac != 0) {
      uint32_t idx1 = idx + 1;
      uint32_t sample_idx1;
      if (v.reverse) {
        sample_idx1 = idx1 >= v.frames ? (v.loop ? v.frames - 1 : sample_idx) : v.frames - 1 - idx1;
      } else {
        sample_idx1 = idx1 >= v.frames ? (v.loop ? 0 : idx) : idx1;
      }
      int32_t s1 = v.pcm[sample_idx1];
      s += ((s1 - s) * (int32_t)frac) >> 16;
    }
    if (v.volume_q8 != 256) { s = (int32_t)(((int64_t)s * v.volume_q8) >> 8); }
    mixed += (int64_t)s << 16;
    v.pos_fp += v.step_fp;
  }
  return mixed;
}

static inline int32_t saturate32(int64_t value)
{
  if (value > INT32_MAX) { return INT32_MAX; }
  if (value < INT32_MIN) { return INT32_MIN; }
  return (int32_t)value;
}

static inline void process_master_fx(int32_t& l, int32_t& r)
{
  if (!fx[1].active) { return; }
  if (fx[1].active && fx[1].param != 0) {
    int param = fx[1].param * 2;
    if (param > 100) { param = 100; }
    if (param < -100) { param = -100; }
    int amount = param < 0 ? -param : param;
    int shift = 1 + (amount * 7) / 100;
    filter_l += (l - filter_l) >> shift;
    filter_r += (r - filter_r) >> shift;
    if (param < 0) {
      l = filter_l;            // negative: low-pass
      r = filter_r;
    } else {
      l = l - filter_l;        // positive: high-pass
      r = r - filter_r;
    }
  } else {
    filter_l = l;
    filter_r = r;
  }

}

static inline int16_t input_to_pcm16(int32_t l, int32_t r)
{
  int32_t mono = ((l >> 16) + (r >> 16)) >> 1;
  if (mono > INT16_MAX) { return INT16_MAX; }
  if (mono < INT16_MIN) { return INT16_MIN; }
  return (int16_t)mono;
}

static inline void record_input_frame(int32_t l, int32_t r)
{
  if (!recorder.active) { return; }
  uint32_t pos = recorder.frames;
  if (pos >= recorder.capacity) {
    recorder.overflow = true;
    recorder.active = false;
    return;
  }
  recorder.buffer[pos] = input_to_pcm16(l, r);
  recorder.frames = pos + 1;
}

//-------------------------------------------------------------------------
// I2S 初期化 (task_i2s.cpp と同一設定)

#if !defined (M5UNIFIED_PC_BUILD)

// CoreS3内蔵マイク(M5.Mic/ES7210)が I2S_NUM_1 を使うため、
// KANTAN base側の出力/パススルーは I2S_NUM_0 で動かす。
static constexpr const i2s_port_t i2s_port = I2S_NUM_0;
static constexpr const uint16_t i2s_dma_desc_num = 4;

#if __has_include(<driver/i2s_std.h>)

static i2s_chan_handle_t _i2s_tx_handle = nullptr;
static i2s_chan_handle_t _i2s_rx_handle = nullptr;
static esp_err_t _i2s_init(void)
{
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG((i2s_port_t)i2s_port, I2S_ROLE_SLAVE);
  chan_cfg.dma_desc_num = i2s_dma_desc_num;
  chan_cfg.dma_frame_num = i2s_dma_frame_num >> 1;
  esp_err_t err = i2s_new_channel(&chan_cfg, &_i2s_tx_handle, &_i2s_rx_handle);
  if (err != ESP_OK) { return err; }
  i2s_std_config_t i2s_config;
  memset(&i2s_config, 0, sizeof(i2s_std_config_t));
  i2s_config.clk_cfg.clk_src = i2s_clock_src_t::I2S_CLK_SRC_PLL_160M;
  i2s_config.clk_cfg.sample_rate_hz = 48000; // dummy setting
  i2s_config.slot_cfg.data_bit_width = i2s_data_bit_width_t::I2S_DATA_BIT_WIDTH_32BIT;
  i2s_config.slot_cfg.slot_bit_width = i2s_slot_bit_width_t::I2S_SLOT_BIT_WIDTH_32BIT;
  i2s_config.slot_cfg.slot_mode = i2s_slot_mode_t::I2S_SLOT_MODE_STEREO;
  i2s_config.slot_cfg.slot_mask = i2s_std_slot_mask_t::I2S_STD_SLOT_BOTH;
  i2s_config.slot_cfg.ws_width = 32;
  i2s_config.slot_cfg.ws_pol = false;
  i2s_config.slot_cfg.bit_shift = true;
#if SOC_I2S_HW_VERSION_1    // For esp32/esp32-s2
  i2s_config.slot_cfg.msb_right = false;
#else
  i2s_config.slot_cfg.left_align = true;
  i2s_config.slot_cfg.big_endian = false;
  i2s_config.slot_cfg.bit_order_lsb = false;
#endif
  i2s_config.gpio_cfg.bclk = kp::def::hw::pin::i2s_bck;
  i2s_config.gpio_cfg.ws   = kp::def::hw::pin::i2s_ws;
  i2s_config.gpio_cfg.dout = kp::def::hw::pin::i2s_out;
  i2s_config.gpio_cfg.mclk = kp::def::hw::pin::i2s_mclk;
  i2s_config.gpio_cfg.din  = kp::def::hw::pin::i2s_in;
  err = i2s_channel_init_std_mode(_i2s_tx_handle, &i2s_config);
  err = i2s_channel_init_std_mode(_i2s_rx_handle, &i2s_config);

  return ESP_OK;
}

static esp_err_t _i2s_start(void) {
  if (_i2s_tx_handle == nullptr) { return ESP_FAIL; }
  return i2s_channel_enable(_i2s_tx_handle) || i2s_channel_enable(_i2s_rx_handle);
}

static esp_err_t _i2s_write(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_channel_write(_i2s_tx_handle, buf, len, result, tick);
}

static esp_err_t _i2s_read(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_channel_read(_i2s_rx_handle, buf, len, result, tick);
}

#else

static esp_err_t _i2s_init(void)
{
    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config_t));
    i2s_config.mode                 = (i2s_mode_t)( I2S_MODE_SLAVE | I2S_MODE_TX | I2S_MODE_RX );
    i2s_config.sample_rate          = 48000; // dummy setting
    i2s_config.bits_per_sample      = i2s_bits_per_sample_t::I2S_BITS_PER_SAMPLE_32BIT;
    i2s_config.channel_format       = i2s_channel_fmt_t::I2S_CHANNEL_FMT_RIGHT_LEFT;
    i2s_config.communication_format = i2s_comm_format_t::I2S_COMM_FORMAT_STAND_I2S;
    i2s_config.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    i2s_config.tx_desc_auto_clear   = false;
    i2s_config.use_apll             = false;
    i2s_config.fixed_mclk           = 0;
    i2s_config.mclk_multiple        = i2s_mclk_multiple_t::I2S_MCLK_MULTIPLE_DEFAULT;
    i2s_config.bits_per_chan        = i2s_bits_per_chan_t::I2S_BITS_PER_CHAN_32BIT;
#if I2S_DRIVER_VERSION > 1
    i2s_config.dma_desc_num         = i2s_dma_desc_num;
    i2s_config.dma_frame_num        = i2s_dma_frame_num >> 1;
#else
    i2s_config.dma_buf_count        = i2s_dma_desc_num;
    i2s_config.dma_buf_len          = i2s_dma_frame_num >> 1;
#endif
    esp_err_t err;
    if (ESP_OK != (err = i2s_driver_install(i2s_port, &i2s_config, 0, nullptr)))
    {
      M5_LOGE("i2s_driver_install: %d", err);
      return err;
    }

    i2s_pin_config_t pin_config;
    memset(&pin_config, ~0u, sizeof(i2s_pin_config_t)); /// all pin set to I2S_PIN_NO_CHANGE
    pin_config.bck_io_num     = kp::def::hw::pin::i2s_bck;
    pin_config.ws_io_num      = kp::def::hw::pin::i2s_ws;
    pin_config.data_out_num   = kp::def::hw::pin::i2s_out;
    pin_config.data_in_num    = kp::def::hw::pin::i2s_in;
    pin_config.mck_io_num     = kp::def::hw::pin::i2s_mclk;
    err = i2s_set_pin(i2s_port, &pin_config);
    if (ESP_OK != err)
    {
      M5_LOGE("i2s_set_pin: %d", err);
      return err;
    }

  return ESP_OK;
}

static esp_err_t _i2s_start(void) {
  return i2s_start(i2s_port);
}

static esp_err_t _i2s_write(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_write(i2s_port, buf, len, result, tick);
}

static esp_err_t _i2s_read(void* buf, size_t len, size_t* result, TickType_t tick) {
  return i2s_read(i2s_port, buf, len, result, tick);
}

#endif

#endif

static int32_t* bufdata = nullptr;
static constexpr const size_t buf_size = (i2s_dma_frame_num) * sizeof(int32_t);

bool sampler_audio_t::start(void)
{
  int len = kp::system_registry->raw_wave_length;
  auto wav_buf = kp::system_registry->raw_wave;
  for (int i = 0; i < len; ++i) {
    wav_buf[i] = std::make_pair(128, 128);
  }

#if defined (M5UNIFIED_PC_BUILD)
  auto thread = SDL_CreateThread((SDL_ThreadFunction)task_func, "i2s", this);
  (void)thread;
#else
  // メモリブロックの断片化への対策 (task_i2s.cpp と同じ手順)
  auto dummy = m5gfx::heap_alloc_dma(heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  bufdata = (int32_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
  m5gfx::heap_free(dummy);
  if (bufdata == nullptr) {
    bufdata = (int32_t*)heap_caps_malloc(buf_size, MALLOC_CAP_DMA);
  }
  memset(bufdata, 0, buf_size);

  _i2s_init();

  xTaskCreatePinnedToCore((TaskFunction_t)task_func, "i2s", 1024*3, this, kp::def::system::task_priority_i2s, nullptr, kp::def::system::task_cpu_i2s);
#endif
  return true;
}

// 波形表示用リングバッファに min/max を記録 (task_i2s.cpp と同一形式)
static inline void push_raw_wave(int32_t min_level, int32_t max_level)
{
  min_level = ((min_level >> 16) + 32768 + 128) >> 8;
  max_level = ((max_level >> 16) + 32768 + 128) >> 8;

  auto wav_buf = kp::system_registry->raw_wave;
  auto raw_wave_pos = kp::system_registry->raw_wave_pos;
  wav_buf[raw_wave_pos ++] = std::make_pair<uint8_t, uint8_t>(min_level, max_level);
  if (raw_wave_pos >= (kp::system_registry->raw_wave_length)) {
    raw_wave_pos = 0;
  }
  kp::system_registry->raw_wave_pos = raw_wave_pos;
}

void sampler_audio_t::task_func(sampler_audio_t* me)
{
  (void)me;
#if defined (M5UNIFIED_PC_BUILD)
  // PCビルドでは実音声出力は行わず、ボイスを進めて波形表示のみ更新する
  static int32_t pcbuf[i2s_dma_frame_num];
  for (;;) {
    int32_t min_level = INT32_MAX;
    int32_t max_level = INT32_MIN;
    for (int i = 0; i < i2s_dma_frame_num; i += 2) {
      int64_t l = 0, r = 0;
      int64_t mixed = mix_voices();
      l += mixed;
      r += mixed;
      int32_t ll = saturate32(l);
      int32_t rr = saturate32(r);
      process_master_fx(ll, rr);
      pcbuf[i  ] = ll;
      pcbuf[i+1] = rr;
      record_input_frame(pcbuf[i], pcbuf[i+1]);
      if (min_level > pcbuf[i  ]) { min_level = pcbuf[i  ]; }
      if (max_level < pcbuf[i  ]) { max_level = pcbuf[i  ]; }
      if (min_level > pcbuf[i+1]) { min_level = pcbuf[i+1]; }
      if (max_level < pcbuf[i+1]) { max_level = pcbuf[i+1]; }
    }
    push_raw_wave(min_level, max_level);
    SDL_Delay(1);
  }
#else
  int32_t* i2sbuf = bufdata;

  size_t transfer_size;

#if __has_include(<driver/i2s_std.h>)
  do {
    i2s_channel_preload_data(_i2s_tx_handle, i2sbuf, buf_size, &transfer_size);
  } while (transfer_size == buf_size);
#endif

  _i2s_start();

  int32_t current_volume = 0;
  int shifted_volume = 0;

  for (;;) {
    _i2s_read(i2sbuf, buf_size, &transfer_size, 128);
    kp::system_registry->task_status.setWorking(kp::system_registry_t::reg_task_status_t::bitindex_t::TASK_I2S);

    // マスターボリュームのレンジ0~100を 1~256に変換 (task_i2s.cpp と同一)
    int32_t target_volume = kp::system_registry->user_setting.getMasterVolume() << 8;
    if (target_volume > 25600) { target_volume = 25600; }
    if (current_volume != target_volume) {
      current_volume += (target_volume - current_volume + (target_volume < current_volume ? 0 : 32)) >> 5;
      shifted_volume = current_volume / 100;
    }

    int32_t min_level = INT32_MAX;
    int32_t max_level = INT32_MIN;

    for (int i = 0; i < i2s_dma_frame_num; i += 2) {
      // 入力(SAM音源/マイク)のパススルーにボイスをミキシング
      record_input_frame(i2sbuf[i], i2sbuf[i+1]);
      int64_t l = output_muted ? 0 : i2sbuf[i  ];
      int64_t r = output_muted ? 0 : i2sbuf[i+1];
      if (!output_muted) {
        int64_t mixed = mix_voices();
        l += mixed;
        r += mixed;
      }
      int32_t ll = saturate32(l);
      int32_t rr = saturate32(r);
      if (!output_muted) { process_master_fx(ll, rr); }
      if (min_level > ll) { min_level = ll; }
      if (max_level < ll) { max_level = ll; }
      if (min_level > rr) { min_level = rr; }
      if (max_level < rr) { max_level = rr; }
      i2sbuf[i  ] = (ll >> 8) * shifted_volume;
      i2sbuf[i+1] = (rr >> 8) * shifted_volume;
    }
    push_raw_wave(min_level, max_level);

    kp::system_registry->task_status.setSuspend(kp::system_registry_t::reg_task_status_t::bitindex_t::TASK_I2S);
    _i2s_write(bufdata, buf_size, &transfer_size, 128);
  }
#endif
}

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif // defined (KANPLAY_SAMPLER)
