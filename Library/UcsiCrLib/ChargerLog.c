/**
  Read-only ChargerPD ULOG protocol adapted from OnePlus Android
  drivers/soc/qcom/charger-ulog-glink.c and Linux pmic_pdcharger_ulog.c.
  Copyright (c) 2021, The Linux Foundation.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <ucsi/ChargerLog.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
#ifdef _KERNEL_MODE
#include "ChargerLog.tmh"
#endif

#define OWNER_CHARGER 32778U
#define GET_ULOG 0x18U
#define GET_INIT_ULOG 0x23U
#define MAX_INIT_REQUESTS 8U
#define MAX_RUNTIME_REQUESTS 32U
#define LOG_TIMEOUT_US 1000000ULL
#define RUNTIME_INTERVAL_US 1000000ULL
#define RUNTIME_CAPTURE_US 60000000ULL

STATIC UINT32 Get32(CONST UINT8 *Data)
{ UINT32 Value; cr_memcpy(&Value, Data, sizeof(Value)); return Value; }

VOID CrChargerLogInitialize(CR_CHARGER_LOG *Log, CR_CHARGER_LOG_SEND Send, VOID *Context)
{
  cr_memset(Log, 0, sizeof(*Log));
  Log->Send = Send; Log->Context = Context;
  Log->LastError = CR_NOT_READY;
}

VOID CrChargerLogOnline(CR_CHARGER_LOG *Log, BOOLEAN Online)
{
  CR_CHARGER_LOG_SEND Send = Log->Send;
  VOID *Context = Log->Context;
  CrChargerLogInitialize(Log, Send, Context);
  Log->Online = Online;
}

CR_STATUS CrChargerLogReceive(CR_CHARGER_LOG *Log, CONST UINT8 *Data, UINTN Length)
{
  if (Log == NULL || Data == NULL) return CR_INVALID_PARAMETER;
  if (!Log->Online || !Log->Pending) return CR_SUCCESS;
  if (Length != 12U + CR_CHARGER_LOG_BYTES || Get32(Data) != OWNER_CHARGER ||
      (Get32(Data + 4) != 1 && Get32(Data + 4) != 2) ||
      Get32(Data + 8) != Log->Opcode) {
    if (++Log->Malformed <= 4U)
      log_warn("UCSICr: ChargerPD ignored malformed/stale log reply bytes=%u\n",
              (UINT32)Length);
    return CR_SUCCESS; /* diagnostic errors must not stop the PMIC router */
  }
  cr_memcpy(Log->Buffer, Data + 12, CR_CHARGER_LOG_BYTES);
  Log->Length = 0; Log->Offset = 0;
  while (Log->Length < sizeof(Log->Buffer) && Log->Buffer[Log->Length] != 0)
    ++Log->Length;
  Log->Pending = FALSE; Log->LastError = CR_SUCCESS;
  log_info("UCSICr: ChargerPD " CR_LOG_CHAR8_STR_FMT " log page=%u bytes=%u\n",
          Log->Runtime ? "runtime" : "init", Log->Requests, (UINT32)Log->Length);
  if (Log->Runtime && Log->RuntimeRequests == MAX_RUNTIME_REQUESTS) {
    Log->Done = TRUE;
    log_info("UCSICr: ChargerPD runtime capture reached %u-page limit\n",
            Log->RuntimeRequests);
  } else if (!Log->Runtime && (Log->Length == 0 || Log->InitRequests == MAX_INIT_REQUESTS)) {
    Log->Runtime = TRUE;
  }
  return CR_SUCCESS;
}

STATIC VOID PrintPage(CR_CHARGER_LOG *Log)
{
  UINTN Budget;
  /* Keep each serial/timer invocation bounded. Filter firmware control
     characters and pass text as an argument, never a format string. */
  for (Budget = 0; Budget < 4U && Log->Offset < Log->Length; ++Budget) {
    CHAR8 Line[161];
    UINTN Used = 0;
    while (Log->Offset < Log->Length && Used < sizeof(Line) - 1U) {
      UINT8 Character = Log->Buffer[Log->Offset++];
      if (Character == '\n') break;
      if (Character == '\r') continue;
      Line[Used++] = Character >= 32U && Character <= 126U ? (CHAR8)Character : '.';
    }
    Line[Used] = 0;
    if (Used != 0) log_info("ChargerPD: " CR_LOG_CHAR8_STR_FMT "\n", Line);
  }
  if (Log->Offset == Log->Length) Log->Offset = Log->Length = 0;
}

VOID CrChargerLogTick(CR_CHARGER_LOG *Log, UINT64 NowUs, BOOLEAN Trigger)
{
  CR_STATUS Status;
  UINT32 Request[4];
  if (Log == NULL || !Log->Online || Log->Send == NULL) return;
  Log->Triggered |= Trigger;
  PrintPage(Log);
  if (Log->Length != 0 || Log->Done || !Log->Triggered) return;
  if ((Log->Pending || Log->Waiting) && NowUs >= Log->DeadlineUs) {
    Log->Pending = Log->Waiting = FALSE; Log->LastError = CR_TIMEOUT;
    log_warn("UCSICr: ChargerPD log opcode=0x%x timed out\n", Log->Opcode);
    if (Log->Runtime) { Log->Done = TRUE; return; }
    Log->Runtime = TRUE;
  }
  if (Log->Pending) return;
  if (Log->Runtime && Log->RuntimeDeadlineUs != 0 && NowUs >= Log->RuntimeDeadlineUs) {
    Log->Done = TRUE;
    log_info("UCSICr: ChargerPD runtime capture complete after %u pages\n",
            Log->RuntimeRequests);
    return;
  }
  if (Log->Runtime && NowUs < Log->NextRequestUs) return;
  Request[0] = OWNER_CHARGER; Request[1] = 1;
  Request[2] = Log->Runtime ? GET_ULOG : GET_INIT_ULOG;
  Request[3] = CR_CHARGER_LOG_BYTES;
  Log->Opcode = Request[2];
  if (!Log->Waiting) {
    Log->Waiting = TRUE;
    Log->DeadlineUs = NowUs + LOG_TIMEOUT_US;
  }
  Status = Log->Send(Log->Context, Request, sizeof(Request));
  if (Status == CR_NOT_READY) return;
  Log->Waiting = FALSE;
  Log->LastError = Status;
  if (CR_ERROR(Status)) {
    Log->Done = TRUE;
    log_warn("UCSICr: ChargerPD diagnostic send failed: " CR_LOG_STATUS_FMT "\n", Status);
    return;
  }
  Log->Pending = TRUE;
  Log->DeadlineUs = NowUs + LOG_TIMEOUT_US;
  ++Log->Requests;
  if (!Log->Runtime) ++Log->InitRequests;
  else {
    ++Log->RuntimeRequests;
    if (Log->RuntimeDeadlineUs == 0) Log->RuntimeDeadlineUs = NowUs + RUNTIME_CAPTURE_US;
    Log->NextRequestUs = NowUs + RUNTIME_INTERVAL_US;
  }
}
