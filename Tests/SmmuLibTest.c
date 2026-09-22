/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/smmu.h>

#define BASE 0x15000000ULL
#define CB(n) (0x80000U + (n) * 4096U)
#define SMR(n) (0x800U + (n) * 4U)
#define S2CR(n) (0xC00U + (n) * 4U)
#define TABLE_BYTES (SMMU_TABLE_PAGES * SMMU_PAGE_SIZE)

static UINT32 Registers[0x100000 / 4];
static unsigned Writes, Publishes, Delays;
static BOOLEAN Timeout, RejectS2cr;

static UINT32 Read32(VOID *Cookie, UINT64 Address) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers));
  if (Timeout && Address >= BASE + CB(0) && (Address & 0xFFF) == 0x7F4) {
    return 1;
  }
  return Registers[(Address - BASE) / 4];
}

static VOID Write32(VOID *Cookie, UINT64 Address, UINT32 Value) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers));
  Writes++;
  if (RejectS2cr && Address >= BASE + S2CR(0) && Address < BASE + S2CR(128)) {
    return;
  }
  Registers[(Address - BASE) / 4] = Value;
}

static VOID Write64(VOID *Cookie, UINT64 Address, UINT64 Value) {
  Write32(Cookie, Address, (UINT32)Value);
  Write32(Cookie, Address + 4, (UINT32)(Value >> 32));
}

static VOID Publish(VOID *Cookie, VOID *Address, UINTN Size) {
  (void)Cookie;
  assert(Address != NULL && Size != 0);
  Publishes++;
}

static VOID DelayUs(VOID *Cookie, UINT32 Delay) {
  (void)Cookie;
  Delays += Delay;
}

static SmmuIoOps Ops = {Read32, Write32, Write64, Publish, DelayUs, NULL};

static void Init(void) {
  memset(Registers, 0, sizeof(Registers));
  Registers[0x20 / 4] = 0x48000000 | 128; /* stage 1, stream matching */
  Registers[0x24 / 4] = 0x60020010;       /* 4K, 128 pages, 2 S2 banks, 16 CBs */
  Registers[0x28 / 4] = 0x1022;          /* 4K tables, 40-bit physical */
  Writes = Publishes = Delays = 0;
  Timeout = RejectS2cr = FALSE;
}

static void *Tables(void) {
  void *P = NULL;
  assert(posix_memalign(&P, SMMU_PAGE_SIZE, TABLE_BYTES) == 0);
  memset(P, 0xA5, TABLE_BYTES);
  return P;
}

int main(void) {
  SmmuDevice Device;
  SmmuDomain A = {0}, B = {0};
  UINT64 *Ta = Tables(), *Tb = Tables();
  UINT32 Before[sizeof(Registers) / sizeof(Registers[0])];
  unsigned N;

  Init();
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(Writes == 0 && Device.ContextBase == BASE + CB(0));
  assert(Device.BankCount == 16 && Device.Stage2BankCount == 2);
  assert(Device.AddressBits == 40);
  /* Existing display bank 2 and ASID 1 must not be reused. */
  Registers[SMR(0)/4] = 0x80000900;
  Registers[S2CR(0)/4] = 2;
  Registers[CB(2)/4] = 1;
  Registers[(CB(2)+0x24)/4] = 1U << 16;
  memcpy(Before, Registers, sizeof(Before));
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(A.Bank == 3 && A.Stream == 1 && A.Asid == 2);
  assert(memcmp(Before + CB(2)/4, Registers + CB(2)/4, 4096) == 0);
  assert(Registers[SMR(0)/4] == Before[SMR(0)/4]);
  assert(Registers[S2CR(0)/4] == Before[S2CR(0)/4]);
  assert(Registers[0] == Before[0]);
  assert(Ta[0] == 0x20001003 && Ta[512 + 128] == 0x20002003);
  assert(Ta[512 + 159] == 0x20021003);
  for (N = 1024; N < TABLE_BYTES / 8; N++) assert(Ta[N] == 0);
  assert(SmmuAttach(&Device, &B, 0x1C81, Tb, 0x20100000) == CR_SUCCESS);
  assert(B.Bank == 4 && B.Asid == 3 && B.Stream == 2);

  /* Detach first invalidates the stream, then restores every register which
   * SmmuAttach changed.  Reattach so the remaining mapping tests keep their
   * original two-domain setup. */
  {
    UINT16 Stream = B.Stream;
    UINT16 Bank = B.Bank;
    UINT32 OriginalSmr = B.OriginalSmr;
    UINT32 OriginalS2cr = B.OriginalS2cr;
    assert(SmmuDetach(&B) == CR_SUCCESS);
    assert(!B.Active);
    assert(Registers[SMR(Stream)/4] == OriginalSmr);
    assert(Registers[S2CR(Stream)/4] == OriginalS2cr);
    assert(Registers[CB(Bank)/4] == B.OriginalSctlr);
    assert(Registers[(CB(Bank)+0x10)/4] == B.OriginalTcr2);
    assert(Registers[(CB(Bank)+0x20)/4] == (UINT32)B.OriginalTtbr0);
    assert(Registers[(CB(Bank)+0x24)/4] == (UINT32)(B.OriginalTtbr0 >> 32));
    assert(Registers[(CB(Bank)+0x28)/4] == (UINT32)B.OriginalTtbr1);
    assert(Registers[(CB(Bank)+0x2C)/4] == (UINT32)(B.OriginalTtbr1 >> 32));
    assert(Registers[(CB(Bank)+0x30)/4] == B.OriginalTcr);
    assert(Registers[(CB(Bank)+0x38)/4] == B.OriginalMair0);
    assert(Registers[(CB(Bank)+0x3C)/4] == B.OriginalMair1);
    assert(SmmuAttach(&Device, &B, 0x1C81, Tb, 0x20100000) == CR_SUCCESS);
  }

  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + 4096, 0x12345000, 2,
                        SMMU_ACCESS_READ) == CR_SUCCESS);
  assert((Ta[1025] & 0x0000FFFFFFFFF000ULL) == 0x12345000);
  assert((Ta[1025] & 0xC3) == 0xC3); /* read-only, unprivileged, valid */
  assert(Ta[1026] == Ta[1025] + 4096 && Tb[1025] == 0);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + 4096, 0x56789000, 2, 3) == CR_SUCCESS);
  assert((Ta[1025] & 0x80) == 0);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + 4096, 0, 2, 0) == CR_SUCCESS);
  assert(Ta[1025] == 0 && Ta[1026] == 0);
  N = Writes;
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE - 4096, 0, 1, 1) == CR_INVALID_PARAMETER);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 1, 1, 1) == CR_INVALID_PARAMETER);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 1ULL << 40, 1, 1) == CR_INVALID_PARAMETER);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + SMMU_IOVA_SIZE - 4096, 0, 2, 1) == CR_INVALID_PARAMETER);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0, (UINTN)-1, 1) == CR_INVALID_PARAMETER);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0, 1, 4) == CR_INVALID_PARAMETER);
  assert(Writes == N);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + SMMU_IOVA_SIZE - 4096,
                        (1ULL << 40) - 4096, 1, 3) == CR_SUCCESS);
  assert(SmmuBlock(&A) == CR_SUCCESS);
  assert(Ta[TABLE_BYTES/8-1] == 0);

  Timeout = TRUE;
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0x12345000, 1, 1) == CR_TIMEOUT);
  assert(A.Failed && Delays == 10000 && Ta[1024] == 0);
  N = Writes;
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0, 1, 0) == CR_DEVICE_ERROR);
  assert(Writes == N);

  Init();
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  Registers[SMR(0)/4] = 0x807F1C00; /* wildcard covering 1c01 */
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_BUSY);
  assert(Writes == 0);
  Registers[SMR(0)/4] = 0x80001C01; /* exact firmware-owned entry */
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_BUSY);
  assert(Writes == 0);

  Init();
  Registers[0] = 1; /* cold, disabled, no existing clients */
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert((Registers[0] & 0x401) == 0x400 && Writes == 1);
  Init();
  Registers[0] = 1;
  Registers[CB(3)/4] = 1;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
  assert(Writes == 0);
  Init();
  Registers[0] = 1;
  Registers[SMR(0)/4] = 0x80000900;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
  assert(Writes == 0);
  Init();
  assert(SmmuProbe(&Device, BASE, 0x10000, &Ops) == CR_UNSUPPORTED);
  Init();
  Registers[0x20/4] &= ~0x40000000;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_UNSUPPORTED);
  assert(Writes == 0);

  Init();
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  /* A disabled bank with residual translation state remains firmware-owned. */
  Registers[(CB(2)+0x20)/4] = 0x20000000;
  Registers[(0x1000+2*4)/4] = 0x1F300;
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(A.Bank == 3);
  assert(Registers[(CB(2)+0x20)/4] == 0x20000000);

  Init();
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  RejectS2cr = TRUE;
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_DEVICE_ERROR);
  assert(A.Failed && !A.Active && Registers[SMR(0)/4] == 0);
  free(Ta);
  free(Tb);
  puts("SMMUv2: preservation, cold enable, isolation, permissions, boundaries, timeout and rejected-write checks passed");
  return 0;
}
