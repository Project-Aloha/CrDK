/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "pmic_arb_v1.tmh"
#endif

CR_STATUS
PmicArbGetCoreResourceV1(SpmiDeviceContext *Ctx) {
  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }

  Ctx->PmicArb.CoreAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CORE].BaseAddress;
  Ctx->PmicArb.WriteAddr = Ctx->PmicArb.CoreAddr;
  Ctx->PmicArb.ReadAddr = Ctx->PmicArb.CoreAddr;
  Ctx->PmicArb.CoreSize =
      (UINT32)Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CORE].Size;
  Ctx->PmicArb.MaxPeriphs = SPMI_PMIC_ARB_MAX_PERIPHS;
  Ctx->PmicArb.BusAvailable = 1;

  if (Ctx->PmicArb.CoreAddr == 0 || Ctx->PmicArb.CoreSize == 0) {
    log_err("SPMI: invalid core region for arbiter v1");
    return CR_NOT_FOUND;
  }

  Ctx->Bus.InterruptAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_INTERRUPT].BaseAddress;
  Ctx->Bus.ConfigAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].BaseAddress;
  if (Ctx->Bus.InterruptAddr == 0 || Ctx->Bus.ConfigAddr == 0) {
    log_err("SPMI: arbiter v1 requires intr and cnfg regions");
    return CR_NOT_FOUND;
  }

  return CR_SUCCESS;
}

CR_STATUS
PmicArbInitApidV1(SpmiDeviceContext *Ctx, UINT32 Index) {
  if (Ctx == NULL || Index != 0) {
    log_err("SPMI: v1 only supports a single bus");
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.BaseApid = 0;
  Ctx->Bus.ApidCount = Ctx->PmicArb.MaxPeriphs;
  Ctx->Bus.ApidMapValid = FALSE;
  return PmicArbInitApidMinMax(Ctx);
}

CR_STATUS
PmicArbPpidToApidV1(SpmiDeviceContext *Ctx, UINT16 Ppid, UINT16 *Apid) {
  UINT32 Index = 0;
  UINT32 Data = 0;
  UINTN i;

  if (Ctx == NULL || Apid == NULL) {
    return CR_INVALID_PARAMETER;
  }

  for (i = 0; i < SPMI_MAPPING_TABLE_TREE_DEPTH; ++i) {
    Data =
        SpmiIoRead32(Ctx, Ctx->Bus.ConfigAddr + SPMI_MAPPING_TABLE_REG(Index));

    if (Ppid & BIT(SPMI_MAPPING_BIT_INDEX(Data))) {
      if (SPMI_MAPPING_BIT_IS_1_FLAG(Data)) {
        Index = SPMI_MAPPING_BIT_IS_1_RESULT(Data);
      } else {
        *Apid = (UINT16)SPMI_MAPPING_BIT_IS_1_RESULT(Data);
        Ctx->Bus.PpidToApid[Ppid] = *Apid | (UINT16)SPMI_PMIC_ARB_APID_VALID;
        Ctx->Bus.ApidData[*Apid].Ppid = Ppid;
        return CR_SUCCESS;
      }
    } else {
      if (SPMI_MAPPING_BIT_IS_0_FLAG(Data)) {
        Index = SPMI_MAPPING_BIT_IS_0_RESULT(Data);
      } else {
        *Apid = (UINT16)SPMI_MAPPING_BIT_IS_0_RESULT(Data);
        Ctx->Bus.PpidToApid[Ppid] = *Apid | (UINT16)SPMI_PMIC_ARB_APID_VALID;
        Ctx->Bus.ApidData[*Apid].Ppid = Ppid;
        return CR_SUCCESS;
      }
    }
  }

  log_err("SPMI: failed to translate PPID 0x%X on arbiter v1", Ppid);
  return CR_NOT_FOUND;
}

UINT32
PmicArbGetChannelOffsetV1(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                          SPMI_ARB_CHANNEL_TYPE ChannelType) {
  UNREFERENCED_PARAMETER(Sid);
  UNREFERENCED_PARAMETER(Addr);
  UNREFERENCED_PARAMETER(ChannelType);
  return 0x800 + 0x80 * Ctx->Channel;
}

UINT32
PmicArbFormatCmdV1(UINT8 Opc, UINT8 Sid, UINT16 Addr, UINT8 Bc) {
  return (UINT32)((Opc << 27) | ((Sid & 0xF) << 20) | (Addr << 4) | (Bc & 0x7));
}

CR_STATUS
PmicArbCheckChannelStatusV1(SpmiDeviceContext *Ctx, UINT32 Status, UINT8 Sid,
                            UINT16 Addr, UINT32 Offset) {
  UNREFERENCED_PARAMETER(Ctx);

  if ((Status & BIT(0)) == 0) {
    return CR_BUSY; /* not done yet */
  }
  if (Status & BIT(1)) {
    log_err(
        "SPMI: sid 0x%X addr 0x%X transaction failed (status 0x%X) reg 0x%X",
        Sid, Addr, Status, Offset);
    return CR_DEVICE_ERROR;
  }
  if (Status & BIT(2)) {
    log_err("SPMI: sid 0x%X addr 0x%X transaction denied (status 0x%X)", Sid,
            Addr, Status);
    return CR_ABORT;
  }
  if (Status & BIT(3)) {
    log_err("SPMI: sid 0x%X addr 0x%X transaction dropped (status 0x%X)", Sid,
            Addr, Status);
    return CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}

UINTN
PmicArbGetOwnerAccStatusV1(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index) {
  return Ctx->Bus.InterruptAddr + 0x20 * Ee + 0x4 * Index;
}

UINTN
PmicArbGetAccEnableV1(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0x200 + 0x4 * Apid;
}

UINTN
PmicArbGetIrqStatusV1(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0x600 + 0x4 * Apid;
}

UINTN
PmicArbGetIrqClearV1(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.InterruptAddr + 0xA00 + 0x4 * Apid;
}

UINTN
PmicArbGetApidOwnerV1(SpmiDeviceContext *Ctx, UINT16 Apid) {
  return Ctx->Bus.ConfigAddr + 0x700 + 0x4 * Apid;
}

UINT32
PmicArbGetApidMapOffsetV1(UINT16 Apid) { return 0x800 + 0x4 * Apid; }
