/** Shell USB2 mass-storage device test, automatic UFS LUN0 plus SD BlockIo.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Protocol/UsbFunctionIo.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UsbMscCrLib.h>
#include "UsbMscBlock.h"

#define MSC_CONTROL_BYTES 512U
#define MSC_EVENT_BUDGET 128U
STATIC_ASSERT(MSC_DISK_MAX == CR_USB_MSC_MAX_LUNS, "application/backend LUN capacity");

typedef struct {
  EFI_USB_CONFIG_DESCRIPTOR Config;
  EFI_USB_INTERFACE_DESCRIPTOR Interface;
  EFI_USB_ENDPOINT_DESCRIPTOR Out, In;
} MSC_CONFIGURATION;
STATIC_ASSERT(sizeof(MSC_CONFIGURATION) == 32, "packed USB configuration");

/* pid.codes 1209:0001 is explicitly reserved for private tests. It is not a
   unique product identity and must not be shipped as a production device. */
STATIC EFI_USB_DEVICE_DESCRIPTOR mMscDevice = {
  18, 1, 0x0200, 0, 0, 0, 64, 0x1209, 0x0001, 0x0100, 1, 2, 3, 1
};
STATIC MSC_CONFIGURATION mMscConfiguration = {
  {9, 2, 32, 1, 1, 0, 0xC0, 1},
  {9, 4, 0, 0, 2, 8, 6, 0x50, 0},
  {7, 5, 0x01, 2, 512, 0}, {7, 5, 0x81, 2, 512, 0}
};
STATIC EFI_USB_ENDPOINT_DESCRIPTOR *mMscEndpoints[] = {
  &mMscConfiguration.Out, &mMscConfiguration.In
};
STATIC EFI_USB_INTERFACE_INFO mMscInterface = {
  &mMscConfiguration.Interface, mMscEndpoints
};
STATIC EFI_USB_INTERFACE_INFO *mMscInterfaces[] = {&mMscInterface};
STATIC EFI_USB_CONFIG_INFO mMscConfigInfo = {&mMscConfiguration.Config, mMscInterfaces};
STATIC EFI_USB_CONFIG_INFO *mMscConfigs[] = {&mMscConfigInfo};
STATIC EFI_USB_DEVICE_INFO mMscInfo = {&mMscDevice, mMscConfigs};

typedef struct {
  EFI_USBFN_IO_PROTOCOL *Io;
  CR_USB_MSC Core;
  CR_USB_MSC_ACTION Action;
  VOID *Control, *Bulk;
  MSC_BLOCK Blocks[MSC_DISK_MAX];
  UINTN LunCount;
  EFI_USB_BUS_SPEED Speed;
  UINT8 Configuration;
  UINT8 ControlStage; /* 0 idle, 1 IN data, 2 status */
  EFI_USBFN_ENDPOINT_DIRECTION ControlDirection;
  UINTN ControlLength;
  BOOLEAN Started, Touched, HaveAction, BulkPending;
  UINT64 InBytes, OutBytes;
  UINT32 ScsiErrors;
} MSC_APP;

STATIC EFI_STATUS MscControlTransfer(MSC_APP *A, BOOLEAN In, UINTN Length, UINT8 Stage)
{
  EFI_STATUS S;
  UINTN Size = Length;
  S = A->Io->Transfer(A->Io, 0, In ? EfiUsbEndpointDirectionDeviceTx :
                      EfiUsbEndpointDirectionDeviceRx, &Size, A->Control);
  if (S == EFI_SUCCESS) {
    if (Size != Length) return EFI_PROTOCOL_ERROR;
    A->ControlStage = Stage;
    A->ControlDirection = In ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx;
    A->ControlLength = Length;
  }
  return S;
}

STATIC EFI_STATUS MscReply(MSC_APP *A, CONST EFI_USB_DEVICE_REQUEST *R,
                           CONST VOID *Data, UINTN Length)
{
  Length = MIN(Length, R->Length);
  if (Length > MSC_CONTROL_BYTES) return EFI_BAD_BUFFER_SIZE;
  if (Length != 0) CopyMem(A->Control, Data, Length);
  /* An IN request with no data has an IN status transaction. */
  return MscControlTransfer(A, TRUE, Length, R->Length == 0 ? 2 : 1);
}

STATIC EFI_STATUS MscControlStall(MSC_APP *A)
{
  BOOLEAN Halt = TRUE;
  A->ControlStage = 0;
  return A->Io->SetEndpointStallState(A->Io, 0, EfiUsbEndpointDirectionDeviceTx, &Halt);
}

STATIC EFI_STATUS MscCancelBulk(MSC_APP *A)
{
  EFI_STATUS S;
  if (A->Started) {
    S = A->Io->AbortTransfer(A->Io, 1, EfiUsbEndpointDirectionDeviceRx);
    if (S != EFI_SUCCESS) return S;
    S = A->Io->AbortTransfer(A->Io, 1, EfiUsbEndpointDirectionDeviceTx);
    if (S != EFI_SUCCESS) return S;
  }
  if (A->HaveAction) {
    S = CrUsbMscComplete(&A->Core, FALSE, 0);
    // The core reports the cancelled command, after entering reset recovery.
    if (S != EFI_DEVICE_ERROR) return S == EFI_SUCCESS ? EFI_PROTOCOL_ERROR : S;
  }
  A->HaveAction = A->BulkPending = FALSE;
  /* Abort is synchronous, but its completion messages remain queued. Do not
     issue another bulk transfer until EventHandler has returned MsgNone. */
  return EFI_SUCCESS;
}

STATIC BOOLEAN MscValidEndpoint(UINT16 Index)
{ return Index == 0 || Index == 0x80 || Index == 1 || Index == 0x81; }

STATIC EFI_STATUS MscDescriptor(MSC_APP *A, CONST EFI_USB_DEVICE_REQUEST *R)
{
  UINT8 Type = (UINT8)(R->Value >> 8), Index = (UINT8)R->Value;
  UINT8 String[128];
  CONST CHAR16 *Text;
  UINTN I, Length;
  MSC_CONFIGURATION Config = mMscConfiguration;
  STATIC CONST UINT8 Qualifier[] = {10, 6, 0, 2, 0, 0, 0, 64, 1, 0};
  if (Type != 3 && R->Index != 0) return MscControlStall(A);
  if (Type == 1 && Index == 0) return MscReply(A, R, &mMscDevice, sizeof(mMscDevice));
  if ((Type == 2 || Type == 7) && Index == 0) {
    UINT16 Packet = A->Speed == UsbBusSpeedFull ? 64 : 512;
    if (Type == 7) Packet = Packet == 64 ? 512 : 64;
    Config.Config.DescriptorType = Type;
    Config.Out.MaxPacketSize = Config.In.MaxPacketSize = Packet;
    return MscReply(A, R, &Config, sizeof(Config));
  }
  if (Type == 6 && Index == 0) return MscReply(A, R, Qualifier, sizeof(Qualifier));
  if (Type != 3) return MscControlStall(A);
  if (Index == 0 && R->Index == 0) {
    CONST UINT8 Languages[] = {4, 3, 9, 4};
    return MscReply(A, R, Languages, sizeof(Languages));
  }
  if (R->Index != 0x0409) return MscControlStall(A);
  switch (Index) {
    case 1: Text = L"Project Aloha"; break;
    case 2: Text = L"Crane UFS/SD Test"; break;
    case 3: Text = A->Blocks[0].Serial[0] ? A->Blocks[0].Serial : L"4352414E450001"; break;
    default: return MscControlStall(A);
  }
  for (Length = 0; Text[Length] != 0; ++Length) {}
  if (Length > (sizeof(String) - 2) / 2) return EFI_BAD_BUFFER_SIZE;
  String[0] = (UINT8)(2 + Length * 2); String[1] = 3;
  for (I = 0; I < Length; ++I) {
    String[2 + 2 * I] = (UINT8)Text[I]; String[3 + 2 * I] = (UINT8)(Text[I] >> 8);
  }
  return MscReply(A, R, String, String[0]);
}

STATIC EFI_STATUS MscSetup(MSC_APP *A, CONST EFI_USB_DEVICE_REQUEST *R)
{
  EFI_STATUS S;
  UINT8 Data[2] = {0, 0};
  BOOLEAN Halt, In;
  EFI_USBFN_ENDPOINT_DIRECTION Direction;
  A->ControlStage = 0;
  DEBUG((DEBUG_INIT, "UsbMscTest: setup %02x/%02x value=%04x index=%04x len=%u\n",
         R->RequestType, R->Request, R->Value, R->Index, R->Length));
  if (R->RequestType == 0x80 && R->Request == 6) return MscDescriptor(A, R);
  if (R->RequestType == 0 && R->Request == 5 && R->Value <= 127 &&
      R->Index == 0 && R->Length == 0) {
    // Some controllers notify the client after accepting SET_ADDRESS.
    return MscControlTransfer(A, TRUE, 0, 2);
  }
  if (R->RequestType == 0x80 && R->Request == 8 && R->Value == 0 &&
      R->Index == 0 && R->Length == 1) {
    Data[0] = A->Configuration; return MscReply(A, R, Data, 1);
  }
  if (R->RequestType == 0 && R->Request == 9 && R->Value <= 1 &&
      R->Index == 0 && R->Length == 0) {
    S = MscCancelBulk(A);
    if (S != EFI_SUCCESS) return S;
    S = CrUsbMscBusReset(&A->Core);
    if (S != EFI_SUCCESS) return S;
    A->Configuration = (UINT8)R->Value;
    if (A->Configuration != 0) {
      if (A->Speed != UsbBusSpeedFull && A->Speed != UsbBusSpeedHigh) return EFI_UNSUPPORTED;
      S = CrUsbMscSetMaxPacket(&A->Core, A->Speed == UsbBusSpeedFull ? 64 : 512);
      if (S != EFI_SUCCESS) return S;
    }
    Print(L"UsbMscTest: configuration %u\n", A->Configuration);
    return MscControlTransfer(A, TRUE, 0, 2);
  }
  if (R->Request == 0 && R->Value == 0 && R->Length == 2) {
    if (R->RequestType == 0x80 && R->Index == 0) Data[0] = 1; // self powered
    else if (R->RequestType == 0x81 && R->Index == 0 && A->Configuration != 0) {}
    else if (R->RequestType == 0x82 && MscValidEndpoint(R->Index) &&
             ((R->Index & 15) == 0 || A->Configuration != 0)) {
      Direction = (R->Index & 0x80) ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx;
      S = A->Io->GetEndpointStallState(A->Io, (UINT8)(R->Index & 15), Direction, &Halt);
      if (S != EFI_SUCCESS) return S;
      Data[0] = Halt ? 1 : 0;
    } else return MscControlStall(A);
    return MscReply(A, R, Data, 2);
  }
  if (R->RequestType == 0x81 && R->Request == 10 && R->Value == 0 &&
      R->Index == 0 && R->Length == 1 && A->Configuration != 0)
    return MscReply(A, R, Data, 1);
  if (R->RequestType == 1 && R->Request == 11 && R->Value == 0 &&
      R->Index == 0 && R->Length == 0 && A->Configuration != 0)
    return MscControlTransfer(A, TRUE, 0, 2); // sole alternate setting
  if (R->RequestType == 0xA1 && R->Request == 0xFE && R->Value == 0 &&
      R->Index == 0 && R->Length == 1 && A->Configuration != 0) {
    S = CrUsbMscGetMaxLun(&A->Core, &Data[0]);
    return S == EFI_SUCCESS ? MscReply(A, R, Data, 1) : S;
  }
  if (R->RequestType == 0x21 && R->Request == 0xFF && R->Value == 0 &&
      R->Index == 0 && R->Length == 0 && A->Configuration != 0) {
    S = MscCancelBulk(A);
    if (S != EFI_SUCCESS) return S;
    S = CrUsbMscReset(&A->Core);
    if (S != EFI_SUCCESS) return S;
    Print(L"UsbMscTest: bulk-only reset\n");
    return MscControlTransfer(A, TRUE, 0, 2);
  }
  if (R->RequestType == 2 && (R->Request == 1 || R->Request == 3) &&
      R->Value == 0 && R->Length == 0 && (R->Index == 1 || R->Index == 0x81) &&
      A->Configuration != 0) {
    In = (R->Index & 0x80) != 0;
    Direction = In ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx;
    Halt = R->Request == 3;
    if (Halt) {
      S = MscCancelBulk(A);
      if (S != EFI_SUCCESS) return S;
    } else if (!CrUsbMscClearHalt(&A->Core, In)) {
      // Invalid CBW requires BOT reset before a clear may remove either halt.
      return MscControlTransfer(A, TRUE, 0, 2);
    }
    S = A->Io->SetEndpointStallState(A->Io, 1, Direction, &Halt);
    if (S != EFI_SUCCESS) return S;
    return MscControlTransfer(A, TRUE, 0, 2);
  }
  return MscControlStall(A);
}

STATIC EFI_STATUS MscStop(MSC_APP *A)
{
  EFI_STATUS S;
  if (!A->Touched) return EFI_SUCCESS;
  S = A->Io->StopController(A->Io);
  if (S != EFI_SUCCESS) return S;
  A->Started = A->Touched = A->HaveAction = A->BulkPending = FALSE;
  A->Configuration = A->ControlStage = 0;
  A->Speed = UsbBusSpeedUnknown;
  return CrUsbMscBusReset(&A->Core);
}

STATIC EFI_STATUS MscEvent(MSC_APP *A, EFI_USBFN_MESSAGE Message,
                           UINTN Size, CONST EFI_USBFN_MESSAGE_PAYLOAD *P)
{
  EFI_STATUS S;
  CONST EFI_USBFN_TRANSFER_RESULT *T = &P->utr;
  switch (Message) {
    case EfiUsbMsgSetupPacket:
      if (Size != sizeof(P->udr)) return EFI_PROTOCOL_ERROR;
      return A->Started ? MscSetup(A, &P->udr) : EFI_SUCCESS;
    case EfiUsbMsgEndpointStatusChangedRx:
    case EfiUsbMsgEndpointStatusChangedTx:
      if (Size != sizeof(*T)) return EFI_PROTOCOL_ERROR;
      if (T->EndpointIndex == 0) {
        if (T->Buffer != A->Control || A->ControlStage == 0) return EFI_SUCCESS;
        if (T->TransferStatus == UsbTransferStatusAborted) { A->ControlStage = 0; return EFI_SUCCESS; }
        if (T->TransferStatus != UsbTransferStatusComplete || T->Direction != A->ControlDirection ||
            T->BytesTransferred != A->ControlLength) return EFI_PROTOCOL_ERROR;
        if (A->ControlStage == 1) return MscControlTransfer(A, FALSE, 0, 2);
        A->ControlStage = 0;
      } else if (T->EndpointIndex == 1 && A->BulkPending) {
        BOOLEAN In = A->Action.Type == CrUsbMscBulkIn;
        if (T->Buffer != A->Bulk || T->Direction != (In ? EfiUsbEndpointDirectionDeviceTx :
            EfiUsbEndpointDirectionDeviceRx) || T->BytesTransferred > A->Action.Length)
          return EFI_PROTOCOL_ERROR;
        A->BulkPending = A->HaveAction = FALSE;
        if (T->TransferStatus == UsbTransferStatusAborted) {
          S = CrUsbMscComplete(&A->Core, FALSE, 0);
          return S == EFI_DEVICE_ERROR ? EFI_SUCCESS : S;
        }
        if (T->TransferStatus != UsbTransferStatusComplete) return EFI_PROTOCOL_ERROR;
        if (!In && T->BytesTransferred != 0) CopyMem(A->Action.Buffer, A->Bulk, T->BytesTransferred);
        if (In) A->InBytes += T->BytesTransferred; else A->OutBytes += T->BytesTransferred;
        return CrUsbMscComplete(&A->Core, TRUE, (UINT32)T->BytesTransferred);
      }
      break;
    case EfiUsbMsgBusEventAttach: Print(L"UsbMscTest: device port attached\n"); break;
    case EfiUsbMsgBusEventDetach:
      Print(L"UsbMscTest: detached; waiting for reconnect\n");
      return MscStop(A);
    case EfiUsbMsgBusEventReset:
      Print(L"UsbMscTest: USB bus reset\n");
      S = MscCancelBulk(A);
      if (S != EFI_SUCCESS) return S;
      A->Configuration = A->ControlStage = 0;
      return CrUsbMscBusReset(&A->Core);
    case EfiUsbMsgBusEventSpeed:
      if (Size != sizeof(P->ubs)) return EFI_PROTOCOL_ERROR;
      A->Speed = P->ubs;
      Print(L"UsbMscTest: USB %s speed\n", A->Speed == UsbBusSpeedHigh ? L"high" :
            A->Speed == UsbBusSpeedFull ? L"full" : L"unsupported");
      break;
    default: break;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS MscPump(MSC_APP *A)
{
  EFI_STATUS S;
  BOOLEAN In, Halt = TRUE;
  UINTN Size;
  if (!A->Started || A->Configuration == 0 || A->BulkPending) return EFI_SUCCESS;
  if (!A->HaveAction) {
    S = CrUsbMscNext(&A->Core, &A->Action);
    if (S != EFI_SUCCESS || A->Action.Type == CrUsbMscNone) return S;
    A->HaveAction = TRUE;
    if (A->Action.Type == CrUsbMscBulkIn && A->Action.Buffer == A->Core.Csw &&
        A->Core.Status != 0 && A->ScsiErrors++ < 32) {
      CONST CR_USB_MSC_LUN *Lun = A->Core.ActiveLun < A->Core.LunCount ?
        &A->Core.Luns[A->Core.ActiveLun] : &A->Core.UnsupportedLun;
      Print(L"UsbMscTest: SCSI LUN=%u op=%02x status=%u sense=%02x/%02x/%02x residue=%u\n",
            A->Core.ActiveLun, A->Core.Cbw[15], A->Core.Status, Lun->SenseKey,
            Lun->SenseAsc, Lun->SenseAscq, A->Core.Residue);
    }
  }
  In = A->Action.Type == CrUsbMscBulkIn || A->Action.Type == CrUsbMscStallIn;
  if (A->Action.Type == CrUsbMscStallIn || A->Action.Type == CrUsbMscStallOut) {
    S = A->Io->SetEndpointStallState(A->Io, 1, In ? EfiUsbEndpointDirectionDeviceTx :
                                    EfiUsbEndpointDirectionDeviceRx, &Halt);
    if (S != EFI_SUCCESS) return S;
    A->HaveAction = FALSE;
    return CrUsbMscComplete(&A->Core, TRUE, 0);
  }
  if (A->Action.Length > CR_USB_MSC_TRANSFER_MAX) return EFI_BAD_BUFFER_SIZE;
  if (In && A->Action.Length != 0) CopyMem(A->Bulk, A->Action.Buffer, A->Action.Length);
  Size = A->Action.Length;
  S = A->Io->Transfer(A->Io, 1, In ? EfiUsbEndpointDirectionDeviceTx :
                      EfiUsbEndpointDirectionDeviceRx, &Size, A->Bulk);
  if (S == EFI_SUCCESS) {
    A->BulkPending = TRUE;
    if (Size != A->Action.Length) return EFI_PROTOCOL_ERROR;
  }
  return S;
}

STATIC EFI_STATUS MscDrain(MSC_APP *A, BOOLEAN Dispatch, BOOLEAN *Empty)
{
  EFI_USBFN_MESSAGE Message;
  EFI_USBFN_MESSAGE_PAYLOAD Payload;
  EFI_STATUS S;
  UINTN Size, I;
  *Empty = FALSE;
  for (I = 0; I < MSC_EVENT_BUDGET; ++I) {
    Size = sizeof(Payload);
    S = A->Io->EventHandler(A->Io, &Message, &Size, &Payload);
    if (S != EFI_SUCCESS) return S;
    if (Message == EfiUsbMsgNone) { *Empty = TRUE; return EFI_SUCCESS; }
    if (Dispatch) {
      S = MscEvent(A, Message, Size, &Payload);
      if (S != EFI_SUCCESS) return S;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS MscStart(MSC_APP *A)
{
  EFI_STATUS S;
  BOOLEAN Zlp = FALSE;
  S = A->Io->SetEndpointPolicy(A->Io, 1, EfiUsbEndpointDirectionDeviceTx,
           EfiUsbPolicyZeroLengthTermination, sizeof(Zlp), &Zlp);
  if (S != EFI_SUCCESS) return S;
  // A failed start may retain provider allocations after a failed rollback.
  A->Touched = TRUE;
  S = A->Io->StartController(A->Io);
  if (S != EFI_SUCCESS) return S;
  S = A->Io->ConfigureEnableEndpoints(A->Io, &mMscInfo);
  if (S != EFI_SUCCESS) return S;
  A->Started = TRUE;
  Print(L"UsbMscTest: USB2 mass-storage controller started\n");
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  MSC_APP *A;
  EFI_HANDLE *Handles = NULL, Handle = NULL;
  EFI_STATUS S, Cleanup;
  EFI_INPUT_KEY Key;
  UINTN Count, I, MaxTransfer, Tick = 0;
  BOOLEAN Empty, Quit = FALSE;
  CR_USB_MSC_BLOCK_BACKEND Backends[MSC_DISK_MAX];
  (VOID)SystemTable;
  A = AllocateZeroPool(sizeof(*A));
  if (A == NULL) return EFI_OUT_OF_RESOURCES;
  S = MscAttachBlocks(ImageHandle, A->Blocks, Backends, &A->LunCount);
  if (S != EFI_SUCCESS) goto Done;
  S = CrUsbMscInitBlocks(&A->Core, Backends, (UINT8)A->LunCount);
  if (S != EFI_SUCCESS) goto Done;
  S = gBS->LocateHandleBuffer(ByProtocol, &gEfiUsbFunctionIoProtocolGuid, NULL, &Count, &Handles);
  if (S != EFI_SUCCESS) goto Done;
  for (I = 0; I < Count; ++I) {
    S = gBS->OpenProtocol(Handles[I], &gEfiUsbFunctionIoProtocolGuid, (VOID **)&A->Io,
                          ImageHandle, NULL, EFI_OPEN_PROTOCOL_EXCLUSIVE);
    if (S == EFI_SUCCESS) { Handle = Handles[I]; break; }
  }
  if (Handle == NULL) goto Done;
  if (A->Io->Revision < EFI_USBFN_IO_PROTOCOL_REVISION) { S = EFI_UNSUPPORTED; goto Done; }
  S = A->Io->GetMaxTransferSize(A->Io, &MaxTransfer);
  if (S != EFI_SUCCESS) goto Done;
  if (MaxTransfer < CR_USB_MSC_TRANSFER_MAX) { S = EFI_UNSUPPORTED; goto Done; }
  S = A->Io->AllocateTransferBuffer(A->Io, MSC_CONTROL_BYTES, &A->Control);
  if (S == EFI_SUCCESS) S = A->Io->AllocateTransferBuffer(A->Io, CR_USB_MSC_TRANSFER_MAX, &A->Bulk);
  if (S != EFI_SUCCESS) goto Done;
  // Clear an earlier function session only after obtaining exclusive ownership.
  A->Touched = TRUE;
  S = MscStop(A);
  if (S != EFI_SUCCESS) goto Done;
  S = MscDrain(A, FALSE, &Empty);
  if (S != EFI_SUCCESS || !Empty) { if (S == EFI_SUCCESS) S = EFI_NOT_READY; goto Done; }
  Print(L"UsbMscTest: USB ID 1209:0001.\n"
        L"Connect the HDK Type-C port directly to a computer with a data cable.\n"
        L"Waiting for device role. Eject on PC before Esc or Q to exit.\n");
  while (!Quit) {
    if (gST->ConIn != NULL && gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_SUCCESS)
      Quit = Key.ScanCode == SCAN_ESC || Key.UnicodeChar == L'q' || Key.UnicodeChar == L'Q';
    if (Quit) break;
    S = MscDrain(A, TRUE, &Empty);
    if (S != EFI_SUCCESS) break;
    if (Empty) {
      if (!A->Started && Tick % 200 == 0) {
        S = MscStart(A);
        if (S != EFI_SUCCESS && A->Touched) {
          Cleanup = MscStop(A);
          if (Cleanup != EFI_SUCCESS) { S = Cleanup; break; }
        }
      } else S = MscPump(A);
      if (S != EFI_SUCCESS && S != EFI_NOT_READY) break;
    }
    ++Tick;
    gBS->Stall(1000);
  }
  if (Quit) S = EFI_SUCCESS;
Done:
  if (Handle != NULL) {
    Cleanup = MscStop(A);
    if (Cleanup == EFI_SUCCESS) Cleanup = MscDrain(A, FALSE, &Empty);
    if (Cleanup == EFI_SUCCESS && !Empty) Cleanup = EFI_NOT_READY;
    if (Cleanup == EFI_SUCCESS && A->Control != NULL)
      Cleanup = A->Io->FreeTransferBuffer(A->Io, A->Control);
    if (Cleanup == EFI_SUCCESS && A->Bulk != NULL)
      Cleanup = A->Io->FreeTransferBuffer(A->Io, A->Bulk);
    if (Cleanup != EFI_SUCCESS) {
      // The controller may still reference its client buffers. Retain these
      // allocations; the provider owns no callbacks into this application.
      Print(L"UsbMscTest: cleanup %r; USB buffers retained. Reboot before retry.\n", Cleanup);
      S = Cleanup;
    }
    gBS->CloseProtocol(Handle, &gEfiUsbFunctionIoProtocolGuid, ImageHandle, NULL);
  }
  Print(L"UsbMscTest: %r; USB IN %Lu bytes, OUT %Lu bytes.\n", S, A->InBytes, A->OutBytes);
  if (Handles != NULL) FreePool(Handles);
  for (I = MSC_DISK_MAX; I-- > 0;) {
    Cleanup = MscReleaseBlock(&A->Blocks[I]);
    if (Cleanup != EFI_SUCCESS) {
      Print(L"UsbMscTest: disk LUN%u cleanup: %r\n", (UINT32)I, Cleanup);
      S = Cleanup;
    }
  }
  FreePool(A);
  return S;
}
