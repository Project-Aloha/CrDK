/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "pmic_arb_v2.tmh"
#endif

CR_STATUS
PmicArbGetCoreResourceV2(SpmiDeviceContext *Ctx) {
  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }

  Ctx->PmicArb.CoreAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CORE].BaseAddress;
  Ctx->PmicArb.WriteAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].BaseAddress;
  Ctx->PmicArb.ReadAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].BaseAddress;
  Ctx->PmicArb.CoreSize =
      (UINT32)Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CORE].Size;
  Ctx->PmicArb.MaxPeriphs = SPMI_PMIC_ARB_MAX_PERIPHS;
  Ctx->PmicArb.BusAvailable = 1;

  Ctx->Bus.InterruptAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_INTERRUPT].BaseAddress;
  Ctx->Bus.ConfigAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].BaseAddress;

  if (Ctx->PmicArb.CoreAddr == 0 || Ctx->PmicArb.WriteAddr == 0 ||
      Ctx->PmicArb.ReadAddr == 0 || Ctx->Bus.InterruptAddr == 0 ||
      Ctx->Bus.ConfigAddr == 0) {
    log_err("SPMI: arbiter v2 requires core/chnls/obsrvr/intr/cnfg regions");
    return CR_NOT_FOUND;
  }

  return CR_SUCCESS;
}

CR_STATUS
PmicArbInitApidV2(SpmiDeviceContext *Ctx, UINT32 Index) {
  if (Ctx == NULL || Index != 0) {
    log_err("SPMI: v2 only supports a single bus");
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.BaseApid = 0;
  Ctx->Bus.ApidCount = Ctx->PmicArb.MaxPeriphs;
  Ctx->Bus.ApidMapValid = FALSE;
  Ctx->Bus.LastApid = 0;
  return PmicArbInitApidMinMax(Ctx);
}

UINT16
PmicArbFindApid(SpmiDeviceContext *Ctx, UINT16 Ppid) {
  SpmiPmicArbBus *Bus = &Ctx->Bus;
  UINT32 Offset;
  UINT32 RegVal;
  UINT16 Apid;
  UINT16 Id;

  for (Apid = Bus->LastApid; Apid < Ctx->PmicArb.MaxPeriphs; Apid++) {
    Offset = PmicArbGetApidMapOffsetV2(Apid);
    if (Offset >= Ctx->PmicArb.CoreSize) {
      break;
    }

    RegVal = SpmiIoRead32(Ctx, PmicArbGetApidOwnerV2(Ctx, Apid));
    Bus->ApidData[Apid].IrqEe = (UINT8)SPMI_OWNERSHIP_PERIPH2OWNER(RegVal);
    Bus->ApidData[Apid].WriteEe = Bus->ApidData[Apid].IrqEe;

    RegVal = SpmiIoRead32(Ctx, Ctx->PmicArb.CoreAddr + Offset);
    if (RegVal == 0) {
      continue;
    }

    Id = (UINT16)((RegVal >> 8) & SPMI_PMIC_ARB_PPID_MASK);
    Bus->PpidToApid[Id] = Apid | (UINT16)SPMI_PMIC_ARB_APID_VALID;
    Bus->ApidData[Apid].Ppid = Id;
    if (Id == Ppid) {
      return Apid | (UINT16)SPMI_PMIC_ARB_APID_VALID;
    }
  }

  Bus->LastApid = Apid;
  return 0;
}

CR_STATUS
PmicArbPpidToApidV2(SpmiDeviceContext *Ctx, UINT16 Ppid, UINT16 *Apid) {
  UINT16 ApidValid;

  if (Ctx == NULL || Apid == NULL) {
    return CR_INVALID_PARAMETER;
  }

  if (Ppid >= SPMI_PMIC_ARB_MAX_PPID) {
    return CR_INVALID_PARAMETER;
  }
  ApidValid = Ctx->Bus.PpidToApid[Ppid];
  if ((ApidValid & (UINT16)SPMI_PMIC_ARB_APID_VALID) == 0) {
    ApidValid = PmicArbFindApid(Ctx, Ppid);
    if ((ApidValid & (UINT16)SPMI_PMIC_ARB_APID_VALID) == 0) {
      return CR_NOT_FOUND;
    }
  }

  *Apid = (UINT16)(ApidValid & ~SPMI_PMIC_ARB_APID_VALID);
  return CR_SUCCESS;
}

UINT32
PmicArbGetChannelOffsetV2(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                          SPMI_ARB_CHANNEL_TYPE ChannelType) {
  UINT16 Ppid;
  UINT16 Apid;

  UNREFERENCED_PARAMETER(ChannelType);

  Ppid = (UINT16)((Sid << 8) | ((Addr >> 8) & 0xFF));
  if (CR_ERROR(PmicArbPpidToApidV2(Ctx, Ppid, &Apid))) {
    return (UINT32)-1;
  }

  return 0x1000 * Ctx->ActiveEE + 0x8000 * Apid;
}

UINT32
PmicArbFormatCmdV2(UINT8 Opc, UINT8 Sid, UINT16 Addr, UINT8 Bc) {
  UNREFERENCED_PARAMETER(Sid);
  return (UINT32)((Opc << 27) | ((Addr & 0xFF) << 4) | (Bc & 0x7));
}

UINTN
PmicArbGetOwnerAccStatusV2(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index) {
  return Ctx->Bus.InterruptAddr + 0x100000 + 0x1000 * Ee + 0x4 * Index;
}

UINTN
PmicArbGetAccEnableV2(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0x1000 * Apid;
}

UINTN
PmicArbGetIrqStatusV2(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0x4 + 0x1000 * Apid;
}

UINTN
PmicArbGetIrqClearV2(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0x8 + 0x1000 * Apid;
}

UINTN
PmicArbGetApidOwnerV2(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.ConfigAddr + 0x700 + 0x4 * Apid;
}

UINT32
PmicArbGetApidMapOffsetV2(UINT16 Apid) { return 0x800 + 0x4 * Apid; }
