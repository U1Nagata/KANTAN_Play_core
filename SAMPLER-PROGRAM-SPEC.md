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
| `main/sampler/sampler_app.cpp` | アプリ本体、入力処理、画面描画、REC/PLAY/LOOP/EDIT状態管理 |
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
- `reverse`: 逆再生フラグ
- `play_type`: `One` / `Hold` / `Loop`

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
- SDロード:
  - 起動時に `/sampler/*.wav` をファイル名の若い順に最大12個ロードし、Pad 1から順に配置する
  - SDから読み込めない場合は組み込みサンプルをロード
- 組み込みサンプル:
  - `docs/Sample_Sound/` の8個のWAVを `44.1kHz / PCM16 / mono` に正規化して埋め込み
  - Pad 1-4: KICK, SNARE, CLAP, HAT
  - Pad 5-8: HATDIG, COW, CHIN, TOML
  - Pad 9-12: 空欄
  - 下段はKICK/SNARE/CLAP/HATの基本ビート、中段はハイハット／パーカッション／金物／TOML、上段は空欄

## オーディオエンジン

`sampler_audio_t` がサンプル再生と外部入力録音を担当します。

- 出力サンプルレート: 48kHz
- 最大ボイス数: 12
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
  - PLAY通常時: I2S入力/出力の生波形を高さ112pxでリアルタイム表示
  - PLAY中にLOOP再生中: LOOPモードと同じタイムラインを表示
  - REC時: 入力の生波形は表示せず、選択中Padのサンプル波形を高さ112pxで固定表示
  - EDIT時: 選択サンプルの波形とStart/Endマーカー、中央に選択パラメーター名、左下に値
  - LOOP時: 4拍タイムライン、16分割補助グリッド、記録イベント、再生ヘッド、左下に状態/長さ
  - FX時: 3段のパラメータバー
- モードタブ: REC / PLAY / LOOP / FX
- 4x3 Pad
  - Pad/Fnボタンは44x44pxの正方形
  - 空Pad: 空色
  - サンプル入りPad: サンプル波形サムネイル
  - サムネイルはサンプル登録時に96分割の縮小波形を作成し、演奏中にPCM全体を再走査しない
  - Pad右上バッジ: 再生方式のミニアイコン（One=▶+終端バー / Hold=ゲート波形 / Loop=円弧矢印）
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
| REC | 鉛筆（EDIT） | ◀◀（Reverse） | ゴミ箱（Delete） |
| EDIT中 | 鉛筆＝Start/Endトグル（Start=橙・左バー / End=青・右バー、選択中は枠線） | スピーカー（Volume、選択中は緑枠） | ドア+矢印（Exit） |
| PLAY | ▶+終端バー（One） | ゲート波形（Hold） | 円弧矢印（Loop） |
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

- 上段4ボタン: REC / PLAY / LOOP / FX 切替
- ENC1: マスターボリューム
- ENC1押し込み: 全音停止。LOOP再生も停止
- ENC2:
  - EDIT中: 現在パラメータ編集
  - FX中: Fnを押しながら選択中FXのパラメータ編集

## RECモード

目的: 空Padへ音を録って即Pad化します。

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
- 検出範囲のピークを基準に、録音PCM全体を約30000へ正規化
- PCM自体は自動Cropで切り詰めず、再生範囲だけを `start_frame` / `end_frame` に保存する

## PLAYモード

目的: Loopへ記録せず自由に演奏します。

Pad再生方式:

- `ONE`: 押すと最後まで再生
- `HOLD`: 押している間だけ再生
- `LOOP`: 押すとループ再生、再度押すと停止

設定操作:

- `ONE` Fn + Pad
- `HOLD` Fn + Pad
- `LOOP` Fn + Pad

再生には、Start/End、Volume、Reverseが反映されます。

## EDIT機能

RECモードで `EDIT` Fn を押すと（Pad併用不要）EDITモードに入ります。
編集対象はREC画面で波形表示中のPad、無ければ最初の有効Padが選ばれ、プレビュー再生されます。
EDIT中にPadを押すと編集対象を切り替えます。

RECモード通常時は `REV` Fn + Pad で対象PadのReverseをトグルし、即プレビューします。
Reverse有効時は、REC/EDITのサンプル波形表示も左右反転し、Start/Endマーカーは反転後の見た目に合わせて表示します。

Fn:

- Fn1（鉛筆）: 編集対象を `START` ⇔ `END` でトグル（`VOLUME` 選択中は `START` へ戻る）
- Fn2（スピーカー）: `VOLUME` 編集を選択
- Fn3（ドア）: EDIT終了

EDIT中は DEL/REV の Fn+Pad 修飾と空Padの録音開始を無効化しています（誤操作防止）。

ENC2:

- `START` / `END`: 20ms単位で範囲編集
- `VOLUME`: 約5%単位で0〜200%編集

ENC2押し込み:

- 現在の編集状態でプレビュー
- EDIT中に別Padを押すと編集対象を切り替え、同時にそのPadをワンショットでプレビュー再生
- EDIT中に別Padへ移っても、選択中パラメーターは維持する
- 波形中央には現在選択中の編集パラメーター名と値を小さな透過風アウトラインチップ内に表示する。左下には `P番号 / 長さ / Vol値` のみを表示する

EDITは非破壊です。PCMデータ自体は書き換えず、スロットの再生メタ情報だけを変更します。

## LOOPモード

目的: Pad演奏を4拍タイムラインへ記録し、繰り返し再生します。

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
- イベント内容:
  - Pad番号
  - Note On / Note Off
  - ループ内位置ms
  - layer番号
- タイムライン表示:
  - 4拍グリッド
  - 4拍をさらに4分割した補助グリッド
  - 記録済みイベント
  - 再生ヘッド
  - ミュート中Padのイベントとレーンは消さずにグレー表示

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
- 初回ループの録音中に下Fn `DEL` を押した場合:
  - 現在の録音を強制終了し、記録中のイベントを破棄して未確定の新規ループ記録状態へ戻る
- 停止中に下Fn `DEL` 長押し:
  - 全てのループ記録データを削除し、未確定の新規ループ記録状態へ戻る

HOLDに設定されたPadは、押下時にNote On、リリース時にNote Offを同じlayer番号で記録します。
LOOP再生時はNote Offで対象Padの再生を停止します。
ONEのPadはNote Onのみで最後まで再生します。
LOOPのPadはNote Onでループ再生し、Note Offで停止できます。
Start/End、Volume、Reverseは反映されます。
LOOP再生中に別モードへ移動しても再生は継続し、ENC1押し込みで明示的に全停止します。

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
- Repeat中もメインのループカーソルは進み続けるが、出力は選択区間内のPad Note On / Note Offイベントを再生し直す

FXモードでもPad演奏できます。

## タッチ操作

画面上のPad領域をタッチするとPad演奏します。
モードタブ領域のタッチでモード切替します。

## 既知の制約

- サンプル、EDIT情報、LOOPイベントは現状RAM上のみで、電源OFF後は保持されません。
- 外部マイクの検出は物理検出ではなく入力レベル判定です。
- REC中は出力をミュートするため、録音中のモニタリングは行いません。
- LOOP長は新規記録時の `OFF` Fnタイミングで確定します。
- FXは現状マスターFXのみで、Pad個別FXやFXパラメータ保存は未実装です。
- `esp-idf-size --ng` 警告がPlatformIOビルド中に出ますが、ファームウェア生成と書き込みは成功します。

## 今後の実装候補

- CHOP: サンプルを2/4/8等へ分割してPadへ配置
- LOOP:
  - BPM/長さ設定
  - クオンタイズ分解能の設定
  - レイヤー単位Undo
  - ループ保存/読込
- FX:
  - Repeatバリエーション
  - Reverse系エフェクト
  - Pad個別FX
- キット保存:
  - サンプルスロット
  - EDITメタ情報
  - LOOPイベント
  - Pad設定
