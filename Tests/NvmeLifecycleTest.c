/* SPDX-License-Identifier: MIT */
/* Actual MU NVMe lifecycle and callback contracts, DEBUG host. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include <Uefi.h>
#include <Library/PcdLib.h>
#ifndef NVME_LIFECYCLE_OLD_CHILD
#include "NvmExpressHci.c"
#include "NvmExpressPassthru.c"
#include "NvmExpressBlockIo.c"
#include "NvmExpressMediaSanitize.c"
#include "NvmExpressDiskInfo.c"
#endif
#include "NvmExpress.c"

EFI_BOOT_SERVICES *gBS;
static NVME_CONTROLLER_PRIVATE_DATA Owner;
static NVME_CONTROLLER_PRIVATE_DATA *ProtocolOwner = &Owner;
static UINT32 CcRegister, AqaRegister;
static unsigned Submitted, ResetEntries, Timeouts, Events, Mappings;
static BOOLEAN CompleteAll;
static unsigned PollCount;
static BOOLEAN FailUnmap;
static unsigned BufferFrees, DisableWrites;
static BOOLEAN SuppressAssertions;
static BOOLEAN FailAllocation, FailDisable, FailFree, FailAttributes, FailPciOpen, FailCloseEvent;
static BOOLEAN FailDoorbell, BadCompletion, ErrorCompletion;
static BOOLEAN ImmediateCompletion, CompletionPending, HoldReady;
static EFI_GUID *FailCloseGuid;
static BOOLEAN FailUninstall;
static BOOLEAN ModelChild, FailChildOpen;
static EFI_GUID *FailChildInstallGuid;
static unsigned ChildProtocolsMask, LastChildUninstallMask;
static EFI_GUID *FailChildUninstallGuid;
static unsigned ReentryServices, ServiceCallbacks;
static VOID ServiceReentry (unsigned Service);
static unsigned InstallNotifyMask, InstallCallbacks, LifecycleServiceMask, LifecycleCallbacks;
static VOID LifecycleNotify (unsigned Mask, BOOLEAN HasChild);
static BOOLEAN CompleteStart;
static NVME_SQ *AdminSq;
static NVME_CQ *AdminCq;
static unsigned AdminCompletions;
static unsigned ProtocolOpens, PciCloses, PathCloses, MmioCalls, ClientSignals, SubtaskFrees;
static BOOLEAN PathOwned, PciOwned;
static VOID *WatchSubtasks[2];
static EFI_PCI_IO_PROTOCOL PciProvider;
static EFI_DEVICE_PATH_PROTOCOL EndPath = {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {4, 0}};
static NVME_DEVICE_PRIVATE_DATA *ChildInterface;
static BOOLEAN Reenter, ExpectRetainedDma;
static EFI_BLOCK_IO2_TOKEN *ActiveToken;
static UINT8 *ActiveClient;
static EFI_STATUS ExpectedTokenStatus;
static unsigned CompletionCount;
typedef struct { EFI_EVENT_NOTIFY Notify; VOID *Context; } TEST_EVENT;
static NVME_DEVICE_PRIVATE_DATA Child;
static BOOLEAN LiveChild;
static unsigned ProtocolCloses, UninstallCalls;
EFI_GUID gEfiNvmExpressPassThruProtocolGuid = {0};
EFI_GUID gEfiPciIoProtocolGuid = {0};
EFI_GUID gEfiDevicePathProtocolGuid = {0};
EFI_GUID gEfiResetNotificationProtocolGuid = {0};
EFI_GUID gEfiBlockIoProtocolGuid = {0};
EFI_GUID gEfiBlockIo2ProtocolGuid = {0};
EFI_GUID gEfiDiskInfoProtocolGuid = {0};
EFI_GUID gMediaSanitizeProtocolGuid = {0};
EFI_GUID gEfiStorageSecurityCommandProtocolGuid = {0};
EFI_HANDLE gImageHandle;

VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {
  if (strstr (Format, "Begin Controller Reset")) ResetEntries++;
  if (strstr (Format, "Timeout occurs")) Timeouts++;
}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return TRUE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Text) {
  fprintf (stderr, "firmware assertion %s:%zu %s\n", File, (size_t)Line, Text);
  if (!SuppressAssertions) abort ();
}
VOID *EFIAPI ZeroMem (VOID *P, UINTN N) { return memset (P, 0, N); }
VOID *EFIAPI CopyMem (VOID *P, CONST VOID *Q, UINTN N) { return memcpy (P, Q, N); }
VOID *EFIAPI AllocateZeroPool (UINTN N) { return FailAllocation ? NULL : calloc (1, N); }
VOID EFIAPI FreePool (VOID *P) {
  for (unsigned I = 0; I < 2; ++I) if (P != NULL && P == WatchSubtasks[I]) { SubtaskFrees++; WatchSubtasks[I] = NULL; }
  free (P);
}
EFI_STATUS EFIAPI FreeUnicodeStringTable (EFI_UNICODE_STRING_TABLE *P) { return EFI_SUCCESS; }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINT64 EFIAPI DivU64x64Remainder (UINT64 A, UINT64 B, UINT64 *R) {
  if (R) *R = A % B; return A / B;
}
UINT64 EFIAPI RShiftU64 (UINT64 A, UINTN B) { return A >> B; }
UINT64 EFIAPI LShiftU64 (UINT64 A, UINTN B) { return A << B; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *P) { UINT32 V; memcpy (&V, P, 4); return V; }
UINT64 EFIAPI ReadUnaligned64 (CONST UINT64 *P) { UINT64 V; memcpy (&V, P, 8); return V; }
UINT64 EFIAPI WriteUnaligned64 (UINT64 *P, UINT64 V) {
  UINT8 *Bytes = (VOID *)P;
  for (UINTN I = 0; I < sizeof (V); I++) Bytes[I] = (UINT8)(V >> (8 * I));
  return V;
}
UINT32 EFIAPI WriteUnaligned32 (UINT32 *P, UINT32 V) {
  UINT8 *Bytes = (VOID *)P;
  for (UINTN I = 0; I < sizeof (V); I++) Bytes[I] = (UINT8)(V >> (8 * I));
  return V;
}
VOID EFIAPI MemoryFence (VOID) { __sync_synchronize (); }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *L) { L->ForwardLink = L->BackLink = L; return L; }
LIST_ENTRY *EFIAPI GetFirstNode (CONST LIST_ENTRY *L) { return L->ForwardLink; }
LIST_ENTRY *EFIAPI GetNextNode (CONST LIST_ENTRY *L, CONST LIST_ENTRY *N) { return N->ForwardLink; }
BOOLEAN EFIAPI IsNull (CONST LIST_ENTRY *L, CONST LIST_ENTRY *N) { return L == N; }
BOOLEAN EFIAPI IsListEmpty (CONST LIST_ENTRY *L) { return L->ForwardLink == L; }
LIST_ENTRY *EFIAPI InsertTailList (LIST_ENTRY *H, LIST_ENTRY *E) {
  E->BackLink = H->BackLink; E->ForwardLink = H; H->BackLink->ForwardLink = E; H->BackLink = E; return H;
}
LIST_ENTRY *EFIAPI InsertHeadList (LIST_ENTRY *H, LIST_ENTRY *E) {
  E->ForwardLink = H->ForwardLink; E->BackLink = H; H->ForwardLink->BackLink = E; H->ForwardLink = E; return H;
}
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *E) {
  E->BackLink->ForwardLink = E->ForwardLink; E->ForwardLink->BackLink = E->BackLink; return E->ForwardLink;
}
RETURN_STATUS EFIAPI ReportStatusCode (EFI_STATUS_CODE_TYPE T, EFI_STATUS_CODE_VALUE V) { return EFI_SUCCESS; }
BOOLEAN EFIAPI ReportProgressCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI ReportErrorCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI ReportDebugCodeEnabled (VOID) { return FALSE; }
EFI_GUID gNVMeEnableStartEventGroupGuid = {0};
EFI_GUID gNVMeEnableCompleteEventGroupGuid = {0};
EFI_STATUS EFIAPI EfiEventGroupSignal (CONST EFI_GUID *Guid) { return EFI_SUCCESS; }
static EFI_TPL EFIAPI Raise (EFI_TPL T) { return TPL_APPLICATION; }
static VOID EFIAPI Restore (EFI_TPL T) {
#ifndef NVME_LIFECYCLE_OLD_CHILD
  if (ImmediateCompletion && CompletionPending && ProtocolOwner &&
      !ProtocolOwner->CallbackInProgress && !ProtocolOwner->RecoveryInProgress) {
    CompletionPending = FALSE;
    ProcessAsyncTaskList (NULL, ProtocolOwner);
  }
#endif
}
static EFI_STATUS EFIAPI Stall (UINTN Microseconds) {
  if (LiveChild) {
    fputs ("RED: actual child Stop polls a permanently live BlockIo2 queue; contract requires bounded refusal before Stall/Close/free\n", stderr);
    assert (!LiveChild);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventCreate (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *E) {
  TEST_EVENT *Record = calloc (1, sizeof (*Record)); assert (Record);
  Record->Notify = Notify; Record->Context = Context; *E = Record; Events++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventClose (EFI_EVENT E) {
  ServiceReentry (4);
  if (FailCloseEvent) return EFI_DEVICE_ERROR;
  free (E); assert (Events); Events--; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventTimer (EFI_EVENT E, EFI_TIMER_DELAY Type, UINT64 Interval) {
  PollCount = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventCheck (EFI_EVENT E) { return ++PollCount > 2 ? EFI_SUCCESS : EFI_NOT_READY; }
static EFI_STATUS EFIAPI EventSignal (EFI_EVENT E) {
  TEST_EVENT *Record = E;
  EFI_EVENT_NOTIFY Notify = Record->Notify; VOID *Context = Record->Context;
  if (Notify) Notify (E, Context);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LocateProtocol (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  assert (Guid == &gEfiResetNotificationProtocolGuid);
  *Interface = NULL;
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Map (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_OPERATION Op,
  VOID *Host, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Device, VOID **Token) {
  *Device = (UINTN)Host; *Token = malloc (1); assert (*Token); Mappings++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap (EFI_PCI_IO_PROTOCOL *P, VOID *Token) {
  ServiceReentry (1);
  if (FailUnmap) return EFI_DEVICE_ERROR;
  assert (Mappings); Mappings--; free (Token); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate (EFI_PCI_IO_PROTOCOL *P, EFI_ALLOCATE_TYPE Type,
  EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Host, UINT64 Attributes) {
  *Host = aligned_alloc (EFI_PAGE_SIZE, Pages * EFI_PAGE_SIZE); assert (*Host); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free (EFI_PCI_IO_PROTOCOL *P, UINTN Pages, VOID *Host) {
  ServiceReentry (2);
  if (FailFree) return EFI_DEVICE_ERROR;
  BufferFrees++; free (Host); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **Interface,
  EFI_HANDLE A, EFI_HANDLE C, UINT32 Attributes) {
  ProtocolOpens++;
  if (G == &gEfiBlockIoProtocolGuid) {
    if (ModelChild && !(ChildProtocolsMask & 2)) return EFI_UNSUPPORTED;
    *Interface = &ChildInterface->BlockIo; return EFI_SUCCESS;
  }
  if (G == &gEfiBlockIo2ProtocolGuid) {
    if (!ModelChild || !(ChildProtocolsMask & 16)) return EFI_UNSUPPORTED;
    *Interface = &ChildInterface->BlockIo2; return EFI_SUCCESS;
  }
  if (G == &gEfiDevicePathProtocolGuid) {
    *Interface = &EndPath;
    if (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if (PathOwned) return EFI_ALREADY_STARTED;
      PathOwned = TRUE;
    }
    return EFI_SUCCESS;
  }
  if (G == &gEfiPciIoProtocolGuid) {
    if (FailPciOpen) return EFI_DEVICE_ERROR;
    *Interface = &PciProvider;
    if (Attributes == EFI_OPEN_PROTOCOL_BY_DRIVER) {
      if (PciOwned) return EFI_ALREADY_STARTED;
      PciOwned = TRUE;
    }
    return EFI_SUCCESS;
  }
  if (G == &gEfiStorageSecurityCommandProtocolGuid) {
    if (!ModelChild || !(ChildProtocolsMask & 32)) return EFI_NOT_FOUND;
    *Interface = &ChildInterface->StorageSecurity; return EFI_SUCCESS;
  }
  if (G == &gEfiNvmExpressPassThruProtocolGuid && ProtocolOwner != NULL) {
    if (Attributes == EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER && FailChildOpen) return EFI_DEVICE_ERROR;
    *Interface = &ProtocolOwner->Passthru; return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI CloseProtocol (EFI_HANDLE H, EFI_GUID *G, EFI_HANDLE A, EFI_HANDLE C) {
  ProtocolCloses++;
  ServiceReentry (16);
  if (G == FailCloseGuid) return EFI_DEVICE_ERROR;
  if (G == &gEfiPciIoProtocolGuid) { assert (PciOwned); PciOwned = FALSE; PciCloses++; }
  if (G == &gEfiDevicePathProtocolGuid) { assert (PathOwned); PathOwned = FALSE; PathCloses++; }
  return EFI_SUCCESS;
}
static unsigned ChildProtocolBit (EFI_GUID *G) {
  return G == &gEfiDevicePathProtocolGuid ? 1 : G == &gEfiBlockIoProtocolGuid ? 2 :
    G == &gEfiDiskInfoProtocolGuid ? 4 : G == &gMediaSanitizeProtocolGuid ? 8 :
    G == &gEfiBlockIo2ProtocolGuid ? 16 : G == &gEfiStorageSecurityCommandProtocolGuid ? 32 : 0;
}
static EFI_STATUS EFIAPI UninstallProtocols (EFI_HANDLE H, ...) {
  UninstallCalls++;
  ServiceReentry (8);
  if (ModelChild && H == (VOID *)0x200) {
    va_list Args; unsigned Mask = 0; BOOLEAN Fails = FALSE;
    va_start (Args, H);
    for (EFI_GUID *G = va_arg (Args, EFI_GUID *); G != NULL; G = va_arg (Args, EFI_GUID *)) {
      (VOID)va_arg (Args, VOID *); Mask |= ChildProtocolBit (G);
      if (G == FailChildUninstallGuid) Fails = TRUE;
    }
    va_end (Args);
    LastChildUninstallMask = Mask;
    assert ((Mask & ChildProtocolsMask) == Mask);
    if (Fails) return EFI_DEVICE_ERROR;
    ChildProtocolsMask &= ~Mask;
  }
  return FailUninstall ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI UninstallSingle (EFI_HANDLE H, EFI_GUID *G, VOID *Interface) {
  if (G == FailChildUninstallGuid) return EFI_DEVICE_ERROR;
  ChildProtocolsMask &= ~ChildProtocolBit (G); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallProtocols (EFI_HANDLE *H, ...) {
  va_list Args; unsigned Mask = 0; BOOLEAN Fails = FALSE, Parent = FALSE;
  va_start (Args, H);
  for (EFI_GUID *G = va_arg (Args, EFI_GUID *); G; G = va_arg (Args, EFI_GUID *)) {
    VOID *Interface = va_arg (Args, VOID *);
    if (G == &gEfiNvmExpressPassThruProtocolGuid) {
      Parent = TRUE; ProtocolOwner = NVME_CONTROLLER_PRIVATE_DATA_FROM_PASS_THRU (Interface);
    } else Mask |= ChildProtocolBit (G);
    if (G == FailChildInstallGuid) Fails = TRUE;
    if (G == &gEfiBlockIoProtocolGuid) ChildInterface = NVME_DEVICE_PRIVATE_DATA_FROM_BLOCK_IO (Interface);
  }
  va_end (Args);
  if (Fails) return EFI_DEVICE_ERROR;
  if (!Parent && ModelChild) { *H = (VOID *)0x200; ChildProtocolsMask |= Mask; }
  LifecycleNotify (Parent ? 64 : Mask, !Parent);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallSingle (EFI_HANDLE *H, EFI_GUID *G, EFI_INTERFACE_TYPE T, VOID *I) {
  if (G == FailChildInstallGuid) return EFI_DEVICE_ERROR;
  ChildProtocolsMask |= ChildProtocolBit (G); LifecycleNotify (ChildProtocolBit (G), TRUE); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LocatePath (EFI_GUID *G, EFI_DEVICE_PATH_PROTOCOL **P, EFI_HANDLE *H) {
  if (LifecycleServiceMask & 1) { LifecycleServiceMask &= ~1u; LifecycleCallbacks++; LifecycleNotify (128, FALSE); }
  return EFI_NOT_FOUND;
}

static EFI_STATUS EFIAPI Attributes (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION Op,
 UINT64 Attributes, UINT64 *Result) {
  if (Op == EfiPciIoAttributeOperationSupported) *Result = EFI_PCI_DEVICE_ENABLE;
  if (Op == EfiPciIoAttributeOperationGet) {
    if (FailAttributes) return EFI_DEVICE_ERROR;
    *Result = 0;
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ConfigRead (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT32 Offset, UINTN Count, VOID *Data) {
  assert (Offset == PCI_VENDOR_ID_OFFSET && W == EfiPciIoWidthUint16 && Count == 2);
  ((UINT16 *)Data)[0] = 0x1234; ((UINT16 *)Data)[1] = 0x5678; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemRead (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Data) {
  assert (Bar == NVME_BAR); MmioCalls++;
  if (Offset == NVME_CAP_OFFSET) {
    NVME_CAP *Cap = Data; ZeroMem (Data, 8);
    if (!CompleteStart) return EFI_DEVICE_ERROR;
    Cap->Css = 1; Cap->Mqes = 255; Cap->To = 1; return EFI_SUCCESS;
  }
  *(UINT32 *)Data = Offset == NVME_CC_OFFSET ? CcRegister :
    Offset == NVME_CSTS_OFFSET ? (HoldReady ? 1 : (CcRegister & 1)) :
    Offset == NVME_AQA_OFFSET ? AqaRegister : 0;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemWrite (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Data) {
  assert (Bar == NVME_BAR); MmioCalls++;
  if (Offset == NVME_CC_OFFSET) {
    if ((*(UINT32 *)Data & 1) == 0 && FailDisable) return EFI_DEVICE_ERROR;
    CcRegister = *(UINT32 *)Data;
    if ((CcRegister & 1) == 0) DisableWrites++;
  }
  if (Offset == NVME_AQA_OFFSET) AqaRegister = *(UINT32 *)Data;
  if (CompleteStart && Offset == NVME_ASQ_OFFSET) AdminSq = (VOID *)(UINTN)*(UINT64 *)Data;
  if (CompleteStart && Offset == NVME_ACQ_OFFSET) AdminCq = (VOID *)(UINTN)*(UINT64 *)Data;
  if (CompleteStart && Offset == NVME_SQTDBL_OFFSET (0, 0)) {
    unsigned Tail = *(UINT32 *)Data;
    NVME_SQ *Sq = AdminSq + ((Tail + 1) % 2);
    NVME_CQ *Cq = AdminCq + (AdminCompletions % 2);
    ZeroMem (Cq, sizeof (*Cq)); Cq->Cid = Sq->Cid; Cq->Sqid = 0;
    Cq->Sqhd = Tail; Cq->Pt = 1 ^ ((AdminCompletions / 2) & 1); AdminCompletions++;
    if (Sq->Opc == NVME_ADMIN_IDENTIFY_CMD) {
      if ((Sq->Payload.Raw.Cdw10 & 0xFF) == 1) {
        NVME_ADMIN_CONTROLLER_DATA *Identify = (VOID *)(UINTN)Sq->Prp[0];
        ZeroMem (Identify, sizeof (*Identify)); Identify->Nn = 1;
        Identify->Oacs = SECURITY_SEND_RECEIVE_SUPPORTED;
      } else {
        NVME_ADMIN_NAMESPACE_DATA *Identify = (VOID *)(UINTN)Sq->Prp[0];
        ZeroMem (Identify, sizeof (*Identify)); Identify->Ncap = Identify->Nsze = 1024;
        Identify->LbaFormat[0].Lbads = 9;
      }
    } else if (Sq->Opc == NVME_ADMIN_SET_FEATURES_CMD) Cq->Dword0 = 0x10001;
  }
  if (Offset == NVME_SQTDBL_OFFSET (2, 0)) {
#ifndef NVME_LIFECYCLE_OLD_CHILD
    NVME_CONTROLLER_PRIVATE_DATA *Private = ProtocolOwner;
    unsigned Tail = *(UINT32 *)Data;
    NVME_SQ *Sq = Private->SqBuffer[2] + ((Tail + 63) % 64);
    Submitted++;
    if (FailDoorbell) { FailDoorbell = FALSE; return EFI_DEVICE_ERROR; }
    if (CompleteAll) {
      NVME_CQ *Cq = Private->CqBuffer[2] + CompletionCount++;
      ZeroMem (Cq, sizeof (*Cq)); Cq->Cid = Sq->Cid; Cq->Sqid = BadCompletion ? 1 : 2;
      Cq->Sqhd = Tail; Cq->Pt = Private->Pt[2] ^ 1; Cq->Sc = ErrorCompletion ? 1 : 0;
      CompletionPending = TRUE;
      if (Sq->Opc == NVME_IO_READ_OPC) memset ((VOID *)(UINTN)Sq->Prp[0], 0xA5, 512);
    }
#endif
  }
  return EFI_SUCCESS;
}

EFI_GUID gEfiDriverBindingProtocolGuid = {0};
EFI_GUID gEfiDriverSupportedEfiVersionProtocolGuid = {0};
EFI_GUID gEfiComponentNameProtocolGuid = {0};
EFI_GUID gEfiComponentName2ProtocolGuid = {0};
EFI_GUID gEfiDriverDiagnosticsProtocolGuid = {0};
EFI_GUID gEfiDriverDiagnostics2ProtocolGuid = {0};
EFI_GUID gEfiDiskInfoNvmeInterfaceGuid = {0};
BOOLEAN EFIAPI IsDevicePathEnd (CONST VOID *P) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)P)->Type == END_DEVICE_PATH_TYPE; }
UINT8 EFIAPI DevicePathType (CONST VOID *P) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)P)->Type; }
UINT8 EFIAPI DevicePathSubType (CONST VOID *P) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)P)->SubType; }
UINTN EFIAPI DevicePathNodeLength (CONST VOID *P) { return ((CONST EFI_DEVICE_PATH_PROTOCOL *)P)->Length[0]; }
UINT16 EFIAPI SetDevicePathNodeLength (VOID *P, UINTN Length) {
  ((EFI_DEVICE_PATH_PROTOCOL *)P)->Length[0] = Length & 255;
  ((EFI_DEVICE_PATH_PROTOCOL *)P)->Length[1] = Length >> 8;
  return (UINT16)Length;
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode (CONST EFI_DEVICE_PATH_PROTOCOL *P, CONST EFI_DEVICE_PATH_PROTOCOL *N) {
  return AllocateZeroPool (sizeof (NVME_NAMESPACE_DEVICE_PATH) + sizeof (EndPath));
}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI CreateDeviceNode (UINT8 Type, UINT8 SubType, UINT16 Length) {
  return AllocateZeroPool (Length);
}
EFI_STATUS EFIAPI AddUnicodeString2 (CONST CHAR8 *Language, CONST CHAR8 *Supported, EFI_UNICODE_STRING_TABLE **Table,
 CONST CHAR16 *String, BOOLEAN Iso) { return EFI_SUCCESS; }
RETURN_STATUS EFIAPI UnicodeSPrint (CHAR16 *Buffer, UINTN Size, CONST CHAR16 *Format, ...) { return 0; }
UINTN EFIAPI UnicodeSPrintAsciiFormat (CHAR16 *Buffer, UINTN Size, CONST CHAR8 *Format, ...) { return 0; }
EFI_COMPONENT_NAME_PROTOCOL gNvmExpressComponentName = {.SupportedLanguages = "eng"};
EFI_COMPONENT_NAME2_PROTOCOL gNvmExpressComponentName2 = {.SupportedLanguages = "en"};
BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return memcmp (A, B, sizeof (*A)) == 0; }
VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
RETURN_STATUS EFIAPI AsciiStrToUnicodeStrS (CONST CHAR8 *Source, CHAR16 *Destination, UINTN Size) { return RETURN_SUCCESS; }

#ifndef NVME_LIFECYCLE_OLD_CHILD
static EFI_DRIVER_BINDING_PROTOCOL Binding;
static EFI_HANDLE ParentHandle = (VOID *)0x100, ChildHandle = (VOID *)0x200;

static VOID ResetProviders (VOID) {
  assert (!Events && !Mappings && IsListEmpty (&mRetainedControllers));
  FailAllocation = FailDisable = FailFree = FailAttributes = FailPciOpen = FailCloseEvent = FALSE;
  FailDoorbell = BadCompletion = ErrorCompletion = FailUnmap = FailUninstall = FALSE;
  ImmediateCompletion = CompletionPending = HoldReady = FALSE;
  ReentryServices = ServiceCallbacks = 0;
  InstallNotifyMask = InstallCallbacks = LifecycleServiceMask = LifecycleCallbacks = 0;
  CompleteStart = FALSE; AdminSq = NULL; AdminCq = NULL; AdminCompletions = 0;
  ModelChild = FailChildOpen = FALSE; ChildProtocolsMask = LastChildUninstallMask = 0; FailChildUninstallGuid = FailChildInstallGuid = NULL;
  FailCloseGuid = NULL; PathOwned = PciOwned = FALSE;
  ProtocolOpens = ProtocolCloses = PciCloses = PathCloses = MmioCalls = UninstallCalls = 0;
  ClientSignals = SubtaskFrees = Submitted = CompletionCount = BufferFrees = DisableWrites = 0;
  Reenter = ExpectRetainedDma = FALSE; CompleteAll = FALSE; CcRegister = 1;
  ProtocolOwner = NULL; ActiveToken = NULL; ActiveClient = NULL;
  PciProvider = (EFI_PCI_IO_PROTOCOL){.Mem = {.Read = MemRead, .Write = MemWrite},
    .Pci = {.Read = ConfigRead}, .Map = Map, .Unmap = Unmap, .AllocateBuffer = Allocate,
    .FreeBuffer = Free, .Attributes = Attributes};
  ZeroMem (&Child, sizeof (Child)); Child.Signature = NVME_DEVICE_PRIVATE_DATA_SIGNATURE;
  InitializeListHead (&Child.AsyncQueue); ChildInterface = &Child;
}
static NVME_CONTROLLER_PRIVATE_DATA *NewOwner (BOOLEAN Queues) {
  NVME_CONTROLLER_PRIVATE_DATA *P = AllocateZeroPool (sizeof (*P)); assert (P);
  P->Signature = NVME_CONTROLLER_PRIVATE_DATA_SIGNATURE;
  P->PciIo = &PciProvider; P->ControllerHandle = ParentHandle;
  P->DriverBindingHandle = Binding.DriverBindingHandle;
  P->PciIoOpened = P->DevicePathOpened = P->PassThruInstalled = TRUE;
  PathOwned = PciOwned = TRUE;
  P->NumberOfIoQueuePairs = 2; P->Cap.Mqes = 255;
  P->Passthru.Mode = &P->PassThruMode; P->PassThruMode = gEfiNvmExpressPassThruMode;
  P->PassThruMode.IoAlign = 1;
  P->Passthru.PassThru = NvmExpressPassThru;
  P->ControllerData = AllocateZeroPool (4096); P->ControllerData->Nn = 1;
  InitializeListHead (&P->AsyncPassThruQueue); InitializeListHead (&P->UnsubmittedSubtasks);
  InitializeListHead (&P->DmaRequests); InitializeListHead (&P->RetainedControllerLink);
  if (Queues) {
    assert (Allocate (&PciProvider, AllocateAnyPages, EfiBootServicesData, 2, (VOID **)&P->Buffer, 0) == EFI_SUCCESS);
    assert (Allocate (&PciProvider, AllocateAnyPages, EfiBootServicesData, 4, (VOID **)&P->IoQueueBuffer, 0) == EFI_SUCCESS);
    P->AdminQueuePages = 2; P->IoQueuePages = 4;
    UINTN Size = 8192; EFI_PHYSICAL_ADDRESS Address;
    assert (Map (&PciProvider, EfiPciIoOperationBusMasterCommonBuffer, P->Buffer, &Size, &Address, &P->Mapping) == EFI_SUCCESS);
    P->BufferPciAddr = (VOID *)(UINTN)Address;
    Size = 16384;
    assert (Map (&PciProvider, EfiPciIoOperationBusMasterCommonBuffer, P->IoQueueBuffer, &Size, &Address, &P->IoQueueMapping) == EFI_SUCCESS);
    P->IoQueueBufferPciAddr = (VOID *)(UINTN)Address;
    ZeroMem (P->Buffer, 8192); ZeroMem (P->IoQueueBuffer, 16384);
    for (unsigned Q = 0; Q < 3; Q++) {
      UINT8 *Base = Q == 0 ? P->Buffer : P->IoQueueBuffer + (Q - 1) * 8192;
      P->SqBuffer[Q] = (VOID *)Base; P->CqBuffer[Q] = (VOID *)(Base + 4096);
      P->SqBufferPciAddr[Q] = P->SqBuffer[Q]; P->CqBufferPciAddr[Q] = P->CqBuffer[Q];
    }
  }
  ProtocolOwner = P; Child.Controller = P; Child.NamespaceId = 1;
  Child.Media.BlockSize = 512;
  return P;
}
static EFI_STATUS ParentStop (VOID) { return NvmExpressDriverBindingStop (&Binding, ParentHandle, 0, NULL); }
static VOID ServiceReentry (unsigned Service) {
  if (!(ReentryServices & Service) || ProtocolOwner == NULL) return;
  ReentryServices &= ~Service; ServiceCallbacks++;
  EFI_STATUS Status = ParentStop ();
  fprintf (stderr, "cleanup service=%u nested Stop=%llx guarded=%u\n", Service, (unsigned long long)Status, ProtocolOwner->CallbackInProgress);
  assert (Status == EFI_NOT_READY && ProtocolOwner->CallbackInProgress);
}
static VOID TestServiceReentry (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  assert (EventCreate (EVT_TIMER, TPL_NOTIFY, ProcessAsyncTaskList, P, &P->TimerEvent) == EFI_SUCCESS);
  ReentryServices = 31;
  assert (ParentStop () == EFI_SUCCESS && ServiceCallbacks == 5 && !ReentryServices);
  assert (!Mappings && !Events && BufferFrees == 2 && PciCloses == 1 && PathCloses == 1);
  ProtocolOwner = NULL;
  puts ("NVMe actual Dispose: provider unmap/free/event/uninstall/close nested Stop refused PASS");
}
static VOID CheckQueuePointersCleared (NVME_CONTROLLER_PRIVATE_DATA *P) {
  for (unsigned Q = 0; Q < 3; Q++) {
    assert (!P->SqBuffer[Q] && !P->CqBuffer[Q]);
    assert (!P->SqBufferPciAddr[Q] && !P->CqBufferPciAddr[Q]);
  }
}
static VOID CheckDetached (NVME_CONTROLLER_PRIVATE_DATA *P) {
  for (LIST_ENTRY *L = GetFirstNode (&P->DmaRequests); !IsNull (&P->DmaRequests, L); L = GetNextNode (&P->DmaRequests, L)) {
    NVME_DMA_REQUEST *R = BASE_CR (L, NVME_DMA_REQUEST, Link);
    assert (R->Detached && !R->CallerData && !R->CallerMetadata);
  }
}
static VOID EFIAPI ClientNotify (EFI_EVENT Event, VOID *Context) {
  NVME_CONTROLLER_PRIVATE_DATA *P = ProtocolOwner;
  ClientSignals++;
  assert (ActiveToken->TransactionStatus == ExpectedTokenStatus);
  assert (IsListEmpty (&Child.AsyncQueue) && IsListEmpty (&P->AsyncPassThruQueue));
  assert (IsListEmpty (&P->UnsubmittedSubtasks));
  assert (!WatchSubtasks[0] && !WatchSubtasks[1]);
  if (ExpectRetainedDma) { assert (!IsListEmpty (&P->DmaRequests)); CheckDetached (P); }
  if (Reenter) {
    unsigned Before = MmioCalls;
    EFI_NVM_EXPRESS_COMMAND Command = {0}; EFI_NVM_EXPRESS_COMPLETION Completion = {0};
    EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET Packet = {.NvmeCmd = &Command, .NvmeCompletion = &Completion, .QueueType = NVME_IO_QUEUE};
    assert (P->CallbackInProgress);
    assert (EFI_ERROR (NvmExpressPassThru (&P->Passthru, 1, &Packet, NULL)));
    assert (NvmeControllerReset (P) == EFI_NOT_READY);
    assert (ParentStop () == EFI_NOT_READY);
    assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_NOT_READY);
    assert (MmioCalls == Before);
  }
}
static NVME_BLKIO2_REQUEST *QueueReads (EFI_BLOCK_IO2_TOKEN *Token, UINT8 *Client, unsigned Count) {
  NVME_BLKIO2_REQUEST *Request = AllocateZeroPool (sizeof (*Request)); assert (Request);
  Request->Signature = NVME_BLKIO2_REQUEST_SIGNATURE; Request->Token = Token;
  InitializeListHead (&Request->SubtasksQueue); InsertTailList (&Child.AsyncQueue, &Request->Link);
  assert (EventCreate (EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ClientNotify, NULL, &Token->Event) == EFI_SUCCESS);
  Token->TransactionStatus = EFI_SUCCESS;
  ActiveToken = Token; ActiveClient = Client;
  for (unsigned I = 0; I < Count; I++) {
    assert (AsyncReadSectors (&Child, Request, (UINTN)(Client + 512 * I), I, 1, I + 1 == Count) == EFI_SUCCESS);
    LIST_ENTRY *Link = ProtocolOwner->UnsubmittedSubtasks.BackLink;
    WatchSubtasks[I] = NVME_BLKIO2_SUBTASK_FROM_LINK (Link);
  }
  return Request;
}
static VOID FinishToken (EFI_BLOCK_IO2_TOKEN *Token) { assert (EventClose (Token->Event) == EFI_SUCCESS); ActiveToken = NULL; }

static VOID TestParentStopFailures (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  VOID *Admin = P->Buffer, *Io = P->IoQueueBuffer, *MapAdmin = P->Mapping, *MapIo = P->IoQueueMapping;
  memset (Admin, 0x5A, 8192); memset (Io, 0x3C, 16384);
  assert (NvmExpressDriverCleanUpQueues (P) == EFI_NOT_READY && BufferFrees == 0);
  FailDisable = TRUE;
  assert (EFI_ERROR (ParentStop ()));
  assert (NvmeFindRetainedController (ParentHandle) == P && !P->ControllerStopped);
  assert (P->Mapping == MapAdmin && P->IoQueueMapping == MapIo && P->Buffer == Admin && P->IoQueueBuffer == Io);
  assert (((UINT8 *)Admin)[0] == 0x5A && ((UINT8 *)Io)[0] == 0x3C && !BufferFrees && !ProtocolCloses);
  FailDisable = FALSE; FailUnmap = TRUE;
  assert (EFI_ERROR (ParentStop ()) && P->ControllerStopped && P->Mapping == MapAdmin && P->IoQueueMapping == MapIo);
  assert (!BufferFrees && !ProtocolCloses);
  FailUnmap = FALSE; FailFree = TRUE;
  assert (EFI_ERROR (ParentStop ()) && !P->Mapping && !P->IoQueueMapping && P->Buffer == Admin && P->IoQueueBuffer == Io);
  assert (!Mappings && !BufferFrees);
  FailFree = FALSE; FailUninstall = TRUE;
  assert (EFI_ERROR (ParentStop ()) && P->PassThruInstalled && !P->Buffer && !P->IoQueueBuffer);
  CheckQueuePointersCleared (P); assert (NvmExpressUnload (gImageHandle) == EFI_ACCESS_DENIED);
  unsigned Before = MmioCalls;
  UINT32 Namespace = 0; EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  assert (EFI_ERROR (NvmExpressGetNextNamespace (&P->Passthru, &Namespace)));
  assert (EFI_ERROR (NvmExpressBuildDevicePath (&P->Passthru, 1, &Path)));
  assert (MmioCalls == Before && !Path);
  FailUninstall = FALSE; FailCloseGuid = &gEfiPciIoProtocolGuid;
  assert (EFI_ERROR (ParentStop ()) && P->PciIoOpened && P->DevicePathOpened && !P->PassThruInstalled);
  FailCloseGuid = &gEfiDevicePathProtocolGuid;
  assert (EFI_ERROR (ParentStop ()) && !P->PciIoOpened && P->DevicePathOpened);
  Before = MmioCalls; P->PciIo = NULL; // An already-closed PCI relationship is inaccessible on retry.
  FailCloseGuid = NULL;
  assert (ParentStop () == EFI_SUCCESS && MmioCalls == Before);
  assert (!PathOwned && !PciOwned && !Mappings && BufferFrees == 2 && PciCloses == 1 && PathCloses == 1);
  ProtocolOwner = NULL;
  puts ("NVMe parent Stop: fence/map/free/uninstall/close failures retain exact owner, retry PASS");
}
static VOID TestReadyFenceAndChildClose (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  VOID *Admin = P->Buffer; VOID *Mapping = P->Mapping; HoldReady = TRUE;
  assert (EFI_ERROR (ParentStop ()) && !P->ControllerStopped);
  assert (P->Buffer == Admin && P->Mapping == Mapping && !BufferFrees && !ProtocolCloses);
  assert (MmioCalls <= 502 && NvmeFindRetainedController (ParentHandle) == P);
  HoldReady = FALSE;
  assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  ResetProviders (); P = NewOwner (FALSE);
  NVME_DEVICE_PRIVATE_DATA *Heap = AllocateZeroPool (sizeof (*Heap)); assert (Heap);
  Heap->Signature = NVME_DEVICE_PRIVATE_DATA_SIGNATURE; Heap->Controller = P;
  Heap->ControllerRelationshipOpen = Heap->RegisteredChild = TRUE; P->ChildCount++;
  InitializeListHead (&Heap->AsyncQueue); ChildInterface = Heap;
  FailCloseGuid = &gEfiNvmExpressPassThruProtocolGuid;
  assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_DEVICE_ERROR);
  assert (!UninstallCalls && Heap->Controller == P);
  FailCloseGuid = NULL;
  assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_SUCCESS);
  ChildInterface = &Child; assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe actual Stop: bounded RDY0 wait retains queues; child failed CloseProtocol retains child PASS");
}
static VOID TestAtomicChildUninstall (VOID) {
  for (unsigned Optional = 0; Optional < 4; Optional++) {
    ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (FALSE);
    P->NumberOfIoQueuePairs = Optional & 1 ? 2 : 1;
    NVME_DEVICE_PRIVATE_DATA *Heap = AllocateZeroPool (sizeof (*Heap)); assert (Heap);
    Heap->Signature = NVME_DEVICE_PRIVATE_DATA_SIGNATURE; Heap->Controller = P;
    Heap->ControllerRelationshipOpen = Heap->RegisteredChild = TRUE; P->ChildCount++;
    InitializeListHead (&Heap->AsyncQueue); ChildInterface = Heap;
    ModelChild = TRUE;
    unsigned Installed = 15 | (Optional & 1 ? 16 : 0) | (Optional & 2 ? 32 : 0);
    ChildProtocolsMask = Installed;
    if (Optional == 1) P->NumberOfIoQueuePairs = 1; // Installed child identity survives mutable controller count.
    if (Optional == 3) FailChildOpen = TRUE;
    FailChildUninstallGuid = Optional & 2 ? &gEfiStorageSecurityCommandProtocolGuid :
      Optional & 1 ? &gEfiBlockIo2ProtocolGuid : &gEfiBlockIoProtocolGuid;
    assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_DEVICE_ERROR);
    assert (ChildProtocolsMask == Installed && LastChildUninstallMask == Installed && P->ChildCount == 1);
    unsigned CloseCount = ProtocolCloses;
    assert (ParentStop () == EFI_NOT_READY && P->ChildCount == 1 && !BufferFrees);
    if (Optional == 3) {
      assert (!Heap->ControllerRelationshipOpen && NvmeFindRetainedController (ParentHandle) == P);
      assert (NvmExpressUnload (gImageHandle) == EFI_ACCESS_DENIED);
    }
    // Atomic rollback keeps BlockIo available for exact Stop retry.
    VOID *Interface;
    assert (OpenProtocol (ChildHandle, &gEfiBlockIoProtocolGuid, &Interface, NULL, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL) == EFI_SUCCESS);
    assert (Interface == &Heap->BlockIo);
    FailChildUninstallGuid = NULL; FailChildOpen = FALSE;
    assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_SUCCESS && !ChildProtocolsMask);
    if (Optional == 3) assert (ProtocolCloses == CloseCount); // No close of a failed reopen.
    assert (P->ChildCount == 0);
    ModelChild = FALSE; ChildInterface = &Child;
    assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  }
  puts ("NVMe actual child Stop: atomic optional interfaces, failed uninstall and retry in all four combinations PASS");
}
static EFI_STATUS EFIAPI EnumIdentify (EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *P, UINT32 Ns,
 EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *Packet, EFI_EVENT Event) {
  NVME_ADMIN_NAMESPACE_DATA *Data = Packet->TransferBuffer;
  assert (Packet->NvmeCmd->Cdw0.Opcode == NVME_ADMIN_IDENTIFY_CMD && Ns == 1);
  ZeroMem (Data, sizeof (*Data)); Data->Ncap = Data->Nsze = 1024; Data->LbaFormat[0].Lbads = 9;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EnumPath (EFI_NVM_EXPRESS_PASS_THRU_PROTOCOL *P, UINT32 Ns, EFI_DEVICE_PATH_PROTOCOL **Path) {
  *Path = AllocateZeroPool (sizeof (NVME_NAMESPACE_DEVICE_PATH)); return EFI_SUCCESS;
}
// Removing lifecycle ownership must let these provider notifications destroy
// the published parent/base/optional owner before its installer returns.
static VOID LifecycleNotify (unsigned Mask, BOOLEAN HasChild) {
  if (!(InstallNotifyMask & Mask) || ProtocolOwner == NULL) return;
  InstallNotifyMask &= ~Mask;
  if (Mask != 128) InstallCallbacks++;
  unsigned BeforeFrees = BufferFrees, BeforeCloses = ProtocolCloses;
  assert (ParentStop () == EFI_NOT_READY);
  if (HasChild) {
    assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_NOT_READY);
    assert (NvmExpressDriverBindingStop (&Binding, ParentHandle, 1, &ChildHandle) == EFI_DEVICE_ERROR);
  }
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_NOT_READY);
  assert (EnumerateNvmeDevNamespace (ProtocolOwner, 1) == EFI_NOT_READY);
  assert (DiscoverAllNamespaces (ProtocolOwner, 0) == EFI_NOT_READY);
  assert (BufferFrees == BeforeFrees && ProtocolCloses == BeforeCloses);
}
static VOID TestInstallNotifications (unsigned Mask) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (FALSE); P->ControllerStopped = TRUE;
  P->Passthru.PassThru = EnumIdentify; P->Passthru.BuildDevicePath = EnumPath;
  P->ParentDevicePath = &EndPath; P->ControllerData->Oacs = SECURITY_SEND_RECEIVE_SUPPORTED;
  ModelChild = TRUE; InstallNotifyMask = Mask; LifecycleServiceMask = (Mask & 128) ? 1 : 0;
  assert (EnumerateNvmeDevNamespace (P, 1) == EFI_SUCCESS);
  assert (InstallCallbacks == ((Mask & 1) ? 1u : 0u) + ((Mask & 16) ? 1u : 0u) + ((Mask & 32) ? 1u : 0u));
  assert (LifecycleCallbacks == ((Mask & 128) ? 1u : 0u) && !InstallNotifyMask);
  assert (P->ChildCount == 1 && ChildProtocolsMask == 63 && ChildInterface->ControllerRelationshipOpen);
  assert (!P->LifecycleInProgress && !NvmeFindRetainedController (ParentHandle));
  assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_SUCCESS);
  ModelChild = FALSE; ChildInterface = &Child;
  assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe actual Enumerate: base/optional install and ordinary service nested Stop/Start/discovery refused PASS");
}
static VOID TestStartNotifications (VOID) {
  ResetProviders (); CompleteStart = TRUE; ModelChild = TRUE;
  InstallNotifyMask = 64 | 1 | 16 | 32 | 128; LifecycleServiceMask = 1;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, NULL) == EFI_SUCCESS);
  NVME_CONTROLLER_PRIVATE_DATA *P = ProtocolOwner;
  assert (InstallCallbacks == 4 && LifecycleCallbacks == 1 && !InstallNotifyMask);
  assert (P->PassThruInstalled && P->ChildCount == 1 && P->NumberOfIoQueuePairs == 2);
  assert (!P->LifecycleInProgress && !NvmeFindRetainedController (ParentHandle));
  assert (ChildProtocolsMask == 63 && ChildInterface->ControllerRelationshipOpen);
  assert (AdminCompletions >= 9);
  assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_SUCCESS);
  ModelChild = FALSE; ChildInterface = &Child;
  assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe actual Start/Init/Discover/Identify: parent/base/optional install nested destruction refused PASS");
}
static VOID TestEnumerationRollback (VOID) {
  for (unsigned Failure = 0; Failure < 4; Failure++) {
    ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (FALSE); P->ControllerStopped = TRUE;
    P->Passthru.PassThru = EnumIdentify; P->Passthru.BuildDevicePath = EnumPath;
    P->ParentDevicePath = &EndPath; ModelChild = TRUE;
    if (Failure == 2) FailChildInstallGuid = &gEfiBlockIo2ProtocolGuid;
    else if (Failure == 3) {
      FailChildInstallGuid = &gEfiStorageSecurityCommandProtocolGuid;
      P->ControllerData->Oacs |= SECURITY_SEND_RECEIVE_SUPPORTED;
    } else FailChildOpen = TRUE;
    if (Failure != 0) FailChildUninstallGuid = &gEfiBlockIoProtocolGuid;
    assert (EnumerateNvmeDevNamespace (P, 1) == EFI_DEVICE_ERROR);
    if (Failure != 0) {
      assert (P->ChildCount == 1 && ChildInterface->RegisteredChild && !ChildInterface->ControllerRelationshipOpen);
      assert (NvmeFindRetainedController (ParentHandle) == P && P->ControllerFailed);
      assert (ParentStop () == EFI_NOT_READY && NvmExpressUnload (gImageHandle) == EFI_ACCESS_DENIED);
      FailChildOpen = FALSE; FailChildUninstallGuid = FailChildInstallGuid = NULL;
      assert (UnregisterNvmeNamespace (&Binding, ParentHandle, ChildHandle) == EFI_SUCCESS);
    }
    assert (!P->ChildCount && !ChildProtocolsMask);
    ChildInterface = &Child; ModelChild = FALSE;
    assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  }
  puts ("NVMe actual Enumerate: partial optional-install/BY_CHILD failures rollback or retain reachable child/parent PASS");
}
static VOID TestTimerCloseFailure (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  assert (EventCreate (EVT_TIMER, TPL_NOTIFY, ProcessAsyncTaskList, P, &P->TimerEvent) == EFI_SUCCESS);
  EFI_EVENT Original = P->TimerEvent;
  FailCloseEvent = TRUE;
  assert (EFI_ERROR (ParentStop ()) && P->TimerEvent == Original && P->PassThruInstalled);
  FailCloseEvent = FALSE; assert (ParentStop () == EFI_SUCCESS && !Events); ProtocolOwner = NULL;
  puts ("NVMe parent Stop: failed CloseEvent retains timer and owner PASS");
}
static VOID TestEarlyStartClosure (VOID) {
  ResetProviders (); FailAllocation = TRUE;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_OUT_OF_RESOURCES);
  assert (!ProtocolOpens && !ProtocolCloses && !PathOwned && !PciOwned);
  FailAllocation = FALSE; FailPciOpen = TRUE; FailCloseGuid = &gEfiDevicePathProtocolGuid;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_DEVICE_ERROR);
  NVME_CONTROLLER_PRIVATE_DATA *P = NvmeFindRetainedController (ParentHandle); assert (P);
  assert (P->DevicePathOpened && !P->PciIoOpened && !P->PassThruInstalled && PathOwned && !PciOwned);
  unsigned Opens = ProtocolOpens;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_NOT_READY && Opens == ProtocolOpens);
  assert (NvmExpressUnload (gImageHandle) == EFI_ACCESS_DENIED);
  FailCloseGuid = NULL; unsigned Before = MmioCalls;
  assert (ParentStop () == EFI_SUCCESS && !PathOwned && MmioCalls == Before);
  ResetProviders (); FailAttributes = TRUE;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_DEVICE_ERROR);
  assert (!PathOwned && !PciOwned && PathCloses == 1 && PciCloses == 1 && !MmioCalls);
  ResetProviders (); FailDisable = TRUE;
  assert (NvmExpressDriverBindingStart (&Binding, ParentHandle, &EndPath) == EFI_DEVICE_ERROR);
  P = NvmeFindRetainedController (ParentHandle); assert (P && P->PciIoOpened && P->DevicePathOpened && !P->PassThruInstalled);
  FailDisable = FALSE;
  assert (ParentStop () == EFI_SUCCESS && !PathOwned && !PciOwned); ProtocolOwner = NULL;
  puts ("NVMe actual Start: allocation/early-open/attributes/init failures and registry-only Stop PASS");
}
static VOID TestAsyncCompletion (BOOLEAN CleanupFailure, BOOLEAN CqError, BOOLEAN IdentityError) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  UINT8 Client[512]; memset (Client, 0x11, sizeof (Client));
  EFI_BLOCK_IO2_TOKEN Token;
  QueueReads (&Token, Client, 1); CompleteAll = TRUE; ErrorCompletion = CqError; BadCompletion = IdentityError;
  FailUnmap = CleanupFailure; Reenter = TRUE;
  ExpectRetainedDma = CleanupFailure;
  ExpectedTokenStatus = CleanupFailure || CqError || IdentityError ? EFI_DEVICE_ERROR : EFI_SUCCESS;
  ProcessAsyncTaskList (NULL, P);
  assert (ClientSignals == 1 && SubtaskFrees == 1 && Events == 1);
  assert (Token.TransactionStatus == ExpectedTokenStatus);
  assert (Client[0] == (ExpectedTokenStatus == EFI_SUCCESS ? 0xA5 : 0x11));
  if (CleanupFailure) { assert (P->ControllerFailed && P->AdmissionBlocked); CheckDetached (P); }
  FailUnmap = FALSE;
  FinishToken (&Token); assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts (CleanupFailure ? "NVMe async actual BlockIo2: failed cleanup synthesizes error/detaches before callback PASS" :
        IdentityError ? "NVMe async actual BlockIo2: wrong SQID aborts with error/no publish PASS" :
        CqError ? "NVMe async actual BlockIo2: CQ error excludes copyback PASS" :
        "NVMe async actual BlockIo2: valid completion/copyback/callback reentry PASS");
}
static VOID TestImmediateCompletion (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  UINT8 Client[512]; memset (Client, 0x11, sizeof (Client)); EFI_BLOCK_IO2_TOKEN Token;
  QueueReads (&Token, Client, 1);
  CompleteAll = ImmediateCompletion = TRUE; Reenter = TRUE; ExpectedTokenStatus = EFI_SUCCESS;
  ProcessAsyncTaskList (NULL, P);
  assert (ClientSignals == 1 && SubtaskFrees == 1 && Submitted == 1 && Client[0] == 0xA5);
  FinishToken (&Token); assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe actual Process current-last submission: callback completes before PassThru returns PASS");
}
static VOID TestNestedDoorbellAbort (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  UINT8 Client[1024]; memset (Client, 0x11, sizeof (Client)); EFI_BLOCK_IO2_TOKEN Token;
  QueueReads (&Token, Client, 2); FailDoorbell = TRUE; Reenter = TRUE;
  ExpectedTokenStatus = EFI_ABORTED;
  ProcessAsyncTaskList (NULL, P);
  assert (ClientSignals == 1 && SubtaskFrees == 2 && Submitted == 1);
  assert (IsListEmpty (&Child.AsyncQueue) && IsListEmpty (&P->UnsubmittedSubtasks));
  assert (Client[0] == 0x11 && Client[512] == 0x11);
  FinishToken (&Token); assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe actual Process/PassThru/abort/BlockIo2: current first+sibling last doorbell error PASS");
}
static VOID TestFailedTokenDrain (VOID) {
  ResetProviders (); NVME_CONTROLLER_PRIVATE_DATA *P = NewOwner (TRUE);
  UINT8 Client[1024] = {0}; EFI_BLOCK_IO2_TOKEN Token;
  QueueReads (&Token, Client, 2); Token.TransactionStatus = EFI_DEVICE_ERROR;
  ExpectedTokenStatus = EFI_DEVICE_ERROR; Reenter = TRUE;
  ProcessAsyncTaskList (NULL, P);
  assert (ClientSignals == 1 && SubtaskFrees == 2 && Submitted == 0);
  FinishToken (&Token); assert (ParentStop () == EFI_SUCCESS); ProtocolOwner = NULL;
  puts ("NVMe unsubmitted error: each real callback retires once, final token once PASS");
}
#else
static VOID ServiceReentry (unsigned Service) {}
static VOID LifecycleNotify (unsigned Mask, BOOLEAN HasChild) {}
#endif

int main (int Argc, char **Argv) {
  EFI_BOOT_SERVICES Bs = {.RaiseTPL = Raise, .RestoreTPL = Restore, .Stall = Stall,
    .CreateEvent = EventCreate, .CloseEvent = EventClose, .SetTimer = EventTimer,
    .CheckEvent = EventCheck, .SignalEvent = EventSignal, .OpenProtocol = OpenProtocol,
    .CloseProtocol = CloseProtocol, .UninstallMultipleProtocolInterfaces = UninstallProtocols,
    .InstallMultipleProtocolInterfaces = InstallProtocols, .UninstallProtocolInterface = UninstallSingle, .InstallProtocolInterface = InstallSingle, .LocateDevicePath = LocatePath, .LocateProtocol = LocateProtocol};
  EFI_DRIVER_BINDING_PROTOCOL ChildBinding = {0}; LIST_ENTRY Queued;
  gBS = &Bs;
#ifndef NVME_LIFECYCLE_OLD_CHILD
  if (Argc > 1 && strcmp (Argv[1], "child-atomic") == 0) { TestAtomicChildUninstall (); return 0; }
  if (Argc > 1 && strcmp (Argv[1], "service-reentry") == 0) { TestServiceReentry (); return 0; }
  if (Argc > 1 && strcmp (Argv[1], "install-notify") == 0) { TestInstallNotifications (1); TestInstallNotifications (16); TestInstallNotifications (32); TestInstallNotifications (128); TestStartNotifications (); return 0; }
#endif
  Child.Signature = NVME_DEVICE_PRIVATE_DATA_SIGNATURE;
  Child.Controller = &Owner; ChildInterface = &Child;
  InitializeListHead (&Child.AsyncQueue); InsertTailList (&Child.AsyncQueue, &Queued);
  LiveChild = TRUE;
  EFI_STATUS Status = UnregisterNvmeNamespace (&ChildBinding, (VOID *)1, (VOID *)2);
  LiveChild = FALSE;
  assert (Status == EFI_NOT_READY);
  assert (!ProtocolCloses && !UninstallCalls && !IsListEmpty (&Child.AsyncQueue));
  RemoveEntryList (&Queued);
  EFI_HANDLE Handle = (VOID *)2;
  InsertTailList (&Child.AsyncQueue, &Queued);
  assert (NvmExpressDriverBindingStop (&ChildBinding, (VOID *)1, 1, &Handle) == EFI_DEVICE_ERROR);
  assert (!ProtocolCloses && !UninstallCalls && !IsListEmpty (&Child.AsyncQueue));
  RemoveEntryList (&Queued);
  puts ("NVMe actual live-child Stop: bounded refusal PASS");
#ifndef NVME_LIFECYCLE_OLD_CHILD
  TestParentStopFailures ();
  TestReadyFenceAndChildClose ();
  TestAtomicChildUninstall ();
  TestServiceReentry ();
  TestEnumerationRollback ();
  TestInstallNotifications (1);
  TestInstallNotifications (16);
  TestInstallNotifications (32);
  TestInstallNotifications (128);
  TestStartNotifications ();
  TestTimerCloseFailure ();
  TestEarlyStartClosure ();
  TestAsyncCompletion (FALSE, FALSE, FALSE);
  TestAsyncCompletion (FALSE, TRUE, FALSE);
  TestAsyncCompletion (TRUE, FALSE, FALSE);
  TestAsyncCompletion (FALSE, FALSE, TRUE);
  TestImmediateCompletion ();
  TestNestedDoorbellAbort ();
  TestFailedTokenDrain ();
  ResetProviders ();
  puts ("NVMe actual lifecycle/async contracts: DEBUG ASan/UBSan PASS");
#endif
  return 0;
}
