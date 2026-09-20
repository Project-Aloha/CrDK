/** @file
 *  Cross-platform spin lock wrapper.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

#ifdef _KERNEL_MODE
#include <ntddk.h>

typedef struct {
  KSPIN_LOCK Lock;
  KIRQL Irql;
} CR_LOCK;

STATIC inline VOID CrLockInit(IN OUT CR_LOCK *Lock) {
  KeInitializeSpinLock(&Lock->Lock);
  Lock->Irql = PASSIVE_LEVEL;
}

STATIC inline VOID CrLockAcquire(IN OUT CR_LOCK *Lock) {
  KIRQL OldIrql;

  KeAcquireSpinLock(&Lock->Lock, &OldIrql);
  Lock->Irql = OldIrql;
}

STATIC inline VOID CrLockRelease(IN OUT CR_LOCK *Lock) {
  KeReleaseSpinLock(&Lock->Lock, Lock->Irql);
}

#else
#include <Library/SynchronizationLib.h>

typedef SPIN_LOCK CR_LOCK;

STATIC inline VOID CrLockInit(IN OUT CR_LOCK *Lock) { *Lock = 0; }

STATIC inline VOID CrLockAcquire(IN OUT CR_LOCK *Lock) {
  AcquireSpinLock(Lock);
}

STATIC inline VOID CrLockRelease(IN OUT CR_LOCK *Lock) {
  ReleaseSpinLock(Lock);
}

#endif
