# Projectとサンプルキット

KANTAN Samplerには、音色だけを保存する`Sample Kit`と、曲全体を保存する`Project`があります。

## どちらを保存するか

| 保存形式 | 保存する内容 | 向いている用途 |
|---|---|---|
| サンプルキット | 12個のサンプル、パッド編集設定、サンプラーパートの音量 | 効果音セット、ドラムセット、別の曲でも使う音色セット |
| Project | サンプルキットの内容、ビート、最大4個のLoop Section、Rec、Loop長、Key / Scale、Bass / Melody / Chord、FX、Mixer | 制作中の曲やライブセット全体 |

迷った場合はProjectを保存します。

Projectと自動復元は、Loop Section数、各SectionのRecとパターンビート、現在選択中のSectionも保存します。
サンプル、Synth音色、Key、Tempo/Groove、FX/Mixer、オーディオビート、MusicはSection間で共有されます。
旧形式のProjectを読み込んだ場合、従来のRecはS1として復元されます。

## Projectを保存する

1. SDカードを挿入します
2. 下のダイヤルを押してメニューを開きます
3. `Project` → `Save`へ進みます
4. 保存方法を選びます

表示される候補は現在の状態によって変わります。

- `Update`：現在のProjectへ上書き
- `Copy`：現在名をもとに複製
- `New`：日時または連番の新しいProjectとして保存

初めて保存する場合は`New`を選びます。

<!-- IMAGE SAVE-01: Save Project画面。Update / Copy / Newの候補が分かる。 -->

## Projectを読み込む

1. `Project` → `Load`へ進みます
2. 読み込みたいProjectを選びます
3. `OK`で確定し、処理が終わるまで待ちます

内蔵Projectの`DISCO Beat`も一覧の先頭から選べます。

!!! warning
    Projectを読み込むと、現在のサンプル、ビート、Recなどが置き換わります。必要な状態は先に保存してください。

## サンプルキットを保存する

1. `SAMPLER`パートを選びます
2. メニューの`Sample` → `Save Sample Kit`へ進みます
3. 保存方法を選びます

- `Update`：現在のKitへ上書き
- `Copy`：現在のKitを複製
- `New`：新しいKitとして保存
- `Save as Default`：自分用のDefault Kitとして保存。必要なときは`Load Sample Kit`から読み込みます

サンプルキットを保存しても、ビート、Rec、Key / Scale、FXは含まれません。

## サンプルキットを読み込む

`Sample` → `Load Sample Kit`から選びます。現在の12個のパッドと編集設定が置き換わりますが、ビートとRecは残ります。

## Clear KitとReset Kit

- `Clear Kit`：12パッドを空にする
- `Reset Kit`：内蔵Project `DISCO Beat`と同じSampler Kit（10音とパッド編集設定）へ戻す

Default Kitは個人用の保存先です。`Reset Kit`の結果には使われないため、SDカードの有無でResetの結果は変わりません。

## 自動復元について

本体は直前の状態を自動保存し、次回起動時に復元します。マイク録音したサンプルも、SDカード装着時は一時保存されます。

!!! warning
    SDカードがない状態で録音したサンプルは、電源を切ったあと復元できません。大切な録音はSDカードを入れ、Projectまたはサンプルキットとして保存してください。

## ファイルをPCへバックアップする

File EditorではProject、Kitと、それぞれに対応する音声ファイルをDownloadできます。RenameやDeleteでは関連する音声フォルダーも一緒に処理されます。

[File Editorの使い方](file-editor.md){ .md-button }
