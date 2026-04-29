# KANTAN Play Core HTTP API リファレンス

ブラウザや外部ツールから KANTAN Play Core (CoreS3) を操作するための HTTP API 仕様。

- 配信元: デバイス本体に組み込まれた HTTP サーバ (ポート 80)
- ホスト: WiFi STA 接続時は mDNS `kanplay.local`、または取得した IP アドレス
- 文字エンコーディング: UTF-8 / `application/json`
- 認証: **現状なし** (同一ネットワーク内なら誰でもアクセス可能)

---

## 共通仕様

### URL 規約

```
http://kanplay.local/<endpoint>
```

`/api/*` 配下が本ドキュメントが対象とする「データ操作 API」です。`/ctrl`, `/wifi`, `/ws` 等のレガシー UI / WebSocket エンドポイントは末尾「付録」を参照。

### Content-Type

| 場面 | Content-Type |
|---|---|
| リクエスト body (PUT) | `application/json` |
| 成功レスポンス | `application/json` |
| エラーレスポンス | `application/json` |

### エラーレスポンス共通形式

```json
{ "error": "<message>" }
```

| HTTP ステータス | 意味 |
|---|---|
| 400 Bad Request | パラメータ不正 / JSON 不正 / ファイル名不正 |
| 403 Forbidden | 書き込み禁止ディレクトリへの操作 |
| 404 Not Found | ディレクトリまたはファイルが存在しない |
| 405 Method Not Allowed | 当該 URL でサポートされないメソッド |
| 408 Request Timeout | 受信タイムアウト |
| 409 Conflict | リネーム先などが既に存在する |
| 413 Payload Too Large | リクエスト body が上限超過 (256KB) |
| 500 Internal Server Error | サーバ内部エラー (メモリ確保失敗、SD 書き込み失敗など) |

### サイズ上限

| 項目 | 上限 |
|---|---|
| リクエスト body | 256 KB (`def::app::max_file_len`) |
| ファイル名 | 64 文字 |
| クエリ文字列全体 | 256 文字 |

### ファイル名バリデーション

- 空文字禁止
- 先頭が `.` または `/` 禁止
- `/`, `\` 禁止
- `..` を含むのは禁止
- 必須拡張子 (`.json`) 必須・大文字小文字無視

---

## 対象ディレクトリ (`<dir>` トークン)

`/api/files/*` で操作可能なディレクトリは以下のホワイトリストに限定される。

| URL トークン | 実パス | 用途 | 拡張子 | 書き込み |
|---|---|---|---|---|
| `songs/user` | `/songs/user/` | ユーザー保存ソング | `.json` | ○ |
| `songs/extra` | `/songs/extra/` | エクストラソング | `.json` | ○ |
| `arpeggio/user` | `/arpeggio/user/` | ユーザー保存アルペジオパターン | `.json` | ○ |
| `progression/user` | `/progression/user/` | ユーザー保存コード進行 | `.json` | ○ |

これら以外のパスは **404 Not Found** を返す (incbin に焼かれたプリセット類は読み出せない)。

---

## L1: ファイル CRUD

### `GET /api/files/<dir>` — 一覧

**レスポンス例**:
```json
{
  "dir": "songs/user",
  "files": [
    { "name": "mysong.json", "size": 12345 },
    { "name": "another.json", "size": 6789 }
  ]
}
```

**備考**: 内部で `file_manage.updateFileList()` を呼ぶため、SD 上の最新状態が反映される。

---

### `GET /api/files/<dir>/<name>` — 取得

ファイル中身をそのまま返す (`Content-Type: application/json`)。

**例**:
```bash
curl http://kanplay.local/api/files/songs/extra/G1-01_Pop_Basic1.json -o sample.json
```

**エラー**:
- 404: ファイルが存在しない

---

### `PUT /api/files/<dir>/<name>` — 保存 (上書き)

リクエスト body に JSON 全体を送る。既存ファイルがあれば上書き、無ければ新規作成。

**例**:
```bash
curl -X PUT http://kanplay.local/api/files/songs/user/test.json \
     -H 'Content-Type: application/json' \
     --data-binary @local.json
```

**バリデーション**:
- Content-Length ≤ 256KB
- Body 先頭が `{` であること (簡易 JSON チェック)
- 拡張子マッチ (`.json` 必須)

**レスポンス**: `{"result":"ok"}` (200)

**エラー**:
- 400: JSON 不正、ファイル名不正
- 403: 書き込み禁止 dir
- 413: サイズ超過
- 500: SD 書き込み失敗

---

### `DELETE /api/files/<dir>/<name>` — 削除

**例**:
```bash
curl -X DELETE http://kanplay.local/api/files/songs/user/test.json
```

**レスポンス**: `{"result":"ok"}` (200)

**エラー**:
- 403: 書き込み禁止 dir
- 404: 削除に失敗 (存在しない)

---

### `POST /api/files/<dir>/<old>/rename?to=<new>` — リネーム

同一ディレクトリ内のリネーム。

**クエリパラメータ**:

| 名前 | 必須 | 説明 |
|---|---|---|
| `to` | ○ | 新しいファイル名。dir の拡張子要件を満たすこと。URL エンコード可 |

**例**:
```bash
curl -X POST 'http://kanplay.local/api/files/songs/user/old.json/rename?to=new.json'
```

**動作**:
- `to` が既存なら **409 Conflict** (上書き禁止)
- `<old>` が存在しなければ **404 Not Found**
- `to == old` は 400
- 編集中のソングがリネームされた場合、`setLatestFileInfo` を新名で更新 (内部状態が追従)

**レスポンス**: `{"result":"ok"}` (200)

---

## L2: ライブソング

現在演奏中/編集中のソング全体 (`song_data` = progression + 8 slot + tempo 等) にアクセスする。

### `GET /api/song` — 取得

現在のライブソングを Song 形式 JSON で返す。

**例**:
```bash
curl http://kanplay.local/api/song > live.json
```

**レスポンス例** (抜粋):
```json
{
  "format": "KANTANPlayCore",
  "type": "Song",
  "version": 3,
  "tempo": 210,
  "swing": 0,
  "base_key": 0,
  "progression": { "version": 1, "length": 32, "timeline": { /* ... */ } },
  "drum_note": [ /* ... */ ],
  "slot": [ /* ... */ ]
}
```

---

### `PUT /api/song` — 差し替え

リクエスト body のソング JSON でライブ状態を差し替える。

**実装メモ**: HTTP タスクから直接 `song_data` を書き換えると再生中のオーディオタスクと競合するため、内部では **`file_load_notify` キューに投入** し、`task_operator` が既存の `backup_song_data` 経由ロード処理に乗せる (匿名アップロード相当として扱われ、ファイル名は "new_YYYYMMDD_HHMMSS" にリセットされる)。

**例**:
```bash
curl -X PUT http://kanplay.local/api/song \
     -H 'Content-Type: application/json' \
     --data-binary @live.json
```

**レスポンス**: `{"result":"queued"}` (**202 Accepted**)

レスポンス時点ではまだロード処理がキュー投入されたのみで、適用は task_operator のターンで行われる。

---

### `POST /api/song/save?dir=<dir>&name=<name>` — ファイル保存

現在のライブソングを SD に保存する。

**クエリパラメータ**:

| 名前 | 必須 | 値 |
|---|---|---|
| `dir` | ○ | `songs/user` または `songs/extra` |
| `name` | ○ | ファイル名 (`.json` 拡張子) |

**動作**:
- `setLatestFileInfo` を更新 (これが UI 上の「現在のソング名」)
- `updateUnchangedSongCRC32` で「未変更状態」基準点を更新
- `checkSongModified` で変更フラグを再計算

**例**:
```bash
curl -X POST 'http://kanplay.local/api/song/save?dir=songs/user&name=edited.json'
```

**レスポンス**: `{"result":"ok"}` (200)

---

## L2: ライブ進行データ

現在のコード進行 (timeline + length) のみにアクセスする。slot/part の編集状態には触らない。

### `GET /api/progression` — 取得

```bash
curl http://kanplay.local/api/progression
```

**レスポンス例**:
```json
{
  "format": "KANTANPlayCore",
  "type": "Progression",
  "progression": {
    "version": 1,
    "length": 32,
    "timeline": {
      "0":  { "main": "1", "slot": 0, "part": [0,1,2,3,4,5] },
      "8":  { "main": "5" },
      "16": { "main": "6" },
      "24": { "main": "4" }
    }
  }
}
```

**timeline のキー記法**:
- キー: ステップ番号 (文字列)
- `main` / `bass`: 度数文字列。 `"1"`〜`"7"` + 任意で `b`(♭) / `#`(♯) + 任意で `~`(マイナースワップ)。例: `"5"`, `"3b"`, `"5#~"`
- `mod`: モディファイア名。`"7"`, `"M7"`, `"dim"`, `"sus4"`, `"6"`, `"add9"`, `"aug"`, `"7sus4"`, `"dim7"`, `"m7-5"`
- `slot`: 0〜7
- `part`: 有効パート番号配列 (0〜5)
- 前ステップと同値の項目は省略される (差分記録)

---

### `PUT /api/progression` — 差し替え

```bash
curl -X PUT http://kanplay.local/api/progression \
     -H 'Content-Type: application/json' \
     --data-binary @prog.json
```

**副作用**: 再生位置 (`runtime_info.progression_position`) が 0 にリセットされる。

**レスポンス**: `{"result":"ok"}` (200)

---

### `POST /api/progression/save?dir=progression/user&name=<name>`

```bash
curl -X POST 'http://kanplay.local/api/progression/save?dir=progression/user&name=test.json'
```

`dir` は `progression/user` のみ許可。

**レスポンス**: `{"result":"ok"}` (200)

---

## エンドポイント一覧 (早見表)

| メソッド | パス | 用途 |
|---|---|---|
| GET | `/api/files/<dir>` | ファイル一覧 |
| GET | `/api/files/<dir>/<name>` | ファイル取得 |
| PUT | `/api/files/<dir>/<name>` | ファイル保存 |
| DELETE | `/api/files/<dir>/<name>` | ファイル削除 |
| POST | `/api/files/<dir>/<old>/rename?to=<new>` | リネーム |
| GET | `/api/song` | ライブソング取得 |
| PUT | `/api/song` | ライブソング差し替え (非同期、202) |
| POST | `/api/song/save?dir=&name=` | ライブソング → ファイル保存 |
| GET | `/api/progression` | ライブ進行データ取得 |
| PUT | `/api/progression` | ライブ進行データ差し替え |
| POST | `/api/progression/save?dir=progression/user&name=` | 進行データ → ファイル保存 |

---

## 典型的な利用フロー例

### 1. SD 上のソングをダウンロードして編集 → 戻す

```bash
H=http://kanplay.local
curl $H/api/files/songs/user/mysong.json -o local.json
# 外部エディタで local.json を編集
curl -X PUT $H/api/files/songs/user/mysong.json \
     -H 'Content-Type: application/json' \
     --data-binary @local.json
```

### 2. ライブ状態を取得 → 編集 → 即時反映 → 名前を付けて保存

```bash
curl $H/api/song > live.json
# 外部エディタで live.json を編集
curl -X PUT $H/api/song -H 'Content-Type: application/json' \
     --data-binary @live.json
# 気に入ったらファイルに保存
curl -X POST "$H/api/song/save?dir=songs/user&name=edited_$(date +%Y%m%d_%H%M%S).json"
```

### 3. 進行だけ別パターンに差し替えて試す

```bash
# 既存プリセットの中身を取り出してライブに送る
PROG_JSON=$(curl -s $H/api/files/progression/user/sample.json)
echo "$PROG_JSON" | curl -X PUT $H/api/progression \
                         -H 'Content-Type: application/json' --data-binary @-
```

### 4. ファイル名変更

```bash
curl -X POST "$H/api/files/songs/user/old_draft.json/rename?to=final.json"
```

---

## 既知の制限・注意

- **認証なし**: 同一ネットワーク内の任意クライアントから操作可能。配置環境に注意。
- **競合**: SD カードへの I/O は task_operator 等他タスクと排他制御していない。再生中の頻繁な書き込みは推奨しない。
- **アトミック保存ではない**: PUT の途中で電源断するとファイルが破損する可能性がある (現状 `<name>.tmp` → rename パターン未実装)。
- **プリセット読み出し未対応**: incbin に焼かれたジャンルプリセットや preset 進行データは API からは読めない。
- **WebSocket 状態 push 未対応**: ブラウザに対して再生中のステップ/コード等を能動的に通知する仕組みは未実装。

---

## 内部実装の参照箇所

| 機能 | ファイル | 備考 |
|---|---|---|
| API ハンドラ全般 | `main/task_wifi/task_wifi_api.cpp` | `/api/*` の全エンドポイント |
| URI 登録/解除関数 | `main/task_wifi/task_wifi_api.hpp` | `task_wifi_api_register_uris` / `task_wifi_api_unregister_uris` |
| HTTP サーバ起動 | `main/task_wifi.cpp` の `start_webserver` | API 登録呼び出し |
| ホワイトリスト | `task_wifi_api.cpp` の `api_dir_table[]` | 書き込み可能 dir |
| 共通バリデータ | `task_wifi_api.cpp` の `api_is_valid_filename` 等 | |
| ファイル下層 | `main/file_manage.{cpp,hpp}` | `loadFile`/`saveFile`/`removeFile`/`renameFile` |
| ソング JSON | `main/system_registry.cpp` の `saveSongJSON`/`loadSongJSON` | |
| 進行 JSON | `main/system_registry.cpp` の `saveProgressionJSON`/`loadProgressionJSON` | |

---

## 付録: レガシー / UI 系エンドポイント

`/api/*` 以外で既存のもの (本ドキュメントの主対象外、参考まで)。

| メソッド | パス | 用途 |
|---|---|---|
| GET | `/` | `/wifi` へリダイレクト |
| GET | `/wifi` | WiFi 設定ページ HTML |
| POST | `/wifi` | SSID/PASS 受信、STA 接続試行 |
| GET | `/main` | トップメニュー HTML |
| GET | `/ctrl` | キーボード操作ページ HTML (動的生成) |
| GET | `/ssid` | SSID スキャン結果 JSON |
| GET | `/status` | WiFi 接続状態 JSON |
| POST | `/done` | 完了通知 |
| GET | `/ws` | WebSocket。テキスト `cmd=p<raw>` (press) / `cmd=r<raw>` (release) で物理ボタン操作を送信 |
