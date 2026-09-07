# KANTAN Sampler Manual Image Briefs

マニュアル本文に合わせて制作する画像の指示書です。撮影・キャプチャー後は、対応するIDのHTMLコメント位置へ画像を挿入します。

## 共通ルール

- 実機写真は正面に近い角度で、押すボタンと画面を同時に確認できるようにする
- 画面キャプチャーは表示文字が読める解像度にする
- 選択中Pad、点灯中LED、モードが本文の手順と一致する状態で撮る
- 1画像では1つの判断または操作を説明する
- 手や矢印を入れる場合も、画面表示とボタン全体を隠さない
- 日本語本文へ組み込む画像内に、別の用語説明を大量に書き込まない

## 画像一覧

| ID | 掲載ページ | 必要な内容 |
|---|---|---|
| `QS-01` | はじめての5分 | SAMPLER + PLAYの全体写真。初期8 Sample、4モード、右端Fn列、上下ダイヤルが分かる |
| `ADD-01` | Sampleを追加する | SAMPLER + SOUND。P9〜P12が空であることが分かる全画面 |
| `ADD-02` | Sampleを追加する | 空Pad選択後の`TAP: IMPORT / HOLD: RECORD`表示 |
| `REC-01` | マイクで録音する | `SAMPLING / MIC INPUT`と録音先Pad番号 |
| `REC-02` | マイクで録音する | `SAMPLE SAVED`と録音済みPad |
| `EDIT-01` | Sampleを編集する | Edit全画面。波形、Start、End、Vol、Pitch、Fn1 / Fn2 / Fn3が読める |
| `EDIT-02` | Sampleを編集する | 波形上のStart / End境界と、選択中の境界 |
| `FILE-01` | File Editor | Sampleタブ。Assignment target、Location、Play、Assignが同時に見える |
| `BEAT-01` | Beatを選ぶ | Beatメニューで`Select Beat`を選んだ状態 |
| `BEAT-02` | Beatを選ぶ | 内蔵Beat一覧とFn1 Preview表示 |
| `LOOP-01` | 演奏をRecする | SAMPLER + REC。Beat再生中、Recイベントなし |
| `LOOP-02` | 演奏をRecする | Samplerイベントを2〜4個記録したREC画面 |
| `FX-01` | FXで変化させる | FX待機画面。各Padの役割とTargetが読める |
| `FX-02` | FXで変化させる | Filterを押し、パラメーターを表示した状態 |
| `MIX-01` | FXで変化させる | Mixerの6パートとMix 1〜4が分かる |
| `CHOP-01` | 曲をChopして遊ぶ | Sample EditのP1 `Chop`と、対象のStart / End範囲が分かる |
| `CHOP-02` | 曲をChopして遊ぶ | Chop画面のFIT、KEEP、TAP CUT、4 / 8 / 12 / MANUALと分割線 |
| `CHOP-03` | 曲をChopして遊ぶ | Chop後のP1から並ぶSliceと`HOLD: MAKE LOOP`表示 |
| `MUSIC-01` | 曲を流しながら演奏する | `Load Music`のファイル一覧と選択中の曲名 |
| `MUSIC-02` | 曲を流しながら演奏する | MUSIC + PLAY。曲情報とP5〜P8の再生操作 |
| `SYNTH-01` | Sampleから楽器を作る | Sample EditからP8 `Synth`を選ぶ状態 |
| `SYNTH-02` | Sampleから楽器を作る | Sample SynthのSustain、In、Out、Relと波形上の区間 |
| `SAMPLE-ADV-01` | Sampleの応用操作 | Hold、Repeat、Reverse、Chokeの状態が分かるSample Edit画面 |
| `SAMPLE-ADV-02` | Sampleの応用操作 | `MOVE / MIX`と移動先の選び方が分かる画面 |
| `BEAT-SET-01` | Beatとタイミングを調整する | Tap Tempoの入力画面とOK / Back |
| `MUSIC-PART-01` | 音楽パートを演奏する | BASSまたはMELODYの12 Padと音名 |
| `MUSIC-PART-02` | 音楽パートを演奏する | CHORDの7コードと5 Modifier |
| `PERF-REC-01` | 演奏全体をWAVへ録音する | Performance Recording待機中のヘッダーとFn1 |
| `PERF-REC-02` | 演奏全体をWAVへ録音する | 録音終了後の`TAP: SAVE / HOLD: DELETE` |
| `SAVE-01` | ProjectとSample Kit | Project保存時のUpdate / Copy / New候補 |
| `WIFI-01` | Wi-FiとUpdate | Smartphone Setupの接続用QR、SSID、Password |
| `WIFI-02` | Wi-FiとUpdate | 黄色の`UP!`またはUpdate進捗画面 |
| `EXT-01` | 外部コントローラー | BLE機器一覧または接続確認画面 |
| `EXT-02` | 外部コントローラー | Learnの割り当て先選択と外部入力待ち |

## ファイル名

次の名前で`docs/docs/ja/sampler/assets/images/`へ保存します。

| ID | ファイル名 |
|---|---|
| `QS-01` | `quickstart-play-overview.jpg` |
| `ADD-01` | `add-sample-empty-pads.jpg` |
| `ADD-02` | `add-sample-import-record.jpg` |
| `REC-01` | `record-sample-mic.jpg` |
| `REC-02` | `record-sample-saved.jpg` |
| `EDIT-01` | `edit-sample-overview.jpg` |
| `EDIT-02` | `edit-sample-start-end.jpg` |
| `FILE-01` | `file-editor-sample.png` |
| `BEAT-01` | `select-beat-menu.jpg` |
| `BEAT-02` | `select-beat-preview.jpg` |
| `LOOP-01` | `record-loop-empty.jpg` |
| `LOOP-02` | `record-loop-events.jpg` |
| `FX-01` | `fx-overview.jpg` |
| `FX-02` | `fx-filter.jpg` |
| `MIX-01` | `mixer-overview.jpg` |
| `CHOP-01` | `chop-source-range.jpg` |
| `CHOP-02` | `chop-settings.jpg` |
| `CHOP-03` | `chop-complete.jpg` |
| `MUSIC-01` | `music-select.jpg` |
| `MUSIC-02` | `music-player.jpg` |
| `SYNTH-01` | `sample-synth-entry.jpg` |
| `SYNTH-02` | `sample-synth-sustain.jpg` |
| `SAMPLE-ADV-01` | `sample-advanced-playback.jpg` |
| `SAMPLE-ADV-02` | `sample-move-mix.jpg` |
| `BEAT-SET-01` | `beat-tap-tempo.jpg` |
| `MUSIC-PART-01` | `music-part-notes.jpg` |
| `MUSIC-PART-02` | `music-part-chords.jpg` |
| `PERF-REC-01` | `performance-record-ready.jpg` |
| `PERF-REC-02` | `performance-record-save.jpg` |
| `SAVE-01` | `project-save.jpg` |
| `WIFI-01` | `wifi-smartphone-setup.jpg` |
| `WIFI-02` | `wifi-update.jpg` |
| `EXT-01` | `external-ble-list.jpg` |
| `EXT-02` | `external-input-learn.jpg` |

現在の`device-play-overview.jpg`はQS-01の仮素材です。差し替え画像が完成するまではそのまま使用します。
