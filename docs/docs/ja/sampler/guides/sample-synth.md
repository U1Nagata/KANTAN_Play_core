# Sampleから楽器を作る

Sample Synthを使うと、1つのSampleを音程に合わせて鳴らし、Bass、Melody、Chordの音源として演奏できます。声の「あー」、単音の楽器、持続するシンセ音などが向いています。

打楽器、複数の音程が同時に鳴る素材、音程が大きく動く素材は、最初の練習には向きません。

## 仕組みを知る

Sample Synthでは、Sampleの最初の部分を鳴らしたあと、安定した波形の一部を`Sustain Loop`として繰り返します。Padを離すか音の終わりへ進むと、`Release`で自然に音を閉じます。

ここでいうSustain Loopは、Recで作る演奏のLoopとは別の機能です。

## 1. Sampleを用意して整える

1. 単音のSampleをSampler Padへ追加します
2. `SAMPLER`の`SOUND`モードで、そのPadを2回短く押してEditへ入ります
3. Start / End、Volume、Pitchを必要に応じて調整します
4. P8 `Synth`を押します

[Sampleを編集する](../tutorials/edit-sample.md){ .md-button }

<!-- IMAGE SYNTH-01: Sample Edit画面からP8 Synthを選べることが分かる状態。 -->

## 2. Sustainを試す

Sample Synth画面では、最初にP9 `Sustain`を選び、下のダイヤルでモードを変えます。

| 設定 | 動作 |
|---|---|
| `OFF` | Sustain Loopを使わず、Sampleを通常再生する |
| `AUTO` | 安定して繰り返せる区間を本体が探す |
| `ON` | P10 `In`とP11 `Out`で指定した区間を使う |

初心者はまず`AUTO`を選びます。`NO SUSTAIN`と表示された場合は、そのSampleには安定して繰り返せる区間が見つかっていません。別のSampleを試すか、`ON`で手動調整します。

`Fn1`はPreviewです。Sample Synth画面では、`Fn1`を押している間Sustainが続き、離すとReleaseへ移ります。

<!-- IMAGE SYNTH-02: Sample Synth画面。Sustain、In、Out、Relと波形上のSustain区間が読める状態。 -->

## 3. In / OutとReleaseを調整する

- P10 `In`：Sustain Loopの開始位置。下のダイヤルで動かす
- P11 `Out`：Sustain Loopの終了位置。下のダイヤルで動かす
- P12 `Rel`：Padを離したあとの音の消え方を選ぶ

不自然な周期音やクリックが聞こえる場合は、InまたはOutを少しずつ動かします。音を素早く止めたい場合は短いRelease、余韻を残したい場合は長いReleaseを選びます。

!!! note
    SustainとReverseは同時に使えません。Sustainを有効にするとReverseはOFFになります。`Whole Sample`のRepeatとも同時には使えません。

## 4. Bass、Melody、Chordへ割り当てる

Sample Synth画面の上段で、使いたいパートを選びます。

| Pad | 割り当て先 |
|---|---|
| P1 `Mel` | Melody |
| P2 `Chord` | Chord |
| P3 `Bass` | Bass |

誤操作を防ぐため、同じPadをもう一度押すと割り当てが確定します。割り当て済みのPadを同じ手順で押すと解除できます。

## 5. 音階で演奏する

1. `Fn2 OK`で編集を確定します
2. 下のダイヤルを回し、割り当てたBass、Melody、またはChordへ移動します
3. PLAYモードで12個のPadを鳴らします

本体はSampleの基準音程`Pad Base Note`を自動推定します。音程が合わない場合は、各パートのメニューで`Sound Source` → `Pad` → `Pad Base Note`を開き、元Sampleの音名を手動で選びます。

## 向いているSample

- 1音だけを長めに鳴らした声や楽器音
- 音程が安定している音
- ノイズや余韻が少なく、Start / Endを決めやすい音

Chopで作ったSliceはリズム位置を保つため、Sample Synthへは割り当てられません。
