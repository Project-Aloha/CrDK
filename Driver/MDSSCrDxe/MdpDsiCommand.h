/** @file Linux MSM 6G command DMA transport. SPDX-License-Identifier: GPL-2.0-only */
#ifndef MDP_DSI_COMMAND_H_
#define MDP_DSI_COMMAND_H_
#include <Uefi.h>
/** Idle-video only, write packets only. Controller must match the single host selected by the resource provider.
    Calls must be serialized; a retained transfer pins its original host.
    EFI_NOT_READY means retained DMA/mapping: keep host clocks/PHY/resources on. */
EFI_STATUS MdpDsiCommandTransfer (UINT32 Controller, UINT8 VirtualChannel,
  UINT8 DataType, CONST UINT8 *Payload, UINTN Length, BOOLEAN LowPower);
/** Retry fencing/releasing a retained transfer before resource shutdown. */
EFI_STATUS MdpDsiCommandQuiesce (UINT32 Controller);
/** Close command admission and fence a retained transfer without freeing or
    revoking its DMA storage during the SMMU pre-exit observer. */
EFI_STATUS MdpDsiCommandFreeze (VOID);
#endif
