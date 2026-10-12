/* Actual WDF OSKAL/IPCC code against host API-shape and concurrency mocks.
   SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <ucsi/Ipcc.h>
#include "../Library/OskInterruptLib/oskinterrupt_wdf.c"

struct WDF_TEST_INTERRUPT {
  VOID *Context;
  WDF_INTERRUPT_CONFIG Config;
  pthread_mutex_t Lock;
};
STATIC WDFINTERRUPT Objects[16];
STATIC UINTN ObjectCount, DisableCalls;
STATIC UINT32 SyncFailAt;
STATIC BOOLEAN CreateFails;
STATIC _Thread_local KIRQL CurrentIrql;
STATIC pthread_mutex_t TestLock = PTHREAD_MUTEX_INITIALIZER;
STATIC pthread_cond_t TestCondition = PTHREAD_COND_INITIALIZER;
STATIC BOOLEAN HandlerEntered, AllowReturn, WaitEntered, StopDone;
STATIC CR_STATUS StopStatus;
STATIC UINTN HandlerCalls;

KIRQL KeGetCurrentIrql(VOID) { return CurrentIrql; }
VOID ExInitializeRundownProtection(EX_RUNDOWN_REF *Ref) {
  assert(pthread_mutex_init(&Ref->Mutex, NULL) == 0);
  assert(pthread_cond_init(&Ref->Condition, NULL) == 0);
  Ref->Active = 0; Ref->Closing = FALSE;
}
VOID ExReInitializeRundownProtection(EX_RUNDOWN_REF *Ref) {
  pthread_mutex_lock(&Ref->Mutex);
  assert(Ref->Active == 0 && Ref->Closing);
  Ref->Closing = FALSE;
  pthread_mutex_unlock(&Ref->Mutex);
}
BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *Ref) {
  BOOLEAN Acquired;
  assert(CurrentIrql <= DISPATCH_LEVEL);
  pthread_mutex_lock(&Ref->Mutex);
  Acquired = !Ref->Closing;
  if (Acquired) ++Ref->Active;
  pthread_mutex_unlock(&Ref->Mutex);
  return Acquired;
}
VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *Ref) {
  pthread_mutex_lock(&Ref->Mutex);
  assert(Ref->Active != 0);
  if (--Ref->Active == 0) pthread_cond_broadcast(&Ref->Condition);
  pthread_mutex_unlock(&Ref->Mutex);
}
VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *Ref) {
  assert(CurrentIrql == PASSIVE_LEVEL);
  pthread_mutex_lock(&Ref->Mutex);
  Ref->Closing = TRUE;
  pthread_mutex_lock(&TestLock);
  WaitEntered = TRUE;
  pthread_cond_broadcast(&TestCondition);
  pthread_mutex_unlock(&TestLock);
  while (Ref->Active != 0) pthread_cond_wait(&Ref->Condition, &Ref->Mutex);
  pthread_mutex_unlock(&Ref->Mutex);
}
VOID *WdfTestGetContext(WDFINTERRUPT Interrupt) { return Interrupt->Context; }
NTSTATUS WdfInterruptCreate(WDFDEVICE Device, WDF_INTERRUPT_CONFIG *Config,
                           WDF_OBJECT_ATTRIBUTES *Attributes, WDFINTERRUPT *Interrupt) {
  assert(Device != NULL && CurrentIrql == PASSIVE_LEVEL);
  if (CreateFails) return STATUS_INSUFFICIENT_RESOURCES;
  assert(ObjectCount < sizeof(Objects) / sizeof(Objects[0]));
  WDFINTERRUPT Object = calloc(1, sizeof(*Object));
  assert(Object != NULL);
  Object->Context = calloc(1, Attributes->ContextSize);
  assert(Object->Context != NULL);
  assert(pthread_mutex_init(&Object->Lock, NULL) == 0);
  Object->Config = *Config;
  Objects[ObjectCount++] = Object;
  *Interrupt = Object;
  return STATUS_SUCCESS;
}
VOID WdfInterruptDisable(WDFINTERRUPT Interrupt) {
  assert(Interrupt != NULL && CurrentIrql == PASSIVE_LEVEL);
  ++DisableCalls;
}
BOOLEAN WdfInterruptSynchronize(WDFINTERRUPT Interrupt,
                                BOOLEAN (*Callback)(WDFINTERRUPT, WDFCONTEXT), WDFCONTEXT Context) {
  BOOLEAN Result;
  pthread_mutex_lock(&Interrupt->Lock);
  if (SyncFailAt != 0 && --SyncFailAt == 0) Result = FALSE;
  else Result = Callback(Interrupt, Context);
  pthread_mutex_unlock(&Interrupt->Lock);
  return Result;
}
BOOLEAN WdfInterruptQueueDpcForIsr(WDFINTERRUPT Interrupt) { (VOID)Interrupt; return TRUE; }
BOOLEAN WdfInterruptQueueWorkItemForIsr(WDFINTERRUPT Interrupt) { (VOID)Interrupt; return TRUE; }

STATIC CR_INTERRUPT_CONFIG Configuration(VOID) {
  CR_INTERRUPT_CONFIG Config = {0};
  Config.Device = (VOID *)(uintptr_t)1;
  Config.WdfInterruptConfig.InterruptRaw = (VOID *)(uintptr_t)2;
  Config.WdfInterruptConfig.InterruptTranslated = (VOID *)(uintptr_t)3;
  return Config;
}
STATIC VOID Count(VOID *Context) { assert(Context == &HandlerCalls); ++HandlerCalls; }
STATIC VOID Blocking(VOID *Context) {
  assert(Context == &HandlerCalls);
  pthread_mutex_lock(&TestLock);
  ++HandlerCalls;
  HandlerEntered = TRUE;
  pthread_cond_broadcast(&TestCondition);
  while (!AllowReturn) pthread_cond_wait(&TestCondition, &TestLock);
  pthread_mutex_unlock(&TestLock);
}
STATIC BOOLEAN Fire(WDFINTERRUPT Interrupt) {
  BOOLEAN Recognized;
  CurrentIrql = 3;
  pthread_mutex_lock(&Interrupt->Lock);
  Recognized = Interrupt->Config.EvtInterruptIsr(Interrupt, 0);
  pthread_mutex_unlock(&Interrupt->Lock);
  CurrentIrql = PASSIVE_LEVEL;
  return Recognized;
}
STATIC VOID *RunDeferred(VOID *Argument) {
  WDFINTERRUPT Interrupt = Argument;
  if (Interrupt->Config.EvtInterruptDpc != NULL) {
    CurrentIrql = DISPATCH_LEVEL;
    Interrupt->Config.EvtInterruptDpc(Interrupt, NULL);
  } else {
    CurrentIrql = PASSIVE_LEVEL;
    Interrupt->Config.EvtInterruptWorkItem(Interrupt, NULL);
  }
  return NULL;
}
STATIC VOID *StopDeferred(VOID *Argument) {
  CR_STATUS Status = CrUnregisterInterrupt(Argument);
  pthread_mutex_lock(&TestLock);
  StopStatus = Status; StopDone = TRUE;
  pthread_cond_broadcast(&TestCondition);
  pthread_mutex_unlock(&TestLock);
  return NULL;
}
STATIC VOID TeardownWaitsForDeferred(BOOLEAN WorkItem) {
  CR_INTERRUPT_CONFIG Config = Configuration();
  pthread_t CallbackThread, StopThread;
  HandlerEntered = AllowReturn = WaitEntered = StopDone = FALSE;
  Config.Handler = Blocking; Config.Param = &HandlerCalls;
  Config.LIrqLCbType = WorkItem ? OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM :
                                OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC;
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS);
  assert(pthread_create(&CallbackThread, NULL, RunDeferred, Config.Interrupt) == 0);
  pthread_mutex_lock(&TestLock);
  while (!HandlerEntered) pthread_cond_wait(&TestCondition, &TestLock);
  pthread_mutex_unlock(&TestLock);
  assert(pthread_create(&StopThread, NULL, StopDeferred, &Config) == 0);
  pthread_mutex_lock(&TestLock);
  while (!WaitEntered) pthread_cond_wait(&TestCondition, &TestLock);
  assert(!StopDone); /* Callback still holds the owner; stop cannot succeed. */
  AllowReturn = TRUE;
  pthread_cond_broadcast(&TestCondition);
  pthread_mutex_unlock(&TestLock);
  assert(pthread_join(CallbackThread, NULL) == 0);
  assert(pthread_join(StopThread, NULL) == 0);
  assert(StopDone && StopStatus == CR_SUCCESS && !CrInterruptIsRegistered(&Config));
  UINTN Before = HandlerCalls;
  RunDeferred(Config.Interrupt); /* Already queued work after detachment is harmless. */
  CurrentIrql = PASSIVE_LEVEL;
  assert(HandlerCalls == Before && !Fire(Config.Interrupt));
}

STATIC UINT32 Control = 0xa5U, ReceiveId = ~0U;
STATIC UINTN Reads, Writes, Wakes;
STATIC BOOLEAN SignalEnabled;
UINT32 CrMmioRead32(UINTN Address) {
  ++Reads;
  if (Address == CR_IPCC_BASE + 8) return Control;
  assert(Address == CR_IPCC_BASE + 0x10);
  return ReceiveId;
}
UINT32 CrMmioWrite32(UINTN Address, UINT32 Value) {
  ++Writes;
  if (Address == CR_IPCC_BASE + 8) Control = Value;
  else {
    assert(Value == CR_IPCC_LPASS_GLINK);
    if (Address == CR_IPCC_BASE + 0x14) SignalEnabled = TRUE;
    else if (Address == CR_IPCC_BASE + 0x18) SignalEnabled = FALSE;
    else if (Address == CR_IPCC_BASE + 0x1c) ReceiveId = ~0U;
    else assert(Address == CR_IPCC_BASE + 0xc);
  }
  return Value;
}
STATIC CR_STATUS Wake(VOID *Context) {
  CR_IPCC *Ipcc = Context;
  assert(CurrentIrql == 3 && Ipcc->Pending != 0);
  ++Wakes;
  return CR_SUCCESS;
}
STATIC VOID PortableIpcc(VOID) {
  CR_IPCC Ipcc = {0};
  CR_INTERRUPT_CONFIG Config = Configuration();
  BOOLEAN Received;
  assert(CrIpccStart(&Ipcc, CR_IPCC_BASE, &Config, Wake, &Ipcc) == CR_SUCCESS);
  assert(SignalEnabled && Control == 0xa4 && CrInterruptIsRegistered(&Ipcc.Interrupt));
  UINTN Before = Reads;
  for (UINTN I = 0; I < 1000; ++I)
    assert(CrIpccService(&Ipcc, &Received) == CR_SUCCESS && !Received);
  assert(Reads == Before);
  ReceiveId = CR_IPCC_LPASS_GLINK;
  assert(Fire(Ipcc.Interrupt.Interrupt) && Wakes == 1 && !SignalEnabled);
  assert(CrIpccService(&Ipcc, &Received) == CR_SUCCESS && Received && SignalEnabled);
  CurrentIrql = DISPATCH_LEVEL;
  assert(CrIpccStop(&Ipcc) == CR_BUSY && Ipcc.Registered && !SignalEnabled);
  CurrentIrql = PASSIVE_LEVEL;
  assert(CrIpccStop(&Ipcc) == CR_SUCCESS && !Ipcc.Registered && Control == 0xa5);
  WDFINTERRUPT Handle = Ipcc.Interrupt.Interrupt;
  Before = ObjectCount;
  assert(Handle != NULL && !CrInterruptIsRegistered(&Ipcc.Interrupt));
  assert(CrIpccStart(&Ipcc, CR_IPCC_BASE, &Ipcc.Interrupt, Wake, &Ipcc) == CR_SUCCESS);
  assert(Ipcc.Interrupt.Interrupt == Handle && ObjectCount == Before);
  ReceiveId = CR_IPCC_LPASS_GLINK;
  assert(Fire(Handle) && Wakes == 2);
  assert(CrIpccService(&Ipcc, &Received) == CR_SUCCESS && Received);
  assert(CrIpccStop(&Ipcc) == CR_SUCCESS && Control == 0xa5);
  Before = Writes;
  CrIpccSend(CR_IPCC_BASE);
  assert(Writes == Before + 1);
}

int main(void) {
  CR_INTERRUPT_CONFIG Config = Configuration();
  Config.Handler = Count; Config.Param = &HandlerCalls;
  assert(CrRegisterInterrupt(NULL) == CR_INVALID_PARAMETER);
  assert(!CrInterruptIsRegistered(NULL));
  CreateFails = TRUE;
  assert(CrRegisterInterrupt(&Config) == CR_OUT_OF_RESOURCES && Config.Interrupt == NULL);
  CreateFails = FALSE;
  assert(CrRegisterInterrupt(&Config) == CR_SUCCESS && CrInterruptIsRegistered(&Config));
  assert(CrRegisterInterrupt(&Config) == CR_ALREADY_STARTED && ObjectCount == 1);
  assert(Fire(Config.Interrupt) && HandlerCalls == 1);
  CR_INTERRUPT_CONFIG Foreign = Config;
  Foreign.Param = NULL;
  UINTN Before = DisableCalls;
  assert(!CrInterruptIsRegistered(&Foreign));
  assert(CrUnregisterInterrupt(&Foreign) == CR_NOT_FOUND && DisableCalls == Before);
  CurrentIrql = DISPATCH_LEVEL;
  assert(CrUnregisterInterrupt(&Config) == CR_BUSY && CrInterruptIsRegistered(&Config));
  CurrentIrql = PASSIVE_LEVEL;
  SyncFailAt = 1;
  assert(CrUnregisterInterrupt(&Config) == CR_DEVICE_ERROR && CrInterruptIsRegistered(&Config));
  assert(Fire(Config.Interrupt) && HandlerCalls == 2);
  SyncFailAt = 2;
  assert(CrUnregisterInterrupt(&Config) == CR_DEVICE_ERROR && CrInterruptIsRegistered(&Config));
  assert(!Fire(Config.Interrupt) && HandlerCalls == 2);
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS && !CrInterruptIsRegistered(&Config));
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  WDFINTERRUPT Handle = Config.Interrupt;
  assert(Handle != NULL && CrRegisterInterrupt(&Config) == CR_SUCCESS);
  assert(Config.Interrupt == Handle && ObjectCount == 1 && Fire(Handle));
  assert(CrUnregisterInterrupt(&Config) == CR_SUCCESS);
  TeardownWaitsForDeferred(FALSE);
  TeardownWaitsForDeferred(TRUE);
  PortableIpcc();
  /* Simulate final device removal: framework owns deletion of these objects. */
  for (UINTN I = 0; I < ObjectCount; ++I) {
    OSK_WDF_INTERRUPT_CONTEXT *Context = OskInterruptGetIrqContext(Objects[I]);
    assert(Context->Config.Handler == NULL && Context->DeferredCallbacks.Active == 0);
    pthread_mutex_destroy(&Context->DeferredCallbacks.Mutex);
    pthread_cond_destroy(&Context->DeferredCallbacks.Condition);
    pthread_mutex_destroy(&Objects[I]->Lock);
    free(Objects[I]->Context); free(Objects[I]);
  }
  puts("OSKAL WDF shape: retained ownership, synchronous deferred rundown, object rebind and portable IPCC passed");
  return 0;
}
