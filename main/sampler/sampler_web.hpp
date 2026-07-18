// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_WEB_HPP
#define KANTAN_SAMPLER_WEB_HPP

#if defined(KANPLAY_SAMPLER)

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace sampler_ns {

// HTTPタスクは状態を直接変更せず、JSON要求をアプリ更新ループへ渡す。
bool sampler_web_enqueue_command(const uint8_t* data, size_t size);
bool sampler_web_export_state(std::string& out);
// SD操作の直前にアプリ更新ループへ全音停止を依頼し、完了まで待つ。
// remount=true はSDの再初期化もメインループ側で直列に実行する。
bool sampler_web_prepare_storage_operation(bool remount = false);
// ファイルサーバーの利用開始を本体UIへ通知する。
void sampler_web_note_client_access(void);

struct sampler_web_audio_t {
  const int16_t* pcm = nullptr;
  uint32_t frames = 0;
  uint32_t sample_rate = 0;
};
bool sampler_web_get_audio(bool background, uint8_t pad, sampler_web_audio_t& out);

} // namespace sampler_ns

#endif // defined(KANPLAY_SAMPLER)
#endif // KANTAN_SAMPLER_WEB_HPP
