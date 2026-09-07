# KANTAN Sampler Manual Content Plan

この文書は、初心者向けWebマニュアルの構成、制作順、画面資料の管理方針を定めます。
製品機能と用語の正本は[`product-spec.md`](./product-spec.md)です。

## 読者と最初のゴール

中心読者は、サンプラーや音楽制作を経験したことがない人です。
最初のページでは機能一覧を説明せず、次の成功体験を順番に作ります。

1. 内蔵の8 Sampleを鳴らす
2. Beatを再生する
3. Beatに合わせてSampleを鳴らす
4. RECへ切り替えて演奏をLoopとして残す
5. Undoで失敗を戻す

この段階では、BeatとLoopの違いを操作結果と結び付けて説明します。

## 初心者向けチュートリアルの順序

1. **最初の5分**: Sample、Beat、Rec、Loop、Undo
2. **音を追加する**: マイク録音、内蔵Sample、SDファイル、File Editor
3. **Sampleを整える**: Start / End、Volume、Pitch、Reverse、Hold、Repeat
4. **Loopを変化させる**: Mute、FX、Mixer
5. **音楽要素を増やす**: Bass、Melody、Chord、Key / Scale
6. **素材を作り替える**: Chop、Sample Synth

マイク録音は製品固有の楽しさが強い一方、最初に録音品質やトリミング判断を要求します。
そのため、初期SampleとBeatで操作の成功を体験したあとに配置します。

## チュートリアル後の入口

- 効果音を任意のタイミングで鳴らす
- Beat、Sampler、Bass、Melody、Chordで音楽Loopを作る
- 曲をChopしてSampleへ分ける
- Music Playerで曲を流し、Sampleや合いの手を重ねる
- DJのようにFXとMixerで展開を作る
- SampleをBass、Melody、Chord用の楽器にする

FXは複数の用途から参照する共通ガイド、Sample Synthは発展ガイドとして独立させます。

## 画面資料のルール

- 写真は「どの物理ボタンを押すか」を示すページに使用する
- 画面キャプチャーは表示文字、選択状態、設定値を読ませるページに使用する
- 1枚の画像で説明する操作は1つを基本とする
- 画像内だけに操作説明を書かず、本文にも同じ操作を記載する
- 写真と実装が異なる場合は、推測せず製品仕様と実機で確認する

最初の基準写真は`docs/docs/ja/sampler/assets/images/device-play-overview.jpg`です。
SAMPLER + PLAY、初期8 Sample、Fnの再生・Mute表示、物理Pad、モードボタン、右側ダイヤルを確認できます。

## 必要な写真・キャプチャー

初回公開へ向け、優先順に次を追加します。

1. 今回の写真と同じ角度で、ボタン位置が読み取れる高解像度の全体写真
2. RECモードへ切り替えた直後
3. Rec中と、記録した演奏がLoop再生されている状態
4. Fn3を押したときのUndo表示
5. 空Padから`Import / Record`を選ぶ画面
6. マイク録音中と録音完了後
7. Sample EditのStart / End画面
8. FX画面とMixer画面

続いてChop、Music Player、Sample Synth、音楽パート、Performance Recording、保存、Wi-Fi、外部入力の画面を追加します。全画像の状態、ID、ファイル名は[`manual-image-briefs.md`](./manual-image-briefs.md)を正本とします。

画像ごとの構図とファイル名は[`manual-image-briefs.md`](./manual-image-briefs.md)で管理します。

## 制作状況

- 完了: マニュアルホーム
- 完了: はじめての5分
- 完了: 画面とボタン
- 完了: Sampleを追加する
- 完了: マイクでSampleを録音する
- 完了: Sampleを編集する
- 完了: File Editor
- 完了: ポン出しとして使う
- 完了: 音楽Loopを作る
- 完了: Beatの選択と変更
- 完了: 演奏をRecする
- 完了: FXとMixer
- 完了: 曲をChopして遊ぶ
- 完了: Music Playerで曲に合わせて演奏する
- 完了: Sample Synth
- 完了: Bass、Melody、Chord、Key / Scaleの基礎
- 完了: Sampleの応用操作
- 完了: Beat、Tempo、Swing、Quantize、Pattern編集
- 完了: Performance Recording
- 完了: ProjectとSample Kitの保存・読み込み
- 完了: Wi-Fi設定とUpdate
- 完了: 外部コントローラーとInput Assign
- 完了: System設定とReset
- 完了: DJのように演奏する用途別ガイド
- 完了: トラブルシューティング
- 完了: 用語集と主な仕様

初心者向けWebマニュアルの本文ページは完成です。以後は実機写真・画面キャプチャーの差し替え、実機確認で見つかった表現修正、ファームウェア変更との同期を行います。

## ソースと公開先

- マニュアル原稿: `docs/docs/ja/sampler/`
- Sampler専用MkDocs設定: `docs/mkdocs-sampler.yml`
- 製品仕様の正本: `docs/development/sampler/product-spec.md`
- 将来の公開想定: `https://kantan-play.com/sampler/manual/`

公開サイトへ反映する際は、製品仕様、原稿、実機画面の3点を同じ変更単位で確認します。
