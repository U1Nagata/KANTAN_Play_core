#!/usr/bin/env node
// SPDX-License-Identifier: MIT
// Generates the original 8-bit-style WAV one-shots in docs/Sample_Sound/Chiptune_Drums.
// No third-party samples are used; run with: node tools/generate_chiptune_drums.mjs

import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const rate = 48000;
const output = resolve('docs/Sample_Sound/Chiptune_Drums');
mkdirSync(output, { recursive: true });

let randomState = 0x8c3d19e7;
function noise() {
  randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0;
  return ((randomState >>> 8) / 0x7fffff) - 1;
}
function crush(value) {
  // 8-bit amplitude and 11.025 kHz sample-and-hold, encoded as 16-bit PCM.
  return Math.round(Math.max(-1, Math.min(1, value)) * 127) / 127;
}
function square(phase) { return Math.sin(phase) >= 0 ? 1 : -1; }
function wav(name, seconds, generator) {
  const frames = Math.round(seconds * rate);
  const pcm = Buffer.alloc(frames * 2);
  let held = 0;
  for (let i = 0; i < frames; i++) {
    if (i % 4 === 0) held = crush(generator(i / rate, i / frames));
    pcm.writeInt16LE(Math.round(held * 26000), i * 2);
  }
  const header = Buffer.alloc(44);
  header.write('RIFF', 0); header.writeUInt32LE(36 + pcm.length, 4); header.write('WAVE', 8);
  header.write('fmt ', 12); header.writeUInt32LE(16, 16); header.writeUInt16LE(1, 20);
  header.writeUInt16LE(1, 22); header.writeUInt32LE(rate, 24); header.writeUInt32LE(rate * 2, 28);
  header.writeUInt16LE(2, 32); header.writeUInt16LE(16, 34); header.write('data', 36);
  header.writeUInt32LE(pcm.length, 40);
  writeFileSync(resolve(output, name), Buffer.concat([header, pcm]));
}

wav('01_Chip_Kick.wav', 0.42, (t) => {
  const pitch = 54 * Math.exp(-t * 16) + 38;
  const body = Math.sin(2 * Math.PI * pitch * t) * Math.exp(-t * 10);
  const click = noise() * Math.exp(-t * 80) * 0.14;
  return body * 0.95 + click;
});
wav('02_Chip_Snare.wav', 0.30, (t) => {
  const envelope = Math.exp(-t * 17);
  return (noise() * 0.78 + square(2 * Math.PI * 178 * t) * 0.22) * envelope;
});
wav('03_Chip_Clap.wav', 0.28, (t) => {
  const bursts = [0, 0.034, 0.070].reduce((sum, at) => sum + (t >= at ? Math.exp(-(t - at) * 72) : 0), 0);
  return noise() * bursts * 0.48;
});
wav('04_Chip_Hat_Closed.wav', 0.11, (t) => noise() * Math.exp(-t * 52) * 0.72);
wav('05_Chip_Hat_Open.wav', 0.46, (t) => noise() * Math.exp(-t * 9) * 0.58);
wav('06_Chip_Tom.wav', 0.38, (t) => square(2 * Math.PI * (105 * Math.exp(-t * 4) + 64) * t) * Math.exp(-t * 9) * 0.76);
wav('07_Chip_Rim.wav', 0.10, (t) => (square(2 * Math.PI * 920 * t) * 0.5 + noise() * 0.26) * Math.exp(-t * 48));
wav('08_Chip_Cowbell.wav', 0.34, (t) => (square(2 * Math.PI * 540 * t) * 0.43 + square(2 * Math.PI * 800 * t) * 0.36) * Math.exp(-t * 8));
