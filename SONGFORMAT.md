# KANTAN Play Core — Song JSON フォーマット仕様

ブラウザアプリやツールが `GET /api/song` / `PUT /api/song` などを通じてやり取りするデータの仕様です。  
HTTP API の接続方法は [API.md](API.md) を参照してください。

---

## 目次

1. [全体構造](#1-全体構造)
2. [トップレベルフィールド](#2-トップレベルフィールド)
3. [progression — コード進行](#3-progression--コード進行)
4. [drum_note — デフォルトドラムノート番号](#4-drum_note--デフォルトドラムノート番号)
5. [slot — スロット](#5-slot--スロット)
6. [part — パート](#6-part--パート)
7. [arpeggio / style — アルペジオパターン](#7-arpeggio--style--アルペジオパターン)
8. [Progression 単体フォーマット](#8-progression-単体フォーマット)
9. [列挙値リファレンス](#9-列挙値リファレンス)
10. [デフォルト値一覧](#10-デフォルト値一覧)
11. [完全なサンプル JSON](#11-完全なサンプル-json)
12. [バージョン履歴と後方互換性](#12-バージョン履歴と後方互換性)

---

## 1. 全体構造

```json
{
  "format": "KANTANPlayCore",
  "type": "Song",
  "version": 3,
  "num_slot": 4,
  "tempo": 120,
  "swing": 0,
  "base_key": 0,
  "progression": { ... },
  "drum_note": [ [...], [...], [...], [...], [...], [...] ],
  "slot": [ {...}, {...}, ... ]
}
```

`GET /api/song` はこの形式を返し、`PUT /api/song` はこの形式を受け取ります。

---

## 2. トップレベルフィールド

| フィールド | 型 | 必須 | 説明 |
|---|---|---|---|
| `format` | string | ○ | `"KANTANPlayCore"` (固定) |
| `type` | string | ○ | `"Song"` (固定) |
| `version` | int | ○ | スキーマバージョン。現在は `3` |
| `num_slot` | int | — | 有効スロット数 (2〜64、省略時は `slot` 配列の長さから推定) |
| `tempo` | int | — | テンポ BPM (20〜400、デフォルト: 実装依存) |
| `swing` | int | — | スウィング量 0〜100 (%) |
| `base_key` | int | — | ベースキー (0=C, 1=C♯, 2=D, ..., 11=B) |
| `progression` | object | — | コード進行データ (省略時は空の進行) |
| `drum_note` | array | — | デフォルトドラムノート番号 (後述) |
| `slot` | array | — | スロット配列 (後述) |

> **書き込み時の注意:** `PUT /api/song` は非同期処理 (202 Accepted) です。受け付けた後に `task_operator` がロード処理を行います。ファイル名は「匿名アップロード」として扱われ、デバイス上のソング名がリセットされます。

---

## 3. progression — コード進行

コード進行はステップ番号をキーとした「変化点のみを記録する」差分形式です。

```json
"progression": {
  "version": 1,
  "length": 32,
  "timeline": {
    "0":  { "main": "1",  "mod": "M7", "slot": 0, "part": [0,1,2,3,4,5] },
    "8":  { "main": "5",  "mod": "7" },
    "16": { "main": "6~" },
    "24": { "main": "4" }
  }
}
```

### progression フィールド

| フィールド | 型 | 説明 |
|---|---|---|
| `version` | int | 内部バージョン (現在は `1`) |
| `length` | int | 進行の長さ (ステップ数)。0〜10000 |
| `timeline` | object | ステップ番号(文字列) → ステップ定義 のマップ |

### timeline エントリフィールド

| フィールド | 型 | 省略時の動作 | 説明 |
|---|---|---|---|
| `main` | string | 前エントリの値を継続 | メインコードの度数文字列 (後述) |
| `mod` | string | 前エントリの値を継続 | コードモディファイア名 (後述) |
| `bass` | string | ベースなし | ベース音の度数文字列。省略でベース音なし |
| `slot` | int | 前エントリの値を継続 | 使用するスロット番号 (0〜63) |
| `part` | int[] | 前エントリの値を継続 | 有効なパート番号の配列 (例: `[0,1,2]`) |

> **差分記録**: 前のエントリと同じ値のフィールドは省略されます。  
> ブラウザアプリで生成する場合、必ず `"0"` のエントリから始め、全フィールドを明示することを推奨します。

### 度数文字列 (`main`, `bass`)

書式: `<degree>[b|#][~]`

| 部分 | 意味 |
|---|---|
| `<degree>` | 1〜7 のスケール度数 |
| `b` | 半音下 (フラット) |
| `#` | 半音上 (シャープ) |
| `~` | マイナースワップ (メジャーコードをマイナーに、マイナーをメジャーに) |

例:

| 文字列 | 意味 |
|---|---|
| `"1"` | I度 (ルート) |
| `"4"` | IV度 |
| `"5"` | V度 |
| `"3~"` | III度・マイナースワップ (短調の VI 相当) |
| `"7b"` | VII♭度 |
| `"5#"` | V♯度 |
| `"3b~"` | III♭度・マイナースワップ |

### モディファイア名 (`mod`)

| 文字列 | コード記号の例 | 内部 enum |
|---|---|---|
| (省略) | メジャートライアド | `Modifier_None` |
| `"dim"` | dim | `Modifier_dim` |
| `"m7-5"` | m7♭5 (ハーフディミニッシュ) | `Modifier_m7_5` |
| `"sus4"` | sus4 | `Modifier_sus4` |
| `"6"` | 6th | `Modifier_6` |
| `"7"` | 7th (ドミナントセブンス) | `Modifier_7` |
| `"add9"` | add9 | `Modifier_Add9` |
| `"M7"` | M7 (メジャーセブンス) | `Modifier_M7` |
| `"aug"` | aug | `Modifier_aug` |
| `"7sus4"` | 7sus4 | `Modifier_7sus4` |
| `"dim7"` | dim7 | `Modifier_dim7` |

> `"RESERVED"` は使用できません。

---

## 4. drum_note — デフォルトドラムノート番号

全スロット共通のドラムノート番号を定義します。パートごとに 7 つのピッチ (pitch 0〜6) に対するMIDIノート番号を指定します。

```json
"drum_note": [
  [57, 42, 46, 50, 39, 38, 36],
  [57, 42, 46, 50, 39, 38, 36],
  [57, 42, 46, 50, 39, 38, 36],
  [57, 42, 46, 50, 39, 38, 36],
  [57, 42, 46, 50, 39, 38, 36],
  [57, 42, 46, 50, 39, 38, 36]
]
```

- 外側の配列: パート 0〜5 (6 要素)
- 内側の配列: pitch 0〜6 の MIDI ノート番号 (7 要素)

**デフォルトのドラムノート番号:**

| pitch | MIDI note | 一般的な音色 |
|---|---|---|
| 0 | 57 | A3 (クラッシュシンバル系) |
| 1 | 42 | F♯2 (ハイハット・クローズ) |
| 2 | 46 | A♯2 (ハイハット・オープン) |
| 3 | 50 | D3 |
| 4 | 39 | D♯2 |
| 5 | 38 | D2 (スネア) |
| 6 | 36 | C2 (バスドラム) |

スロット個別にドラムノートを変えたい場合は、各 `part` オブジェクト内に `"drum_note"` フィールドを追加します（後述）。

---

## 5. slot — スロット

スロットは演奏音色・アルペジオパターンのプリセット切り替え単位です。最大 64 個。

```json
"slot": [
  {
    "key_offset": 0,
    "step_per_beat": 2,
    "chord_mode": {
      "part": [ {...}, {...}, {...}, {...}, {...}, {...} ]
    }
  },
  { "copy": [0] },
  {}
]
```

### slot フィールド

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `key_offset` | int | `0` | ベースキーへの半音オフセット (-64〜+63) |
| `step_per_beat` | int | `2` | 1拍あたりのステップ数 (1〜4) |
| `chord_mode` | object | — | コード演奏設定 |
| `copy` | int[] | — | `[src_slot]` を指定すると、そのスロット全体を複製 |

**空オブジェクト `{}`**: デフォルト値のスロット（未使用スロット）として扱われます。

**`copy` 参照**: `"copy": [0]` とすると slot 0 の全パートを複製します。参照先は自分より前のスロットのみ有効です。

### chord_mode.part

6 要素の配列。各要素が 1 つのパートを表します。

---

## 6. part — パート

パートは MIDI 音源・ボイシング・アルペジオパターンなどを定義する演奏単位です。1 スロットに 6 パートあります。

```json
{
  "volume": 100,
  "tone": 0,
  "octave": 0,
  "voicing": "Close",
  "loop_step": 1,
  "anchor_step": 0,
  "stroke_speed": 20,
  "enabled": true,
  "pan": 0,
  "drum_note": [57, 42, 46, 50, 39, 38, 36],
  "arpeggio": [ [...], [...], [...], [...], [...], [...], [...] ],
  "style": ["D", "", "U", "M"],
  "copy": [0, 0]
}
```

### part フィールド

| フィールド | 型 | デフォルト | 説明 |
|---|---|---|---|
| `volume` | int | `100` | ボリューム (0〜127) |
| `tone` | int | `0` | MIDI プログラム番号 (0〜127: 通常音源、`128`: ドラムパート) |
| `octave` | int | `0` | オクターブオフセット。1/12オクターブ単位。`-12`〜`+12` が通常範囲 |
| `voicing` | string | `"Close"` | ボイシング種別 (後述) |
| `loop_step` | int | `1` | ループ終端ステップ (1〜64)。アルペジオのループ長 |
| `anchor_step` | int | `0` | アンカーステップ (0〜63)。このステップ以降でループが許可される |
| `stroke_speed` | int | `20` | ストロークスピード (msec)。0〜127 |
| `enabled` | bool | `true` | パート有効/無効 |
| `pan` | int | `0` | パンニング (-5=左、0=センター、+5=右) |
| `drum_note` | int[] | (トップレベルを継承) | このパート個別のドラムノート番号 (7 要素)。トップレベルの `drum_note` と同じ場合は省略される |
| `arpeggio` | array | — | アルペジオパターン (後述) |
| `style` | string[] | — | ストロークスタイル配列 (後述) |
| `copy` | int[] | — | `[src_slot, src_part]` を指定すると、そのパートを複製 |

**空オブジェクト `{}`**: デフォルト値のパートとして扱われます。

**`copy` 参照**: `"copy": [0, 2]` とすると slot 0 の part 2 を複製します。参照先は自分より前のスロットのパートのみ有効です。

> **tone の特殊値 `128`**: ドラムパートとして扱います。このとき構成音の MIDI ノート番号は `drum_note` に従います。

### voicing 文字列値

| 文字列 | 説明 |
|---|---|
| `"Close"` | クローズドボイシング (ピアノ向け)。**デフォルト** |
| `"Guitar"` | ギターボイシング |
| `"Static"` | スタティックボイシング (ベースライン向け) |
| `"Ukulele"` | ウクレレボイシング |
| `"M-Major"` | メジャースケールのメロディボイシング |
| `"M-Penta"` | メジャーペンタトニックスケールのメロディボイシング |
| `"M-Chroma"` | クロマティックスケールのメロディボイシング |

---

## 7. arpeggio / style — アルペジオパターン

アルペジオパターンは、各ステップ・各ピッチの MIDI ベロシティ (0〜127) を格納した 2D 配列です。

```json
"arpeggio": [
  [0, 80, 0, 80, 0, 80, 0, 80],
  [80, 0, 0, 0, 80, 0, 0, 0],
  [0, 0, 80, 0, 0, 0, 80, 0],
  [],
  [],
  [],
  []
],
"style": ["D", "", "D", ""]
```

### arpeggio 配列の構造

- 外側の配列: **pitch 0〜6** (7 要素)
  - pitch 0〜5: コード構成音 (1st〜6th)
  - pitch 6: ドラムパートのみ使用
- 内側の配列: **step 0〜63** の MIDI ベロシティ
  - `0`: そのステップは発音しない
  - `1〜127`: MIDI ベロシティ値
  - 末尾の `0` は省略可

> **保存時の省略**: デフォルト (全て 0) の pitch 配列は `[]` として出力されます。最後の使用ステップより後の要素も省略されます。

### style 配列

各ステップのストロークスタイルを指定します。`arpeggio` と同じステップ数が最大値。

| 文字列 | 意味 |
|---|---|
| `""` (空文字) | 同時発音 (スタイル指定なし) |
| `"D"` | ダウンストローク (低音から高音の順) |
| `"U"` | アップストローク (高音から低音の順) |
| `"M"` | ミュート |

---

## 8. Progression 単体フォーマット

`GET /api/progression` / `PUT /api/progression` でやり取りするコード進行専用の形式です。Song JSON のサブセットです。

```json
{
  "format": "KANTANPlayCore",
  "type": "Progression",
  "progression": {
    "version": 1,
    "length": 32,
    "timeline": {
      "0":  { "main": "1", "mod": "M7", "slot": 0, "part": [0,1,2,3,4,5] },
      "8":  { "main": "5", "mod": "7" },
      "16": { "main": "6~" },
      "24": { "main": "4" }
    }
  }
}
```

> **副作用**: `PUT /api/progression` は再生位置を 0 にリセットします。

---

## 9. 列挙値リファレンス

### base_key (ベースキー)

| 値 | キー |
|---|---|
| 0 | C |
| 1 | C♯ / D♭ |
| 2 | D |
| 3 | D♯ / E♭ |
| 4 | E |
| 5 | F |
| 6 | F♯ / G♭ |
| 7 | G |
| 8 | G♯ / A♭ |
| 9 | A |
| 10 | A♯ / B♭ |
| 11 | B |

### tone (MIDI プログラム番号)

MIDI General MIDI (GM) に準拠した 0〜127 の値です。  
`128` はドラムパートを表す特殊値です (MIDI channel 10 相当の扱い)。

---

## 10. デフォルト値一覧

### パート (part) のデフォルト

| フィールド | デフォルト値 |
|---|---|
| `volume` | `100` |
| `tone` | `0` (Grand Piano) |
| `octave` | `0` |
| `voicing` | `"Close"` |
| `loop_step` | `1` |
| `anchor_step` | `0` |
| `stroke_speed` | `20` |
| `enabled` | `true` |
| `pan` | `0` |

### スロット (slot) のデフォルト

| フィールド | デフォルト値 |
|---|---|
| `key_offset` | `0` |
| `step_per_beat` | `2` |

### drum_note のデフォルト

| pitch | MIDI note |
|---|---|
| 0 | 57 |
| 1 | 42 |
| 2 | 46 |
| 3 | 50 |
| 4 | 39 |
| 5 | 38 |
| 6 | 36 |

### 数値範囲

| 項目 | 最小 | 最大 | 単位 |
|---|---|---|---|
| `tempo` | 20 | 400 | BPM |
| `swing` | 0 | 100 | % |
| `base_key` | 0 | 11 | — |
| `num_slot` | 2 | 64 | — |
| `key_offset` | -64 | +63 | 半音 |
| `step_per_beat` | 1 | 4 | steps/beat |
| `volume` | 0 | 127 | — |
| `tone` | 0 | 128 | (128=drum) |
| `loop_step` | 1 | 64 | steps |
| `anchor_step` | 0 | 63 | steps |
| `stroke_speed` | 0 | 127 | msec |
| `pan` | -5 | +5 | — |
| `progression.length` | 0 | 10000 | steps |
| ファイルサイズ上限 | — | 262144 | bytes (256KB) |

---

## 11. 完全なサンプル JSON

シンプルな 2 スロット・4 コード進行のサンプルです。

```json
{
  "format": "KANTANPlayCore",
  "type": "Song",
  "version": 3,
  "num_slot": 2,
  "tempo": 120,
  "swing": 0,
  "base_key": 0,
  "progression": {
    "version": 1,
    "length": 32,
    "timeline": {
      "0":  { "main": "1", "mod": "M7", "slot": 0, "part": [0,1,2,3,4,5] },
      "8":  { "main": "5", "mod": "7" },
      "16": { "main": "6~" },
      "24": { "main": "4" }
    }
  },
  "drum_note": [
    [57, 42, 46, 50, 39, 38, 36],
    [57, 42, 46, 50, 39, 38, 36],
    [57, 42, 46, 50, 39, 38, 36],
    [57, 42, 46, 50, 39, 38, 36],
    [57, 42, 46, 50, 39, 38, 36],
    [57, 42, 46, 50, 39, 38, 36]
  ],
  "slot": [
    {
      "step_per_beat": 2,
      "chord_mode": {
        "part": [
          {
            "volume": 100,
            "tone": 0,
            "voicing": "Close",
            "loop_step": 4,
            "stroke_speed": 20,
            "arpeggio": [
              [80, 0, 60, 0, 80, 0, 60, 0],
              [0, 80, 0, 0, 0, 80, 0, 0],
              [0, 0, 0, 80, 0, 0, 0, 80],
              [],
              [],
              [],
              []
            ],
            "style": ["D", "", "D", ""]
          },
          {},
          {},
          {},
          {},
          {}
        ]
      }
    },
    {
      "chord_mode": {
        "part": [
          { "tone": 128, "loop_step": 8 },
          {},
          {},
          {},
          {},
          {}
        ]
      }
    }
  ]
}
```

---

## 12. バージョン履歴と後方互換性

### version 3 (現行)

- 空オブジェクト `{}` = デフォルト値 (未使用スロット/パート) の意味に統一
- `"copy":[slot]` / `"copy":[slot, part]` で参照コピーをサポート
- デフォルト値と一致するフィールドは省略（読み込み側は不在時にデフォルトを使用）
- `"num_slot"` フィールド追加

### version 2 (後方互換)

- `"sequence"` キー → version 3 では `"progression"` に変更 (読み込み時にフォールバック対応)
- 末尾以外の空スロット `{}` は直前スロットの複製として解釈（version 3 ではデフォルト値）
- `"play_mode"` フィールドが存在したが現在は無視される

### version 1

- スロット・パートのほぼ全フィールドが常に出力されていた
- `"progression"` の代わりに `"sequence"` キーを使用

> **読み込み時**: `version` フィールドが 3 より大きい場合はログ警告を出しつつ読み込みを試みます。バージョン 1/2 のファイルは現在のデバイスで読み込み可能です。

---

## ブラウザアプリ実装のヒント

### 接続先の取得

デバイスの組み込み UI は `http://kanplay.local/main` を開くと読み込まれます。
このとき `window.KANPLAY = { api: location.origin }` がセットされるため、
同じ HTML を使う場合は `window.KANPLAY.api` を API のベース URL として使用できます。

```javascript
const API_BASE = window.KANPLAY?.api ?? 'http://kanplay.local';
```

### 基本的な読み書きパターン

```javascript
// 現在のソングデータを取得
const song = await fetch(`${API_BASE}/api/song`).then(r => r.json());

// 編集後に書き戻し (非同期: 202 Accepted)
await fetch(`${API_BASE}/api/song`, {
  method: 'PUT',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(song),
});

// SDカードに名前を付けて保存
await fetch(`${API_BASE}/api/song/save?dir=songs/user&name=mysong.json`, {
  method: 'POST',
});
```

### コード進行だけ編集する場合

スロット設定を変えずにコード進行だけ差し替えるには `/api/progression` を使います。  
こちらは同期 (200 OK) で、すぐに反映されます。

```javascript
const progression = await fetch(`${API_BASE}/api/progression`).then(r => r.json());
// timeline を編集 ...
await fetch(`${API_BASE}/api/progression`, {
  method: 'PUT',
  headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify(progression),
});
```

### CORS について

デバイスの HTTP サーバーは CORS ヘッダーを返しません。  
ブラウザから `http://kanplay.local` 上で動く HTML ページ（同一オリジン）からアクセスする場合は問題ありません。  
外部ドメインの Web アプリからアクセスする場合は CORS エラーになります。その場合は：

- デバイスのファームウェアに `Access-Control-Allow-Origin: *` ヘッダーを追加する（要ファームウェア改変）
- またはサーバーサイドのプロキシを経由する
