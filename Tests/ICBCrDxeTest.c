/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../Driver/ICBCrDxe/ICBCrDxe.c"

#define TEST_MAX_CALLS  64U

struct npa_client {
  UINTN  Id;
};

typedef struct {
  CHAR8              ResourceName[32];
  CHAR8              ClientName[32];
  npa_client_type     ClientType;
  UINT32              ClientValue;
  ICB_NPA_MASTER_SLAVE Pair;
  npa_client_handle   Handle;
} CREATE_CALL;

typedef struct {
  npa_client_handle  Handle;
  UINT32             ElementCount;
  ICB_NPA_REQUEST    Request;
} VECTOR_CALL;

STATIC EFI_BOOT_SERVICES  mBootServices;
EFI_BOOT_SERVICES         *gBS = &mBootServices;

EFI_GUID  gEfiNpaProtocolGuid = EFI_NPA_PROTOCOL_GUID;
EFI_GUID  gEfiInterconnectCrProtocolGuid = EFI_INTERCONNECT_CR_PROTOCOL_GUID;

STATIC EFI_NPA_PROTOCOL                 mNpaProtocol;
STATIC CONST InterconnectTargetContext *mDalTarget;
STATIC struct npa_client                mHandles[TEST_MAX_CALLS];
STATIC CREATE_CALL                      mCreateLog[TEST_MAX_CALLS];
STATIC VECTOR_CALL                      mVectorLog[TEST_MAX_CALLS];
STATIC npa_client_handle                mDestroyLog[TEST_MAX_CALLS];
STATIC UINTN                            mLocateCalls;
STATIC UINTN                            mQueryCalls;
STATIC UINTN                            mInstallCalls;
STATIC UINTN                            mCreateCalls;
STATIC UINTN                            mVectorCalls;
STATIC UINTN                            mDestroyCalls;
STATIC UINTN                            mCompleteCalls;
STATIC UINTN                            mFailCreateAt;
STATIC UINTN                            mNullCreateAt;
STATIC UINTN                            mFailVectorAt;
STATIC UINTN                            mFailDestroyAt;
STATIC EFI_STATUS                       mLocateStatus;
STATIC EFI_STATUS                       mQueryStatus;
STATIC EFI_STATUS                       mInstallStatus;
STATIC npa_query_status                 mAvailability;
STATIC VOID                            *mInstalledInterface;

EFI_LOCK *
EFIAPI
EfiInitializeLock (
  IN OUT EFI_LOCK  *Lock,
  IN EFI_TPL        Priority
  )
{
  Lock->Tpl      = Priority;
  Lock->OwnerTpl = TPL_APPLICATION;
  Lock->Lock     = EfiLockReleased;
  return Lock;
}

VOID
EFIAPI
EfiAcquireLock (
  IN EFI_LOCK  *Lock
  )
{
  assert ((Lock != NULL) && (Lock->Tpl == TPL_NOTIFY));
  assert (Lock->Lock == EfiLockReleased);
  Lock->Lock = EfiLockAcquired;
}

VOID
EFIAPI
EfiReleaseLock (
  IN EFI_LOCK  *Lock
  )
{
  assert ((Lock != NULL) && (Lock->Lock == EfiLockAcquired));
  Lock->Lock = EfiLockReleased;
}

VOID *
EFIAPI
ZeroMem (
  OUT VOID  *Buffer,
  IN UINTN   Length
  )
{
  return memset (Buffer, 0, Length);
}

INTN
EFIAPI
AsciiStrCmp (
  IN CONST CHAR8  *FirstString,
  IN CONST CHAR8  *SecondString
  )
{
  return strcmp (FirstString, SecondString);
}

VOID
EFIAPI
DebugPrint (
  IN UINTN        ErrorLevel,
  IN CONST CHAR8 *Format,
  ...
  )
{
  (VOID)ErrorLevel;
  (VOID)Format;
}

CONST InterconnectTargetContext *
CrDalGetInterconnectContext (
  VOID
  )
{
  return mDalTarget;
}

STATIC
EFI_STATUS
EFIAPI
MockCreateSyncClientEx (
  IN CONST CHAR8        *ResourceName,
  IN CONST CHAR8        *ClientName,
  IN npa_client_type     ClientType,
  IN UINT32              ClientValue,
  IN VOID               *ClientReference,
  OUT npa_client_handle *ClientHandle
  )
{
  CREATE_CALL  *Log;
  UINTN         Call;

  assert ((ClientHandle != NULL) && (mCreateCalls < TEST_MAX_CALLS));
  Call = ++mCreateCalls;
  Log  = &mCreateLog[Call - 1];
  assert (strlen (ResourceName) < sizeof (Log->ResourceName));
  assert (strlen (ClientName) < sizeof (Log->ClientName));
  strcpy (Log->ResourceName, ResourceName);
  strcpy (Log->ClientName, ClientName);
  Log->ClientType  = ClientType;
  Log->ClientValue = ClientValue;
  if ((ClientReference != NULL) &&
      (ClientValue == sizeof (ICB_NPA_MASTER_SLAVE))) {
    memcpy (&Log->Pair, ClientReference, sizeof (Log->Pair));
  }

  *ClientHandle = NULL;
  if (Call == mFailCreateAt) {
    return EFI_DEVICE_ERROR;
  }

  if (Call == mNullCreateAt) {
    return EFI_SUCCESS;
  }

  mHandles[Call - 1].Id = Call;
  Log->Handle            = &mHandles[Call - 1];
  *ClientHandle          = Log->Handle;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockCompleteRequest (
  IN npa_client_handle  Client
  )
{
  assert (Client != NULL);
  mCompleteCalls++;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockQueryResourceAvailable (
  IN CONST CHAR8       *ResourceName,
  OUT npa_query_status *Status
  )
{
  assert ((ResourceName != NULL) && (Status != NULL));
  assert (strcmp (ResourceName, ICB_NPA_RESOURCE_NAME) == 0);
  mQueryCalls++;
  *Status = mAvailability;
  return mQueryStatus;
}

STATIC
EFI_STATUS
EFIAPI
MockIssueVectorRequest (
  IN npa_client_handle  Client,
  IN UINT32             ElementCount,
  IN npa_resource_state *Vector
  )
{
  VECTOR_CALL  *Log;
  UINTN         Call;

  assert ((Client != NULL) && (Vector != NULL));
  assert (mVectorCalls < TEST_MAX_CALLS);
  Call              = ++mVectorCalls;
  Log               = &mVectorLog[Call - 1];
  Log->Handle        = Client;
  Log->ElementCount  = ElementCount;
  assert ((ElementCount * sizeof (*Vector)) == sizeof (Log->Request));
  memcpy (&Log->Request, Vector, sizeof (Log->Request));
  return (Call == mFailVectorAt) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockDestroyClient (
  IN npa_client_handle  Client
  )
{
  UINTN  Call;

  assert ((Client != NULL) && (mDestroyCalls < TEST_MAX_CALLS));
  Call                  = ++mDestroyCalls;
  mDestroyLog[Call - 1] = Client;
  return (Call == mFailDestroyAt) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockLocateProtocol (
  IN EFI_GUID  *Protocol,
  IN VOID      *Registration,
  OUT VOID    **Interface
  )
{
  assert ((Protocol == &gEfiNpaProtocolGuid) && (Registration == NULL));
  assert (Interface != NULL);
  mLocateCalls++;
  if (EFI_ERROR (mLocateStatus)) {
    *Interface = NULL;
    return mLocateStatus;
  }

  *Interface = &mNpaProtocol;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockInstallMultipleProtocolInterfaces (
  IN OUT EFI_HANDLE  *Handle,
  ...
  )
{
  EFI_GUID  *Guid;
  va_list    Args;

  assert ((Handle != NULL) && (*Handle == (EFI_HANDLE)(UINTN)0x1234));
  va_start (Args, Handle);
  Guid                = va_arg (Args, EFI_GUID *);
  mInstalledInterface = va_arg (Args, VOID *);
  assert (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  assert (Guid == &gEfiInterconnectCrProtocolGuid);
  assert (mInstalledInterface == &mInterconnectProtocol);
  assert (mInitialized);
  mInstallCalls++;
  return mInstallStatus;
}

STATIC
VOID
ResetMock (
  VOID
  )
{
  memset (&mBootServices, 0, sizeof (mBootServices));
  memset (&mNpaProtocol, 0, sizeof (mNpaProtocol));
  memset (mHandles, 0, sizeof (mHandles));
  memset (mCreateLog, 0, sizeof (mCreateLog));
  memset (mVectorLog, 0, sizeof (mVectorLog));
  memset (mDestroyLog, 0, sizeof (mDestroyLog));
  memset (mPaths, 0, sizeof (mPaths));
  memset (&mPathLock, 0, sizeof (mPathLock));
  mNpa                = NULL;
  mTarget             = NULL;
  mInitialized        = FALSE;
  mDalTarget          = CrTargetGetInterconnectContext ();
  mLocateCalls        = 0;
  mQueryCalls         = 0;
  mInstallCalls       = 0;
  mCreateCalls        = 0;
  mVectorCalls        = 0;
  mDestroyCalls       = 0;
  mCompleteCalls      = 0;
  mFailCreateAt       = 0;
  mNullCreateAt       = 0;
  mFailVectorAt       = 0;
  mFailDestroyAt      = 0;
  mLocateStatus       = EFI_SUCCESS;
  mQueryStatus        = EFI_SUCCESS;
  mInstallStatus      = EFI_SUCCESS;
  mAvailability       = NPA_QUERY_SUCCESS;
  mInstalledInterface = NULL;
  mBootServices.LocateProtocol = MockLocateProtocol;
  mBootServices.InstallMultipleProtocolInterfaces =
    MockInstallMultipleProtocolInterfaces;
  mNpaProtocol.Revision               =
    EFI_NPA_PROTOCOL_VER_WITH_BATCH_REQUEST_SUPPORT;
  mNpaProtocol.CreateSyncClientEx     = MockCreateSyncClientEx;
  mNpaProtocol.CompleteRequest        = MockCompleteRequest;
  mNpaProtocol.QueryResourceAvailable = MockQueryResourceAvailable;
  mNpaProtocol.DestroyClient          = MockDestroyClient;
  mNpaProtocol.IssueVectorRequest     = MockIssueVectorRequest;
}

STATIC
EFI_INTERCONNECT_CR_PROTOCOL *
StartDriver (
  VOID
  )
{
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS
    );
  assert ((mLocateCalls == 1) && (mQueryCalls == 1) && (mInstallCalls == 1));
  assert (mInstalledInterface == &mInterconnectProtocol);
  assert (mPathLock.Tpl == TPL_NOTIFY);
  return (EFI_INTERCONNECT_CR_PROTOCOL *)mInstalledInterface;
}

STATIC
VOID
AssertZeroRequest (
  IN CONST VECTOR_CALL *Call,
  IN npa_client_handle  Handle
  )
{
  assert (Call->Handle == Handle);
  assert (Call->ElementCount == 8);
  assert (Call->Request.Type == ICB_NPA_REQUEST_TYPE_3);
  assert (Call->Request.Reserved == 0);
  assert (Call->Request.Data.InstantaneousBandwidth == 0);
  assert (Call->Request.Data.AverageBandwidth == 0);
  assert (Call->Request.Data.LatencyNs == 0);
  assert (Call->Request.Data.Reserved == 0);
}

STATIC
VOID
TestDiscoveryAndAvailability (
  VOID
  )
{
  ResetMock ();
  mLocateStatus = EFI_NOT_FOUND;
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_NOT_FOUND
    );
  assert (!mInitialized && (mInstallCalls == 0));

  ResetMock ();
  mNpaProtocol.Revision = EFI_NPA_PROTOCOL_VER_LEGACY - 1;
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_UNSUPPORTED
    );
  assert (!mInitialized && (mQueryCalls == 0));

  ResetMock ();
  mQueryStatus = EFI_DEVICE_ERROR;
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_DEVICE_ERROR
    );
  assert (!mInitialized && (mInstallCalls == 0));

  ResetMock ();
  mAvailability = NPA_QUERY_UNKNOWN_RESOURCE;
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_NOT_READY
    );
  assert (!mInitialized && (mInstallCalls == 0));

  ResetMock ();
  mInstallStatus = EFI_DEVICE_ERROR;
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_DEVICE_ERROR
    );
  assert (!mInitialized && (mNpa == NULL) && (mTarget == NULL));

  ResetMock ();
  StartDriver ();
  assert (
    InterconnectEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) ==
    EFI_ALREADY_STARTED
    );
}

STATIC
VOID
TestNativeRoutesAndBandwidth (
  VOID
  )
{
  STATIC CONST struct {
    UINT32       Source;
    UINT32       Destination;
    UINT32       Master;
    UINT32       Slave;
    CONST CHAR8 *ClientName;
  } Expected[] = {
    { 0x1000, 0x1100, 65, 0,  "crane-pcie0-mem" },
    { 0x2000, 0x2100, 0,  84, "crane-pcie0-cfg" },
    { 0x1001, 0x1101, 66, 0,  "crane-pcie1-mem" },
    { 0x2000, 0x2101, 0,  85, "crane-pcie1-cfg" },
  };
  EFI_INTERCONNECT_CR_PROTOCOL  *Protocol;
  INTERCONNECT_PATH_HANDLE       Paths[ARRAY_SIZE (Expected)];
  UINTN                          Index;
  UINTN                          VectorCalls;
  UINT64                         OldAverage;
  UINT64                         OldPeak;

  ResetMock ();
  Protocol = StartDriver ();
  assert ((mDalTarget != NULL) && (mDalTarget->NativeRouteCount == 4));
  assert (
    Protocol->AcquirePath (Protocol, NULL, 0, 0, &Paths[0]) ==
    EFI_INVALID_PARAMETER
    );
  assert (
    Protocol->AcquirePath (
                Protocol,
                "qcom,sm8450-pcie",
                0xdead,
                0xbeef,
                &Paths[0]
                ) == EFI_NOT_FOUND
    );

  for (Index = 0; Index < ARRAY_SIZE (Expected); Index++) {
    assert (
      Protocol->AcquirePath (
                  Protocol,
                  "qcom,sm8450-pcie",
                  Expected[Index].Source,
                  Expected[Index].Destination,
                  &Paths[Index]
                  ) == EFI_SUCCESS
      );
    assert (Paths[Index] == Index + 1);
    assert (strcmp (mCreateLog[Index].ResourceName, "/icb/arbiter") == 0);
    assert (
      strcmp (mCreateLog[Index].ClientName, Expected[Index].ClientName) == 0
      );
    assert (mCreateLog[Index].ClientType == NPA_CLIENT_VECTOR);
    assert (mCreateLog[Index].ClientValue == 8);
    assert (mCreateLog[Index].Pair.Master == Expected[Index].Master);
    assert (mCreateLog[Index].Pair.Slave == Expected[Index].Slave);
    assert (mCreateLog[Index].Handle != NULL);
    if (Index != 0) {
      assert (mCreateLog[Index].Handle != mCreateLog[0].Handle);
    }
  }

  assert (
    Protocol->SetBandwidth (Protocol, Paths[0], 125000, 250000) == EFI_SUCCESS
    );
  assert ((sizeof (ICB_NPA_REQUEST) == 32) && (mVectorCalls == 1));
  assert (mVectorLog[0].ElementCount == 8);
  assert (mVectorLog[0].Request.Type == ICB_NPA_REQUEST_TYPE_3);
  assert (mVectorLog[0].Request.Data.InstantaneousBandwidth == 250000000ULL);
  assert (mVectorLog[0].Request.Data.AverageBandwidth == 125000000ULL);
  assert (
    Protocol->SetBandwidth (Protocol, Paths[1], 0, 1000) == EFI_SUCCESS
    );
  assert (mVectorLog[1].Request.Data.InstantaneousBandwidth == 1000000ULL);
  assert (mVectorLog[1].Request.Data.AverageBandwidth == 0);

  VectorCalls = mVectorCalls;
  assert (
    Protocol->SetBandwidth (Protocol, Paths[0], 2, 1) ==
    EFI_INVALID_PARAMETER
    );
  assert (
    Protocol->SetBandwidth (
                Protocol,
                Paths[0],
                0,
                (MAX_UINT64 / ICB_NPA_BANDWIDTH_SCALE) + 1
                ) == EFI_INVALID_PARAMETER
    );
  assert (mVectorCalls == VectorCalls);

  OldAverage       = mPaths[Paths[0] - 1].AverageBandwidth;
  OldPeak          = mPaths[Paths[0] - 1].PeakBandwidth;
  mFailVectorAt    = mVectorCalls + 1;
  assert (
    Protocol->SetBandwidth (Protocol, Paths[0], 200000, 300000) ==
    EFI_DEVICE_ERROR
    );
  assert (mPaths[Paths[0] - 1].AverageBandwidth == OldAverage);
  assert (mPaths[Paths[0] - 1].PeakBandwidth == OldPeak);

  mFailVectorAt = 0;
  for (Index = 0; Index < ARRAY_SIZE (Expected); Index++) {
    assert (Protocol->ReleasePath (Protocol, Paths[Index]) == EFI_SUCCESS);
  }
  assert ((mDestroyCalls == 4) && (mCompleteCalls == 0));
}

STATIC
VOID
TestCreateFailureDoesNotOccupySlot (
  VOID
  )
{
  EFI_INTERCONNECT_CR_PROTOCOL  *Protocol;
  INTERCONNECT_PATH_HANDLE       Path;

  ResetMock ();
  Protocol      = StartDriver ();
  mFailCreateAt = 1;
  Path          = 99;
  assert (
    Protocol->AcquirePath (
                Protocol,
                "qcom,sm8450-pcie",
                0x1000,
                0x1100,
                &Path
                ) == EFI_DEVICE_ERROR
    );
  assert (Path == INTERCONNECT_INVALID_PATH);
  assert (!mPaths[0].InUse && (mPaths[0].Client == NULL));

  mFailCreateAt = 0;
  assert (
    Protocol->AcquirePath (
                Protocol,
                "qcom,sm8450-pcie",
                0x1000,
                0x1100,
                &Path
                ) == EFI_SUCCESS
    );
  assert (Path == 1);
  assert (Protocol->ReleasePath (Protocol, Path) == EFI_SUCCESS);

  ResetMock ();
  Protocol      = StartDriver ();
  mNullCreateAt = 1;
  assert (
    Protocol->AcquirePath (
                Protocol,
                "qcom,sm8450-pcie",
                0x1000,
                0x1100,
                &Path
                ) == EFI_OUT_OF_RESOURCES
    );
  assert (!mPaths[0].InUse && (mPaths[0].Client == NULL));
}

STATIC
VOID
TestReleaseRetries (
  VOID
  )
{
  EFI_INTERCONNECT_CR_PROTOCOL  *Protocol;
  INTERCONNECT_PATH_HANDLE       Path;
  npa_client_handle              Client;
  UINTN                          Calls;

  ResetMock ();
  Protocol = StartDriver ();
  assert (
    Protocol->AcquirePath (
                Protocol,
                "qcom,sm8450-pcie",
                0x1000,
                0x1100,
                &Path
                ) == EFI_SUCCESS
    );
  Client = mPaths[Path - 1].Client;
  assert (Protocol->SetBandwidth (Protocol, Path, 100, 200) == EFI_SUCCESS);

  mFailVectorAt = mVectorCalls + 1;
  assert (Protocol->ReleasePath (Protocol, Path) == EFI_DEVICE_ERROR);
  assert (mPaths[Path - 1].InUse && (mPaths[Path - 1].Client == Client));
  assert (mPaths[Path - 1].AverageBandwidth == 100);
  assert (mPaths[Path - 1].PeakBandwidth == 200);
  assert ((mDestroyCalls == 0) && (mCompleteCalls == 0));

  mFailVectorAt  = 0;
  mFailDestroyAt = 1;
  Calls          = mVectorCalls;
  assert (Protocol->ReleasePath (Protocol, Path) == EFI_DEVICE_ERROR);
  assert (mVectorCalls == Calls + 1);
  AssertZeroRequest (&mVectorLog[mVectorCalls - 1], Client);
  assert (mPaths[Path - 1].InUse && (mPaths[Path - 1].Client == Client));
  assert (mPaths[Path - 1].AverageBandwidth == 0);
  assert (mPaths[Path - 1].PeakBandwidth == 0);
  assert ((mDestroyCalls == 1) && (mDestroyLog[0] == Client));

  mFailDestroyAt = 0;
  Calls          = mVectorCalls;
  assert (Protocol->ReleasePath (Protocol, Path) == EFI_SUCCESS);
  assert (mVectorCalls == Calls + 1);
  AssertZeroRequest (&mVectorLog[mVectorCalls - 1], Client);
  assert (!mPaths[Path - 1].InUse && (mPaths[Path - 1].Client == NULL));
  assert ((mDestroyCalls == 2) && (mDestroyLog[1] == Client));
  assert (mCompleteCalls == 0);
}

int
main (
  void
  )
{
  TestDiscoveryAndAvailability ();
  TestNativeRoutesAndBandwidth ();
  TestCreateFailureDoesNotOccupySlot ();
  TestReleaseRetries ();
  puts (
    "ICBCrDxe: native routes, NPA vectors, units and rollback passed"
    );
  return 0;
}
