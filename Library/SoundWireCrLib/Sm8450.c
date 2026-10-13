/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * Android HDK DT252b88fe + OnePlus audio877203a7; see README/LICENSE. */
#include <Library/SoundWireCrLib.h>

CONST CR_SW_TARGET CrSoundWireSm8450Rx = {
  0x03210000, 0x2000, 155, 0, 2, 2, 1, 20,
  {{0x000d01170224ULL, 0, 0, 0, 43}, {0}} /* WCD register map uses TX slave. */
};
CONST CR_SW_TARGET CrSoundWireSm8450Tx = {
  0x033b0000, 0x2000, 496, 520, 3, 5, 1, 20,
  {{0x000d01170223ULL, 0x3401, 0x34b0, 0, 43}, {0}}
};
CONST CR_SW_TARGET CrSoundWireSm8450Wsa = {
  0x03250000, 0x2000, 170, 0, 1, 2, 2, 5000,
  {{0x000202170221ULL, 0x3401, 0x3580, 0x3410, 1},
   {0x000202170222ULL, 0x3401, 0x3580, 0x3410, 89}}
};
