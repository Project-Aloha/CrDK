/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include "../Driver/RpmhCrDxe/RpmhCrDxe.c"

#ifndef NULL
#define NULL  ((void *)0)
#endif

#define TEST_MAX_CALLS  32U

struct npa_client {
  UINTN  Id;
};

typedef struct {
  CONST CHAR8        *ResourceName;
  CONST CHAR8        *ClientName;
  npa_client_type     ClientType;
  UINT32              ClientValue;
  VOID               *ClientReference;
  npa_client_handle   Handle;
} CREATE_CALL;

typedef struct {
  npa_client_handle  Handle;
  UINT32             State;
} SCALAR_CALL;

STATIC CONST PcieTargetSupply  mSupplies[] = {
  { "vdda-phy", "rpmh", "ldob5", 880, 0, 7 },
  { "vdda-pll", "rpmh", "ldob6", 1200, 0, 7 },
  { "vdda-phy", "rpmh", "ldoh2", 880, 0, 7 },
  { "vdda-pll", "rpmh", "ldob6", 1200, 0, 7 },
};

STATIC CONST PcieTargetContext  mTarget = {
  .Supplies    = mSupplies,
  .SupplyCount = ARRAY_SIZE (mSupplies),
};

STATIC EFI_BOOT_SERVICES  mBootServices;
EFI_BOOT_SERVICES         *gBS = &mBootServices;

EFI_GUID  gEfiNpaProtocolGuid    = EFI_NPA_PROTOCOL_GUID;
EFI_GUID  gEfiRpmhCrProtocolGuid = EFI_RPMH_CR_PROTOCOL_GUID;

STATIC EFI_NPA_PROTOCOL  mNpaProtocol;
STATIC struct npa_client mHandles[TEST_MAX_CALLS];
STATIC CREATE_CALL       mCreateLog[TEST_MAX_CALLS];
STATIC SCALAR_CALL       mScalarLog[TEST_MAX_CALLS];
STATIC npa_client_handle mDestroyLog[TEST_MAX_CALLS];
STATIC UINTN             mCreateCalls;
STATIC UINTN             mScalarCalls;
STATIC UINTN             mDestroyCalls;
STATIC UINTN             mCompleteCalls;
STATIC UINTN             mInstallCalls;
STATIC UINTN             mFailCreateAt;
STATIC UINTN             mNullCreateAt;
STATIC UINTN             mFailScalarAt;
STATIC UINTN             mFailDestroyAt;
STATIC EFI_STATUS        mLocateStatus;
STATIC EFI_STATUS        mInstallStatus;
STATIC VOID             *mInstalledInterface;

EFI_LOCK *
EFIAPI
EfiInitializeLock (
  IN OUT EFI_LOCK  *Lock,
  IN EFI_TPL        Priority
  )
{
  assert (Priority == TPL_NOTIFY);
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
  assert ((Lock != NULL) && (Lock->Lock == EfiLockReleased));
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

CONST PcieTargetContext *
CrDalGetPcieContext (
  VOID
  )
{
  return &mTarget;
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
  UINTN  Call;

  assert (mCreateCalls < TEST_MAX_CALLS);
  Call                            = ++mCreateCalls;
  mCreateLog[Call - 1].ResourceName    = ResourceName;
  mCreateLog[Call - 1].ClientName      = ClientName;
  mCreateLog[Call - 1].ClientType      = ClientType;
  mCreateLog[Call - 1].ClientValue     = ClientValue;
  mCreateLog[Call - 1].ClientReference = ClientReference;
  *ClientHandle = NULL;
  if (Call == mFailCreateAt) {
    return EFI_DEVICE_ERROR;
  }
  if (Call == mNullCreateAt) {
    return EFI_SUCCESS;
  }

  mHandles[Call - 1].Id             = Call;
  *ClientHandle                     = &mHandles[Call - 1];
  mCreateLog[Call - 1].Handle       = *ClientHandle;
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
MockScalarRequest (
  IN npa_client_handle  Client,
  IN npa_resource_state State
  )
{
  UINTN  Call;

  assert ((Client != NULL) && (mScalarCalls < TEST_MAX_CALLS));
  Call                         = ++mScalarCalls;
  mScalarLog[Call - 1].Handle = Client;
  mScalarLog[Call - 1].State  = State;
  return (Call == mFailScalarAt) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
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
  Call                         = ++mDestroyCalls;
  mDestroyLog[Call - 1]       = Client;
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
  assert (Guid == &gEfiRpmhCrProtocolGuid);
  assert (mInstalledInterface == &gRpmhCrProtocol);
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
  memset (mScalarLog, 0, sizeof (mScalarLog));
  memset (mDestroyLog, 0, sizeof (mDestroyLog));
  memset (mRails, 0, sizeof (mRails));
  mNpa                  = NULL;
  mRailCount            = 0;
  mCreateCalls          = 0;
  mScalarCalls          = 0;
  mDestroyCalls         = 0;
  mCompleteCalls        = 0;
  mInstallCalls         = 0;
  mFailCreateAt         = 0;
  mNullCreateAt         = 0;
  mFailScalarAt         = 0;
  mFailDestroyAt        = 0;
  mLocateStatus         = EFI_SUCCESS;
  mInstallStatus        = EFI_SUCCESS;
  mInstalledInterface   = NULL;
  mBootServices.LocateProtocol = MockLocateProtocol;
  mBootServices.InstallMultipleProtocolInterfaces =
    MockInstallMultipleProtocolInterfaces;
  mNpaProtocol.Revision           = EFI_NPA_PROTOCOL_VER_WITH_DEINIT_SUPPORT;
  mNpaProtocol.CreateSyncClientEx = MockCreateSyncClientEx;
  mNpaProtocol.CompleteRequest    = MockCompleteRequest;
  mNpaProtocol.ScalarRequest      = MockScalarRequest;
  mNpaProtocol.DestroyClient      = MockDestroyClient;
}

STATIC
VOID
StartDriver (
  VOID
  )
{
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS
    );
  assert ((mInstallCalls == 1) && (mRailCount == 3));
}

STATIC
VOID
TestEntryCreatesUniqueRequiredClients (
  VOID
  )
{
  STATIC CONST CHAR8  *Resources[] = {
    "/pm/ldob5/mV", "/pm/ldob5/mode", "/pm/ldob5/en",
    "/pm/ldob6/mV", "/pm/ldob6/mode", "/pm/ldob6/en",
    "/pm/ldoh2/mV", "/pm/ldoh2/mode", "/pm/ldoh2/en",
  };
  UINTN  Index;

  ResetMock ();
  StartDriver ();
  assert (mCreateCalls == ARRAY_SIZE (Resources));
  for (Index = 0; Index < mCreateCalls; Index++) {
    assert (strcmp (mCreateLog[Index].ResourceName, Resources[Index]) == 0);
    assert (mCreateLog[Index].ClientType == NPA_CLIENT_REQUIRED);
    assert (mCreateLog[Index].ClientValue == 0);
    assert (mCreateLog[Index].ClientReference == NULL);
    assert (mCreateLog[Index].Handle != NULL);
  }
  assert (strcmp (mCreateLog[0].ClientName, "CraneRpmhVoltage") == 0);
  assert (strcmp (mCreateLog[1].ClientName, "CraneRpmhMode") == 0);
  assert (strcmp (mCreateLog[2].ClientName, "CraneRpmhEnable") == 0);
  assert (
    ProtocolRpmhWrite (&gRpmhCrProtocol, NULL, 0) == EFI_UNSUPPORTED
    );
  assert (mCompleteCalls == 0);
}

STATIC
VOID
TestOrderedVotesAndSharedReference (
  VOID
  )
{
  ResetMock ();
  StartDriver ();

  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldob6", 1200) ==
    EFI_SUCCESS
    );
  assert (
    ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldob6", 7) == EFI_SUCCESS
    );
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", TRUE) == EFI_SUCCESS
    );
  assert (mScalarCalls == 3);
  assert (
    (mScalarLog[0].Handle == mCreateLog[3].Handle) &&
    (mScalarLog[0].State == 1200)
    );
  assert (
    (mScalarLog[1].Handle == mCreateLog[4].Handle) &&
    (mScalarLog[1].State == 7)
    );
  assert (
    (mScalarLog[2].Handle == mCreateLog[5].Handle) &&
    (mScalarLog[2].State == 1)
    );

  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", TRUE) == EFI_SUCCESS
    );
  assert ((mScalarCalls == 3) && (mRails[1].ReferenceCount == 2));
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS
    );
  assert ((mScalarCalls == 3) && (mRails[1].ReferenceCount == 1));
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS
    );
  assert ((mScalarCalls == 6) && (mRails[1].ReferenceCount == 0));
  assert (
    (mScalarLog[3].Handle == mCreateLog[5].Handle) &&
    (mScalarLog[3].State == 0)
    );
  assert (
    (mScalarLog[4].Handle == mCreateLog[4].Handle) &&
    (mScalarLog[4].State == 0)
    );
  assert (
    (mScalarLog[5].Handle == mCreateLog[3].Handle) &&
    (mScalarLog[5].State == 0)
    );
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS
    );
  assert ((mScalarCalls == 6) && (mCompleteCalls == 0));
}

STATIC
VOID
TestPreEnableRollbackAndReleaseRetry (
  VOID
  )
{
  ResetMock ();
  StartDriver ();
  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldob5", 880) ==
    EFI_SUCCESS
    );
  mFailScalarAt = 2;
  assert (
    ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldob5", 7) ==
    EFI_DEVICE_ERROR
    );
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob5", FALSE) == EFI_SUCCESS
    );
  assert ((mScalarCalls == 4) && (mScalarLog[2].State == 0));
  assert (mScalarLog[2].Handle == mCreateLog[1].Handle);
  assert ((mScalarLog[3].State == 0) &&
          (mScalarLog[3].Handle == mCreateLog[0].Handle));
  assert (!mRails[0].VoltageRequested && !mRails[0].ModeRequested);

  mFailScalarAt = 0;
  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldob5", 880) ==
    EFI_SUCCESS
    );
  assert (
    ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldob5", 7) == EFI_SUCCESS
    );
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob5", TRUE) == EFI_SUCCESS
    );
  mFailScalarAt = mScalarCalls + 2;
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob5", FALSE) ==
    EFI_DEVICE_ERROR
    );
  assert (!mRails[0].EnableRequested);
  assert (mRails[0].ModeRequested);
  assert (mRails[0].VoltageRequested);
  mFailScalarAt = 0;
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob5", FALSE) == EFI_SUCCESS
    );
  assert (!mRails[0].ModeRequested);
  assert (!mRails[0].VoltageRequested);
  assert (mCompleteCalls == 0);
}

STATIC
VOID
TestFailedEnableOwnershipAndInputLimits (
  VOID
  )
{
  ResetMock ();
  StartDriver ();
  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldoh2", 0) ==
    EFI_INVALID_PARAMETER
    );
  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldoh2", 0x10000) ==
    EFI_INVALID_PARAMETER
    );
  assert (
    ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldoh2", 5) ==
    EFI_INVALID_PARAMETER
    );
  assert (mScalarCalls == 0);

  assert (
    ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldoh2", 880) ==
    EFI_SUCCESS
    );
  assert (
    ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldoh2", 7) == EFI_SUCCESS
    );
  mFailScalarAt = 3;
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldoh2", TRUE) ==
    EFI_DEVICE_ERROR
    );
  assert (mRails[2].EnableRequested && (mRails[2].ReferenceCount == 0));

  mFailScalarAt = 4;
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldoh2", FALSE) ==
    EFI_DEVICE_ERROR
    );
  assert (mScalarCalls == 4);
  assert (mRails[2].EnableRequested && mRails[2].ModeRequested &&
          mRails[2].VoltageRequested);

  mFailScalarAt = 0;
  assert (
    ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldoh2", FALSE) == EFI_SUCCESS
    );
  assert (mScalarCalls == 7);
  assert (!mRails[2].EnableRequested && !mRails[2].ModeRequested &&
          !mRails[2].VoltageRequested);
}

STATIC
VOID
TestEntryValidationAndCleanup (
  VOID
  )
{
  ResetMock ();
  mNpaProtocol.Revision = EFI_NPA_PROTOCOL_VER_LEGACY;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) ==
    EFI_INCOMPATIBLE_VERSION
    );
  assert ((mCreateCalls == 0) && (mInstallCalls == 0));

  ResetMock ();
  mNpaProtocol.ScalarRequest = NULL;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) ==
    EFI_INCOMPATIBLE_VERSION
    );
  assert (mCreateCalls == 0);

  ResetMock ();
  mFailCreateAt = 2;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_DEVICE_ERROR
    );
  assert ((mCreateCalls == 2) && (mDestroyCalls == 1));
  assert (mDestroyLog[0] == mCreateLog[0].Handle);

  ResetMock ();
  mNullCreateAt = 2;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_NOT_FOUND
    );
  assert ((mCreateCalls == 2) && (mDestroyCalls == 1));

  ResetMock ();
  mInstallStatus = EFI_ACCESS_DENIED;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_ACCESS_DENIED
    );
  assert ((mCreateCalls == 9) && (mDestroyCalls == 9));
  assert (mDestroyLog[0] == mCreateLog[8].Handle);
  assert (mDestroyLog[8] == mCreateLog[0].Handle);

  ResetMock ();
  mInstallStatus = EFI_ACCESS_DENIED;
  mFailDestroyAt = 1;
  assert (
    RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS
    );
  assert ((mInstallCalls == 1) && (mDestroyCalls == 9));
  assert (mRails[2].EnableClient != NULL);
}

int
main (
  void
  )
{
  TestEntryCreatesUniqueRequiredClients ();
  TestOrderedVotesAndSharedReference ();
  TestPreEnableRollbackAndReleaseRetry ();
  TestFailedEnableOwnershipAndInputLimits ();
  TestEntryValidationAndCleanup ();
  puts ("Crane RPMh NPA adapter: ownership, ordering and rollback checks passed");
  return 0;
}
