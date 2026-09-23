/** @file
 *  SM8450 QUPv3 and GPI resource description.
 *
 *  Addresses and interrupt/SID values are taken from Linux's sm8450.dtsi:
 *  QUP wrappers at 0x9c0000/0xac0000/0x8c0000 and GPI DMA blocks at
 *  0x900000/0xa00000/0x800000.  The data is intentionally passive; MU's
 *  prebuilt QUP/GPI drivers remain the register owners.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/CrTargetQupLib.h>

#define QUP_SE_COUNT  8

STATIC CONST CR_TARGET_QUP_WRAPPER mSm8450Qup[] = {
  {
    .Id                 = 0,
    .Base               = 0x009c0000ULL,
    .Size               = 0x2000,
    .SerialEngineCount  = QUP_SE_COUNT,
    .SerialEngineBase   = {
      0x00980000ULL, 0x00984000ULL, 0x00988000ULL, 0x0098c000ULL,
      0x00990000ULL, 0x00994000ULL, 0x00998000ULL, 0x0099c000ULL,
    },
    .SerialEngineSize   = 0x4000,
    .IommuStreamId      = 0x5a3,
  },
  {
    .Id                 = 1,
    .Base               = 0x00ac0000ULL,
    .Size               = 0x6000,
    .SerialEngineCount  = 7,
    .SerialEngineBase   = {
      0x00a80000ULL, 0x00a84000ULL, 0x00a88000ULL, 0x00a8c000ULL,
      0x00a90000ULL, 0x00a94000ULL, 0x00a98000ULL, 0,
    },
    .SerialEngineSize   = 0x4000,
    .IommuStreamId      = 0x43,
  },
  {
    .Id                 = 2,
    .Base               = 0x008c0000ULL,
    .Size               = 0x2000,
    .SerialEngineCount  = 7,
    .SerialEngineBase   = {
      0x00880000ULL, 0x00884000ULL, 0x00888000ULL, 0x0088c000ULL,
      0x00890000ULL, 0x00894000ULL, 0x00898000ULL, 0,
    },
    .SerialEngineSize   = 0x4000,
    .IommuStreamId      = 0x483,
  },
};

STATIC CONST CR_TARGET_GPI_CONTROLLER mSm8450Gpi[] = {
  {
    .Id               = 0,
    .WrapperId        = 0,
    .Base             = 0x00900000ULL,
    .Size             = 0x60000,
    .ChannelMask      = 0x7e,
    .IommuStreamId    = 0x5b6,
    .InterruptCount   = 12,
    .Interrupts       = { 244, 245, 246, 247, 248, 249,
                           250, 251, 252, 253, 254, 255 },
  },
  {
    .Id               = 1,
    .WrapperId        = 1,
    .Base             = 0x00a00000ULL,
    .Size             = 0x60000,
    .ChannelMask      = 0x7e,
    .IommuStreamId    = 0x56,
    .InterruptCount   = 12,
    .Interrupts       = { 279, 280, 281, 282, 283, 284,
                           293, 294, 295, 296, 297, 298 },
  },
  {
    .Id               = 2,
    .WrapperId        = 2,
    .Base             = 0x00800000ULL,
    .Size             = 0x60000,
    .ChannelMask      = 0x7e,
    .IommuStreamId    = 0x496,
    .InterruptCount   = 12,
    .Interrupts       = { 588, 589, 590, 591, 592, 593,
                           594, 595, 596, 597, 598, 599 },
  },
};

STATIC CR_TARGET_QUP_CONTEXT mSm8450QupContext = {
  .Qup      = mSm8450Qup,
  .QupCount = sizeof (mSm8450Qup) / sizeof (mSm8450Qup[0]),
  .Gpi      = mSm8450Gpi,
  .GpiCount = sizeof (mSm8450Gpi) / sizeof (mSm8450Gpi[0]),
};

CR_TARGET_QUP_CONTEXT *
CrTargetGetQupContext (
  VOID
  )
{
  return &mSm8450QupContext;
}
