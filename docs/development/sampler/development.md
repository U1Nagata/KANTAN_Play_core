# KANTAN Sampler 開発ガイド

KANTAN Play core と同一のハードウェア（M5Stack CoreS3 + KANTAN Play base）で動作する、
**別ファームウェア**としてのサンプラーマシンの開発ドキュメント。
製品仕様は [製品仕様](./product-spec.md) を参照。

## 全体戦略

- **同一リポジトリ・同一ソースツリーからのビルドバリアント**として開発する
  - `-DKANPLAY_SAMPLER=1` が定義されたビルドでは、`main/main.cpp` が無効化され、
    `main/sampler/sampler_app.cpp` がエントリポイント（`setup()`/`loop()`）になる
  - ハードウェア制御層（I2C/I2S/SPI・KANTAN Play base の電源投入・STM32 通信）や
    GUI/メニュー基盤の資産をそのまま参照できる
- 将来 OTA カタログに「KANTAN Play / KANTAN Sampler」の選択肢を追加し、
  メニューから相互に切り替えられるようにする（両者は同じパーティション構成を使用）

## ビルド環境（platformio.ini）

| 環境名 | 用途 |
|---|---|
| `sampler_s3` | CoreS3 サンプラー **リリースビルド** |
| `sampler_s3_debug` | CoreS3 サンプラー デバッグビルド |
| `sampler_native_m1mac` | Mac (Apple Silicon) SDL2 シミュレータ |

```bash
# サンプラー リリースビルド
pio run -e sampler_s3

# 実機書き込み
pio run -e sampler_s3 -t upload

# シミュレータ
pio run -e sampler_native_m1mac
```

リリースビルド時、`generate_user_custom.py` により以下が自動生成される：

- `docs/firmware/KANTAN_Sampler_CoreS3_full.bin` — ESP Web Tools 用フルバイナリ
- `ota_bin/KANTAN_Sampler_CoreS3_OTA.bin` — OTA 用アプリバイナリ

## 開発者向け USB 書き込みページ

`docs/sampler.html`（GitHub Pages 公開後は `https://<account>.github.io/<repo>/sampler.html`）から
ブラウザ（Chrome/Edge）+ USB で書き込み可能。マニフェストは `docs/manifest_sampler.json`。

## ファイル構成

```
main/
  sampler/                  # サンプラー専用コード（KANPLAY_SAMPLER 時のみ有効）
    sampler_app.cpp         #   エントリポイント・入力処理・LED制御・画面描画
    sampler_audio.cpp/hpp   #   サンプル再生エンジン（I2S 48kHz・12ボイス・リサンプル対応）
    sampler_samples.hpp     #   検証用組み込みサンプル（docs/Sample_Sound の WAV を埋め込み）
    sampler_define.hpp      #   モード enum・Pad 定義などの共通定義
    sampler_version.hpp     #   サンプラー独自のバージョン定義
  main.cpp                  # KANTAN Play エントリポイント（サンプラー時は無効）
  task_i2c.cpp              # 共有（サンプラー時は gui 呼び出し1行のみ無効化）
  （その他は KANTAN Play と共有）
```

## オーディオ経路（調査結果）

- ESP32 は **I2S スレーブ**。クロックは base 上の SI5351（MCLK 6.144MHz）が供給し、
  ES8388 が I2S マスターとして動作。実効サンプルレートは **48kHz / 32bit / stereo**
- ES8388 の ADC には SAM 音源・マイク・ライン入力の音が流れ込み、ESP32 は
  read → マスターボリューム適用 → write のパススルーを行う（`task_i2s.cpp`）
- サンプラーはこのループに PCM ボイスをミキシングして音を出す（`sampler_audio.cpp`）。
  44.1kHz 等のサンプルは 16.16 固定小数の線形補間でリサンプル（将来のピッチ変更にも流用可）
- 録音（マイク/Resample）は I2S read 側のデータを取り込むことで実現できる見込み

## 入力・LED 経路（調査結果）

- ボタン: STM32（I2C 0x56）→ `internal_kanplay.cpp` → `system_registry->internal_input`
  のボタンビットマスク履歴。MAIN_01〜15（左4列＝4×3 Pad、右端列＝Fn×3）、
  SUB_1〜4（上段4ボタン＝SOUND/PLAY/REC/FX）、ENC1/2/3・レバー(SIDE)・KNOB も同経路
- LED: `system_registry->rgbled_control.setColor(index, r|g<<8|b<<16)`（index はボタン番号 0〜18）
- タッチ: `internal_input` の TOUCH_VALUE 履歴から取得（task_i2c が M5.update() を実行）
- IMU: `task_i2c` がBMI270から読み取った最新加速度XYZとジャイロXYZを `internal_imu` へ奇数/偶数シーケンス付きで公開する。SamplerはI2Cを直接読まず、同一スナップショットをロックなしで取得する。左右操作は押下直後のバイアスを引いたジャイロXを低頻度で積分する

- 名前空間は `sampler_ns`（KANTAN Play 側は `kanplay_ns`）
- コーディング規約は [KANTAN Play core アーキテクチャ](../core/architecture.md) に準拠

## 資産の再利用マップ（仕様 → 既存モジュール）

| サンプラー仕様 | 再利用する既存資産 | 備考 |
|---|---|---|
| 12個メインPad・Fnボタン・LED | `main/task_i2c.cpp` + `main/in_i2c/internal_kanplay.hpp`（STM32 通信） | 入力ビット→コマンド変換は `task_commander.cpp` のマッピング機構を参考に |
| スピーカー出力・マイク入力 | `main/task_i2s.cpp`（ES8388 コーデック） | サンプル再生ミキサー・録音パスは新規実装 |
| TFカード（サンプル保存） | `main/file_manage.cpp`（SdFat/VFS） | WAV 読み書きは新規実装 |
| 画面描画基盤 | `main/gui/gui_base.inl` の UI 部品パターン | サンプラー用画面は `main/sampler/` 配下に新設 |
| メニューシステム | `main/menu_data/` の `MENU_BUILDER`・`mi_*_t` パターン | サンプラー設定メニューに流用 |
| 状態管理・コマンドキュー | `main/system_registry.hpp` の registry/キュー機構 | サンプラー用に縮小版を用意するか要検討 |
| 電源制御・起動シーケンス | `main/main.cpp` の AW9523 電源投入手順 | `sampler_app.cpp` に反映済み |
| WiFi・OTA | `main/task_wifi.cpp` / `task_http_client.cpp` | フェーズ後半で組み込み |

## 開発フェーズ

- **Phase 0（完了）**: ビルド環境整備・エントリポイント分離・画面骨組み
  （SOUND/PLAY/REC/FX のモードタブ表示・タッチで切替）
- **Phase 1（完了）**: 入力/LED/再生の垂直スライス —
  Pad・Fn・モードボタン・エンコーダ入力（task_i2c 再利用）、Pad 押下で組み込みサンプル発音、
  LED・画面連動、波形表示、ENC1 でマスターボリューム、タッチ演奏
- **Phase 2（完了）**: PSRAMサンプルプール（5MB・48kHz/16bit/mono正規化・Long素材上限20秒）、
  Chop SliceはLong PCM Assetを共有し、分割後にPCMを重複保持しない設計、
  SD `/sampler/*.wav` の起動時ロード（名前順最大12個、SDなしは組み込みサンプルへフォールバック）、
  One/Hold/Repeat の3再生方式（PLAYモードで Fn+Pad で設定、Padにバッジ表示）、
  SOUND モードの DEL+Pad 削除、プール使用量表示、再生中Padのハイライト
- **Phase 3（完了）**: SOUND モード — 内蔵Mic(16kHz)/外部入力(48kHz)の自動判定録音、
  自動Crop/Normalize、即 Pad 化（`RECxx`）
- **Phase 4（完了）**: EDIT — 非破壊 Start/End/Volume/Reverse/Synth 編集、
  4/8/12/Auto CHOP、Beatの64グリッドを基準とした自動速度変換
- **Phase 5（完了）**: REC モード — イベント記録（重み付きクオンタイズ・早押し補正）、
  1ms 周期の再生タスク、タイムライン表示、MUTE/DEL、ループ長確定
- **Phase 6（一部完了）**: FX モード — Pitch / Filter / Repeat（Fn押下中のみ適用、ENC2でパラメータ）。
  Crush/Vinyl/Gate 等は未実装
- **Phase 7（未着手）**: 配布 — OTA カタログへの登録、KANTAN Play ⇔ Sampler の相互切替
- **その他未実装**: 永続保存（サンプル・EDITメタ・LOOPイベントは電源OFFで消える）、
  キット保存/切替、BPM連動、レバー Beat Repeat

> **注**: 実装済みプログラム仕様の正確な記録は [プログラム仕様](./program-spec.md) を正とする。
