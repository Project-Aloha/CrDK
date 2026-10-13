/** UCSICr PMIC router and Alt Mode engine. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <ucsi/TypeC.h>
typedef CR_STATUS (*CR_PMIC_SEND)(VOID *Context, CONST VOID *Data, UINTN Length);
typedef struct {
  CR_PMIC_SEND Send;
  VOID *Context;
  CR_USB_TYPEC_STATE State;
  CR_USB_TYPEC_STATE AltState;
  UINT8 ReadBuffer[528];
  UINT16 Version;
  UINTN BufferSize;
  UINTN Stage, Operation;
  UINT64 Command, RequestedCommand;
  UINT64 DeadlineUs, ReplyDeadlineUs, NextPollUs;
  BOOLEAN ReadPending, ReadDone, WritePending, WriteDone;
  BOOLEAN PanPending, PanDone, PanEnabled;
  UINT32 PanCommand, PanAcks;
  UINT64 PanDeadlineUs;
  UINT32 Cci;
  CR_STATUS ReplyStatus;
  BOOLEAN Faulted;
  BOOLEAN ResetClearing, PanFaulted, AltValid, CompletionStarted;
  BOOLEAN StatusDirty;
  UINT32 Retries;
  UINT64 RetryAtUs;
  CR_STATUS PanError;
  UINT32 RxMessages, ReadReplies, WriteReplies, Notifications, ResetWrites;
  UINT32 LastOwner, LastType, LastOpcode, LastLength;
} CR_PMIC_GLINK;
VOID CrPmicInitialize(CR_PMIC_GLINK *Pmic, CR_PMIC_SEND Send, VOID *Context);
VOID CrPmicFail(CR_PMIC_GLINK *Pmic, CR_STATUS Status);
VOID CrPmicReceive(CR_PMIC_GLINK *Pmic, CONST UINT8 *Data, UINTN Length);
CR_STATUS CrPmicTick(CR_PMIC_GLINK *Pmic, UINT64 NowUs);
CR_STATUS CrPmicRequestRole(CR_PMIC_GLINK *Pmic, BOOLEAN Power, UINT32 Role);
BOOLEAN CrPmicNeedsService(CONST CR_PMIC_GLINK *Pmic);
