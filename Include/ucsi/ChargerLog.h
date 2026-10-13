/** Bounded ChargerPD firmware diagnostics. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_CHARGER_LOG_BYTES 8192U
typedef CR_STATUS (*CR_CHARGER_LOG_SEND)(VOID *Context, CONST VOID *Data, UINTN Length);
typedef struct {
  CR_CHARGER_LOG_SEND Send;
  VOID *Context;
  BOOLEAN Online, Triggered, Pending, Waiting, Runtime, Done;
  UINT32 Opcode, Requests, InitRequests, RuntimeRequests, Malformed;
  UINT64 DeadlineUs, RuntimeDeadlineUs, NextRequestUs;
  UINT8 Buffer[CR_CHARGER_LOG_BYTES];
  UINTN Length, Offset;
  CR_STATUS LastError;
} CR_CHARGER_LOG;

VOID CrChargerLogInitialize(CR_CHARGER_LOG *Log, CR_CHARGER_LOG_SEND Send, VOID *Context);
VOID CrChargerLogOnline(CR_CHARGER_LOG *Log, BOOLEAN Online);
CR_STATUS CrChargerLogReceive(CR_CHARGER_LOG *Log, CONST UINT8 *Data, UINTN Length);
VOID CrChargerLogTick(CR_CHARGER_LOG *Log, UINT64 NowUs, BOOLEAN Trigger);
