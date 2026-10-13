/* Qualcomm remote SMEM mutex, shared by native and NT adapters.
 * SPDX-License-Identifier: MIT */
#include <Library/smem.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_time.h>

STATIC CR_STATUS ValidateMutex(CONST SmemHardwareMutex *Mutex) {
  if (Mutex==NULL || Mutex->Address==0 || (Mutex->Address&3)!=0 ||
      Mutex->Size<4 || (Mutex->Size&3)!=0 ||
      Mutex->Size-1>(UINTN)-1-Mutex->Address || Mutex->Owner==0 ||
      Mutex->TimeoutUs==0 || Mutex->TimeoutUs>1000000 ||
      Mutex->PollUs==0 || Mutex->PollUs>Mutex->TimeoutUs || Mutex->Held>TRUE)
    return CR_INVALID_PARAMETER;
#ifdef _KERNEL_MODE
  // Sleeping contention must never become a one-second DISPATCH stall.
  if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return CR_BUSY;
#endif
  return CR_SUCCESS;
}

CR_STATUS SmemHardwareMutexAcquire(SmemHardwareMutex *Mutex) {
  CR_STATUS Status=ValidateMutex(Mutex);
  if (CR_ERROR(Status)) return Status;
  if (Mutex->Held) return CR_BUSY;
  UINT32 Remaining=Mutex->TimeoutUs;
#ifdef _KERNEL_MODE
  UINT64 Start=KeQueryInterruptTime();
#endif
  while (Remaining!=0) {
#ifdef _KERNEL_MODE
    // A scheduled sleep can take longer than requested. Charge that actual
    // elapsed time as well as the finite poll budget before another try.
    UINT64 Elapsed=(KeQueryInterruptTime()-Start)/10;
    if (Elapsed>=Mutex->TimeoutUs) break;
    if (Remaining>Mutex->TimeoutUs-Elapsed)
      Remaining=(UINT32)(Mutex->TimeoutUs-Elapsed);
#endif
    CrMmioWrite32(Mutex->Address,Mutex->Owner);
    CrMemoryFence();
    if (CrMmioRead32(Mutex->Address)==Mutex->Owner) {
      CrMemoryFence();
      Mutex->Held=TRUE;
      return CR_SUCCESS;
    }
    UINT32 Delay=Remaining<Mutex->PollUs ? Remaining : Mutex->PollUs;
    cr_sleep(Delay);
    Remaining-=Delay;
  }
  return CR_TIMEOUT;
}

CR_STATUS SmemHardwareMutexRelease(SmemHardwareMutex *Mutex) {
  CR_STATUS Status=ValidateMutex(Mutex);
  if (CR_ERROR(Status)) return Status;
  if (!Mutex->Held) return CR_SUCCESS;
  UINT32 Owner=CrMmioRead32(Mutex->Address);
  if (Owner!=0 && Owner!=Mutex->Owner) return CR_DEVICE_ERROR;
  CrMemoryFence();
  if (Owner==Mutex->Owner) {
    CrMmioWrite32(Mutex->Address,0);
    CrMemoryFence();
  }
  Mutex->Held=FALSE;
  return CR_SUCCESS;
}
