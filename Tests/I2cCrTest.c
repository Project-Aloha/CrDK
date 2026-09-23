/** @file
 *  Host regression coverage for the private MU bus adapters.
 *  SPDX-License-Identifier: MIT
 */
#include "MuBusTestCommon.h"
#include "../Driver/I2CCrDxe/I2CCrDxe.c"

EFI_GUID gQcomI2CProtocolGuid;
EFI_GUID gEfiI2cMasterProtocolGuid;
EFI_GUID gEfiI2cCrProtocolGuid;
static unsigned Calls;
static UINT32 Result;
static BOOLEAN Short;
static UINT32 SavedFlags[2];
static MU_I2C_STATUS EFIAPI Transfer (VOID *Handle, MU_I2C_SLAVE_CONFIG *Config,
    MU_I2C_DESCRIPTOR *Desc, UINT16 Count, MU_I2C_CALLBACK Callback,
    VOID *Context, UINT32 Delay, UINT32 *Transferred) {
  assert(Handle == (VOID *)0x3333 && Config->SlaveAddress == 0x36);
  assert(Config->BusFrequencyKHz == 400 && Config->SlaveMaxClockStretchUs == 500);
  assert(Callback == NULL && Context == NULL && Delay == 0);
  assert(Count == 2 && Desc[0].Length == 2 && Desc[1].Length == 3);
  assert(Desc[0].Buffer[0] == 0x12 && Desc[1].Buffer[0] == 0x34);
  SavedFlags[0] = Desc[0].Flags;
  SavedFlags[1] = Desc[1].Flags;
  ++Calls;
  *Transferred = Short ? 4 : 5;
  return Result;
}

static unsigned Opens;
static unsigned Closes;
static MU_I2C_STATUS EFIAPI Open(MU_I2C_INSTANCE Instance, VOID **Handle) {
  ++Opens;
  if (Instance != 1) return MU_I2C_ERROR_UNSUPPORTED_INSTANCE;
  *Handle = (VOID *)0x3333;
  return MU_I2C_SUCCESS;
}
static MU_I2C_STATUS EFIAPI Close(VOID *Handle) {
  assert(Handle == (VOID *)0x3333);
  ++Closes;
  return MU_I2C_SUCCESS;
}
static EFI_STATUS EFIAPI StandardFrequency(CONST EFI_I2C_MASTER_PROTOCOL *This,
    UINTN *Hz) {
  assert(This == StandardBackend && *Hz == 333333);
  *Hz = 300000;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI StandardReset(CONST EFI_I2C_MASTER_PROTOCOL *This) {
  assert(This == StandardBackend);
  return EFI_DEVICE_ERROR;
}
static EFI_STATUS EFIAPI StandardRequest(CONST EFI_I2C_MASTER_PROTOCOL *This,
    UINTN Address, EFI_I2C_REQUEST_PACKET *Packet, EFI_EVENT Event,
    EFI_STATUS *Status) {
  assert(This == StandardBackend && Address == 0x456);
  assert(Packet != NULL && Event == (EFI_EVENT)0x9999 && Status == NULL);
  return EFI_TIMEOUT;
}
static void TestEntry(void) {
  MU_I2C_PROTOCOL Mu = { .Revision = MU_I2C_PROTOCOL_REVISION, .Open = Open,
    .Transfer = Transfer, .Close = Close };
  EFI_I2C_CONTROLLER_CAPABILITIES Caps = {sizeof(Caps), 100, 100, 200};
  EFI_I2C_MASTER_PROTOCOL Master = {StandardFrequency, StandardReset,
    StandardRequest, &Caps};
  EFI_I2C_REQUEST_PACKET Packet = {0};
  EFI_I2C_CR_PROTOCOL *Protocol;
  UINTN Hz = 333333;
  StandardGuid = &gEfiI2cMasterProtocolGuid;
  PrivateGuid = &gQcomI2CProtocolGuid;
  PublishedGuid = &gEfiI2cCrProtocolGuid;
  StandardBackend = &Master;
  PrivateBackend = &Mu;
  InitEntryServices();
  StandardLocateStatus = EFI_OUT_OF_RESOURCES;
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_OUT_OF_RESOURCES);
  assert(PrivateLocateCalls == 0 && Allocations == 0);
  StandardLocateStatus = EFI_SUCCESS;
  InstallStatus = EFI_DEVICE_ERROR;
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_DEVICE_ERROR);
  assert(PrivateLocateCalls == 0 && Allocations == 0 && mInstances == NULL);
  InstallStatus = EFI_SUCCESS;
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  Protocol = &mInstances[0].Protocol;
  assert(Protocol->MasterHandle == (EFI_HANDLE)0x4444);
  assert(Protocol->Master == &Master && Protocol->I2cControllerCapabilities == &Caps);
  assert(Protocol->Qcom == NULL && PrivateLocateCalls == 0);
  assert(Protocol->SetBusFrequency(Protocol, &Hz) == EFI_SUCCESS && Hz == 300000);
  assert(Protocol->Reset(Protocol) == EFI_DEVICE_ERROR);
  assert(Protocol->StartRequest(Protocol, 0x456, &Packet, (EFI_EVENT)0x9999, NULL) == EFI_TIMEOUT);
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_ALREADY_STARTED);
  FreePool(mInstances); mInstances = NULL;
  StandardLocateStatus = EFI_NOT_FOUND;
  InstallStatus = EFI_OUT_OF_RESOURCES;
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_OUT_OF_RESOURCES);
  assert(Opens == 0 && Closes == 0 && mInstances == NULL && Allocations == 0);
  InstallStatus = EFI_SUCCESS;
  assert(I2CCrEntryPoint((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  Protocol = &mInstances[0].Protocol;
  assert(Protocol->Instance == 1 && Protocol->Qcom == &Mu);
  assert(Protocol->I2cControllerCapabilities->MaximumTotalBytes == 0x00ffffff);
  assert(Opens == 0 && Closes == 0 && Allocations == 1);
  {
    MU_I2C_SLAVE_CONFIG Config = {
      .BusFrequencyKHz = 400,
      .SlaveAddress = 0x36,
      .Mode = MU_I2C_MODE_I2C,
      .SlaveMaxClockStretchUs = 500
    };
    UINT8 First[2] = {0x12, 0};
    UINT8 Second[3] = {0x34, 0, 0};
    MU_I2C_DESCRIPTOR Descriptors[2] = {
      { First, sizeof (First), MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE },
      { Second, sizeof (Second), MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE |
                                 MU_I2C_FLAG_STOP }
    };
    Result = MU_I2C_SUCCESS;
    assert (Protocol->MuTransfer (Protocol, &Config, Descriptors, 2,
                                  NULL, NULL, 0, NULL) == EFI_SUCCESS);
    assert (Opens == 1 && Protocol->QcomHandle == (VOID *)0x3333);
    assert (Protocol->Qcom->Close (Protocol->QcomHandle) == MU_I2C_SUCCESS);
  }
  FreePool(mInstances); mInstances = NULL;
}

static I2C_CR_INSTANCE *PendingInstance;
static EFI_STATUS *PendingStatus;
static void CheckCompletion(void) {
  assert(PendingInstance->Busy == 0 && *PendingStatus == EFI_SUCCESS);
}
int main(void) {
  MU_I2C_PROTOCOL Mu = { .Transfer = Transfer };
  I2C_CR_INSTANCE Instance = { .Protocol = {
    .Qcom = &Mu, .QcomHandle = (VOID *)0x3333, .BusFrequencyKHz = 400
  }};
  struct { UINTN OperationCount; EFI_I2C_OPERATION Operation[2]; } Packet;
  UINT8 First[2] = {0x12, 0}, Second[3] = {0x34, 0, 0};
  EFI_STATUS Status = EFI_ABORTED;
  UINTN Hz;
  InitBootServices();
  Packet.OperationCount = 2;
  Packet.Operation[0] = (EFI_I2C_OPERATION){0, 2, First};
  Packet.Operation[1] = (EFI_I2C_OPERATION){0, 3, Second};
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, &Status) == EFI_SUCCESS);
  assert(Status == EFI_SUCCESS && Calls == 1 && Allocations == 0);
  assert(SavedFlags[0] == (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE));
  assert(SavedFlags[1] == (MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE | MU_I2C_FLAG_STOP));
  PendingInstance = &Instance; PendingStatus = &Status; SignalHook = CheckCompletion;
  Packet.Operation[1].Flags = I2C_FLAG_READ;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, (EFI_EVENT)0x2222, &Status) == EFI_SUCCESS);
  assert(Calls == 1 && Allocations == 1 && Instance.Busy == 1);
  Hz = 100000;
  assert(I2cCrSetBusFrequency(&Instance.Protocol, &Hz) == EFI_ALREADY_STARTED);
  assert(I2cCrReset(&Instance.Protocol) == EFI_ALREADY_STARTED);
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_ALREADY_STARTED);
  Packet.OperationCount = 1; Packet.Operation[0].LengthInBytes = 99;
  Work(PendingEvent, WorkContext);
  assert(Calls == 2 && Signals == 1 && Allocations == 0 && Instance.Busy == 0);
  assert(Status == EFI_SUCCESS);
  assert(SavedFlags[1] == (MU_I2C_FLAG_START | MU_I2C_FLAG_READ | MU_I2C_FLAG_STOP));
  Packet.OperationCount = 2; Packet.Operation[0].LengthInBytes = 2;
  TimerStatus = EFI_OUT_OF_RESOURCES;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, (EFI_EVENT)0x2222, NULL) == EFI_OUT_OF_RESOURCES);
  assert(Allocations == 0 && Instance.Busy == 0 && Calls == 2 && Signals == 1);
  TimerStatus = EFI_SUCCESS;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, (EFI_EVENT)0x2222, NULL) == EFI_SUCCESS);
  Work(PendingEvent, WorkContext);
  assert(Allocations == 0 && Signals == 2);
  CreateStatus = EFI_OUT_OF_RESOURCES;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, (EFI_EVENT)0x2222, NULL) == EFI_OUT_OF_RESOURCES);
  assert(Allocations == 0 && PendingEvent == NULL && Instance.Busy == 0);
  CreateStatus = EFI_SUCCESS;
  FailAllocationAt = AllocationAttempts + 1;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_OUT_OF_RESOURCES);
  FailAllocationAt = AllocationAttempts + 2;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, (EFI_EVENT)0x2222, NULL) == EFI_OUT_OF_RESOURCES);
  assert(Allocations == 0 && Instance.Busy == 0);
  FailAllocationAt = 0;
  Packet.Operation[0].LengthInBytes = 0x01000000;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_BAD_BUFFER_SIZE);
  Packet.Operation[0].LengthInBytes = 0x00fffffe;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_BAD_BUFFER_SIZE);
  Packet.Operation[0].LengthInBytes = 2;
  {
    MU_I2C_SLAVE_CONFIG Config = {0};
    MU_I2C_DESCRIPTOR Descriptor = {First, 0x01000000, MU_I2C_FLAG_WRITE};
    assert(I2cCrMuTransfer(&Instance.Protocol, &Config, &Descriptor, 1, NULL, NULL, 0, NULL) == EFI_BAD_BUFFER_SIZE);
    Descriptor.Length = 1; Descriptor.Buffer = NULL;
    assert(I2cCrMuTransfer(&Instance.Protocol, &Config, &Descriptor, 1, NULL, NULL, 0, NULL) == EFI_INVALID_PARAMETER);
  }
  Short = TRUE;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_DEVICE_ERROR);
  Short = FALSE;
  Result = 10;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_TIMEOUT);
  Result = 42;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_DEVICE_ERROR);
  Packet.Operation[0].LengthInBytes = 0;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_UNSUPPORTED);
  Packet.Operation[0].LengthInBytes = 2;
  Packet.Operation[0].Flags = I2C_FLAG_SMBUS_OPERATION;
  assert(I2cCrStartRequest(&Instance.Protocol, 0x36, (VOID *)&Packet, NULL, NULL) == EFI_UNSUPPORTED);
  assert(I2cCrStartRequest(&Instance.Protocol, 0x80000036, (VOID *)&Packet, NULL, NULL) == EFI_UNSUPPORTED);
  assert(I2cCrStartRequest(&Instance.Protocol, 0x100, (VOID *)&Packet, NULL, NULL) == EFI_NOT_FOUND);
  Hz = 99999; assert(I2cCrSetBusFrequency(&Instance.Protocol, &Hz) == EFI_UNSUPPORTED);
  Hz = 399999; assert(I2cCrSetBusFrequency(&Instance.Protocol, &Hz) == EFI_SUCCESS && Hz == 100000);
  Hz = 999999; assert(I2cCrSetBusFrequency(&Instance.Protocol, &Hz) == EFI_SUCCESS && Hz == 400000);
  Hz = 1000001; assert(I2cCrSetBusFrequency(&Instance.Protocol, &Hz) == EFI_SUCCESS && Hz == 1000000);
  assert(I2cCrReset(&Instance.Protocol) == EFI_UNSUPPORTED);
  TestEntry();
  assert(Allocations == 0);
  puts("I2C: framing, async lifetime, exclusion, limits, errors, entry and PI forwarding passed");
  return 0;
}
