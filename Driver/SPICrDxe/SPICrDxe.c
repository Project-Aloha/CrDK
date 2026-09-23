/** @file
 *  Crane adapter for the MU Qualcomm SPI protocol.
 *
 *  The prebuilt MU SPIDxe remains the only owner of GENI/SPI registers.  This
 *  driver opens each usable MU instance and translates PI host-controller
 *  transactions to SpiDeviceInfoType-compatible data.
 */

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFIMuBusProtocol.h>
#include <Protocol/EFISpiCrProtocol.h>

#define SPI_CR_DEFAULT_FREQUENCY_HZ  1000000U
#define SPI_CR_MAX_SLAVE_NUMBER     3U
#define SPI_CR_FRAME_SIZE_MASK      0xFFFFFF80U
#define SPI_CR_MAX_TRANSFER_BYTES   0x00FFFFFFU

typedef struct {
  EFI_HANDLE          AliasHandle;
  EFI_SPI_CR_PROTOCOL Protocol;
  volatile UINT32     Busy;
} SPI_CR_INSTANCE;

STATIC MU_SPI_PROTOCOL *mMuSpi;
STATIC SPI_CR_INSTANCE *mInstances;

STATIC SPI_CR_INSTANCE *
SpiCrInstanceFromProtocol (
  IN EFI_SPI_CR_PROTOCOL *This
  )
{
  if (This == NULL) {
    return NULL;
  }
  return BASE_CR (This, SPI_CR_INSTANCE, Protocol);
}

STATIC EFI_STATUS
MuSpiStatusToEfi (
  IN MU_SPI_STATUS Status
  )
{
  if (Status == MU_SPI_SUCCESS) {
    return EFI_SUCCESS;
  }
  if (Status == MU_SPI_ERROR_INVALID_PARAM) {
    return EFI_INVALID_PARAMETER;
  }
  if (Status == MU_SPI_ERROR_UNSUPPORTED_IN_ISLAND_MODE) {
    return EFI_UNSUPPORTED;
  }
  if (Status == MU_SPI_ERROR_TRANSFER_TIMEOUT) {
    return EFI_TIMEOUT;
  }
  if (Status == MU_SPI_TRANSFER_FORCE_TERMINATED) {
    return EFI_ABORTED;
  }
  if (Status == MU_SPI_ERROR_PENDING_TRANSFER) {
    return EFI_NOT_READY;
  }
  if ((Status == MU_SPI_ERROR_HW_INFO_ALLOCATION) ||
      (Status == MU_SPI_ERROR_MEM_ALLOC) ||
      (Status == MU_SPI_ERROR_HANDLE_ALLOCATION)) {
    return EFI_OUT_OF_RESOURCES;
  }
  return EFI_DEVICE_ERROR;
}

STATIC UINT32
SpiCrChipSelectFromPeripheral (
  IN CONST EFI_SPI_PERIPHERAL *SpiPeripheral
  )
{
  if ((SpiPeripheral == NULL) || (SpiPeripheral->ChipSelectParameter == NULL)) {
    return MAX_UINT32;
  }
  /* Board descriptions carry the MU slave number as a UINT32.  Copying avoids
     an unaligned load when the board structure is packed. */
  {
    UINT32 SlaveNumber;

    CopyMem (&SlaveNumber, SpiPeripheral->ChipSelectParameter,
             sizeof (SlaveNumber));
    return SlaveNumber;
  }
}

STATIC EFI_STATUS
SpiCrBuildDeviceInfo (
  IN SPI_CR_INSTANCE          *Instance,
  IN CONST EFI_SPI_BUS_TRANSACTION *BusTransaction,
  OUT MU_SPI_DEVICE_INFO      *DeviceInfo
  )
{
  CONST EFI_SPI_PERIPHERAL *Peripheral;
  CONST EFI_SPI_PART        *Part;
  UINT32                    ClockHz;

  if ((Instance == NULL) || (BusTransaction == NULL) || (DeviceInfo == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  Peripheral = BusTransaction->SpiPeripheral;
  if ((Peripheral == NULL) || (BusTransaction->BusWidth != 1) ||
      (BusTransaction->FrameSize < 8) || (BusTransaction->FrameSize > 32)) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (DeviceInfo, sizeof (*DeviceInfo));
  Part = Peripheral->SpiPart;
  ClockHz = (Peripheral->MaxClockHz != 0) ? Peripheral->MaxClockHz :
            ((Part != NULL) ? Part->MaxClockHz : 0);
  if ((Part != NULL) && (Part->MaxClockHz != 0) &&
      ((ClockHz == 0) || (ClockHz > Part->MaxClockHz))) {
    ClockHz = Part->MaxClockHz;
  }
  if (ClockHz == 0) {
    ClockHz = SPI_CR_DEFAULT_FREQUENCY_HZ;
  }

  DeviceInfo->DeviceParameters.ClockMode = MU_SPI_CLK_NORMAL;
  DeviceInfo->DeviceParameters.ClockPolarity = Peripheral->ClockPolarity
                                                   ? MU_SPI_CLK_IDLE_HIGH
                                                   : MU_SPI_CLK_IDLE_LOW;
  DeviceInfo->DeviceParameters.ShiftMode = Peripheral->ClockPhase
                                              ? MU_SPI_OUTPUT_FIRST_MODE
                                              : MU_SPI_INPUT_FIRST_MODE;
  DeviceInfo->DeviceParameters.MinSlaveFrequencyHz = 0;
  DeviceInfo->DeviceParameters.MaxSlaveFrequencyHz = ClockHz;
  DeviceInfo->DeviceParameters.CsPolarity =
      ((Part != NULL) && Part->ChipSelectPolarity)
        ? MU_SPI_CS_ACTIVE_HIGH : MU_SPI_CS_ACTIVE_LOW;
  /* One private transfer contains the complete PI transaction, including
     command bytes and the following read clocks.  Waipio's wrapper interprets
     DEASSERT as the final descriptor and leaves CS continuous within it. */
  DeviceInfo->DeviceParameters.CsMode = MU_SPI_CS_DEASSERT;
  DeviceInfo->DeviceParameters.HsMode = FALSE;

  DeviceInfo->BoardInfo.SlaveNumber = SpiCrChipSelectFromPeripheral (Peripheral);
  if (DeviceInfo->BoardInfo.SlaveNumber > SPI_CR_MAX_SLAVE_NUMBER) {
    return EFI_INVALID_PARAMETER;
  }
  DeviceInfo->BoardInfo.CoreMode = MU_SPI_CORE_MODE_MASTER;
  DeviceInfo->TransferParameters.NumBits = BusTransaction->FrameSize;
  DeviceInfo->TransferParameters.LoopbackMode = MU_SPI_LOOPBACK_DISABLED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
SpiCrMuTransfer (
  IN EFI_SPI_CR_PROTOCOL *This,
  IN MU_SPI_DEVICE_INFO  *DeviceInfo,
  IN CONST UINT8         *WriteBuffer,
  IN UINT32               WriteLength,
  OUT UINT8              *ReadBuffer,
  IN UINT32               ReadLength
  )
{
  SPI_CR_INSTANCE *Instance;
  MU_SPI_STATUS    MuStatus;
  UINT32           FrameBytes;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Qcom == NULL) ||
      (Instance->Protocol.QcomHandle == NULL) || (DeviceInfo == NULL) ||
      (Instance->Protocol.Qcom->Transfer == NULL) ||
      (WriteBuffer == NULL) || (ReadBuffer == NULL) ||
      (WriteLength == 0) || (WriteLength != ReadLength)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((DeviceInfo->BoardInfo.SlaveNumber > MU_SPI_MAX_SLAVE_NUMBER) ||
      (DeviceInfo->TransferParameters.NumBits < MU_SPI_MIN_FRAME_BITS) ||
      (DeviceInfo->TransferParameters.NumBits > MU_SPI_MAX_FRAME_BITS)) {
    return EFI_INVALID_PARAMETER;
  }
  FrameBytes = (DeviceInfo->TransferParameters.NumBits > 16) ? 4U :
                 ((DeviceInfo->TransferParameters.NumBits > 8) ? 2U : 1U);
  if ((WriteLength > SPI_CR_MAX_TRANSFER_BYTES) ||
      ((WriteLength % FrameBytes) != 0)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }
  MuStatus = Instance->Protocol.Qcom->Transfer (
      Instance->Protocol.QcomHandle, DeviceInfo, WriteBuffer, WriteLength,
      ReadBuffer, ReadLength);
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  return MuSpiStatusToEfi (MuStatus);
}

STATIC EFI_STATUS
EFIAPI
SpiCrChipSelect (
  IN EFI_SPI_CR_PROTOCOL      *This,
  IN CONST EFI_SPI_PERIPHERAL *SpiPeripheral,
  IN BOOLEAN                   PinValue
  )
{
  SPI_CR_INSTANCE *Instance;

  Instance = SpiCrInstanceFromProtocol (This);
  if (Instance == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Host != NULL) {
    return Instance->Protocol.Host->ChipSelect (
        Instance->Protocol.Host, SpiPeripheral, PinValue);
  }
  if (SpiCrChipSelectFromPeripheral (SpiPeripheral) > SPI_CR_MAX_SLAVE_NUMBER) {
    return EFI_INVALID_PARAMETER;
  }
  /* MU controls CS within transfer(), with no standalone pin operation. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
SpiCrClock (
  IN EFI_SPI_CR_PROTOCOL      *This,
  IN CONST EFI_SPI_PERIPHERAL *SpiPeripheral,
  IN UINT32                   *ClockHz
  )
{
  SPI_CR_INSTANCE *Instance;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (ClockHz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Host != NULL) {
    return Instance->Protocol.Host->Clock (
        Instance->Protocol.Host, SpiPeripheral, ClockHz);
  }
  /* The private API accepts a frequency ceiling on each transfer, but has no
     standalone clock control or way to report the selected actual rate. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
SpiCrTransaction (
  IN EFI_SPI_CR_PROTOCOL    *This,
  IN EFI_SPI_BUS_TRANSACTION *BusTransaction
  )
{
  SPI_CR_INSTANCE       *Instance;
  MU_SPI_DEVICE_INFO     DeviceInfo;
  EFI_STATUS             Status;
  CONST UINT8           *WriteBuffer;
  UINT8                 *ReadBuffer;
  UINT32                 WriteLength;
  UINT32                 ReadLength;
  UINT32                 TransferLength;
  UINT32                 FrameBytes;
  UINT32                 ReadOffset;
  UINT8                 *TxScratch;
  UINT8                 *RxScratch;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (BusTransaction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Host != NULL) {
    return Instance->Protocol.Host->Transaction (
        Instance->Protocol.Host, BusTransaction);
  }
  WriteBuffer = BusTransaction->WriteBuffer;
  ReadBuffer  = BusTransaction->ReadBuffer;
  WriteLength = BusTransaction->WriteBytes;
  ReadLength  = BusTransaction->ReadBytes;

  if (((WriteLength != 0) && (WriteBuffer == NULL)) ||
      ((ReadLength != 0) && (ReadBuffer == NULL))) {
    return EFI_INVALID_PARAMETER;
  }
  Status = SpiCrBuildDeviceInfo (Instance, BusTransaction, &DeviceInfo);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  FrameBytes = (BusTransaction->FrameSize > 16) ? 4U :
                 ((BusTransaction->FrameSize > 8) ? 2U : 1U);
  if (((WriteLength % FrameBytes) != 0) ||
      ((ReadLength % FrameBytes) != 0)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  ReadOffset = 0;
  switch (BusTransaction->TransactionType) {
    case SPI_TRANSACTION_FULL_DUPLEX:
      if ((WriteLength == 0) || (WriteLength != ReadLength)) {
        return EFI_BAD_BUFFER_SIZE;
      }
      return SpiCrMuTransfer (This, &DeviceInfo, WriteBuffer, WriteLength,
                              ReadBuffer, ReadLength);
    case SPI_TRANSACTION_WRITE_ONLY:
      if ((WriteLength == 0) || (ReadLength != 0)) {
        return EFI_BAD_BUFFER_SIZE;
      }
      TransferLength = WriteLength;
      break;
    case SPI_TRANSACTION_READ_ONLY:
      if ((ReadLength == 0) || (WriteLength != 0)) {
        return EFI_BAD_BUFFER_SIZE;
      }
      TransferLength = ReadLength;
      break;
    case SPI_TRANSACTION_WRITE_THEN_READ:
      if ((WriteLength == 0) || (ReadLength == 0) ||
          (WriteLength > MAX_UINT32 - ReadLength)) {
        return EFI_BAD_BUFFER_SIZE;
      }
      ReadOffset = WriteLength;
      TransferLength = WriteLength + ReadLength;
      break;
    default:
      return EFI_UNSUPPORTED;
  }
  if (TransferLength > SPI_CR_MAX_TRANSFER_BYTES) {
    return EFI_BAD_BUFFER_SIZE;
  }

  /* MU creates one full-duplex descriptor and ignores ReadLength.  Supply
     equal buffers, combining write-then-read so CS has no gap between phases. */
  TxScratch = AllocateZeroPool (TransferLength);
  if (TxScratch == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  RxScratch = AllocateZeroPool (TransferLength);
  if (RxScratch == NULL) {
    FreePool (TxScratch);
    return EFI_OUT_OF_RESOURCES;
  }
  if (WriteLength != 0) {
    CopyMem (TxScratch, WriteBuffer, WriteLength);
  }
  Status = SpiCrMuTransfer (This, &DeviceInfo, TxScratch, TransferLength,
                            RxScratch, TransferLength);
  if (!EFI_ERROR (Status) && (ReadLength != 0)) {
    CopyMem (ReadBuffer, RxScratch + ReadOffset, ReadLength);
  }
  FreePool (RxScratch);
  FreePool (TxScratch);
  return Status;
}

STATIC EFI_STATUS
SpiCrInstallStandardInstances (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS           Status;
  EFI_STATUS           FailureStatus;
  EFI_HANDLE          *Handles;
  UINTN                HandleCount;
  UINTN                Index;
  UINTN                Installed;
  EFI_SPI_HC_PROTOCOL *Host;

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
      ByProtocol, &gEfiSpiHcProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((HandleCount == 0) || (Handles == NULL)) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
    return EFI_NOT_FOUND;
  }
  mInstances = AllocateZeroPool (HandleCount * sizeof (*mInstances));
  if (mInstances == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  Installed = 0;
  FailureStatus = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; ++Index) {
    EFI_HANDLE AliasHandle;

    Host = NULL;
    Status = gBS->OpenProtocol (
        Handles[Index], &gEfiSpiHcProtocolGuid, (VOID **)&Host,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR (Status) || (Host == NULL) ||
        (Host->ChipSelect == NULL) || (Host->Clock == NULL) ||
        (Host->Transaction == NULL)) {
      continue;
    }
    mInstances[Installed].Protocol.Revision = EFI_SPI_CR_PROTOCOL_REVISION;
    mInstances[Installed].Protocol.HostHandle = Handles[Index];
    mInstances[Installed].Protocol.Host = Host;
    mInstances[Installed].Protocol.Attributes = Host->Attributes;
    mInstances[Installed].Protocol.FrameSizeSupportMask =
        Host->FrameSizeSupportMask;
    mInstances[Installed].Protocol.MaximumTransferBytes =
        Host->MaximumTransferBytes;
    mInstances[Installed].Protocol.ChipSelect = SpiCrChipSelect;
    mInstances[Installed].Protocol.Clock = SpiCrClock;
    mInstances[Installed].Protocol.Transaction = SpiCrTransaction;
    mInstances[Installed].Protocol.MuTransfer = SpiCrMuTransfer;
    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiSpiCrProtocolGuid,
        &mInstances[Installed].Protocol, NULL);
    if (EFI_ERROR (Status)) {
      FailureStatus = Status;
      continue;
    }
    mInstances[Installed++].AliasHandle = AliasHandle;
  }
  FreePool (Handles);
  if (Installed != 0) {
    return EFI_SUCCESS;
  }
  FreePool (mInstances);
  mInstances = NULL;
  return FailureStatus;
}

EFI_STATUS
EFIAPI
SpiCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;
  EFI_STATUS FailureStatus;
  UINTN      Index;
  UINTN      Installed;

  (VOID)SystemTable;
  if (mInstances != NULL) {
    return EFI_ALREADY_STARTED;
  }
  Status = SpiCrInstallStandardInstances (ImageHandle);
  if (Status != EFI_NOT_FOUND) {
    return Status;
  }

  Status = gBS->LocateProtocol (
      &gQcomSPIProtocolGuid, NULL, (VOID **)&mMuSpi);
  if (EFI_ERROR (Status) || (mMuSpi == NULL) ||
      (mMuSpi->Revision < MU_SPI_PROTOCOL_REVISION) ||
      (mMuSpi->Open == NULL) || (mMuSpi->Transfer == NULL) ||
      (mMuSpi->Close == NULL)) {
    return EFI_NOT_FOUND;
  }

  mInstances = AllocateZeroPool (MU_SPI_INSTANCE_MAX * sizeof (*mInstances));
  if (mInstances == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Installed = 0;
  FailureStatus = EFI_NOT_FOUND;
  for (Index = MU_SPI_INSTANCE_001; Index < MU_SPI_INSTANCE_MAX; ++Index) {
    VOID          *Handle;
    MU_SPI_STATUS  MuStatus;
    EFI_HANDLE     AliasHandle;

    Handle = NULL;
    MuStatus = mMuSpi->Open ((MU_SPI_INSTANCE)Index, &Handle);
    if ((MuStatus != MU_SPI_SUCCESS) || (Handle == NULL)) {
      continue;
    }

    mInstances[Installed].Protocol.Revision   = EFI_SPI_CR_PROTOCOL_REVISION;
    mInstances[Installed].Protocol.Qcom       = mMuSpi;
    mInstances[Installed].Protocol.Instance   = (MU_SPI_INSTANCE)Index;
    mInstances[Installed].Protocol.QcomHandle = Handle;
    /* The adapter implements these using one equal-length MU descriptor. */
    mInstances[Installed].Protocol.Attributes =
        HC_SUPPORTS_WRITE_ONLY_OPERATIONS | HC_SUPPORTS_READ_ONLY_OPERATIONS |
        HC_SUPPORTS_WRITE_THEN_READ_OPERATIONS;
    mInstances[Installed].Protocol.FrameSizeSupportMask = SPI_CR_FRAME_SIZE_MASK;
    mInstances[Installed].Protocol.MaximumTransferBytes =
        SPI_CR_MAX_TRANSFER_BYTES;
    mInstances[Installed].Protocol.HostHandle = NULL;
    mInstances[Installed].Protocol.Host       = NULL;
    ZeroMem (&mInstances[Installed].Protocol.DeviceInfo,
             sizeof (mInstances[Installed].Protocol.DeviceInfo));
    mInstances[Installed].Protocol.DeviceInfo.DeviceParameters.MaxSlaveFrequencyHz =
        SPI_CR_DEFAULT_FREQUENCY_HZ;
    mInstances[Installed].Protocol.ChipSelect = SpiCrChipSelect;
    mInstances[Installed].Protocol.Clock      = SpiCrClock;
    mInstances[Installed].Protocol.Transaction = SpiCrTransaction;
    mInstances[Installed].Protocol.MuTransfer  = SpiCrMuTransfer;

    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiSpiCrProtocolGuid,
        &mInstances[Installed].Protocol, NULL);
    if (EFI_ERROR (Status)) {
      mMuSpi->Close (Handle);
      FailureStatus = Status;
      continue;
    }
    mInstances[Installed].AliasHandle = AliasHandle;
    ++Installed;
  }

  if (Installed == 0) {
    FreePool (mInstances);
    mInstances = NULL;
    return FailureStatus;
  }
  (VOID)ImageHandle;
  return EFI_SUCCESS;
}
