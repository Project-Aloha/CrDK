/** Standalone Crane Linux ICC/BCM DXE adapter test. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#ifndef NULL
#define NULL ((void *)0)
#endif
#include <Library/interconnect.h>
#include <Library/CrDalLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/rpmh.h>
#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>


/* Include the production adapter after test-owned platform hooks. */
EFI_GUID gEfiCmdDBCrProtocolGuid = EFI_CMD_DB_PROTOCOL_GUID;
EFI_GUID gEfiInterconnectCrProtocolGuid = EFI_INTERCONNECT_CR_PROTOCOL_GUID;

STATIC EFI_BOOT_SERVICES mBootServices;
EFI_BOOT_SERVICES *gBS = &mBootServices;
STATIC EFI_TPL mCurrentTpl = TPL_APPLICATION;
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID) { return mCurrentTpl; }
STATIC EFI_TPL EFIAPI Raise(EFI_TPL Tpl) {
  EFI_TPL Old = mCurrentTpl; assert(Tpl >= Old); mCurrentTpl = Tpl; return Old;
}
STATIC VOID EFIAPI Restore(EFI_TPL Tpl) {
  assert(Tpl <= mCurrentTpl); mCurrentTpl = Tpl;
}
STATIC EFI_CMD_DB_PROTOCOL mCmdDb;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mInstalled;
STATIC BOOLEAN mCmdDbAvailable;
STATIC UINT32 mRpmhWrites;
STATIC UINT32 mRpmhCommands;
STATIC UINT32 mRpmhFailFrom;
STATIC UINT32 mRpmhFailCall;
STATIC BOOLEAN mInstallFails;
STATIC BOOLEAN mFailInstallCleanup;
STATIC BOOLEAN mUninstallFails;

VOID *EFIAPI CopyMem(OUT VOID *D, IN CONST VOID *S, IN UINTN N) { return memmove(D, S, N); }
VOID *EFIAPI SetMem(OUT VOID *D, IN UINTN N, IN UINT8 V) { return memset(D, V, N); }
VOID *EFIAPI ZeroMem(OUT VOID *D, IN UINTN N) { return memset(D, 0, N); }
INTN EFIAPI CompareMem(IN CONST VOID *A, IN CONST VOID *B, IN UINTN N) { return memcmp(A, B, N); }
INTN EFIAPI AsciiStrCmp(IN CONST CHAR8 *A, IN CONST CHAR8 *B) { return strcmp(A, B); }
VOID EFIAPI DebugPrint(IN UINTN Level, IN CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return TRUE; }

UINT32 EFIAPI InterlockedCompareExchange32(
    IN OUT volatile UINT32 *Value, IN UINT32 Compare, IN UINT32 Exchange) {
  UINT32 Old = *Value;
  if (Old == Compare) *Value = Exchange;
  return Old;
}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) { *Lock = 1; return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 1); *Lock = 2; return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 2); *Lock = 1; return Lock; }

static EFI_STATUS EFIAPI Locate(
    IN EFI_GUID *Guid, IN VOID *Registration, OUT VOID **Interface) {
  (VOID)Registration;
  if (Guid == &gEfiCmdDBCrProtocolGuid && mCmdDbAvailable) {
    *Interface = &mCmdDb; return EFI_SUCCESS;
  }
  *Interface = NULL;
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Install(
    IN OUT EFI_HANDLE *Handle, ...) {
  va_list Args;
  EFI_GUID *Guid;
  VOID *Interface;
  (VOID)Handle;
  va_start(Args, Handle);
  Guid = va_arg(Args, EFI_GUID *);
  Interface = va_arg(Args, VOID *);
  va_end(Args);
  assert(Guid == &gEfiInterconnectCrProtocolGuid);
  if (mInstallFails) {
    if (mFailInstallCleanup) mRpmhFailFrom=mRpmhWrites+1;
    return EFI_DEVICE_ERROR;
  }
  mInstalled = (EFI_INTERCONNECT_CR_PROTOCOL *)Interface;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Uninstall(IN EFI_HANDLE Handle, ...) {
  va_list Args;
  (VOID)Handle;
  va_start(Args,Handle);
  assert(va_arg(Args,EFI_GUID *)==&gEfiInterconnectCrProtocolGuid);
  assert(va_arg(Args,VOID *)==mInstalled);
  va_end(Args);
  if (mUninstallFails) return EFI_DEVICE_ERROR;
  mInstalled=NULL;return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI GetAddress(
    EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT32 *Address) {
  (VOID)This;
  if (Name == NULL || Address == NULL) return EFI_INVALID_PARAMETER;
  *Address = 0x1000U + (UINT32)(Name[0] & 0xffU) * 4U;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetAux(
    EFI_CMD_DB_PROTOCOL *This, CONST CHAR8 *Name, UINT8 *Data, UINT32 *Length) {
  (VOID)This; (VOID)Name;
  if (Data == NULL || Length == NULL || *Length < 8) return EFI_INVALID_PARAMETER;
  memset(Data, 0, *Length);
  Data[0] = 1; Data[4] = 1; *Length = 8;
  return EFI_SUCCESS;
}

static RpmhDeviceContext mRpmh;
RpmhDeviceContext *CrDalGetRpmhContext(VOID) { return &mRpmh; }
STATIC InterconnectTargetContext *mTarget;
CONST CR_DAL_DEVICE_INFO *mInfo;
EFI_STATUS CrDalGetDeviceInfo(
    CR_DAL_DEVICE_TYPE Type, UINT32 Instance, UINT32 Rev, UINTN Size,
    CONST CR_DAL_DEVICE_INFO **Out) {
  (VOID)Type; (VOID)Instance; (VOID)Rev; (VOID)Size;
  *Out = mInfo; return mInfo == NULL ? EFI_NOT_FOUND : EFI_SUCCESS;
}
CONST InterconnectTargetContext *CrDalGetInterconnectContext(VOID) { return mTarget; }
CrDebugUartContext *CrDalGetDebugUartContext(VOID) { return NULL; }

CR_STATUS RpmhLibInit(IN OUT RpmhDeviceContext **Context) {
  assert(Context != NULL && *Context == &mRpmh);
  (*Context)->Initialized = TRUE;
  (*Context)->NumCmdsPerTcs = 16;
  return CR_SUCCESS;
}
CR_STATUS RpmhWrite(RpmhDeviceContext *Context, RpmhTcsCmd *Commands, UINT32 Count) {
  assert(Context == &mRpmh && Commands != NULL && Count != 0);
  mRpmhWrites++; mRpmhCommands += Count;
  if ((mRpmhFailFrom!=0 && mRpmhWrites>=mRpmhFailFrom) || mRpmhWrites==mRpmhFailCall)
    return CR_DEVICE_ERROR;
  return CR_SUCCESS;
}

#include "../Driver/ICBCrDxe/ICBCrDxe.c"

int main(void) {
  CR_DAL_DEVICE_INFO Info;
  InterconnectTargetContext *Target;
  INTERCONNECT_PATH_HANDLE Path;
  INTERCONNECT_PATH_HANDLE UfsPath;
  INTERCONNECT_PATH_HANDLE UfsCfgPath;
  EFI_STATUS Status;

  memset(&mBootServices, 0, sizeof(mBootServices));
  mBootServices.LocateProtocol = Locate;
  mBootServices.InstallMultipleProtocolInterfaces = Install;
  mBootServices.UninstallMultipleProtocolInterfaces = Uninstall;
  mBootServices.RaiseTPL = Raise;
  mBootServices.RestoreTPL = Restore;
  mCmdDb.Revision = EFI_CMD_DB_PROTOCOL_REVISION;
  mCmdDb.GetEntryAddressByName = GetAddress;
  mCmdDb.GetAuxDataByName = GetAux;
  Target = CrTargetGetInterconnectContext();
  assert(Target->NativeRouteCount == 0 && Target->NativeRoutes == NULL);
  mTarget = Target;
  memset(&Info, 0, sizeof(Info));
  Info.Size = sizeof(Info);
  Info.Type = CrDalDeviceRpmh;
  Info.Instance = 0;
  Info.Data = &mRpmh;
  Info.DataSize = sizeof(mRpmh);
  Info.DataRevision = CR_DAL_DATA_REVISION_1;
  mInfo = &Info;

  mCmdDbAvailable = TRUE;
  Status = InterconnectEntryPoint((EFI_HANDLE)(UINTN)1, NULL);
  assert(Status == EFI_SUCCESS && mInstalled != NULL);
  assert(mRpmhWrites != 0); /* Linux keepalive BCM votes were committed locally. */
  for (UINT32 Index = 0; Index < 3; ++Index) {
    UINT64 Bandwidth = Index == 2 ? 1000 : 1;
    assert(mInstalled->AcquirePath(mInstalled, "qcom,sm8450-qup1",
        0x5000 + 2 * Index, 0x5001 + 2 * Index, &Path) == EFI_SUCCESS);
    assert(mInstalled->SetBandwidth(mInstalled, Path, Bandwidth, Bandwidth) == EFI_SUCCESS);
    assert(mInstalled->ReleasePath(mInstalled, Path) == EFI_SUCCESS);
  }

  Status = mInstalled->AcquirePath(
      mInstalled, "qcom,sm8450-pcie", 0x1000, 0x1100, &Path);
  assert(Status == EFI_SUCCESS && Path != INTERCONNECT_INVALID_PATH);
  assert(mInstalled->SetBandwidth(mInstalled, Path, 100, 200) == EFI_SUCCESS);
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_ACCESS_DENIED);
  assert(mInstalled!=NULL && mInitialized && mInterconnect->Paths[Path-1].InUse);
  Status = mInstalled->AcquirePath(
      mInstalled, "qcom,sm8450-ufs", 0x3000, 0x3001, &UfsPath);
  assert(Status == EFI_SUCCESS && UfsPath != INTERCONNECT_INVALID_PATH);
  assert(mInstalled->SetBandwidth(mInstalled, UfsPath, 50000, 100000) == EFI_SUCCESS);
  Status = mInstalled->AcquirePath(
      mInstalled, "qcom,sm8450-ufs", 0x3002, 0x3003, &UfsCfgPath);
  assert(Status == EFI_SUCCESS && UfsCfgPath != INTERCONNECT_INVALID_PATH);
  assert(mInstalled->SetBandwidth(mInstalled, UfsCfgPath, 0, 819200) == EFI_SUCCESS);
  assert(mInstalled->ReleasePath(mInstalled, UfsCfgPath) == EFI_SUCCESS);
  assert(mInstalled->ReleasePath(mInstalled, UfsPath) == EFI_SUCCESS);
  assert(mInstalled->ReleasePath(mInstalled, Path) == EFI_SUCCESS);
  assert(mRpmhCommands >= 3);
  mUninstallFails=TRUE;
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_DEVICE_ERROR);
  assert(mInitialized && mInstalled!=NULL);
  mUninstallFails=FALSE;mRpmhFailFrom=mRpmhWrites+1;
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_DEVICE_ERROR);
  assert(mInstalled==NULL && !mInitialized && mInterconnect->DeinitPending);
  assert(mRuntime.Rpmh==&mRpmh && mRuntime.CmdDb==&mCmdDb);
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_SUCCESS);
  assert(mInstalled==NULL && mInterconnect->DeinitPending && mRuntime.Rpmh==&mRpmh);
  mRpmhFailFrom=0;
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_SUCCESS);
  assert(mInterconnect==NULL && mRuntime.CmdDb==NULL && mRuntime.Rpmh==NULL);

  /* A one-shot init error can return failure after clearing uncertain votes. */
  mRpmhFailCall=mRpmhWrites+1;
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_DEVICE_ERROR);
  assert(mInterconnect==NULL && mInstalled==NULL && !mInitialized);
  mRpmhFailCall=0;
  /* Persistent init/cleanup failure keeps unpublished callback code resident. */
  mRpmhFailFrom=mRpmhWrites+1;
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_SUCCESS);
  assert(mInstalled==NULL && mInterconnect->DeinitPending && !mInitialized);
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_DEVICE_ERROR);
  mRpmhFailFrom=0;
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_SUCCESS);
  assert(mInstalled!=NULL && mInitialized && !mInterconnect->DeinitPending);
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_SUCCESS);

  mInstallFails=TRUE;
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_DEVICE_ERROR);
  assert(mInterconnect==NULL && mInstalled==NULL);
  mFailInstallCleanup=TRUE;
  assert(InterconnectEntryPoint((EFI_HANDLE)(UINTN)1,NULL)==EFI_SUCCESS);
  assert(mInterconnect!=NULL && mInterconnect->DeinitPending && mInstalled==NULL);
  mRpmhFailFrom=0;
  assert(InterconnectUnload((EFI_HANDLE)(UINTN)1)==EFI_SUCCESS);
  assert(mInterconnect==NULL && mRuntime.CmdDb==NULL);
  puts("Crane ICBCrDxe: active-path unload, retained init/install/clear ownership and retry passed");
  return 0;
}
