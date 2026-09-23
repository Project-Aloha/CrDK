/** @file
 *  Host regression coverage for the private MU bus adapters.
 *  SPDX-License-Identifier: MIT
 */
#include "MuBusTestCommon.h"
#include "../Driver/SPICrDxe/SPICrDxe.c"

EFI_GUID gQcomSPIProtocolGuid;
EFI_GUID gEfiSpiHcProtocolGuid;
EFI_GUID gEfiSpiCrProtocolGuid;
static unsigned Calls;
static UINT32 Result;
static UINT32 ExpectedLength;
static UINT32 ExpectedWrite;
static UINT8 Command[] = {0x9f, 0xab, 0xcd};
static MU_SPI_STATUS EFIAPI Transfer (VOID *Handle, MU_SPI_DEVICE_INFO *Info,
    CONST UINT8 *Write, UINT32 WriteLength, UINT8 *Read, UINT32 ReadLength) {
  assert(Handle == (VOID *)0x3333 && Info->BoardInfo.SlaveNumber == 3);
  assert(Info->DeviceParameters.CsMode == MU_SPI_CS_DEASSERT);
  assert(Info->DeviceParameters.MaxSlaveFrequencyHz == 5000000);
  assert(WriteLength == ExpectedLength && ReadLength == ExpectedLength);
  for (UINT32 i = 0; i < ExpectedWrite; ++i) assert(Write[i] == Command[i]);
  for (UINT32 i = ExpectedWrite; i < WriteLength; ++i) assert(Write[i] == 0);
  for (UINT32 i = 0; i < ReadLength; ++i) Read[i] = (UINT8)(0x60+i);
  ++Calls;
  return Result;
}

static unsigned Opens;
static unsigned Closes;
static MU_SPI_STATUS OpenResult = MU_SPI_SUCCESS;
static MU_SPI_STATUS EFIAPI Open(MU_SPI_INSTANCE Instance, VOID **Handle) {
  ++Opens;
  if (Instance != 0) return MU_SPI_ERROR_INVALID_PARAM;
  if (OpenResult != MU_SPI_SUCCESS) {
    *Handle = NULL;
    return OpenResult;
  }
  *Handle = (VOID *)0x3333;
  return MU_SPI_SUCCESS;
}
static MU_SPI_STATUS EFIAPI Close(VOID *Handle) {
  assert(Handle == (VOID *)0x3333);
  ++Closes;
  return MU_SPI_SUCCESS;
}
static EFI_STATUS EFIAPI StandardSelect(CONST EFI_SPI_HC_PROTOCOL *This,
    CONST EFI_SPI_PERIPHERAL *Peripheral, BOOLEAN Pin) {
  assert(This == StandardBackend && Peripheral != NULL && Pin);
  return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI StandardClock(CONST EFI_SPI_HC_PROTOCOL *This,
    CONST EFI_SPI_PERIPHERAL *Peripheral, UINT32 *Hz) {
  assert(This == StandardBackend && Peripheral != NULL && *Hz == 333333);
  *Hz = 300000;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StandardTransaction(CONST EFI_SPI_HC_PROTOCOL *This,
    EFI_SPI_BUS_TRANSACTION *Transaction) {
  assert(This == StandardBackend && Transaction != NULL);
  return EFI_TIMEOUT;
}
static void TestEntry(void) {
  MU_SPI_PROTOCOL Mu = {MU_SPI_PROTOCOL_REVISION, Open, Transfer, Close};
  EFI_SPI_HC_PROTOCOL Host = { .Attributes = HC_SUPPORTS_READ_ONLY_OPERATIONS,
    .FrameSizeSupportMask = BIT7, .MaximumTransferBytes = 256,
    .ChipSelect = StandardSelect, .Clock = StandardClock,
    .Transaction = StandardTransaction };
  EFI_SPI_CR_PROTOCOL *Protocol;
  EFI_SPI_PERIPHERAL Peripheral = {0};
  EFI_SPI_BUS_TRANSACTION Transaction = {0};
  UINT32 Hz = 333333;
  StandardGuid = &gEfiSpiHcProtocolGuid;
  PrivateGuid = &gQcomSPIProtocolGuid;
  PublishedGuid = &gEfiSpiCrProtocolGuid;
  StandardBackend = &Host;
  PrivateBackend = &Mu;
  InitEntryServices();
  StandardLocateStatus = EFI_OUT_OF_RESOURCES;
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_OUT_OF_RESOURCES);
  assert(PrivateLocateCalls == 0 && Allocations == 0);
  StandardLocateStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_DEVICE_ERROR);
  assert(PrivateLocateCalls == 0 && Allocations == 0 && mInstances == NULL);
  InstallStatus = EFI_SUCCESS;
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  Protocol = LastInstalled;
  assert(Protocol->HostHandle == (EFI_HANDLE)0x4444 && Protocol->Host == &Host);
  assert(Protocol->MaximumTransferBytes == 256 && Protocol->FrameSizeSupportMask == BIT7);
  assert(Protocol->Attributes == Host.Attributes && Protocol->Qcom == NULL);
  assert(Protocol->ChipSelect(Protocol, &Peripheral, TRUE) == EFI_DEVICE_ERROR);
  assert(Protocol->Clock(Protocol, &Peripheral, &Hz) == EFI_SUCCESS && Hz == 300000);
  assert(Protocol->Transaction(Protocol, &Transaction) == EFI_TIMEOUT);
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_ALREADY_STARTED);
  FreePool(mInstances); mInstances = NULL;
  StandardLocateStatus = EFI_NOT_FOUND;
  InstallStatus = EFI_OUT_OF_RESOURCES;
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_OUT_OF_RESOURCES);
  assert(Opens == 0 && Closes == 0 && mInstances == NULL && Allocations == 0);
  InstallStatus = EFI_SUCCESS;
  assert(SpiCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  Protocol = LastInstalled;
  assert(Protocol->Instance == 0 && Protocol->Qcom == &Mu);
  assert(Protocol->MaximumTransferBytes == 0x00ffffff);
  assert(Opens == 0 && Closes == 0 && Allocations == 1);
  {
    MU_SPI_DEVICE_INFO Info = {0};
    UINT8 Tx[1] = {0};
    UINT8 Rx[1] = {0};

    Info.DeviceParameters.MaxSlaveFrequencyHz = 5000000;
    Info.BoardInfo.SlaveNumber = 3;
    Info.TransferParameters.NumBits = 8;
    Result = MU_SPI_SUCCESS;
    ExpectedLength = 1;
    ExpectedWrite = 0;
    OpenResult = MU_SPI_ERROR_HW_INFO_ALLOCATION;
    assert(Protocol->MuTransfer (Protocol, &Info, Tx, sizeof (Tx), Rx,
                                 sizeof (Rx)) == EFI_OUT_OF_RESOURCES);
    assert(Opens == 1 && Protocol->QcomHandle == NULL);
    OpenResult = MU_SPI_SUCCESS;
    assert(Protocol->MuTransfer (Protocol, &Info, Tx, sizeof (Tx), Rx,
                                 sizeof (Rx)) == EFI_SUCCESS);
    assert(Opens == 2 && Protocol->QcomHandle == (VOID *)0x3333);
    assert(Protocol->Qcom->Close (Protocol->QcomHandle) == MU_SPI_SUCCESS);
  }
  FreePool(mInstances); mInstances = NULL;
}
int main(void) {
  MU_SPI_PROTOCOL Mu = { .Transfer = Transfer };
  SPI_CR_INSTANCE Instance = { .Protocol = { .Qcom = &Mu, .QcomHandle = (VOID *)0x3333 }};
  UINT32 ChipSelect = 3;
  EFI_SPI_PART Part = { .MaxClockHz = 5000000, .ChipSelectPolarity = FALSE };
  EFI_SPI_PERIPHERAL Peripheral = { .SpiPart = &Part, .MaxClockHz = 10000000, .ChipSelectParameter = &ChipSelect };
  UINT8 Read[4] = {0};
  EFI_SPI_BUS_TRANSACTION Transaction = { .SpiPeripheral = &Peripheral, .BusWidth = 1, .FrameSize = 8,
    .WriteBuffer = Command, .WriteBytes = 3, .ReadBuffer = Read, .ReadBytes = 4 };
  UINT32 Hz = 0;
  InitBootServices();
  Transaction.TransactionType = SPI_TRANSACTION_WRITE_THEN_READ;
  ExpectedLength = 7; ExpectedWrite = 3;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_SUCCESS);
  Instance.Busy = 1;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_ALREADY_STARTED);
  Instance.Busy = 0;
  assert(Calls == 1 && Allocations == 0 && Read[0] == 0x63 && Read[3] == 0x66);
  Transaction.TransactionType = SPI_TRANSACTION_READ_ONLY;
  Transaction.WriteBytes = 0; Transaction.WriteBuffer = NULL;
  ExpectedLength = 4; ExpectedWrite = 0;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_SUCCESS);
  assert(Read[0] == 0x60 && Read[3] == 0x63 && Allocations == 0);
  Transaction.TransactionType = SPI_TRANSACTION_WRITE_ONLY;
  Transaction.WriteBytes = 3; Transaction.WriteBuffer = Command;
  Transaction.ReadBytes = 0; Transaction.ReadBuffer = NULL;
  ExpectedLength = 3; ExpectedWrite = 3;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_SUCCESS);
  assert(Allocations == 0);
  Transaction.TransactionType = SPI_TRANSACTION_FULL_DUPLEX;
  Transaction.ReadBytes = 3; Transaction.ReadBuffer = Read;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_SUCCESS);
  Result = 21;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_TIMEOUT);
  ChipSelect = 4;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_INVALID_PARAMETER);
  ChipSelect = 3; Transaction.FrameSize = 16;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_BAD_BUFFER_SIZE);
  Transaction.FrameSize = 8; Transaction.BusWidth = 4;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_INVALID_PARAMETER);
  Transaction.BusWidth = 1; Transaction.TransactionType = (EFI_SPI_TRANSACTION_TYPE)99;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_UNSUPPORTED);
  assert(SpiCrChipSelect(&Instance.Protocol, &Peripheral, FALSE) == EFI_UNSUPPORTED);
  assert(SpiCrClock(&Instance.Protocol, &Peripheral, &Hz) == EFI_UNSUPPORTED && Hz == 0);
  Transaction.TransactionType = SPI_TRANSACTION_READ_ONLY;
  Transaction.WriteBytes = 0; Transaction.WriteBuffer = NULL;
  FailAllocationAt = AllocationAttempts + 1;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_OUT_OF_RESOURCES);
  FailAllocationAt = AllocationAttempts + 2;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_OUT_OF_RESOURCES);
  assert(Allocations == 0 && Instance.Busy == 0);
  FailAllocationAt = 0;
  Transaction.ReadBytes = 0x01000000;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_BAD_BUFFER_SIZE);
  Transaction.ReadBytes = 0x00ffffff;
  Transaction.TransactionType = SPI_TRANSACTION_WRITE_THEN_READ;
  Transaction.WriteBytes = 1; Transaction.WriteBuffer = Command;
  assert(SpiCrTransaction(&Instance.Protocol, &Transaction) == EFI_BAD_BUFFER_SIZE);
  {
    MU_SPI_DEVICE_INFO Info = {0};
    Info.TransferParameters.NumBits = 16;
    assert(SpiCrMuTransfer(&Instance.Protocol, &Info, Command, 3, Read, 3) == EFI_BAD_BUFFER_SIZE);
    Info.TransferParameters.NumBits = 8;
    assert(SpiCrMuTransfer(&Instance.Protocol, &Info, Command, 0x01000000, Read, 0x01000000) == EFI_BAD_BUFFER_SIZE);
  }
  TestEntry();
  assert(Allocations == 0);
  puts("SPI: duplex, one-way, continuous write/read, limits, errors, entry and PI forwarding passed");
  return 0;
}
