/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/spmi.h>

/* V1 */
CR_STATUS PmicArbGetCoreResourceV1(SpmiDeviceContext *Ctx);
CR_STATUS PmicArbInitApidV1(SpmiDeviceContext *Ctx, UINT32 Index);
CR_STATUS PmicArbPpidToApidV1(SpmiDeviceContext *Ctx, UINT16 Ppid,
                              UINT16 *Apid);
UINT32 PmicArbGetChannelOffsetV1(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                                 SPMI_ARB_CHANNEL_TYPE ChannelType);
UINT32 PmicArbFormatCmdV1(UINT8 Opc, UINT8 Sid, UINT16 Addr, UINT8 Bc);
CR_STATUS PmicArbCheckChannelStatusV1(SpmiDeviceContext *Ctx, UINT32 Status,
                                      UINT8 Sid, UINT16 Addr, UINT32 Offset);
UINTN PmicArbGetOwnerAccStatusV1(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);
UINTN PmicArbGetAccEnableV1(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqStatusV1(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqClearV1(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetApidOwnerV1(SpmiDeviceContext *Ctx, UINT16 Apid);
UINT32 PmicArbGetApidMapOffsetV1(UINT16 Apid);

/* V2 (also used by V3) */
CR_STATUS PmicArbGetCoreResourceV2(SpmiDeviceContext *Ctx);
CR_STATUS PmicArbInitApidV2(SpmiDeviceContext *Ctx, UINT32 Index);
CR_STATUS PmicArbPpidToApidV2(SpmiDeviceContext *Ctx, UINT16 Ppid,
                              UINT16 *Apid);
UINT32 PmicArbGetChannelOffsetV2(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                                 SPMI_ARB_CHANNEL_TYPE ChannelType);
UINT32 PmicArbFormatCmdV2(UINT8 Opc, UINT8 Sid, UINT16 Addr, UINT8 Bc);
UINTN PmicArbGetOwnerAccStatusV2(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);
UINTN PmicArbGetAccEnableV2(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqStatusV2(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqClearV2(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetApidOwnerV2(SpmiDeviceContext *Ctx, UINT16 Apid);
UINT32 PmicArbGetApidMapOffsetV2(UINT16 Apid);

/* V3 */
UINTN PmicArbGetOwnerAccStatusV3(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);

/* V5 */
CR_STATUS PmicArbInitApidV5(SpmiDeviceContext *Ctx, UINT32 Index);
CR_STATUS PmicArbPpidToApidV5(SpmiDeviceContext *Ctx, UINT16 Ppid,
                              UINT16 *Apid);
UINT32 PmicArbGetChannelOffsetV5(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                                 SPMI_ARB_CHANNEL_TYPE ChannelType);
UINTN PmicArbGetOwnerAccStatusV5(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);
UINTN PmicArbGetAccEnableV5(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqStatusV5(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqClearV5(SpmiDeviceContext *Ctx, UINT16 Apid);
UINT32 PmicArbGetApidMapOffsetV5(UINT16 Apid);
CR_STATUS PmicArbReadApidMapV5(SpmiDeviceContext *Ctx);

/* V7 */
CR_STATUS PmicArbGetCoreResourceV7(SpmiDeviceContext *Ctx);
CR_STATUS PmicArbInitApidV7(SpmiDeviceContext *Ctx, UINT32 Index);
UINT32 PmicArbGetChannelOffsetV7(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                                 SPMI_ARB_CHANNEL_TYPE ChannelType);
UINTN PmicArbGetOwnerAccStatusV7(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);
UINTN PmicArbGetAccEnableV7(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqStatusV7(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqClearV7(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetApidOwnerV7(SpmiDeviceContext *Ctx, UINT16 Apid);
UINT32 PmicArbGetApidMapOffsetV7(UINT16 Apid);

/* V8 */
CR_STATUS PmicArbGetCoreResourceV8(SpmiDeviceContext *Ctx);
CR_STATUS PmicArbInitApidV8(SpmiDeviceContext *Ctx, UINT32 Index);
UINT32 PmicArbGetChannelOffsetV8(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                                 SPMI_ARB_CHANNEL_TYPE ChannelType);
UINTN PmicArbGetOwnerAccStatusV8(SpmiDeviceContext *Ctx, UINT8 Ee,
                                 UINT16 Index);
UINTN PmicArbGetAccEnableV8(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqStatusV8(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetIrqClearV8(SpmiDeviceContext *Ctx, UINT16 Apid);
UINTN PmicArbGetApidOwnerV8(SpmiDeviceContext *Ctx, UINT16 Apid);
UINT32 PmicArbGetApidMapOffsetV8(UINT16 Apid);
CR_STATUS PmicArbCheckChannelStatusV8P5(SpmiDeviceContext *Ctx, UINT32 Status,
                                        UINT8 Sid, UINT16 Addr, UINT32 Offset);

/* Shared helpers */
CR_STATUS PmicArbInitApidMinMax(SpmiDeviceContext *Ctx);
CR_STATUS PmicArbReadApidMapCommon(SpmiDeviceContext *Ctx, UINTN PpidBase,
                                   UINT64 PpidMsk, UINT8 PpidSft,
                                   UINT64 IrqOwnerMsk);
CR_STATUS PmicArbInitApidCommonV7V8(SpmiDeviceContext *Ctx, UINT32 Index,
                                    UINT32 MaxBuses, UINT64 PeriphMsk);
UINT16 PmicArbFindApid(SpmiDeviceContext *Ctx, UINT16 Ppid);
