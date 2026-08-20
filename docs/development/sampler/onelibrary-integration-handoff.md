# OneLibrary 対応 開発引き継ぎ

## 1. 目的

rekordboxから書き出したSDカードを KANTAN Sampler で読み、Music音源と以下の解析済み情報を利用できるようにする。

- トラック名、音源パス、プレイリスト
- キー
- BPM
- ビートグリッドの絶対時刻
- 拍頭、ダウンビート
- 将来候補: Cue / Hot Cue / Phrase

本対応の最重要目的は、BPM数値の表示ではなく、**曲中の複数の絶対拍位置をMusicとRecループの同期基準に使うこと**である。現在の音声解析で推定した一つの周期長だけを使うより、長時間再生時のドリフトを補正しやすい。

## 2. 用語と対象形式

- **rekordbox**: AlphaThetaのDJ楽曲管理ソフト。
- **OneLibrary**: 新しいエクスポートライブラリ形式。旧称`Device Library Plus`。
- **Device Library**: 従来のエクスポートライブラリ形式。

rekordbox 7.2.11以降は、SD/USBへのエクスポート時にOneLibraryとDevice Libraryの両方を自動生成するとAlphaThetaが案内している。将来性を考慮し、実装は**OneLibrary優先、Device Libraryは後方互換の第2段階**とする。

> 注意: 公式資料は収録される情報と互換性を説明しているが、ファームから直接読むための低レベルスキーマが全て公開されているとは限らない。ファイル構造と利用条件は実物SDで検証すること。

## 3. 現在の KANTAN Sampler 実装

### Music再生

- `main/sampler/sampler_music_player.hpp/cpp`
  - SD上のWAV/MP3をストリーミング再生する。
  - 出力は48kHz stereo。
  - `positionFrames()`はMusic/Rec同期用の48kHz絶対フレーム位置を返す。
  - `keyAnalysis()`と`timingAnalysis()`は、メタデータがない場合の音声解析用。
- `main/sampler/sampler_app.cpp`
  - Musicロード、キー反映、ループ周期の検出、MusicをマスターとしたRec同期を管理する。
  - `music_detected_cycle_output_frames`は検出したMusic周期の48kHzフレーム数。
  - `music_loop_sync_origin_frame`を起点に、Music位置からRecループ位置を計算している。

### 現在の制約

- ファイル一覧は原則`/sampler/music/`以下を対象とする。
- 現在の音声解析は、キーとループに使う周期長を推定する。
- 曲全体の可変ビートグリッドは保持していない。
- SDの長時間占有と重い解析はMusicストリームの安定性に影響するため、OneLibrary解析は再生タスクと分離する。

## 4. 推奨アーキテクチャ

OneLibraryのデータ構造をMusicプレイヤーに直接混ぜない。フォーマット固有層をアダプターとして分離する。

```text
SD Card
  └─ OneLibrary / Device Library
       └─ library adapter (read-only)
            └─ sampler_music_metadata_t
                 ├─ Music file browser
                 ├─ Key / Scale application
                 ├─ Loop-grid derivation
                 └─ Long-term beat synchronization

Metadata unavailable / invalid
  └─ Existing audio key and timing analysis
```

### 内部中立データの候補

```cpp
struct sampler_music_beat_t {
  uint64_t source_frame; // 原音サンプルレート基準の絶対位置
  uint8_t beat_in_bar;   // 1..N、0=不明
};

struct sampler_music_metadata_t {
  char audio_path[256];
  char title[128];
  char artist[128];
  float bpm;
  int8_t key_pitch_class; // C=0..B=11、-1=不明
  bool minor;
  uint32_t source_sample_rate;
  std::vector<sampler_music_beat_t> beats;
};
```

実装時はESP32-S3のメモリを考慮し、固定長テキスト、ビート位置の差分符号化、必要区間だけのオンデマンド読み込みを検討する。`float`のBPMは表示・参考用であり、同期には絶対フレーム位置を使う。

## 5. KANTANのグリッドへの変換

KANTAN SamplerはBPMを意識させず、ループ長をグリッドで分割する製品である。OneLibraryのBPMをそのままUIの中心にしない。

1. ロードしたMusicのビートグリッドから、連続する8/16/32拍候補の実時間長を求める。
2. 現行の体験に合わせ、基本的に4〜8秒に収まる音楽的な区間を選ぶ。
3. 選んだ区間を本機のループ全長とNote Gridに変換する。
4. 同期は推定BPMを累積せず、直近のOneLibrary拍位置とMusicの`positionFrames()`を比較する。
5. 補正はノートを落とさない範囲で行い、大きな位相ジャンプは原則として次のループ境界で適用する。

可変テンポ楽曲は、ビート配列をそのまま使う。一つの平均BPMに置き換えない。

## 6. フォールバックと優先順位

トラックごとに以下の順で情報を選ぶ。

1. 妥当性検査を通過したOneLibraryメタデータ
2. 将来対応するDevice Libraryメタデータ
3. KANTAN用サイドカーJSONメタデータ
4. 現在の音声キー・ビート解析
5. 全て失敗した場合は、現在のKey/Loop設定を変えない

メタデータのトラックパスが存在しない、ビート時刻が非単調増加、キー値が範囲外などの場合は、その項目だけを無効にする。

## 7. 実装フェーズ

### Phase A: SD形式調査

- 最新rekordboxで小さな検証SDを作成する。
- 最低3曲を登録する。固定テンポ、可変テンポ、メタデータ未解析の曲を含める。
- プレイリスト、Key、Beat Grid、Cueを設定してエクスポートする。
- SD内のディレクトリ、DB、バージョン、エンディアン、文字コード、パス表現を記録する。
- 日本語、濁点、長いファイル名を含める。
- 使用する外部パーサーがある場合、ライセンスと商用製品への組み込み可否を先に確認する。

### Phase B: ホスト上のリーダー

- Mac上でOneLibraryを読み、中立メタデータをJSON出力する調査ツールを先に作る。
- rekordbox画面の値と、トラックパス、Key、BPM、Beat Grid時刻を照合する。
- パーサーとKANTANのグリッド変換を単体テスト可能にする。

### Phase C: ファーム組み込み

- SD挿入時または`Load Music`開始時にOneLibraryを検出する。
- 最初はプレイリスト非対応でもよい。トラック一覧、パス、Key、Beat Gridを優先する。
- ライブラリDBは読み取り専用で開き、書き込み・更新・削除を行わない。
- Music再生開始前に必要メタデータを開放可能な小型キャッシュへ変換し、ライブラリDBのSDハンドルを閉じる。
- Musicデコード中にDBを頻繁にランダムアクセスしない。

### Phase D: 同期

- OneLibrary拍位置からKANTANループ長を決定する。
- Music開始位置をユーザー操作の起点とし、曲全体の固定ゼロ点へ強制ジャンプしない。
- 開始後は最寄りのビートグリッドを位相参照にする。
- 音楽的に目立つ補正を避け、小さな誤差はループ期間の微調整、大きな誤差は境界で再同期する。

## 8. UI方針

- 通常の`Project > Load Music`を維持する。
- OneLibraryを検出した場合は、SD上の通常Musicとプレイリストを同じファイラー体験で表示する。
- ユーザーにOneLibraryとDevice Libraryの違いを選ばせない。
- Key/Beat Gridが読めた場合は即時適用し、読めない場合は現在の`DETECTING BEATS`等の解析に進む。
- 判別が必要な場合だけ、`GRID DATA` / `ANALYZING AUDIO`程度の短いステータスを出す。
- rekordboxの商標やロゴをUIで使う場合は、別途表示ルールを確認する。初期実装は`DJ Library`など中立表示でもよい。

## 9. 保護要件

- OneLibraryとDevice Libraryのファイルは必ず読み取り専用で扱う。
- 不正サイズ、循環参照、壊れたレコード、過大文字列でメモリを使い切らない。
- ビート数と文字列長に上限を設ける。
- パスはSDルート以外を参照できないよう正規化する。
- 解析失敗はMusic再生失敗にしない。音源ファイルが読めれば、メタデータなしで再生する。
- File Editor、Music再生、ライブラリ読み取りが同時にSDを所有しないようにする。
- SD脱去、ファイル欠落、デコードエラー後もカードを再マウントできること。

## 10. 受け入れ条件

- rekordboxから書き出したSDのトラックを本体から選択・再生できる。
- 日本語名、濁点、サブフォルダの音源を扱える。
- rekordboxのKeyがKANTANのKey/Scaleに正しく反映される。
- ビートグリッドから本機のループ長が決まり、8の倍数で分割して音楽的に成立する。
- 3分以上の固定テンポ曲で、MusicとPattern Beat/Recが知覚可能なドリフトを起こさない。
- 音声解析の結果よりOneLibraryメタデータが優先される。
- OneLibraryがないSDの従来Music再生が変わらず動作する。
- 壊れたライブラリを挿してもクラッシュせず、音声解析へフォールバックする。
- 読み取り後もライブラリDBとrekordboxの音源ファイルがバイト単位で変化しない。

## 11. 別タスクへの開始指示

新しい開発タスクでは、以下のように指示する。

> `docs/development/sampler/onelibrary-integration-handoff.md`を仕様として読み、OneLibrary対応のPhase Aから開始してください。まず実物のrekordboxエクスポートSDの構造を調査し、ファームは編集せず、読み取れる情報、フォーマット、ライセンス上の制約、推奨パーサー方針を報告してください。OneLibraryファイルには書き込まないでください。

実物SDが準備できた後は、そのボリュームパスも指示に追記する。

## 12. 公式参考資料

2026-08-19確認。実装開始時に最新情報を再確認すること。

- [AlphaTheta: Important notice for customers using USB devices with our DJ equipment](https://alphatheta.com/en/information/important-notice-for-customers-using-usb-devices-with-our-dj-equipment/)
- [AlphaTheta Support: What is Device Library Plus?](https://support.alphatheta.com/en-us/articles/16290620247321)
- [AlphaTheta Support: OneLibraryとDevice Library Plusの関係](https://support.alphatheta.com/ja/articles/51301232380185?product=51297420966937)
- [rekordbox Device Library Plus guide](https://cdn.rekordbox.com/files/20231208144230/rekordbox6.8.1_Device_Library_Plus_guide_EN.pdf)
- [rekordbox manual](https://cdn.rekordbox.com/files/20230316171900/rekordbox6.7.0_manual_EN.pdf)
