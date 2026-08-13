# KANTAN Sampler Beat Pattern制作仕様書

## 1. 文書の目的

本文書は、ChatGPT Workなどを使ってKANTAN Sampler用のドラムパターンを企画・制作するための仕様書です。

Workは、音源そのものを作る`Beat Kit`と、演奏位置と強弱を作る`Beat Pattern`を混同しないでください。

- Beat Kit: 12個のドラム・パーカッション音源
- Beat Pattern: どのPadを、いつ、どのVelocityで鳴らすかを記録した演奏データ
- Project: Beat Kit、Beat Pattern、Sample Kit、Synth音色、Key / Scaleなどをまとめた完成状態

次の資料と一緒に参照してください。

- `preset-library-production-guide.md`
- `preset-project-structure-guide.md`
- `product-spec.md`
- `program-spec.md`

## 2. 成果物の形式

Beat Patternの交換形式には、Standard MIDI Fileを使用します。

- 拡張子: `.mid`または`.midi`
- Format: Type 0またはType 1
- 時間形式: PPQN
- 推奨PPQN: 480
- 拍子: 4/4
- Tempo: 最初のTempo Meta Eventに記録
- Drum Channel: MIDI Channel 10
- 使用イベント: Note Onを中心とする
- Note Off: Pattern Beatでは不要
- SMPTE時間形式: 使用しない

MIDI Channel 10のNote Onが1つでも存在する場合、本体はChannel 10を優先します。ドラム以外のチャンネルへPattern Noteを混在させないでください。

WorkはProject JSONを推測して直接生成せず、まずMIDI Patternを成果物として作成してください。完成Projectは本体または対応ツールで保存します。

## 3. PadとMIDI Noteの対応

| Pad | 役割 | MIDI Note | GM名の目安 |
|---:|---|---:|---|
| 1 | Kick | 36 | Bass Drum 1 |
| 2 | Snare | 40 | Electric Snare |
| 3 | Side Stick | 37 | Side Stick |
| 4 | Clap | 39 | Hand Clap |
| 5 | Tom Low | 41 | Low Floor Tom |
| 6 | Tom Mid | 43 | High Floor Tom |
| 7 | Tom High | 45 | Low Tom |
| 8 | Hi-Hat Closed | 42 | Closed Hi-Hat |
| 9 | Shaker | 70 | Maracas |
| 10 | Crash | 49 | Crash Cymbal 1 |
| 11 | Ride | 51 | Ride Cymbal 1 |
| 12 | Hi-Hat Open | 46 | Open Hi-Hat |

このNote配列は、Acoustic Kit、Dance Kit、Chiptune Kitなど、音色が変わっても共通です。

## 4. Velocity仕様

### 4.1 基本ルール

- 有効範囲: `1〜127`
- `0`: Note Offとして扱い、発音イベントにはしない
- 値が存在しない場合: `110`
- 本体の感圧非対応Padから手入力した場合: `110`
- 外部MIDI入力: 受信した`1〜127`を保持
- MIDI Pattern: ファイル内のVelocityを保持
- 古いProjectやVelocity項目のないデータ: `110`

通常値を`110`とすることで、標準演奏に十分な音量を確保しながら、`127`のアクセントを表現できる余地を残します。

### 4.2 Workで使う推奨4段階

| 表現 | 推奨Velocity | 主な用途 |
|---|---:|---|
| Ghost | 50 | ゴーストスネア、補助パーカッション |
| Soft | 80 | 裏拍、ハイハットの弱拍、控えめな装飾 |
| Normal | 110 | 通常のKick、Snare、基本拍 |
| Accent | 127 | 小節頭、強拍、Fill終端、Crash |

内部ではすべての`1〜127`を保持できます。ただし、プリセット制作では強弱の意図が伝わりやすいよう、まず上記4段階を基準にしてください。

### 4.3 音量への適用

Velocityは波形を書き換えず、発音時の音量倍率として使用します。

```text
最終音量 = Pad音量 × Beat音量 × Mixer音量 × Velocity / 127
```

- `127`: 元設定の100%
- `110`: 約86.6%
- `80`: 約63.0%
- `50`: 約39.4%

現段階ではVelocityによる音色切替やマルチサンプル切替は行いません。音量差だけで演奏表現を作ります。

## 5. Patternの長さとタイミング

### 5.1 推奨単位

- 標準: 2小節
- 短い基本Pattern: 1小節
- 展開を含むPattern: 4小節まで。ただし全長8秒以内
- 2小節Patternでは、2小節目の終端に軽いFillや変化を入れてよい
- Pattern終端は小節境界へ正確に合わせる

本体は4/4の小節を基準にPattern長を決めます。最後のNoteの直後でMIDIファイルを終わらせず、意図した小節終端までEnd of Trackの時刻を進めてください。

### 5.2 Grid

- 基本のKick / Snare: 8分音符または16分音符Gridへ正確に配置
- Hi-Hatや細かなPercussion: 必要に応じて16分音符
- Fill: 16分音符を基本とし、必要な場合だけ32分音符
- 意味のない数ms単位のランダムなHumanizeは使わない
- 人間らしさは、まずVelocity差と音数で表現する

### 5.3 Swing

標準PatternはStraight、Swing 0%で制作してください。

Swing専用Patternを作る場合は、裏拍を一定比率で遅らせ、ランダムなズレにしないでください。ファイル名と制作表へSwing量を明記し、本体側のSwingを重ねて適用しないようにします。

## 6. 音楽的な制作ルール

- KickとSnareの基本骨格を先に作る
- Hi-Hatをすべて同じVelocityにしない
- 小節頭、Back Beat、Fill終端に必要なAccentを付ける
- Ghost Noteは主拍より明確に小さくする
- CrashやOpen Hi-Hatを毎拍使わない
- 初心者がMuteや生演奏を加えられる余白を残す
- 音を埋め尽くさず、Sampler、Bass、Melody、Chordが入る空間を残す
- 2小節目は基本Grooveを壊さない範囲で軽く変化させる
- 同じPad、同じ時刻のNote Onを重複させない
- 複数の異なるPadを同時刻に鳴らすことは可能

## 7. データ量と安全基準

- 本体が保持できるLoopイベント総数: 最大512
- この512にはBeat以外のSampler、Bass、Melody、Chord、ユーザーの追加録音も含まれる
- Beat Pattern単体の推奨上限: 128 Note On
- 通常の2小節Pattern: 32〜96 Note Onを目安とする
- 必要のない連打や聞こえないGhost Noteを大量に置かない
- MIDIファイル上限: 512KB
- Pattern再生時間: 最大8秒

本体でBeat Repeatを使用するとPatternイベントが複製される場合があります。Pattern単体で512イベントを使い切らないでください。

## 8. ファイル名と管理表

推奨ファイル名：

```text
Genre_Style_Variant.mid
```

例：

```text
Pop_Basic_A.mid
House_FourOnFloor_A.mid
HipHop_LaidBack_B.mid
Chiptune_Run_A.mid
```

各Patternについて、次の情報を表で管理してください。

| 項目 | 内容 |
|---|---|
| File Name | MIDIファイル名 |
| Display Name | 本体・Web表示名 |
| Genre | Pop / Rock / House / Hip-Hopなど |
| Bars | 1 / 2 / 4 |
| BPM | MIDIへ記録したTempo |
| Swing | 原則0%、専用品のみ明記 |
| Note Count | Note On総数 |
| Velocity Range | 最小値〜最大値 |
| Main Accent | アクセント位置 |
| Recommended Kit | Acoustic / Dance / Chiptuneなど |
| Distribution | Built-in / Official Web Library |
| Notes | Fill、演奏の狙い、注意事項 |

## 9. Built-inとOfficial Web Library

### Built-in Pattern

- 初心者がすぐ演奏できる
- 音数が多すぎない
- 複数のBeat Kitで成立する
- 代表的なジャンルを少数用意する
- 容量よりも、選択肢を増やしすぎないことを重視する

### Official Web Library Pattern

- ジャンル別の追加Groove
- SwingやHalf Timeなど特徴の強いPattern
- 特定Kit向けのPattern
- FillやVariation
- アーティスト、テーマ、OEM向けPattern

Audio Beatと異なり、MIDI Patternは小容量なのでOfficial Web Libraryへ豊富に用意できます。ただし、似たPatternを名前だけ変えて大量に並べないでください。

## 10. Workの制作手順

### Phase 1: 企画

音楽ファイルを生成する前に、次を表で提案してください。

1. Pattern名
2. ジャンルと用途
3. 小節数とBPM
4. Kick / Snareの骨格
5. Hi-Hat / Percussionの役割
6. Velocity設計
7. 2小節目または終端のVariation
8. 推奨Beat Kit
9. Built-inまたはOfficial Web Libraryの区分

### Phase 2: イベント表

承認されたPatternを、MIDI生成前に次の形で提示してください。

| Bar.Beat.Step | Pad | MIDI Note | Velocity | 意図 |
|---|---:|---:|---:|---|
| 1.1.1 | 1 | 36 | 127 | 小節頭のKick |
| 1.2.1 | 2 | 40 | 110 | Back Beat |
| 1.2.3 | 8 | 42 | 80 | 弱い裏拍Hat |

### Phase 3: MIDI生成

- Channel 10へNote Onを配置する
- Velocityを省略しない
- Tempo Meta Eventを入れる
- 意図した小節終端へEnd of Trackを置く
- 同一Pad、同一時刻の重複Noteを除去する

### Phase 4: 機械検証

- MIDI形式が正しい
- PPQNである
- Channel 10にNote Onがある
- Noteが対応表の範囲にある
- Velocityが`1〜127`である
- 未指定値を`110`へ補完した
- Pattern終端が小節境界にある
- Note On数が推奨上限内である

### Phase 5: 実機検証

- Patternを読み込める
- Pattern本来のTempoでPreviewできる
- NormalとAccentの差が聞こえる
- Ghost Noteが消えず、主拍を邪魔しない
- Acoustic / Electronicなど複数Kitで破綻しない
- Beat Repeat後もイベント数に余裕がある
- Sampler、Bass、Melody、Chordを重ねても音量が過大にならない
- Project保存、再読込み後もVelocityが保持される

## 11. ファームウェアでの扱い

Workが作ったVelocity付きPatternは、ファームウェア内で次のように共通処理します。

- 内蔵PatternイベントへVelocityを追加
- MIDI Note OnのVelocityを破棄せず保持
- Loop / RecイベントへVelocityを追加
- Project保存・読込みでVelocityを保持
- Velocity未指定時は`110`
- Pattern Previewと通常再生の両方でVelocityを音量へ反映
- 同一Pad、同一時刻の重複Noteは最大Velocityの1イベントへ統合
- ピアノロールではVelocityを4段階程度の明るさで表示

Velocity項目のない旧Projectは、すべて標準値`110`として読み込みます。正式なPatternライブラリ検証は、Velocity対応ファームウェアで行ってください。

## 12. Workへの最初の指示例

```text
次の仕様書を参照し、KANTAN Sampler用Beat Patternの制作計画を作ってください。

docs/development/sampler/beat-pattern-production-guide.md
docs/development/sampler/preset-project-structure-guide.md
docs/development/sampler/preset-library-production-guide.md

最初はMIDIファイルを生成せず、ジャンル、Pattern名、小節数、BPM、
Kick/Snareの骨格、Velocity設計、Variation、推奨Beat Kit、配布区分を表にしてください。
Velocity未指定時と本体手入力の標準値は110、Accentは127です。
私が構成を承認してから、イベント表とStandard MIDI Fileを作成してください。
```
