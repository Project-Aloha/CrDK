/* Native display rendezvous and explicit SMMU retirement ordering. MIT */
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Library/DebugLib.h>
#include <Library/MdssHandoffUefiLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC CR_MDSS_UEFI_HANDOFF mHandoff;
STATIC EFI_SMMU_CR_PROTOCOL *mSmmu;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mAcpiEvent, mEndEvent, mReadyEvent, mExitEvent;
STATIC VOID *mAcpiRegistration;
STATIC BOOLEAN mActive, mPreparing, mPrepareObserver, mCompleteObserver;
STATIC EFI_MDSS_HANDOFF_PROTOCOL mProtocol;

STATIC EFI_STATUS EFIAPI
SetParticipant (EFI_MDSS_HANDOFF_PROTOCOL *This, UINT32 Owner, VOID *Context,
                EFI_MDSS_HANDOFF_FREEZE Freeze, EFI_MDSS_HANDOFF_SNAPSHOT Snapshot)
{
  EFI_STATUS Status;
  EFI_TPL Tpl;
  if (This != &mProtocol) return EFI_INVALID_PARAMETER;
  Tpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = mActive ? CrMdssUefiSetParticipant (&mHandoff, Owner, Context, Freeze, Snapshot) : EFI_NOT_READY;
  gBS->RestoreTPL (Tpl);
  return Status;
}

STATIC VOID EFIAPI
Prepare (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Event; (VOID)Context;
  if (!mActive || mPreparing || mHandoff.Frozen || mHandoff.Freezing || mHandoff.Installed) return;
  mPreparing = TRUE;
  Status = CrMdssUefiPrepare (&mHandoff);
  mPreparing = FALSE;
  DEBUG ((EFI_ERROR(Status) ? DEBUG_WARN : DEBUG_INFO,
    "MDSH: reserved record=0x%lx generation=%Lu prepare: %r\n",
    mHandoff.Record, mHandoff.Generation, Status));
}

STATIC EFI_STATUS EFIAPI
BeforeRevoke (VOID *Context)
{
  EFI_STATUS Status;
  if (Context != &mHandoff) return EFI_INVALID_PARAMETER;
  if (!mActive) return EFI_SUCCESS;
  /* An interrupted prepare must never be treated as a complete publication.
     Still close every producer before SMMU revokes transient DMA. */
  Status = CrMdssUefiFreeze (&mHandoff);
  if (mPreparing && Status == EFI_SUCCESS) {
    Status = EFI_NOT_READY;
    mHandoff.FreezeStatus = Status;
  }
  return Status;
}

STATIC VOID TryPublish (VOID)
{
  if (!mActive || mHandoff.Published || !mHandoff.SmmuComplete) return;
  mHandoff.PublishStatus = CrMdssUefiPublish (&mHandoff, mSmmu);
  DEBUG ((EFI_ERROR(mHandoff.PublishStatus) ? DEBUG_ERROR : DEBUG_INFO,
    "MDSH: final freeze=%r publish=%r\n", mHandoff.FreezeStatus, mHandoff.PublishStatus));
}

STATIC VOID EFIAPI
AfterRevoke (VOID *Context, EFI_STATUS Status)
{
  if (Context != &mHandoff || !mActive) return;
  /* Global SMMU failure does not substitute for per-map evidence. Publish
     immutable diagnostic states; the portable claim validator rejects them. */
  mHandoff.SmmuComplete = TRUE;
  DEBUG ((DEBUG_INFO, "MDSH: SMMU pass complete: %r\n", Status));
  TryPublish ();
}

STATIC VOID EFIAPI
OnExit (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  BeforeRevoke (&mHandoff);
  TryPublish ();
}

/* No unload entry: the ACPI record and callback rendezvous are permanent
   firmware services. Entry rollback releases callbacks before code removal. */
STATIC EFI_STATUS Rollback (VOID)
{
  EFI_STATUS Status;
  mActive = FALSE;
  if (mPrepareObserver) {
    Status = mSmmu->SetExitPrepareObserver(mSmmu,&mHandoff,NULL,NULL);
    if (Status != EFI_SUCCESS) return Status;
    mPrepareObserver = FALSE;
  }
  if (mCompleteObserver) {
    Status = mSmmu->SetExitObserver(mSmmu,&mHandoff,NULL,NULL);
    if (Status != EFI_SUCCESS) return Status;
    mCompleteObserver = FALSE;
  }
  EFI_EVENT *Events[] = {&mAcpiEvent,&mEndEvent,&mReadyEvent,&mExitEvent};
  for (UINTN I=0; I<ARRAY_SIZE(Events); ++I) if (*Events[I]) {
    Status = gBS->CloseEvent(*Events[I]);
    if (Status != EFI_SUCCESS) return Status;
    *Events[I] = NULL;
  }
  return CrMdssUefiRelease(&mHandoff);
}

EFI_STATUS EFIAPI
MdssHandoffCrDxeEntryPoint (EFI_HANDLE Image, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status, Cleanup;
  (VOID)Image; (VOID)SystemTable;
  if (mActive || mPrepareObserver || mCompleteObserver || mAcpiEvent || mExitEvent || mEndEvent || mReadyEvent)
    return EFI_ALREADY_STARTED;
  Status = gBS->LocateProtocol(&gEfiSmmuCrProtocolGuid,NULL,(VOID**)&mSmmu);
  if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
  if (!mSmmu || mSmmu->Revision < EFI_SMMU_CR_EXIT_REVOCATION_REVISION ||
      !mSmmu->SetExitPrepareObserver || !mSmmu->SetExitObserver ||
      !mSmmu->QueryExitMapping || !mSmmu->QueryExitRevokedMapping) return EFI_UNSUPPORTED;
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&mAcpiEvent);
  if (Status != EFI_SUCCESS) goto Failed;
  Status = gBS->RegisterProtocolNotify(&gEfiAcpiTableProtocolGuid,mAcpiEvent,&mAcpiRegistration);
  if (Status != EFI_SUCCESS) goto Failed;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&gEfiEndOfDxeEventGroupGuid,&mEndEvent);
  if (Status != EFI_SUCCESS) goto Failed;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&gEfiEventReadyToBootGuid,&mReadyEvent);
  if (Status != EFI_SUCCESS) goto Failed;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,OnExit,NULL,&gEfiEventExitBootServicesGuid,&mExitEvent);
  if (Status != EFI_SUCCESS) goto Failed;
  Status = mSmmu->SetExitPrepareObserver(mSmmu,&mHandoff,BeforeRevoke,&mHandoff);
  if (Status != EFI_SUCCESS) goto Failed;
  mPrepareObserver = TRUE;
  Status = mSmmu->SetExitObserver(mSmmu,&mHandoff,AfterRevoke,&mHandoff);
  if (Status != EFI_SUCCESS) goto Failed;
  mCompleteObserver = TRUE;
  mProtocol.Revision = EFI_MDSS_HANDOFF_PROTOCOL_REVISION;
  mProtocol.SetParticipant = SetParticipant;
  mActive = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces(&mHandle,&gEfiMdssHandoffProtocolGuid,&mProtocol,NULL);
  if (Status != EFI_SUCCESS) goto Failed;
  /* Monotonic count and ACPI may be dispatched later than this early
     rendezvous. Protocol notify/EndOfDxe/ReadyToBoot retry storage only. */
  Prepare(NULL,NULL);
  return EFI_SUCCESS;
Failed:
  Cleanup = Rollback();
  if (Cleanup != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR,"MDSH: entry=%r rollback=%r; retaining callbacks\n",Status,Cleanup));
    return EFI_SUCCESS;
  }
  return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
}
