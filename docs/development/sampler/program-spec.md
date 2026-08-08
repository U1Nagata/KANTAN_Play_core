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
| `main/sampler/sampler_app.cpp` | アプリ本体、入力処理、画面描画、5パートとSOUND/PLAY/REC/FX状態管理 |
| `main/sampler/sampler_audio.hpp/cpp` | 48kHz I2S再生エンジン、30ボイスミキサー、外部入力録音 |
| `main/sampler/sampler_pool.hpp/cpp` | PSRAM上のSampler/Pattern Beatサンプル管理、WAV/PCMロード |
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
- `chop_group_id` / `chop_slice_index` / `chop_slice_count`: Chop後の音楽的なまとまりと並び順
- `chop_native_loop_msec` / `chop_tempo_q8`: Chop作成時の基準フレーズ長と、現在のBeatへ合わせる再生倍率

`playStart()` / `playEnd()` / `playFrames()` は、編集範囲を反映した再生範囲を返します。

## Beatパート

ユーザーに見せるパートは `BEAT / SAMPLER / BASS / MELODY / CHORD` の5つです。Beatは曲のリズムを決める1パートであり、ユーザーUI上にAudio/Patternという2レイヤーを同時表示しません。

- `Audio Beat`: WAV/MP3を専用PCMへ読み込み、その長さをLoop基準にする
- `Pattern Beat`: 12個の短いBeat音源と、既存のLoopイベントを組み合わせる
- Audio BeatとPattern Beatは排他的。新しいBeatを選ぶと、以前の形式の音源とBeatイベントを停止・解放する
- 組み込みPatternは `POP / ROCK / HOUSE / HIP HOP / DISCO / BREAK`。同じ内蔵Beatサンプルを共有し、イベントとLoop長だけを切り替える
- Pattern Beatの `Select Kit` は `Acoustic` / `Chiptune` を選べる。Pattern、テンポ、Loop長は維持したままBeat専用の12 Pad音色だけを切り替える
- Acoustic Kitは、Kick / Snare / Rim / Clap / Low-Mid-High Tom / Closed Hat / Shaker / Crash / Ride / Open Hatを用いる。Crash/Rideは内蔵用に短尺mono化し、Pattern Beatの2秒上限内で扱う
- 各Patternは64 tickで1小節。内部テンポは順に100 / 120 / 124 / 88 / 116 / 110 BPM相当で、通常演奏では固有Loop長として扱う
- Pattern Beatの `Tempo`はTap Tempo専用画面で調整する。ユーザーがBPMを知りたい場合に限り、推定値を `~***.* BPM`で表示する
- `New Pattern` は組み込みBeat音源だけを読み込み、最初の演奏からLoop長を決める
- SDの `.mid/.midi` は軽量なStandard MIDI File読込でPatternへ変換する。CH10を優先し、CH10がなければ全チャンネルのNote OnをGM Drum配列へ割り当てる
- 旧KITはBGMがあればAudio Beat、BGMがなくDrumイベントがあればPattern Beatへ移行する。旧ページID `drum` はKIT互換のため内部に残す
- BeatパートのMuteは、Audioでは背景PCM、Patternでは記録イベントを止める。Patternの生演奏はMute中も鳴る

Pattern Beat音源はSamplerの12 Padとは別の `beat_pool_t` に保持します。

- 12スロット、PCM16 mono
- PSRAM予算: 1.5MB
- 1音あたり最大2秒
- 最大8音のBeat専用One Shotボイス
- Open/Closed HiHatは1つのChokeグループを共有する

### パート別SOUNDモード

SOUNDモードはSamplerパートへの強制移動ではなく、現在のパートに対応する共通の音源選択・編集ページです。波形Pad描画、選択枠、上部波形Canvasは同じ実装を再利用します。

- `SAMPLER`: 録音、Import、非破壊編集、Chop、Synth設定を行う
- `BEAT`: Audio Beatでは全体波形、Pattern Beatでは12音の波形Padと試聴を表示する
- `BASS / MELODY / CHORD`: Samplerの12音を一覧し、押したPadをそのパートのPad Soundに選択して基準音で試聴する
- Chop Sliceはリズム演奏専用とし、Bass / Melody / Chordおよび外部MIDIのPad Soundには割り当てない。SOUNDでは波形を暗くして `CHOP` を表示し、押した場合は `CHOP: SAMPLER ONLY` を通知する。シンセ音源には通常SampleをStart/End編集して使用する
- 旧ProjectやChop実行前の割当がChop Sliceを指した場合は、別の通常Sampleへ修復し、候補がなければGeneral MIDIへ戻す
- SOUND内でパートを切り替えた場合はPLAYへ戻す。新しいパートに前パートの編集・アサイン待機状態を引き継がない
- ページ/モード切替時はSOUND音源選択プレビューへ必ずNote Offを送り、試聴音を残さない
- SOUNDへはモードボタンの1タップで入る。Padは1回目で選択/試聴、2回目で編集または音色割り当てを確定する
- SOUNDで選択中のPadは白い2px枠を常時表示する。押下・発音中は従来のパート色枠を優先し、発音表示が終了したら白枠へ戻す。編集終了、Import/録音完了、移動、削除後も同じ選択状態を使う
- SOUNDのFn1は選択音のPreview/Stop。ループの再生状態は変えず、プレビューはシーケンサーと別の音声を使う
- SOUND中でもEnc1押下の全音停止は有効

## サンプルプール

- プール予算: 5MB（Beat Pool、WiFi/TLS、画面Cache用のPSRAM余白は別に確保）
- 内部形式: PCM16 mono
- 最大サンプル長: 20秒。Long素材はChop用として扱い、通常の短いPad素材と同じ総量予算を共有する
- PCM Asset: 通常Import/録音はPadごとの独立Asset、Chop後のSliceは1本のLong Assetの範囲参照として保持する。Sliceを追加してもPCMは複製しないため、複数のLong素材を予算内で安全に共存できる
- マイク録音用の最大20秒作業バッファは、Pad Assetへのコピーと保存が完了した時点で解放する。録音開始失敗時とChop開始時にも残留作業バッファを回収し、Fit用PCMとの一時的な二重確保を避ける
- Chop元Padを削除しても、そのPCMを参照するSliceが残っていれば共有Assetは維持する。最後の参照Sliceが削除または上書きされた時点でPCMを解放する
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
- 組み込みAudio Beat:
  - `docs/Sample_Sound/` のBGM WAVをプリセットBeatとして埋め込む
  - PCMは実ファイル長のまま保持し、Audio Repeatと実ファイル長からLoop長を再計算する
- 終了時自動保存:
  - 電源OFF/Resetコマンドを受けた時、現在のKit状態をLittleFSの `/sampler_resume.json` へ保存する
  - 起動時は `/sampler_resume.json` があれば復元し、無ければ組み込みプリセットをロードする
- 復元対象はPad割当、Start/End、Volume、Pitch、Reverse、Hold/Loop、PadごとのLoop方式とGrid値、Beat形式/音源、Loopイベント、FX値、Mixerの5パート状態とMix A〜D
  - 内蔵サンプルは `builtin:KICK` のような識別子で保存し、SDなしでも復元できる
  - SD上のWAVを割り当てているPadはSDカード上のファイル参照で復元する。録音直後の未保存PCMはWAV化していないため復元対象外

## オーディオエンジン

`sampler_audio_t` がサンプル再生と外部入力録音を担当します。

- 出力サンプルレート: 48kHz
- 最大ボイス数: 30（12 Sampler + Audio Beat + Preview + Pad Synth 8 + Pattern Beat 8）
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
  - パート切替マーカーは選択中を白、未選択を黒で表示する
  - 利用可能なファームウェア更新がある場合は黄色の`UP!`を表示する
- 波形/タイムライン領域
  - 高さ112px
  - 現在モードのボタン色と同じ色で外枠を表示する（メニュー表示中は外枠を表示しない）
  - PLAY通常時: I2S入力/出力の生波形を高さ112pxでリアルタイム表示
  - PLAY中にLOOP再生中: LOOPモードと同じタイムラインを表示
  - SOUND時: 入力の生波形は表示せず、選択中Padのサンプ波形を高さ112pxで固定表示
  - PLAY/SOUNDからのSample録音中: モードに関係なく上画面全体を赤系にし、大きなマイクアイコン、`SAMPLING`、入力ソース、Pad番号を共通表示する。Recモードの演奏記録表示とは別デザインにする
  - EDIT時: 選択サンプルの波形とStart/Endマーカー、中央に選択パラメーター名、左下に値
  - LOOP時: 4拍タイムライン、16分割補助グリッド、記録イベント、再生ヘッド
  - FX時: 3段のパラメータバー
- モードタブ: SOUND / PLAY / REC / FX
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
| SOUND | 選択音のPreview/Stop | Mute | SamplerパートのみDelete |
| EDIT中 | スピーカー（Preview） | OK（保存して終了） | EXIT |
| PLAY | 再生/停止 | Mute | Melody/Bassのみ `TOUCH`（画面タッチ/本体傾き演奏） |
| REC | 未確定=円弧矢印+終端バー（琥珀、ループを閉じる）/ 再生中=■（赤）/ 停止中=▶（緑） | スピーカー✕（Mute） | ゴミ箱（Del） |
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

- 上段4ボタン: SOUND / PLAY / REC / FX 切替
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
  - ステータスバー直下からモードボタン（SOUND/PLAY/REC/FX）領域までをメニューテキスト表示に使う
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

- Sample Kit: `Load Sample Kit` / `Save Sample Kit` / `Import Sample` / `New Kit` / `Reset Kit`
  - Sample Kitは12個のSampler Padの波形と編集設定だけを `/sampler/kits/` に保存する。Beat、Recシーケンス、BGM、FX、各シンセパートの設定は変更しない。
  - Sample Kitを読み込むと、演奏途中のRecデータを残したまま音色セットだけを入れ替えられる。
- Rec: `Quantize` / `Note Grid` / `Note Off Grid` / `Save Project` / `Load Project` / `Save as Beat` / `Clear Rec`
  - `Clear Rec` はユーザーが記録したSampler / Bass / Melody / Chord / Beatの演奏レイヤーだけを消去する。Audio Beat、Patternのプリセットレイヤー、Beat Kit、Tempo、Beat Repeatは維持する
  - Projectは `/sampler/projects/` に保存する完全な楽曲状態。Sampler/Beatの波形、BGM、Recシーケンス、Key/Scale、各パート設定、FX、Mixer状態を1セットとして保存する。
  - `Performance`は最終ミックスをWAVとして保存する機能、`Sample`はマイクからPadへ録音する機能、`Rec`は演奏イベントをループへ記録する機能として用語を使い分ける。
  - SD上のWAVパスがあるサンプルを復元対象とする。録音直後の未保存PCMをWAVとして書き出す処理は未実装
  - `Import Sample`: `/sampler/samples/` のWAV/MP3をファイル名順に一覧表示する。試聴可能な行ではFn1をスピーカーアイコンへ切り替え、最大2秒のプレビューを再生／停止する。OKは試聴せず割り当て先Padの選択へ進む
  - 割り当て先Pad選択中は全Padボタンを演奏画面と同じ波形付きPad表示にし、Fn3位置をBackとして使う
- Beat: `Select Beat` / `Select Kit` / `Tempo` / `Clear Pattern` / `Beat Volume` / `Beat Repeat` / `File Editor`
  - `Select Beat` は `Built-in`、`SD Card`、`Sampler Pad` の3経路に分ける。内蔵は組み込みPattern/Audio、SDは`/sampler/loops/` のWAV/MP3/MID/MIDIを表示する
  - Select BeatのPattern/MIDI行でFn1を押すと、候補を本来のTempoで1周だけ試聴する。MIDIにTempo情報がない場合は120 BPMとする。現在のTempo / Beat Repeatは適用しない
  - Pattern試聴は現在のLoopと生演奏を停止し、最大8秒の32kHz / mono一時PCMへオフライン合成して専用Voiceで再生する。現在のBeat Pool、Kit、Recイベントは書き換えない
  - 現在のPattern KitがRAMにある場合はそのPCMを読み取り専用で利用し、不足Padだけ内蔵WAVを直接参照する。試聴停止、カーソル移動、Back / Exit、自然終了で一時PCMを解放する
  - Audio Beat（内蔵/WAV/MP3）と`Sampler Pad`の選択行ではFn1で最大2秒を試聴／停止する。Pattern/MIDIは専用の一時PCMで試聴する。いずれもカーソル移動、Back、OKで必ず停止する
  - `Select Kit` はPattern Beat表示時だけ現れ、`Acoustic` または `Chiptune` のドラム音色セットを選ぶ。Audio Beatには適用しない
  - `Sampler Pad` はPadをプレビューしてから確認し、現在のStart/End/Reverseを反映した独立Audio Beatを作る。元Padは変更・削除しない。作成したBeatは`/sampler/session/beat_from_pad.wav`へ保存し、以後のPad編集や削除からも独立する
  - SDのAudio BeatとSampler Pad由来のAudio Beatは、読込後に楽曲向けのキー推定を行う。十分な和声・低音の手がかりがある場合だけMelody/Bass/Chordの共通Keyを更新し、ドラムのみなど曖昧な素材は現在のKeyを維持する。組み込みBeatとPattern Beatは自動変更しない
- Recデータがある状態でBeatを差し替えると、`Follow New Beat` / `Keep Current Tempo` / `Clear Rec` を選ぶ。`Follow New Beat` はSampler/Bass/Melody/Chordの記録位置を新しいLoop長へ比例配置し、Chopグループの再生倍率も新しいBeatへ合わせる。`Keep Current Tempo` はRec位置と現在のLoop長を保ち、読み込むAudio/Pattern Beat側を現在の速さへ合わせる。`Clear Rec` は記録を消して新しいBeatを基準にし、残っているChop素材は新しい速さへ追随する
- ChopグループのTempo FitはPCMを複製・再変換せず、Sampler再生専用の固定小数倍率で行う。ユーザーのPitch値やBass/Melody/Chordの音程計算とは独立させ、Project/KIT/Resumeへグループ情報と倍率を保存する。元のLong素材が削除・圧縮済みでも、現在のSlice PCMだけで追随できる
- Chop SliceはChokeグループで直前のSliceを止めるため、テンポ追随時も不要な長尺ボイスが積み重ならない。Tempo Fit済みPCMを別途保存せず、共有PCMと再生倍率を使うことでPSRAM、SD書込み、変換待ちを抑える
- Chop Sliceの編集では、Preview、Volume、Hold、Repeat/Grid、Delete、Move/Copyだけを許可する。Start/End、Pitch、Reverse、Synth、再Chopは、拍頭Anchor、クロスフェード、グループTempoを壊すため無効化する。Chokeは常時ONに固定し、無効な操作には `CHOP TIMING / LOCKED` を表示する
  - Audio Beat取り込み時は、その音声長とAudio Repeatをループ長に設定する
  - Pattern Beat取り込み時は、Beat音源とPatternイベントを読み込み、Audio Beatを解放する
- `Tempo`はPattern Beat専用。現在の速さに合わせて4ドットを循環させ、点滅が75〜150 BPM相当になるよう表示上の拍単位だけを2倍単位で選ぶ
- Tap Tempo確定時にChopグループがあれば `PROCESSING / FITTING CHOPS` を表示し、確定後のLoop長へ一括追随する。ダイヤル・Tapの途中ではPCM処理や保存を行わない
  - Tap TempoのFn1はスピーカーアイコンのプレビュー。押すたびにPatternを先頭から再生/停止し、再生開始時は4ドットも1番目から同期させる
  - TapまたはエンコーダーでTempoが変わった時点でプレビューを停止する。再生中のリアルタイム伸縮は行わない
  - 4回目のTapで直近3間隔、5回目以降は直近4間隔の移動平均を反映する。Enc2/Enc3は1カウント=0.5 BPMで微調整し、入力差分を1回で反映する
  - Tempoは読み込み時のPattern基準に対し50〜200%へ制限する。Backは画面進入時の値へ戻し、OKはKit/再開データに保存する
  - Tempo変更ではLoopイベントを新しいLoop長へ比例変換する。Note Grid / Note Off Gridと各イベントのグリッド位置は変えない
  - 新しいLoop長が確定した時は、1 Gridが約125msになるよう `8 / 16 / 32 / 64 / 128` からNote Gridを自動選択する。Note Off Gridはその一段細かい値とし、上限は128にする
  - Audio Beatの実ファイル長とAudio Repeatを掛けた全体長を基準にし、Repeat変更時もNote GridとNote Off Gridを再計算する。QuantizeのOn/Offは自動変更しない
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
  - 設定送信後はQRを閉じ、全画面の `CONNECTING WI-FI` から `CHECKING INTERNET` へ遷移する。通常ボタンUIは通信用メモリの解放完了まで描画しない
  - IP取得をWi-Fi接続成功とし、OTAカタログへのHTTP到達をInternet接続として別々に確認する。結果は `Wi-Fi connected / ONLINE` または `Wi-Fi connected / OFFLINE` と表示する
  - Internet確認が失敗しても、Wi-Fi接続に成功したSSID/パスワードは消去しない
  - `WPS` はWPSプッシュボタン接続を開始する
  - `File Server` ONでWi-Fiファイル操作モードを起動する
- Audio: `Input Source`
  - `Auto` / `Internal` / `External`
- System: `Display` / `LED` / `Language` / `Info` / `Reset All`

## SOUNDモード

目的: 空Padへ音を録って即Pad化し、録音済みサンプルの選択・編集・整理を行います。SamplerパートのPLAYから開始したSample録音も同じ録音処理とマイク画面を使用します。

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

1. SOUNDモードで音入りPadを約650ms長押し
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

演奏ページは左から `BEAT / SAMPLER / BASS / MELODY / CHORD` の順に切り替える。
SAMPLERを中心に、左側にリズム、右側に音程・和音系のページを配置する。
ENC2/ENC3のページ選択ウィンドウと左右サイドボタンは、この並びの両端で停止する。端から反対端へ循環させず、高速操作時の意図しない大移動を防ぐ。

ENC2/ENC3を回すと、ヘッダーとPadを残したまま上部情報エリア全体にパートセレクターを表示する。5パートを常時一覧表示し、選択中のパートはページカラーの面と大きな太字、非選択行は各ページ本来の明るい文字色で表す。操作を止めるかエンコーダーを押すと確定する。表示中にPad、Fn、レバー、モードなどの演奏操作が入った場合は、選択中のパートを即時確定し、同じ入力イベントを演奏に使う。画面復元は発音より後に行う。

セレクターは上部情報エリア1枚のPSRAM Canvasだけを保持し、選択状態ごとの全画面キャッシュは持たない。閉じる際は既存の波形／情報Canvasを戻し、全画面再構築を避ける。
保存済みKIT／Loopとの互換性を保つため、内部の既存ページIDは変更せず、BASSを末尾IDとして追加し、表示順だけを上記の並びにする。

### BASSページ

BASSは、リズム、コード、メロディに加えて低音パートを初心者でも自然に組み立てられるようにする専用ページである。

- 12Padをスケール順の音程演奏に使う
- 同時発音数は1音。新しいPadを押すと、それまでのBass Noteを停止して新しい音へ切り替える
- 音源は `General MIDI` または任意の `Pad Sound`
- General MIDIはSAM2695のCH3を使用する
- 初期音色はGM 39 `Synth Bass 1`（内部Program 38）
- 初期Octave表示は `0`、初期Volumeは `80%`
- BassのOctave `0`はMelodyのOctave `0`より実音程を1オクターブ低くする
- 選択範囲は表示上 `-2〜+2`。実音域全体をMelodyより1オクターブ低く配置する
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

現在表示中のページに対応する `Beat / Sample / Bass / Melody / Chord` をメインメニュー先頭へ表示する。
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

- Fn1: Beat/Loop再生・停止
- Fn2: Sampler/Pattern BeatではFn2+Padで個別Pad Mute、Melody/Bass/ChordではFn2単押しでパートMute
- Melody/Bass/ChordのFn2とMixerのPart Muteは同じ状態を操作する。別々のMuteフラグを持たず、どちらの画面で解除しても同じ結果になる
- Muteは音声出力全体ではなく、Loopに記録されたシーケンスの再生だけを止める。Mute中もPadによる生演奏は発音し、記録済み演奏と生演奏を即座に入れ替えられる
- Sampler/Pattern Beatの個別Pad MuteとMixerのPart Muteは併存し、前者は1 Pad、後者はパート内の全記録イベントを対象にする
- Audio Beatには生演奏経路がないため、Beat MuteはAudio Beat音声を止める
- Melody/Bassのカオシレーター操作中は、そのパートの記録済みシーケンスを一時的にMuteする。カオシレーター終了時は、開始前のMixer/Play Mute状態を変更せず通常再生へ戻る
- Melody/BassのFn3 `TOUCH`を押した姿勢を中央とする。画面に触れていない間は、KANTAN Play筐体のY軸回りに約10度以上の意図的な姿勢変化があると発音を開始し、基準から上90度〜下90度を12音に対応させる。左右の振りは押下時を中央とした±60度で、左振りで値を下げ、右振りで上げる。画面タップ中はタッチ操作を優先し、姿勢入力を無視する
- 姿勢入力は約40Hzで最新加速度とジャイロを取得し、発音はLCD描画完了を待たない。Fn3解除またはタッチ解除時はNote Offを送り、タッチ後は新たな意図動作があるまで再発音しない
- Hold、Loop Grid、ReverseなどのSample固有設定はSOUND EDIT内の機能Padで変更する
- Loop GridはPadごとに半ステップ単位の値で保存し、実際の再トリガ周期は現在のBGM/Loop長とNote Gridからミリ秒へ変換する。BGM長やBGM Repeatが変わった場合は周期を再計算する

再生には、Start/End、Volume、Pitch、Reverseが反映されます。

## EDIT機能

- EDIT中にExitを押さず他モードへ移動した場合も、編集状態と一時表示を破棄し、移動先の波形/ピアノロールを全面再描画する

SOUNDモードで中身のあるSampler Padを押すと選択し、停止中はプレビューする。同じPadをもう1回押して短く離すとEDITへ入る。
2回目のPadを押している間は`HOLD TO MOVE`の進捗を表示し、650ms長押しするとEDITへ入らずMove/Mixの移動先選択へ切り替わる。
空Padは誤操作防止の長押しメーター完了後、`TAP: LOAD SAMPLE / HOLD: RECORD`を表示する。2回目の操作ではメーターを再表示せず、短く離せばImport、そのまま押し続ければ録音画面へ移る。録音後は通常のSOUND表示に戻り、自動でEDITには入らない。

EDITの`Start`・`End`は通常タップで選択し、プレビュー中は現在の再生位置を設定する。約480ms長押しすると個別にトリムを解除し、`Start`は元PCMの先頭、`End`は元PCMの末尾へ戻す。
Reverse有効時は、SAMPLE/EDITのサンプル波形表示も左右反転し、Start/Endマーカーは反転後の見た目に合わせて表示します。

Fn:

- Fn1（スピーカー）: 現在のStart/End、Volume、Pitch、Reverseに加え、Hold／Repeat設定も通常演奏と同じ挙動でPreviewする。One Shotは離しても継続するが、再生中にFn1をもう一度押すと停止する。Holdはボタンを離すと停止し、Toggle Repeatも再押下で停止する
- Fn2: `OK`。SDセッションとResume Kitへ保存してEDIT終了
- Fn3: `EXIT`。現在のRAM上の設定を維持してEDIT終了
- Start/End変更後の基音・Sustain再解析では全画面の`PROCESSING / ANALYZING`を表示する。`OK`は続けて`SAVING`へ切り替え、処理中もドットだけを更新する。解析や保存が不要な`EXIT`には待機画面を出さない

機能Pad:

- Pad 1 `Mel`: 2回押してMelodyのPad Soundへ割り当て
- Pad 2 `Chord`: 2回押してChordのPad Soundへ割り当て
- Pad 3 `Bass`: 2回押してBassのPad Soundへ割り当て
- Pad 4（ゴミ箱アイコン）: 2回押して削除し、EDIT終了
- Pad 5 `Hold`: 1回目は選択のみ。選択中にENC2を正方向へ回すとOn、逆方向へ回すとOff。同じPadをもう一度押してもOn/Offを切り替えられる
- Pad 6 `Rep`: Repeat方式を選択。`None / Whole Sample / 8 / 4 / 2 / 1 / 0.5`。Whole SampleはBGMやNote Gridに同期せず、編集済みのStart/End範囲をオーディオボイス内で連続再生する
- Pad 7 `Rev`: 1回目は選択のみ。選択中にENC2を正方向へ回すとOn、逆方向へ回すとOff。同じPadをもう一度押してもOn/Offを切り替えられる
- Pad 9〜12: `Start / End / Vol / Pitch` を選択
- Pad 8 `Synth`: Attackを残し、波形途中をSustain LoopしてReleaseさせる音作りページへ移動

Chopページ:

- 通常EditのPad 1 `Chop`から開く。編集中のStart/End範囲を対象にする
- Start/Endは「使いたい範囲」の意図として扱う。Startは約180ms以内の強い拍頭へ、Endは短く切りがちな操作を補うため後方約520msまでの次の拍頭へ補正する。アタックを確信できない素材は元の範囲を維持する。
- Pad 1 `FIT`: BGMまたは確定済みLoop長を64グリッドの基準とし、1 Sliceの長さ×4個分（4〜6 Chop）または8個分（7〜12 Chop）を仮想フレーズ長として自動変換する。現在のBeatとの比率が75%未満または150%以上なら、同じテンポの半分/倍フレーズと解釈してからFITし、極端な速度変化を避ける
- Pad 2 `KEEP`: 素材の速度と音程を変えずに分割する
- Pad 4 `TAP CUT`: 1回目でStart/End範囲の再生を開始し、再生中のタップごとに切断点を追加する。最大12 Sliceで、Fn1で途中確定、または再生終了で自動確定する。切断中は分割数Padを無視し、誤操作で入力済みの切断点を消さない
- Pad 5〜8: `4 / 8 / 12 / AUTO`。固定分割でも各境界を近い拍頭/アタックへ寄せる。AUTOは強いアタック数から4〜12分割を選ぶため、5 / 7 / 9 / 10個など素材に自然な数も選べる。判定が曖昧な場合は8分割に戻す
- Chopページは4 / 8 / 12 / AUTO / TAP CUTの切断点を波形上の縦線で表示する。切断点の手動移動編集は行わない
- `KEEP SPEED`でも、Start/End範囲から拍頭と楽曲Keyを解析する。Pattern Beatが選ばれている時は、PCMの速度を変えずにPatternイベントを素材のフレーズ長へ比例配置し直し、Beat Repeatは`1 / 2 / 4`から現在の拍感に最も近い値を自動選択する
- Fn1 `PLAY`: 分割予定のSliceをP1から順に1つずつプレビューする。FIT時は変換後と同じ速度・音程で確認し、方式または分割数を変えるとP1へ戻る。Fn2 `CHOP`: 実行、Fn3 `BACK`: 通常Editへ戻る
- 等分位置を音楽的な拍頭 `Beat Anchor` として保存する。実際のPCMはAnchor前後に約8msの重なりを残し、その端だけを近傍のゼロクロスへ寄せる
- Chop PadのLoop再生はイベント位置をAnchorとして扱い、Anchorまでの音を前周回から先行再生する。最初の周回の0msイベントはAnchorから再生し、無音になるのを防ぐ
- Loop/BGM中のライブ入力はAnchorを最寄りNote Gridへ合わせる。早い入力は先行再生を予約し、少し遅い入力はプリロール内を途中から再生してAnchorを合わせる
- `FIT`は一般的なサンプラーと同じく再生速度と音程を一緒に変える。音程を保つタイムストレッチはあえて行わない
- `CHOP`確定時、KEEPは元のPCM AssetをSlice群で共有する。FITは変換後PCMを1本だけAsset化してSlice群で共有する。元素材をPadから削除しても、Sliceが残る限りAssetは解放されない
- Chop数が4〜6なら先頭4 Slice、7〜12なら先頭8 SliceをMake Loopの1周とする。DONEでも既存Recの長さは同じ75%/150%の倍率判定を使い、素材全体の長さはLoop長の基準にしない
- Sample Copyは元PadのPCM、Start/End、Volume/Pitch/Reverse、Hold/Repeat、Sustain Loop、Releaseなどを一切変更せず復元し、コピー先だけに現在のStart/End範囲を独立AssetとしてBakeする。実効範囲が3秒以下なら編集設定も座標を補正して複写する。3秒を超えるコピーは長い素材を安全に切り出す用途として、複写先のHold/Repeat/Sustain設定を初期化する。コピー確定時は一時的なMoveで付け替えたRecイベントも元Padへ戻す
- CHOP実行時は、実際に配置する変換後PCMの複数区間から12音のクロマと低域のベース分布を解析する。コード構成音が現在のScaleに収まり、低域の中心とも整合するKeyを選ぶ。Scaleは維持し、判定に十分な確信がある場合だけMelody / Bass / Chord共通のKeyを自動設定する。打楽器や判定の曖昧な素材ではKeyを変更しない
- BPM値はUIに出さない。ユーザーはBGMのテンポを数値設定せず、耳で素材を選ぶ
- `FIT`成功後は基準にしたBGMを維持する。`KEEP`成功後はBGM音声を消去するが、BGMが作ったLoop長と64グリッドは残す。上書きするPadの既存Loopイベントのみ削除する
- FIT用の変換PCMは一時確保し、変換後はSlice群で共有する。変換に失敗した場合はBGMや既存Padを変更しない

Synthページ:

- Pad 8 `Edit`: 通常のSample Editへ戻る
- Pad 9 `IN`: Sustain Loopの開始位置を選択
- Pad 10 `OUT`: Sustain Loopの終了位置を選択
- Pad 11 `REL`: Releaseを `10 / 30 / 80 / 120 / 200 / 500 / 1000 / 2000ms` から選択
- Pad 12 `SUS`: `Off / Auto / On` を選択。Autoは波形の安定区間を解析し、OnはIN/OUTを直接使用する（内部保存上はManual）
- 波形上ではSustain区間を薄い縦線群とIN/OUTマーカーで表示する
- One ShotではAttackとSustain Loopを鳴らした後に自動Release、Holdではボタンを離した時にReleaseへ移る
- SynthページのFn1プレビューはPadのHold設定にかかわらず、押している間Sustainを継続し、離した時にReleaseへ移る。通常Edit／通常演奏ではPadのHold設定を反映する
- `Sustain` と `Reverse` は併用不可。片方を有効にすると他方をOffにする
- `Sustain` と `Rep: Whole Sample` は併用不可。Sustain有効中のRep選択肢からWhole Sampleを除外する
- Note Grid基準のRepeatはSustainと併用でき、Sustain音を選択Gridで再トリガする
- Pitchは併用可能。Loop位置はPCMフレームで保持し、Pitchに応じてLoop時間とRelease到達時間が自然に伸縮する

誤操作の影響が大きいMelody／Bass／Chord割当とDeleteは、3.2秒以内の2回押しで確定する。確認メッセージは英語2行表示とする。

EDIT Padは通常演奏用の12色を流用せず、機能カテゴリごとの暗い色面とアクセント色を使う。Start／End／Vol／Pitch／Rep／Hold／Reverse／Synthのうち、エンコーダーで現在編集する対象だけを明るい面・白文字・下線で示す。Hold／Repeat／Reverse／SustainはON状態を中間の明るさとアクセント枠で示し、フォーカスと混同しない。Melody／Bass／Chord／Deleteは即時コマンド、空きPadはニュートラルな濃灰色とする。

ENC2:

- `START` / `END`: 20ms単位で範囲編集
- `VOLUME`: 約5%単位で0〜200%編集
- `PITCH`: 約5%単位で50〜200%編集。再生速度を変える軽量方式で、音程と長さが同時に変わる
- `REPEAT`: `None / Whole Sample / 8 / 4 / 2 / 1 / 0.5`を選択
- `HOLD` / `REVERSE`: 正方向でOn、逆方向でOff
- `LOOP IN` / `LOOP OUT`: 20ms単位でSustain区間を編集。編集した時点でManualへ切り替える
- `RELEASE`: 8段階の時間を選択
- `SUSTAIN`: `Off / Auto / On`を選択

- 波形中央には現在選択中の編集パラメーター名と値を小さな透過風アウトラインチップ内に表示する。ENC2で値を変更している間は波形を隠しにくい小型チップへ切り替え、値だけを表示する。1秒間操作がなければ項目名付き表示へ戻る
- Hold／Reverse／Repeat状態と2回押し確認は、波形中央の2行メッセージで表示する
- 左下には `P番号 / 長さ / Vol値 / Pitch値` を表示する

EDITは非破壊です。PCMデータ自体は書き換えず、スロットの再生メタ情報だけを変更します。

## RECモード

目的: Pad演奏を4拍タイムラインへ記録し、繰り返し再生します。

### BeatとLoop

リズム感に自信がないユーザーはAudio Beatを選び、ビートメイク経験者はPattern BeatをPad演奏またはMIDIから作成できます。UI上はいずれも同じBeatパートです。

- 読込場所: `/sampler/loops/`
- Audio対応形式: PCM16 WAVまたはMP3、mono/stereo
- Pattern対応形式: Standard MIDI File `.mid/.midi`、または本体Pad演奏
- MP3はHelixで取り込み時に48kHz / mono / PCM16へ変換し、演奏中はデコードしない
- 内部形式: PCM16 mono
- 推奨長: 3〜8秒程度の2小節/4小節リズムトラック
- 読込上限: 最大8秒
  - 48kHz / PCM16 / stereo / 8秒のWAVを安全圏の一時読込上限とする
  - 常駐データはmono変換後のPCMのみ保持するため、48kHz / 8秒で約768KB
- Audio Beat用にSampler Padとは別の専用ボイスを1つ使う
- 読み込んだAudio Beatの長さとAudio RepeatからLoop長を決める
- Loop再生開始時、Audio BeatはLoop再生位置に同期してループ再生する
- Pad演奏とLoopイベント録音はこれまで通り行える
- Kit v8はBeat形式、名前、音量、Audio参照またはPattern音源12個とLoopイベントを保存する
- Beat音量はBeatメニューの `Beat Volume` で調整する

メモリ目安:

- 44.1kHz / PCM16 / mono: 約88KB/秒
- 8秒BGM: 約706KB
- stereo WAVは取り込み時にmonoへ変換するため、常駐メモリはmono相当。ただし読込中はWAVファイル全体の一時バッファも必要

読込エラー:

- SDが読めない: `No SD`
- 対応ファイルがない: `No Beat file`
- ファイルサイズが安全上限を超える: `Beat file too big`
- Audio形式が対象外: `Bad audio Beat`
- MIDI形式が対象外: `Bad MIDI pattern`
- PSRAM不足: `No Beat memory`

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
  - ライブ発音のSoft Snap対象は通常Sample、Drum、Bass、Chord。Melodyは常に即時発音し、カオシレーター操作とPitch BendにもSnapを適用しない
  - Bass/ChordのSoft SnapはNote Onだけを短い早押し範囲で予約する。Note Offは即時反映し、発音前に離した場合のみ最小Gateを保証する
- 停止中に最初のPadを叩くと自動で再生開始
  - Beat Anchor付きSampleで開始した場合も、前周回のない初回だけはAnchor位置から即時発音し、開始音を欠落させない
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
Repeatは量子化位置から先の最終ミックスを取り込み、PCM区間として反復するMaster Repeatです。
DelayはNote Gridから算出した時間で最終ミックスを反復する、1タップのステレオFeedback Delayです。
スクラッチはBGMボイスを直接動かさず、リミッター後の最終ミックス履歴を読み出すMaster Scratchです。
録音入力はFX前の信号を使います。

Pad:

- Pad 1〜4: Repeat `4 / 2 / 1 / 0.5` Grid
- Pad 5: Filter
- Pad 6: Tempo（従来のPitch/Speed FX）
- Pad 7: Tape Stop
- Pad 8: Delay
- Pad 9〜10: FX Target `BEAT / PARTS`
- Pad 11〜12: 将来拡張用
- FX Padの同時適用はせず、後から押したPadを有効にする
- FX Padは待機中を濃灰色の面と機能色の文字／枠で表示し、押下中だけ機能色の面・白枠・黒文字へ反転する。役割色はRepeat=黄、Filter=水色、Tempo=赤、Delay=緑、Tape Stop=紫とする
- Pad表示は幅に合わせて `REP / FIL / TMP / DLY` を使う。将来拡張用のPadは空Sampleと同じ濃灰色で表示する

ENC2:

- 回転: フォーカス中のFXパラメータを変更
- 押したFilter/Tempo/Delay Padへフォーカスを合わせる
- Padから指を離しても、パラメータ値は保持される
- FX適用はPadを押している間だけ
- Filter/Tempo/Delay Padの押下中は、ENC2/3に加えて筐体の左右の振りでも値を変更する。押下角を0度として±60度を全可動範囲とし、左振りで値を下げ、右振りで上げる。押下時はFilter/Tempoを0、Delayを2 Gridへ戻す。押下中にENC2/3を操作したら、その操作が終わるまで姿勢入力を無視する

パラメータ:

- 初期値: Tempo / Filter / Repeatは0、Delayは2 Grid
- Pitch: UI表示/操作値は -50〜+50。内部では2倍感度で適用し、±50で従来の最大効果へ到達する。0で原音、マイナスで低く遅く、プラスで高く速くする。音長維持型のピッチシフトは処理負荷を考慮して実装しない
- Tempoを押した瞬間のLoop位置を通常速度の基準時計として保持する。解除後はグリッドを待たず、位相差に応じた約240〜1000msの加速／減速演出で基準時計へ追従する。厳密な追従時間より短いテープ操作感を優先する
- 復帰速度は0.5〜2倍の範囲に制限する。復帰終了時はLoop時計を基準位置へ揃え、BGMの残差だけ既存の2msフェード付きseekで補正する。SAM2695の内蔵シンセ音程は変更しない
- Filter: UI表示/操作値は -50〜+50。内部では2倍感度で適用する。0で原音、マイナスは深いローパス、プラスは低中域を少し残しつつ高域を最大約2倍へ強調する演奏向けのHIキャラクター。Filter出力は64bit余裕を持って既存リミッターへ渡し、極端な設定でも先行する整数飽和を避ける
- Repeat: ループクオンタイズ幅を基準に `8 / 4 / 2 / 1 / 0.5` ステップの5段階
- Repeat開始位置は押下後の最寄りの量子化グリッドとし、その位置から選択幅ぶんを一度通常再生しながらDeck Bufferへ取り込む
- BGM、Sample、Padシンセ、SAM2695を含む最終ミックスをPCMで反復するため、すでに鳴っている長いSampleやReleaseの途中からでも音を保持できる
- Repeat中もメインのループカーソル、BGM、各ボイスとNoteイベントは裏で進み続ける。解除時は10msのクロスフェードで現在のミックスへ戻り、seekやボイス再生成を行わない
- 最初の開始位置から最大4 Gridぶんを同じ共有バッファへ保持する。Repeatを押している間に `4 / 2 / 1 / 0.5` を切り替えても開始位置は固定する
- Delay: `4 / 2 / 1 / 0.5 Grid` の4段階。初期値は `2 Grid`。Loop/BGMがない場合は仮想4秒Loopを基準にする
- DelayはWet 50%、Feedback 40%の1タップ方式とし、入力を離した後も最大2秒以内で自然減衰する。Wet/Feedbackは演奏向けの固定値とし、ユーザー設定を増やさない。押下中のGrid変更は新旧タップを10msでクロスフェードする
- DelayのFeedbackを含む合成結果は既存ピークリミッターへ渡す。最終ミックス録音にはDelay結果を含めるが、Loopイベントとしては記録しない
- Master Repeat、Master Scratch、Tape Stop、Delayは同じDeck Bufferを排他的に利用し、追加の大容量PSRAM確保を行わない

### Master Scratch（FX中の右上レバー）

- FX画面中だけ、Tape Stop用PSRAMを共用してスクラッチ前の最終ステレオミックスを循環保存する
- BGM、Sample、Padシンセ、SAM2695のMelody/Bass/Chord/Drumをまとめてスクラッチする
- 操作中も本来のLoop、BGM、各ボイスは裏で進行する。解除時はseekせず、10msのクロスフェードで現在のミックスへ戻る
- リングへ書くのは常に未加工のミックスで、スクラッチ出力を再度履歴へ入れない
- 解除後もFX画面中は履歴書き込みを継続するため、即座に再スクラッチできる。FX画面へ入った直後は蓄積済みの範囲だけを使う
- Tape Stop、Master Scratch、Master Repeat、Delayは同時に読み出さず、後から行った操作を優先する
- 最終ミックス録音はDeck処理後に行うため、スクラッチの結果もWAVへ残る
- 演奏録音の停止後は、SD上の隠し一時WAVを保持した確認状態に入る
- 確認ポップアップまたはFn1を短押しすると Performance_NNN.wav へ改名して保存し、長押しゲージを完了すると一時WAVを削除する
- 確認中は誤操作による画面遷移や演奏を防ぎ、Enc1の音量操作だけを維持する。保存失敗時は一時WAVを残し、再試行または削除を選べる

### パート別メニュー

- Beatの読み込み、パターン、テンポ、音量、繰り返し設定はBeatページを開いている時だけ表示する
- Sampler、Bass、Melody、Chordページでは、現在のパート設定、Loop、Key/Scale、External Device、Wi-Fi、Systemだけを表示する

### パート音量

- PadのVolは各サンプル素材の基準音量として保存する
- SamplerメニューのVolumeはキットへ保存されるSamplerパート全体の音量で、PadのVolに乗算する
- FX MixerのSamplerフェーダーは演奏中だけの相対音量としてその後段に乗算し、Loop停止時は他パートと同様に100%へ戻す

### Pad Repeat（右上レバー）

- レバー下は1グリッド、レバー上は0.5グリッドごとに、押下中Padを再トリガする
- 先にPadを押している場合は次のグリッドから開始し、レバーを倒したままPadを押した場合は即時に開始する
- 複数Padを同時に対象にできる。レバーまたはPadを離すと対象PadのRepeatだけを止める
- LOOPモードでは、Pad Repeatが生成した量子化済みのNote On / Hold用Note Offを通常のLoopイベントとして記録する

FXモードのPadはFXトリガ専用で、通常演奏には使用しません。

### Mixer

FXモードでFn3を押すと、通常のFX PadとMixerを切り替えます。Fn3を押している間だけではなく、もう一度Fn3を押すまでMixerを維持し、両手で操作できるようにします。FXモードから離れた場合は通常FXへ戻ります。

- Pad 1〜3: `BEAT / SAMPLER / BASS`
- Pad 5〜6: `MELODY / CHORD`
- インフォメーションエリアも上段を `MELODY / CHORD`、下段を `BEAT / SAMPLER / BASS` とし、物理Padと位置を揃える
- Part Padを短く押して離す: そのパートの記録済みシーケンスのMuteを切り替える。Mute中もPadの生演奏は発音する
- Part Padを保持しながらENC2またはENC3を回す: そのパートのVolumeを5%単位で変更する
- Part Padを保持しながら筐体を左右に振る: 押下角を基準に3度ごと5%、左振りで下げ、右振りで上げる。同じVolume変更経路で増減し、押下中にENC2/3を操作したら姿勢入力を無視する
- エンコーダーを動かした場合、Padを離してもMuteは切り替えない
- Mute中の短押し解除はMute前のVolumeへ即座に戻す
- Mute中にPart Padを保持してエンコーダーを上方向へ回すと、即座にMuteを解除し、Volumeを0%から5%単位で上げる。下方向ではMuteを維持する
- 複数のPart Padを同時に押して、複数パートのMuteを素早く切り替えられる
- Mixer Volumeは各パート固有のVolumeを上書きせず、その後段に掛かる0〜100%のグループ音量とし、記録再生と生演奏の両方に適用する
- Sample/Pad音源の再生中Volume変更は、オーディオタスク側で数msかけて目標値へ移動し、クリックノイズを避ける

Pad 9〜12はMix A〜Dです。

- 表示は大きな数字の `1 / 2 / 3 / 4` とする。空きMixは黒いPad、保存済みMixは青い縁と文字、呼び出し待ちは白枠の点滅、適用中は明るい青面と黒文字で示す
- 短押し: 保存済みのMixを呼び出す
- 長押し: 現在の5パートのVolume/Muteを保存する
- Loop再生中の呼び出しは次のループ先頭で適用し、停止中は即時適用する
- 呼び出し時のVolume変化も滑らかに適用する
- Mix適用後にPart VolumeまたはMuteを変更した場合は、適用中表示を解除する
- Mixに保存するのは5パートのVolume/Muteのみ。Loopイベント、音色、FX値は含めない
- Mixer状態とMix A〜DはKitおよび終了時状態へ保存する

## タッチ操作

画面上のPad領域をタッチするとPad演奏します。
モードタブ領域のタッチでモード切替します。

## 既知の制約

- KitメニューでSD上のSampler WAV参照、Beat Audio/Pattern、EDIT情報、Loopイベント、FX値をJSON保存できます。
- 録音直後のSampler音源はSDのセッション領域へ退避し、Resume KITから復元します。SDがない場合はRAM上だけで有効です。
- Kit/Beat/Sampleは本体とFile Editorのファイル選択UIから選択します。
- 外部マイクの検出は物理検出ではなく入力レベル判定です。
- REC中は出力をミュートするため、録音中のモニタリングは行いません。
- Audio Beat未使用時のLoop長は新規記録時の `END` Fnタイミングで確定します。Audio Beat使用時はAudio長とRepeatから決まります。
- FXは現状マスターFXのみで、Pad個別FXは未実装です。
- `esp-idf-size --ng` 警告がPlatformIOビルド中に出ますが、ファームウェア生成と書き込みは成功します。

## 今後の実装候補

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
