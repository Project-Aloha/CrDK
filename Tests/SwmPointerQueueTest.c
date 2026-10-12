/** Exercise the real SWM queue callbacks and layouts. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Protocol/AbsolutePointer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Override/MU_PLUS/MsGraphicsPkg/SimpleWindowManagerDxe/WindowManager.h"

/* Assertions remain active in the RELEASE test, independently of DebugLib. */
#undef assert
#define assert(E) do { if (!(E)) { fprintf (stderr, "%s:%d: %s\n", __FILE__, __LINE__, #E); abort (); } } while (0)

static EFI_BOOT_SERVICES  Services;
EFI_BOOT_SERVICES         *gBS = &Services;
static EFI_TPL            CurrentTpl = TPL_APPLICATION;
static UINTN              Raises, Restores, TplDepth;
static EFI_TPL            TplStack[32];
static WINMGR_CLIENT      Clients[2];
static UINTN              Signals[2], Notifications[2];
static BOOLEAN            NotifySignal[2], NotifyPeek[2], NotifyConsume[2];

BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *F, UINTN L, CONST CHAR8 *D) {
  fprintf (stderr, "%s:%zu %s\n", F, (size_t)L, D); abort ();
}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN L) { (void)L; return FALSE; }
VOID EFIAPI DebugPrint (UINTN L, CONST CHAR8 *F, ...) { (void)L; (void)F; }

static EFI_TPL EFIAPI Raise (EFI_TPL Tpl) {
  assert (Tpl == TPL_NOTIFY && CurrentTpl <= Tpl && TplDepth < 32);
  EFI_TPL Previous = CurrentTpl;
  TplStack[TplDepth++] = Previous; CurrentTpl = Tpl; ++Raises;
  return Previous;
}
static VOID EFIAPI Restore (EFI_TPL Tpl) {
  assert (CurrentTpl == TPL_NOTIFY && TplDepth && TplStack[TplDepth - 1] == Tpl);
  --TplDepth; CurrentTpl = Tpl; ++Restores;
}
static EFI_STATUS EFIAPI Signal (EFI_EVENT Event) {
  assert (CurrentTpl == TPL_NOTIFY);
  UINTN Index = Event == &Clients[0] ? 0 : 1;
  assert (Event == &Clients[Index]); ++Signals[Index];
  return EFI_SUCCESS;
}

#include "swm_pointer_queue_actual.inc"

static UINTN IndexOf (WINMGR_CLIENT *C) {
  UINTN I = C == &Clients[0] ? 0 : 1; assert (C == &Clients[I]); return I;
}
static BOOLEAN EFIAPI Notify (VOID *Context) {
  WINMGR_CLIENT *C = Context; UINTN I = IndexOf (C);
  assert (CurrentTpl == TPL_NOTIFY); ++Notifications[I];
  if (NotifyPeek[I]) {
    MS_SWM_ABSOLUTE_POINTER_QUEUE Before = C->Queue;
    EFI_ABSOLUTE_POINTER_STATE Out;
    assert (PeekAtAbsolutePointerEventInQueue (C, &Out) == EFI_SUCCESS);
    assert (!memcmp (&C->Queue, &Before, sizeof (Before)));
  }
  if (NotifyConsume[I]) {
    /* A synchronous callback consumes the just-enqueued single sample. */
    EFI_ABSOLUTE_POINTER_STATE Out;
    NotifyConsume[I] = FALSE;
    assert (ExtractAbsolutePointerEventFromQueue (C, &Out) == EFI_SUCCESS);
    assert (C->Queue.bQueueEmpty);
  }
  return NotifySignal[I];
}
static VOID Init (VOID) {
  memset (Clients, 0, sizeof (Clients));
  memset (Signals, 0, sizeof (Signals)); memset (Notifications, 0, sizeof (Notifications));
  memset (NotifySignal, 0, sizeof (NotifySignal)); memset (NotifyPeek, 0, sizeof (NotifyPeek));
  memset (NotifyConsume, 0, sizeof (NotifyConsume));
  for (UINTN I = 0; I < 2; ++I) {
    Clients[I].Signature = WINMGR_CLIENT_SIGNATURE;
    Clients[I].Queue.bQueueEmpty = TRUE;
    Clients[I].ClientAbsPtr.WaitForInput = &Clients[I];
  }
  Services.RaiseTPL = Raise; Services.RestoreTPL = Restore; Services.SignalEvent = Signal;
  CurrentTpl = TPL_APPLICATION; Raises = Restores = TplDepth = 0;
}
static VOID Balanced (VOID) { assert (!TplDepth && Raises == Restores); }
static VOID Put (UINTN I, UINT64 X, UINT32 Buttons) {
  EFI_ABSOLUTE_POINTER_STATE State = { X, 3 * X + 7, 0xf00d, Buttons };
  EFI_ABSOLUTE_POINTER_STATE Before = State; EFI_TPL Tpl = CurrentTpl;
  assert (InsertPointerEventIntoQueue (&Clients[I], &State) == EFI_SUCCESS);
  assert (!memcmp (&State, &Before, sizeof (State)) && CurrentTpl == Tpl); Balanced ();
  assert (Clients[I].Queue.QueueInputPosition < POINTER_STATE_INPUT_QUEUE_SIZE);
  assert (Clients[I].Queue.QueueOutputPosition < POINTER_STATE_INPUT_QUEUE_SIZE);
}
static VOID Take (UINTN I, UINT64 X, UINT32 Buttons) {
  EFI_ABSOLUTE_POINTER_STATE State, Peek;
  MS_SWM_ABSOLUTE_POINTER_QUEUE Before = Clients[I].Queue;
  UINTN BeforeSignals = Signals[I], BeforeNotifications = Notifications[I]; EFI_TPL Tpl = CurrentTpl;
  assert (PeekAtAbsolutePointerEventInQueue (&Clients[I], &Peek) == EFI_SUCCESS);
  assert (!memcmp (&Before, &Clients[I].Queue, sizeof (Before)));
  assert (Signals[I] == BeforeSignals && Notifications[I] == BeforeNotifications);
  assert (ExtractAbsolutePointerEventFromQueue (&Clients[I], &State) == EFI_SUCCESS);
  assert (State.CurrentX == X && State.CurrentY == 3 * X + 7);
  assert (!State.CurrentZ && State.ActiveButtons == (Buttons & 1));
  assert (State.CurrentX == Peek.CurrentX && State.CurrentY == Peek.CurrentY &&
          State.CurrentZ == Peek.CurrentZ && State.ActiveButtons == Peek.ActiveButtons);
  assert (CurrentTpl == Tpl); Balanced ();
}
static VOID Empty (UINTN I) {
  EFI_ABSOLUTE_POINTER_STATE State; memset (&State, 0xa5, sizeof (State));
  EFI_ABSOLUTE_POINTER_STATE Before = State;
  MS_SWM_ABSOLUTE_POINTER_QUEUE Queue = Clients[I].Queue;
  UINTN S = Signals[I], N = Notifications[I]; EFI_TPL Tpl = CurrentTpl;
  assert (Queue.bQueueEmpty);
  assert (PeekAtAbsolutePointerEventInQueue (&Clients[I], &State) == EFI_NOT_FOUND);
  assert (!memcmp (&State, &Before, sizeof (State)));
  assert (ExtractAbsolutePointerEventFromQueue (&Clients[I], &State) == EFI_NOT_FOUND);
  assert (!memcmp (&State, &Before, sizeof (State)) && !memcmp (&Queue, &Clients[I].Queue, sizeof (Queue)));
  assert (Signals[I] == S && Notifications[I] == N && CurrentTpl == Tpl); Balanced ();
}
static VOID Motion (VOID) {
  Init ();
  for (UINT64 X = 1; X <= 10000; ++X) Put (0, X, 0);
  assert (Signals[0] == 10000);
  Take (0, 1, 0); Take (0, 10000, 0); Empty (0);
}
static VOID Edges (VOID) {
  Init ();
  for (UINT64 X = 1; X <= 20; ++X) Put (0, X, 0);
  for (UINT64 X = 21; X <= 3020; ++X) Put (0, X, 1);
  for (UINT64 X = 3021; X <= 7020; ++X) Put (0, X, 0);
  Take (0, 1, 0); Take (0, 20, 0); Take (0, 21, 1);
  Take (0, 3020, 1); Take (0, 3021, 0); Take (0, 7020, 0); Empty (0);
  Put (0, 8001, 1); Put (0, 8002, 0); Put (0, 8003, 1);
  Take (0, 8001, 1); Take (0, 8002, 0); Take (0, 8003, 1); Empty (0);
}
static VOID Full (VOID) {
  Init ();
  for (UINT64 X = 1; X <= 1373; ++X) Put (0, X, (UINT32)X & 1);
  for (UINT64 X = 1324; X <= 1373; ++X) Take (0, X, (UINT32)X & 1);
  Empty (0);
}
static VOID Wrap (VOID) {
  Init ();
  for (UINT64 X = 1; X <= 49; ++X) Put (0, X, (UINT32)X & 1);
  for (UINT64 X = 1; X <= 49; ++X) Take (0, X, (UINT32)X & 1);
  Empty (0); assert (Clients[0].Queue.QueueInputPosition == 49);
  for (UINT64 X = 1001; X <= 7000; ++X) Put (0, X, 0);
  Take (0, 1001, 0); Take (0, 7000, 0); Empty (0);
}
static VOID FullTail (VOID) {
  Init ();
  for (UINT64 X = 1; X <= 49; ++X) Put (0, X, (UINT32)X & 1);
  Put (0, 50, 1); Put (0, 51, 1); Put (0, 52, 0);
  for (UINT64 X = 2; X <= 49; ++X) Take (0, X, (UINT32)X & 1);
  Take (0, 51, 1); Take (0, 52, 0); Empty (0);
}
static VOID ReadBetween (VOID) {
  Init (); Put (0, 1, 1); Put (0, 2, 1); Put (0, 3, 1);
  Take (0, 1, 1); Put (0, 4, 1); Put (0, 5, 1);
  Take (0, 3, 1); Take (0, 5, 1); Empty (0);
  Put (0, 6, 0); Take (0, 6, 0); Empty (0);
}
static VOID NotificationsTest (VOID) {
  Init (); Clients[0].DataNotificationCallback = Notify;
  Clients[0].DataNotificationContext = &Clients[0]; NotifyPeek[0] = TRUE;
  const EFI_TPL Tpls[] = { TPL_APPLICATION, TPL_CALLBACK, TPL_NOTIFY };
  for (UINTN I = 0; I < 3; ++I) {
    CurrentTpl = Tpls[I];
    Put (0, 10 * I + 1, 0); Put (0, 10 * I + 2, 0); Put (0, 10 * I + 3, 0);
    assert (Signals[0] == 0 && Notifications[0] == 4 * I + 3);
    Take (0, 10 * I + 1, 0); Take (0, 10 * I + 3, 0); Empty (0);
    assert (Notifications[0] == 4 * (I + 1));
  }
  NotifySignal[0] = TRUE;
  Put (0, 100, 1); Put (0, 101, 0);
  assert (Signals[0] == 2); Take (0, 100, 1); assert (Signals[0] == 3);
  Take (0, 101, 0); assert (Signals[0] == 3); Empty (0);
  NotifyConsume[0] = TRUE; NotifySignal[0] = FALSE;
  Put (0, 102, 1); Empty (0); assert (Signals[0] == 3);
}
static VOID Isolation (VOID) {
  Init ();
  Put (1, 0x100000001ULL, 0xfffffffeU); Put (1, 0x100000002ULL, 0xffffffffU);
  MS_SWM_ABSOLUTE_POINTER_QUEUE Other = Clients[1].Queue;
  for (UINT64 X = 1; X <= 2000; ++X) Put (0, X, (UINT32)X & 1);
  assert (!memcmp (&Other, &Clients[1].Queue, sizeof (Other)) && Signals[1] == 2);
  Take (1, 0x100000001ULL, 0); Take (1, 0x100000002ULL, 1); Empty (1);
  for (UINT64 X = 1951; X <= 2000; ++X) Take (0, X, (UINT32)X & 1);
  Empty (0); Empty (1);
}
static VOID Basic (VOID) {
  Init (); Empty (0); Put (0, 42, 3); Take (0, 42, 1); Empty (0);
  assert (Signals[0] == 1 && Signals[1] == 0);
}
int main (int Argc, char **Argv) {
  assert (Argc == 2);
  if (!strcmp (Argv[1], "motion")) Motion ();
  else if (!strcmp (Argv[1], "edges")) Edges ();
  else if (!strcmp (Argv[1], "full")) Full ();
  else if (!strcmp (Argv[1], "wrap")) Wrap ();
  else if (!strcmp (Argv[1], "full-tail")) FullTail ();
  else if (!strcmp (Argv[1], "read-between")) ReadBetween ();
  else if (!strcmp (Argv[1], "notifications")) NotificationsTest ();
  else if (!strcmp (Argv[1], "isolation")) Isolation ();
  else { assert (!strcmp (Argv[1], "basic")); Basic (); }
  Balanced (); printf ("PASS actual SWM queue %s\n", Argv[1]); return 0;
}
