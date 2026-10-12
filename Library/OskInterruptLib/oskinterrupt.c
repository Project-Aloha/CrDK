/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#include <Protocol/HardwareInterrupt2.h>

STATIC CR_INTERRUPT_ENTRY mCrIntTable[MAX_INTERRUPT_ENTRIES];
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL *mHwInterrupt2;
STATIC UINTN mDispatchDepth;
typedef struct { CONST CR_INTERRUPT_CONFIG *Config; UINT64 Generation; BOOLEAN Leased; } CR_IRQ_LIFETIME;
STATIC CR_IRQ_LIFETIME mCrIntLifetime[MAX_INTERRUPT_ENTRIES];
STATIC UINT64 mCrIntGeneration;
STATIC CR_INTERRUPT_ENTRY *mDispatchEntry;
STATIC CR_STATUS CrInterruptInit(VOID);
STATIC CR_STATUS LeaseExact(CR_STATUS Status);

CR_STATUS CrInterruptGuardAcquire(CR_INTERRUPT_GUARD *Guard) {
  if (Guard == NULL) return CR_INVALID_PARAMETER;
  Guard->Active=FALSE;
  if (gBS == NULL || gBS->RaiseTPL == NULL || gBS->RestoreTPL == NULL)
    return CR_NOT_READY;
  if (mDispatchDepth != 0) return CR_BUSY;
  Guard->PreviousPriority=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  if (mHwInterrupt2 == NULL) {
    /* DXE LocateProtocol takes the protocol database's TPL_NOTIFY lock.
     * Bootstrap before the HIGH route/table exclusion. Nested GPIO/PDC
     * setup guards then reuse the validated provider without boot services. */
    gBS->RestoreTPL(Guard->PreviousPriority);
    if (Guard->PreviousPriority > TPL_NOTIFY) return CR_NOT_READY;
    CR_STATUS Status=CrInterruptInit();
    if (CR_ERROR(Status)) return Status;
    Guard->PreviousPriority=gBS->RaiseTPL(TPL_HIGH_LEVEL);
  }
  Guard->Active=TRUE;
  return CR_SUCCESS;
}
VOID CrInterruptGuardRelease(CR_INTERRUPT_GUARD *Guard) {
  if (Guard == NULL || !Guard->Active) return;
  Guard->Active=FALSE;
  gBS->RestoreTPL(Guard->PreviousPriority);
}

VOID EFIAPI CrInternalInterruptEntry(IN HARDWARE_INTERRUPT_SOURCE Source,
                                     IN EFI_SYSTEM_CONTEXT SystemContext) {
  CR_INTERRUPT_ENTRY *Entry = NULL;
  EFI_TPL OriginalTpl;
  (VOID)SystemContext;
  /* ArmGicDxe invokes source handlers without changing UEFI's current TPL.
   * Hold HIGH through the ISR and EOI, as Arm TimerDxe does, so SignalEvent
   * only queues lower-TPL work instead of running it before IRQ completion. */
  OriginalTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  ++mDispatchDepth;
  for (UINTN i = 0; i < MAX_INTERRUPT_ENTRIES; i++) {
    if ((mCrIntTable[i].Source == Source) &&
        (mCrIntTable[i].Handler != NULL)) {
      Entry = &mCrIntTable[i];
      mDispatchEntry = Entry;
      Entry->Handler(Entry->Param);
      mDispatchEntry = NULL;
      break;
    }
  }
  // End of interrupt handling
  mHwInterrupt2->EndOfInterrupt(mHwInterrupt2, Source);
  --mDispatchDepth;
  gBS->RestoreTPL(OriginalTpl);
}

STATIC CR_STATUS
CrInterruptInit(VOID) {
  CR_STATUS Status;
  EFI_HARDWARE_INTERRUPT2_PROTOCOL *Provider = NULL;
  if (mHwInterrupt2 != NULL) return CR_SUCCESS;
  if (gBS == NULL || gBS->LocateProtocol == NULL ||
      gBS->RaiseTPL == NULL || gBS->RestoreTPL == NULL) {
    return CR_NOT_FOUND;
  }
  /* Called only outside the HIGH guard at <=TPL_NOTIFY. Publish the cache
   * only after validating every method used by dispatch and rollback. */
  Status = gBS->LocateProtocol(&gHardwareInterrupt2ProtocolGuid, NULL,
                               (VOID **)&Provider);
  if (CR_ERROR(Status)) {
    log_err("HardwareInterrupt2 Protocol not found: %r", Status);
    return CR_NOT_FOUND;
  }
  if (Provider == NULL || Provider->RegisterInterruptSource == NULL ||
      Provider->EnableInterruptSource == NULL ||
      Provider->DisableInterruptSource == NULL ||
      Provider->EndOfInterrupt == NULL || Provider->SetTriggerType == NULL)
    return CR_UNSUPPORTED;
  mHwInterrupt2 = Provider;
  return CR_SUCCESS;
}

STATIC inline EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE
CrInterruptTypeTanslate(IN CR_INTERRUPT_TRIGGER_TYPE Type) {
  switch (Type) {
  case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_LOW;
  case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH;
  case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_FALLING;
  case CR_INTERRUPT_TRIGGER_EDGE_RISING:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_RISING;
  case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
    // Not supported directly
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_RISING;
  default:
    return 0xFF;
  }
}

STATIC
CR_INTERRUPT_ENTRY *
FindInterruptEntry(IN UINT32 InterruptNumber)
{
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES; Index++) {
    if (mCrIntTable[Index].Handler != NULL &&
        mCrIntTable[Index].Source == InterruptNumber) {
      return &mCrIntTable[Index];
    }
  }
  return NULL;
}

STATIC CR_STATUS
RegisterInterruptLocked(CR_INTERRUPT_CONFIG *InterruptConfig)
{
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  CR_INTERRUPT_ENTRY *Entry;
  EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE TriggerType;

  if (InterruptConfig == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (InterruptConfig->Handler == NULL) {
    return CrUnregisterInterrupt(InterruptConfig);
  }
  if (mHwInterrupt2 == NULL) return CR_NOT_READY;

  TriggerType = CrInterruptTypeTanslate(InterruptConfig->TriggerType);
  if (TriggerType == 0xFF) {
    return CR_INVALID_PARAMETER;
  }
  if (FindInterruptEntry(InterruptConfig->InterruptNumber) != NULL) {
    return CR_BUSY;
  }

  Entry = NULL;
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES; Index++) {
    if (mCrIntTable[Index].Handler == NULL) {
      Entry = &mCrIntTable[Index];
      break;
    }
  }
  if (Entry == NULL) {
    return CR_OUT_OF_RESOURCES;
  }

  if (mCrIntGeneration == MAX_UINT64) return CR_OUT_OF_RESOURCES;
  mCrIntLifetime[Entry - mCrIntTable].Config = InterruptConfig;
  mCrIntLifetime[Entry - mCrIntTable].Leased = FALSE;
  mCrIntLifetime[Entry - mCrIntTable].Generation = ++mCrIntGeneration;

  /* MU's RegisterInterruptSource may enable the interrupt immediately, so
   * publish the dispatch entry before installing the wrapper. */
  Entry->Handler = InterruptConfig->Handler;
  Entry->Param = InterruptConfig->Param;
  Entry->Source = InterruptConfig->InterruptNumber;
  Status = mHwInterrupt2->RegisterInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber, CrInternalInterruptEntry);
  if (Status == EFI_ALREADY_STARTED) {
    /* An unrelated driver owns this source.  Never unregister its callback. */
    Entry->Handler = NULL;
    Entry->Param = NULL;
    Entry->Source = 0;
    mCrIntLifetime[Entry - mCrIntTable].Config = NULL;
    mCrIntLifetime[Entry - mCrIntTable].Generation = 0;
    return LeaseExact(Status);
  }
  if (Status != CR_SUCCESS) {
    goto Rollback;
  }

  Status = mHwInterrupt2->SetTriggerType(
      mHwInterrupt2, InterruptConfig->InterruptNumber, TriggerType);
  if (Status != CR_SUCCESS) {
    goto Rollback;
  }
  Status = mHwInterrupt2->EnableInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber);
  if (Status == CR_SUCCESS) {
    return CR_SUCCESS;
  }

Rollback:
  /* Keep the entry if removal fails.  The owning DXE image must then remain
   * resident until a later cleanup succeeds; losing the entry would leave a
   * hardware callback into an image which the dispatcher can unload. */
  CleanupStatus = CrUnregisterInterrupt(InterruptConfig);
  return CleanupStatus != CR_SUCCESS ? LeaseExact(CleanupStatus) : LeaseExact(Status);
}

STATIC CR_STATUS
UnregisterInterruptLocked(IN CR_INTERRUPT_CONFIG *InterruptConfig)
{
  EFI_STATUS Status;
  CR_INTERRUPT_ENTRY *Entry;

  if (InterruptConfig == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Entry = FindInterruptEntry(InterruptConfig->InterruptNumber);
  if (Entry == NULL) {
    return CR_NOT_FOUND;
  }
  if (mCrIntLifetime[Entry - mCrIntTable].Config != InterruptConfig) return CR_BUSY;
  if (InterruptConfig->Handler != NULL &&
      (Entry->Handler != InterruptConfig->Handler ||
       Entry->Param != InterruptConfig->Param)) {
    return CR_BUSY;
  }
  if (mHwInterrupt2 == NULL) {
    /* A retained entry still represents a callback ownership obligation. */
    return CR_DEVICE_ERROR;
  }

  Status = mHwInterrupt2->DisableInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber);
  if (Status != CR_SUCCESS) {
    return LeaseExact(Status);
  }
  if (mCrIntLifetime[Entry - mCrIntTable].Leased) {
    BOOLEAN Enabled=TRUE;
    if (mHwInterrupt2->GetInterruptSourceState == NULL) return CR_UNSUPPORTED;
    Status=mHwInterrupt2->GetInterruptSourceState(mHwInterrupt2,InterruptConfig->InterruptNumber,&Enabled);
    if (Status != CR_SUCCESS) return LeaseExact(Status);
    if (Enabled) return CR_DEVICE_ERROR;
  }
  Status = mHwInterrupt2->RegisterInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber, NULL);
  if (Status != CR_SUCCESS) {
    return LeaseExact(Status);
  }

  Entry->Handler = NULL;
  Entry->Param = NULL;
  Entry->Source = 0;
  mCrIntLifetime[Entry - mCrIntTable].Config = NULL;
  mCrIntLifetime[Entry - mCrIntTable].Generation = 0;
  return CR_SUCCESS;
}

CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  if (Config == NULL) return CR_INVALID_PARAMETER;
  CR_INTERRUPT_GUARD Guard;
  CR_STATUS Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  Status=RegisterInterruptLocked(Config);
  CrInterruptGuardRelease(&Guard);return Status;
}
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  if (Config == NULL) return CR_INVALID_PARAMETER;
  CR_INTERRUPT_GUARD Guard;
  CR_STATUS Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  Status=UnregisterInterruptLocked(Config);
  CrInterruptGuardRelease(&Guard);return Status;
}

BOOLEAN
CrInterruptIsRegistered(IN CR_INTERRUPT_CONFIG *InterruptConfig)
{
  CR_INTERRUPT_ENTRY *Entry;
  if (InterruptConfig == NULL || InterruptConfig->Handler == NULL) return FALSE;
  Entry = FindInterruptEntry(InterruptConfig->InterruptNumber);
  return Entry != NULL && mCrIntLifetime[Entry-mCrIntTable].Config == InterruptConfig &&
         Entry->Handler == InterruptConfig->Handler &&
         Entry->Param == InterruptConfig->Param;
}

STATIC CR_STATUS LeaseExact(CR_STATUS Status) {
  return Status == CR_SUCCESS || CR_ERROR(Status) ? Status : CR_NOT_READY;
}
STATIC CR_INTERRUPT_ENTRY *LeaseEntry(CONST CR_INTERRUPT_LEASE *Lease) {
  if (Lease == NULL || Lease->Config == NULL || Lease->Handler == NULL ||
      Lease->Generation == 0) return NULL;
  for (UINTN I = 0; I < MAX_INTERRUPT_ENTRIES; ++I) {
    CR_INTERRUPT_ENTRY *E = &mCrIntTable[I];
    if (mCrIntLifetime[I].Config == Lease->Config &&
        mCrIntLifetime[I].Generation == Lease->Generation &&
        E->Handler == Lease->Handler && E->Param == Lease->Context &&
        E->Source == Lease->Source) return E;
  }
  return NULL;
}
CR_STATUS CrInterruptGetLease(CR_INTERRUPT_CONFIG *Config, CR_INTERRUPT_LEASE *Out) {
  CR_INTERRUPT_GUARD Guard;
  CR_STATUS Status;
  CR_INTERRUPT_LEASE Result;
  if (Config == NULL || Out == NULL || Config->Handler == NULL)
    return CR_INVALID_PARAMETER;
  UINTN A=(UINTN)Config,B=(UINTN)Out;
  if (A > MAX_UINTN-sizeof(*Config) || B > MAX_UINTN-sizeof(*Out) ||
      (A < B+sizeof(*Out) && B < A+sizeof(*Config))) return CR_INVALID_PARAMETER;
  if (mHwInterrupt2 == NULL) return CR_NOT_READY;
  if (mHwInterrupt2->GetInterruptSourceState == NULL) return CR_UNSUPPORTED;
  Status=CrInterruptGuardAcquire(&Guard);
  if (Status != CR_SUCCESS) return LeaseExact(Status);
  CR_INTERRUPT_ENTRY *Entry=FindInterruptEntry(Config->InterruptNumber);
  Status=CR_NOT_FOUND;
  if (Entry != NULL && Entry->Handler == Config->Handler && Entry->Param == Config->Param &&
      mCrIntLifetime[Entry-mCrIntTable].Config == Config) {
    Result.Config=Config;Result.Handler=Entry->Handler;Result.Context=Entry->Param;
    Result.Generation=mCrIntLifetime[Entry-mCrIntTable].Generation;Result.Source=Entry->Source;
    mCrIntLifetime[Entry-mCrIntTable].Leased=TRUE;
    *Out=Result;Status=CR_SUCCESS;
  }
  CrInterruptGuardRelease(&Guard);return Status;
}
STATIC CR_STATUS SetLeaseMask(CONST CR_INTERRUPT_LEASE *Lease, BOOLEAN Enable) {
  CR_INTERRUPT_GUARD Guard={0};
  CR_STATUS Status;
  CR_INTERRUPT_ENTRY *Entry;
  BOOLEAN Enabled=FALSE;
  if (Lease == NULL) return CR_INVALID_PARAMETER;
  if (mHwInterrupt2 == NULL) return CR_NOT_READY;
  if (mHwInterrupt2->GetInterruptSourceState == NULL) return CR_UNSUPPORTED;
  if (mDispatchDepth != 0) {
    if (Enable) return CR_UNSUPPORTED;
    Entry=LeaseEntry(Lease);
    if (Entry == NULL || Entry != mDispatchEntry) return CR_BUSY;
  } else {
    Status=CrInterruptGuardAcquire(&Guard);
    if (Status != CR_SUCCESS) return LeaseExact(Status);
    Entry=LeaseEntry(Lease);
    if (Entry == NULL) { Status=CR_BUSY;goto Done; }
  }
  Status=Enable ? mHwInterrupt2->EnableInterruptSource(mHwInterrupt2,Entry->Source) :
    mHwInterrupt2->DisableInterruptSource(mHwInterrupt2,Entry->Source);
  if (Status != CR_SUCCESS) { Status=LeaseExact(Status);goto Done; }
  Status=mHwInterrupt2->GetInterruptSourceState(mHwInterrupt2,Entry->Source,&Enabled);
  if (Status == CR_SUCCESS && Enabled != Enable) Status=CR_DEVICE_ERROR;
  Status=LeaseExact(Status);
Done:
  if (Guard.Active) CrInterruptGuardRelease(&Guard);
  return Status;
}
CR_STATUS CrMaskInterrupt(CONST CR_INTERRUPT_LEASE *Lease) { return SetLeaseMask(Lease,FALSE); }
CR_STATUS CrUnmaskInterrupt(CONST CR_INTERRUPT_LEASE *Lease) { return SetLeaseMask(Lease,TRUE); }

CR_STATUS CrRegisterInterruptLeased(CR_INTERRUPT_CONFIG *Config,CR_INTERRUPT_LEASE *Out) {
  CR_INTERRUPT_GUARD Guard;CR_STATUS Status,LeaseStatus;
  if(Config==NULL||Out==NULL||Config->Handler==NULL)return CR_INVALID_PARAMETER;
  UINTN A=(UINTN)Config,B=(UINTN)Out;
  if(A>MAX_UINTN-sizeof(*Config)||B>MAX_UINTN-sizeof(*Out)||
     (A<B+sizeof(*Out)&&B<A+sizeof(*Config)))return CR_INVALID_PARAMETER;
  Status=CrInterruptGuardAcquire(&Guard);if(Status!=CR_SUCCESS)return LeaseExact(Status);
  if(mHwInterrupt2->GetInterruptSourceState==NULL){Status=CR_UNSUPPORTED;goto Done;}
  Status=LeaseExact(RegisterInterruptLocked(Config));
  if(CrInterruptIsRegistered(Config)){
    LeaseStatus=CrInterruptGetLease(Config,Out);
    if(LeaseStatus!=CR_SUCCESS){
      Status=LeaseExact(LeaseStatus);
      CR_STATUS Cleanup=UnregisterInterruptLocked(Config);
      if(Cleanup!=CR_SUCCESS)Status=LeaseExact(Cleanup);
    }
  }
Done:
  CrInterruptGuardRelease(&Guard);return Status;
}
