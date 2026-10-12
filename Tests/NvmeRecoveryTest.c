/* SPDX-License-Identifier: MIT */
/* Actual MU PassThru -> Reset -> Identify recursion boundary, DEBUG host. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/PcdLib.h>
#include "NvmExpressHci.c"
#include "NvmExpressPassthru.c"
#include "NvmExpress.c"

EFI_BOOT_SERVICES *gBS;
static NVME_CONTROLLER_PRIVATE_DATA Owner;
static NVME_CONTROLLER_PRIVATE_DATA *ProtocolOwner = &Owner;
static UINT32 CcRegister, AqaRegister, QueueResult = 0x00010001;
static unsigned Submitted, ResetEntries, Timeouts, Events, Mappings, TimeoutSnapshots;
static BOOLEAN CompleteAll;
static unsigned PollCount;
static BOOLEAN FailUnmap;
static unsigned BufferFrees, DisableWrites;
static BOOLEAN SuppressAssertions;
static EFI_TPL CurrentTpl = TPL_APPLICATION;
static BOOLEAN CompleteOnRestore, GetterReentry;
static unsigned CallerSignals, GetterCalls, PoolAllocations;
static BOOLEAN QuiesceOnMap, NestedOnMap;
static unsigned NestedSignals;
static EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *NestedPacket;
#define IOVA_BIAS 0x1000000000ULL
typedef struct {
  VOID *Host;
  UINT8 *Provider;
  UINTN Length;
  EFI_PHYSICAL_ADDRESS Device;
  EFI_PCI_IO_PROTOCOL_OPERATION Operation;
  unsigned Attempts;
} TEST_MAPPING;
static TEST_MAPPING *LiveMaps[32];
static unsigned MapCalls, UnmapCalls, CommonBuffers, CommonAllocations;
static unsigned ShortMapCall, ErrorMapCall, FailedUnmapCall;
static BOOLEAN FailFreeBuffer, FailDoorbell, FailDisableWrite, FailCstsRead, HoldRdy, FailEnableWrite, HoldRdyZero;
static unsigned FailDisableAfter;
static BOOLEAN WrongCid, WrongSqid, CqError, QuiesceOnPoll, NoLateCompletion;
static UINT8 *ObservedData, *ObservedMetadata;
static UINTN ObservedDataLength, ObservedMetadataLength;
static UINT8 OriginalData, OriginalMetadata;
static BOOLEAN CheckPublication, UnmapReentry, TimerCloseReentry;
static TEST_MAPPING *LookupMap (UINT64 Address) {
  for (unsigned I = 0; I < 32; I++)
    if (LiveMaps[I] && Address >= LiveMaps[I]->Device && Address - LiveMaps[I]->Device < LiveMaps[I]->Length)
      return LiveMaps[I];
  return NULL;
}
static VOID *Resolve (UINT64 Address) {
  TEST_MAPPING *M = LookupMap (Address);
  assert (M); return M->Provider + (Address - M->Device);
}
static VOID AssertUnchanged (VOID) {
  if (!CheckPublication) return;
  for (UINTN I = 0; I < ObservedDataLength; I++) assert (ObservedData[I] == OriginalData);
  for (UINTN I = 0; I < ObservedMetadataLength; I++) assert (ObservedMetadata[I] == OriginalMetadata);
}
static EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *ReentryPacket;
static VOID TestGetters (EFI_STATUS Expected);
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
  if (strstr (Format, "NvmExpressDxe: timeout:")) {
    va_list Args; va_start (Args, Format);
    unsigned Internal = va_arg (Args, unsigned);
    (void)va_arg (Args, unsigned); (void)va_arg (Args, UINT64);
    for (unsigned I = 0; I < 4; I++) (void)va_arg (Args, UINT32);
    (void)va_arg (Args, EFI_STATUS);
    unsigned Opcode = va_arg (Args, unsigned), Qid = va_arg (Args, unsigned);
    if (!Internal && ResetEntries == 0) assert ((Opcode == NVME_IO_READ_OPC || Opcode == NVME_IO_WRITE_OPC) && Qid == 1);
    TimeoutSnapshots++; va_end (Args);
  }
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
VOID *EFIAPI AllocateZeroPool (UINTN N) { PoolAllocations++; return calloc (1, N); }
VOID EFIAPI FreePool (VOID *P) { free (P); }
EFI_STATUS EFIAPI FreeUnicodeStringTable (EFI_UNICODE_STRING_TABLE *P) { return EFI_SUCCESS; }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINT64 EFIAPI DivU64x64Remainder (UINT64 A, UINT64 B, UINT64 *R) {
  if (R) *R = A % B; return A / B;
}
UINT64 EFIAPI RShiftU64 (UINT64 A, UINTN B) { return A >> B; }
UINT64 EFIAPI LShiftU64 (UINT64 A, UINTN B) { return A << B; }
UINT32 EFIAPI ReadUnaligned32 (CONST UINT32 *P) { UINT32 V; memcpy (&V, P, 4); return V; }
UINT64 EFIAPI ReadUnaligned64 (CONST UINT64 *P) { UINT64 V; memcpy (&V, P, 8); return V; }
UINT64 EFIAPI WriteUnaligned64 (UINT64 *P, UINT64 V) { memcpy (P, &V, 8); return V; }
UINT32 EFIAPI WriteUnaligned32 (UINT32 *P, UINT32 V) { memcpy (P, &V, 4); return V; }
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
EFI_STATUS EFIAPI EfiEventGroupSignal (CONST EFI_GUID *Guid) {
  if (GetterReentry) { GetterCalls++; TestGetters (EFI_NOT_READY); }
  return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise (EFI_TPL T) {
  EFI_TPL Old = CurrentTpl; assert (T >= Old); CurrentTpl = T; return Old;
}
static VOID EFIAPI Restore (EFI_TPL T) {
  assert (T <= CurrentTpl); CurrentTpl = T;
  if (CompleteOnRestore && T < TPL_NOTIFY && Submitted != 0) {
    CompleteOnRestore = FALSE;
    ProcessAsyncTaskList (NULL, &Owner);
  }
}
static EFI_STATUS EFIAPI Stall (UINTN Microseconds) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI EventCreate (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *E) {
  *E = malloc (1); assert (*E); Events++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventClose (EFI_EVENT E) {
  free (E); assert (Events); Events--;
  if (TimerCloseReentry) {
    TimerCloseReentry = FALSE;
    assert (IsListEmpty (&Owner.DmaRequests));
    assert (NvmExpressPassThru (&Owner.Passthru, 1, ReentryPacket, NULL) == EFI_NOT_READY);
    TestGetters (EFI_NOT_READY);
    assert (NvmeControllerReset (&Owner) == EFI_NOT_READY);
    EFI_DRIVER_BINDING_PROTOCOL Binding = {0};
    assert (NvmExpressDriverBindingStop (&Binding, NULL, 0, NULL) == EFI_NOT_READY);
  }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventTimer (EFI_EVENT E, EFI_TIMER_DELAY Type, UINT64 Interval) {
  PollCount = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventCheck (EFI_EVENT E) {
  if (QuiesceOnPoll) { QuiesceOnPoll = FALSE; assert (NvmeQuiesceController (&Owner) == EFI_SUCCESS); }
  return ++PollCount > 2 ? EFI_SUCCESS : EFI_NOT_READY;
}
static EFI_STATUS EFIAPI EventSignal (EFI_EVENT E) {
  if (E == (EFI_EVENT)(UINTN)3) { NestedSignals++; return EFI_SUCCESS; }
  assert (E == (EFI_EVENT)(UINTN)2);
  CallerSignals++;
  assert (NvmExpressPassThru (&Owner.Passthru, 1, ReentryPacket, NULL) == EFI_NOT_READY);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_OPERATION Op,
  VOID *Host, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Device, VOID **Token) {
  assert (Host != ObservedData && Host != ObservedMetadata);
  TEST_MAPPING *M = calloc (1, sizeof (*M)); assert (M);
  M->Host = Host; M->Length = *Bytes; M->Operation = Op;
  M->Device = (UINTN)Host + IOVA_BIAS;
  M->Provider = Op == EfiPciIoOperationBusMasterCommonBuffer ? Host : malloc (*Bytes);
  assert (M->Provider);
  if (Op != EfiPciIoOperationBusMasterCommonBuffer) memcpy (M->Provider, Host, *Bytes);
  unsigned Slot = 0; while (Slot < 32 && LiveMaps[Slot]) Slot++; assert (Slot < 32);
  LiveMaps[Slot] = M; *Device = M->Device; *Token = M; Mappings++; MapCalls++;
  if (QuiesceOnMap) { QuiesceOnMap = FALSE; assert (NvmeQuiesceController (&Owner) == EFI_SUCCESS); }
  if (NestedOnMap) {
    NestedOnMap = FALSE;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, NestedPacket, (EFI_EVENT)(UINTN)3) == EFI_SUCCESS);
    ProcessAsyncTaskList (NULL, &Owner);
  }
  if (MapCalls == ShortMapCall) *Bytes /= 2;
  return MapCalls == ErrorMapCall ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap (EFI_PCI_IO_PROTOCOL *P, VOID *Token) {
  TEST_MAPPING *M = Token; UnmapCalls++;
  if (UnmapReentry) {
    UnmapReentry = FALSE;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, ReentryPacket, NULL) == EFI_NOT_READY);
    TestGetters (EFI_NOT_READY);
    assert (NvmeControllerReset (&Owner) == EFI_NOT_READY);
    assert (NvmeQuiesceController (&Owner) == EFI_NOT_READY);
    EFI_DRIVER_BINDING_PROTOCOL Binding = {0};
    assert (NvmExpressDriverBindingStop (&Binding, NULL, 0, NULL) == EFI_NOT_READY);
  }
  if (FailUnmap) return EFI_DEVICE_ERROR;
  assert (M && Mappings); M->Attempts++;
  /* The native provider may copy back before returning a failure. Retry is
     allowed to copy back into the same saved host pointer again. */
  AssertUnchanged ();
  if (M->Operation == EfiPciIoOperationBusMasterWrite) memcpy (M->Host, M->Provider, M->Length);
  if (UnmapCalls == FailedUnmapCall) return EFI_DEVICE_ERROR;
  unsigned Slot = 0; while (Slot < 32 && LiveMaps[Slot] != M) Slot++; assert (Slot < 32);
  LiveMaps[Slot] = NULL; Mappings--;
  if (M->Operation != EfiPciIoOperationBusMasterCommonBuffer) free (M->Provider);
  free (M); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Allocate (EFI_PCI_IO_PROTOCOL *P, EFI_ALLOCATE_TYPE Type,
  EFI_MEMORY_TYPE MemoryType, UINTN Pages, VOID **Host, UINT64 Attributes) {
  *Host = aligned_alloc (EFI_PAGE_SIZE, Pages * EFI_PAGE_SIZE); assert (*Host);
  CommonBuffers++; CommonAllocations++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free (EFI_PCI_IO_PROTOCOL *P, UINTN Pages, VOID *Host) {
  if (FailFreeBuffer) return EFI_DEVICE_ERROR;
  for (unsigned I = 0; I < 32; I++) assert (!LiveMaps[I] || LiveMaps[I]->Host != Host);
  BufferFrees++; if (CommonBuffers) CommonBuffers--; free (Host); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenProtocol (EFI_HANDLE H, EFI_GUID *G, VOID **Interface,
  EFI_HANDLE A, EFI_HANDLE C, UINT32 Attributes) {
  *Interface = &ProtocolOwner->Passthru; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseProtocol (EFI_HANDLE H, EFI_GUID *G, EFI_HANDLE A, EFI_HANDLE C) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI UninstallProtocols (EFI_HANDLE H, ...) { return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ConfigRead (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT32 Offset, UINTN Count, VOID *Data) {
  assert ((Offset == PCI_VENDOR_ID_OFFSET || Offset == PCI_COMMAND_OFFSET) && W == EfiPciIoWidthUint16 && Count == 2);
  ((UINT16 *)Data)[0] = 0x1234; ((UINT16 *)Data)[1] = 0x5678; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemRead (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Data) {
  assert (Bar == NVME_BAR);
  if (Offset == NVME_CSTS_OFFSET && FailCstsRead) return EFI_DEVICE_ERROR;
  *(UINT32 *)Data = Offset == NVME_CC_OFFSET ? CcRegister :
    Offset == NVME_CSTS_OFFSET ? (HoldRdy ? 1 : HoldRdyZero ? 0 : (CcRegister & 1)) :
    Offset == NVME_AQA_OFFSET ? AqaRegister : 0;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI MemWrite (EFI_PCI_IO_PROTOCOL *P, EFI_PCI_IO_PROTOCOL_WIDTH W,
  UINT8 Bar, UINT64 Offset, UINTN Count, VOID *Data) {
  assert (Bar == NVME_BAR);
  if (Offset == NVME_CC_OFFSET) {
    if ((*(UINT32 *)Data & 1) && FailEnableWrite) { CcRegister = *(UINT32 *)Data; return EFI_DEVICE_ERROR; }
    if ((*(UINT32 *)Data & 1) == 0) {
      DisableWrites++;
      if (FailDisableWrite || (FailDisableAfter && Submitted >= FailDisableAfter)) return EFI_DEVICE_ERROR;
    }
    CcRegister = *(UINT32 *)Data;
  }
  if (Offset == NVME_AQA_OFFSET) AqaRegister = *(UINT32 *)Data;
  if (Offset >= NVME_SQTDBL_OFFSET (0, 0) && ((Offset - NVME_SQTDBL_OFFSET (0, 0)) % 8 == 0)) {
    unsigned Qid = (unsigned)((Offset - NVME_SQTDBL_OFFSET (0, 0)) / 8);
    assert (Qid < 3);
    unsigned Tail = *(UINT32 *)Data;
    unsigned Entry = Qid == 2 ? (Tail + 64 - 1) % 64 : Tail ^ 1;
    NVME_SQ *Sq = Owner.SqBuffer[Qid] + Entry;
    NVME_CQ *Cq = Owner.CqBuffer[Qid] + Owner.CqHdbl[Qid].Cqh;
    Submitted++;
    fprintf (stderr, "submitted #%u opcode=%x qid=%u cid=%u bytesPRP=%llx resets=%u\n",
      Submitted, Sq->Opc, Qid, Sq->Cid, (unsigned long long)Sq->Prp[0], ResetEntries);
    /* First client read and recovery Identify never complete. To keep the
       original recursive code's RED run bounded, the third submission and
       later recoveries complete. The driver, not the fixture, performs reset
       and all actual Identify/SetFeatures/CreateQueue commands. */
    if (CompleteAll || (!NoLateCompletion && Submitted >= 3)) {
      if (Sq->Opc == NVME_ADMIN_IDENTIFY_CMD && Qid == 0) {
        NVME_ADMIN_CONTROLLER_DATA *Identify = Resolve (Sq->Prp[0]);
        ZeroMem (Identify, 4096); Identify->Nn = 1;
      }
      if (Qid != 0 && Sq->Prp[0]) {
        TEST_MAPPING *M = LookupMap (Sq->Prp[0]); assert (M);
        if (Sq->Opc == NVME_IO_READ_OPC) memset (M->Provider, 0xa5, M->Length);
        else {
          assert (Sq->Opc == NVME_IO_WRITE_OPC);
          if (ObservedData) for (UINTN I = 0; I < M->Length; I++) assert (M->Provider[I] == ObservedData[I]);
        }
        if (Sq->Mptr) {
          TEST_MAPPING *Meta = LookupMap (Sq->Mptr); assert (Meta);
          if (Sq->Opc == NVME_IO_READ_OPC) memset (Meta->Provider, 0x5a, Meta->Length);
          else if (ObservedMetadata) for (UINTN I = 0; I < Meta->Length; I++) assert (Meta->Provider[I] == ObservedMetadata[I]);
        }
        if (M->Length > 8192) {
          UINT64 *List = Resolve (Sq->Prp[1]);
          UINT64 NextPage = (Sq->Prp[0] + 4096) & ~4095ULL;
          unsigned Pages = (unsigned)((((Sq->Prp[0] & 4095) + M->Length + 4095) / 4096) - 1);
          while (Pages) {
            unsigned Entries = Pages > 512 ? 511 : Pages;
            for (unsigned I = 0; I < Entries; I++) {
              assert (List[I] == NextPage); NextPage += 4096;
            }
            Pages -= Entries;
            if (Pages) List = Resolve (List[511]);
          }
        }
      }
      ZeroMem (Cq, sizeof (*Cq)); Cq->Cid = Sq->Cid + (WrongCid && Qid != 0); Cq->Sqid = Qid + (WrongSqid && Qid != 0);
      Cq->Sc = CqError && Qid != 0 ? 1 : 0; Cq->Sqhd = Tail;
      Cq->Dword0 = Sq->Opc == NVME_ADMIN_SET_FEATURES_CMD ? QueueResult : 0;
      Cq->Pt = Owner.Pt[Qid] ^ 1;
    }
    if (FailDoorbell) return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

UINT16 EFIAPI SetDevicePathNodeLength (VOID *Node, UINTN Length) {
  EFI_DEVICE_PATH_PROTOCOL *Path = Node;
  Path->Length[0] = (UINT8)Length; Path->Length[1] = (UINT8)(Length >> 8); return (UINT16)Length;
}
UINTN EFIAPI DevicePathNodeLength (CONST VOID *Node) {
  CONST EFI_DEVICE_PATH_PROTOCOL *Path = Node;
  return (UINT16)(Path->Length[0] | ((UINT16)Path->Length[1] << 8));
}
static VOID TestGetters (EFI_STATUS Expected) {
  UINT32 Namespace = 0xffffffffU, Parsed = 0;
  EFI_DEVICE_PATH_PROTOCOL *Path = NULL;
  NVME_NAMESPACE_DEVICE_PATH Node = {0};
  Node.Header.Type = MESSAGING_DEVICE_PATH; Node.Header.SubType = MSG_NVME_NAMESPACE_DP;
  SetDevicePathNodeLength (&Node.Header, sizeof (Node)); Node.NamespaceId = 1;
  unsigned BeforeMaps = Mappings, BeforeSubmissions = Submitted, BeforePools = PoolAllocations;
  assert (NvmExpressGetNextNamespace (&Owner.Passthru, &Namespace) == Expected);
  assert (NvmExpressBuildDevicePath (&Owner.Passthru, 1, &Path) == Expected);
  assert (NvmExpressGetNamespace (&Owner.Passthru, &Node.Header, &Parsed) == Expected);
  assert (Namespace == 0xffffffffU && Parsed == 0 && Path == NULL);
  assert (Mappings == BeforeMaps && Submitted == BeforeSubmissions && PoolAllocations == BeforePools);
}

static VOID SetObserved (UINT8 *Data, UINTN Length, UINT8 *Meta, UINTN MetaLength) {
  ObservedData = Data; ObservedDataLength = Length;
  ObservedMetadata = Meta; ObservedMetadataLength = MetaLength;
  OriginalData = 0xdd; OriginalMetadata = 0xee;
  memset (Data, OriginalData, Length); memset (Meta, OriginalMetadata, MetaLength);
  CheckPublication = TRUE;
}
static VOID AssertPattern (UINT8 *Data, UINTN Length, UINT8 Pattern) {
  for (UINTN I = 0; I < Length; I++) assert (Data[I] == Pattern);
}
static VOID ResetControls (VOID) {
  assert (!Mappings && !CommonBuffers && !Events && IsListEmpty (&Owner.DmaRequests));
  Owner.ControllerFailed = Owner.AdmissionBlocked = Owner.RecoveryInProgress = FALSE;
  Owner.ControllerStopped = FALSE; CcRegister = 1;
  FailUnmap = FailFreeBuffer = FailDoorbell = FailDisableWrite = FailCstsRead = HoldRdy = FALSE;
  WrongCid = WrongSqid = CqError = QuiesceOnPoll = FALSE;
  ShortMapCall = ErrorMapCall = FailedUnmapCall = 0;
  CompleteAll = TRUE; NoLateCompletion = TRUE;
  CheckPublication = FALSE; ObservedData = ObservedMetadata = NULL;
}
static VOID DataTransfers (EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *Packet,
  EFI_NVM_EXPRESS_COMMAND *Command) {
  CONST UINTN Lengths[] = {16, 8192, 3 * 4096, 513 * 4096};
  for (unsigned Direction = 0; Direction < 2; Direction++) for (unsigned I = 0; I < 4; I++) {
    ResetControls ();
    UINTN Length = Lengths[I]; UINT8 *Data = malloc (Length), *Meta = malloc (64);
    assert (Data && Meta); SetObserved (Data, Length, Meta, 64);
    Packet->TransferBuffer = Data; Packet->TransferLength = (UINT32)Length;
    Packet->MetadataBuffer = Meta; Packet->MetadataLength = 64;
    Command->Cdw0.Opcode = Direction ? NVME_IO_WRITE_OPC : NVME_IO_READ_OPC;
    unsigned BeforeCommon = CommonAllocations;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, Packet, NULL) == EFI_SUCCESS);
    AssertPattern (Data, Length, Direction ? 0xdd : 0xa5);
    AssertPattern (Meta, 64, Direction ? 0xee : 0x5a);
    assert (!Mappings && !CommonBuffers && IsListEmpty (&Owner.DmaRequests) && !Events);
    if (Length > 8192) assert (CommonAllocations == BeforeCommon + 1);
    free (Data); free (Meta); CheckPublication = FALSE;
  }
  puts ("NVMe data/metadata read-write staging + independent IOVA + chained PRP: PASS");
}
static VOID ProviderFailures (EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *Packet,
  EFI_NVM_EXPRESS_COMMAND *Command) {
  for (unsigned Direction = 0; Direction < 2; Direction++) for (unsigned Case = 0; Case < 11; Case++) {
    ResetControls ();
    UINTN Length = 3 * 4096; UINT8 *Data = malloc (Length), *Meta = malloc (64);
    assert (Data && Meta); SetObserved (Data, Length, Meta, 64);
    Packet->TransferBuffer = Data; Packet->TransferLength = (UINT32)Length;
    Packet->MetadataBuffer = Meta; Packet->MetadataLength = 64;
    Command->Cdw0.Opcode = Direction ? NVME_IO_WRITE_OPC : NVME_IO_READ_OPC;
    unsigned BeforeSubmitted = Submitted, BeforeMaps = MapCalls, BeforeUnmaps = UnmapCalls;
    if (Case == 0) FailedUnmapCall = BeforeUnmaps + 1;
    if (Case == 1) FailedUnmapCall = BeforeUnmaps + 2;
    if (Case >= 2 && Case <= 7) {
      unsigned Position = (Case - 2) / 2 + 1;
      if (Case % 2 == 0) ShortMapCall = BeforeMaps + Position;
      else ErrorMapCall = BeforeMaps + Position;
      FailedUnmapCall = BeforeUnmaps + Position;
    }
    if (Case == 8) FailFreeBuffer = TRUE;
    if (Case == 9) CqError = TRUE;
    if (Case == 10) WrongCid = TRUE;
    EFI_STATUS Status = NvmExpressPassThru (&Owner.Passthru, 1, Packet, NULL);
    assert (EFI_ERROR (Status)); AssertUnchanged ();
    if (Case >= 2 && Case <= 7) assert (Submitted == BeforeSubmitted);
    if (Case < 9) {
      assert (!IsListEmpty (&Owner.DmaRequests));
      NVME_DMA_REQUEST *R = BASE_CR (GetFirstNode (&Owner.DmaRequests), NVME_DMA_REQUEST, Link);
      assert (R->Detached && !R->CallerData && !R->CallerMetadata && R->DeviceWrite == !Direction);
      if (Direction) {
        AssertPattern (R->DataBuffer, Length, 0xdd);
        if (R->MetadataBuffer) AssertPattern (R->MetadataBuffer, 64, 0xee);
      }
      assert (Mappings || R->PrpListHost);
      if (Mappings) {
        VOID *Token = R->MapData ? R->MapData : R->MapMeta ? R->MapMeta : R->MapPrpList;
        TEST_MAPPING *M = Token; assert (M && M->Host != Data && M->Host != Meta && M->Attempts == 1);
      }
      /* Caller storage is actually freed/reused before provider retry. */
      free (Data); free (Meta);
      Data = malloc (Length); Meta = malloc (64); assert (Data && Meta);
      SetObserved (Data, Length, Meta, 64); OriginalData = 0x7c; OriginalMetadata = 0x3d;
      memset (Data, OriginalData, Length); memset (Meta, OriginalMetadata, 64);
      FailedUnmapCall = 0; FailFreeBuffer = FALSE;
      assert (NvmeRetireRequests (&Owner) == EFI_SUCCESS);
      AssertUnchanged ();
    }
    assert (!Mappings && !CommonBuffers && IsListEmpty (&Owner.DmaRequests) && !Events);
    free (Data); free (Meta); CheckPublication = FALSE;
  }
  puts ("NVMe both-direction copyback-before-Unmap-failure, data/metadata/PRP partial-map + Free retries: PASS");
}
static VOID StopFailures (EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *Packet,
  EFI_NVM_EXPRESS_COMMAND *Command) {
  for (unsigned Direction = 0; Direction < 2; Direction++) for (unsigned Case = 0; Case < 4; Case++) {
    ResetControls ();
    UINTN Length = 3 * 4096; UINT8 *Data = malloc (Length), *Meta = malloc (64);
    assert (Data && Meta); SetObserved (Data, Length, Meta, 64);
    Packet->TransferBuffer = Data; Packet->TransferLength = (UINT32)Length;
    Packet->MetadataBuffer = Meta; Packet->MetadataLength = 64;
    Command->Cdw0.Opcode = Direction ? NVME_IO_WRITE_OPC : NVME_IO_READ_OPC;
    CompleteAll = FALSE;
    if (Case == 0) FailDisableWrite = TRUE;
    if (Case == 1) FailCstsRead = TRUE;
    if (Case == 2) HoldRdy = TRUE;
    if (Case == 3) { FailDoorbell = TRUE; FailDisableWrite = TRUE; }
    UINT64 Generation = Owner.DmaGeneration;
    unsigned BeforeUnmaps = UnmapCalls, BeforeFrees = BufferFrees, BeforeSignals = CallerSignals;
    assert (EFI_ERROR (NvmExpressPassThru (&Owner.Passthru, 1, Packet,
      Case == 3 ? (EFI_EVENT)(UINTN)2 : NULL)));
    AssertUnchanged ();
    assert (Owner.ControllerFailed && Owner.AdmissionBlocked && !Owner.ControllerStopped);
    assert (Owner.DmaGeneration == Generation && UnmapCalls == BeforeUnmaps && BufferFrees == BeforeFrees);
    assert (Mappings == 3 && CommonBuffers == 1 && CallerSignals == BeforeSignals && !Events);
    NVME_DMA_REQUEST *R = BASE_CR (GetFirstNode (&Owner.DmaRequests), NVME_DMA_REQUEST, Link);
    assert (R->Submitted && R->Detached && !R->CallerData && !R->CallerMetadata);
    assert (((TEST_MAPPING *)R->MapData)->Host == R->DataBuffer && R->DataBuffer != Data);
    assert (((TEST_MAPPING *)R->MapMeta)->Host == R->MetadataBuffer && R->MetadataBuffer != Meta);
    free (Data); free (Meta);
    Data = malloc (Length); Meta = malloc (64); assert (Data && Meta);
    SetObserved (Data, Length, Meta, 64); OriginalData = 0x7c; OriginalMetadata = 0x3d;
    memset (Data, OriginalData, Length); memset (Meta, OriginalMetadata, 64);
    if (Direction) { AssertPattern (R->DataBuffer, Length, 0xdd); AssertPattern (R->MetadataBuffer, 64, 0xee); }
    FailDoorbell = FailDisableWrite = FailCstsRead = HoldRdy = FALSE;
    assert (NvmeQuiesceController (&Owner) == EFI_SUCCESS);
    assert (Owner.ControllerStopped && Owner.DmaGeneration > Generation && !Mappings && !CommonBuffers);
    assert (IsListEmpty (&Owner.DmaRequests));
    AssertUnchanged (); free (Data); free (Meta); CheckPublication = FALSE;
  }
  puts ("NVMe both-direction failed disable-write/CSTS-read/RDY0/attempted-doorbell retains entire live generation: PASS");
}
static VOID PollReentry (EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET *Packet,
  EFI_NVM_EXPRESS_COMMAND *Command) {
  ResetControls ();
  UINT8 Data[16], Meta[16]; SetObserved (Data, sizeof (Data), Meta, sizeof (Meta));
  Packet->TransferBuffer = Data; Packet->TransferLength = sizeof (Data);
  Packet->MetadataBuffer = Meta; Packet->MetadataLength = sizeof (Meta);
  Command->Cdw0.Opcode = NVME_IO_READ_OPC; QuiesceOnPoll = TRUE;
  assert (EFI_ERROR (NvmExpressPassThru (&Owner.Passthru, 1, Packet, NULL)));
  AssertUnchanged (); assert (Owner.ControllerStopped && !Mappings && !Events && IsListEmpty (&Owner.DmaRequests));
  CheckPublication = FALSE;
  puts ("NVMe synchronous poll callback quiesce cannot authorize stale CQ publication: PASS");
}

int main (int Argc, char **Argv) {
  EFI_BOOT_SERVICES Bs = {.RaiseTPL = Raise, .RestoreTPL = Restore, .Stall = Stall,
    .CreateEvent = EventCreate, .CloseEvent = EventClose, .SetTimer = EventTimer,
    .CheckEvent = EventCheck, .SignalEvent = EventSignal, .OpenProtocol = OpenProtocol,
    .CloseProtocol = CloseProtocol, .UninstallMultipleProtocolInterfaces = UninstallProtocols};
  EFI_PCI_IO_PROTOCOL Pci = {.Mem = {.Read = MemRead, .Write = MemWrite},
    .Pci = {.Read = ConfigRead}, .Map = Map, .Unmap = Unmap, .AllocateBuffer = Allocate,
    .FreeBuffer = Free};
  EFI_NVM_EXPRESS_PASS_THRU_MODE Mode = {.Attributes = EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_LOGICAL | EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_NONBLOCKIO, .IoAlign = 1};
  EFI_NVM_EXPRESS_COMMAND Command = {0}; EFI_NVM_EXPRESS_COMPLETION Completion = {0};
  UINT8 Client[16] = {0};
  EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET Packet = {.NvmeCmd = &Command,
    .NvmeCompletion = &Completion, .QueueType = NVME_IO_QUEUE, .TransferBuffer = Client,
    .TransferLength = sizeof (Client), .CommandTimeout = 50000000};
  gBS = &Bs; Owner.Signature = NVME_CONTROLLER_PRIVATE_DATA_SIGNATURE; Owner.PciIo = &Pci;
  Owner.Passthru.Mode = &Mode; Owner.Passthru.PassThru = NvmExpressPassThru;
  Owner.ControllerData = AllocateZeroPool (4096); Owner.ControllerData->Nn = 1;
  Owner.NumberOfIoQueuePairs = 2; Owner.Cap.Mqes = 255;
  Owner.Buffer = aligned_alloc (4096, 8192); Owner.BufferPciAddr = Owner.Buffer;
  Owner.IoQueueBuffer = aligned_alloc (4096, 16384); Owner.IoQueueBufferPciAddr = Owner.IoQueueBuffer;
  for (unsigned Q = 0; Q < 3; Q++) {
    Owner.SqData[Q].NumberOfEntries = Q == 2 ? 63 : 1; Owner.SqData[Q].EntrySize = 6;
    Owner.CqData[Q].NumberOfEntries = Q == 2 ? 255 : 1; Owner.CqData[Q].EntrySize = 4;
  }
  AqaRegister = 0x00010001; Owner.TimerEvent = (VOID *)1;
  InitializeListHead (&Owner.AsyncPassThruQueue); InitializeListHead (&Owner.UnsubmittedSubtasks);
  InitializeListHead (&Owner.DmaRequests);
  Owner.AdminQueuePages = 2; Owner.IoQueuePages = 4;
  if (Argc > 1 && strcmp (Argv[1], "queue-red") == 0) {
    SuppressAssertions = TRUE;
    Owner.Mapping = malloc (1); Mappings = 1; FailUnmap = TRUE;
    EFI_STATUS Cleanup = NvmExpressDriverCleanUpQueues (&Owner);
    fprintf (stderr, "queue cleanup status=%llx mapping=%p frees=%u\n", (unsigned long long)Cleanup, Owner.Mapping, BufferFrees);
    assert (EFI_ERROR (Cleanup) && Owner.Mapping != NULL && BufferFrees == 0);
    return 0;
  }
  if (Argc > 1 && strcmp (Argv[1], "stop-red") == 0) {
    NVME_CONTROLLER_PRIVATE_DATA *Heap = AllocateZeroPool (sizeof (*Heap));
    CopyMem (Heap, &Owner, sizeof (*Heap));
    InitializeListHead (&Heap->AsyncPassThruQueue); InitializeListHead (&Heap->UnsubmittedSubtasks);
    InitializeListHead (&Heap->DmaRequests);
    ProtocolOwner = Heap; Heap->TimerEvent = NULL; mNvmeControllerNumber = 2;
    EFI_DRIVER_BINDING_PROTOCOL Binding = {0};
    /* OpenProtocol returns this exact private PassThru via a separate owner. */
    EFI_STATUS Stop = NvmExpressDriverBindingStop (&Binding, NULL, 0, NULL);
    fprintf (stderr, "Stop status=%llx disable-writes=%u\n", (unsigned long long)Stop, DisableWrites);
    assert (DisableWrites == 1);
    return 0;
  }
  CompleteAll = TRUE;
  assert (NvmeControllerInitAdminQueues (&Owner) == EFI_SUCCESS);
  if (Argc > 1 && strcmp (Argv[1], "one-queue") == 0) {
    QueueResult = 0;
    assert (NvmeSetFeaturesNumberOfQueues (&Owner, 2) == EFI_SUCCESS);
    assert (Owner.NumberOfIoQueuePairs == 1);
  }
  assert (NvmeControllerInitIoQueues (&Owner) == EFI_SUCCESS);
  Submitted = ResetEntries = Timeouts = 0; CcRegister = 1;
  Command.Cdw0.Opcode = NVME_IO_READ_OPC; Command.Nsid = 1;
  if (Argc > 1 && strcmp (Argv[1], "enable-failures") == 0) {
    for (unsigned Case = 0; Case < 3; Case++) {
      ResetControls ();
      assert (NvmeDisableController (&Owner) == EFI_SUCCESS && Owner.ControllerStopped);
      UINT64 Generation = Owner.DmaGeneration;
      if (Case == 0) FailEnableWrite = TRUE;
      if (Case == 1) HoldRdyZero = TRUE;
      if (Case == 2) FailCstsRead = TRUE;
      assert (EFI_ERROR (NvmeEnableController (&Owner, 6, 4)) && !Owner.ControllerStopped);
      assert (Owner.DmaGeneration == Generation && Owner.Buffer && Owner.IoQueueBuffer);
      FailEnableWrite = HoldRdyZero = FailCstsRead = FALSE; FailDisableWrite = TRUE;
      unsigned BeforeDisable = DisableWrites, BeforeFrees = BufferFrees;
      assert (EFI_ERROR (NvmeQuiesceController (&Owner)) && DisableWrites == BeforeDisable + 1);
      assert (NvmExpressDriverCleanUpQueues (&Owner) == EFI_NOT_READY);
      assert (!Owner.ControllerStopped && Owner.DmaGeneration == Generation && BufferFrees == BeforeFrees);
      FailDisableWrite = FALSE;
      assert (NvmeQuiesceController (&Owner) == EFI_SUCCESS && Owner.ControllerStopped);
      assert (Owner.DmaGeneration == Generation + 1 && DisableWrites == BeforeDisable + 2);
    }
    puts ("NVMe attempted enable-write/RDY1/read failure invalidates prior queue stop authority: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "timer-close-reentry") == 0) {
    Packet.QueueType = NVME_ADMIN_QUEUE;
    Packet.TransferBuffer = NULL; Packet.TransferLength = 0;
    Command.Cdw0.Opcode = NVME_ADMIN_SET_FEATURES_CMD; Command.Nsid = 0;
    ReentryPacket = &Packet; TimerCloseReentry = TRUE;
    assert (NvmExpressPassThru (&Owner.Passthru, 0, &Packet, (EFI_EVENT)(UINTN)2) == EFI_SUCCESS);
    assert (!TimerCloseReentry && CallerSignals == 1 && !Mappings && !Events);
    assert (!Owner.CallbackInProgress && !Owner.ControllerFailed && CurrentTpl == TPL_APPLICATION);
    puts ("NVMe admin timer-close callback + final caller signal retain owner through return: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "negotiation") == 0) {
    CONST UINT32 Results[] = {0, 1, 0x00010000, 0x00010001, 0x00090009, 0x00010001};
    CONST UINT16 Requests[] = {2, 2, 2, 2, 2, 1};
    CONST UINT32 Expected[] = {1, 1, 1, 2, 2, 1};
    for (unsigned I = 0; I < 6; I++) {
      QueueResult = Results[I];
      assert (NvmeSetFeaturesNumberOfQueues (&Owner, Requests[I]) == EFI_SUCCESS);
      assert (Owner.NumberOfIoQueuePairs == Expected[I]);
      assert (!!(Mode.Attributes & EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_NONBLOCKIO) == (Expected[I] == 2));
    }
    puts ("NVMe asymmetric SQ/CQ negotiation and requested queue cap: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "internal-stop-failure") == 0) {
    CompleteAll = FALSE; FailDisableAfter = 2;
    UINT64 Generation = Owner.DmaGeneration;
    assert (EFI_ERROR (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, NULL)));
    assert (ResetEntries == 1 && Submitted == 2 && TimeoutSnapshots == 2 && Timeouts == 2);
    assert (Owner.DmaGeneration == Generation + 1 && !Owner.ControllerStopped && Owner.ControllerFailed);
    assert (Mappings == 1 && IsListEmpty (&Owner.AsyncPassThruQueue) && !Events);
    NVME_DMA_REQUEST *R = BASE_CR (GetFirstNode (&Owner.DmaRequests), NVME_DMA_REQUEST, Link);
    assert (R->Generation == Owner.DmaGeneration && R->Submitted && R->Detached && !R->CallerData);
    FailDisableAfter = 0;
    assert (NvmeQuiesceController (&Owner) == EFI_SUCCESS && !Mappings && IsListEmpty (&Owner.DmaRequests));
    puts ("NVMe prior stop cannot retire new recovery Identify after uncertain second stop: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "unmap-reentry") == 0) {
    ReentryPacket = &Packet; UnmapReentry = TRUE;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, NULL) == EFI_SUCCESS);
    assert (!Mappings && !Events && !Owner.CallbackInProgress && !Owner.ControllerFailed);
    puts ("NVMe Unmap callback public entry/reset/Stop reentry blocked: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "data") == 0) { DataTransfers (&Packet, &Command); goto Done; }
  if (Argc > 1 && strcmp (Argv[1], "failures") == 0) { ProviderFailures (&Packet, &Command); goto Done; }
  if (Argc > 1 && strcmp (Argv[1], "stop-failures") == 0) { StopFailures (&Packet, &Command); goto Done; }
  if (Argc > 1 && strcmp (Argv[1], "poll-reentry") == 0) { PollReentry (&Packet, &Command); goto Done; }
  if (Argc > 1 && strcmp (Argv[1], "async-tpl") == 0) {
    ReentryPacket = &Packet; CompleteOnRestore = TRUE;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, (EFI_EVENT)(UINTN)2) == EFI_SUCCESS);
    assert (CallerSignals == 1 && CurrentTpl == TPL_APPLICATION);
    assert (IsListEmpty (&Owner.AsyncPassThruQueue) && IsListEmpty (&Owner.DmaRequests));
    assert (!Mappings && !Events && !Owner.ControllerFailed);
    puts ("NVMe async immediate RestoreTPL completion/reentry: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "one-queue") == 0) {
    assert (!(Mode.Attributes & EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_NONBLOCKIO));
    assert (Owner.SqBuffer[2] == NULL && Owner.CqBuffer[2] == NULL);
    assert (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, (EFI_EVENT)(UINTN)2) == EFI_SUCCESS);
    assert (!CallerSignals && Submitted == 1 && !Mappings && !Events && CurrentTpl == TPL_APPLICATION);
    puts ("NVMe one I/O queue ignores Event and completes synchronously: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "queue-mismatch") == 0) {
    QueueResult = 0;
    assert (EFI_ERROR (NvmeControllerReset (&Owner)));
    assert (Owner.NumberOfIoQueuePairs == 2 && (Mode.Attributes & EFI_NVM_EXPRESS_PASS_THRU_ATTRIBUTES_NONBLOCKIO));
    assert (Owner.ControllerFailed && Owner.ControllerStopped && !Mappings && !Events);
    puts ("NVMe reset queue mismatch retains installed child capability identity: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "map-quiesce") == 0) {
    QuiesceOnMap = TRUE;
    assert (EFI_ERROR (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, NULL)));
    assert (Submitted == 0 && !Mappings && !Events && CurrentTpl == TPL_APPLICATION);
    assert (Owner.AdmissionBlocked && Owner.ControllerStopped && IsListEmpty (&Owner.DmaRequests));
    puts ("NVMe Map callback quiesce prevents stale submission: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "map-nested") == 0) {
    EFI_NVM_EXPRESS_COMMAND OtherCommand = Command;
    EFI_NVM_EXPRESS_COMPLETION OtherCompletion = {0};
    UINT8 OtherClient[16] = {0};
    EFI_NVM_EXPRESS_PASS_THRU_COMMAND_PACKET Other = Packet;
    Other.NvmeCmd = &OtherCommand; Other.NvmeCompletion = &OtherCompletion;
    Other.TransferBuffer = OtherClient;
    OtherCommand.Flags = CDW10_VALID; OtherCommand.Cdw10 = 0x33333333U;
    Command.Flags = CDW10_VALID; Command.Cdw10 = 0x22222222U;
    NestedPacket = &Other; ReentryPacket = &Packet; NestedOnMap = TRUE;
    assert (NvmExpressPassThru (&Owner.Passthru, 1, &Packet, (EFI_EVENT)(UINTN)2) == EFI_SUCCESS);
    ProcessAsyncTaskList (NULL, &Owner);
    assert (Submitted == 2 && NestedSignals == 1 && CallerSignals == 1 && !Owner.ControllerFailed);
    assert (!Mappings && IsListEmpty (&Owner.DmaRequests) && CurrentTpl == TPL_APPLICATION);
    puts ("NVMe nested Map callback keeps async submission slots distinct: PASS");
    goto Done;
  }
  if (Argc > 1 && strcmp (Argv[1], "getters") == 0) {
    Owner.AdmissionBlocked = Owner.RecoveryInProgress = TRUE;
    GetterReentry = TRUE;
    assert (NvmeEnableController (&Owner, 6, 4) == EFI_SUCCESS && GetterCalls == 2);
    GetterReentry = FALSE;
    Owner.ControllerFailed = TRUE; TestGetters (EFI_DEVICE_ERROR);
    Owner.ControllerFailed = Owner.AdmissionBlocked = Owner.RecoveryInProgress = FALSE;
    FreePool (Owner.ControllerData); Owner.ControllerData = NULL; TestGetters (EFI_DEVICE_ERROR);
    puts ("NVMe public getters reject recovery/failed/missing-data: PASS");
    goto Done;
  }
  CompleteAll = FALSE;
  Command.Cdw0.Opcode = NVME_IO_READ_OPC; Command.Nsid = 1;
  EFI_STATUS Status = NvmExpressPassThru (&Owner.Passthru, 1, &Packet, NULL);
  fprintf (stderr, "client returned status=%llx resets=%u timeouts=%u submissions=%u events=%u maps=%u\n",
    (unsigned long long)Status, ResetEntries, Timeouts, Submitted, Events, Mappings);
  assert (ResetEntries == 1 && TimeoutSnapshots == 2);
  assert (EFI_ERROR (Status) && Timeouts == 2 && !Events && !Mappings);
Done:
  if (Owner.ControllerData) FreePool (Owner.ControllerData);
  free (Owner.Buffer); free (Owner.IoQueueBuffer);
  printf ("NVMe DEBUG host mode=%s PASS\n", Argc > 1 ? Argv[1] : "recursion");
  return 0;
}
