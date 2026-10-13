/** @file
 *  Internal helpers for the Crane Qualcomm PMIC arbiter driver.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/spmi.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_lock.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>

#include "pmic_arb.h"

STATIC inline BOOLEAN SpmiRegionFits(IN CONST SpmiDeviceContext *Ctx,
    IN SPMI_MEMORY_REGION_TYPE Type, IN UINT64 Offset, IN UINTN Length) {
  if (Ctx==NULL || (UINT32)Type>=SPMI_MEMORY_REGION_TYPE_MAX || Length==0) return FALSE;
  CONST SpmiMemoryRegion *Region=&Ctx->Regions[Type];
  return Region->BaseAddress!=0 && (Region->BaseAddress&3U)==0 &&
    Region->Size>=Length && Region->BaseAddress<=(UINTN)-1-Region->Size &&
    Offset<=Region->Size-Length;
}
STATIC inline BOOLEAN SpmiAddressFits(IN CONST SpmiDeviceContext *Ctx,
    IN UINTN Address, IN UINTN Length) {
  if (Ctx==NULL || (Address&3U)!=0) return FALSE;
  for (UINTN I=0;I<SPMI_MEMORY_REGION_TYPE_MAX;++I) {
    if (Address>=Ctx->Regions[I].BaseAddress &&
        SpmiRegionFits(Ctx,(SPMI_MEMORY_REGION_TYPE)I,
                       Address-Ctx->Regions[I].BaseAddress,Length)) return TRUE;
  }
  return FALSE;
}
STATIC inline BOOLEAN SpmiApidValid(IN CONST SpmiDeviceContext *Ctx,IN UINT16 Apid) {
  return Ctx!=NULL && Ctx->PmicArb.MaxPeriphs<=SPMI_PMIC_ARB_MAX_PERIPHS_V8 &&
    Ctx->Bus.ApidCount!=0 && Ctx->Bus.BaseApid<Ctx->PmicArb.MaxPeriphs &&
    Ctx->Bus.ApidCount<=Ctx->PmicArb.MaxPeriphs-Ctx->Bus.BaseApid &&
    Apid>=Ctx->Bus.BaseApid && Apid<(UINT32)Ctx->Bus.BaseApid+Ctx->Bus.ApidCount;
}
STATIC inline BOOLEAN SpmiChannelFits(IN CONST SpmiDeviceContext *Ctx,IN BOOLEAN Write,
    IN UINT32 Offset,IN UINTN Length) {
  SPMI_MEMORY_REGION_TYPE Type=Ctx->Version==SPMI_ARB_VERSION_1 ?
    SPMI_MEMORY_REGION_TYPE_CORE : (Write?SPMI_MEMORY_REGION_TYPE_CH_SLAVES:
                                        SPMI_MEMORY_REGION_TYPE_OBSERVER);
  UINTN Last=Write ? (Length>4?SPMI_PMIC_ARB_WDATA1:SPMI_PMIC_ARB_WDATA0) :
                    (Length>4?SPMI_PMIC_ARB_RDATA1:SPMI_PMIC_ARB_RDATA0);
  return (Offset&3U)==0 &&
    (Write?Ctx->PmicArb.WriteAddr:Ctx->PmicArb.ReadAddr)==Ctx->Regions[Type].BaseAddress &&
    SpmiRegionFits(Ctx,Type,(UINT64)Offset+Last,sizeof(UINT32));
}
STATIC inline BOOLEAN SpmiIrqApidFits(IN SpmiDeviceContext *Ctx,IN UINT16 Apid) {
  CONST SpmiPmicArbOps *Ops=Ctx->PmicArb.Ops;
  return SpmiApidValid(Ctx,Apid) && Ops!=NULL && Ops->GetAccEnable!=NULL &&
    Ops->GetIrqStatus!=NULL && Ops->GetIrqClear!=NULL &&
    SpmiAddressFits(Ctx,Ops->GetAccEnable(Ctx,Apid),sizeof(UINT32)) &&
    SpmiAddressFits(Ctx,Ops->GetIrqStatus(Ctx,Apid),sizeof(UINT32)) &&
    SpmiAddressFits(Ctx,Ops->GetIrqClear(Ctx,Apid),sizeof(UINT32));
}

typedef enum {
  SPMI_ARB_OP_EXT_WRITEL = 0,
  SPMI_ARB_OP_EXT_READL = 1,
  SPMI_ARB_OP_EXT_WRITE = 2,
  SPMI_ARB_OP_RESET = 3,
  SPMI_ARB_OP_SLEEP = 4,
  SPMI_ARB_OP_SHUTDOWN = 5,
  SPMI_ARB_OP_WAKEUP = 6,
  SPMI_ARB_OP_EXT_READ = 13,
  SPMI_ARB_OP_WRITE = 14,
  SPMI_ARB_OP_READ = 15,
  SPMI_ARB_OP_ZERO_WRITE = 16
} SPMI_ARB_OPCODE;

STATIC inline UINT32 SpmiIoRead32(IN SpmiDeviceContext *Ctx, IN UINTN Address) {
  if (Ctx->Io.Read32 != NULL) {
    return Ctx->Io.Read32(Ctx->Io.Context, Address);
  }
  return CrMmioRead32(Address);
}

STATIC inline VOID SpmiIoWrite32(IN SpmiDeviceContext *Ctx, IN UINTN Address,
                                 IN UINT32 Value) {
  if (Ctx->Io.Write32 != NULL) {
    Ctx->Io.Write32(Ctx->Io.Context, Address, Value);
    return;
  }
  CrMmioWrite32(Address, Value);
}

STATIC inline VOID SpmiIoStall(IN SpmiDeviceContext *Ctx,
                               IN UINT32 Microseconds) {
  if (Ctx->Io.StallUs != NULL) {
    Ctx->Io.StallUs(Ctx->Io.Context, Microseconds);
    return;
  }
  cr_sleep(Microseconds);
}

STATIC inline UINT32 SpmiReadReg32(IN SpmiDeviceContext *Ctx,
                                   IN SPMI_MEMORY_REGION_TYPE RegionType,
                                   IN UINT32 Address) {
  return SpmiIoRead32(Ctx, Ctx->Regions[RegionType].BaseAddress + Address);
}

STATIC inline VOID SpmiWriteReg32(IN SpmiDeviceContext *Ctx,
                                  IN SPMI_MEMORY_REGION_TYPE RegionType,
                                  IN UINT32 Address, IN UINT32 Value) {
  SpmiIoWrite32(Ctx, Ctx->Regions[RegionType].BaseAddress + Address, Value);
}

CR_STATUS
SpmiPmicArbProbe(IN OUT SpmiDeviceContext *Ctx);

CR_STATUS
SpmiPmicArbBusInit(IN OUT SpmiDeviceContext *Ctx);

CR_STATUS
SpmiWaitForDone(IN SpmiDeviceContext *Ctx, IN UINTN BaseAddress,
                IN UINT32 Offset, IN UINT8 Sid, IN UINT16 Addr);

CR_STATUS
SpmiIrqInit(IN OUT SpmiDeviceContext *Ctx);

VOID SpmiIrqIsr(IN VOID *Context);
