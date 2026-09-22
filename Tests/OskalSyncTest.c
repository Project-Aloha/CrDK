/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#undef NULL
#include <Uefi.h>
#include <oskal/cr_atomic.h>
#include <oskal/cr_lock.h>

/* These mocks use the EDK2 API ordering and lock states, independently of
 * the CrDK wrappers. Windows InterlockedCompareExchange orders them differently. */
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,
                                         UINT32 Compare, UINT32 Exchange) {
  UINT32 Old = *P;
  if (Old == Compare) *P = Exchange;
  return Old;
}

SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) {
  *Lock = 1;
  return Lock;
}

SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 1);
  *Lock = 2;
  return Lock;
}

SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 2);
  *Lock = 1;
  return Lock;
}

int main(void) {
  volatile UINT32 Bits = 0;
  CR_LOCK Lock;
  assert(CrAtomicOr32(&Bits, 0x24) == 0 && Bits == 0x24);
  assert(CrAtomicOr32(&Bits, 0x80) == 0x24 && Bits == 0xA4);
  assert(CrAtomicAnd32(&Bits, ~0x20U) == 0xA4 && Bits == 0x84);
  assert(CrAtomicLoad32(&Bits) == 0x84 && Bits == 0x84);
  assert(CrAtomicAnd32(&Bits, 0) == 0x84 && Bits == 0);
  CrLockInit(&Lock);
  CrLockAcquire(&Lock);
  CrLockRelease(&Lock);
  CrLockAcquire(&Lock);
  CrLockRelease(&Lock);
  puts("OSKAL: UEFI compare/exchange semantics and spin-lock lifecycle passed");
  return 0;
}
