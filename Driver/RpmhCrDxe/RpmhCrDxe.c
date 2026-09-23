/** @file
 *  Crane PMIC rail adapter for the OEM Qualcomm NPA/PRM stack.
 *
 *  Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *  Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/CrDalLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <oskal/cr_debug.h>

#include <Protocol/EFINpa.h>
#include <Protocol/EFIRpmhCrProtocol.h>

#define RPMH_CR_RESOURCE_NAME_MAX  64U
#define RPMH_CR_VOLTAGE_SUFFIX     "/mV"
#define RPMH_CR_MODE_SUFFIX        "/mode"
#define RPMH_CR_ENABLE_SUFFIX      "/en"

typedef struct {
  CONST CHAR8       *Name;
  CHAR8              VoltageResource[RPMH_CR_RESOURCE_NAME_MAX];
  CHAR8              ModeResource[RPMH_CR_RESOURCE_NAME_MAX];
  CHAR8              EnableResource[RPMH_CR_RESOURCE_NAME_MAX];
  npa_client_handle  VoltageClient;
  npa_client_handle  ModeClient;
  npa_client_handle  EnableClient;
  BOOLEAN            VoltageRequested;
  BOOLEAN            ModeRequested;
  BOOLEAN            EnableRequested;
  UINTN              ReferenceCount;
} RPMH_CR_RAIL;

STATIC EFI_NPA_PROTOCOL  *mNpa;
STATIC RPMH_CR_RAIL       mRails[PCIE_MAX_RESOURCES];
STATIC UINTN              mRailCount;
STATIC EFI_LOCK           mRailLock;

STATIC CONST CHAR8  mVoltageClientName[] = "CraneRpmhVoltage";
STATIC CONST CHAR8  mModeClientName[]    = "CraneRpmhMode";
STATIC CONST CHAR8  mEnableClientName[]  = "CraneRpmhEnable";

STATIC
BOOLEAN
AsciiStringsEqual (
  IN CONST CHAR8  *Left,
  IN CONST CHAR8  *Right
  )
{
  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  while ((*Left != '\0') && (*Left == *Right)) {
    Left++;
    Right++;
  }

  return *Left == *Right;
}

STATIC
EFI_STATUS
BuildResourceName (
  OUT CHAR8       *Destination,
  IN UINTN         DestinationSize,
  IN CONST CHAR8  *RailName,
  IN CONST CHAR8  *Suffix
  )
{
  STATIC CONST CHAR8  Prefix[] = "/pm/";
  UINTN               Index;
  UINTN               Length;

  if ((Destination == NULL) || (DestinationSize == 0) ||
      (RailName == NULL) || (*RailName == '\0') || (Suffix == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Length = 0;
  for (Index = 0; Prefix[Index] != '\0'; Index++) {
    if (Length + 1 >= DestinationSize) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Destination[Length++] = Prefix[Index];
  }

  for (Index = 0; RailName[Index] != '\0'; Index++) {
    if (RailName[Index] == '/') {
      return EFI_COMPROMISED_DATA;
    }

    if (Length + 1 >= DestinationSize) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Destination[Length++] = RailName[Index];
  }

  for (Index = 0; Suffix[Index] != '\0'; Index++) {
    if (Length + 1 >= DestinationSize) {
      return EFI_BAD_BUFFER_SIZE;
    }

    Destination[Length++] = Suffix[Index];
  }

  Destination[Length] = '\0';
  return EFI_SUCCESS;
}

STATIC
RPMH_CR_RAIL *
FindRail (
  IN CONST CHAR8  *Name
  )
{
  UINTN  Index;

  for (Index = 0; Index < mRailCount; Index++) {
    if (AsciiStringsEqual (mRails[Index].Name, Name)) {
      return &mRails[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
InitializeRailDescriptions (
  IN CONST PcieTargetContext  *Target
  )
{
  CONST PcieTargetSupply  *Supply;
  RPMH_CR_RAIL            *Rail;
  EFI_STATUS               Status;
  UINTN                    Index;

  mRailCount = 0;
  if ((Target == NULL) || (Target->SupplyCount == 0) ||
      (Target->Supplies == NULL) || (Target->SupplyCount > PCIE_MAX_RESOURCES)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < Target->SupplyCount; Index++) {
    Supply = &Target->Supplies[Index];
    if ((Supply->Controller == NULL) ||
        !AsciiStringsEqual (Supply->Controller, "rpmh")) {
      continue;
    }

    if ((Supply->Id == NULL) || (*Supply->Id == '\0')) {
      return EFI_COMPROMISED_DATA;
    }

    if (FindRail (Supply->Id) != NULL) {
      continue;
    }

    if (mRailCount >= PCIE_MAX_RESOURCES) {
      return EFI_OUT_OF_RESOURCES;
    }

    Rail       = &mRails[mRailCount];
    Rail->Name = Supply->Id;
    Status     = BuildResourceName (
                   Rail->VoltageResource,
                   sizeof (Rail->VoltageResource),
                   Rail->Name,
                   RPMH_CR_VOLTAGE_SUFFIX
                   );
    if (!EFI_ERROR (Status)) {
      Status = BuildResourceName (
                 Rail->ModeResource,
                 sizeof (Rail->ModeResource),
                 Rail->Name,
                 RPMH_CR_MODE_SUFFIX
                 );
    }

    if (!EFI_ERROR (Status)) {
      Status = BuildResourceName (
                 Rail->EnableResource,
                 sizeof (Rail->EnableResource),
                 Rail->Name,
                 RPMH_CR_ENABLE_SUFFIX
                 );
    }

    if (EFI_ERROR (Status)) {
      return Status;
    }

    mRailCount++;
  }

  return (mRailCount == 0) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC
EFI_STATUS
ValidateNpaProtocol (
  IN CONST EFI_NPA_PROTOCOL  *Npa
  )
{
  if ((Npa == NULL) ||
      ((Npa->Revision >> 16) !=
       (EFI_NPA_PROTOCOL_VER_WITH_DEINIT_SUPPORT >> 16)) ||
      (Npa->Revision < EFI_NPA_PROTOCOL_VER_WITH_DEINIT_SUPPORT) ||
      (Npa->CreateSyncClientEx == NULL) || (Npa->ScalarRequest == NULL) ||
      (Npa->DestroyClient == NULL)) {
    return EFI_INCOMPATIBLE_VERSION;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CreateClient (
  IN CONST CHAR8        *ResourceName,
  IN CONST CHAR8        *ClientName,
  OUT npa_client_handle *Client
  )
{
  EFI_STATUS  Status;

  *Client = NULL;
  Status  = mNpa->CreateSyncClientEx (
                    ResourceName,
                    ClientName,
                    NPA_CLIENT_REQUIRED,
                    0,
                    NULL,
                    Client
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (*Client == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC
EFI_STATUS
CreateRailClients (
  VOID
  )
{
  EFI_STATUS  Status;
  UINTN       Index;

  for (Index = 0; Index < mRailCount; Index++) {
    Status = CreateClient (
               mRails[Index].VoltageResource,
               mVoltageClientName,
               &mRails[Index].VoltageClient
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CreateClient (
               mRails[Index].ModeResource,
               mModeClientName,
               &mRails[Index].ModeClient
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = CreateClient (
               mRails[Index].EnableResource,
               mEnableClientName,
               &mRails[Index].EnableClient
               );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
DestroyClient (
  IN OUT npa_client_handle  *Client
  )
{
  EFI_STATUS  Status;

  if (*Client == NULL) {
    return EFI_SUCCESS;
  }

  Status = mNpa->DestroyClient (*Client);
  if (!EFI_ERROR (Status)) {
    *Client = NULL;
  }

  return Status;
}

STATIC
EFI_STATUS
DestroyRailClients (
  VOID
  )
{
  EFI_STATUS  FirstError;
  EFI_STATUS  Status;
  UINTN       Index;

  FirstError = EFI_SUCCESS;
  for (Index = mRailCount; Index > 0; Index--) {
    Status = DestroyClient (&mRails[Index - 1].EnableClient);
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }

    Status = DestroyClient (&mRails[Index - 1].ModeClient);
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }

    Status = DestroyClient (&mRails[Index - 1].VoltageClient);
    if (EFI_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }

  return FirstError;
}

STATIC
EFI_STATUS
IssueVote (
  IN npa_client_handle  Client,
  IN UINT32             State,
  IN OUT BOOLEAN       *Requested
  )
{
  EFI_STATUS  Status;

  if ((mNpa == NULL) || (Client == NULL) || (Requested == NULL)) {
    return EFI_NOT_READY;
  }

  if (State != 0) {
    /* A failed wrapper call may follow a hardware side effect.  Retain
       ownership until an explicit zero vote succeeds. */
    *Requested = TRUE;
  }

  Status = mNpa->ScalarRequest (Client, State);
  if (!EFI_ERROR (Status) && (State == 0)) {
    *Requested = FALSE;
  }

  return Status;
}

STATIC
EFI_STATUS
ReleaseRail (
  IN OUT RPMH_CR_RAIL  *Rail
  )
{
  EFI_STATUS  Status;

  if (Rail->ReferenceCount > 1) {
    Rail->ReferenceCount--;
    return EFI_SUCCESS;
  }

  if (Rail->ReferenceCount == 1) {
    Rail->ReferenceCount = 0;
  }

  if (Rail->EnableRequested) {
    Status = IssueVote (Rail->EnableClient, 0, &Rail->EnableRequested);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Rail->ModeRequested) {
    Status = IssueVote (Rail->ModeClient, 0, &Rail->ModeRequested);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  if (Rail->VoltageRequested) {
    return IssueVote (Rail->VoltageClient, 0, &Rail->VoltageRequested);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ProtocolRpmhWrite (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN RpmhTcsCmd            *TcsCmd,
  IN UINT32                 NumCmds
  )
{
  (VOID)This;
  (VOID)TcsCmd;
  (VOID)NumCmds;
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
ProtocolRpmhEnableVreg (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN BOOLEAN                Enable
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }

  if (!Enable) {
    Status = ReleaseRail (Rail);
    if (EFI_ERROR (Status)) {
      log_err ("RPMh: release rail %a failed: %r", Name, Status);
    }
    goto Exit;
  }

  if (Rail->ReferenceCount == MAX_UINTN) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }

  if (Rail->ReferenceCount != 0) {
    Rail->ReferenceCount++;
    Status = EFI_SUCCESS;
    goto Exit;
  }

  Status = IssueVote (Rail->EnableClient, 1, &Rail->EnableRequested);
  if (EFI_ERROR (Status)) {
    log_err ("RPMh: enable rail %a failed: %r", Name, Status);
  }
  if (!EFI_ERROR (Status)) {
    Rail->ReferenceCount = 1;
  }

Exit:
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregVoltage (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN CONST UINT32           VoltageMv
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((VoltageMv == 0) || (VoltageMv > 0xFFFFU)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    EfiReleaseLock (&mRailLock);
    return EFI_NOT_FOUND;
  }

  Status = IssueVote (
             Rail->VoltageClient,
             VoltageMv,
             &Rail->VoltageRequested
             );
  if (EFI_ERROR (Status)) {
    log_err ("RPMh: voltage rail %a (%u mV) failed: %r",
             Name, VoltageMv, Status);
  }
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregMode (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN CONST UINT8            Mode
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Mode != 2) && (Mode != 3) && (Mode != 4) &&
      (Mode != 6) && (Mode != 7)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    EfiReleaseLock (&mRailLock);
    return EFI_NOT_FOUND;
  }

  Status = IssueVote (Rail->ModeClient, Mode, &Rail->ModeRequested);
  if (EFI_ERROR (Status)) {
    log_err ("RPMh: mode rail %a (%u) failed: %r", Name, Mode, Status);
  }
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_RPMH_CR_PROTOCOL  gRpmhCrProtocol = {
  .Revision           = EFI_RPMH_CR_PROTOCOL_REVISION,
  .RpmhWrite          = ProtocolRpmhWrite,
  .RpmhEnableVreg     = ProtocolRpmhEnableVreg,
  .RpmhSetVregVoltage = ProtocolRpmhSetVregVoltage,
  .RpmhSetVregMode    = ProtocolRpmhSetVregMode,
};

EFI_STATUS
RpmhEntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST PcieTargetContext  *Target;
  EFI_STATUS                CleanupStatus;
  EFI_STATUS                Status;

  (VOID)SystemTable;
  if ((gBS == NULL) || (gBS->LocateProtocol == NULL) ||
      (gBS->InstallMultipleProtocolInterfaces == NULL)) {
    return EFI_NOT_READY;
  }

  Target = CrDalGetPcieContext ();
  if (Target == NULL) {
    return EFI_NOT_FOUND;
  }

  Status = InitializeRailDescriptions (Target);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gEfiNpaProtocolGuid,
                  NULL,
                  (VOID **)&mNpa
                  );
  if (EFI_ERROR (Status)) {
    mNpa = NULL;
    return Status;
  }

  Status = ValidateNpaProtocol (mNpa);
  if (EFI_ERROR (Status)) {
    mNpa = NULL;
    return Status;
  }

  EfiInitializeLock (&mRailLock, TPL_NOTIFY);
  Status = CreateRailClients ();
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiRpmhCrProtocolGuid,
                  &gRpmhCrProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  return EFI_SUCCESS;

Error:
  CleanupStatus = DestroyRailClients ();
  if (EFI_ERROR (CleanupStatus)) {
    /* NPA retains the immutable resource/client-name pointers.  If an OEM
       destroy callback fails, keep this image resident rather than leave an
       NPA client referring to unloaded storage. */
    log_err (
      "NPA client cleanup failed; retaining RPMh adapter image: 0x%lx",
      CleanupStatus
      );
    return EFI_SUCCESS;
  }

  mNpa       = NULL;
  mRailCount = 0;
  return Status;
}
