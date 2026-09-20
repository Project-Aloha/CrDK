/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "pmic_arb_v7.tmh"
#endif

CR_STATUS
PmicArbGetCoreResourceV7(SpmiDeviceContext *Ctx) {
  CR_STATUS Status;

  Status = PmicArbGetCoreResourceV2(Ctx);
  if (CR_ERROR(Status)) {
    return Status;
  }

  Ctx->PmicArb.MaxPeriphs = SPMI_PMIC_ARB_MAX_PERIPHS_V7;
  Ctx->PmicArb.BusAvailable = 2;
  return CR_SUCCESS;
}

CR_STATUS
PmicArbInitApidV7(SpmiDeviceContext *Ctx, UINT32 Index) {
  CR_STATUS Status;

  Status = PmicArbInitApidCommonV7V8(Ctx, Index, 2,
                                     SPMI_PMIC_ARB_FEATURES_PERIPH_MASK);
  if (CR_ERROR(Status)) {
    log_err("SPMI: failed to init APID for bus %u", Index);
    return Status;
  }

  Status = PmicArbReadApidMapV5(Ctx);
  if (CR_ERROR(Status)) {
    log_err("SPMI: could not read APID->PPID mapping table, Status=0x%X",
            Status);
    return Status;
  }

  return CR_SUCCESS;
}

UINT32
PmicArbGetChannelOffsetV7(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                          SPMI_ARB_CHANNEL_TYPE ChannelType) {
  UINT16 Ppid = (UINT16)((Sid << 8) | (Addr >> 8));
  UINT16 Apid;
  UINT32 Offset = 0;

  if (CR_ERROR(PmicArbPpidToApidV5(Ctx, Ppid, &Apid))) {
    return (UINT32)-1;
  }

  switch (ChannelType) {
  case SPMI_ARB_CHANNEL_OBS:
    Offset = 0x8000 * Ctx->ActiveEE + 0x20 * Apid;
    break;
  case SPMI_ARB_CHANNEL_RW:
    if (Ctx->Bus.ApidData[Apid].WriteEe != Ctx->ActiveEE) {
      log_err("SPMI: disallowed write to sid=%u addr=0x%04X (ee %u owner %u)",
              Sid, Addr, Ctx->ActiveEE, Ctx->Bus.ApidData[Apid].WriteEe);
      return (UINT32)-1;
    }
    Offset = 0x1000 * Apid;
    break;
  default:
    return (UINT32)-1;
  }

  return Offset;
}

UINTN
PmicArbGetOwnerAccStatusV7(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index) {
  return Ctx->Bus.InterruptAddr + 0x1000 * Ee + 0x4 * Index;
}

UINTN
PmicArbGetAccEnableV7(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x100 + 0x1000 * Apid;
}

UINTN
PmicArbGetIrqStatusV7(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x104 + 0x1000 * Apid;
}

UINTN
PmicArbGetIrqClearV7(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x108 + 0x1000 * Apid;
}

UINTN
PmicArbGetApidOwnerV7(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.ConfigAddr + 0x4 * (Apid - Ctx->Bus.BaseApid);
}

UINT32
PmicArbGetApidMapOffsetV7(UINT16 Apid) { return 0x2000 + 0x4 * Apid; }
