// SPDX-License-Identifier: MIT
// Copyright (c) 2025 InstaChord Corp.

#ifndef KANPLAY_TASK_WIFI_HPP
#define KANPLAY_TASK_WIFI_HPP

/*
task_wifi は WiFi通信を利用するタスクです。
*/

namespace kanplay_ns {
//-------------------------------------------------------------------------
class task_wifi_t {
public:
    void start(void);
    // NVSに保存済みのSTA設定があるか。Wi-Fiドライバを起動せず確認できる。
    static bool hasSavedSTAConfig(void);
private:
    static void task_func(task_wifi_t* me);
};

//-------------------------------------------------------------------------
}; // namespace kanplay_ns

#endif
