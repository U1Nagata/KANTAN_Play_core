# Wi-Fi API モジュール解説

`main/task_wifi/` は、KANTAN PlayのWi-Fiタスクが使うHTTP API実装を置くフォルダです。
Wi-Fi無線、アクセスポイント、OTA、HTTPサーバーの起動停止は `main/task_wifi.cpp` に残し、
アプリデータを扱うAPIだけを分離しています。

## 構成

| ファイル | 役割 |
|---|---|
| `task_wifi_api.hpp` | `main/task_wifi.cpp` が呼ぶ、API URIの登録／解除だけを宣言する小さな公開境界。 |
| `task_wifi_api.cpp` | SDカードのファイル管理とライブのソング／進行データを扱う `/api/...` HTTPハンドラを実装。入力検証とURI登録を担当。 |

## `main/task_wifi.cpp` との境界

`main/task_wifi.cpp` はWi-Fiドライバのライフサイクル、STA/AP設定、WPS、DNS、設定ページ、
WebSocket、OTA接続フロー、HTTPサーバーの起動停止を担当します。サーバーを作成後、次を呼びます。

```cpp
task_wifi_api_register_uris(server);
```

サーバー停止時には対になる解除関数を呼びます。新しい `/api/...` エンドポイントはこのフォルダに置き、
無線状態やアクセスポイント制御は `main/task_wifi.cpp` に置いてください。

## `task_wifi_api.hpp`

- PCシミュレーター以外では、ESP HTTPサーバーの型だけをインクルードする。
- URIやハンドラ実装の詳細を `task_wifi.cpp` が知る必要がないよう、2つの関数だけを公開する。
- `M5UNIFIED_PC_BUILD` では除外する。ネイティブシミュレーターはESP-IDFのHTTPサーバーを起動しない。

## `task_wifi_api.cpp`

### ファイルAPI

ファイルAPIはホワイトリストに限定します。現在書き込み可能な場所は次の通りです。

| URLトークン | データ種別 | 拡張子 |
|---|---|---|
| `songs/user` | ユーザーソング | `.json` |
| `songs/extra` | 追加ソング | `.json` |
| `arpeggio/user` | ユーザーアルペジオ | `.json` |
| `progression/user` | ユーザー進行 | `.json` |

ハンドラは一覧、読込、保存、リネーム、削除に対応します。`file_manage` に渡す前に、
パストラバーサル、ファイル名中の区切り文字、誤った拡張子、過大なリクエストを拒否します。

### ライブデータAPI

- `/api/song`: 現在のライブソングを取得または置換する。
- `/api/song/save`: 現在のライブソングを指定ディレクトリ・ファイル名で保存する。
- `/api/progression`: 現在のライブ進行を取得または置換する。
- `/api/progression/save`: 現在のライブ進行を保存する。

応答はJSONです。大きなJSONで追加の全量レスポンスバッファを必要としないよう、
`task_wifi_api.cpp` は必要に応じてチャンク送信を使います。

## 変更時の確認事項

1. 書き込み先は `api_dir_table` 経由だけで追加し、リクエストURLから任意のSDパスを組み立てない。
2. 新しいファイル操作でも `api_is_valid_filename()` を使い、拡張子確認を維持する。
3. リクエスト本文を確保した後は、すべてのエラー経路で `memory_info_t` を解放する。
4. 新しいURIも既存の登録／解除関数を通す。停止済みFile Editorに古いハンドラを残さない。
5. SD I/Oは状態を変える操作として扱う。破壊的なファイル操作を許可する前に音を停止する責務は、
   上位のWi-Fiフローに置く。
