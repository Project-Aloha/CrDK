/* Actual SM8250/SM8350 targets with the PCIe110 board-log MMIO contract.
 * Host model only: no physical DMA or device execution. SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/CrTargetSmmuLib.h>
#include <Library/DebugLib.h>

#define BASE 0x15000000ULL
#define CB(n) (0x80000U + (n) * 4096U)
#define SMR(n) (0x800U + (n) * 4U)
#define S2CR(n) (0xC00U + (n) * 4U)
#define TABLE_BYTES (SMMU_TABLE_PAGES * SMMU_PAGE_SIZE)
static UINT32 Registers[0x100000 / 4], Saved[0x100000 / 4];
static UINT32 ClearSctlr, SetSctlr;
static unsigned Writes;

static UINT32 Read32(VOID *Cookie, UINT64 Address) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers) && !(Address & 3));
  return Registers[(Address - BASE) / 4];
}

static VOID Write32(VOID *Cookie, UINT64 Address, UINT32 Value) {
  (void)Cookie;
  assert(Address >= BASE && Address < BASE + sizeof(Registers) && !(Address & 3));
  ++Writes;
  if (Address >= BASE + CB(0)) {
    if ((Address & 0xFFF) == 0) Value = (Value & ~ClearSctlr) | SetSctlr;
    if ((Address & 0xFFF) == 0x10) Value |= 0x60; /* observed TCR2 RES bits */
    if ((Address & 0xFFF) == 0x58) { /* FSR is W1C */
      Registers[(Address - BASE) / 4] &= ~Value;
      return;
    }
  }
  Registers[(Address - BASE) / 4] = Value;
}

static VOID Write64(VOID *Cookie, UINT64 Address, UINT64 Value) {
  Write32(Cookie, Address, (UINT32)Value);
  Write32(Cookie, Address + 4, (UINT32)(Value >> 32));
}

static VOID Publish(VOID *Cookie, VOID *Address, UINTN Size) {
  (void)Cookie;
  assert(Address && Size);
}
static VOID DelayUs(VOID *Cookie, UINT32 Delay) { (void)Cookie; (void)Delay; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (void)Level; (void)Format; }
static SmmuIoOps Ops = {Read32, Write32, Write64, Publish, DelayUs, NULL};

static VOID *Tables(VOID) {
  VOID *P = NULL;
  assert(!posix_memalign(&P, SMMU_PAGE_SIZE, TABLE_BYTES));
  memset(P, 0xA5, TABLE_BYTES);
  return P;
}

static VOID Init(SmmuDevice *Device, CONST CrTargetSmmuContext *Target) {
  memset(Device, 0, sizeof(*Device));
  memset(Registers, 0, sizeof(Registers));
#if TEST_SOC == 8250
  /* kona.txt: 87 streams, 74 CBs, no S2-only banks, 36-bit apertures. */
  Registers[0x20 / 4] = 0x4C017E57;
  Registers[0x24 / 4] = 0x6000004A;
  Registers[0] = 0x00250406;
#elif TEST_SOC == 8350
  /* lahaina.txt: 89 streams, 75 CBs, no S2-only banks, 36-bit apertures. */
  Registers[0x20 / 4] = 0x4C017E59;
  Registers[0x24 / 4] = 0x6000004B;
  Registers[0] = 0x00200406;
#else
#error Unsupported HDK fixture
#endif
  Registers[0x28 / 4] = 0x00005111;
  /* A live, foreign display route remains untouched by every operation. */
  Registers[SMR(0) / 4] = 0x80000900;
  Registers[S2CR(0) / 4] = 2;
  Registers[CB(2) / 4] = 0x27;
  Registers[(0x1000 + 2 * 4) / 4] = 0x1F300;
  Registers[(0x1800 + 2 * 4) / 4] = 1;
  Registers[(CB(2) + 0x24) / 4] = 1U << 16;
  Writes = 0;
  ClearSctlr = 0x1000; /* all tested RCs read back 0x27 after writing 0x1027 */
  SetSctlr = 0;
  memcpy(Saved, Registers, sizeof(Saved));
  assert(SmmuProbe(Device, Target->Base, Target->Size, &Ops) == CR_SUCCESS);
  assert(!Writes && !Device->Stage2BankCount);
  assert(Device->StreamCount == (TEST_SOC == 8250 ? 87 : 89));
  assert(Device->BankCount == (TEST_SOC == 8250 ? 74 : 75));
  assert(Device->InputAddressBits == 36 && Device->Stage1OutputAddressBits == 36 &&
         Device->AddressBits == 36);
  Device->ActlrSettings = Target->ActlrSettings;
  Device->ActlrSettingCount = Target->ActlrSettingCount;
  Device->SctlrReadbackIgnoreMask = Target->SctlrReadbackIgnoreMask;
}

static VOID Preserved(VOID) {
  assert(Registers[0] == Saved[0]);
  assert(Registers[SMR(0) / 4] == Saved[SMR(0) / 4]);
  assert(Registers[S2CR(0) / 4] == Saved[S2CR(0) / 4]);
  assert(Registers[(0x1000 + 2 * 4) / 4] == Saved[(0x1000 + 2 * 4) / 4]);
  assert(Registers[(0x1800 + 2 * 4) / 4] == Saved[(0x1800 + 2 * 4) / 4]);
  assert(!memcmp(Registers + CB(2) / 4, Saved + CB(2) / 4, 4096));
}

static BOOLEAN Coherent(CONST CrTargetSmmuContext *Target, UINT16 Sid) {
  for (UINTN I = 0; I < Target->CoherentStreamCount; ++I)
    if (Target->CoherentStreamIds[I] == Sid) return TRUE;
  return FALSE;
}

int main(void) {
  CONST CrTargetSmmuContext *Target = CrTargetGetSmmuContext();
  SmmuDevice Device;
  SmmuDomain Domains[10] = {{0}};
  UINT64 *Pte[10] = {0};
  SmmuMappingEvidence Evidence, Empty = {0};
  assert(Target->DefaultStreamCount == (TEST_SOC == 8250 ? 10 : 7));
  Init(&Device, Target);
  /* Real target metadata must allow the observed bit-12 RAZ behavior for
   * UFS and every PCIe SID; a zero target mask reproduces PCIe110's failure. */
  for (UINTN I = 0; I < Target->DefaultStreamCount; ++I) {
    /* Begin with the exact failed RC0 SID, then cover the UFS SIDs too. */
    UINT16 Sid = Target->DefaultStreamIds[(I + (TEST_SOC == 8250 ? 2 : 1)) % Target->DefaultStreamCount];
    CR_STATUS Status;
    Pte[I] = Tables();
    if (Coherent(Target, Sid))
      Status = SmmuAttachCoherent(&Device, &Domains[I], Sid, Pte[I], 0x30000000 + I * 0x100000);
    else
      Status = SmmuAttach(&Device, &Domains[I], Sid, Pte[I], 0x30000000 + I * 0x100000);
    if (Status != CR_SUCCESS)
      fprintf(stderr, "SM%u SID=0x%x attach status=0x%llx SCTLR=0x%x mask=0x%x\n",
              TEST_SOC, Sid, (unsigned long long)Status,
              Registers[CB(Domains[I].Bank) / 4], Device.SctlrReadbackIgnoreMask);
    assert(Status == CR_SUCCESS);
    assert(Domains[I].Active && !Domains[I].Failed && Domains[I].CacheCoherent == Coherent(Target, Sid));
    assert(Registers[CB(Domains[I].Bank) / 4] == 0x27);
    assert(Registers[(CB(Domains[I].Bank) + 0x38) / 4] == (Coherent(Target, Sid) ? 0xFF : 0x44));
    assert(SmmuSetMapping(&Domains[I], SMMU_IOVA_BASE, 0x800000000ULL + I * 0x100000,
                          1, SMMU_ACCESS_READ | SMMU_ACCESS_WRITE) == CR_SUCCESS);
    assert(SmmuQueryMappingEvidence(&Domains[I], SMMU_IOVA_BASE,
           0x800000000ULL + I * 0x100000, 1, 3, &Evidence) == CR_SUCCESS);
    assert(Evidence.Sctlr == 0x27 && Evidence.SctlrIgnoreMask == 0x1000);
    for (UINTN J = 0; J < I; ++J)
      assert(Domains[I].Bank != Domains[J].Bank && Domains[I].Stream != Domains[J].Stream);
    Preserved();
  }
  for (UINTN I = 0; I < Target->DefaultStreamCount; ++I) {
    assert(SmmuBlock(&Domains[I]) == CR_SUCCESS);
    assert(SmmuQueryRevokedMapping(&Domains[I], SMMU_IOVA_BASE, 1, &Evidence) == CR_SUCCESS);
    assert(SmmuDetach(&Domains[I]) == CR_SUCCESS);
    for (UINTN J = I + 1; J < Target->DefaultStreamCount; ++J)
      assert(Domains[J].Active && Pte[J][1024]);
    Preserved();
  }
  /* Bit12 is the entire exception. Loss of M/TRE/AFE/CFRE, or unexpected
   * CFIE, must fail before publishing a stream and retain failed ownership. */
  const UINT32 Essential[] = {1, 2, 4, 0x20, 0x40};
  for (UINTN I = 0; I < ARRAY_SIZE(Essential); ++I) {
    SmmuDomain Failed = {0};
    Init(&Device, Target);
    if (Essential[I] == 0x40) SetSctlr = 0x40;
    else ClearSctlr |= Essential[I];
    assert(SmmuAttachCoherent(&Device, &Failed, 0x1C00, Pte[0], 0x30000000) == CR_DEVICE_ERROR);
    assert(Failed.Failed && !Failed.Active);
    assert(Device.ReservedBanks[Failed.Bank] && Device.ReservedStreams[Failed.Stream] &&
           Device.ReservedAsids[Failed.Asid]);
    assert(!(Registers[SMR(Failed.Stream) / 4] & 0x80000000));
    Preserved();
  }
  /* Live evidence must reject the same essential-bit changes after attach. */
  SmmuDomain Own = {0};
  Init(&Device, Target);
  assert(SmmuAttachCoherent(&Device, &Own, 0x1C00, Pte[0], 0x30000000) == CR_SUCCESS);
  assert(SmmuSetMapping(&Own, SMMU_IOVA_BASE, 0x800000000ULL, 1, 3) == CR_SUCCESS);
  for (UINTN I = 0; I < ARRAY_SIZE(Essential); ++I) {
    Registers[CB(Own.Bank) / 4] ^= Essential[I];
    assert(SmmuQueryMappingEvidence(&Own, SMMU_IOVA_BASE, 0x800000000ULL,
                                    1, 3, &Evidence) == CR_DEVICE_ERROR);
    assert(!memcmp(&Evidence, &Empty, sizeof(Evidence)));
    Registers[CB(Own.Bank) / 4] ^= Essential[I];
  }
  assert(SmmuBlock(&Own) == CR_SUCCESS && SmmuDetach(&Own) == CR_SUCCESS);
  Preserved();
  for (UINTN I = 0; I < Target->DefaultStreamCount; ++I) free(Pte[I]);
  printf("SM%u logged geometry: bit12 RAZ accepted for all UFS/PCIe SIDs; essential SCTLR failures, live evidence, isolation and revocation PASS\n", TEST_SOC);
  return 0;
}
