# KANTAN Play Core（かんぷれ）アプリケーション  
# KANTAN Play Core Application

---

## 概要 / Overview

KANTAN Play Core（通称：かんぷれ）は、M5Stack Core2 以降の機種で動作するオープンソース音楽ガジェットです。  
このプログラムは、専用ハードウェア「KANTAN Play base」と組み合わせてご利用ください。  
KANTAN Play baseは、スイッチ類、MIDI音源、アンプ回路、バッテリー、各種インターフェイスを内蔵した専用デバイスです。  
KANTAN Play baseが接続されていない場合は、プログラムは起動しません。

KANTAN Play Core is an open-source music gadget for M5Stack Core2 or later devices.  
This program is designed to be used together with the dedicated hardware "KANTAN Play base."  
KANTAN Play base is a special hardware device that includes switches, a MIDI sound source, amplifier circuit, battery, and various interfaces.  
The program will not start if KANTAN Play base is not connected.

---

## 特徴 / Features

- 指一本で簡単にコード演奏が可能  
  Play chords easily with just one finger  
- オープンソースによる自由なカスタマイズ  
  Freely customizable thanks to open-source design  
- ソフトウェアと専用ハードウェアの組み合わせに最適化  
  Optimized for use with dedicated hardware  

---

## インストール方法 / Installation

1. このリポジトリをクローンします。  
   Clone this repository.
2. 必要な依存関係をインストールします。  
   Install required dependencies.
3. ビルドしてM5Stack Core2（以降）に書き込みます。  
   Build and flash to your M5Stack Core2 (or later).

---

## ドキュメント / Documentation

- [開発ドキュメント一覧](./docs/development/README.md) — KANTAN Play core / Sampler の資料案内
- [アーキテクチャ](./docs/development/core/architecture.md) — KANTAN Play core の構成とビルド方法
- [HTTP API](./docs/development/core/api.md) — ブラウザからSD上のソング／コード進行を操作するAPI
- [ソング形式](./docs/development/core/song-format.md) — ソングデータ形式
- [Sampler 製品仕様](./docs/development/sampler/product-spec.md) — KANTAN Sampler のUI・体験仕様
- [Sampler プログラム仕様](./docs/development/sampler/program-spec.md) — 実装仕様とデータ形式
- [Sampler 開発ガイド](./docs/development/sampler/development.md) — ビルド・書き込み・開発方針

### 公開用ファームウェア

- `docs/` はGitHub Pagesで公開する書き込みページ、Web UI、現行ファームウェアを置くディレクトリです。
- `ota_bin/` はビルドで更新される**現行OTAバイナリのみ**を置きます。
- 過去のOTAバイナリは [archive/firmware](./archive/firmware/README.md) に保管しています。

---

## ライセンス / License

- このリポジトリ全体はMITライセンスの下で公開されています。詳細は [LICENSE](./LICENSE) をご覧ください。  
  The main repository is licensed under the MIT License. See [LICENSE](./LICENSE) for details.

- `main/kantan-music/` フォルダ内の KANTAN Music API は、別のライセンス条件が適用されます。詳細は [main/kantan-music/LICENSE_KANTAN_MUSIC.md](./main/kantan-music/LICENSE_KANTAN_MUSIC.md) をご覧ください。  
  The `main/kantan-music/` folder contains the KANTAN Music API, which is subject to a separate license. See [main/kantan-music/LICENSE_KANTAN_MUSIC.md](./main/kantan-music/LICENSE_KANTAN_MUSIC.md) for details.

---

## お問い合わせ / Contact

- 技術的なご質問は、[GitHub Issues](https://github.com/InstaChord/KANTAN_Play_core/issues) からご連絡ください。
- その他のお問い合わせは、[公式WEBサイトのコンタクトフォーム](https://instachord.com/contact/) よりご連絡ください。

For technical questions, please use [GitHub Issues](https://github.com/InstaChord/KANTAN_Play_core/issues).
For other inquiries, please contact us via our [official website contact form](https://en.instachord.com/#contact).


---
