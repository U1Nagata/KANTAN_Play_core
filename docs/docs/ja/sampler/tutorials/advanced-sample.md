# Sampleの応用操作

このページでは、Sampleの鳴り方を変える設定と、Pad間のMove、Copy、Mix、Deleteを説明します。基本のStart / End、Volume、Pitchを先に試しておくと理解しやすくなります。

[Sample編集の基本](edit-sample.md){ .md-button }

## 鳴り方を変える

Sample Edit画面で設定します。

### Hold

P5 `Hold`は、Padを押している間だけ音を鳴らす設定です。

- OFF：Padを短く押してもSampleの終わりまで鳴る
- ON：Padを離した時点で音が止まる

長い声や楽器音を演奏するときに向いています。

### Repeat

P6 `Repeat`はSampleを繰り返します。下のダイヤルで方式を選びます。

| 設定 | 動作 |
|---|---|
| `None` | 繰り返さない |
| `Whole Sample` | StartからEndまでを連続して繰り返す |
| `8 / 4 / 2 / 1 / 0.5` | LoopのGridに合わせてSampleを繰り返す |

`Whole Sample`はBeatへ同期しません。Grid指定はBeatまたはRec Loopの速さに合わせて再トリガーします。

### Reverse

P7 `Reverse`をONにすると、Start / Endで指定した範囲を逆向きに再生します。声やシンバルを別の音のように変えたいときに使えます。

### Choke

P2 `Choke`をONにしたSample同士は、新しい音を鳴らすと前の音が止まります。開いたHiHatを閉じたHiHatで止める場合や、長いフレーズを重ねず切り替える場合に便利です。

<!-- IMAGE SAMPLE-ADV-01: Sample Edit画面。Hold、Repeat、Reverse、Chokeの状態が読める。 -->

## PadをMoveする

1. `SAMPLER + SOUND`で移動元Padを1回押し、音を確認します
2. 同じPadをもう一度押したまま保持します
3. 移動先の空Padを押します
4. 移動元Padを離します

空Padを選ぶとSampleが移動します。Rec済みの演奏がある場合は、対応するPad位置も移動先へ引き継がれます。

## Move後にCopyを残す

空PadへMoveした直後、移動元Padをまだ押している間に移動先Padをもう一度短く押すと、元の位置にも同じSampleをCopyできます。画面の`TAP TARGET AGAIN: COPY`案内が出ている間に操作します。

## 2つのSampleをMixする

1. `SAMPLER + SOUND`で元になるPadを選びます
2. 同じPadをもう一度押したまま保持します
3. すでにSampleが入っている別のPadを押します
4. `MIXING`表示が消えるまで待ちます

2つのSampleが1つの音として移動先Padへまとめられます。やり直しに備え、重要なKitは先に保存してください。

<!-- IMAGE SAMPLE-ADV-02: MOVE / MIX画面。SOURCE、EMPTY PAD: MOVE、FILLED PAD: MIXが読める。 -->

## SampleをDeleteする

方法は2つあります。

- Sample Edit画面でP4のゴミ箱を2回押す
- `SAMPLER + SOUND`で`Fn3`を押しながら対象Padを選び、確認後に削除する

Deleteすると、そのSampleのSynth、Repeat、Chokeなどの設定も消えます。保存済みの元WAV / MP3は削除されません。

!!! warning
    Move、Mix、DeleteはPadの内容を変更します。大切な状態は[Sample KitまたはProjectとして保存](../reference/save-and-restore.md)してから操作してください。
