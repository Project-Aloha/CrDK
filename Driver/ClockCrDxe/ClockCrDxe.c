/** @file
  Crane clock adapter backed by Qualcomm's native ClockDxe protocol.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIMuClockProtocol.h>

#define CLOCK_CR_MAX_CLOCK_VOTES         64U
#define CLOCK_CR_MAX_POWER_DOMAIN_VOTES  8U

typedef struct {
  UINTN   NativeId;
  UINT32  References;
  UINT32  RateHz;
  BOOLEAN Uncertain;
  EFI_STATUS FailureStatus;
} CLOCK_CR_VOTE;

STATIC EFI_CLOCK_PROTOCOL *mNativeClock;
STATIC CLOCK_CR_VOTE       mClockVotes[CLOCK_CR_MAX_CLOCK_VOTES];
STATIC CLOCK_CR_VOTE       mPowerDomainVotes[CLOCK_CR_MAX_POWER_DOMAIN_VOTES];
STATIC EFI_LOCK            mVoteLock = EFI_INITIALIZE_LOCK_VARIABLE (TPL_NOTIFY);

STATIC
EFI_STATUS
ResolveNativeResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  EFI_STATUS Status;

  if ((Controller == NULL) || (Id == NULL) || (NativeId == NULL) ||
      (Flags == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CrDalClockResolveResource (
             Controller,
             Id,
             Kind,
             NativeId,
             Flags
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((*NativeId == NULL) ||
      ((*Flags & ~CR_DAL_CLOCK_RESOURCE_VALID_FLAGS) != 0)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

STATIC
CLOCK_CR_VOTE *
FindVote (
  IN CLOCK_CR_VOTE *Votes,
  IN UINTN          VoteCount,
  IN UINTN          NativeId,
  IN BOOLEAN        Allocate
  )
{
  CLOCK_CR_VOTE *FreeVote;
  UINTN          Index;

  FreeVote = NULL;
  for (Index = 0; Index < VoteCount; Index++) {
    if ((Votes[Index].References != 0) &&
        (Votes[Index].NativeId == NativeId)) {
      return &Votes[Index];
    }
    if ((FreeVote == NULL) && (Votes[Index].References == 0)) {
      FreeVote = &Votes[Index];
    }
  }
  return Allocate ? FreeVote : NULL;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetClock (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN UINT64                 RateHz,
  IN BOOLEAN                Enable
  )
{
  CONST CHAR8    *NativeName;
  CLOCK_CR_VOTE  *Vote;
  EFI_STATUS      EnableStatus;
  EFI_STATUS      Status;
  UINT32          Flags;
  UINT32          ResultRate;
  UINTN           NativeId;
  BOOLEAN         IsEnabled;
  BOOLEAN         WasEnabled;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourceClock,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((RateHz > MAX_UINT32) &&
      ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiAcquireLock (&mVoteLock);
  Vote = FindVote (
           mClockVotes,
           CLOCK_CR_MAX_CLOCK_VOTES,
           NativeId,
           Enable
           );
  if (!Enable) {
    if (Vote == NULL) {
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    if (Vote->Uncertain) {
      Status = mNativeClock->IsClockEnabled (
                               mNativeClock,
                               NativeId,
                               &IsEnabled
                               );
      if (!EFI_ERROR (Status) && !IsEnabled) {
        Vote->References = 0;
        Vote->RateHz = 0;
        Vote->Uncertain = FALSE;
        Vote->FailureStatus = EFI_SUCCESS;
        EfiReleaseLock (&mVoteLock);
        return EFI_SUCCESS;
      }

      Status = Vote->FailureStatus;
      EfiReleaseLock (&mVoteLock);
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
    if (Vote->References > 1) {
      Vote->References--;
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    Status = mNativeClock->DisableClock (mNativeClock, NativeId);
    if (!EFI_ERROR (Status)) {
      Vote->References = 0;
      Vote->RateHz = 0;
    }
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  if (Vote == NULL) {
    EfiReleaseLock (&mVoteLock);
    return EFI_OUT_OF_RESOURCES;
  }
  if (Vote->References != 0) {
    if (Vote->Uncertain) {
      Status = Vote->FailureStatus;
      EfiReleaseLock (&mVoteLock);
      return EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
    }
    if (Vote->References == MAX_UINT32) {
      EfiReleaseLock (&mVoteLock);
      return EFI_OUT_OF_RESOURCES;
    }
    if (((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) &&
        (RateHz != 0) && (Vote->RateHz != (UINT32)RateHz)) {
      EfiReleaseLock (&mVoteLock);
      return EFI_ACCESS_DENIED;
    }
    Vote->References++;
    EfiReleaseLock (&mVoteLock);
    return EFI_SUCCESS;
  }
  if ((RateHz != 0) &&
      ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0)) {
    ResultRate = 0;
    Status = mNativeClock->SetClockFreqHz (
                             mNativeClock,
                             NativeId,
                             (UINT32)RateHz,
                             EfiClockFrequencyHzExact,
                             &ResultRate
                             );
    if (EFI_ERROR (Status)) {
      EfiReleaseLock (&mVoteLock);
      return Status;
    }
    if (ResultRate != (UINT32)RateHz) {
      EfiReleaseLock (&mVoteLock);
      return EFI_PROTOCOL_ERROR;
    }
  }
  if ((Flags & CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE) != 0) {
    Status = mNativeClock->SelectExternalSource (
                             mNativeClock,
                             NativeId,
                             0,
                             0,
                             0,
                             0,
                             0,
                             0
                             );
    if (EFI_ERROR (Status)) {
      EfiReleaseLock (&mVoteLock);
      return Status;
    }
  }
  Status = mNativeClock->IsClockEnabled (
                           mNativeClock,
                           NativeId,
                           &WasEnabled
                           );
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  EnableStatus = mNativeClock->EnableClock (mNativeClock, NativeId);
  if (EFI_ERROR (EnableStatus)) {
    Status = mNativeClock->IsClockEnabled (
                             mNativeClock,
                             NativeId,
                             &IsEnabled
                             );
    if (!WasEnabled && (EFI_ERROR (Status) || IsEnabled)) {
      /*
       * A failed synchronous enable normally carries no native reference.
       * If its observable state changed, retain an uncertain local owner so
       * rollback cannot silently disable a pre-existing or unowned vote.
       */
      Vote->NativeId = NativeId;
      Vote->References = 1;
      Vote->RateHz = ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) ?
                     (UINT32)RateHz : 0;
      Vote->Uncertain = TRUE;
      Vote->FailureStatus = EnableStatus;
    }
    EfiReleaseLock (&mVoteLock);
    return EnableStatus;
  }

  Vote->NativeId   = NativeId;
  Vote->References = 1;
  Vote->RateHz = ((Flags & CR_DAL_CLOCK_RESOURCE_NO_RATE) == 0) ?
                 (UINT32)RateHz : 0;
  Vote->Uncertain = FALSE;
  Vote->FailureStatus = EFI_SUCCESS;
  EfiReleaseLock (&mVoteLock);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetGdsc (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN BOOLEAN                Enable
  )
{
  CONST CHAR8    *NativeName;
  CLOCK_CR_VOTE  *Vote;
  EFI_STATUS      Status;
  UINT32          Flags;
  UINTN           NativeId;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourcePowerDomain,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mNativeClock->GetClockPowerDomainID (
                           mNativeClock,
                           NativeName,
                           &NativeId
                           );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiAcquireLock (&mVoteLock);
  Vote = FindVote (
           mPowerDomainVotes,
           CLOCK_CR_MAX_POWER_DOMAIN_VOTES,
           NativeId,
           Enable
           );
  if (!Enable) {
    if (Vote == NULL) {
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    if (Vote->References > 1) {
      Vote->References--;
      EfiReleaseLock (&mVoteLock);
      return EFI_SUCCESS;
    }
    Status = mNativeClock->DisableClockPowerDomain (mNativeClock, NativeId);
    if (!EFI_ERROR (Status)) {
      Vote->References = 0;
    }
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  if (Vote == NULL) {
    EfiReleaseLock (&mVoteLock);
    return EFI_OUT_OF_RESOURCES;
  }
  if (Vote->References != 0) {
    if (Vote->References == MAX_UINT32) {
      EfiReleaseLock (&mVoteLock);
      return EFI_OUT_OF_RESOURCES;
    }
    Vote->References++;
    EfiReleaseLock (&mVoteLock);
    return EFI_SUCCESS;
  }
  Status = mNativeClock->EnableClockPowerDomain (mNativeClock, NativeId);
  if (EFI_ERROR (Status)) {
    EfiReleaseLock (&mVoteLock);
    return Status;
  }

  Vote->NativeId   = NativeId;
  Vote->References = 1;
  Vote->RateHz = 0;
  Vote->Uncertain = FALSE;
  Vote->FailureStatus = EFI_SUCCESS;
  EfiReleaseLock (&mVoteLock);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetReset (
  IN EFI_CLOCK_CR_PROTOCOL *This,
  IN CONST CHAR8           *Controller,
  IN CONST CHAR8           *Id,
  IN BOOLEAN                Assert
  )
{
  CONST CHAR8 *NativeName;
  EFI_STATUS   Status;
  UINT32       Flags;
  UINTN        NativeId;

  if ((This == NULL) || (Controller == NULL) || (Id == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (mNativeClock == NULL) {
    return EFI_NOT_READY;
  }

  Status = ResolveNativeResource (
             Controller,
             Id,
             CrDalClockResourceReset,
             &NativeName,
             &Flags
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mNativeClock->GetClockID (mNativeClock, NativeName, &NativeId);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  EfiAcquireLock (&mVoteLock);
  Status = mNativeClock->ResetClock (
                           mNativeClock,
                           NativeId,
                           Assert ? EfiClockResetAssert : EfiClockResetDeassert
                           );
  EfiReleaseLock (&mVoteLock);
  return Status;
}

STATIC EFI_CLOCK_CR_PROTOCOL mClockCrProtocol = {
  .Revision = EFI_CLOCK_CR_PROTOCOL_REVISION,
  .SetClock = ProtocolSetClock,
  .SetGdsc  = ProtocolSetGdsc,
  .SetReset = ProtocolSetReset,
};

EFI_STATUS
EFIAPI
ClockEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol (
                  &gEfiClockProtocolGuid,
                  NULL,
                  (VOID **)&mNativeClock
                  );
  if (EFI_ERROR (Status)) {
    mNativeClock = NULL;
    return Status;
  }
  if ((mNativeClock == NULL) ||
      (mNativeClock->Version < EFI_CLOCK_PROTOCOL_REVISION) ||
      (mNativeClock->GetClockID == NULL) ||
      (mNativeClock->EnableClock == NULL) ||
      (mNativeClock->DisableClock == NULL) ||
      (mNativeClock->IsClockEnabled == NULL) ||
      (mNativeClock->SetClockFreqHz == NULL) ||
      (mNativeClock->SelectExternalSource == NULL) ||
      (mNativeClock->GetClockPowerDomainID == NULL) ||
      (mNativeClock->EnableClockPowerDomain == NULL) ||
      (mNativeClock->DisableClockPowerDomain == NULL) ||
      (mNativeClock->ResetClock == NULL)) {
    mNativeClock = NULL;
    return EFI_INCOMPATIBLE_VERSION;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiClockCrProtocolGuid,
                  &mClockCrProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ClockCrDxe: protocol installation failed: %r\n", Status));
    mNativeClock = NULL;
  }
  return Status;
}
