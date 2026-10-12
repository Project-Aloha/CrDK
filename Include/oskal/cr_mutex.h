/** OS-independent serialization for operations that can wait.
 *  SPDX-License-Identifier: MIT
 */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#ifdef _KERNEL_MODE
/* Storage must be nonpaged. Unlike a spin/fast mutex, KMUTEX preserves
 * PASSIVE_LEVEL so callbacks may call passive WDF provider interfaces. */
typedef KMUTEX CR_MUTEX;
STATIC inline VOID CrMutexInit(IN OUT CR_MUTEX *Mutex) {
  KeInitializeMutex(Mutex, 0);
}
STATIC inline CR_STATUS CrMutexCheckLevel(VOID) {
  return KeGetCurrentIrql() == PASSIVE_LEVEL ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC inline CR_STATUS CrMutexAcquire(IN OUT CR_MUTEX *Mutex) {
  CR_STATUS Status = CrMutexCheckLevel();
  if (CR_ERROR(Status)) return Status;
  Status = KeWaitForSingleObject(Mutex, Executive, KernelMode, FALSE, NULL);
  return Status == STATUS_SUCCESS ? CR_SUCCESS : CR_ABORT;
}
STATIC inline VOID CrMutexRelease(IN OUT CR_MUTEX *Mutex) {
  (VOID)KeReleaseMutex(Mutex, FALSE);
}
#else
#include <Library/SynchronizationLib.h>
typedef SPIN_LOCK CR_MUTEX;
STATIC inline VOID CrMutexInit(IN OUT CR_MUTEX *Mutex) { InitializeSpinLock(Mutex); }
STATIC inline CR_STATUS CrMutexCheckLevel(VOID) { return CR_SUCCESS; }
STATIC inline CR_STATUS CrMutexAcquire(IN OUT CR_MUTEX *Mutex) {
  AcquireSpinLock(Mutex);
  return CR_SUCCESS;
}
STATIC inline VOID CrMutexRelease(IN OUT CR_MUTEX *Mutex) { ReleaseSpinLock(Mutex); }
#endif
