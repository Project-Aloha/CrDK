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
