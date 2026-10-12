/** SM8550 PDC routing facts from public Linux sm8550.dtsi.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetPdcLib.h>

STATIC PdcDeviceContext mPdc = {
  .BaseAddress = 0x0b220000,
  .Size = 0x10000,
  .SPIConfigBase = 0x174000f0,
  .SPIConfigSize = 0x64,
  .PdcRangeCount = 5,
  .PdcRegion = {{0, 480, 94}, {94, 609, 31}, {125, 63, 1},
                {126, 716, 12}, {138, 251, 5}},
};

PdcDeviceContext *GetPdcDevContext (VOID) { return &mPdc; }
