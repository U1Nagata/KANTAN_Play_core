# KANTAN Sampler Program Specification

この文書は、KANTAN Samplerファームウェアのプログラム仕様を記録するためのものです。
ユーザー体験や将来構想は [製品仕様](./product-spec.md)、ビルド環境や開発方針は
[開発ガイド](./development.md) を参照してください。

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
| `main/sampler/sampler_mp3.hpp/cpp` | Helix MP3デコード、48kHz / mono / PCM16変換 |
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
  - `docs/Sample_Sound/` の8個のWAVを `48kHz / PCM16 / mono` で埋め込み
  - Pad 1-4: KICK, SNARE, CLAP, HAT
  - Pad 5-8: PIKO, COWBELL, CHIN, TOM
  - Pad 9-12: 空欄
  - 下段はKICK/SNARE/CLAP/HATの基本ビート、中段はPIKO／パーカッション／金物／TOM、上段は空欄
- 組み込みBGM:
  - `docs/Sample_Sound/BGM_House.wav` をプリセットKITのBGMとして埋め込む
  - PCMは実ファイル長のまま保持し、BGM再生をループさせる
  - サンプラーのループ長は実ファイル長の2倍として設定する（2秒WAVなら4秒ループ）
  - Kit/終了時状態の復元では、保存済みの長さではなく、現在のBGM実ファイル長とBGM Repeatからループ長を再計算する
- 終了時自動保存:
  - 電源OFF/Resetコマンドを受けた時、現在のKit状態をLittleFSの `/sampler_resume.json` へ保存する
  - 起動時は `/sampler_resume.json` があれば復元し、無ければ組み込みプリセットをロードする
- 復元対象はPad割当、Start/End、Volume、Pitch、Reverse、Hold/Loop、PadごとのLoop方式（Whole Sample／Grid）とGrid値、BGM、Loopイベント、FX値、Mixerの6パート状態とMix A〜D
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
| SAMPLE | 再生/停止 | Mute | なし |
| EDIT中 | スピーカー（Preview） | OK（保存して終了） | EXIT |
| PLAY | 再生/停止 | Mute | なし |
| LOOP | 未確定=円弧矢印+終端バー（琥珀、ループを閉じる）/ 再生中=■（赤）/ 停止中=▶（緑） | スピーカー✕（Mute） | ゴミ箱（Del） |
| FX | 再生/停止 | Mute | なし |

- LOOP演奏中のFn案内はピアノロール全体を置き換えず、EDITパラメーターと同系統の小型チップとして重ねる
- PLAYモードのLoop Gridは、対象Padと `8 / 4 / 2 / 1 / 0.5` の値だけを小型チップに表示する

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
  - `Import Sample`: `/sampler/samples/` のWAV/MP3をファイル名順に一覧表示する。音源をOKで選ぶと最大2秒のプレビューを再生し、最後に割り当て先Padを押す
  - 割り当て先Pad選択中は全Padボタンを演奏画面と同じ波形付きPad表示にし、Fn3位置をBackとして使う
- Loop: `Load BGM` / `Clear BGM` / `BGM Volume` / `Quantize` / `Note Grid` / `Note Off Grid`
  - `Load BGM` は `/sampler/loops/` のWAV/MP3をファイル名順に一覧表示し、選択した音源を背景ループとして取り込む
  - BGM取り込み時は、そのWAVの長さをループ長に設定し、既存のループ録音イベントはクリアする
  - ループ停止や演奏録音の削除はメインUIで行うため、Loopメニューには重複配置しない
- Input Assign: `Learn` / `Assign List` / `Clear All`
  - Learnは、まず割り当て先のPad、モードボタン、またはSTOP ALLを押し、次に外部MIDIノートを入力する
  - BLE MIDI / USB MIDI / Port C MIDIのNote On / Offと、Port A I2C拡張ボタン入力を共通に受ける。Padは押下で発音、離すとHold発音を停止する
  - Port AはM5ByteButton / M5ExtIO2を最大4台（各8入力、合計32ボタン）まで自動検出する
  - 割り当てはKitデータおよび終了時の復元データに保存する
- Connections: `MIDI Input` / `USB Mode` / `USB Host Power`
- Wi-Fi: `Wi-Fi Setup` / `WPS` / `File Server` / `Wi-Fi Info`
  - File Editorのアップロードは32KBずつSDの一時ファイルへストリーム保存し、完了後に置き換える。受信中断時は元ファイルを維持する
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

## 演奏ページ

演奏ページは左から `DRUMKIT / SAMPLER / BASS / MELODY / CHORD` の順に切り替える。
SAMPLERを中心に、左側にリズム、右側に音程・和音系のページを配置する。
保存済みKIT／Loopとの互換性を保つため、内部の既存ページIDは変更せず、BASSを末尾IDとして追加し、表示順だけを上記の並びにする。

### BASSページ

BASSは、リズム、コード、メロディに加えて低音パートを初心者でも自然に組み立てられるようにする専用ページである。

- 12Padをスケール順の音程演奏に使う
- 同時発音数は1音。新しいPadを押すと、それまでのBass Noteを停止して新しい音へ切り替える
- 音源は `General MIDI` または任意の `Pad Sound`
- General MIDIはSAM2695のCH3を使用する
- 初期音色はGM 39 `Synth Bass 1`（内部Program 38）
- 初期Octaveは `-1`、初期Volumeは `80%`
- レバー上下はMELODYと同じ滑らかな半音Pitch Bendとして働く
- LOOPモードではレバーの上／下／中央復帰もページ固有のPitch Bendイベントとして記録する
- LOOPではBASS専用ページとしてNote On／Note Off、Mute、Delete、Undoを記録・管理する
- SAMPLE EDITのPad 3 `Bass` を2回押すと、編集中SampleをBassのPad Soundへ割り当てる

### Global KeyとScale

初心者がMelody／Bass／Chordを外しにくいことを優先し、`Key/Scale`を曲全体の設定とする。

- MELODY、BASS、CHORDは常に同じKeyとScaleを使う。ページ個別のKey、Scale、Follow Chord Keyは表示しない
- Scaleは `Pentatonic / Major / Chromatic / Blues / Japanese / Minor / Pentatonic Min / Dorian / Mixolydian`
- Japaneseは `1 / 2 / b3 / 5 / b6` のHirajoshi系として扱う
- 既存KIT互換のため、従来5種のScale IDは維持し、新しいScaleを後ろへ追加する
- Scale定義はSamplerファームウェア内に限定し、KANTAN Play本体側のScale定義へ影響させない
- MELODY／BASSのPad配色は、実際に発音する音程から動的に決める。ルートは橙、5度は紫、その他はページ固有の低彩度色とする
- 色はScale／Key変更時だけ12Pad分を再計算してキャッシュする。演奏中のPad描画では再計算しない

CHORDはScaleごとのChord Templateを内部で使う。Templateは各度数のルート半音位置と基本品質を持ち、ルートPadの電卓配列を変えずにモードやブルースへ対応する。

| Scale | 基本コードセット |
|---|---|
| Major / Pentatonic / Chromatic | I, ii m, iii m, IV, V, vi m, vii o |
| Minor / Pentatonic Min / Japanese | i m, ii o, bIII, iv m, v m, bVI, bVII |
| Dorian | i m, ii m, bIII, IV, v m, vi o, bVII |
| Mixolydian | I, ii m, iii o, IV, v m, vi m, bVII |
| Blues | I7, ii m, iii m, IV7, V7, vi m, bVII7 |

- `Swap`: MajorとMinorを入れ替える。diminished (`o`) はMinorへ、dominant 7thはminor 7thへ切り替える
- `7th`: 常に7thを追加または維持する。BluesのI7／IV7／V7では押しても7thを外さない
- `sus4 / 9th / M7`は、基本品質へ重ねる演奏中のModifierとして扱う
- コードPadのラベルは、ルートを大文字、半音記号を右上、品質を右下へ小さく組む。diminishedは `o` を使う

### 設定メニュー

現在表示中のページに対応する `Sample / Bass / Melody / Chord / Drum` をメインメニュー先頭へ表示する。
BASSとMELODYの設定項目は同じ構造とする。

- `Sound Source`
  - `General MIDI`
    - `Tone`
  - `Pad`
    - `Pad Sound`
    - `Pad Base Note`
- `Key`
- `Scale`
- `Octave`
- `Volume`

MELODYには上記に加えて `Follow Chord Key` を持つ。BASSのKey変更はHarmony Keyの変更としてChordにも反映する。

### KIT保存

KIT／ResumeにはグローバルなKey／Scaleと、BASSのSound Source、GM Program、Pad Sound、Octave、Volumeを保存する。
旧KITは読み込み時にChord KeyとMelody Scaleをグローバル設定として移行する。
旧KITにBASS情報がない場合は初期値を使い、既存のSample／Melody／Chord／DrumページIDはそのまま読み込む。

## PLAYモード

目的: Loopへ記録せず自由に演奏します。

Pad再生方式:

Padごとに `Hold` と `Loop` の2フラグを持ち、組み合わせで再生方式が決まります。

| Hold | Loop | 挙動 |
|---|---|---|
| Off | Off | One Shot。押すと最後まで鳴る |
| On | Off | Gate。押している間だけ鳴る |
| Off | On | Toggle Loop。押すとNote Grid基準のリピートを開始し、もう一度押すと停止 |
| On | On | Hold Loop。押している間だけNote Grid基準でリピートし、離すと停止 |

設定操作:

- Fn1: BGM/Loop再生・停止
- Fn2: Sample/DrumではFn2+Padで個別Pad Mute、Melody/Bass/ChordではFn2単押しでパートMute
- Melody/Bass/ChordのFn2とMixerのPart Muteは同じ状態を操作する。別々のMuteフラグを持たず、どちらの画面で解除しても同じ結果になる
- Sample/Drumの個別Pad MuteはLoop内容の編集、MixerのSAMPLER/DRUM Muteはパート全体のマスターMuteとして併存する
- Hold、Loop Grid、ReverseなどのSample固有設定はSAMPLE EDIT内の機能Padで変更する
- Loop GridはPadごとに半ステップ単位の値で保存し、実際の再トリガ周期は現在のBGM/Loop長とNote Gridからミリ秒へ変換する。BGM長やBGM Repeatが変わった場合は周期を再計算する

再生には、Start/End、Volume、Pitch、Reverseが反映されます。

## EDIT機能

- EDIT中にExitを押さず他モードへ移動した場合も、編集状態と一時表示を破棄し、移動先の波形/ピアノロールを全面再描画する

SAMPLEモードで中身のあるPadを押すと即座にEDITへ入り、そのPadをプレビューします。
空Padは押している間録音し、録音後のTrim／Normalize／解析／セッション保存が完了すると同じEDITへ入ります。
Reverse有効時は、SAMPLE/EDITのサンプル波形表示も左右反転し、Start/Endマーカーは反転後の見た目に合わせて表示します。

Fn:

- Fn1（スピーカー）: 現在のStart/End、Volume、Pitch、Reverseに加え、Hold／Repeat設定も通常演奏と同じ挙動でPreviewする。One Shotは離しても継続、Holdはボタンを離すと停止、Toggle Repeatは再押下で停止する
- Fn2: `OK`。SDセッションとResume Kitへ保存してEDIT終了
- Fn3: `EXIT`。現在のRAM上の設定を維持してEDIT終了

機能Pad:

- Pad 1 `Mel`: 2回押してMelodyのPad Soundへ割り当て
- Pad 2 `Chord`: 2回押してChordのPad Soundへ割り当て
- Pad 3 `Bass`: 2回押してBassのPad Soundへ割り当て
- Pad 4（ゴミ箱アイコン）: 2回押して削除し、EDIT終了
- Pad 5 `Hold`: 1回目は選択のみ。選択中にENC2を正方向へ回すとOn、逆方向へ回すとOff。同じPadをもう一度押してもOn/Offを切り替えられる
- Pad 6 `Rep`: Repeat方式を選択。`None / Whole Sample / 8 / 4 / 2 / 1 / 0.5`。Whole SampleはBGMやNote Gridに同期せず、編集済みのStart/End範囲をオーディオボイス内で連続再生する
- Pad 7 `Rev`: 1回目は選択のみ。選択中にENC2を正方向へ回すとOn、逆方向へ回すとOff。同じPadをもう一度押してもOn/Offを切り替えられる
- Pad 9〜12: `Start / End / Vol / Pitch` を選択
- Pad 8: 予約

誤操作の影響が大きいMelody／Bass／Chord割当とDeleteは、3.2秒以内の2回押しで確定する。確認メッセージは英語2行表示とする。

EDIT Padは通常演奏用の12色を流用せず、機能カテゴリごとの暗い色面とアクセント色を使う。Start／End／Vol／Pitch／Rep／Hold／Reverseのうち、エンコーダーで現在編集する対象だけを明るい面・白文字・下線で示す。Hold／Repeat／ReverseはON状態を中間の明るさとアクセント枠で示し、フォーカスと混同しない。Melody／Bass／Chord／Deleteは即時コマンド、空きPadはニュートラルな濃灰色とする。

ENC2:

- `START` / `END`: 20ms単位で範囲編集
- `VOLUME`: 約5%単位で0〜200%編集
- `PITCH`: 約5%単位で50〜200%編集。再生速度を変える軽量方式で、音程と長さが同時に変わる
- `REPEAT`: `None / Whole Sample / 8 / 4 / 2 / 1 / 0.5`を選択
- `HOLD` / `REVERSE`: 正方向でOn、逆方向でOff

- 波形中央には現在選択中の編集パラメーター名と値を小さな透過風アウトラインチップ内に表示する。ENC2で値を変更している間は波形を隠しにくい小型チップへ切り替え、値だけを表示する。1秒間操作がなければ項目名付き表示へ戻る
- Hold／Reverse／Repeat状態と2回押し確認は、波形中央の2行メッセージで表示する
- 左下には `P番号 / 長さ / Vol値 / Pitch値` を表示する

EDITは非破壊です。PCMデータ自体は書き換えず、スロットの再生メタ情報だけを変更します。

## LOOPモード

目的: Pad演奏を4拍タイムラインへ記録し、繰り返し再生します。

### BGMループ

リズム感に自信がないユーザーでも伴奏に合わせて演奏できるよう、Pad録音とは別に背景リズムトラックを1本読み込めます。

- 読込場所: `/sampler/loops/*.wav`
- 対応形式: PCM16 WAVまたはMP3、mono/stereo
- MP3はHelixで取り込み時に48kHz / mono / PCM16へ変換し、演奏中はデコードしない
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
  - Note Onは2段階の重み付き量子化。選択中の最小グリッドをすべて残し、その2倍間隔となる偶数位置だけ吸着範囲を約18%広げる
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
Loopが有効なPadはNote OnでPadごとのLoop Gridに従う再トリガを開始します。Holdも有効な場合はNote Offで停止し、Holdが無効な場合は次の同Padイベントまたはループ周回で再起動されます。
Start/End、Volume、Pitch、Reverseは反映されます。
LOOP再生中に別モードへ移動しても再生は継続します。停止した場合は再生位置を保持せず、次回再生は先頭から始まります。ENC1押し込みでは明示的に全停止します。

### Melody／Bass Pitch Bend記録

- LOOPモードでレバーを上げる、下げる、中央へ戻す操作を記録する
- MelodyとBassは独立したPitch Bend状態を持ち、両ページのLoopを同時再生できる
- レバー操作の時刻はNote Gridへ量子化する
- レバーを倒してから中央へ戻すまでを同じUndoレイヤーとして扱う
- 再生順は同一時刻の `Note Off → Pitch Bend → Note On` とし、音の切替時に古いNoteが残らないようにする
- ページMute時はPitch Bendを中央へ戻し、Mute中のPitch Bendイベントは発音へ適用しない
- ピアノロール下端にPitch Bend専用ドットを表示する。上段が半音上、中央が原音、下段が半音下を示し、現在ページの色を使う
- KIT／Resumeには `bendUp / bendDown / bendCenter` のイベント名で保存する。旧KITの `on / off` イベントとの互換性を維持する

未実装:

- BPM連動
- ループ長変更
- UndoのUI再配置
- 永続保存

## FXモード

目的: Padを押している間だけリアルタイムFXを適用します。

PitchはPad/Loopのサンプル再生速度へ適用します。
FilterはI2S出力直前のマスター段に入り、Pad再生と外部入力パススルー後のミックス全体へかかります。
RepeatはLOOPイベントを再生し直すトランスポートFXです。
録音入力はFX前の信号を使います。

Pad:

- Pad 1〜4: Repeat `4 / 2 / 1 / 0.5` Grid
- Pad 5: Filter
- Pad 6: Tempo（従来のPitch/Speed FX）
- Pad 7〜12: 将来拡張用
- FX Padの同時適用はせず、後から押したPadを有効にする
- FX Padは待機中を濃灰色の面と機能色の文字／枠で表示し、押下中だけ機能色の面・白枠・黒文字へ反転する。役割色はRepeat=黄、Filter=水色、Tempo=赤とする
- Pad表示は幅に合わせて `REP / FIL / TMP` を使う。将来拡張用のPadは空Sampleと同じ濃灰色で表示する

ENC2:

- 回転: フォーカス中のFXパラメータを変更
- 押したFilter/Tempo Padへフォーカスを合わせる
- Padから指を離しても、パラメータ値は保持される
- FX適用はPadを押している間だけ

パラメータ:

- 初期値: 0
- Pitch: UI表示/操作値は -50〜+50。内部では2倍感度で適用し、±50で従来の最大効果へ到達する。0で原音、マイナスで低く遅く、プラスで高く速くする。音長維持型のピッチシフトは処理負荷を考慮して実装しない
- Filter: UI表示/操作値は -50〜+50。内部では2倍感度で適用する。0で原音、マイナスは深いローパス、プラスは低中域を少し残しつつ高域を最大約2倍へ強調する演奏向けのHIキャラクター。Filter出力は64bit余裕を持って既存リミッターへ渡し、極端な設定でも先行する整数飽和を避ける
- Repeat: ループクオンタイズ幅を基準に `8 / 4 / 2 / 1 / 0.5` ステップの5段階
- Repeat中もメインのループカーソルは進み続けるが、出力は選択区間内のPad Note On / Note OffイベントとBGMを再生し直す
- Repeatを押している間に幅を変更しても開始グリッドは固定し、同じ開始点から新しい幅で再生を始める

### Pad Repeat（右上レバー）

- レバー下は1グリッド、レバー上は0.5グリッドごとに、押下中Padを再トリガする
- 先にPadを押している場合は次のグリッドから開始し、レバーを倒したままPadを押した場合は即時に開始する
- 複数Padを同時に対象にできる。レバーまたはPadを離すと対象PadのRepeatだけを止める
- LOOPモードでは、Pad Repeatが生成した量子化済みのNote On / Hold用Note Offを通常のLoopイベントとして記録する

FXモードのPadはFXトリガ専用で、通常演奏には使用しません。

### Mixer

FXモードでFn3を押すと、通常のFX PadとMixerを切り替えます。Fn3を押している間だけではなく、もう一度Fn3を押すまでMixerを維持し、両手で操作できるようにします。FXモードから離れた場合は通常FXへ戻ります。

- Pad 1〜3: `DRUM / SAMPLER / BASS`
- Pad 5〜7: `MELODY / CHORD / BGM`
- インフォメーションエリアも上段を `MELODY / CHORD / BGM`、下段を `DRUM / SAMPLER / BASS` とし、物理Padと位置を揃える
- Part Padを短く押して離す: そのパートのMuteを切り替える
- Part Padを保持しながらENC2またはENC3を回す: そのパートのVolumeを5%単位で変更する
- エンコーダーを動かした場合、Padを離してもMuteは切り替えない
- Mute中の短押し解除はMute前のVolumeへ即座に戻す
- Mute中にPart Padを保持してエンコーダーを上方向へ回すと、即座にMuteを解除し、Volumeを0%から5%単位で上げる。下方向ではMuteを維持する
- 複数のPart Padを同時に押して、複数パートのMuteを素早く切り替えられる
- Mixer Volumeは各パート固有のVolumeを上書きせず、その後段に掛かる0〜100%のグループ音量とする
- Sample/Pad音源の再生中Volume変更は、オーディオタスク側で数msかけて目標値へ移動し、クリックノイズを避ける

Pad 9〜12はMix A〜Dです。

- 表示は大きな数字の `1 / 2 / 3 / 4` とする。空きMixは黒いPad、保存済みMixは青い縁と文字、呼び出し待ちは白枠の点滅、適用中は明るい青面と黒文字で示す
- 短押し: 保存済みのMixを呼び出す
- 700ms以上の長押し: 現在の6パートのVolume/Muteを保存する
- Loop再生中の呼び出しは次のループ先頭で適用し、停止中は即時適用する
- 呼び出し時のVolume変化も滑らかに適用する
- Mix適用後にPart VolumeまたはMuteを変更した場合は、適用中表示を解除する
- Mixに保存するのは6パートのVolume/Muteのみ。Loopイベント、音色、FX値は含めない
- Mixer状態とMix A〜DはKitおよび終了時状態へ保存する

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
