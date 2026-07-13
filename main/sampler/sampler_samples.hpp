// SPDX-License-Identifier: MIT
// Copyright (c) 2026 InstaChord Corp.

#ifndef KANTAN_SAMPLER_SAMPLES_HPP
#define KANTAN_SAMPLER_SAMPLES_HPP

// 開発検証用の組み込みサンプル音源 (docs/Sample_Sound/ の WAV をバイナリ埋め込み)
// 将来的には TFカードからの読み込み・ユーザー録音に置き換える。
// アセンブラシンボルを定義するため、このヘッダは1つの翻訳単位からのみ include すること。
// (埋め込み手法は main/file_manage.cpp の IMPORT_FILE と同一)

#include <stdint.h>
#include <stddef.h>

namespace sampler_ns {
//-------------------------------------------------------------------------

#if defined (__APPLE__) && defined (__MACH__) && defined (__arm64__)

#define SAMPLER_IMPORT_WAV(filename, symbol) \
extern "C" const uint8_t symbol[], sizeof_##symbol[]; \
asm (\
  ".section __DATA,__data \n"\
  ".balign 4\n_"\
  #symbol ":\n"\
  ".incbin \"docs/Sample_Sound/" filename "\"\n"\
  ".global _sizeof_" #symbol "\n"\
  ".set _sizeof_" #symbol ", . - _" #symbol "\n"\
  ".global _" #symbol "\n"\
  ".balign 4\n"\
  ".text \n")

#else

#define SAMPLER_IMPORT_WAV(filename, symbol) \
extern "C" const uint8_t symbol[], sizeof_##symbol[]; \
asm (\
  ".section .rodata\n"\
  ".balign 4\n"\
  ".global " #symbol "\n"\
  #symbol ":\n"\
  ".incbin \"docs/Sample_Sound/" filename "\"\n"\
  ".global sizeof_" #symbol "\n"\
  ".set sizeof_" #symbol ", . - " #symbol "\n"\
  ".balign 4\n"\
  ".section \".text\"\n")

#endif

SAMPLER_IMPORT_WAV("kick-one-shot.wav",                       wav_kick);
SAMPLER_IMPORT_WAV("band-snare-one-shot.wav",                 wav_snare);
SAMPLER_IMPORT_WAV("clap-snare-one-shot.wav",                 wav_clap);
SAMPLER_IMPORT_WAV("hi-hat-one-shot_140bpm.wav",              wav_hihat);
SAMPLER_IMPORT_WAV("chin.wav",                                wav_chin);
SAMPLER_IMPORT_WAV("cowbell-808.wav",                         wav_cowbell);
SAMPLER_IMPORT_WAV("piko.wav",                                wav_piko);
SAMPLER_IMPORT_WAV("tom-lofi.wav",                            wav_tom_lofi);
SAMPLER_IMPORT_WAV("BGM_FA.wav",                               wav_bgm_fa);

#undef SAMPLER_IMPORT_WAV

struct sample_source_t {
  const char* name;        // Pad表示用の短い名前
  const uint8_t* data;     // WAVファイル先頭
  const uint8_t* size_sym; // サイズシンボル (アドレス値がサイズ)
  size_t size(void) const { return (size_t)size_sym; }
};

static const sample_source_t builtin_samples[] = {
  { "KICK",    wav_kick,          sizeof_wav_kick          },
  { "SNARE",   wav_snare,         sizeof_wav_snare         },
  { "CLAP",    wav_clap,          sizeof_wav_clap          },
  { "HAT",     wav_hihat,         sizeof_wav_hihat         },
  { "PIKO",    wav_piko,          sizeof_wav_piko          },
  { "COW",     wav_cowbell,       sizeof_wav_cowbell       },
  { "CHIN",    wav_chin,          sizeof_wav_chin          },
  { "TOML",    wav_tom_lofi,      sizeof_wav_tom_lofi      },
};
static constexpr const size_t builtin_sample_count = sizeof(builtin_samples) / sizeof(builtin_samples[0]);

static const sample_source_t builtin_background_loop = {
  "BGM_FA", wav_bgm_fa, sizeof_wav_bgm_fa
};

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
