/** @file
 *  Host regression coverage for the private MU bus adapters.
 *  SPDX-License-Identifier: MIT
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>

static unsigned Allocations;
static unsigned AllocationAttempts;
static unsigned FailAllocationAt;
static unsigned Signals;
static EFI_STATUS TimerStatus = EFI_SUCCESS;
static EFI_STATUS CreateStatus = EFI_SUCCESS;
static VOID (*SignalHook)(VOID);
static EFI_EVENT_NOTIFY Work;
static VOID *WorkContext;
static EFI_EVENT PendingEvent;
static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;

VOID *EFIAPI AllocateZeroPool (UINTN Size) {
  if (++AllocationAttempts == FailAllocationAt) return NULL;
  VOID *P = calloc (1, Size);
  if (P != NULL) ++Allocations;
  return P;
}
VOID EFIAPI FreePool (VOID *P) {
  assert(P != NULL && Allocations != 0);
  --Allocations;
  free(P);
}
VOID *EFIAPI CopyMem (VOID *D, CONST VOID *S, UINTN N) { return memcpy(D, S, N); }
VOID *EFIAPI ZeroMem (VOID *D, UINTN N) { return memset(D, 0, N); }
UINT32 EFIAPI InterlockedCompareExchange32 (volatile UINT32 *P, UINT32 C, UINT32 E) {
  UINT32 Old = *P;
  if (Old == C) *P = E;
  return Old;
}
static EFI_STATUS EFIAPI CreateEvent (UINT32 Type, EFI_TPL Tpl,
    EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *Event) {
  assert(Type == (EVT_TIMER | EVT_NOTIFY_SIGNAL));
  assert(Tpl == TPL_CALLBACK);
  assert(PendingEvent == NULL);
  if (EFI_ERROR(CreateStatus)) return CreateStatus;
  Work = Notify;
  WorkContext = Context;
  PendingEvent = (EFI_EVENT)0x1111;
  *Event = PendingEvent;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetTimer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time) {
  assert(Event == PendingEvent && Type == TimerRelative && Time != 0);
  return TimerStatus;
}
static EFI_STATUS EFIAPI CloseEvent (EFI_EVENT Event) {
  assert(Event == PendingEvent);
  PendingEvent = NULL;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SignalEvent (EFI_EVENT Event) {
  assert(Event == (EFI_EVENT)0x2222);
  if (SignalHook != NULL) SignalHook();
  ++Signals;
  return EFI_SUCCESS;
}
static void InitBootServices(void) {
  BootServices.CreateEvent = CreateEvent;
  BootServices.SetTimer = SetTimer;
  BootServices.CloseEvent = CloseEvent;
  BootServices.SignalEvent = SignalEvent;
}

static EFI_GUID *StandardGuid;
static EFI_GUID *PrivateGuid;
static EFI_GUID *PublishedGuid;
static VOID *StandardBackend;
static VOID *PrivateBackend;
static VOID *LastInstalled;
static EFI_STATUS StandardLocateStatus = EFI_NOT_FOUND;
static EFI_STATUS InstallStatus = EFI_SUCCESS;
static unsigned PrivateLocateCalls;
static unsigned InstallCalls;

static EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Type,
    EFI_GUID *Guid, VOID *Key, UINTN *Count, EFI_HANDLE **Handles) {
  assert(Type == ByProtocol && Guid == StandardGuid && Key == NULL);
  if (EFI_ERROR(StandardLocateStatus)) return StandardLocateStatus;
  *Handles = AllocateZeroPool(sizeof(**Handles));
  if (*Handles == NULL) return EFI_OUT_OF_RESOURCES;
  (*Handles)[0] = (EFI_HANDLE)0x4444;
  *Count = 1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenStandard(EFI_HANDLE Handle, EFI_GUID *Guid,
    VOID **Protocol, EFI_HANDLE Image, EFI_HANDLE Controller, UINT32 Attributes) {
  assert(Handle == (EFI_HANDLE)0x4444 && Guid == StandardGuid);
  assert(Image == (EFI_HANDLE)0xaaaa && Controller == NULL);
  assert(Attributes == EFI_OPEN_PROTOCOL_GET_PROTOCOL);
  *Protocol = StandardBackend;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI LocatePrivate(EFI_GUID *Guid, VOID *Registration,
    VOID **Protocol) {
  assert(Guid == PrivateGuid && Registration == NULL);
  ++PrivateLocateCalls;
  *Protocol = PrivateBackend;
  return (PrivateBackend == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InstallAlias(EFI_HANDLE *Handle, ...) {
  va_list Args;
  assert(*Handle == NULL);
  va_start(Args, Handle);
  assert(va_arg(Args, EFI_GUID *) == PublishedGuid);
  LastInstalled = va_arg(Args, VOID *);
  assert(LastInstalled != NULL && va_arg(Args, VOID *) == NULL);
  va_end(Args);
  ++InstallCalls;
  if (EFI_ERROR(InstallStatus)) return InstallStatus;
  *Handle = (EFI_HANDLE)0x5555;
  return EFI_SUCCESS;
}
static void InitEntryServices(void) {
  BootServices.LocateHandleBuffer = LocateHandles;
  BootServices.OpenProtocol = OpenStandard;
  BootServices.LocateProtocol = LocatePrivate;
  BootServices.InstallMultipleProtocolInterfaces = InstallAlias;
}
