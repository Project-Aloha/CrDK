/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"
#ifdef _KERNEL_MODE
#include "pmic_arb_common.tmh"
#endif

CR_STATUS
PmicArbInitApidMinMax(SpmiDeviceContext *Ctx) {
  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.MaxApid = 0;
  Ctx->Bus.MinApid = (UINT16)(Ctx->PmicArb.MaxPeriphs - 1);
  return CR_SUCCESS;
}

CR_STATUS
PmicArbInitApidCommonV7V8(SpmiDeviceContext *Ctx, UINT32 Index, UINT32 MaxBuses,
                          UINT64 PeriphMsk) {
  UINT32 i;

  if (Ctx == NULL || Index >= MaxBuses) {
    log_err("SPMI: unsupported bus index %u detected", Index);
    return CR_INVALID_PARAMETER;
  }

  Ctx->Bus.BaseApid = 0;
  Ctx->Bus.ApidCount = 0;
  for (i = 0; i <= Index; i++) {
    Ctx->Bus.BaseApid += Ctx->Bus.ApidCount;
    Ctx->Bus.ApidCount = SpmiReadReg32(Ctx, SPMI_MEMORY_REGION_TYPE_CORE,
                                       SPMI_PMIC_ARB_FEATURES + i * 4) &
                         (UINT32)PeriphMsk;
  }

  if (Ctx->Bus.ApidCount == 0) {
    log_err("SPMI: bus %u not implemented", Index);
    return CR_INVALID_PARAMETER;
  }
  if (Ctx->Bus.BaseApid + Ctx->Bus.ApidCount > Ctx->PmicArb.MaxPeriphs) {
    log_err("SPMI: unsupported max APID %u detected (max %u)",
            Ctx->Bus.BaseApid + Ctx->Bus.ApidCount, Ctx->PmicArb.MaxPeriphs);
    return CR_INVALID_PARAMETER;
  }

  return PmicArbInitApidMinMax(Ctx);
}

CR_STATUS
PmicArbReadApidMapCommon(SpmiDeviceContext *Ctx, UINTN PpidBase, UINT64 PpidMsk,
                         UINT8 PpidSft, UINT64 IrqOwnerMsk) {
  SpmiPmicArbBus *Bus;
  UINT32 i;
  UINT32 ApidMax;
  UINT32 RegVal;
  UINT32 Offset;
  UINT16 Ppid;
  UINT16 Apid;
  UINTN MapSize;
  UINTN OwnerAddress;
  UINTN OwnerBase;
  UINTN OwnerSize;
  BOOLEAN Valid;
  BOOLEAN IsIrqEe;
  SpmiApidData *Apidd;
  SpmiApidData *PrevApidd;

  if (Ctx == NULL || Ctx->PmicArb.Ops == NULL ||
      Ctx->PmicArb.Ops->GetApidMapOffset == NULL ||
      Ctx->PmicArb.Ops->GetApidOwner == NULL || PpidBase == 0 ||
      PpidMsk == 0 || PpidSft >= 16 || Ctx->PmicArb.MaxPeriphs == 0 ||
      Ctx->PmicArb.MaxPeriphs > SPMI_PMIC_ARB_MAX_PERIPHS_V8 ||
      Ctx->Bus.BaseApid >= Ctx->PmicArb.MaxPeriphs ||
      Ctx->Bus.ApidCount == 0 ||
      Ctx->Bus.ApidCount > Ctx->PmicArb.MaxPeriphs - Ctx->Bus.BaseApid ||
      Ctx->Bus.BaseApid + Ctx->Bus.ApidCount >
          ARRAY_SIZE(Ctx->Bus.ApidData)) {
    return CR_INVALID_PARAMETER;
  }

  /* APID map entries are four-byte MMIO registers. v8 uses a separate
   * channel-map resource, while older arbiters expose the map in core. */
  MapSize = Ctx->PmicArb.ApidMapAddr != 0
                ? Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CH_MAP].Size
                : Ctx->PmicArb.CoreSize;
  if (MapSize < sizeof(UINT32)) {
    return CR_INVALID_PARAMETER;
  }

  if (Ctx->PmicArb.ApidMapAddr != 0) {
    OwnerBase = Ctx->Bus.ApidOwnerAddr;
    OwnerSize = Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CH_OWNER].Size;
  } else {
    OwnerBase = Ctx->Bus.ConfigAddr;
    OwnerSize = Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].Size;
  }
  if (OwnerBase == 0 || OwnerSize < sizeof(UINT32)) {
    return CR_INVALID_PARAMETER;
  }

  Bus = &Ctx->Bus;
  Apidd = &Bus->ApidData[Bus->BaseApid];
  ApidMax = (UINT32)Bus->BaseApid + Bus->ApidCount;
  Bus->ApidMapValid = FALSE;
  for (i = Bus->BaseApid; i < ApidMax; i++, Apidd++) {
    Offset = Ctx->PmicArb.Ops->GetApidMapOffset((UINT16)i);
    if ((UINTN)Offset > MapSize - sizeof(UINT32) ||
        PpidBase > (UINTN)-1 - (UINTN)Offset) {
      return CR_INVALID_PARAMETER;
    }

    RegVal = SpmiIoRead32(Ctx, PpidBase + Offset);
    if (RegVal == 0) {
      continue;
    }

    if (PpidSft >= 32) {
      return CR_INVALID_PARAMETER;
    }
    Ppid = (UINT16)(((UINT64)RegVal >> PpidSft) & PpidMsk);
    if ((UINT64)Ppid >= SPMI_PMIC_ARB_MAX_PPID) {
      return CR_INVALID_PARAMETER;
    }
    IsIrqEe = (RegVal & IrqOwnerMsk) != 0;

    OwnerAddress = Ctx->PmicArb.Ops->GetApidOwner(Ctx, (UINT16)i);
    if (OwnerAddress < OwnerBase ||
        OwnerAddress - OwnerBase > OwnerSize - sizeof(UINT32)) {
      return CR_INVALID_PARAMETER;
    }
    RegVal = SpmiIoRead32(Ctx, OwnerAddress);
    Apidd->WriteEe = (UINT8)SPMI_OWNERSHIP_PERIPH2OWNER(RegVal);
    Apidd->IrqEe = IsIrqEe ? Apidd->WriteEe : SPMI_PMIC_ARB_INVALID_EE;

    Valid = (Bus->PpidToApid[Ppid] & (UINT16)SPMI_PMIC_ARB_APID_VALID) != 0;
    Apid = (UINT16)(Bus->PpidToApid[Ppid] & ~SPMI_PMIC_ARB_APID_VALID);
    if (Valid && Apid >= Ctx->PmicArb.MaxPeriphs) {
      return CR_INVALID_PARAMETER;
    }
    PrevApidd = Valid ? &Bus->ApidData[Apid] : NULL;

    if (!Valid || Apidd->WriteEe == Ctx->ActiveEE) {
      Bus->PpidToApid[Ppid] = i | (UINT16)SPMI_PMIC_ARB_APID_VALID;
    } else if (Valid && IsIrqEe && PrevApidd->WriteEe == Ctx->ActiveEE) {
      PrevApidd->IrqEe = Apidd->IrqEe;
    }

    Apidd->Ppid = Ppid;
    Bus->LastApid = i;
  }

  Bus->ApidMapValid = TRUE;
  return CR_SUCCESS;
}
