/** @file
 *  Standalone SM8450 Linux-style interconnect/BCM voter.
 *
 *  Crane does not locate the packaged Qualcomm ICB/NPA protocols. The target
 *  graph is the public Linux sm8450 interconnect topology; CmdDB supplies BCM
 *  addresses and Crane's RPMh library submits active-set commands through its
 *  local TCS driver.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/interconnect.h>
#include <Library/rpmh.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>

typedef struct {
  EFI_CMD_DB_PROTOCOL *CmdDb;
  RpmhDeviceContext   *Rpmh;
} ICB_RUNTIME;

STATIC ICB_RUNTIME                   mRuntime;
STATIC InterconnectDeviceContext    *mInterconnect;
STATIC EFI_INTERCONNECT_CR_PROTOCOL  mInterconnectProtocol;
STATIC BOOLEAN                       mInitialized;
STATIC BOOLEAN                       mOperationActive;
STATIC EFI_HANDLE                    mProtocolHandle;
/* The active SM8250 UART12 console shares QUP1 with the touch SE. Its
   lifetime extends beyond transient serial-engine transactions. */
STATIC INTERCONNECT_PATH_HANDLE      mConsolePaths[2];

STATIC EFI_STATUS
EnterOperation (OUT EFI_TPL *CallerTpl)
{
  if (gBS == NULL || gBS->RaiseTPL == NULL || gBS->RestoreTPL == NULL) {
    return EFI_NOT_READY;
  }
  if (EfiGetCurrentTpl () > TPL_NOTIFY) return EFI_ACCESS_DENIED;
  *CallerTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mOperationActive) {
    gBS->RestoreTPL (*CallerTpl);
    return EFI_NOT_READY;
  }
  /* The portable ICC mutex remains owned during synchronous RPMh commits.
     A notification consumer (for example a bridge I2C timer) must not
     preempt that owner and spin on the same CPU. TPL_NOTIFY excludes event
     callbacks while preserving hardware IRQ completion of the TCS request. */
  mOperationActive = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
LeaveOperation (IN EFI_TPL CallerTpl, IN EFI_STATUS Status)
{
  /* Restoring TPL can dispatch a pending client immediately. All core locks
     and lifetime mutations must finish, and recursion ownership must clear,
     before allowing that client to enter the adapter. */
  mOperationActive = FALSE;
  gBS->RestoreTPL (CallerTpl);
  return Status;
}

STATIC EFI_STATUS
AcquireConsolePaths (VOID)
{
  CONST InterconnectTargetContext *Target = mInterconnect->Target;
  CrDebugUartContext *Uart;
  UINT16 Index;
  for (Index = 0; Index < Target->ProviderCount; ++Index) {
    if (AsciiStrCmp (Target->Providers[Index].Compatible,
                     "qcom,sm8250-debug-uart") == 0) break;
  }
  if (Index == Target->ProviderCount) return EFI_SUCCESS;
  Uart = CrDalGetDebugUartContext ();
  if (Uart == NULL || !Uart->IsDebugUart || Uart->Type != DEBUG_UART_TYPE_GENI ||
      Uart->BaseAddress != 0x00a90000 || Uart->BaudRate != 115200) return EFI_UNSUPPORTED;
  /* Linux uart12 has qup-core and qup-config only. geni_serial_set_rate
     requires Bps_to_icc(CORE_2X_19_2_MHZ) = 9600 kB/s and baud / 1000 = 115 kB/s;
     geni_icc_set_bw uses the same average and peak. Do not rewrite the
     source graph's BCM keepalive policy or commandeer UART clocks/pins. */
  CONST UINT32 Source[] = {0x5200, 0x5202};
  CONST UINT32 Bandwidth[] = {9600, 115};
  for (Index = 0; Index < 2; ++Index) {
    EFI_STATUS Status = InterconnectAcquirePath (mInterconnect,
                          "qcom,sm8250-debug-uart", Source[Index], Source[Index] + 1,
                          &mConsolePaths[Index]);
    if (EFI_ERROR (Status)) return Status;
    Status = InterconnectSetBandwidth (mInterconnect, mConsolePaths[Index],
                                       Bandwidth[Index], Bandwidth[Index]);
    if (EFI_ERROR (Status)) return Status;
  }
  DEBUG ((DEBUG_WARN,"ICBCrDxe: UART12 console QUP1 core=9600 config=115 kB/s retained\n"));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CleanupContext (VOID)
{
  EFI_STATUS Status;

  /* DebugLib can still use the UART while handling startup/unload errors.
     Retain partially acquired console votes and their exact producer rather
     than withdrawing its dependency underneath the live serial console. */
  if (mConsolePaths[0] != INTERCONNECT_INVALID_PATH ||
      mConsolePaths[1] != INTERCONNECT_INVALID_PATH) return EFI_ACCESS_DENIED;

  if (mInterconnect != NULL) {
    Status = (EFI_STATUS)InterconnectLibDeinit (mInterconnect);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  mInterconnect = NULL;
  ZeroMem (&mRuntime, sizeof (mRuntime));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
FinishFailedStart (IN EFI_STATUS StartStatus)
{
  EFI_STATUS CleanupStatus = CleanupContext ();

  if (EFI_ERROR (CleanupStatus)) {
    /* Returning an entry-point error permits unloading our callbacks while
       the voter still owns hardware state. Keep the image resident, without
       publishing a usable protocol, until unload/reentry can clear it. */
    DEBUG ((DEBUG_ERROR,
            "ICBCrDxe: init %r, cleanup %r; retaining unpublished voter\n",
            StartStatus, CleanupStatus));
    return EFI_SUCCESS;
  }
  return StartStatus;
}

STATIC EFI_STATUS
UnloadInternal (IN EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  UINT16 Index;

  (VOID)ImageHandle;
  if (mInitialized && mInterconnect != NULL) {
    /* Consumers must release even zero-bandwidth handles before unloading. */
    for (Index = 0; Index < INTERCONNECT_MAX_PATHS; ++Index) {
      if (mInterconnect->Paths[Index].InUse) {
        return EFI_ACCESS_DENIED;
      }
    }
    Status = gBS->UninstallMultipleProtocolInterfaces (
               mProtocolHandle, &gEfiInterconnectCrProtocolGuid,
               &mInterconnectProtocol, NULL);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mInitialized = FALSE;
    mProtocolHandle = NULL;
  }
  /* A failed clear must keep the image and callback context alive. */
  return CleanupContext ();
}

EFI_STATUS
EFIAPI
InterconnectUnload (IN EFI_HANDLE ImageHandle)
{
  EFI_TPL CallerTpl;
  EFI_STATUS Status = EnterOperation (&CallerTpl);
  if (EFI_ERROR (Status)) return Status;
  Status = UnloadInternal (ImageHandle);
  return LeaveOperation (CallerTpl, Status);
}

STATIC
CR_STATUS
MapEfiStatus (
  IN EFI_STATUS Status
  )
{
  if (!EFI_ERROR (Status)) {
    return CR_SUCCESS;
  }
  if (Status == EFI_INVALID_PARAMETER) return CR_INVALID_PARAMETER;
  if (Status == EFI_NOT_FOUND) return CR_NOT_FOUND;
  if (Status == EFI_NOT_READY) return CR_ABORT;
  if (Status == EFI_TIMEOUT) return CR_TIMEOUT;
  if (Status == EFI_OUT_OF_RESOURCES) return CR_OUT_OF_RESOURCES;
  if (Status == EFI_UNSUPPORTED) return CR_UNSUPPORTED;
  if (Status == EFI_ACCESS_DENIED) return CR_BUSY;
  return CR_DEVICE_ERROR;
}

STATIC
CR_STATUS
CmdDbGetAddress (
  IN VOID *Context,
  IN CONST CHAR8 *Name,
  OUT UINT32 *Address
  )
{
  ICB_RUNTIME *Runtime = (ICB_RUNTIME *)Context;

  if (Runtime == NULL || Runtime->CmdDb == NULL ||
      Runtime->CmdDb->GetEntryAddressByName == NULL) {
    return CR_ABORT;
  }
  return MapEfiStatus (Runtime->CmdDb->GetEntryAddressByName (
                         Runtime->CmdDb, Name, Address));
}

STATIC
CR_STATUS
CmdDbGetAuxData (
  IN VOID *Context,
  IN CONST CHAR8 *Name,
  OUT UINT8 *Data,
  IN OUT UINT32 *Length
  )
{
  ICB_RUNTIME *Runtime = (ICB_RUNTIME *)Context;

  if (Runtime == NULL || Runtime->CmdDb == NULL ||
      Runtime->CmdDb->GetAuxDataByName == NULL) {
    return CR_ABORT;
  }
  return MapEfiStatus (Runtime->CmdDb->GetAuxDataByName (
                         Runtime->CmdDb, Name, Data, Length));
}

STATIC
CR_STATUS
WriteRpmh (
  IN VOID *Context,
  IN RpmhTcsCmd *Commands,
  IN UINT32 CommandCount
  )
{
  ICB_RUNTIME *Runtime = (ICB_RUNTIME *)Context;

  if (Runtime == NULL || Runtime->Rpmh == NULL) {
    return CR_ABORT;
  }
  return RpmhWrite (Runtime->Rpmh, Commands, CommandCount);
}

STATIC
EFI_STATUS
EFIAPI
ProtocolAcquirePath (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN CONST CHAR8 *ProviderCompatible,
  IN UINT32 SourceId,
  IN UINT32 DestinationId,
  OUT INTERCONNECT_PATH_HANDLE *Path
  )
{
  EFI_TPL CallerTpl;
  EFI_STATUS Status;
  if (This != &mInterconnectProtocol) return EFI_INVALID_PARAMETER;
  Status = EnterOperation (&CallerTpl);
  if (EFI_ERROR (Status)) return Status;
  if (!mInitialized || mInterconnect == NULL) {
    return LeaveOperation (CallerTpl, EFI_NOT_READY);
  }
  Status = InterconnectAcquirePath (
           mInterconnect, ProviderCompatible, SourceId, DestinationId, Path);
  return LeaveOperation (CallerTpl, Status);
}

STATIC
EFI_STATUS
EFIAPI
ProtocolSetBandwidth (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN INTERCONNECT_PATH_HANDLE Path,
  IN UINT64 AverageBandwidth,
  IN UINT64 PeakBandwidth
  )
{
  EFI_TPL CallerTpl;
  EFI_STATUS Status;
  if (This != &mInterconnectProtocol) return EFI_INVALID_PARAMETER;
  Status = EnterOperation (&CallerTpl);
  if (EFI_ERROR (Status)) return Status;
  if (!mInitialized || mInterconnect == NULL) {
    return LeaveOperation (CallerTpl, EFI_NOT_READY);
  }
  Status = InterconnectSetBandwidth (
           mInterconnect, Path, AverageBandwidth, PeakBandwidth);
  return LeaveOperation (CallerTpl, Status);
}

STATIC
EFI_STATUS
EFIAPI
ProtocolReleasePath (
  IN EFI_INTERCONNECT_CR_PROTOCOL *This,
  IN INTERCONNECT_PATH_HANDLE Path
  )
{
  EFI_TPL CallerTpl;
  EFI_STATUS Status;
  if (This != &mInterconnectProtocol) return EFI_INVALID_PARAMETER;
  Status = EnterOperation (&CallerTpl);
  if (EFI_ERROR (Status)) return Status;
  if (!mInitialized || mInterconnect == NULL) {
    return LeaveOperation (CallerTpl, EFI_NOT_READY);
  }
  Status = InterconnectReleasePath (mInterconnect, Path);
  return LeaveOperation (CallerTpl, Status);
}

STATIC EFI_STATUS
EntryInternal (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  CONST CR_DAL_DEVICE_INFO *Info;
  InterconnectIoOps Io;
  RpmhDeviceContext *Rpmh;

  (VOID)SystemTable;
  if (mInitialized) {
    return EFI_ALREADY_STARTED;
  }
  if (gBS == NULL || gBS->LocateProtocol == NULL ||
      gBS->InstallMultipleProtocolInterfaces == NULL ||
      gBS->UninstallMultipleProtocolInterfaces == NULL) {
    return EFI_NOT_READY;
  }

  /* Retry retained initialization/install cleanup before replacing callbacks. */
  if (mInterconnect != NULL) {
    Status = CleanupContext ();
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "ICBCrDxe: retained cleanup still pending: %r\n", Status));
      return EFI_SUCCESS;
    }
  }

  ZeroMem (&mRuntime, sizeof (mRuntime));
  Status = gBS->LocateProtocol (
             &gEfiCmdDBCrProtocolGuid, NULL, (VOID **)&mRuntime.CmdDb);
  if (EFI_ERROR (Status) || mRuntime.CmdDb == NULL) {
    DEBUG ((DEBUG_ERROR, "ICBCrDxe: Crane CmdDB unavailable: %r\n", Status));
    mRuntime.CmdDb = NULL;
    return EFI_NOT_READY;
  }

  Status = CrDalGetDeviceInfo (
             CrDalDeviceRpmh, 0, CR_DAL_DATA_REVISION_1,
             sizeof (RpmhDeviceContext), &Info);
  if (EFI_ERROR (Status) || Info == NULL || Info->Data == NULL) {
    return EFI_NOT_FOUND;
  }
  Rpmh = (RpmhDeviceContext *)(UINTN)Info->Data;
  mRuntime.Rpmh = Rpmh;
  Status = (EFI_STATUS)RpmhLibInit (&Rpmh);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ICBCrDxe: standalone RPMh init failed: %r\n", Status));
    mRuntime.Rpmh = NULL;
    return Status;
  }

  mInterconnect = NULL;
  ZeroMem (&Io, sizeof (Io));
  Io.GetAddress = CmdDbGetAddress;
  Io.GetAuxData = CmdDbGetAuxData;
  Io.WriteRpmh = WriteRpmh;
  Io.CmdDbContext = &mRuntime;
  Io.RpmhContext = &mRuntime;
  Io.MaxRpmhCommands = mRuntime.Rpmh->NumCmdsPerTcs;
  if (Io.MaxRpmhCommands > INTERCONNECT_MAX_RPMH_COMMANDS) {
    Io.MaxRpmhCommands = INTERCONNECT_MAX_RPMH_COMMANDS;
  }

  Status = (EFI_STATUS)InterconnectLibInit (
             &mInterconnect, CrDalGetInterconnectContext (), &Io);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ICBCrDxe: standalone Linux ICC init failed: %r\n", Status));
    return FinishFailedStart (Status);
  }

  Status = AcquireConsolePaths ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,"ICBCrDxe: active console path acquisition failed: %r\n",Status));
    return FinishFailedStart (Status);
  }

  ZeroMem (&mInterconnectProtocol, sizeof (mInterconnectProtocol));
  mInterconnectProtocol.Revision = EFI_INTERCONNECT_CR_PROTOCOL_REVISION;
  mInterconnectProtocol.AcquirePath = ProtocolAcquirePath;
  mInterconnectProtocol.SetBandwidth = ProtocolSetBandwidth;
  mInterconnectProtocol.ReleasePath = ProtocolReleasePath;
  mInitialized = TRUE;

  Status = gBS->InstallMultipleProtocolInterfaces (
             &ImageHandle, &gEfiInterconnectCrProtocolGuid,
             &mInterconnectProtocol, NULL);
  if (EFI_ERROR (Status)) {
    mInitialized = FALSE;
    return FinishFailedStart (Status);
  }
  mProtocolHandle = ImageHandle;

  DEBUG ((DEBUG_INFO, "ICBCrDxe: standalone Linux ICC/BCM voter ready\n"));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
InterconnectEntryPoint (
  IN EFI_HANDLE ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_TPL CallerTpl;
  EFI_STATUS Status = EnterOperation (&CallerTpl);
  if (EFI_ERROR (Status)) return Status;
  Status = EntryInternal (ImageHandle, SystemTable);
  return LeaveOperation (CallerTpl, Status);
}
