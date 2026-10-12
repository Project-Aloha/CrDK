/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include "../Driver/RpmhCrDxe/RpmhCrDxe.c"
#include "../Target/Sm8450/CrRpmhRails.h"
#ifdef CR_HDK_USB_SHARED_RAIL_TEST
#include "../Target/Sm8350/CrStorageTarget.h"
PcieTargetContext *CrHdk8350GetPcieContext(VOID);
STATIC BOOLEAN mHdk8350;
#endif
#ifdef CR_MDSS_PANEL_RETAINED_TEST
CONST CR_DAL_RPMH_RAILS *CrTestGetPanelRails(VOID);
CONST CR_DAL_RPMH_TOPOLOGY *CrTestGetPanelTopology(VOID);
STATIC BOOLEAN mPanelBoard;
#endif

#ifndef NULL
#define NULL  ((void *)0)
#endif

#define TEST_MAX_WRITES  64U

typedef struct {
  RpmhTcsCmd  Command;
} WRITE_CALL;

STATIC CONST PcieTargetSupply mSupplies[] = {
  { "vdda-phy", "rpmh", "ldob5",  880,  0, 7 },
  { "vdda-pll", "rpmh", "ldob6", 1200,  0, 7 },
  { "vdda-phy", "rpmh", "ldoh2",  880,  0, 7 },
  { "vcc",      "rpmh", "ldob7", 2504,  0, 7 },
  { "vccq",     "rpmh", "ldob9", 1200,  0, 7 },
};

STATIC CONST PcieTargetContext mTarget = {
  .Supplies    = mSupplies,
  .SupplyCount = ARRAY_SIZE (mSupplies),
};

STATIC RpmhDeviceContext mRpmhContext;
STATIC EFI_BOOT_SERVICES mBootServices;
EFI_BOOT_SERVICES *gBS = &mBootServices;

EFI_GUID gEfiCmdDBCrProtocolGuid = EFI_CMD_DB_PROTOCOL_GUID;
EFI_GUID gEfiRpmhCrProtocolGuid  = EFI_RPMH_CR_PROTOCOL_GUID;

STATIC EFI_CMD_DB_PROTOCOL mCmdDbProtocol;
STATIC WRITE_CALL          mWrites[TEST_MAX_WRITES];
STATIC UINTN               mWriteCalls;
STATIC UINTN               mFailWriteAt;
STATIC CR_STATUS mFailStatus;
STATIC UINTN               mInstallCalls;
STATIC EFI_STATUS          mLocateStatus;
STATIC EFI_STATUS          mInstallStatus;
STATIC VOID               *mInstalledInterface;
STATIC UINTN               mInitCalls;
STATIC BOOLEAN            mExtraRails;
STATIC BOOLEAN            mNativeBoard;
STATIC BOOLEAN            mNoPcie;
STATIC CONST CHAR8        *mMissingRail;
STATIC BOOLEAN            mArcAvailable, mArcBadAux;
STATIC UINT32             mArcMissing;
STATIC CONST CHAR8 *CONST mExtraNames[] = {
  "ldob5", "bobc1", "missing", "smpb10", "smpb11", "smpb12",
  "ldob1", "ldob2", "ldoc1"
};
STATIC CONST CR_DAL_RPMH_RAILS mExtra = {ARRAY_SIZE(mExtraNames), mExtraNames};
STATIC CONST CR_DAL_DEVICE_INFO mExtraInfo = {.Data = &mExtra};
STATIC CONST CR_DAL_RPMH_PARENT mBoardParents[] = {
  {"ldob1", "smpb11",  968, 6}, {"ldob2", "bobc1", 3296, 6},
  {"ldob5", "bobc1", 3296, 6}, {"ldob6", "smpb12", 1352, 6},
  {"ldob7", "bobc1", 3296, 6}, {"ldob9", "smpb12", 1352, 6},
  {"ldoc1", "bobc1", 3296, 6}, {"ldoh2", "bobc1", 3296, 6}
};
STATIC CONST CR_DAL_RPMH_TOPOLOGY mBoardTopology = {
  ARRAY_SIZE(mBoardParents), mBoardParents
};
STATIC CONST CR_DAL_RPMH_TOPOLOGY *mTestTopology;
STATIC CR_DAL_DEVICE_INFO mTopologyInfo;
VOID *EFIAPI SetMem (VOID *Buffer, UINTN Length, UINT8 Value) { return memset (Buffer, Value, Length); }
INTN EFIAPI AsciiStrCmp (CONST CHAR8 *Left, CONST CHAR8 *Right) { return strcmp (Left, Right); }

STATIC EFI_STATUS mInventoryStatus=EFI_NOT_FOUND;
STATIC CR_DAL_DEVICE_INFO mInventoryInfo;
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE Type, UINT32 Instance,
  UINT32 Revision, UINTN MinimumSize, CONST CR_DAL_DEVICE_INFO **Info)
{
  assert(Instance == 0 && Revision == CR_DAL_DATA_REVISION_1);
  if (Type == CrDalDeviceRpmhArcInventory) {
    assert(MinimumSize == sizeof(CR_DAL_RPMH_ARC_INVENTORY));
    *Info = mInventoryStatus == EFI_SUCCESS ? &mInventoryInfo : NULL;
    return mInventoryStatus;
  }
#ifdef CR_MDSS_PANEL_RETAINED_TEST
  if (mPanelBoard) {
    mTopologyInfo.Data=Type==CrDalDeviceRpmhRails ?
      (CONST VOID *)CrTestGetPanelRails() : (CONST VOID *)CrTestGetPanelTopology();
    *Info=&mTopologyInfo;return EFI_SUCCESS;
  }
#endif
#ifdef CR_HDK_USB_SHARED_RAIL_TEST
  if (mHdk8350) {
    mTopologyInfo.Data = Type == CrDalDeviceRpmhRails ? (CONST VOID *)CrSm8350GetStorageRails() : (CONST VOID *)CrSm8350GetStorageTopology();
    *Info = &mTopologyInfo; return EFI_SUCCESS;
  }
#endif
  if (mNativeBoard) {
    mTopologyInfo.Data = Type == CrDalDeviceRpmhRails ? (CONST VOID *)&mSm8450Rails : (CONST VOID *)&mSm8450RailTopology;
    *Info = &mTopologyInfo;
    return EFI_SUCCESS;
  }
  if (Type == CrDalDeviceRpmhRails) {
    assert(MinimumSize == sizeof(mExtra));
    *Info = mExtraRails ? &mExtraInfo : NULL;
    return mExtraRails ? EFI_SUCCESS : EFI_NOT_FOUND;
  }
  assert(Type == CrDalDeviceRpmhTopology && MinimumSize == sizeof(*mTestTopology));
  mTopologyInfo.Data = mTestTopology;
  *Info = mTestTopology == NULL ? NULL : &mTopologyInfo;
  return mTestTopology == NULL ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MockGetAddress (IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT32 *Address);
STATIC EFI_STATUS EFIAPI MockGetAux (IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT8 *Aux, IN OUT UINT32 *Length);
STATIC EFI_STATUS EFIAPI MockLocateProtocol (IN EFI_GUID *Protocol, IN VOID *Registration, OUT VOID **Interface);
STATIC EFI_STATUS EFIAPI MockInstallMultipleProtocolInterfaces (IN OUT EFI_HANDLE *Handle, ...);

STATIC
VOID
ResetMocks (
  VOID
  )
{
  memset (&mBootServices, 0, sizeof (mBootServices));
  memset (&mCmdDbProtocol, 0, sizeof (mCmdDbProtocol));
  memset (&mRpmhContext, 0, sizeof (mRpmhContext));
  memset (mWrites, 0, sizeof (mWrites));
  memset (mRails, 0, sizeof (mRails));
  mMissingRail = NULL;
  memset(mRetainedRails,0,sizeof(mRetainedRails)); mFailStatus=CR_DEVICE_ERROR;
  memset (&mArcVoter, 0, sizeof (mArcVoter));
  memset (mArcResources, 0, sizeof (mArcResources));
  memset (mArcEntries, 0, sizeof (mArcEntries));
  mArcAcknowledged = mArcCount = 0;
  mArcFloorReady = mArcFloorAttempted = mArcSealed = FALSE;
  mArcAvailable = mArcBadAux = FALSE; mArcMissing = MAX_UINT32;
  mWriteCalls       = 0;
  mFailWriteAt      = 0;
  mInstallCalls     = 0;
  mLocateStatus     = EFI_SUCCESS;
  mInstallStatus    = EFI_SUCCESS;
  mInstalledInterface = NULL;
  mInitCalls        = 0;
  mExtraRails       = FALSE;
  mNativeBoard      = FALSE;
#ifdef CR_MDSS_PANEL_RETAINED_TEST
  mPanelBoard = FALSE;
#endif
#ifdef CR_HDK_USB_SHARED_RAIL_TEST
  mHdk8350 = FALSE;
#endif
  mNoPcie           = FALSE;
  mTestTopology     = NULL;
  mRpmh             = NULL;
  mCmdDb            = NULL;
  mRailCount        = 0;
  mBootServices.LocateProtocol = MockLocateProtocol;
  mBootServices.InstallMultipleProtocolInterfaces =
    MockInstallMultipleProtocolInterfaces;
  mCmdDbProtocol.Revision = EFI_CMD_DB_PROTOCOL_REVISION;
  mCmdDbProtocol.GetEntryAddressByName = MockGetAddress;
  mCmdDbProtocol.GetAuxDataByName = MockGetAux;
}

EFI_LOCK *
EFIAPI
EfiInitializeLock (
  IN OUT EFI_LOCK *Lock,
  IN EFI_TPL       Priority
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
  IN EFI_LOCK *Lock
  )
{
  assert ((Lock != NULL) && (Lock->Lock == EfiLockReleased));
  Lock->Lock = EfiLockAcquired;
}

VOID
EFIAPI
EfiReleaseLock (
  IN EFI_LOCK *Lock
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
  return mNoPcie ? NULL : &mTarget;
}

RpmhDeviceContext *
CrDalGetRpmhContext (
  VOID
  )
{
  return &mRpmhContext;
}

CR_STATUS
RpmhLibInit (
  IN OUT RpmhDeviceContext **Context
  )
{
  assert (Context != NULL);
  if (*Context == NULL) {
    return CR_NOT_FOUND;
  }
  (*Context)->Initialized = TRUE;
  mInitCalls++;
  return CR_SUCCESS;
}

CR_STATUS
RpmhWrite (
  IN RpmhDeviceContext *Context,
  IN RpmhTcsCmd        *Commands,
  IN UINT32             Count
  )
{
  UINT32 Index;

  assert ((Context == &mRpmhContext) && Context->Initialized);
  if ((Commands == NULL) || (Count == 0) || (Count > TEST_MAX_WRITES)) {
    return CR_INVALID_PARAMETER;
  }
  for (Index = 0; Index < Count; Index++) {
    if (mWriteCalls >= TEST_MAX_WRITES) {
      return CR_OUT_OF_RESOURCES;
    }
    mWrites[mWriteCalls++].Command = Commands[Index];
  }
  if ((mFailWriteAt != 0) && (mWriteCalls >= mFailWriteAt)) {
    return mFailStatus;
  }
  return CR_SUCCESS;
}

CR_STATUS
RpmhSetVregVoltage (
  IN RpmhDeviceContext *Context,
  IN CONST UINT32       Address,
  IN CONST UINT32       VoltageMv
  )
{
  RpmhTcsCmd Command = { Address, VoltageMv, 0, 1 };
  return RpmhWrite (Context, &Command, 1);
}

CR_STATUS
RpmhSetVregMode (
  IN RpmhDeviceContext *Context,
  IN CONST UINT32       Address,
  IN CONST UINT8        Mode
  )
{
  RpmhTcsCmd Command = { Address + 8, Mode, 0, 1 };
  return RpmhWrite (Context, &Command, 1);
}

CR_STATUS
RpmhEnableVreg (
  IN RpmhDeviceContext *Context,
  IN CONST UINT32       Address,
  IN BOOLEAN             Enable
  )
{
  RpmhTcsCmd Command = { Address + 4, Enable, 0, 1 };
  return RpmhWrite (Context, &Command, 1);
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
MockGetAddress (
  IN EFI_CMD_DB_PROTOCOL *This,
  IN CONST CHAR8          *Name,
  OUT UINT32              *Address
  )
{
  (VOID)This;
  assert ((Name != NULL) && (Address != NULL));
  if (mMissingRail != NULL && strcmp(Name,mMissingRail)==0) return EFI_NOT_FOUND;
  if (mArcAvailable) for (UINT32 Index = 0; Index < EFI_RPMH_CR_ARC_RESOURCE_COUNT; ++Index)
    if (strcmp (Name, mArcNames[Index]) == 0) {
      if (Index == mArcMissing) return EFI_NOT_FOUND;
      *Address = 0x30000 + Index * 4; return EFI_SUCCESS;
    }
  if (strcmp (Name, "ldob5") == 0) *Address = 0x00051000;
  else if (strcmp (Name, "ldob6") == 0) *Address = 0x00052000;
  else if (strcmp (Name, "ldoh2") == 0) *Address = 0x00042000;
  else if (strcmp (Name, "ldob7") == 0) *Address = 0x00057000;
  else if (strcmp (Name, "ldob9") == 0) *Address = 0x00059000;
  else if (strcmp (Name, "bobc1") == 0) *Address = 0x00060000;
  else if (strcmp (Name, "smpb10") == 0) *Address = 0x00070000;
  else if (strcmp (Name, "smpb11") == 0) *Address = 0x00071000;
  else if (strcmp (Name, "smpb12") == 0) *Address = 0x00072000;
  else if (strcmp (Name, "ldob1") == 0) *Address = 0x00080000;
  else if (strcmp (Name, "ldob2") == 0) *Address = 0x00081000;
  else if (strcmp (Name, "ldoc1") == 0) *Address = 0x00090000;
  else if (strcmp (Name, "smpc1") == 0) *Address = 0x00091000;
  else if (strcmp (Name, "ldoc9") == 0) *Address = 0x00099000;
  else if (strcmp (Name, "ldoc6") == 0) *Address = 0x00096000;
#ifdef CR_MDSS_PANEL_RETAINED_TEST
  else if (strcmp (Name, "ldoa14") == 0) *Address = 0x000a4000;
  else if (strcmp (Name, "ldoc11") == 0) *Address = 0x0009b000;
#endif
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI MockGetAux (EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT8 *Aux, UINT32 *Length)
{
  (VOID)This;
  STATIC CONST UINT8 Levels[] = {0,0,64,0,128,0,0,0};
  assert (Aux != NULL && Length != NULL && *Length >= sizeof (Levels));
  if (strcmp (Name, "xo.lvl") == 0) return EFI_NOT_FOUND;
  memcpy (Aux, Levels, sizeof (Levels)); *Length = sizeof (Levels);
  if (mArcBadAux) Aux[0] = 1;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockLocateProtocol (
  IN EFI_GUID *Protocol,
  IN VOID     *Registration,
  OUT VOID   **Interface
  )
{
  assert ((Protocol == &gEfiCmdDBCrProtocolGuid) && (Registration == NULL));
  if (EFI_ERROR (mLocateStatus)) {
    *Interface = NULL;
    return mLocateStatus;
  }
  *Interface = &mCmdDbProtocol;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
MockInstallMultipleProtocolInterfaces (
  IN OUT EFI_HANDLE *Handle,
  ...
  )
{
  EFI_GUID *Guid;
  va_list Args;

  assert ((Handle != NULL) && (*Handle == (EFI_HANDLE)(UINTN)0x1234));
  va_start (Args, Handle);
  Guid                = va_arg (Args, EFI_GUID *);
  mInstalledInterface = va_arg (Args, VOID *);
  assert (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  assert (Guid == &gEfiRpmhCrProtocolGuid);
  mInstallCalls++;
  return mInstallStatus;
}

STATIC
VOID
StartDriver (
  VOID
  )
{
  assert (RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS);
  assert ((mInstallCalls == 1) && (mRailCount == 5));
  assert (mInstalledInterface == &gRpmhCrProtocol);
  assert (mInitCalls == 1);
}

STATIC
VOID
TestIndependentEntryAndRawWrite (
  VOID
  )
{
  RpmhTcsCmd Commands[2] = {
    { 0x1234, 0x5678, 0, 1 }, { 0x9abc, 0xdef0, 0, 0 }
  };

  ResetMocks ();
  StartDriver ();
  assert (ProtocolRpmhWrite (&gRpmhCrProtocol, Commands, 2) == EFI_SUCCESS);
  assert (mWriteCalls == 2);
  assert (mWrites[0].Command.addr == 0x1234);
  assert (mWrites[1].Command.addr == 0x9abc);
  assert (ProtocolRpmhWrite (&gRpmhCrProtocol, NULL, 1) == EFI_NOT_READY);
}

STATIC
VOID
TestDirectRailSequenceAndSharing (
  VOID
  )
{
  BOOLEAN Owned;

  ResetMocks ();
  StartDriver ();
  assert (ProtocolRpmhAcquireVreg (&gRpmhCrProtocol, "ldob6", 1200, 7,
                                   &Owned) == EFI_SUCCESS && Owned);
  assert (mWriteCalls == 3);
  assert (mWrites[0].Command.addr == 0x52000);
  assert (mWrites[0].Command.data == 1200);
  assert (mWrites[1].Command.addr == 0x52008);
  assert (mWrites[1].Command.data == 7);
  assert (mWrites[2].Command.addr == 0x52004);
  assert (mWrites[2].Command.data == 1);
  assert (ProtocolRpmhAcquireVreg (&gRpmhCrProtocol, "ldob6", 1200, 7,
                                   &Owned) == EFI_SUCCESS && Owned);
  assert (mWriteCalls == 3);
  assert (ProtocolRpmhAcquireVreg (&gRpmhCrProtocol, "ldob6", 1000, 7,
                                   &Owned) == EFI_ACCESS_DENIED && !Owned);
  assert (ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
  assert (mWriteCalls == 3);
  assert (ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
  assert (mWriteCalls == 6);
  assert (mWrites[3].Command.addr == 0x52004 && mWrites[3].Command.data == 0);
  assert (mWrites[4].Command.addr == 0x52008 && mWrites[4].Command.data == 0);
  assert (mWrites[5].Command.addr == 0x52000 && mWrites[5].Command.data == 0);
}

STATIC
VOID
TestFailureRetentionAndInputChecks (
  VOID
  )
{
  BOOLEAN Owned;

  ResetMocks ();
  StartDriver ();
  assert (ProtocolRpmhSetVregVoltage (&gRpmhCrProtocol, "ldoh2", 0) ==
          EFI_INVALID_PARAMETER);
  assert (ProtocolRpmhSetVregMode (&gRpmhCrProtocol, "ldoh2", 5) ==
          EFI_INVALID_PARAMETER);
  mFailWriteAt = 2;
  assert (ProtocolRpmhAcquireVreg (&gRpmhCrProtocol, "ldoh2", 880, 7,
                                   &Owned) == EFI_DEVICE_ERROR && Owned);
  assert (mRails[2].VoltageRequested && mRails[2].ModeRequested);
  mFailWriteAt = 0;
  assert (ProtocolRpmhEnableVreg (&gRpmhCrProtocol, "ldoh2", FALSE) == EFI_SUCCESS);
  assert (!mRails[2].VoltageRequested && !mRails[2].ModeRequested);
}

STATIC
VOID
TestEntryFailures (
  VOID
  )
{
  ResetMocks ();
  mLocateStatus = EFI_NOT_FOUND;
  assert (RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_NOT_FOUND);
  assert (mInstallCalls == 0);

  ResetMocks ();
  mCmdDbProtocol.GetEntryAddressByName = NULL;
  assert (RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) ==
          EFI_INCOMPATIBLE_VERSION);
  assert (mInstallCalls == 0);

  ResetMocks ();
  mInstallStatus = EFI_ACCESS_DENIED;
  assert (RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_ACCESS_DENIED);
  assert (mInstallCalls == 1);
}

STATIC VOID StartBoard (VOID)
{
  ResetMocks();
  mExtraRails = TRUE;
  mTestTopology = &mBoardTopology;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS);
  assert(mWriteCalls == 0);
}

STATIC VOID TestMissingPcieSupply (VOID)
{
  for (UINTN I=0; I<ARRAY_SIZE(mSupplies); ++I) {
    ResetMocks();mMissingRail=mSupplies[I].Id;
    StartDriver(); /* Missing per-port resource must not suppress provider. */
    BOOLEAN Owned=FALSE;
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,mMissingRail,mSupplies[I].VoltageMv,mSupplies[I].Mode,&Owned)==EFI_NOT_FOUND);
    assert(Owned && mWriteCalls==0);
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,mMissingRail,FALSE)==EFI_SUCCESS && mWriteCalls==0);
    UINTN Other=(I+1)%ARRAY_SIZE(mSupplies);
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,mSupplies[Other].Id,mSupplies[Other].VoltageMv,mSupplies[Other].Mode,&Owned)==EFI_SUCCESS);
    assert(Owned && mWriteCalls==3);
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,mSupplies[Other].Id,FALSE)==EFI_SUCCESS);
  }
  puts("Missing optional PCIe supply: RPMh publication, independent rails and clean acquisition failure passed");
}

STATIC VOID CheckWrite (UINTN Index, UINT32 Address, UINT32 Data)
{
  assert(Index < mWriteCalls);
  assert(mWrites[Index].Command.addr == Address && mWrites[Index].Command.data == Data);
}

STATIC VOID TestParentOrderAndSharing (VOID)
{
  BOOLEAN Owned;
  StartBoard();
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &Owned) == EFI_SUCCESS && Owned);
  assert(mWriteCalls == 6);
  CheckWrite(0, 0x72000, 1352); CheckWrite(1, 0x72008, 6); CheckWrite(2, 0x72004, 1);
  CheckWrite(3, 0x52000, 1200); CheckWrite(4, 0x52008, 7); CheckWrite(5, 0x52004, 1);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob9", 1200, 7, &Owned) == EFI_SUCCESS && Owned);
  assert(mWriteCalls == 9 && FindRail("smpb12")->ReferenceCount == 2);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &Owned) == EFI_SUCCESS && Owned);
  assert(FindRail("smpb12")->ReferenceCount == 2); /* one input lease per active child */
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS && mWriteCalls == 9);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS && mWriteCalls == 12);
  assert(FindRail("smpb12")->ReferenceCount == 1);
  CheckWrite(9, 0x52004, 0); CheckWrite(10, 0x52008, 0); CheckWrite(11, 0x52000, 0);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob9", FALSE) == EFI_SUCCESS && mWriteCalls == 18);
  CheckWrite(15, 0x72004, 0); CheckWrite(16, 0x72008, 0); CheckWrite(17, 0x72000, 0);

  StartBoard();
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob2", 3072, 7, &Owned) == EFI_SUCCESS && Owned);
  CheckWrite(0, 0x60000, 3296); CheckWrite(3, 0x81000, 3072);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldoh2", 912, 7, &Owned) == EFI_SUCCESS && Owned);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "bobc1", 3296, 6, &Owned) == EFI_SUCCESS && Owned);
  assert(mWriteCalls == 9 && FindRail("bobc1")->ReferenceCount == 3);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob2", FALSE) == EFI_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "bobc1", FALSE) == EFI_SUCCESS);
  assert(mWriteCalls == 12 && FindRail("bobc1")->ReferenceCount == 1);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldoh2", FALSE) == EFI_SUCCESS && mWriteCalls == 18);
}

STATIC VOID TestParentFailureRetention (VOID)
{
  BOOLEAN Owned, OtherOwned;
  for (UINTN Fail = 1; Fail <= 6; ++Fail) {
    StartBoard(); mFailWriteAt = Fail;
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &Owned) == EFI_DEVICE_ERROR && Owned);
    assert(mWriteCalls == Fail && FindRail("ldob6")->ParentOwned);
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &OtherOwned) == EFI_ACCESS_DENIED && !OtherOwned);
    mFailWriteAt = 0;
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
    for (UINTN I = 0; I < mRailCount; ++I)
      assert(!mRails[I].ParentOwned && !mRails[I].VoltageRequested &&
             !mRails[I].ModeRequested && !mRails[I].EnableRequested && mRails[I].ReferenceCount == 0);
  }
  for (UINTN Fail = 7; Fail <= 12; ++Fail) {
    StartBoard();
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &Owned) == EFI_SUCCESS && Owned);
    mFailWriteAt = Fail;
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_DEVICE_ERROR);
    assert(FindRail("ldob6")->ParentOwned && mWriteCalls == Fail);
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &OtherOwned) == EFI_NOT_READY && !OtherOwned);
    if (Fail <= 9) assert(FindRail("smpb12")->Ready && FindRail("smpb12")->ReferenceCount == 1);
    mFailWriteAt = 0;
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
    assert(!FindRail("ldob6")->ParentOwned && !FindRail("smpb12")->EnableRequested);
  }
  StartBoard();
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "bobc1", 3008, 6, &Owned) == EFI_SUCCESS && Owned);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldoh2", 912, 7, &OtherOwned) == EFI_ACCESS_DENIED && OtherOwned);
  assert(!FindRail("ldoh2")->ParentOwned && mWriteCalls == 3);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldoh2", FALSE) == EFI_SUCCESS && mWriteCalls == 3);
  assert(FindRail("bobc1")->ReferenceCount == 1); /* never release a rejected input lease */
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "bobc1", FALSE) == EFI_SUCCESS);
  /* Legacy enable-only callers have not requested voltage/mode. A new
     high-level caller must not inherit stale values from an earlier lease. */
  ResetMocks(); StartDriver();
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &Owned) == EFI_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", TRUE) == EFI_SUCCESS);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "ldob6", 1200, 7, &OtherOwned) == EFI_ACCESS_DENIED && !OtherOwned);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "ldob6", FALSE) == EFI_SUCCESS);
}

STATIC VOID TestInvalidTopology (VOID)
{
  CONST CR_DAL_RPMH_PARENT Cases[][2] = {
    {{"ldob5", "unknown", 3296, 6}, {"ldob6", "smpb12", 1352, 6}},
    {{"ldob5", "ldob5", 3296, 6}, {"ldob6", "smpb12", 1352, 6}},
    {{"ldob5", "bobc1", 3296, 6}, {"ldob5", "smpb12", 1352, 6}},
    {{"ldob5", "bobc1", 3296, 6}, {"bobc1", "ldob5", 880, 7}},
    {{"ldob5", "bobc1", 3296, 6}, {"ldob2", "bobc1", 3008, 6}},
    {{"ldob5", "bobc1", 3296, 5}, {"ldob6", "smpb12", 1352, 6}}
  };
  for (UINTN I = 0; I < ARRAY_SIZE(Cases); ++I) {
    CR_DAL_RPMH_TOPOLOGY Invalid = {2, Cases[I]};
    ResetMocks(); mExtraRails = TRUE; mTestTopology = &Invalid;
    assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_COMPROMISED_DATA);
    assert(mWriteCalls == 0 && mInstallCalls == 0);
  }
}

STATIC VOID TestManagedArcFloor (VOID)
{
  UINT32 Owners[123] = {0};
  EFI_RPMH_CR_ARC_SNAPSHOT Snapshot, Saved;
  ResetMocks (); mArcAvailable = TRUE; StartDriver ();
  assert (mArcFloorReady && mArcAcknowledged == 63 && mWriteCalls == 6);
  assert (gRpmhCrProtocol.Revision >= 0x10003 && gRpmhCrProtocol.EnsureBootFloor != NULL);
  assert (gRpmhCrProtocol.EnsureBootFloor (&gRpmhCrProtocol) == EFI_SUCCESS && mWriteCalls == 6);
  for (UINT32 I=0; I<6; ++I) {
    CheckWrite (I, 0x30000 + I*4, I==5 ? 3 : 2);
    assert (mWrites[I].Command.wait && mWrites[I].Command.response_required);
  }
  RpmhTcsCmd Batch[2]={{0x1234,1,1,1},{0x30004,0,1,1}};
  assert (ProtocolRpmhWrite (&gRpmhCrProtocol,Batch,2)==EFI_ACCESS_DENIED && mWriteCalls==6);
  Batch[1].addr++; assert (ProtocolRpmhWrite (&gRpmhCrProtocol,Batch,2)==EFI_ACCESS_DENIED && mWriteCalls==6);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners,0x30000,3)==EFI_INVALID_PARAMETER);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners,0x30014,1)==EFI_INVALID_PARAMETER);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners,0x30000,1)==EFI_SUCCESS);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners+1,0x30000,2)==EFI_SUCCESS && mWriteCalls==6);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners,0x30000,0)==EFI_SUCCESS && mWriteCalls==6);
  memset (&Snapshot,0x5a,sizeof (Snapshot)); Saved=Snapshot;
  assert (ProtocolRpmhSealArcSnapshot (&gRpmhCrProtocol,&Snapshot)==EFI_NOT_READY && !memcmp(&Snapshot,&Saved,sizeof(Saved)) && !mArcSealed);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners+1,0x30000,0)==EFI_SUCCESS);
  mRpmhContext.TcsBusy=1;
  assert (ProtocolRpmhSealArcSnapshot (&gRpmhCrProtocol,&Snapshot)==EFI_NOT_READY && !memcmp(&Snapshot,&Saved,sizeof(Saved)));
  mRpmhContext.TcsBusy=0;mArcVoter.States[1].Pending=TRUE;
  assert (ProtocolRpmhSealArcSnapshot (&gRpmhCrProtocol,&Snapshot)==EFI_NOT_READY);
  mArcVoter.States[1].Pending=FALSE;mArcEntries[0].Value.Corner=1;
  assert (ProtocolRpmhSealArcSnapshot (&gRpmhCrProtocol,&Snapshot)==EFI_NOT_READY);
  mArcEntries[0].Value.Corner=2;
  assert (ProtocolRpmhSealArcSnapshot (&gRpmhCrProtocol,&Snapshot)==EFI_SUCCESS && mArcSealed && mWriteCalls==6);
  assert (Snapshot.Size==sizeof(Snapshot) && Snapshot.Revision==1 && Snapshot.Count==6 && Snapshot.OwnerCount==1 && Snapshot.Flags==7);
  for(UINT32 I=0;I<6;++I)assert(!strcmp(Snapshot.Resources[I].Name,mArcNames[I]) && Snapshot.Corners[I]==(I==5?3:2));
  Saved=Snapshot;assert(ProtocolRpmhSealArcSnapshot(&gRpmhCrProtocol,&Snapshot)==EFI_SUCCESS && !memcmp(&Snapshot,&Saved,sizeof(Saved)));
  assert (gRpmhCrProtocol.EnsureBootFloor (&gRpmhCrProtocol) == EFI_SUCCESS && mWriteCalls == 6);
  assert (ProtocolRpmhVoteArc (&gRpmhCrProtocol,Owners,0x30000,1)==EFI_ACCESS_DENIED && mWriteCalls==6);

  ResetMocks ();mArcAvailable=TRUE;StartDriver ();
  for(UINT32 I=0;I<122;++I)assert(ProtocolRpmhVoteArc(&gRpmhCrProtocol,Owners+I,0x30000,1)==EFI_SUCCESS);
  assert(ProtocolRpmhVoteArc(&gRpmhCrProtocol,Owners+122,0x30000,1)==EFI_OUT_OF_RESOURCES && mWriteCalls==6);
  for(UINT32 I=0;I<122;++I)assert(ProtocolRpmhVoteArc(&gRpmhCrProtocol,Owners+I,0x30000,0)==EFI_SUCCESS);
  assert(ProtocolRpmhSealArcSnapshot(&gRpmhCrProtocol,&Snapshot)==EFI_SUCCESS && mWriteCalls==6);
}
STATIC VOID TestArcBootstrapRetention (VOID)
{
  UINT32 Owner=0;EFI_RPMH_CR_ARC_SNAPSHOT Snapshot,Saved;
  for(UINT32 Fail=1;Fail<=6;++Fail){
    ResetMocks ();mArcAvailable=TRUE;mFailWriteAt=Fail;StartDriver ();
    assert(!mArcFloorReady && mArcAcknowledged==((1U<<(Fail-1))-1) && mWriteCalls==Fail);
    memset(&Snapshot,0x5a,sizeof(Snapshot));Saved=Snapshot;
    assert(ProtocolRpmhSealArcSnapshot(&gRpmhCrProtocol,&Snapshot)==EFI_NOT_READY && !memcmp(&Snapshot,&Saved,sizeof(Saved)));
    mFailWriteAt=0;
    assert(gRpmhCrProtocol.EnsureBootFloor(&gRpmhCrProtocol)==EFI_SUCCESS && mArcFloorReady && mWriteCalls==7);
    assert(ProtocolRpmhVoteArc(&gRpmhCrProtocol,&Owner,0x30000,1)==EFI_SUCCESS && mArcFloorReady && mWriteCalls==7);
    for(UINT32 I=0;I<mWriteCalls;++I)assert(mWrites[I].Command.data==(mWrites[I].Command.addr==0x30014?3:2));
    assert(ProtocolRpmhVoteArc(&gRpmhCrProtocol,&Owner,0x30000,0)==EFI_SUCCESS && mWriteCalls==7);
    assert(ProtocolRpmhSealArcSnapshot(&gRpmhCrProtocol,&Snapshot)==EFI_SUCCESS);
  }
  ResetMocks ();mArcAvailable=TRUE;mArcMissing=2;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234,NULL)==EFI_COMPROMISED_DATA && !mWriteCalls && !mInitCalls && !mInstallCalls);
  ResetMocks ();mArcAvailable=TRUE;mArcBadAux=TRUE;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234,NULL)==EFI_COMPROMISED_DATA && !mWriteCalls && !mInitCalls);
  ResetMocks ();mArcAvailable=TRUE;mInstallStatus=EFI_ACCESS_DENIED;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234,NULL)==EFI_SUCCESS && mArcFloorReady && mRpmh==&mRpmhContext && mCmdDb!=NULL);
}

STATIC VOID TestBootFloorQualification (VOID)
{
  ResetMocks ();
  assert (ProtocolRpmhEnsureBootFloor (NULL) == EFI_NOT_READY);
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
  StartDriver ();
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_FOUND && mWriteCalls == 0);
  ResetMocks (); mArcAvailable = TRUE; StartDriver ();
  mArcAcknowledged &= ~1U;
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
  mArcAcknowledged = 63;
  mRpmhContext.TcsBusy = 1;
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
  mRpmhContext.TcsBusy = 0; mRpmhContext.TcsSynchronous = 1;
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
  mRpmhContext.TcsSynchronous = 0; mRpmhContext.TcsRetiring = 1;
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
  mRpmhContext.TcsRetiring = 0;
  for (UINT32 I = 0; I < 6; ++I) {
    mArcVoter.States[I].Known = FALSE;
    assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
    mArcVoter.States[I].Known = TRUE; mArcVoter.States[I].Pending = TRUE;
    assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
    mArcVoter.States[I].Pending = FALSE; mArcVoter.States[I].Applied--;
    assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_NOT_READY);
    mArcVoter.States[I].Applied++;
  }
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_SUCCESS && mWriteCalls == 6);
  mArcSealed = TRUE; mArcFloorReady = FALSE;
  assert (ProtocolRpmhEnsureBootFloor (&gRpmhCrProtocol) == EFI_ACCESS_DENIED && mWriteCalls == 6);
}

STATIC VOID TestNativeSdSupplies (VOID)
{
  ResetMocks (); mNativeBoard = TRUE;
  assert (RpmhEntryPoint ((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS);
  BOOLEAN Owned = FALSE;
  assert (gRpmhCrProtocol.AcquireVreg (&gRpmhCrProtocol, "ldoc9", 3008, 7, &Owned) == EFI_SUCCESS && Owned);
  assert (mWriteCalls == 6 && mWrites[0].Command.addr == 0x60000 && mWrites[0].Command.data == 3296);
  assert (mWrites[1].Command.addr == 0x60008 && mWrites[1].Command.data == 6);
  assert (mWrites[3].Command.addr == 0x99000 && mWrites[3].Command.data == 3008);
  assert (gRpmhCrProtocol.AcquireVreg (&gRpmhCrProtocol, "ldoc6", 3008, 7, &Owned) == EFI_SUCCESS && Owned);
  assert (mWriteCalls == 9 && mWrites[6].Command.addr == 0x96000 && mWrites[6].Command.data == 3008);
  assert (FindRail ("bobc1")->ReferenceCount == 2);
  assert (gRpmhCrProtocol.RpmhEnableVreg (&gRpmhCrProtocol, "ldoc9", FALSE) == EFI_SUCCESS);
  assert (FindRail ("bobc1")->ReferenceCount == 1 && mWriteCalls == 12);
  assert (gRpmhCrProtocol.RpmhEnableVreg (&gRpmhCrProtocol, "ldoc6", FALSE) == EFI_SUCCESS);
  assert (FindRail ("bobc1")->ReferenceCount == 0 && mWriteCalls == 18);
}

STATIC VOID TestRetainedBaseline(VOID)
{
  for (UINTN Warning=0; Warning<2; ++Warning)
    for (UINTN Fail=1; Fail<=6; ++Fail) {
      StartBoard(); mFailWriteAt=Fail; mFailStatus=Warning?1:CR_DEVICE_ERROR;
      assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==EFI_DEVICE_ERROR);
      RPMH_CR_RAIL *C=FindRail("ldob6"), *P=FindRail("smpb12");
      assert(C->ReferenceCount==1 && C->ParentOwned && P->ReferenceCount==1);
      assert(!C->Ready && RetainedFloor(C)==1 && RetainedFloor(P)==1);
      assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldob6",FALSE)==EFI_ACCESS_DENIED);
      assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"smpb12",FALSE)==EFI_ACCESS_DENIED);
      assert(RpmhEntryPoint((EFI_HANDLE)1,NULL)==EFI_ALREADY_STARTED);
      UINTN Before=mWriteCalls;
      assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1300,7)==EFI_ACCESS_DENIED);
      assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,6)==EFI_ACCESS_DENIED);
      assert(mWriteCalls==Before);
      mFailWriteAt=0;
      assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==EFI_SUCCESS);
      assert(C->Ready && P->Ready && C->ReferenceCount==1 && P->ReferenceCount==1);
      for (UINTN I=0; I<mWriteCalls; ++I) assert(mWrites[I].Command.data);
      Before=mWriteCalls;
      for (UINTN I=0; I<3; ++I) {
        BOOLEAN Owned=FALSE;
        assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==EFI_SUCCESS);
        assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"ldob6",1200,7,&Owned)==EFI_SUCCESS && Owned);
        assert(C->ReferenceCount==2);
        assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldob6",FALSE)==EFI_SUCCESS);
        assert(C->ReferenceCount==1 && P->ReferenceCount==1 && mWriteCalls==Before);
      }
      for (UINT32 Offset=0; Offset<12; ++Offset) {
        RpmhTcsCmd Raw={.addr=C->Address+Offset,.data=0};
        assert(ProtocolRpmhWrite(&gRpmhCrProtocol,&Raw,1)==EFI_ACCESS_DENIED);
        Raw.addr=P->Address+Offset;
        assert(ProtocolRpmhWrite(&gRpmhCrProtocol,&Raw,1)==EFI_ACCESS_DENIED);
      }
    }
  StartBoard(); BOOLEAN Owned;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"smpb12",1352,7,&Owned)==EFI_SUCCESS && Owned);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==CR_BUSY);
  assert(!FindRail("ldob6")->ParentOwned);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"smpb12",FALSE)==EFI_SUCCESS);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==EFI_SUCCESS);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob9",1200,7)==EFI_SUCCESS);
  assert(FindRail("smpb12")->ReferenceCount==2 && RetainedFloor(FindRail("smpb12"))==2);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"smpb12",1352,6)==EFI_SUCCESS);
  assert(FindRail("smpb12")->ReferenceCount==3 && RetainedFloor(FindRail("smpb12"))==3);
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"smpb12",1352,6,&Owned)==EFI_SUCCESS && Owned);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"smpb12",FALSE)==EFI_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"smpb12",FALSE)==EFI_ACCESS_DENIED);
  StartBoard();
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"ldob6",1200,7,&Owned)==EFI_SUCCESS && Owned);
  UINTN Before=mWriteCalls;
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",1200,7)==EFI_SUCCESS);
  assert(FindRail("ldob6")->ReferenceCount==2 && mWriteCalls==Before);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldob6",FALSE)==EFI_SUCCESS);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"ldob6",FALSE)==EFI_ACCESS_DENIED);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"unqualified",1200,7)==EFI_NOT_FOUND);
  assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"ldob6",0,7)==EFI_INVALID_PARAMETER);
  puts("retained provider: positive-only parent/child retries, immutable identity, protected graph floor PASS");
}

#ifdef CR_HDK_USB_SHARED_RAIL_TEST
STATIC VOID TestHdkUsbPcieSharing(VOID) {
  CONST CR_DAL_RPMH_TOPOLOGY *T=CrSm8350GetStorageTopology();
  CONST PcieTargetContext *P=CrHdk8350GetPcieContext();
  CONST CHAR8 *Children[]={"ldoc1","ldob1"};
  for(UINTN C=0;C<ARRAY_SIZE(Children);++C) {
    CONST CR_DAL_RPMH_PARENT *Parent=NULL;
    for(UINTN I=0;I<T->Count;++I) if(!strcmp(T->Parents[I].Child,Children[C])) Parent=&T->Parents[I];
    assert(Parent);
    CONST PcieTargetSupply *Supply=NULL;
    for(UINTN I=0;I<P->SupplyCount;++I) if(!strcmp(P->Supplies[I].Id,Parent->Parent)) Supply=&P->Supplies[I];
    assert(Supply);
    for(UINTN Order=0;Order<2;++Order) {
      ResetMocks();mHdk8350=TRUE;
      assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234,NULL)==EFI_SUCCESS);
      BOOLEAN A=FALSE,B=FALSE;
      UINT32 ChildMv=C?912:1800;
      if(Order==0) {
        assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,Supply->Id,Supply->VoltageMv,Supply->Mode,&A)==EFI_SUCCESS && A);
        assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,Children[C],ChildMv,7,&B)==EFI_SUCCESS && B);
      } else {
        assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,Children[C],ChildMv,7,&B)==EFI_SUCCESS && B);
        assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,Supply->Id,Supply->VoltageMv,Supply->Mode,&A)==EFI_SUCCESS && A);
      }
      assert(FindRail(Supply->Id)->ReferenceCount==2);
      assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,Children[C],FALSE)==EFI_SUCCESS);
      assert(FindRail(Supply->Id)->ReferenceCount==1 && FindRail(Supply->Id)->Ready);
      assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,Supply->Id,FALSE)==EFI_SUCCESS);
    }
  }
  puts("HDK8350 actual RPMh provider: USB/PCIe shared S1C and S11B both init orders and independent release PASS");
}
#endif

int
main (
  void
  )
{
#ifdef CR_HDK_USB_SHARED_RAIL_TEST
  TestHdkUsbPcieSharing();
#endif
  /* An optional UFS supplier inherits voltage/mode. Even uncertain enable
   * completion must protect it against cleanup, raw writes and re-entry. */
  for (UINTN Failure=0; Failure<3; ++Failure) {
    StartBoard();
    mFailWriteAt=Failure?1:0; mFailStatus=Failure==2?1:CR_DEVICE_ERROR;
    EFI_STATUS Status=gRpmhCrProtocol.EnsureRetainedVregEnabled(&gRpmhCrProtocol,"smpb12");
    assert(Status==(Failure?EFI_DEVICE_ERROR:EFI_SUCCESS));
    RPMH_CR_RAIL *Rail=FindRail("smpb12");
    assert(Rail->ReferenceCount==1 && RetainedFloor(Rail)==1);
    assert(!Rail->VoltageRequested && !Rail->ModeRequested && !Rail->VoltageMv && !Rail->Mode);
    assert(mWriteCalls==1 && mWrites[0].Command.addr==Rail->Address+4 && mWrites[0].Command.data==1);
    UINTN Before=mWriteCalls; BOOLEAN Owned=TRUE;
    assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol,"smpb12",FALSE)==EFI_ACCESS_DENIED);
    assert(ProtocolRpmhSetVregVoltage(&gRpmhCrProtocol,"smpb12",1256)==EFI_ACCESS_DENIED);
    assert(ProtocolRpmhSetVregMode(&gRpmhCrProtocol,"smpb12",6)==EFI_ACCESS_DENIED);
    assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"smpb12",1256,6,&Owned)==CR_BUSY && !Owned);
    assert(ProtocolRpmhEnsureRetainedVreg(&gRpmhCrProtocol,"smpb12",1256,6)==CR_BUSY);
    assert(RpmhEntryPoint((EFI_HANDLE)1,NULL)==EFI_ALREADY_STARTED);
    for(UINT32 Offset=0;Offset<12;++Offset) {
      RpmhTcsCmd Raw={.addr=Rail->Address+Offset,.data=0};
      assert(ProtocolRpmhWrite(&gRpmhCrProtocol,&Raw,1)==EFI_ACCESS_DENIED);
    }
    assert(mWriteCalls==Before);mFailWriteAt=0;
    assert(gRpmhCrProtocol.EnsureRetainedVregEnabled(&gRpmhCrProtocol,"smpb12")==EFI_SUCCESS);
    assert(Rail->Ready && Rail->ReferenceCount==1 && mWriteCalls==Before+(Failure!=0));
    Before=mWriteCalls;
    assert(gRpmhCrProtocol.EnsureRetainedVregEnabled(&gRpmhCrProtocol,"smpb12")==EFI_SUCCESS);
    assert(mWriteCalls==Before);
  }
  StartBoard(); BOOLEAN Existing;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol,"smpb12",1352,6,&Existing)==EFI_SUCCESS);
  UINTN Previous=mWriteCalls;
  assert(gRpmhCrProtocol.EnsureRetainedVregEnabled(&gRpmhCrProtocol,"smpb12")==CR_BUSY);
  assert(gRpmhCrProtocol.EnsureRetainedVregEnabled(&gRpmhCrProtocol,"ldob6")==EFI_UNSUPPORTED);
  assert(mWriteCalls==Previous);
  puts("retained enable-only supplier: no voltage/mode writes; timeout/warning retry and raw/release protection PASS");
  TestRetainedBaseline();
  ResetMocks(); mNoPcie = TRUE; mExtraRails = TRUE;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS);
  assert(mInstalledInterface == &gRpmhCrProtocol && mRailCount == ARRAY_SIZE(mExtraNames));
  assert(mWriteCalls == 0 && mInitCalls == 1);
  BOOLEAN IndependentOwned = FALSE;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "bobc1", 3008, 6, &IndependentOwned) == EFI_SUCCESS);
  assert(IndependentOwned && mWriteCalls == 3);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "bobc1", FALSE) == EFI_SUCCESS);
  PcieTargetContext Invalid = {0};
  assert(InitializeRailDescriptions(&Invalid) == EFI_COMPROMISED_DATA && mRailCount == 0);
  ResetMocks();
  mExtraRails = TRUE;
  assert(RpmhEntryPoint((EFI_HANDLE)(UINTN)0x1234, NULL) == EFI_SUCCESS);
  assert(mRailCount == 13 && mWriteCalls == 0);
  BOOLEAN Owned = FALSE;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "bobc1", 3008, 6, &Owned) == EFI_SUCCESS);
  assert(Owned && mWriteCalls == 3 && mWrites[0].Command.addr == 0x60000);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "bobc1", FALSE) == EFI_SUCCESS);
  UINTN Calls = mWriteCalls;
  assert(ProtocolRpmhAcquireVreg(&gRpmhCrProtocol, "missing", 1000, 7, &Owned) == EFI_NOT_FOUND);
  assert(Owned && mWriteCalls == Calls);
  assert(ProtocolRpmhEnableVreg(&gRpmhCrProtocol, "missing", FALSE) == EFI_SUCCESS);
  TestNativeSdSupplies ();
  TestMissingPcieSupply ();
  TestIndependentEntryAndRawWrite ();
  TestDirectRailSequenceAndSharing ();
  TestFailureRetentionAndInputChecks ();
  TestEntryFailures ();
  TestParentOrderAndSharing ();
  TestParentFailureRetention ();
  TestInvalidTopology ();
  TestManagedArcFloor ();
  TestArcBootstrapRetention ();
  TestBootFloorQualification ();
  puts ("Crane RPMh: CmdDB/TCS, parent-before-child, shared inputs, failed cleanup and topology validation passed");
  return 0;
}

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
