/** @file
 *  Qualcomm PMIC arbiter v8/v8.5 register layout.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"

CR_STATUS
PmicArbGetCoreResourceV8(IN SpmiDeviceContext *Ctx) {
  CR_STATUS Status;

  Status = PmicArbGetCoreResourceV2(Ctx);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Ctx->PmicArb.MaxPeriphs = SPMI_PMIC_ARB_MAX_PERIPHS_V8;
  Ctx->PmicArb.BusAvailable = SPMI_PMIC_ARB_MAX_BUSES;
  Ctx->PmicArb.ApidMapAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CH_MAP].BaseAddress;
  Ctx->Bus.ApidOwnerAddr =
      Ctx->Regions[SPMI_MEMORY_REGION_TYPE_CH_OWNER].BaseAddress;
  if (Ctx->PmicArb.ApidMapAddr == 0 || Ctx->Bus.ApidOwnerAddr == 0) {
    return CR_NOT_FOUND;
  }
  return CR_SUCCESS;
}

CR_STATUS
PmicArbInitApidV8(IN SpmiDeviceContext *Ctx, IN UINT32 Index) {
  CR_STATUS Status;

  Status = PmicArbInitApidCommonV7V8(Ctx, Index, SPMI_PMIC_ARB_MAX_BUSES,
                                     SPMI_PMIC_ARB_FEATURES_V8_PERIPH_MASK);
  if (CR_ERROR(Status)) {
    return Status;
  }
  return PmicArbReadApidMapCommon(
      Ctx, Ctx->PmicArb.ApidMapAddr, SPMI_PMIC_ARB_V8_PPID_MASK,
      SPMI_PMIC_ARB_PPID_SHIFT_V8, SPMI_PMIC_ARB_V8_CHAN_IS_IRQ_OWNER_MASK);
}

UINT32
PmicArbGetChannelOffsetV8(IN SpmiDeviceContext *Ctx, IN UINT8 Sid,
                          IN UINT16 Addr,
                          IN SPMI_ARB_CHANNEL_TYPE ChannelType) {
  UINT16 Apid;

  if (CR_ERROR(PmicArbPpidToApidV5(Ctx, (UINT16)((Sid << 8) | (Addr >> 8)),
                                   &Apid))) {
    return (UINT32)-1;
  }
  if (ChannelType == SPMI_ARB_CHANNEL_OBS) {
    return 0x40000U * Ctx->ActiveEE + 0x20U * Apid;
  }
  if (ChannelType != SPMI_ARB_CHANNEL_RW ||
      Ctx->Bus.ApidData[Apid].WriteEe != Ctx->ActiveEE) {
    return (UINT32)-1;
  }
  return 0x200U * Apid;
}

UINTN
PmicArbGetAccEnableV8(IN SpmiDeviceContext *Ctx, IN UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x100 + 0x200 * Apid;
}

UINTN
PmicArbGetIrqStatusV8(IN SpmiDeviceContext *Ctx, IN UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x104 + 0x200 * Apid;
}

UINTN
PmicArbGetIrqClearV8(IN SpmiDeviceContext *Ctx, IN UINT16 Apid) {
  return Ctx->PmicArb.WriteAddr + 0x108 + 0x200 * Apid;
}

UINTN
PmicArbGetApidOwnerV8(IN SpmiDeviceContext *Ctx, IN UINT16 Apid) {
  return Ctx->Bus.ApidOwnerAddr + 0x4 * (Apid - Ctx->Bus.BaseApid);
}

UINT32
PmicArbGetApidMapOffsetV8(IN UINT16 Apid) { return 0x4 * Apid; }

CR_STATUS
PmicArbCheckChannelStatusV8P5(IN SpmiDeviceContext *Ctx, IN UINT32 Status,
                              IN UINT8 Sid, IN UINT16 Addr, IN UINT32 Offset) {
  UNREFERENCED_PARAMETER(Ctx);
  UNREFERENCED_PARAMETER(Sid);
  UNREFERENCED_PARAMETER(Addr);
  UNREFERENCED_PARAMETER(Offset);

  if ((Status & BIT(0)) == 0) {
    return CR_BUSY;
  }
  if ((Status & (BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(6))) != 0) {
    return CR_DEVICE_ERROR;
  }
  if ((Status & BIT(5)) != 0) {
    return CR_ABORT;
  }
  return CR_SUCCESS;
}
