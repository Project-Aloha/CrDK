/* Actual remote-mutex core with an arbitrating register model. MIT */
#include <Library/smem.h>
#include <oskal/cr_memory.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UINT32 Owner,Reads,Writes,Unlocks,Delays;
static UINT64 TimeUs,DelayedUs;
static UINT32 ReleaseAfter,ExtraSleepUs;
static BOOLEAN ReacquireAfterUnlock;
static UINT8 Irql;
#define ADDRESS ((UINTN)0x1f43000)

static UINT32 Read(UINTN Address) {
  assert(Address==ADDRESS); ++Reads; return Owner;
}
static UINT32 Write(UINTN Address,UINT32 Value) {
  assert(Address==ADDRESS);
  if(Value==1) {
    ++Writes;
    if(ReleaseAfter && Writes==ReleaseAfter) Owner=0;
    if(Owner==0) Owner=1;
  } else {
    assert(Value==0 && Owner==1); ++Unlocks;
    Owner=ReacquireAfterUnlock ? 2 : 0;
  }
  return Value;
}
static VOID Delay(UINT64 Us) {++Delays;DelayedUs+=Us;TimeUs+=Us+ExtraSleepUs;}
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN Address) {return Read(Address);}
UINT32 CrMmioWrite32(UINTN Address,UINT32 Value) {return Write(Address,Value);}
KIRQL KeGetCurrentIrql(VOID) {return Irql;}
ULONGLONG KeQueryInterruptTime(VOID) {return TimeUs*10;}
VOID KeStallExecutionProcessor(ULONG Us) {Delay(Us);}
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE Mode,BOOLEAN Alert,LARGE_INTEGER *Interval) {
  assert(Mode==KernelMode && !Alert && Interval->QuadPart<0 && Irql==PASSIVE_LEVEL);
  Delay((UINT64)(-Interval->QuadPart)/10);return STATUS_SUCCESS;
}
#else
UINT32 EFIAPI MmioRead32(UINTN Address) {return Read(Address);}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {return Write(Address,Value);}
VOID EFIAPI MemoryFence(VOID) {__sync_synchronize();}
UINTN EFIAPI MicroSecondDelay(UINTN Us) {Delay(Us);return Us;}
#endif
static SmemHardwareMutex Reset(VOID) {
  Owner=Reads=Writes=Unlocks=Delays=ReleaseAfter=ExtraSleepUs=Irql=0;
  TimeUs=DelayedUs=0;ReacquireAfterUnlock=FALSE;
  return (SmemHardwareMutex){ADDRESS,4,1,1000000,1000,FALSE};
}
static VOID Bad(SmemHardwareMutex Mutex) {
  UINT32 Before=Reads+Writes+Unlocks;
  assert(SmemHardwareMutexAcquire(&Mutex)==CR_INVALID_PARAMETER);
  assert(SmemHardwareMutexRelease(&Mutex)==CR_INVALID_PARAMETER);
  assert(Reads+Writes+Unlocks==Before && Delays==0);
}
int main(void) {
  SmemHardwareMutex M=Reset(),Invalid;
  assert(SmemHardwareMutexAcquire(NULL)==CR_INVALID_PARAMETER);
  assert(SmemHardwareMutexRelease(NULL)==CR_INVALID_PARAMETER);
  Invalid=M;Invalid.Address=0;Bad(Invalid);
  Invalid=M;Invalid.Address++;Bad(Invalid);
  Invalid=M;Invalid.Size=3;Bad(Invalid);
  Invalid=M;Invalid.Size=5;Bad(Invalid);
  Invalid=M;Invalid.Address=(UINTN)-4;Invalid.Size=8;Bad(Invalid);
  Invalid=M;Invalid.Owner=0;Bad(Invalid);
  Invalid=M;Invalid.TimeoutUs=0;Bad(Invalid);
  Invalid=M;Invalid.TimeoutUs=1000001;Bad(Invalid);
  Invalid=M;Invalid.PollUs=0;Bad(Invalid);
  Invalid=M;Invalid.PollUs=M.TimeoutUs+1;Bad(Invalid);
  Invalid=M;Invalid.Held=2;Bad(Invalid);
  assert(SmemHardwareMutexRelease(&M)==CR_SUCCESS && Reads==0);
  assert(SmemHardwareMutexAcquire(&M)==CR_SUCCESS && M.Held && Owner==1);
  UINT32 Before=Reads+Writes;
  assert(SmemHardwareMutexAcquire(&M)==CR_BUSY && Reads+Writes==Before);
  assert(SmemHardwareMutexRelease(&M)==CR_SUCCESS && !M.Held && Owner==0 && Unlocks==1);
  M=Reset();Owner=2;
  assert(SmemHardwareMutexAcquire(&M)==CR_TIMEOUT && !M.Held && Owner==2);
  assert(Writes==1000 && Reads==1000 && Delays==1000 && DelayedUs==1000000 && !Unlocks);
  M=Reset();Owner=2;M.TimeoutUs=21;M.PollUs=10;
  assert(SmemHardwareMutexAcquire(&M)==CR_TIMEOUT && Writes==3 && DelayedUs==21);
  M=Reset();Owner=2;ReleaseAfter=3;
  assert(SmemHardwareMutexAcquire(&M)==CR_SUCCESS && Writes==3 && Delays==2 && M.Held);
  Owner=2;Before=Writes+Unlocks;
  assert(SmemHardwareMutexRelease(&M)==CR_DEVICE_ERROR && M.Held && Owner==2);
  assert(SmemHardwareMutexAcquire(&M)==CR_BUSY && Writes+Unlocks==Before);
  assert(SmemHardwareMutexRelease(&M)==CR_DEVICE_ERROR && Writes+Unlocks==Before);
  Owner=0;
  assert(SmemHardwareMutexRelease(&M)==CR_SUCCESS && !M.Held && Writes+Unlocks==Before);
  assert(SmemHardwareMutexAcquire(&M)==CR_SUCCESS);ReacquireAfterUnlock=TRUE;
  assert(SmemHardwareMutexRelease(&M)==CR_SUCCESS && !M.Held && Owner==2);
#ifdef _KERNEL_MODE
  M=Reset();Irql=DISPATCH_LEVEL;
  assert(SmemHardwareMutexAcquire(&M)==CR_BUSY && Reads+Writes+Delays==0);
  M.Held=TRUE;
  assert(SmemHardwareMutexRelease(&M)==CR_BUSY && M.Held && Reads+Writes+Delays==0);
  M=Reset();Owner=2;ExtraSleepUs=1000000;
  assert(SmemHardwareMutexAcquire(&M)==CR_TIMEOUT && Writes==1 && Delays==1 && Owner==2);
#endif
  puts("SMEM mutex: bounds, timeout, foreign ownership, retained release, remote reacquire and IRQL passed");
  return 0;
}
