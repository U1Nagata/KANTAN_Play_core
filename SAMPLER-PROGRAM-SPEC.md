# KANTAN Sampler Program Specification

この文書は、KANTAN Samplerファームウェアのプログラム仕様を記録するためのものです。
ユーザー体験や将来構想は `Sampler-specification.md`、ビルド環境や開発方針は
`SAMPLER-DEVELOPMENT.md` を参照してください。

## 対象

- ハードウェア: M5Stack CoreS3 + KANTAN Play base
- ビルド環境: PlatformIO `sampler_s3`
- エントリポイント: `main/sampler/sampler_app.cpp`
- 有効化条件: `KANPLAY_SAMPLER=1`
- 名前空間: `sampler_ns`

## 主要ファイル

| ファイル | 役割 |
|---|---|
| `main/sampler/sampler_app.cpp` | アプリ本体、入力処理、画面描画、SAMPLE/PLAY/LOOP/EDIT状態管理 |
| `main/sampler/sampler_audio.hpp/cpp` | 48kHz I2S再生エンジン、12ボイスミキサー、外部入力録音 |
| `main/sampler/sampler_pool.hpp/cpp` | PSRAM上のサンプルスロット管理、WAV/PCMロード |
| `main/sampler/sampler_wav.hpp` | WAVヘッダ解析 |
| `main/sampler/sampler_define.hpp` | モード、Pad数、Pad再生方式 |
| `main/sampler/sampler_samples.hpp` | SD未使用時の組み込みサンプル |

## サンプルスロット

スロット数は12個です。各Padが1つの `sample_slot_t` に対応します。

`sample_slot_t` の主な状態:

- `pcm`: PSRAM上のPCM16 monoデータ
- `frames`: PCMフレーム数
- `sample_rate`: サンプルの元レート
- `start_frame` / `end_frame`: 非破壊Start/End編集範囲
- `volume_q8`: Pad音量。`256` が100%、最大は現UI上200%
- `pitch_q8`: Padピッチ倍率。`256` が100%、現UI上は50%〜200%
- `reverse`: 逆再生フラグ
- `hold_enabled`: 離したときに停止するか
- `loop_enabled`: 終端で繰り返すか

`playStart()` / `playEnd()` / `playFrames()` は、編集範囲を反映した再生範囲を返します。

## サンプルプール

- プール予算: 6MB
- 内部形式: PCM16 mono
- 最大サンプル長: 16秒
- WAVロード:
  - PCM16 mono/stereoに対応
  - stereoはmonoへ平均化
  - 48kHz以下を想定
- Pad番号:
  - ユーザー向け番号は左下から右へ `1,2,3,4`、中段 `5,6,7,8`、上段 `9,10,11,12`
- 起動時ロード:
  - 起動時はSDカードを読みに行かず、内蔵メモリの組み込みサンプルをロードする
- SDロード:
  - メニューからSD関連機能を開いた時に、必要フォルダ `/sampler/samples` / `/sampler/loops` / `/sampler/kits` を作成する
  - `Reload Samples` では `/sampler/samples/*.wav` をファイル名の若い順に最大12個ロードし、Pad 1から順に配置する
  - SDサンプルが読み込めない場合は内蔵サンプルへ戻す
- 組み込みサンプル:
  - `docs/Sample_Sound/` の8個のWAVを `44.1kHz / PCM16 / mono` に正規化して埋め込み
  - Pad 1-4: KICK, SNARE, CLAP, HAT
  - Pad 5-8: PIKO, COW, CHIN, TOML
  - Pad 9-12: 空欄
  - 下段はKICK/SNARE/CLAP/HATの基本ビート、中段はPIKO／パーカッション／金物／TOML、上段は空欄
- 組み込みBGM:
  - `docs/Sample_Sound/BGM_FA.wav` をプリセットKITのBGMとして埋め込む
  - PCMは実ファイル長のまま保持し、BGM再生をループさせる
  - サンプラーのループ長は実ファイル長の2倍として設定する（2秒WAVなら4秒ループ）
- 終了時自動保存:
  - 電源OFF/Resetコマンドを受けた時、現在のKit状態をLittleFSの `/sampler_resume.json` へ保存する
  - 起動時は `/sampler_resume.json` があれば復元し、無ければ組み込みプリセットをロードする
  - 復元対象はPad割当、Start/End、Volume、Pitch、Reverse、Hold/Loop、BGM、Loopイベント、FX値
  - 内蔵サンプルは `builtin:KICK` のような識別子で保存し、SDなしでも復元できる
  - SD上のWAVを割り当てているPadはSDカード上のファイル参照で復元する。録音直後の未保存PCMはWAV化していないため復元対象外

## オーディオエンジン

`sampler_audio_t` がサンプル再生と外部入力録音を担当します。

- 出力サンプルレート: 48kHz
- 最大ボイス数: 14（12 Pad + BGM + メニュープレビュー）
- 出力経路: KANTAN Play base側 ES8388 / I2S
- I2Sポート:
  - KANTAN Play base出力/入力: `I2S_NUM_0`
  - CoreS3内蔵MicはM5Unified側で別I2Sを使用
- 再生:
  - 16.16固定小数で元サンプルレートから48kHzへ線形補間
  - 再生位置は64bitで保持し、長尺One Shotでも固定小数位置がオーバーフローしない
  - `loop`
  - `reverse`
  - `volume_q8`
  - `setOutputMuted()` により録音中の出力をミュート可能
  - Pitch倍率はFX操作時に各ボイスへ反映し、オーディオフレームごとの倍率計算は行わない
  - Filter/Repeatが無効な間はマスターFX処理をバイパスする
  - 出力直前にピークリミッターを適用し、多重発音時は実出力ピークが約75%を超えないようゲインを自動で下げる
- 発音優先UI:
  - Pad押下/離上直後は短時間描画を遅延し、音声発音を優先する
  - ループ再生中はPad再生ハイライトの定期ポーリングを省き、ボタン/波形更新は必要時にまとめて反映する
  - ループのピアノロール背景はイベント変更時だけ再生成し、通常再生時はカーソル列のみを軽量更新する
- 外部入力録音:
  - I2S入力をPCM16 monoへ変換
  - `startRecording(buffer, capacity, initial_frames)`
  - `recordingFrames()`
  - `stopRecording()`
  - `recordingOverflowed()`

## 画面構成

画面は縦320x240を前提に、`sampler_app.cpp` が直接M5GFXへ描画します。

主な領域:

- ステータスバー: 24px
  - アプリ名
  - PSRAMプール使用量
  - バッテリーアイコン
  - マスターボリューム円形アイコン
- 波形/タイムライン領域
  - 高さ112px
  - 現在モードのボタン色と同じ色で外枠を表示する（メニュー表示中は外枠を表示しない）
  - PLAY通常時: I2S入力/出力の生波形を高さ112pxでリアルタイム表示
  - PLAY中にLOOP再生中: LOOPモードと同じタイムラインを表示
  - SAMPLE時: 入力の生波形は表示せず、選択中Padのサンプル波形を高さ112pxで固定表示
  - SAMPLE録音中: 上画面全体を赤系にし、大きなマイクアイコン、`SAMPLING`、入力ソース、Pad番号を表示する。LOOP録音の `RECORDING` 表示とは別デザインにする
  - EDIT時: 選択サンプルの波形とStart/Endマーカー、中央に選択パラメーター名、左下に値
  - LOOP時: 4拍タイムライン、16分割補助グリッド、記録イベント、再生ヘッド
  - FX時: 3段のパラメータバー
- モードタブ: SAMPLE / PLAY / LOOP / FX
- 4x3 Pad
  - Pad/Fnボタンは44x44pxの正方形
  - 空Pad: 空色
  - サンプル入りPad: サンプル波形サムネイル
  - サムネイルはサンプル登録時に96分割の縮小波形を作成し、演奏中にPCM全体を再走査しない
  - Pad右上バッジ: 再生方式のミニアイコン（One Shot=▶+終端バー / Hold=ゲート波形 / Loop=円弧矢印 / Hold+Loop=円弧矢印+H）
  - ミュート中Pad: 赤いスピーカー✕アイコン＋波形サムネイルを減光表示
- 右列Fnボタン

モードタブの文字は、標準フォントを横1倍・縦2倍にし、1px横に重ね描きして太字風にしています。

## Fnボタンのアイコン

FnボタンはFXモードを除きアイコン表示。Padバッジも同じアイコンの縮小版で視覚言語を統一。

- アイコン素材: **Phosphor Icons (Fill)**（MITライセンス、`tools/sampler_icons/LICENSE`）
- `tools/sampler_icons/rasterize.html`（ブラウザCanvas製の変換ツール）で SVG →
  24px/12px の8bitアルファマップに変換し `main/sampler/sampler_icons.inl` として埋め込み
  （12pxはオフセット重ね描きで太らせ、小サイズの視認性を確保）
- `draw_icon()` がアルファ値で前景色/背景色をブレンドして描画（アンチエイリアス付き・任意色）。
  下地が単色である必要があるため、Padバッジは暗色プレートを敷いた上に描画する
- アイコンを追加する場合: `tools/sampler_icons/svg/` にSVGを追加 → rasterize.html の
  リストに登録 → ローカルHTTPサーバ経由でブラウザで開き、出力を sampler_icons.inl へ反映

| モード | Fn1 | Fn2 | Fn3 |
|---|---|---|---|
| SAMPLE | 鉛筆（EDIT） | ◀◀（Reverse） | ゴミ箱（Delete） |
| EDIT中 | 鉛筆＝Start/Endトグル（Start=橙・左バー / End=青・右バー、選択中は枠線） | スピーカー＝Volume/Pitchトグル（Pitch選択中はP表示） | ドア+矢印（Exit） |
| PLAY | 再生/停止（Loopモードと同じ） | ゲート波形（Hold On/Off） | 円弧矢印（Loop On/Off） |
| LOOP | 未確定=円弧矢印+終端バー（琥珀、ループを閉じる）/ 再生中=■（赤）/ 停止中=▶（緑） | スピーカー✕（Mute） | ゴミ箱（Del） |
| FX | 文字（PITCH） | 文字（FILTER） | 文字（REPEAT） |

## LED

LEDは `system_registry->rgbled_control.setColor()` で制御します。

- Pad LED:
  - Pad位置ごとに5色を循環
  - 押下中、再生中、録音中、EDIT対象は明色
- Fn LED:
  - 押下中に明色
- モードLED:
  - 現在モードは明色
  - 非選択モードは減光

## 入力

入力はKANTAN Play base側STM32から `system_registry->internal_input` へ入ります。

- 12 Pad
- 右列3 Fn
- 上段4モードボタン
- ENC1/ENC2
- タッチパネル

主な操作:

- 上段4ボタン: SAMPLE / PLAY / LOOP / FX 切替
- ENC1: マスターボリューム
- ENC1押し込み: 全音停止。LOOP再生も停止
- ENC2:
  - EDIT中: 現在パラメータ編集
  - FX中: Fnを押しながら選択中FXのパラメータ編集
  - エンコーダー回転は内部カウンタの差分をまとめて反映する。描画が追いつかない場合でも、読み取った差分ぶん値を進めて最終値を描画する

## メニュー

通常UIで扱えるサンプル編集項目はメニューへ重複配置せず、キット、ループ、外部接続、Wi-Fi、システム系だけを簡潔にまとめます。
メニュー表示はかんぷれappと同じ思想で、画面上部のインフォメーションエリアにテキストを表示し、Pad/Fnボタンをテンキー/操作キーとして使います。

操作:

- `SIDE_2`: メニュー表示/非表示
- メニュー中の表示エリア:
  - ステータスバー直下からモードボタン（SAMPLE/PLAY/LOOP/FX）領域までをメニューテキスト表示に使う
  - リスト項目は `1 Kit` のように数字インデックス付きで表示する
- Pad/Fnボタン:
  - `1,2,3,0,Exit / 4,5,6,Back,OK / 7,8,9,未割当,未割当` として扱う
  - 数字ボタンは該当インデックスへフォーカス移動
  - `OK`: 決定。値項目は押すたびに次の値へ切替
- `Back`: 1階層戻る。ルートではメニューを閉じる
- 下層からBackした時は、上位メニューの先頭ではなく、戻り元に対応する親項目へフォーカスする
- `Exit`: メニューを閉じる
- 通常時の `ENC2押し込み`: FXモード以外ではメニューを開く。FXモードではFXフォーカス切替
- メニュー中の `ENC2`: 項目移動。先頭/末尾ではループせずクランプする
- メニュー中の `ENC2押し込み`: 決定。値項目は押すたびに次の値へ切替（値項目のみOKで循環する）
- `ENC1押し込み`: 1階層戻る。ルートではメニューを閉じる
- 項目フォーカス移動、決定、Back/Exit時は、かんぷれappと同じ `menu_cursor_sound` / `menu_navigate_sound` コマンドで短いメニュー操作音を鳴らす
- カーソルで表示ウィンドウが1行動く時は、旧位置→新位置へ一方向に滑らかにスクロールする（バウンスしない）
- 階層へ入る/戻る時は、かんぷれappと同様にメニュー表示エリアを横スクロール遷移させる
- スクロール/遷移アニメは速度優先で描画回数を抑える（縦スクロールは中間1フレーム、横遷移は3フレーム）。横遷移は日本語フォント描画を最初の1フレームのみ行い、以降は描画済みスプライトをオフセット違いで貼るだけにする

構成:

- Kit: `Load Kit` / `Save Kit` / `Import Sample` / `New Kit` / `Reload Samples`
  - `Save Kit` は `/sampler/kits/current.json` に、Pad割当、Start/End、Volume、Pitch、Reverse、Hold/Loopフラグ、Loopイベント、FX値を保存する
  - `Load Kit` は `/sampler/kits/*.json` をファイル名順に一覧表示し、選択したKitを読み込む
  - SD上のWAVパスがあるサンプルを復元対象とする。録音直後の未保存PCMをWAVとして書き出す処理は未実装
  - `Import Sample`: `/sampler/samples/*.wav` をファイル名順に一覧表示する。WAVをOKで選ぶと最大2秒のプレビューを再生し、最後に割り当て先Padを押す
  - 割り当て先Pad選択中は全Padボタンを演奏画面と同じ波形付きPad表示にし、Fn3位置をBackとして使う
- Loop: `Load BGM` / `Clear BGM` / `BGM Volume` / `Quantize` / `Note Grid` / `Note Off Grid`
  - `Load BGM` は `/sampler/loops/*.wav` をファイル名順に一覧表示し、選択したWAVを背景ループとして取り込む
  - BGM取り込み時は、そのWAVの長さをループ長に設定し、既存のループ録音イベントはクリアする
  - ループ停止や演奏録音の削除はメインUIで行うため、Loopメニューには重複配置しない
- Input Assign: `Learn` / `Assign List` / `Clear All`
  - Learnは、まず割り当て先のPad、モードボタン、またはSTOP ALLを押し、次に外部MIDIノートを入力する
  - BLE MIDI / USB MIDI / Port C MIDIのNote On / Offと、Port A I2C拡張ボタン入力を共通に受ける。Padは押下で発音、離すとHold発音を停止する
  - Port AはM5ByteButton / M5ExtIO2を最大4台（各8入力、合計32ボタン）まで自動検出する
  - 割り当てはKitデータおよび終了時の復元データに保存する
- Connections: `MIDI Input` / `USB Mode` / `USB Host Power`
- Wi-Fi: `Wi-Fi Setup` / `WPS` / `File Server` / `Wi-Fi Info`
  - `Wi-Fi Setup` はかんぷれappと同じ設定用AP `kanplay-ap`（PASS: `01234567`）を起動する。スマートフォンを接続し、ブラウザで `192.168.4.1` を開いてSSIDとパスワードを登録する
  - 接続情報はWi-FiタスクがNVSへ保存し、以後のFile Server起動時にSTA接続へ自動復帰する
  - `WPS` はWPSプッシュボタン接続を開始する
  - `File Server` ONでWi-Fiファイル操作モードを起動する
- Audio: `Input Source`
  - `Auto` / `Internal` / `External`
- System: `Display` / `LED` / `Language` / `Info` / `Reset All`

## SAMPLEモード

目的: 空Padへ音を録って即Pad化し、録音済みサンプルの選択・編集・整理を行います。

### 空Pad押下

録音開始:

1. 既存再生を全停止
2. 出力をミュート
3. 外部入力を約200msプローブ
4. 外部入力に十分な信号がある場合は外部入力録音
5. そうでなければCoreS3内蔵Mic録音

録音終了:

1. 録音停止
2. 出力ミュート解除
3. 先頭100msを破棄
4. 末尾100msを破棄
5. 自動Crop位置を検出
6. Normalize
7. 未CropのPCMを `RECxx` 名でPadへ登録
8. 検出したCrop位置を `start_frame` / `end_frame` へ設定

### サンプル移動 / ミックス

誤操作防止のため、通常の同時押しでは発動しません。

1. SAMPLEモードで音入りPadを約650ms長押し
2. 上画面に `MOVE / MIX` と移動元Padを表示
3. 長押ししたまま別Padを押す

- 移動先が空Pad: サンプルスロットをそのまま移動し、移動元Padを空にする
- 移動先が音入りPad: 移動元と移動先の現在の再生範囲、Reverse、Volume、Pitchを反映して1つのPCMへミックスする
- ミックス時はピークを計算し、約90%を超える場合だけ全体ゲインを下げて音量過大を防ぐ
- 移動元Padのループイベントは移動先Padへ付け替える
- 移動元Padを離すとキャンセル

### 入力ソース

- 内蔵Mic:
  - `M5.Mic`
  - 16kHz
- 外部入力:
  - KANTAN Play base I2S入力
  - 48kHz

外部入力の専用挿入検出信号は使っていません。短時間の入力レベルで判定します。
そのため、外部マイクが挿さっていても完全無音の場合は内蔵Micへフォールバックします。

### 自動Crop/Normalize

`auto_crop_and_normalize()` が担当します。

- ノイズ床を録音冒頭から推定
- 小さめの動的しきい値で有音範囲を検出
- 前方5ms、後方20msの余白を残す
- Start/Endは近傍5ms以内のゼロクロスポイントへ寄せ、クリックノイズを抑える
- 検出範囲のピークを基準に、録音PCM全体を約30000へ正規化
- PCM自体は自動Cropで切り詰めず、再生範囲だけを `start_frame` / `end_frame` に保存する

## PLAYモード

目的: Loopへ記録せず自由に演奏します。

Pad再生方式:

Padごとに `Hold` と `Loop` の2フラグを持ち、組み合わせで再生方式が決まります。

| Hold | Loop | 挙動 |
|---|---|---|
| Off | Off | One Shot。押すと最後まで鳴る |
| On | Off | Gate。押している間だけ鳴る |
| Off | On | Toggle Loop。押すとループ開始、もう一度押すと停止 |
| On | On | Hold Loop。押している間だけループし、離すと停止 |

設定操作:

- Fn1: BGM/Loop再生・停止
- Padを押している間に `HOLD` Fnを押すと、そのPadのHoldをOn/Offする
- Padを押している間に `LOOP` Fnを押すと、そのPadのLoopをOn/Offする
- Pad保持中はFnボタンの画面枠を控えめに表示し、同時押しで追加操作できることを示す。Fn LEDは点灯させず、Playボタンもヒント表示の対象外
- 従来互換として、Fnを押しながらPadを押す操作でもHold/Loopを変更できる

再生には、Start/End、Volume、Pitch、Reverseが反映されます。

## EDIT機能

RECモード通常時に、Padを押している間に `EDIT` Fnを押すと、そのPadを編集対象にしてEDITモードへ入ります。
EDIT中にPadを押すと編集対象を切り替えます。
ただしLOOP再生中はループ音を邪魔しないため、RECモード内のPad選択、REV切替、EDIT選択、EDIT中Pad移動、ENC2押し込みによるプレビュー発音は行わず、表示と選択だけを更新します。

RECモード通常時は、Padを押している間に `REV` Fnを押すと対象PadのReverseをトグルし、即プレビューします。
Padを押している間に `DEL` Fnを押すと対象Padを削除します。
従来互換として、Fnを押しながらPadを押す操作でも `EDIT` / `REV` / `DEL` を適用できます。
Reverse有効時は、SAMPLE/EDITのサンプル波形表示も左右反転し、Start/Endマーカーは反転後の見た目に合わせて表示します。

Fn:

- Fn1（鉛筆）: 編集対象を `START` ⇔ `END` でトグル（`VOLUME` 選択中は `START` へ戻る）
- Fn2（スピーカー）: `VOLUME` / `PITCH` 編集をトグル
- Fn3（ドア）: EDIT終了

EDIT中は DEL/REV の Fn+Pad 修飾と空Padの録音開始を無効化しています（誤操作防止）。

ENC2:

- `START` / `END`: 20ms単位で範囲編集
- `VOLUME`: 約5%単位で0〜200%編集
- `PITCH`: 約5%単位で50〜200%編集。再生速度を変える軽量方式で、音程と長さが同時に変わる

ENC2押し込み:

- 現在の編集状態でプレビュー（LOOP再生中は発音しない）
- EDIT中に別Padを押すと編集対象を切り替え、同時にそのPadをワンショットでプレビュー再生
- EDIT中に別Padへ移っても、選択中パラメーターは維持する
- 波形中央には現在選択中の編集パラメーター名と値を小さな透過風アウトラインチップ内に表示する。ENC2で値を変更している間は波形を隠しにくい小型チップへ切り替え、値だけを表示する。1秒間操作がなければ項目名付き表示へ戻る
- 左下には `P番号 / 長さ / Vol値 / Pitch値` を表示する

EDITは非破壊です。PCMデータ自体は書き換えず、スロットの再生メタ情報だけを変更します。

## LOOPモード

目的: Pad演奏を4拍タイムラインへ記録し、繰り返し再生します。

### BGMループ

リズム感に自信がないユーザーでも伴奏に合わせて演奏できるよう、Pad録音とは別に背景リズムトラックを1本読み込めます。

- 読込場所: `/sampler/loops/*.wav`
- 対応形式: PCM16 WAV、mono/stereo、48kHz以下
- 内部形式: PCM16 mono
- 推奨長: 3〜8秒程度の2小節/4小節リズムトラック
- 読込上限: 最大8秒
  - 48kHz / PCM16 / stereo / 8秒のWAVを安全圏の一時読込上限とする
  - 常駐データはmono変換後のPCMのみ保持するため、48kHz / 8秒で約768KB
- BGM用にPad 12個とは別の専用ボイスを1つ使う
- 読み込んだBGM WAVの長さがLoop長になる
- Loop再生開始時、BGMはLoop再生位置に同期してループ再生する
- Pad演奏とLoopイベント録音はこれまで通り行える
- Kit保存時はBGMのSD上WAVパスと音量を保存する
- BGM音量はLoopメニューの `BGM Volume` で5段階調整する

メモリ目安:

- 44.1kHz / PCM16 / mono: 約88KB/秒
- 8秒BGM: 約706KB
- stereo WAVは取り込み時にmonoへ変換するため、常駐メモリはmono相当。ただし読込中はWAVファイル全体の一時バッファも必要

読込エラー:

- SDが読めない: `No SD`
- `/sampler/loops/*.wav` がない: `No BGM wav`
- ファイルサイズが安全上限を超える: `BGM file too big`
- WAV形式が対象外: `Bad BGM WAV`
- 0.5秒未満: `BGM too short`
- 8秒超過: `BGM too long`
- PSRAM不足: `No BGM memory`

現状の実装:

- ループ長:
  - 新規ループ記録時は最初のPad押下から `END` Fn押下までの長さで確定
  - 未確定時の表示基準は4000ms
  - 最短長は250ms
- 最大イベント数: 96
- クオンタイズ: 初期値はON / Note Onは32分割 / Note Offは64分割
  - 内部選択肢: 8 / 16 / 32 / 64 / 128分割
  - Note Onは重み付き量子化。4分位置、8分位置、16分位置、その他の順に吸着を強くし、細かな32分グリッドを残しながら演奏の揺れを強拍へ寄せる
  - 将来メニューからON/OFF、Note On分解能、Note Off分解能を変更できる設計
  - OFF時は記録イベントの位置を吸着せず、早押し補正も無効
  - Repeatの基準幅は、クオンタイズON/OFFとは独立して選択中の分解能値を参照
- LOOPモードから他モードへ移動しても、ループ再生は継続する
- 再生イベントはUI描画とは別の1ms周期タスクで発火し、画面更新によるタイミングの揺れを避ける
- LOOPモードでループエンド未確定の初回録音中だけ、タイムラインのドット/グリッドを更新せず、軽量な `RECORDING` 表示だけにする。発音タイミングを優先するため
- Pad記録のNote On/Offではタイムラインを即時全面再描画しない
- イベント内容:
  - Pad番号
  - Note On / Note Off
  - ループ内位置ms
  - layer番号
- タイムライン表示:
  - 4拍グリッド
  - 4拍をさらに4分割した補助グリッド
  - 記録済みイベント
  - レーンはユーザー向けPad番号順で、P1を最下段としてP12へ向かって上へ積み上げる
  - 再生ヘッド
  - ミュート中Padのイベントとレーンは消さずにグレー表示
  - BGM読込済み、ループエンド確定後、PLAYモードでのループ再生中、LOOP停止中は詳細表示する

操作:

- Pad:
  - 新規ループ記録中は生タイミングでイベントを仮記録
  - ループ長確定後もLOOPモード中は常にイベントを追加記録する
  - PLAYモードへ移動すると、ループへ上書き/追加記録せず演奏のみ行う
  - クオンタイズON時、ループ長確定後にクオンタイズ位置の直前で微妙に早く押した場合は即時発音せず、クオンタイズ位置で発音する
  - この早押し補正はLOOPモードのオーバーダブ時だけでなく、ループ再生中の通常Pad演奏にも適用する
  - 微妙に遅い入力は演奏感を優先し、押した瞬間に発音する
  - 停止中に最初のPadを叩くと自動で再生開始
- 上Fn:
  - 起動時/未確定時は `END`
  - 新規ループ記録中に押すと、その時点の経過時間でループ長を確定
  - ループ長確定後もLOOPモード中の記録状態は維持する
  - クオンタイズON時、確定時に仮記録イベントを選択中グリッドへまとめてクオンタイズ
  - 確定後の再生中は `STOP`
  - 停止中は `PLAY`
- 中Fn `MUTE` + Pad:
  - Padごとのループ再生ミュート切替
  - ミュート中PadはPad右上に赤いスピーカー✕アイコン＋波形サムネイル減光で表示
  - タイムライン上の該当Padレーンとイベントはグレー表示
- 下Fn `DEL` + Pad:
  - Padごとのループ記録データを削除
  - 全Padのループ記録データが空になった場合は、未確定の新規ループ記録状態へ戻る
- LOOP録音中に下Fn `DEL` を押した場合:
  - 直近に記録した演奏レイヤーを1回ずつUNDOする
  - Hold PadのNote On / Note Offは同じレイヤーとしてまとめてUNDOする
- 停止中に下Fn `DEL` 長押し:
  - 全てのループ記録データを削除し、未確定の新規ループ記録状態へ戻る

Holdが有効なPadは、押下時にNote On、リリース時にNote Offを同じlayer番号で記録します。
Loop再生時は、Holdが有効なPadだけNote Offで対象Padの再生を停止します。
Holdが無効なPadはNote Onのみを記録し、One Shotまたはループ開始イベントとして再生します。
Loopが有効なPadはNote Onでループ再生します。Holdも有効な場合はNote Offで停止し、Holdが無効な場合は次の同Padイベントまたはループ周回で再トリガーされます。
Start/End、Volume、Pitch、Reverseは反映されます。
LOOP再生中に別モードへ移動しても再生は継続します。停止した場合は再生位置を保持せず、次回再生は先頭から始まります。ENC1押し込みでは明示的に全停止します。

未実装:

- BPM連動
- ループ長変更
- UndoのUI再配置
- 永続保存

## FXモード

目的: Fnボタンを押している間だけリアルタイムFXを適用します。

PitchはPad/Loopのサンプル再生速度へ適用します。
FilterはI2S出力直前のマスター段に入り、Pad再生と外部入力パススルー後のミックス全体へかかります。
RepeatはLOOPイベントを再生し直すトランスポートFXです。
録音入力はFX前の信号を使います。

Fn:

- `PITCH`: 押している間、サンプル再生Pitchを変更
- `FILTER`: 押している間、Filterを適用
- `REPEAT`: 押している間、次の32分グリッドからLOOPイベントの指定区間を繰り返し再生

ENC2:

- 回転: フォーカス中のFXパラメータを変更
- 押し込み: フォーカスするFXを `PITCH` → `FILTER` → `REPEAT` の順に切り替え
- Fn押下時: 押したFnのFXへフォーカスを強制的に合わせる
- Fnから指を離しても、パラメータ値は保持される
- Fnを押していない間もパラメータ変更できるが、FX適用はFnを押している間だけ

パラメータ:

- 初期値: 0
- Pitch: UI表示/操作値は -50〜+50。内部では2倍感度で適用し、±50で従来の最大効果へ到達する。0で原音、マイナスで低く遅く、プラスで高く速くする。音長維持型のピッチシフトは処理負荷を考慮して実装しない
- Filter: UI表示/操作値は -50〜+50。内部では2倍感度で適用し、±50で従来の最大効果へ到達する。0で原音、マイナスでローパス、プラスでハイパス
- Repeat: ループクオンタイズ幅を基準に `8 / 4 / 2 / 1 / 0.5` ステップの5段階
- Repeat中もメインのループカーソルは進み続けるが、出力は選択区間内のPad Note On / Note OffイベントとBGMを再生し直す
- Repeatを押している間に幅を変更しても開始グリッドは固定し、同じ開始点から新しい幅で再生を始める

### Pad Repeat（右上レバー）

- レバー下は1グリッド、レバー上は0.5グリッドごとに、押下中Padを再トリガする
- 先にPadを押している場合は次のグリッドから開始し、レバーを倒したままPadを押した場合は即時に開始する
- 複数Padを同時に対象にできる。レバーまたはPadを離すと対象PadのRepeatだけを止める
- LOOPモードでは、Pad Repeatが生成した量子化済みのNote On / Hold用Note Offを通常のLoopイベントとして記録する

FXモードでもPad演奏できます。

## タッチ操作

画面上のPad領域をタッチするとPad演奏します。
モードタブ領域のタッチでモード切替します。

## 既知の制約

- KitメニューでSD上のPad WAV参照、BGM WAV参照、EDIT情報、LOOPイベント、FX値をJSON保存できます。
- 録音直後のサンプルは現状RAM上のみです。Kit保存時にWAVとしてSDへ書き出す処理は未実装のため、電源OFF後に復元できません。
- Kit読込は現状 `/sampler/kits/*.json` の先頭ファイルを読み込む簡易実装です。ファイル選択UIは未実装です。
- 外部マイクの検出は物理検出ではなく入力レベル判定です。
- REC中は出力をミュートするため、録音中のモニタリングは行いません。
- BGM未使用時のLOOP長は新規記録時の `OFF` Fnタイミングで確定します。BGM使用時はBGM WAVの長さがLOOP長になります。
- FXは現状マスターFXのみで、Pad個別FXは未実装です。
- `esp-idf-size --ng` 警告がPlatformIOビルド中に出ますが、ファームウェア生成と書き込みは成功します。

## 今後の実装候補

- CHOP: サンプルを2/4/8等へ分割してPadへ配置
- LOOP:
  - BPM/長さ設定
  - レイヤー単位Undo
- FX:
  - Repeatバリエーション
  - Reverse系エフェクト
  - Pad個別FX
- キット保存:
  - 録音サンプルのWAV書き出し
  - Kitファイル選択UI
  - 起動時の前回Kit自動復元
