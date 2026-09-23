/** @file
 *  Bridge MU I2C masters to Crane consumers.
 *
 *  Prefer an existing PI I2C master.  Waipio's private Qualcomm backend is
 *  adapted without taking ownership of QUP/GENI registers or DMA.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFII2cCrProtocol.h>

#define I2C_CR_DEFAULT_FREQUENCY_KHZ  400U
#define I2C_CR_DEFAULT_STRETCH_US     500U
/* GENI's TX/RX TRANS_LEN fields are 24 bits.  Cap the complete adapter
   request too, so all three PI capability limits describe the same policy. */
#define I2C_CR_MAX_TRANSFER_BYTES     0x00FFFFFFU

typedef struct {
  EFI_HANDLE             AliasHandle;
  EFI_I2C_CR_PROTOCOL     Protocol;
  volatile UINT32        Busy;
} I2C_CR_INSTANCE;

typedef struct {
  I2C_CR_INSTANCE     *Instance;
  EFI_EVENT           WorkEvent;
  EFI_EVENT           CompletionEvent;
  EFI_STATUS         *I2cStatus;
  MU_I2C_CALLBACK     Callback;
  VOID               *CallbackContext;
  MU_I2C_SLAVE_CONFIG Config;
  UINT16              DescriptorCount;
  UINT32              Delay;
  UINT32              ExpectedBytes;
  MU_I2C_DESCRIPTOR   Descriptors[];
} I2C_ASYNC_CONTEXT;

STATIC I2C_CR_INSTANCE *mInstances;

STATIC EFI_I2C_CONTROLLER_CAPABILITIES mI2cCapabilities = {
  sizeof (EFI_I2C_CONTROLLER_CAPABILITIES),
  I2C_CR_MAX_TRANSFER_BYTES,
  I2C_CR_MAX_TRANSFER_BYTES,
  I2C_CR_MAX_TRANSFER_BYTES
};

STATIC EFI_STATUS
MuI2cStatusToEfi (
  IN MU_I2C_STATUS Status
  )
{
  /* The initial status values are stable across the Qualcomm revisions.
     Later DMA/queue values changed without a protocol revision change; do not
     give those errors the meaning of an older enum value. */
  switch (Status) {
    case MU_I2C_SUCCESS:
      return EFI_SUCCESS;
    case MU_I2C_ERROR_INVALID_PARAMETER:
      return EFI_INVALID_PARAMETER;
    case MU_I2C_ERROR_UNSUPPORTED_INSTANCE:
    case MU_I2C_ERROR_API_INVALID_EXECUTION_LEVEL:
    case MU_I2C_ERROR_API_NOT_SUPPORTED:
    case MU_I2C_ERROR_API_ASYNC_MODE_NOT_SUPPORTED:
    case MU_I2C_ERROR_API_PROTOCOL_MODE_NOT_SUPPORTED:
      return EFI_UNSUPPORTED;
    case MU_I2C_ERROR_HANDLE_ALLOCATION:
    case MU_I2C_ERROR_HW_INFO_ALLOCATION:
    case MU_I2C_ERROR_MEM_ALLOC_FAIL:
      return EFI_OUT_OF_RESOURCES;
    case MU_I2C_ERROR_BUS_NOT_IDLE:
      return EFI_NOT_READY;
    case MU_I2C_ERROR_TRANSFER_TIMEOUT:
      return EFI_TIMEOUT;
    default:
      return EFI_DEVICE_ERROR;
  }
}

STATIC I2C_CR_INSTANCE *
I2cCrInstanceFromProtocol (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  return (This == NULL) ? NULL : BASE_CR (This, I2C_CR_INSTANCE, Protocol);
}

STATIC VOID
EFIAPI
I2cCrAsyncWorker (
  IN EFI_EVENT Event,
  IN VOID     *Context
  )
{
  I2C_ASYNC_CONTEXT *Async;
  MU_I2C_STATUS     MuStatus;
  EFI_STATUS        Status;
  UINT32            Transferred;

  Async = Context;
  gBS->CloseEvent (Event);
  Transferred = 0;
  MuStatus = Async->Instance->Protocol.Qcom->Transfer (
      Async->Instance->Protocol.QcomHandle, &Async->Config,
      Async->Descriptors, Async->DescriptorCount, NULL, NULL,
      Async->Delay, &Transferred);
  Status = MuI2cStatusToEfi (MuStatus);
  if (!EFI_ERROR (Status) && (Transferred != Async->ExpectedBytes)) {
    Status = EFI_DEVICE_ERROR;
  }
  if (Async->I2cStatus != NULL) {
    *Async->I2cStatus = Status;
  }

  InterlockedCompareExchange32 (&Async->Instance->Busy, 1, 0);
  if (Async->Callback != NULL) {
    Async->Callback (MuStatus, Transferred, Async->CallbackContext);
  }
  if (Async->CompletionEvent != NULL) {
    gBS->SignalEvent (Async->CompletionEvent);
  }
  FreePool (Async);
}

STATIC EFI_STATUS
I2cCrSubmit (
  IN I2C_CR_INSTANCE      *Instance,
  IN MU_I2C_SLAVE_CONFIG *Config,
  IN MU_I2C_DESCRIPTOR   *Descriptors,
  IN UINT16               DescriptorCount,
  IN MU_I2C_CALLBACK      Callback OPTIONAL,
  IN VOID                *CallbackContext OPTIONAL,
  IN EFI_EVENT            Event OPTIONAL,
  OUT EFI_STATUS         *I2cStatus OPTIONAL,
  IN UINT32               Delay,
  OUT UINT32             *Transferred OPTIONAL
  )
{
  EFI_STATUS         Status;
  MU_I2C_STATUS      MuStatus;
  I2C_ASYNC_CONTEXT *Async;
  UINT64             ExpectedBytes;
  UINT32             CompletedBytes;
  UINTN              Index;
  UINTN              DescriptorBytes;

  if ((Instance == NULL) || (Config == NULL) || (Descriptors == NULL) ||
      (DescriptorCount == 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Instance->Protocol.Qcom == NULL) ||
      (Instance->Protocol.QcomHandle == NULL) ||
      (Instance->Protocol.Qcom->Transfer == NULL)) {
    return EFI_UNSUPPORTED;
  }

  ExpectedBytes = 0;
  for (Index = 0; Index < DescriptorCount; ++Index) {
    if ((Descriptors[Index].Buffer == NULL) ||
        (Descriptors[Index].Length == 0)) {
      return EFI_INVALID_PARAMETER;
    }
    ExpectedBytes += Descriptors[Index].Length;
  }
  if (ExpectedBytes > I2C_CR_MAX_TRANSFER_BYTES) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }

  if ((Event != NULL) || (Callback != NULL)) {
    DescriptorBytes = DescriptorCount * sizeof (*Descriptors);
    Async = AllocateZeroPool (sizeof (*Async) + DescriptorBytes);
    if (Async == NULL) {
      InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
      return EFI_OUT_OF_RESOURCES;
    }
    Async->Instance         = Instance;
    Async->CompletionEvent = Event;
    Async->I2cStatus       = I2cStatus;
    Async->Callback        = Callback;
    Async->CallbackContext = CallbackContext;
    Async->Config          = *Config;
    Async->DescriptorCount = DescriptorCount;
    Async->Delay           = Delay;
    Async->ExpectedBytes   = (UINT32)ExpectedBytes;
    CopyMem (Async->Descriptors, Descriptors, DescriptorBytes);

    /* The private UEFI backend may poll even with a callback and some
       revisions never invoke it.  Execute the synchronous API at TPL_CALLBACK
       after returning to the caller, retaining every backend input until it
       completes.  The caller retains the data buffers until its event fires. */
    Status = gBS->CreateEvent (
        EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, I2cCrAsyncWorker,
        Async, &Async->WorkEvent);
    if (!EFI_ERROR (Status)) {
      Status = gBS->SetTimer (Async->WorkEvent, TimerRelative, 1);
      if (EFI_ERROR (Status)) {
        gBS->CloseEvent (Async->WorkEvent);
      }
    }
    if (EFI_ERROR (Status)) {
      FreePool (Async);
      InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
    }
    return Status;
  }

  CompletedBytes = 0;
  MuStatus = Instance->Protocol.Qcom->Transfer (
      Instance->Protocol.QcomHandle, Config, Descriptors, DescriptorCount,
      NULL, NULL, Delay, &CompletedBytes);
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  Status = MuI2cStatusToEfi (MuStatus);
  if (!EFI_ERROR (Status) && (CompletedBytes != ExpectedBytes)) {
    Status = EFI_DEVICE_ERROR;
  }
  if (Transferred != NULL) {
    *Transferred = CompletedBytes;
  }
  if (I2cStatus != NULL) {
    *I2cStatus = Status;
  }
  return Status;
}

STATIC EFI_STATUS
EFIAPI
I2cCrMuTransfer (
  IN EFI_I2C_CR_PROTOCOL *This,
  IN MU_I2C_SLAVE_CONFIG *Config,
  IN MU_I2C_DESCRIPTOR   *Descriptors,
  IN UINT16               DescriptorCount,
  IN MU_I2C_CALLBACK      Callback OPTIONAL,
  IN VOID                *Context OPTIONAL,
  IN UINT32               Delay,
  OUT UINT32             *Transferred OPTIONAL
  )
{
  return I2cCrSubmit (
      I2cCrInstanceFromProtocol (This), Config, Descriptors, DescriptorCount,
      Callback, Context, NULL, NULL, Delay, Transferred);
}

STATIC EFI_STATUS
EFIAPI
I2cCrSetBusFrequency (
  IN EFI_I2C_CR_PROTOCOL *This,
  IN OUT UINTN           *BusClockHertz
  )
{
  I2C_CR_INSTANCE *Instance;
  UINTN            ActualHz;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (BusClockHertz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Master != NULL) {
    return Instance->Protocol.Master->SetBusFrequency (
        Instance->Protocol.Master, BusClockHertz);
  }
  if (*BusClockHertz >= 1000000U) {
    ActualHz = 1000000U;
  } else if (*BusClockHertz >= 400000U) {
    ActualHz = 400000U;
  } else if (*BusClockHertz >= 100000U) {
    ActualHz = 100000U;
  } else {
    return EFI_UNSUPPORTED;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }
  Instance->Protocol.BusFrequencyKHz = (UINT32)(ActualHz / 1000U);
  *BusClockHertz = ActualHz;
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
I2cCrReset (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  if (Instance == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Master != NULL) {
    return Instance->Protocol.Master->Reset (Instance->Protocol.Master);
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  /* Closing a client handle does not reset a controller shared with other MU
     clients.  The private protocol exposes no controller reset operation. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
I2cCrStartRequest (
  IN EFI_I2C_CR_PROTOCOL    *This,
  IN UINTN                   SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET *RequestPacket,
  IN EFI_EVENT               Event OPTIONAL,
  OUT EFI_STATUS            *I2cStatus OPTIONAL
  )
{
  I2C_CR_INSTANCE      *Instance;
  MU_I2C_SLAVE_CONFIG   Config;
  MU_I2C_DESCRIPTOR    *Descriptors;
  EFI_STATUS            Status;
  UINTN                 Index;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (RequestPacket == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Instance->Protocol.Master != NULL) {
    return Instance->Protocol.Master->StartRequest (
        Instance->Protocol.Master, SlaveAddress, RequestPacket, Event,
        I2cStatus);
  }
  if ((SlaveAddress & I2C_ADDRESSING_10_BIT) != 0) {
    return ((SlaveAddress & ~((UINTN)I2C_ADDRESSING_10_BIT | 0x3FFU)) != 0)
             ? EFI_NOT_FOUND : EFI_UNSUPPORTED;
  }
  if ((SlaveAddress & ~(UINTN)0x7FU) != 0) {
    return EFI_NOT_FOUND;
  }
  if (RequestPacket->OperationCount == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (RequestPacket->OperationCount > MAX_UINT16) {
    return EFI_BAD_BUFFER_SIZE;
  }

  ZeroMem (&Config, sizeof (Config));
  Config.BusFrequencyKHz = Instance->Protocol.BusFrequencyKHz;
  Config.SlaveAddress = (UINT32)SlaveAddress;
  Config.Mode = MU_I2C_MODE_I2C;
  Config.SlaveMaxClockStretchUs = I2C_CR_DEFAULT_STRETCH_US;
  Descriptors = AllocateZeroPool (
      RequestPacket->OperationCount * sizeof (*Descriptors));
  if (Descriptors == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EFI_SUCCESS;
  for (Index = 0; Index < RequestPacket->OperationCount; ++Index) {
    EFI_I2C_OPERATION *Operation;

    Operation = &RequestPacket->Operation[Index];
    if ((Operation->Flags & ~I2C_FLAG_READ) != 0) {
      Status = EFI_UNSUPPORTED;
      break;
    }
    if (Operation->LengthInBytes == 0) {
      /* MU cannot reliably execute PI quick-read or quick-write probes. */
      Status = EFI_UNSUPPORTED;
      break;
    }
    if (Operation->Buffer == NULL) {
      Status = EFI_INVALID_PARAMETER;
      break;
    }
    Descriptors[Index].Buffer = Operation->Buffer;
    Descriptors[Index].Length = Operation->LengthInBytes;
    /* PI specifies a repeated START and address before every operation. */
    Descriptors[Index].Flags = MU_I2C_FLAG_START |
        (((Operation->Flags & I2C_FLAG_READ) != 0)
          ? MU_I2C_FLAG_READ : MU_I2C_FLAG_WRITE);
    if (Index + 1 == RequestPacket->OperationCount) {
      Descriptors[Index].Flags |= MU_I2C_FLAG_STOP;
    }
  }
  if (!EFI_ERROR (Status)) {
    Status = I2cCrSubmit (
        Instance, &Config, Descriptors,
        (UINT16)RequestPacket->OperationCount,
        NULL, NULL, Event, I2cStatus, 0, NULL);
  }
  FreePool (Descriptors);
  return Status;
}

STATIC VOID
I2cCrInitializeProtocol (
  OUT EFI_I2C_CR_PROTOCOL *Protocol
  )
{
  Protocol->Revision        = EFI_I2C_CR_PROTOCOL_REVISION;
  Protocol->SetBusFrequency = I2cCrSetBusFrequency;
  Protocol->Reset           = I2cCrReset;
  Protocol->StartRequest    = I2cCrStartRequest;
  Protocol->MuTransfer      = I2cCrMuTransfer;
}

STATIC EFI_STATUS
I2cCrInstallStandardInstances (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS               Status;
  EFI_STATUS               FailureStatus;
  EFI_HANDLE              *Handles;
  UINTN                    HandleCount;
  UINTN                    Index;
  UINTN                    Installed;
  EFI_I2C_MASTER_PROTOCOL *Master;

  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
      ByProtocol, &gEfiI2cMasterProtocolGuid, NULL, &HandleCount, &Handles);
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

    Master = NULL;
    Status = gBS->OpenProtocol (
        Handles[Index], &gEfiI2cMasterProtocolGuid, (VOID **)&Master,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR (Status) || (Master == NULL) ||
        (Master->SetBusFrequency == NULL) || (Master->Reset == NULL) ||
        (Master->StartRequest == NULL)) {
      continue;
    }
    I2cCrInitializeProtocol (&mInstances[Installed].Protocol);
    mInstances[Installed].Protocol.MasterHandle = Handles[Index];
    mInstances[Installed].Protocol.Master = Master;
    mInstances[Installed].Protocol.I2cControllerCapabilities =
        Master->I2cControllerCapabilities;
    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiI2cCrProtocolGuid,
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
I2CCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS       Status;
  EFI_STATUS       FailureStatus;
  MU_I2C_PROTOCOL *MuI2c;
  UINTN            Index;
  UINTN            Installed;

  (VOID)SystemTable;
  if (mInstances != NULL) {
    return EFI_ALREADY_STARTED;
  }
  Status = I2cCrInstallStandardInstances (ImageHandle);
  if (Status != EFI_NOT_FOUND) {
    return Status;
  }
  Status = gBS->LocateProtocol (
      &gQcomI2CProtocolGuid, NULL, (VOID **)&MuI2c);
  if (EFI_ERROR (Status) || (MuI2c == NULL) ||
      (MuI2c->Revision < MU_I2C_PROTOCOL_REVISION) ||
      (MuI2c->Open == NULL) || (MuI2c->Transfer == NULL) ||
      (MuI2c->Close == NULL)) {
    return EFI_NOT_FOUND;
  }

  mInstances = AllocateZeroPool (
      (MU_I2C_INSTANCE_MAX - MU_I2C_INSTANCE_001) * sizeof (*mInstances));
  if (mInstances == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Installed = 0;
  FailureStatus = EFI_NOT_FOUND;
  for (Index = MU_I2C_INSTANCE_001; Index < MU_I2C_INSTANCE_MAX; ++Index) {
    VOID          *Handle;
    MU_I2C_STATUS  MuStatus;
    EFI_HANDLE     AliasHandle;

    Handle = NULL;
    MuStatus = MuI2c->Open ((MU_I2C_INSTANCE)Index, &Handle);
    if ((MuStatus != MU_I2C_SUCCESS) || (Handle == NULL)) {
      continue;
    }
    I2cCrInitializeProtocol (&mInstances[Installed].Protocol);
    mInstances[Installed].Protocol.Qcom = MuI2c;
    mInstances[Installed].Protocol.Instance = (MU_I2C_INSTANCE)Index;
    mInstances[Installed].Protocol.QcomHandle = Handle;
    mInstances[Installed].Protocol.BusFrequencyKHz =
        I2C_CR_DEFAULT_FREQUENCY_KHZ;
    mInstances[Installed].Protocol.I2cControllerCapabilities =
        &mI2cCapabilities;

    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiI2cCrProtocolGuid,
        &mInstances[Installed].Protocol, NULL);
    if (EFI_ERROR (Status)) {
      MuI2c->Close (Handle);
      FailureStatus = Status;
      continue;
    }
    mInstances[Installed++].AliasHandle = AliasHandle;
  }
  if (Installed != 0) {
    return EFI_SUCCESS;
  }
  FreePool (mInstances);
  mInstances = NULL;
  return FailureStatus;
}
