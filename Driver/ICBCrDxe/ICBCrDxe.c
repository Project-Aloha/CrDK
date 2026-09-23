/** @file
 *  Adapter from Crane interconnect paths to Qualcomm's native NPA ICB voter.
 *
 *  Waipio's packaged ICBDxe owns BCM aggregation and RPMh programming. This
 *  driver creates independent /icb/arbiter clients so Crane votes compose with
 *  the other firmware clients instead of overwriting their DRV state.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFINpa.h>

#define ICB_NPA_RESOURCE_NAME    "/icb/arbiter"
#define ICB_NPA_REQUEST_TYPE_3   0x4U
#define ICB_NPA_BANDWIDTH_SCALE  1000ULL

typedef struct {
  UINT32  Master;
  UINT32  Slave;
} ICB_NPA_MASTER_SLAVE;

typedef struct {
  UINT64  InstantaneousBandwidth;
  UINT64  AverageBandwidth;
  UINT32  LatencyNs;
  UINT32  Reserved;
} ICB_NPA_REQUEST_TYPE3;

typedef struct {
  UINT32                 Type;
  UINT32                 Reserved;
  ICB_NPA_REQUEST_TYPE3  Data;
} ICB_NPA_REQUEST;

typedef struct {
  CONST InterconnectTargetNativeRoute  *Route;
  npa_client_handle                     Client;
  UINT64                                AverageBandwidth;
  UINT64                                PeakBandwidth;
  BOOLEAN                               InUse;
} ICB_NPA_PATH;

STATIC_ASSERT (sizeof (ICB_NPA_MASTER_SLAVE) == 8, "ICB route ABI changed");
STATIC_ASSERT (sizeof (ICB_NPA_REQUEST) == 32, "ICB request ABI changed");

STATIC EFI_NPA_PROTOCOL                 *mNpa;
STATIC CONST InterconnectTargetContext  *mTarget;
STATIC ICB_NPA_PATH                      mPaths[INTERCONNECT_MAX_PATHS];
STATIC EFI_LOCK                          mPathLock;
STATIC BOOLEAN                           mInitialized;

STATIC
EFI_STATUS
ScaleBandwidth (
  IN  UINT64  Bandwidth,
  OUT UINT64 *ScaledBandwidth
  )
{
  if ((ScaledBandwidth == NULL) ||
      (Bandwidth > (MAX_UINT64 / ICB_NPA_BANDWIDTH_SCALE))) {
    return EFI_INVALID_PARAMETER;
  }

  *ScaledBandwidth = Bandwidth * ICB_NPA_BANDWIDTH_SCALE;
  return EFI_SUCCESS;
}

STATIC
CONST InterconnectTargetNativeRoute *
FindNativeRoute (
  IN CONST CHAR8  *ProviderCompatible,
  IN UINT32        SourceId,
  IN UINT32        DestinationId
  )
{
  UINT16  Index;

  if ((mTarget == NULL) || (ProviderCompatible == NULL) ||
      (mTarget->NativeRoutes == NULL)) {
    return NULL;
  }

  for (Index = 0; Index < mTarget->NativeRouteCount; Index++) {
    CONST InterconnectTargetNativeRoute  *Route;

    Route = &mTarget->NativeRoutes[Index];
    if ((Route->ProviderCompatible != NULL) &&
        (AsciiStrCmp (Route->ProviderCompatible, ProviderCompatible) == 0) &&
        (Route->SourceId == SourceId) &&
        (Route->DestinationId == DestinationId)) {
      return Route;
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
ValidateTargetRoutes (
  IN CONST InterconnectTargetContext  *Target
  )
{
  UINT16  Index;
  UINT16  Other;

  if ((Target == NULL) || (Target->NativeRoutes == NULL) ||
      (Target->NativeRouteCount == 0) ||
      (Target->NativeRouteCount > INTERCONNECT_MAX_PATHS)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < Target->NativeRouteCount; Index++) {
    CONST InterconnectTargetNativeRoute  *Route;

    Route = &Target->NativeRoutes[Index];
    if ((Route->ProviderCompatible == NULL) || (Route->ClientName == NULL)) {
      return EFI_COMPROMISED_DATA;
    }

    for (Other = 0; Other < Index; Other++) {
      CONST InterconnectTargetNativeRoute  *Candidate;

      Candidate = &Target->NativeRoutes[Other];
      if ((Route->SourceId == Candidate->SourceId) &&
          (Route->DestinationId == Candidate->DestinationId) &&
          (AsciiStrCmp (
             Route->ProviderCompatible,
             Candidate->ProviderCompatible
             ) == 0)) {
        return EFI_COMPROMISED_DATA;
      }
    }
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
IssueBandwidth (
  IN ICB_NPA_PATH  *Path,
  IN UINT64         AverageBandwidth,
  IN UINT64         PeakBandwidth
  )
{
  ICB_NPA_REQUEST  Request;
  EFI_STATUS       Status;

  if ((Path == NULL) || !Path->InUse || (Path->Client == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Request, sizeof (Request));
  Request.Type = ICB_NPA_REQUEST_TYPE_3;
  Status = ScaleBandwidth (
             PeakBandwidth,
             &Request.Data.InstantaneousBandwidth
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = ScaleBandwidth (
             AverageBandwidth,
             &Request.Data.AverageBandwidth
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = mNpa->IssueVectorRequest (
                   Path->Client,
                   sizeof (Request) / sizeof (npa_resource_state),
                   (npa_resource_state *)&Request
                   );
  if (!EFI_ERROR (Status)) {
    Path->AverageBandwidth = AverageBandwidth;
    Path->PeakBandwidth    = PeakBandwidth;
  }

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolAcquirePath (
  IN EFI_INTERCONNECT_CR_PROTOCOL  *This,
  IN CONST CHAR8                   *ProviderCompatible,
  IN UINT32                         SourceId,
  IN UINT32                         DestinationId,
  OUT INTERCONNECT_PATH_HANDLE     *Path
  )
{
  CONST InterconnectTargetNativeRoute  *Route;
  ICB_NPA_MASTER_SLAVE                  Pair;
  npa_client_handle                     Client;
  EFI_STATUS                            Status;
  UINT32                                Index;

  (VOID)This;
  if (!mInitialized) {
    return EFI_NOT_READY;
  }

  if ((ProviderCompatible == NULL) || (Path == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *Path = INTERCONNECT_INVALID_PATH;
  Route = FindNativeRoute (ProviderCompatible, SourceId, DestinationId);
  if (Route == NULL) {
    return EFI_NOT_FOUND;
  }

  EfiAcquireLock (&mPathLock);
  for (Index = 0; Index < INTERCONNECT_MAX_PATHS; Index++) {
    if (!mPaths[Index].InUse) {
      break;
    }
  }

  if (Index == INTERCONNECT_MAX_PATHS) {
    EfiReleaseLock (&mPathLock);
    return EFI_OUT_OF_RESOURCES;
  }

  Pair.Master = Route->MasterId;
  Pair.Slave  = Route->SlaveId;
  Client      = NULL;
  Status = mNpa->CreateSyncClientEx (
                   ICB_NPA_RESOURCE_NAME,
                   Route->ClientName,
                   NPA_CLIENT_VECTOR,
                   sizeof (Pair),
                   &Pair,
                   &Client
                   );
  if (EFI_ERROR (Status) || (Client == NULL)) {
    EfiReleaseLock (&mPathLock);
    return EFI_ERROR (Status) ? Status : EFI_OUT_OF_RESOURCES;
  }

  mPaths[Index].Route             = Route;
  mPaths[Index].Client            = Client;
  mPaths[Index].AverageBandwidth  = 0;
  mPaths[Index].PeakBandwidth     = 0;
  mPaths[Index].InUse             = TRUE;
  *Path = Index + 1;
  EfiReleaseLock (&mPathLock);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetBandwidth (
  IN EFI_INTERCONNECT_CR_PROTOCOL  *This,
  IN INTERCONNECT_PATH_HANDLE       Path,
  IN UINT64                         AverageBandwidth,
  IN UINT64                         PeakBandwidth
  )
{
  EFI_STATUS  Status;

  (VOID)This;
  if (!mInitialized) {
    return EFI_NOT_READY;
  }

  if ((Path == INTERCONNECT_INVALID_PATH) ||
      (Path > INTERCONNECT_MAX_PATHS) ||
      (PeakBandwidth < AverageBandwidth) ||
      (AverageBandwidth > (MAX_UINT64 / ICB_NPA_BANDWIDTH_SCALE)) ||
      (PeakBandwidth > (MAX_UINT64 / ICB_NPA_BANDWIDTH_SCALE))) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mPathLock);
  Status = IssueBandwidth (
             &mPaths[Path - 1],
             AverageBandwidth,
             PeakBandwidth
             );
  EfiReleaseLock (&mPathLock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
ProtocolReleasePath (
  IN EFI_INTERCONNECT_CR_PROTOCOL  *This,
  IN INTERCONNECT_PATH_HANDLE       Path
  )
{
  ICB_NPA_PATH  *Runtime;
  EFI_STATUS     Status;

  (VOID)This;
  if (!mInitialized) {
    return EFI_NOT_READY;
  }

  if ((Path == INTERCONNECT_INVALID_PATH) ||
      (Path > INTERCONNECT_MAX_PATHS)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mPathLock);
  Runtime = &mPaths[Path - 1];
  if (!Runtime->InUse || (Runtime->Client == NULL)) {
    EfiReleaseLock (&mPathLock);
    return EFI_NOT_FOUND;
  }

  /* CompleteRequest is fire-and-forget in the packaged NPA implementation.
   * Submit an explicit zero vector first so cold-init rollback cannot return
   * while the hardware vote is still pending. */
  Status = IssueBandwidth (Runtime, 0, 0);
  if (!EFI_ERROR (Status)) {
    Status = mNpa->DestroyClient (Runtime->Client);
  }

  if (!EFI_ERROR (Status)) {
    ZeroMem (Runtime, sizeof (*Runtime));
  }

  EfiReleaseLock (&mPathLock);
  return Status;
}

STATIC EFI_INTERCONNECT_CR_PROTOCOL  mInterconnectProtocol = {
  .Revision     = EFI_INTERCONNECT_CR_PROTOCOL_REVISION,
  .AcquirePath  = ProtocolAcquirePath,
  .SetBandwidth = ProtocolSetBandwidth,
  .ReleasePath  = ProtocolReleasePath,
};

EFI_STATUS
EFIAPI
InterconnectEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS        Status;
  npa_query_status  QueryStatus;

  (VOID)SystemTable;
  if (mInitialized) {
    return EFI_ALREADY_STARTED;
  }

  Status = gBS->LocateProtocol (
                  &gEfiNpaProtocolGuid,
                  NULL,
                  (VOID **)&mNpa
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((mNpa->Revision < EFI_NPA_PROTOCOL_VER_LEGACY) ||
      (mNpa->CreateSyncClientEx == NULL) ||
      (mNpa->IssueVectorRequest == NULL) ||
      (mNpa->DestroyClient == NULL) ||
      (mNpa->QueryResourceAvailable == NULL)) {
    mNpa = NULL;
    return EFI_UNSUPPORTED;
  }

  QueryStatus = NPA_QUERY_NO_VALUE;
  Status = mNpa->QueryResourceAvailable (
                   ICB_NPA_RESOURCE_NAME,
                   &QueryStatus
                   );
  if (EFI_ERROR (Status)) {
    mNpa = NULL;
    return Status;
  }

  if (QueryStatus != NPA_QUERY_SUCCESS) {
    mNpa = NULL;
    return EFI_NOT_READY;
  }

  mTarget = CrDalGetInterconnectContext ();
  Status  = ValidateTargetRoutes (mTarget);
  if (EFI_ERROR (Status)) {
    mNpa    = NULL;
    mTarget = NULL;
    return Status;
  }

  ZeroMem (mPaths, sizeof (mPaths));
  EfiInitializeLock (&mPathLock, TPL_NOTIFY);
  mInitialized = TRUE;
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiInterconnectCrProtocolGuid,
                  &mInterconnectProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    mInitialized = FALSE;
    mNpa         = NULL;
    mTarget      = NULL;
    return Status;
  }

  DEBUG ((DEBUG_INFO, "ICBCrDxe: using native NPA /icb/arbiter clients\n"));
  return EFI_SUCCESS;
}
