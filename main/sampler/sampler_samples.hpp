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

SAMPLER_IMPORT_WAV("kick.wav",                                wav_kick);
SAMPLER_IMPORT_WAV("Snare.wav",                               wav_snare);
SAMPLER_IMPORT_WAV("clap.wav",                                wav_clap);
SAMPLER_IMPORT_WAV("Hat.wav",                                 wav_hihat);
SAMPLER_IMPORT_WAV("chin.wav",                                wav_chin);
SAMPLER_IMPORT_WAV("Cowbell.wav",                             wav_cowbell);
SAMPLER_IMPORT_WAV("Piko.wav",                                wav_piko);
SAMPLER_IMPORT_WAV("Tom.wav",                                 wav_tom);
SAMPLER_IMPORT_WAV("Air-Horn.wav",                            wav_air_horn);
SAMPLER_IMPORT_WAV("Cat.wav",                                 wav_cat);
SAMPLER_IMPORT_WAV("Gunshot.wav",                             wav_gunshot);
SAMPLER_IMPORT_WAV("Hat_Close.wav",                           wav_hat_close);
SAMPLER_IMPORT_WAV("Jump.wav",                                wav_jump);
SAMPLER_IMPORT_WAV("Laser.wav",                               wav_laser);
SAMPLER_IMPORT_WAV("Sheep.wav",                               wav_sheep);
SAMPLER_IMPORT_WAV("BGM_House.wav",                           wav_bgm_house);
SAMPLER_IMPORT_WAV("BGM_Break.wav",                           wav_bgm_break);
SAMPLER_IMPORT_WAV("BGM_DnB.wav",                             wav_bgm_dnb);
SAMPLER_IMPORT_WAV("Acoustic_Drums/01_Acoustic_Tom_Low.wav",  wav_acoustic_tom_low);
SAMPLER_IMPORT_WAV("Acoustic_Drums/02_Acoustic_Tom_Mid.wav",  wav_acoustic_tom_mid);
SAMPLER_IMPORT_WAV("Acoustic_Drums/03_Acoustic_Tom_High.wav", wav_acoustic_tom_high);
SAMPLER_IMPORT_WAV("Acoustic_Drums/04_Acoustic_Rim.wav",      wav_acoustic_rim);
SAMPLER_IMPORT_WAV("Acoustic_Drums/05_Acoustic_Crash.wav",    wav_acoustic_crash);
SAMPLER_IMPORT_WAV("Acoustic_Drums/06_Acoustic_Ride.wav",     wav_acoustic_ride);
SAMPLER_IMPORT_WAV("Acoustic_Drums/07_Acoustic_Shaker.wav",   wav_acoustic_shaker);
SAMPLER_IMPORT_WAV("Chiptune_Drums/01_Chip_Kick.wav",         wav_chip_kick);
SAMPLER_IMPORT_WAV("Chiptune_Drums/02_Chip_Snare.wav",        wav_chip_snare);
SAMPLER_IMPORT_WAV("Chiptune_Drums/03_Chip_Clap.wav",         wav_chip_clap);
SAMPLER_IMPORT_WAV("Chiptune_Drums/04_Chip_Hat_Closed.wav",   wav_chip_hat_closed);
SAMPLER_IMPORT_WAV("Chiptune_Drums/05_Chip_Hat_Open.wav",     wav_chip_hat_open);
SAMPLER_IMPORT_WAV("Chiptune_Drums/06_Chip_Tom.wav",          wav_chip_tom);
SAMPLER_IMPORT_WAV("Chiptune_Drums/07_Chip_Rim.wav",          wav_chip_rim);
SAMPLER_IMPORT_WAV("Chiptune_Drums/08_Chip_Cowbell.wav",      wav_chip_cowbell);

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
  { "COWBELL", wav_cowbell,       sizeof_wav_cowbell       },
  { "CHIN",    wav_chin,          sizeof_wav_chin          },
  { "TOM",     wav_tom,           sizeof_wav_tom           },
  { "AIR HORN", wav_air_horn,      sizeof_wav_air_horn      },
  { "CAT",      wav_cat,           sizeof_wav_cat           },
  { "GUNSHOT",  wav_gunshot,       sizeof_wav_gunshot       },
  { "HAT CLOSE",wav_hat_close,     sizeof_wav_hat_close     },
  { "JUMP",     wav_jump,          sizeof_wav_jump          },
  { "LASER",    wav_laser,         sizeof_wav_laser         },
  { "SHEEP",    wav_sheep,         sizeof_wav_sheep         },
  { "TOM LOW",  wav_acoustic_tom_low,  sizeof_wav_acoustic_tom_low  },
  { "TOM MID",  wav_acoustic_tom_mid,  sizeof_wav_acoustic_tom_mid  },
  { "TOM HIGH", wav_acoustic_tom_high, sizeof_wav_acoustic_tom_high },
  { "RIM",      wav_acoustic_rim,      sizeof_wav_acoustic_rim      },
  { "CRASH",    wav_acoustic_crash,    sizeof_wav_acoustic_crash    },
  { "RIDE",     wav_acoustic_ride,     sizeof_wav_acoustic_ride     },
  { "SHAKER",   wav_acoustic_shaker,   sizeof_wav_acoustic_shaker   },
  { "CHIP KICK",   wav_chip_kick,       sizeof_wav_chip_kick       },
  { "CHIP SNARE",  wav_chip_snare,      sizeof_wav_chip_snare      },
  { "CHIP CLAP",   wav_chip_clap,       sizeof_wav_chip_clap       },
  { "CHIP HAT C",  wav_chip_hat_closed, sizeof_wav_chip_hat_closed },
  { "CHIP HAT O",  wav_chip_hat_open,   sizeof_wav_chip_hat_open   },
  { "CHIP TOM",    wav_chip_tom,        sizeof_wav_chip_tom        },
  { "CHIP RIM",    wav_chip_rim,        sizeof_wav_chip_rim        },
  { "CHIP COWBELL",wav_chip_cowbell,    sizeof_wav_chip_cowbell    },
};
static constexpr const size_t builtin_sample_count = sizeof(builtin_samples) / sizeof(builtin_samples[0]);
// The factory kit intentionally leaves the top row empty. Additional built-in
// sounds remain available from Import Sample without changing this layout.
static constexpr const size_t builtin_default_sample_count = 8;

struct background_source_t {
  const char* file;
  sample_source_t source;
};

static const background_source_t builtin_background_loops[] = {
  { "BGM_House.wav",   { "BGM HOUSE",   wav_bgm_house,   sizeof_wav_bgm_house   } },
  { "BGM_Break.wav",   { "BGM BREAK",   wav_bgm_break,   sizeof_wav_bgm_break   } },
  { "BGM_DnB.wav",     { "BGM DNB",     wav_bgm_dnb,     sizeof_wav_bgm_dnb     } },
};
static constexpr const size_t builtin_background_loop_count =
  sizeof(builtin_background_loops) / sizeof(builtin_background_loops[0]);

//-------------------------------------------------------------------------
} // namespace sampler_ns

#endif
