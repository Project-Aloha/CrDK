/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "pmic_arb_v5.tmh"
#endif

CR_STATUS
PmicArbReadApidMapV5(SpmiDeviceContext *Ctx) {
  return PmicArbReadApidMapCommon(
      Ctx, Ctx->PmicArb.CoreAddr, SPMI_PMIC_ARB_PPID_MASK,
      SPMI_PMIC_ARB_PPID_SHIFT_V5, SPMI_PMIC_ARB_CHAN_IS_IRQ_OWNER_MASK);
}

CR_STATUS
PmicArbInitApidV5(SpmiDeviceContext *Ctx, UINT32 Index) {
  CR_STATUS Status;

  if (Ctx == NULL || Index != 0) {
    log_err("SPMI: v5 only supports a single bus");
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.BaseApid = 0;
  Ctx->Bus.ApidCount =
      SpmiReadReg32(Ctx, SPMI_MEMORY_REGION_TYPE_CORE, SPMI_PMIC_ARB_FEATURES) &
      SPMI_PMIC_ARB_FEATURES_PERIPH_MASK;

  if (Ctx->Bus.BaseApid + Ctx->Bus.ApidCount > Ctx->PmicArb.MaxPeriphs) {
    log_err("SPMI: unsupported APID count %u detected",
            Ctx->Bus.BaseApid + Ctx->Bus.ApidCount);
    return CR_INVALID_PARAMETER;
  }

  Status = PmicArbInitApidMinMax(Ctx);
  if (CR_ERROR(Status)) {
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

CR_STATUS
PmicArbPpidToApidV5(SpmiDeviceContext *Ctx, UINT16 Ppid, UINT16 *Apid) {
  UINT16 ApidValid;

  if (Ctx == NULL || Apid == NULL) {
    return CR_INVALID_PARAMETER;
  }

  if (Ppid >= SPMI_PMIC_ARB_MAX_PPID) {
    return CR_INVALID_PARAMETER;
  }

  ApidValid = Ctx->Bus.PpidToApid[Ppid];
  if ((ApidValid & (UINT16)SPMI_PMIC_ARB_APID_VALID) == 0) {
    return CR_NOT_FOUND;
  }

  *Apid = (UINT16)(ApidValid & ~SPMI_PMIC_ARB_APID_VALID);
  return CR_SUCCESS;
}

UINT32
PmicArbGetChannelOffsetV5(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                          SPMI_ARB_CHANNEL_TYPE ChannelType) {
  UINT16 Ppid = (UINT16)((Sid << 8) | (Addr >> 8));
  UINT16 Apid;
  UINT32 Offset = 0;

  if (CR_ERROR(PmicArbPpidToApidV5(Ctx, Ppid, &Apid))) {
    return (UINT32)-1;
  }

  switch (ChannelType) {
  case SPMI_ARB_CHANNEL_OBS:
    Offset = 0x10000 * Ctx->ActiveEE + 0x80 * Apid;
    break;
  case SPMI_ARB_CHANNEL_RW:
    if (Ctx->Bus.ApidData[Apid].WriteEe != Ctx->ActiveEE) {
      log_err("SPMI: disallowed write to sid=%u addr=0x%04X (ee %u owner %u)",
              Sid, Addr, Ctx->ActiveEE, Ctx->Bus.ApidData[Apid].WriteEe);
      return (UINT32)-1;
    }
    Offset = 0x10000 * Apid;
    break;
  default:
    return (UINT32)-1;
  }

  return Offset;
}

UINTN
PmicArbGetOwnerAccStatusV5(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index) {
  return Ctx->Bus.InterruptAddr + 0x10000 * Ee + 0x4 * Index;
}

UINTN
PmicArbGetAccEnableV5(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x100 + 0x10000 * Apid;
}

UINTN
PmicArbGetIrqStatusV5(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x104 + 0x10000 * Apid;
}

UINTN
PmicArbGetIrqClearV5(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x108 + 0x10000 * Apid;
}

UINT32
PmicArbGetApidMapOffsetV5(UINT16 Apid) { return 0x900 + 0x4 * Apid; }
