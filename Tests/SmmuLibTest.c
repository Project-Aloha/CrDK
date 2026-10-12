/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/smmu.h>
#include <Library/DebugLib.h>

static unsigned LogCount, ErrorLogCount, FaultLogCount, TimeoutLogCount, ProbeLogCount, DetachLogCount;
static UINT32 CapturedActlr, CapturedTf, CapturedPf, CapturedLevel, CapturedWrite, CapturedPtw;
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  /* Firmware emits through DebugLib. Capture format identity here instead of
   * feeding EDK2's trailing %a source filename to libc printf. */
  assert((Level == DEBUG_ERROR || Level == DEBUG_WARN) && Format != NULL);
  LogCount++;
  if (Level == DEBUG_ERROR) ErrorLogCount++;
  if (strstr(Format, "SMMU snapshot")) FaultLogCount++;
  if (strstr(Format, "SMMU snapshot ACTLR=")) {
    va_list Args; va_start(Args,Format); CapturedActlr=va_arg(Args,UINT32); va_end(Args);
  }
  if (strstr(Format, "SMMU snapshot fault TF=")) {
    va_list Args; va_start(Args,Format);
    CapturedTf=va_arg(Args,UINT32); CapturedPf=va_arg(Args,UINT32);
    CapturedLevel=va_arg(Args,UINT32); CapturedWrite=va_arg(Args,UINT32); CapturedPtw=va_arg(Args,UINT32);
    va_end(Args);
  }
  if (strstr(Format, "SMMU sync timeout")) TimeoutLogCount++;
  if (strstr(Format, "SMMU probe registers")) ProbeLogCount++;
  if (strstr(Format, "SMMU owned-domain detach complete")) DetachLogCount++;
}


#define BASE 0x15000000ULL
#define CB(n) (0x80000U + (n) * 4096U)
#define SMR(n) (0x800U + (n) * 4U)
#define S2CR(n) (0xC00U + (n) * 4U)
#define TABLE_BYTES (SMMU_TABLE_PAGES * SMMU_PAGE_SIZE)

static UINT32 Registers[0x100000 / 4];
static unsigned Reads, Writes, Publishes, Delays;
static BOOLEAN Timeout, RejectS2cr, QualcommFaultQuirk, ModelAsidWidth;
static BOOLEAN DropSctlrAsidPne;
static BOOLEAN Tcr2ReservedBits;
static UINT64 RejectAddress;
static unsigned SyncWrites;
static BOOLEAN EnforceShortWidth;
static unsigned ActlrWrites;

static UINT32 Read32(VOID *Cookie, UINT64 Address) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers));
  Reads++;
  if (Timeout && Address >= BASE + CB(0) && (Address & 0xFFF) == 0x7F4) {
    return 1;
  }
  return Registers[(Address - BASE) / 4];
}

static VOID Write32(VOID *Cookie, UINT64 Address, UINT32 Value) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers));
  Writes++;
  if (Address >= BASE + CB(0) && (Address & 0xFFF) == 4) ActlrWrites++;
  if (Address >= BASE + CB(0) && (Address & 0xFFF) == 0x7F0) SyncWrites++;
  if (Address == RejectAddress) return;
  if (QualcommFaultQuirk && Address >= BASE + S2CR(0) && Address < BASE + S2CR(128)) {
    if ((Value & 0x30000) == 0x20000) return;
    if ((Value & 0x30000) == 0x10000) Value = (Value & ~0x30000U) | 0x20000;
  }
  /* ASID[15:8] is not writable when TCR2.AS is clear. */
  if (ModelAsidWidth && Address >= BASE + CB(0) &&
      ((Address & 0xFFF) == 0x24 || (Address & 0xFFF) == 0x2C) &&
      !(Registers[((Address - BASE) & ~0xFFFULL) / 4 + 0x10 / 4] & 0x10))
    Value &= 0x00FFFFFFU;
  /* SM8450 exposes implementation bits 6:5 in TCR2. They survive writes and
   * are not part of the Linux-defined PASIZE/AS/SEP configuration fields. */
  if (Tcr2ReservedBits && Address >= BASE + CB(0) &&
      (Address & 0xFFF) == 0x10)
    Value |= 0x60;
  if (RejectS2cr && Address >= BASE + S2CR(0) && Address < BASE + S2CR(128)) {
    return;
  }
  if (DropSctlrAsidPne && Address >= BASE + CB(0) &&
      (Address & 0xFFF) == 0) {
    Value &= ~0x1000U;
  }
  Registers[(Address - BASE) / 4] = Value;
}

static VOID Write64(VOID *Cookie, UINT64 Address, UINT64 Value) {
  if (EnforceShortWidth && Address >= BASE + CB(0)) {
    UINT32 Bank = (UINT32)(Address - BASE - CB(0)) / 4096;
    BOOLEAN Short = (Registers[(0x1000+Bank*4)/4] & 0x30000) != 0 &&
      !(Registers[(0x1800+Bank*4)/4] & 1) &&
      !(Registers[(CB(Bank)+0x30)/4] & 0x80000000U);
    assert(!Short); /* Linux accesses short-descriptor TTBRs as 32-bit. */
  }
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

static void Init(SmmuDevice *Device, SmmuDomain *A, SmmuDomain *B) {
  /* Resetting the mock hardware denotes a fresh controller lifetime. */
  memset(Device, 0, sizeof(*Device));
  memset(A, 0, sizeof(*A));
  memset(B, 0, sizeof(*B));
  memset(Registers, 0, sizeof(Registers));
  Registers[0x20 / 4] = 0x48000000 | 128; /* stage 1, stream matching */
  Registers[0x24 / 4] = 0x60020010;       /* 4K, 128 pages, 2 S2 banks, 16 CBs */
  Registers[0x28 / 4] = 0x1022;          /* 4K tables, 40-bit physical */
  Reads = Writes = Publishes = Delays = 0;
  Timeout = RejectS2cr = QualcommFaultQuirk = ModelAsidWidth = FALSE;
  Tcr2ReservedBits = FALSE;
  RejectAddress = 0;
  SyncWrites = ActlrWrites = 0;
  EnforceShortWidth = FALSE;
  DropSctlrAsidPne = FALSE;
}

static void *Tables(void) {
  void *P = NULL;
  assert(posix_memalign(&P, SMMU_PAGE_SIZE, TABLE_BYTES) == 0);
  memset(P, 0xA5, TABLE_BYTES);
  return P;
}

/* Walk according to the programmed architectural T0SZ, rather than assuming
 * the implementation's hierarchy. This detects a missing L0 level when the
 * hardware supports a wider input aperture than these firmware tables. */
static UINT64 Translate(SmmuDomain *Domain, UINT64 Iova) {
  unsigned Bits = 64U - (Registers[(CB(Domain->Bank)+0x30)/4] & 0x3FU);
  unsigned Levels = (Bits - 12U + 8U) / 9U;
  UINT64 Table = Domain->TableAddress;
  assert(Iova < (1ULL << Bits));
  for (; Levels != 0; --Levels) {
    unsigned Shift = 12U + 9U * (Levels - 1U);
    UINT64 Offset = Table - Domain->TableAddress;
    UINT64 Entry;
    assert(Table >= Domain->TableAddress && Offset <= TABLE_BYTES - 4096);
    Entry = Domain->Tables[Offset / 8 + ((Iova >> Shift) & 0x1FFU)];
    if ((Entry & 3U) != 3U) return MAX_UINT64;
    Table = Entry & 0x0000FFFFFFFFF000ULL;
  }
  return Table | (Iova & 0xFFFU);
}

int main(void) {
  SmmuDevice Device = {0};
  SmmuDomain A = {0}, B = {0};
  UINT64 *Ta = Tables(), *Tb = Tables();
  UINT32 Before[sizeof(Registers) / sizeof(Registers[0])];
  unsigned N;

  /* A translating route above our software capacity must not disappear
   * from the ownership scan, even if the controller appears cold. */
  Init(&Device, &A, &B);
  Registers[0x20 / 4] = 0x48000000 | 129;
  Registers[0] = 1;
  Registers[SMR(128) / 4] = 0x80000100;
  Registers[S2CR(128) / 4] = 2;
  memcpy(Before, Registers, sizeof(Before));
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_UNSUPPORTED);
  assert(Writes == 0 && memcmp(Before, Registers, sizeof(Before)) == 0);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);

  unsigned ErrorsBeforeSuccess = ErrorLogCount;
  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(Writes == 0 && Device.ContextBase == BASE + CB(0));
  assert(Device.BankCount == 16 && Device.Stage2BankCount == 2);
  assert(Device.AddressBits == 40);
  /* Existing display bank 2 and ASID 1 must not be reused. */
  Registers[SMR(0)/4] = 0x80000900;
  Registers[S2CR(0)/4] = 2;
  Registers[CB(2)/4] = 1;
  Registers[(0x1000+2*4)/4] = 0x1F300;
  Registers[(0x1800+2*4)/4] = 1;
  Registers[(CB(2)+0x24)/4] = 1U << 16;
  memcpy(Before, Registers, sizeof(Before));
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(A.Bank == 3 && A.Stream == 1 && A.Asid == 2);
  assert(Registers[S2CR(A.Stream)/4] == A.Bank); /* Linux S2CR_PRIVCFG_DEFAULT */
  assert(memcmp(Before + CB(2)/4, Registers + CB(2)/4, 4096) == 0);
  assert(Registers[SMR(0)/4] == Before[SMR(0)/4]);
  assert(Registers[S2CR(0)/4] == Before[S2CR(0)/4]);
  assert(Registers[0] == Before[0]);
  assert(Ta[0] == 0x20001003 && Ta[512 + 128] == 0x20002003);
  assert(Ta[512 + 159] == 0x20021003);
  for (N = 1024; N < TABLE_BYTES / 8; N++) assert(Ta[N] == 0);
  assert(SmmuAttach(&Device, &B, 0x1C81, Tb, 0x20100000) == CR_SUCCESS);
  assert(B.Bank == 4 && B.Asid == 3 && B.Stream == 2);
  assert(Registers[S2CR(B.Stream)/4] == B.Bank);
  assert(ErrorLogCount == ErrorsBeforeSuccess);

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
  /* Linux arm_lpae_prot_to_pte() marks stage-1 DMA entries nG.  This keeps
   * the mapping scoped to ASID 1 instead of allowing a stale global TLB
   * entry to alias a later context bank. */
  assert((Ta[1025] & (1ULL << 11)) != 0);
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
  assert(Device.SyncFailed);
  assert(SmmuBlock(&A) == CR_DEVICE_ERROR);
  assert(SmmuBlock(&B) == CR_DEVICE_ERROR);
  assert(SmmuDetach(&B) == CR_DEVICE_ERROR);
  assert(SmmuSetMapping(&B, SMMU_IOVA_BASE, 0, 1, 0) == CR_DEVICE_ERROR);
  assert(SmmuAttach(&Device, &B, 0x1234, Tb, 0x20100000) == CR_DEVICE_ERROR);
  assert(Writes == N); /* even another domain cannot submit more syncs */
  {
    unsigned OldReads = Reads;
    SmmuDevice SavedDevice = Device;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
    assert(Reads == OldReads && Writes == N);
    assert(memcmp(&Device, &SavedDevice, sizeof(Device)) == 0);
  }

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  Registers[SMR(0)/4] = 0x807F1C00; /* wildcard covering 1c01 */
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_BUSY);
  assert(Writes == 0);
  Registers[SMR(0)/4] = 0x80001C01; /* exact firmware-owned entry */
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_BUSY);
  assert(Writes == 0);

  Init(&Device, &A, &B);
  Registers[0] = 1; /* cold, disabled, no existing clients */
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(Registers[0] == 0x1C36 && Writes == 3);
  Init(&Device, &A, &B);
  Registers[0] = 1;
  Registers[CB(3)/4] = 1;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
  assert(Writes == 0);
  Init(&Device, &A, &B);
  Registers[0] = 1;
  Registers[SMR(0)/4] = 0x80000900;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
  assert(Writes == 0);

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, 0x10000, &Ops) == CR_UNSUPPORTED);
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_INVALID_PARAMETER);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
  assert(Writes == 0);
  Init(&Device, &A, &B);
  /* This implementation builds 4 KiB LPAE tables; a controller whose
   * context-bank register stride is 64 KiB must be rejected explicitly. */
  Registers[0x24 / 4] |= 0x80000000U;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_UNSUPPORTED);
  Init(&Device, &A, &B);
  Registers[0x20/4] &= ~0x40000000;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_UNSUPPORTED);
  assert(Writes == 0);

  /* Linux's stage-1 config keeps ID2.IAS for TCR2.PASIZE and ID2.OAS for
   * physical/table-walker bounds. Exercise the asymmetric case explicitly. */
  Init(&Device, &A, &B);
  Registers[0x28 / 4] = 0x1021; /* IAS=36 bits, OAS=40 bits, 4K tables */
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(Device.InputAddressBits == 32 && Device.InputAddressEncoding == 0);
  assert(Device.Stage1OutputAddressBits == 36 &&
         Device.Stage1OutputAddressEncoding == 1);
  assert(Device.AddressBits == 40 && Device.AddressEncoding == 2);
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(Registers[(CB(A.Bank) + 0x10) / 4] == 0x38011);
  /* Stage-1 output/IPA uses IAS (36 bits here); the table walker still uses
   * OAS (40 bits), so a 40-bit table address is valid while a 36-bit DMA
   * output is the highest address accepted by SmmuSetMapping(). */
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, (1ULL << 36) - 4096, 1,
                        SMMU_ACCESS_READ) == CR_SUCCESS);
  assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + 4096, 1ULL << 36, 1,
                        SMMU_ACCESS_READ) == CR_INVALID_PARAMETER);
  assert(SmmuDetach(&A) == CR_SUCCESS);

  /* SM8450 drops SCTLR.S1_ASIDPNE (0x1000) on readback. The target-specific
   * mask accepts only that documented bit; architectural enable and fault
   * controls remain exact requirements. */
  Init(&Device, &A, &B);
  DropSctlrAsidPne = TRUE;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  Device.SctlrReadbackIgnoreMask = 0x1000;
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert((Registers[CB(A.Bank) / 4] & 0x1027) == 0x27);
  assert(SmmuDetach(&A) == CR_SUCCESS);
  Init(&Device, &A, &B);
  DropSctlrAsidPne = TRUE;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_DEVICE_ERROR);
  assert(A.Failed && Device.ReservedBanks[A.Bank]);

  /* A target cannot waive architectural SCTLR bits through the quirk field. */
  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  Device.SctlrReadbackIgnoreMask = 0x2000;
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_INVALID_PARAMETER);

  /* Every supported UBS encoding must use a hierarchy that translates the
   * first and final firmware IOVA pages, including a high physical address. */
  for (N = 0; N < 6; ++N) {
    Init(&Device, &A, &B);
    Registers[0x28 / 4] = 0x1055U | (N << 8); /* IAS/OAS=48, UBS varies. */
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
    assert(Device.TranslationAddressBits <= 39);
    assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0x123456000ULL, 1, 3) == CR_SUCCESS);
    assert(Translate(&A, SMMU_IOVA_BASE + 13) == 0x12345600DULL);
    assert(SmmuSetMapping(&A, SMMU_IOVA_BASE + SMMU_IOVA_SIZE - 4096,
                          0xABCDEF000ULL, 1, 1) == CR_SUCCESS);
    assert(Translate(&A, SMMU_IOVA_BASE + SMMU_IOVA_SIZE - 1) == 0xABCDEFFFFULL);
    assert(SmmuSetMapping(&A, SMMU_IOVA_BASE, 0, 1, 0) == CR_SUCCESS);
    assert(Translate(&A, SMMU_IOVA_BASE) == MAX_UINT64);
    {
      unsigned OldReads = Reads, OldWrites = Writes;
      SmmuDevice SavedDevice = Device;
      SmmuDomain SavedDomain = A;
      assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_BUSY);
      assert(SmmuAttach(&Device, &A, 0x1C81, Tb, 0x20100000) == CR_BUSY);
      assert(Reads == OldReads && Writes == OldWrites);
      assert(memcmp(&Device, &SavedDevice, sizeof(Device)) == 0);
      assert(memcmp(&A, &SavedDomain, sizeof(A)) == 0);
    }
    assert(SmmuDetach(&A) == CR_SUCCESS);
    assert(!Device.ReservedAsids[A.Asid]);
  }

  /* Linux frees disabled banks without clearing TTBR/TCR/MAIR/CBAR. Model
   * the observed SM8450 geometry with stale configuration in every bank. */
  Init(&Device, &A, &B);
  Registers[0x20/4] = 0x4C017E71;
  Registers[0x24/4] = 0x6000004B;
  Registers[0x28/4] = 0x00005111;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(Device.BankCount == 75 && Device.StreamCount == 113);
  Tcr2ReservedBits = TRUE;
  for (N = 0; N < Device.BankCount; N++) {
    Registers[CB(N)/4] = 0x1026; /* disabled; non-M control fields retained */
    Registers[(CB(N)+0x10)/4] = 0x38061;
    Registers[(CB(N)+0x20)/4] = 0x30000000 + N * 4096;
    Registers[(CB(N)+0x24)/4] = 0x40000;
    Registers[(CB(N)+0x28)/4] = 0x31000000 + N * 4096;
    Registers[(CB(N)+0x2C)/4] = 0x50000;
    Registers[(CB(N)+0x30)/4] = 0x803520;
    Registers[(CB(N)+0x38)/4] = 0xFF;
    Registers[(CB(N)+0x3C)/4] = 0x44;
    Registers[(0x1000+N*4)/4] = 0x1F300;
    Registers[(0x1800+N*4)/4] = 1;
  }
  /* A disabled but stream-bound bank and an enabled, unbound bank are both
   * owned. Their ASIDs (including TTBR1) must also remain reserved. */
  Registers[SMR(0)/4] = 0x80000900;
  Registers[S2CR(0)/4] = 0;
  Registers[(CB(0)+0x24)/4] = 1U << 16;
  Registers[(CB(0)+0x2C)/4] = 2U << 16;
  Registers[CB(1)/4] |= 1;
  Registers[(CB(1)+0x24)/4] = 3U << 16;
  memcpy(Before, Registers, sizeof(Before));
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(A.Bank == 2 && A.Stream == 1 && A.Asid == 4);
  assert(Registers[(CB(A.Bank)+0x10)/4] == 0x38071);
  for (N = 0; N < Device.BankCount; N++) {
    if (N == A.Bank) continue;
    assert(memcmp(Before + CB(N)/4, Registers + CB(N)/4, 4096) == 0);
    assert(Registers[(0x1000+N*4)/4] == Before[(0x1000+N*4)/4]);
    assert(Registers[(0x1800+N*4)/4] == Before[(0x1800+N*4)/4]);
  }
  assert(Registers[SMR(0)/4] == Before[SMR(0)/4]);
  assert(Registers[S2CR(0)/4] == Before[S2CR(0)/4]);
  assert(SmmuDetach(&A) == CR_SUCCESS);
  /* TLB command registers are write-only operations, not saved configuration. */
  Before[(CB(A.Bank)+0x610)/4] = Registers[(CB(A.Bank)+0x610)/4];
  Before[(CB(A.Bank)+0x7F0)/4] = Registers[(CB(A.Bank)+0x7F0)/4];
  assert(memcmp(Before, Registers, sizeof(Before)) == 0);

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  /* Disabled banks are still unavailable if valid translations reference
   * them; the unreferenced S2 banks and final bypass bank remain reserved. */
  for (N = Device.Stage2BankCount; N + 1U < Device.BankCount; N++) {
    Registers[SMR(N)/4] = 0x80000100U + N;
    Registers[S2CR(N)/4] = N;
  }
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_OUT_OF_RESOURCES);
  assert(Device.LastAttachResource == SmmuAttachResourceBank && Writes == 0);

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  for (N = 0; N < Device.StreamCount; N++) {
    Registers[SMR(N)/4] = 0x80000100U + N;
    Registers[S2CR(N)/4] = 0x30000U;
  }
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_OUT_OF_RESOURCES);
  assert(Device.LastAttachResource == SmmuAttachResourceStream && Writes == 0);

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  for (N = Device.Stage2BankCount; N + 1U < Device.BankCount; N++) {
    Registers[CB(N)/4] = 1;
  }
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_OUT_OF_RESOURCES);
  assert(Device.LastAttachResource == SmmuAttachResourceBank && Writes == 0);

  Init(&Device, &A, &B);
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  RejectS2cr = TRUE;
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_DEVICE_ERROR);
  assert(A.Failed && !A.Active && Registers[SMR(0)/4] == 0);

  /* Restore a 16-bit ASID with register semantics, not a RAM-only mock. */
  Init(&Device, &A, &B);
  ModelAsidWidth = TRUE;
  Registers[(0x1000+2*4)/4] = 0x1F300;
  Registers[(0x1800+2*4)/4] = 1;
  Registers[(CB(2)+0x10)/4] = 0x38012;
  Registers[(CB(2)+0x24)/4] = 0xABCD0000;
  Registers[(CB(2)+0x2C)/4] = 0x98760000;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(SmmuDetach(&A) == CR_SUCCESS);
  assert(Registers[(CB(2)+0x24)/4] == 0xABCD0000);
  assert(Registers[(CB(2)+0x2C)/4] == 0x98760000);

  /* Restore FAULT both with normal and Qualcomm-protected S2CR semantics. */
  for (N = 0; N < 2; N++) {
    Init(&Device, &A, &B);
    QualcommFaultQuirk = N != 0;
    Registers[S2CR(0)/4] = 0x200FF;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
    assert(SmmuDetach(&A) == CR_SUCCESS);
    assert(Registers[S2CR(0)/4] == 0x200FF);
    assert(!Device.ReservedBanks[A.Bank] && !Device.ReservedStreams[A.Stream]);
  }

  /* Every material bank configuration write must be accepted before attach. */
  {
    static const UINT32 Offsets[] = {0x1808, 0x1008, CB(2)+0x10,
      CB(2)+0x30, CB(2)+0x20, CB(2)+0x24, CB(2)+0x2C,
      CB(2)+0x38, CB(2)+0x3C, CB(2)};
    for (N = 0; N < sizeof(Offsets)/sizeof(Offsets[0]); N++) {
      Init(&Device, &A, &B);
      Registers[Offsets[N]/4] = 0x40; /* differs even for expected zero */
      if (Offsets[N] == CB(2)) Registers[Offsets[N]/4] = 0;
      assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
      RejectAddress = BASE + Offsets[N];
      assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_DEVICE_ERROR);
      assert(A.Failed && !A.Active && Registers[SMR(0)/4] == 0);
      assert(Device.ReservedBanks[2] && Device.ReservedStreams[0]);
      assert(Device.ReservedAsids[A.Asid]);
      RejectAddress = 0;
      assert(SmmuAttach(&Device, &B, 0x1C81, Tb, 0x20100000) == CR_SUCCESS);
      assert(B.Bank == 3 && B.Stream == 1);
      assert(B.Asid != A.Asid); /* failed, disabled banks still reserve ASIDs */
    }
  }

  /* Rejected detach writes retain ownership and stop before unsafe reuse. */
  for (N = 0; N < 4; N++) {
    unsigned OldSync;
    Init(&Device, &A, &B);
    Registers[(CB(2)+0x38)/4] = 0xFF;
    Registers[S2CR(0)/4] = 0x200FF;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
    RejectAddress = BASE + (N == 0 ? SMR(0) : N == 1 ? CB(2) :
                             N == 2 ? CB(2)+0x38 : S2CR(0));
    OldSync = SyncWrites;
    assert(SmmuDetach(&A) == CR_DEVICE_ERROR);
    assert(A.Active && A.Failed && Device.ReservedBanks[2] && Device.ReservedStreams[0]);
    if (N == 0) assert(SyncWrites == OldSync && Registers[CB(2)/4] == 0x1027);
    if (N == 1) assert(Registers[(CB(2)+0x38)/4] == 0x44);
    OldSync = SyncWrites;
    assert(SmmuBlock(&A) == CR_DEVICE_ERROR);
    assert(SyncWrites == OldSync);
  }


  /* Short-descriptor firmware ASIDs live in CONTEXTIDR, for both enabled
   * banks and disabled banks still referenced by a firmware stream. */
  Init(&Device, &A, &B);
  Registers[(0x1000+2*4)/4] = 0x1F300;
  Registers[CB(2)/4] = 1;
  Registers[(CB(2)+0x34)/4] = 0xABCD0001;
  Registers[(0x1000+3*4)/4] = 0x1F300;
  Registers[(CB(3)+0x34)/4] = 0xABCD0002;
  Registers[SMR(0)/4] = 0x80000900;
  Registers[S2CR(0)/4] = 3;
  /* The acquired disabled bank is also short-format and has nonzero TTBRs. */
  Registers[(0x1000+4*4)/4] = 0x1F300;
  Registers[(CB(4)+0x20)/4] = 0x12344000;
  Registers[(CB(4)+0x28)/4] = 0x23444000;
  Registers[(CB(4)+0x34)/4] = 0xABCD0055;
  EnforceShortWidth = TRUE;
  assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
  assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
  assert(A.Bank == 4 && A.Asid == 3);
  assert(SmmuDetach(&A) == CR_SUCCESS);
  assert(Registers[(CB(4)+0x20)/4] == 0x12344000);
  assert(Registers[(CB(4)+0x28)/4] == 0x23444000);
  assert(Registers[(CB(4)+0x34)/4] == 0xABCD0055);
  assert(Registers[(CB(2)+0x34)/4] == 0xABCD0001);
  assert(Registers[(CB(3)+0x34)/4] == 0xABCD0002);

  {
    static const SmmuActlrSetting Settings[] = {{0x900, 0x402, 1}, {0x902, 0, 1}};
    static const SmmuActlrSetting Conflict[] = {{0x900, 0x402, 1}, {0x902, 0, 2}};
    Init(&Device, &A, &B);
    Registers[(CB(2)+4)/4] = 0x80;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    Device.ActlrSettings = Settings;
    Device.ActlrSettingCount = 2;
    assert(SmmuAttach(&Device, &A, 0x902, Ta, 0x20000000) == CR_SUCCESS);
    assert(A.OwnsActlr && Registers[(CB(2)+4)/4] == 1 && ActlrWrites == 1);
    assert(SmmuDetach(&A) == CR_SUCCESS);
    assert(Registers[(CB(2)+4)/4] == 0x80 && ActlrWrites == 2);
    /* Unmatched streams never touch ACTLR, even at detach. */
    assert(SmmuAttach(&Device, &A, 0x1C01, Ta, 0x20000000) == CR_SUCCESS);
    assert(!A.OwnsActlr && ActlrWrites == 2);
    assert(SmmuDetach(&A) == CR_SUCCESS && ActlrWrites == 2);
    Device.ActlrSettings = Conflict;
    N = Writes;
    assert(SmmuAttach(&Device, &A, 0x902, Ta, 0x20000000) == CR_INVALID_PARAMETER);
    assert(Writes == N);
    Device.ActlrSettings = Settings;
    RejectAddress = BASE + CB(2) + 4;
    assert(SmmuAttach(&Device, &A, 0x902, Ta, 0x20000000) == CR_DEVICE_ERROR);
    assert(A.Failed && !A.Active && Device.ReservedBanks[2]);
    assert(Registers[CB(2)/4] == 0 && Registers[SMR(0)/4] == 0);

    Init(&Device, &A, &B);
    Registers[(CB(2)+4)/4] = 0x80;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    Device.ActlrSettings = Settings;
    Device.ActlrSettingCount = 2;
    assert(SmmuAttach(&Device, &A, 0x902, Ta, 0x20000000) == CR_SUCCESS);
    RejectAddress = BASE + CB(2) + 4;
    assert(SmmuDetach(&A) == CR_DEVICE_ERROR);
    assert(A.Active && A.Failed && Device.ReservedBanks[2]);
  }

  /* A handoff witness must describe real page-table and register state,
     rather than merely echoing the request or the software ownership flag. */
  {
    SmmuMappingEvidence E, Empty = {0};
    SmmuDomain Before;
    static const SmmuActlrSetting Settings[] = {{0x2800, 0x402, 1}};
    static const SmmuActlrSetting Conflict[] = {{0x2800, 0x402, 1}, {0x2800, 0, 2}};
    UINT64 Iova = SMMU_IOVA_BASE + 511ULL * SMMU_PAGE_SIZE;
    UINT64 Physical = 0x36000000;
    UINTN Pages = 520, First = 1024 + 511;
    UINT32 Saved;
    UINT64 SavedPte;
    unsigned OldWrites, OldPublishes;
    assert(sizeof(E) == 128);
    Init(&Device, &A, &B);
    Tcr2ReservedBits = TRUE; DropSctlrAsidPne = TRUE;
    assert(SmmuProbe(&Device, BASE, sizeof(Registers), &Ops) == CR_SUCCESS);
    Device.SctlrReadbackIgnoreMask = 0x1000;
    Device.ActlrSettings = Settings;
    Device.ActlrSettingCount = 1;
    assert(SmmuAttach(&Device, &A, 0x2800, Ta, 0x20000000) == CR_SUCCESS);
    assert(SmmuSetMapping(&A, Iova, Physical, Pages, SMMU_ACCESS_READ) == CR_SUCCESS);
    Before = A; OldWrites = Writes; OldPublishes = Publishes;
    assert(SmmuQueryMappingEvidence(&A, Iova, Physical, Pages, SMMU_ACCESS_READ, &E) == CR_SUCCESS);
    assert(E.Base == BASE && E.Tables == 0x20000000 && E.TableBytes == TABLE_BYTES);
    assert(E.Iova == Iova && E.Physical == Physical && E.Bytes == Pages * 4096);
    assert(E.Sid == 0x2800 && E.Bank == A.Bank && E.Stream == A.Stream && E.Asid == A.Asid);
    assert(E.Sctlr == 0x27 && (E.Tcr2 & 0x60) == 0x60 && E.Access == SMMU_ACCESS_READ);
    assert(E.Ttbr0 == (0x20000000ULL | ((UINT64)A.Asid << 48)));
    assert(SmmuQueryMappingEvidence(&A, Iova, Physical, Pages, 3, &E) == CR_DEVICE_ERROR);
    assert(!memcmp(&E, &Empty, sizeof(E)));
    {
      UINT32 Offsets[] = {SMR(A.Stream), S2CR(A.Stream), CB(A.Bank), CB(A.Bank)+4,
        0x1000U+A.Bank*4U,0x1800U+A.Bank*4U,CB(A.Bank)+0x30,CB(A.Bank)+0x10,
        CB(A.Bank)+0x20,CB(A.Bank)+0x24,CB(A.Bank)+0x28,CB(A.Bank)+0x2C,
        CB(A.Bank)+0x38,CB(A.Bank)+0x3C,0};
      for (UINTN I=0; I<sizeof(Offsets)/sizeof(Offsets[0]); ++I) {
        Saved=Registers[Offsets[I]/4]; Registers[Offsets[I]/4]^=1;
        assert(SmmuQueryMappingEvidence(&A, Iova, Physical, Pages, 1, &E) == CR_DEVICE_ERROR);
        assert(!memcmp(&E, &Empty, sizeof(E))); Registers[Offsets[I]/4]=Saved;
      }
    }
    Device.ActlrSettings = NULL;
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
    assert(!memcmp(&E,&Empty,sizeof(E)));
    Device.ActlrSettingCount = 0;
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
    assert(!memcmp(&E,&Empty,sizeof(E)));
    Device.ActlrSettings = Conflict; Device.ActlrSettingCount = 2;
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
    assert(!memcmp(&E,&Empty,sizeof(E)));
    Device.ActlrSettings = Settings; Device.ActlrSettingCount = 1;
    A.OwnsActlr = FALSE;
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
    A.OwnsActlr = TRUE;
    {
      UINTN Entries[] = {0,512+(SMMU_IOVA_BASE>>21),512+(SMMU_IOVA_BASE>>21)+2,
                         First,First+511,First+Pages-1};
      for(UINTN I=0;I<sizeof(Entries)/sizeof(Entries[0]);++I) {
        SavedPte=A.Tables[Entries[I]]; A.Tables[Entries[I]]^=0x1000;
        assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
        assert(!memcmp(&E,&Empty,sizeof(E))); A.Tables[Entries[I]]=SavedPte;
      }
    }
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,0,1,&E)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(&A,Iova+1,Physical,Pages,1,&E)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical+1,Pages,1,&E)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,0,&E)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(&A,SMMU_IOVA_BASE+SMMU_IOVA_SIZE-4096,Physical,2,1,&E)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,NULL)==CR_INVALID_PARAMETER);
    assert(SmmuQueryMappingEvidence(NULL,Iova,Physical,Pages,1,&E)==CR_NOT_READY);
    A.Failed=TRUE;assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_NOT_READY);A.Failed=FALSE;
    Device.SyncFailed=TRUE;assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_NOT_READY);Device.SyncFailed=FALSE;
    assert(Writes==OldWrites && Publishes==OldPublishes && !memcmp(&A,&Before,sizeof(A)));
    assert(SmmuQueryRevokedMapping(&A,Iova,Pages,&E)==CR_DEVICE_ERROR);
    assert(!memcmp(&E,&Empty,sizeof(E)));
    assert(SmmuBlock(&A)==CR_SUCCESS);
    assert(SmmuQueryMappingEvidence(&A,Iova,Physical,Pages,1,&E)==CR_DEVICE_ERROR);
    OldWrites=Writes;OldPublishes=Publishes;
    assert(SmmuQueryRevokedMapping(&A,Iova,Pages,&E)==CR_SUCCESS);
    assert(E.Physical==0&&E.Access==0&&E.Bytes==Pages*SMMU_PAGE_SIZE&&E.Sid==A.Sid);
    assert(SmmuQueryRevokedMapping(&A,SMMU_IOVA_BASE,SMMU_IOVA_SIZE/SMMU_PAGE_SIZE,&E)==CR_SUCCESS);
    assert(E.Iova==SMMU_IOVA_BASE&&E.Bytes==SMMU_IOVA_SIZE&&!E.Physical&&!E.Access);
    UINTN WholeDomainOffsets[]={0,SMMU_IOVA_SIZE/SMMU_PAGE_SIZE/2,SMMU_IOVA_SIZE/SMMU_PAGE_SIZE-1};
    for(UINTN I=0;I<ARRAY_SIZE(WholeDomainOffsets);++I) {
      A.Tables[1024+WholeDomainOffsets[I]]=Physical|0x83;
      assert(SmmuQueryRevokedMapping(&A,SMMU_IOVA_BASE,SMMU_IOVA_SIZE/SMMU_PAGE_SIZE,&E)==CR_DEVICE_ERROR);
      assert(!memcmp(&E,&Empty,sizeof(E)));A.Tables[1024+WholeDomainOffsets[I]]=0;
    }
    UINTN RevokedOffsets[]={0,Pages/2,Pages-1};
    for(UINTN I=0;I<ARRAY_SIZE(RevokedOffsets);++I) {
      A.Tables[First+RevokedOffsets[I]]=Physical|0x83;
      assert(SmmuQueryRevokedMapping(&A,Iova,Pages,&E)==CR_DEVICE_ERROR);
      assert(!memcmp(&E,&Empty,sizeof(E)));A.Tables[First+RevokedOffsets[I]]=0;
    }
    Saved=Registers[(CB(A.Bank)+4)/4];Registers[(CB(A.Bank)+4)/4]^=1;
    assert(SmmuQueryRevokedMapping(&A,Iova,Pages,&E)==CR_DEVICE_ERROR);
    Registers[(CB(A.Bank)+4)/4]=Saved;
    Device.SyncFailed=TRUE;
    assert(SmmuQueryRevokedMapping(&A,Iova,Pages,&E)==CR_NOT_READY);
    Device.SyncFailed=FALSE;
    assert(SmmuQueryRevokedMapping(&A,Iova,0,&E)==CR_INVALID_PARAMETER);
    assert(Writes==OldWrites&&Publishes==OldPublishes);
  }

  /* Fault diagnostics must work even when the domain/controller is failed,
     while preserving every fault, table, register and software-owned bit. */
  Init(&Device,&A,&B);
  assert(SmmuProbe(&Device,BASE,sizeof(Registers),&Ops)==CR_SUCCESS);
  assert(SmmuAttach(&Device,&A,0xE0,Ta,0x20000000)==CR_SUCCESS);
  assert(SmmuSetMapping(&A,SMMU_IOVA_BASE,0x30000000,1,3)==CR_SUCCESS);
  UINT64 FarCases[]={SMMU_IOVA_BASE+0x3000, SMMU_IOVA_BASE+17, SMMU_IOVA_BASE+SMMU_IOVA_SIZE-1,
                    SMMU_IOVA_BASE-1, SMMU_IOVA_BASE+SMMU_IOVA_SIZE, 0xffffffffffffffffULL};
  for(UINTN I=0;I<ARRAY_SIZE(FarCases);I++) {
    UINT64 Far=FarCases[I];
    Registers[(CB(A.Bank)+4)/4]=0x1234; // Observe actual, unowned ACTLR.
    Registers[(CB(A.Bank)+0x58)/4]=0x402;
    Registers[(CB(A.Bank)+0x60)/4]=(UINT32)Far;
    Registers[(CB(A.Bank)+0x64)/4]=(UINT32)(Far>>32);
    Registers[(CB(A.Bank)+0x68)/4]=0x240003;
    Registers[(CB(A.Bank)+0x6C)/4]=0xB900;
    Registers[0x48/4]=2; Registers[0x54/4]=0xE0;
    A.Failed=TRUE; Device.SyncFailed=TRUE;
    SmmuDomain SavedDomain=A; SmmuDevice SavedDevice=Device;
    UINT64 SavedTables[SMMU_TABLE_PAGES*4096/8]; memcpy(SavedTables,Ta,TABLE_BYTES);
    memcpy(Before,Registers,sizeof(Before));
    unsigned OldWrites=Writes, OldPub=Publishes, OldDelay=Delays, OldFaults=FaultLogCount;
    assert(SmmuDumpFaultState(&A)==CR_SUCCESS && FaultLogCount>OldFaults);
    assert(CapturedActlr==0x1234 && CapturedTf==1 && CapturedPf==0);
    assert(CapturedLevel==3 && CapturedWrite==0 && CapturedPtw==0);
    assert(Writes==OldWrites && Publishes==OldPub && Delays==OldDelay);
    assert(!memcmp(Before,Registers,sizeof(Before)) && !memcmp(SavedTables,Ta,TABLE_BYTES));
    assert(!memcmp(&SavedDomain,&A,sizeof(A)) && !memcmp(&SavedDevice,&Device,sizeof(Device)));
  }
  N=Reads; assert(SmmuDumpFaultState(NULL)==CR_INVALID_PARAMETER && Reads==N);
  A.BankBase+=4096; assert(SmmuDumpFaultState(&A)==CR_NOT_READY && Reads==N); A.BankBase-=4096;
  Device.ReservedStreams[A.Stream]=FALSE;
  assert(SmmuDumpFaultState(&A)==CR_NOT_READY && Reads==N);

  assert(LogCount != 0 && FaultLogCount != 0 && TimeoutLogCount != 0 &&
         ProbeLogCount != 0 && DetachLogCount != 0);
  free(Ta);
  free(Tb);
  puts("SMMUv2: preservation, cold enable, isolation, permissions, boundaries, timeout and rejected-write checks passed");
  return 0;
}
