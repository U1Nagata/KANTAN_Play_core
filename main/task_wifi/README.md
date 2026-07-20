# Wi-Fi API Layer

このフォルダは、KANTAN PlayのWi-Fi機能におけるHTTP API実装です。

- `task_wifi_api.hpp`: API URIの登録／解除を公開する境界
- `task_wifi_api.cpp`: SDカードのファイル操作、ライブソング・進行データのHTTP API

Wi-Fi無線、STA/AP設定、WPS、OTA、HTTPサーバーの起動停止は、親ディレクトリの
`main/task_wifi.cpp` が担当します。新しい `/api/...` エンドポイントはこのフォルダに追加します。

詳細は [Wi-Fi APIモジュール解説](../../docs/development/core/wifi-api-module.md) を参照してください。
