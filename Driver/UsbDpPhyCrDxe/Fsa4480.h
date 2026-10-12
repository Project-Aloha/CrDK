/** Native standard-mode SPB transport for the shared FSA4480 policy.
    SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Protocol/EFII2cCrProtocol.h>
typedef struct { EFI_I2C_CR_PROTOCOL *Bus; } CR_FSA4480;
EFI_STATUS CrFsaRead(CR_FSA4480 *Fsa, UINT8 Reg, UINT8 *Value);
EFI_STATUS CrFsaWrite(CR_FSA4480 *Fsa, UINT8 Reg, UINT8 Value, BOOLEAN *Dispatched);
