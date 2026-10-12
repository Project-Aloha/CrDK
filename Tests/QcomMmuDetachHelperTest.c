/** Early Qualcomm SMMU handoff regression. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static UINT8 KeepList[16];
static UINTN KeepSize;
static BOOLEAN CaptureEnabled;
#define _PCD_VALUE_PcdSMMUSIDsToKeep KeepList
#define _PCD_SIZE_PcdSMMUSIDsToKeep KeepSize
#define _PCD_VALUE_PcdSmmuUfsPreDetachCapture CaptureEnabled
#include "../../MUCommonCrPkg/Library/QcomMmuDetachHelperLib/QcomMmuDetachHelperLib.c"
#include "../../../Silicon/QC/Sm8450Cr/QcomPkg/Library/PlatformPrePiLib/PlatformUtils.c"

#define TEST_BASE 0x15000000U
#define TEST_CB(n) (0x80000U + (n) * 0x1000U)
#define TEST_SMR(n) ARM_SMMU_GR0_SMR(n)
#define TEST_S2CR(n) ARM_SMMU_GR0_S2CR(n)
static UINT32 Registers[SIZE_1MB / 4], Before[SIZE_1MB / 4];
static UINTN Reads, Writes, Syncs, Delays;
static UINTN RejectAddress;
static BOOLEAN Timeout, FaultQuirk, ChangeRoute;
static UINTN FirstRouteReads;
static ARM_MEMORY_REGION_DESCRIPTOR_EX Region;
static EFI_STATUS LookupStatus;
static jmp_buf Halt;
static BOOLEAN ExpectHalt;
static CHAR8 Capture[32768];
static UINTN CaptureSize, BankReads;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return !CaptureEnabled; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{ (VOID)Level; (VOID)Format; }
UINTN EFIAPI AsciiVSPrint(CHAR8 *Buffer, UINTN Size, CONST CHAR8 *Format, VA_LIST Args)
{
  /* Host adapter for the two EDK-specific specifiers used by the real logger. */
  CHAR8 HostFormat[512];
  UINTN Out = 0;
  for (UINTN i = 0; Format[i]; i++) {
    assert(Out + 3 < sizeof(HostFormat));
    if (Format[i] == '%' && Format[i + 1] == 'a') {
      HostFormat[Out++] = '%'; HostFormat[Out++] = 's'; i++;
    } else if (Format[i] == '%' && Format[i + 1] == 'L' && Format[i + 2] == 'x') {
      HostFormat[Out++] = '%'; HostFormat[Out++] = 'l'; HostFormat[Out++] = 'l';
      HostFormat[Out++] = 'x'; i += 2;
    } else {
      HostFormat[Out++] = Format[i];
    }
  }
  HostFormat[Out] = 0;
  int Count = vsnprintf(Buffer, Size, HostFormat, Args);
  assert(Count >= 0 && (UINTN)Count < Size);
  return (UINTN)Count;
}
UINTN EFIAPI SerialPortWrite(UINT8 *Buffer, UINTN Size)
{
  assert(CaptureSize + Size < sizeof(Capture));
  memcpy(Capture + CaptureSize, Buffer, Size);
  CaptureSize += Size;
  Capture[CaptureSize] = 0;
  return Size;
}
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays += Us; return Us; }
VOID EFIAPI CpuDeadLoop(VOID)
{
  assert(ExpectHalt);
  longjmp(Halt, 1);
}
EFI_STATUS EFIAPI LocateMemoryMapAreaByName(
    CHAR8 *Name, ARM_MEMORY_REGION_DESCRIPTOR_EX *Result)
{
  assert(strcmp(Name, "SMMU") == 0);
  if (EFI_ERROR(LookupStatus)) return LookupStatus;
  *Result = Region;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  assert(Address >= TEST_BASE && Address < TEST_BASE + sizeof(Registers));
  assert((Address & 3U) == 0);
  Reads++;
  if (Address >= TEST_BASE + TEST_CB(0)) BankReads++;
  if (Timeout && Address == TEST_BASE + ARM_SMMU_GR0_sTLBGSTATUS) return 1;
  if (ChangeRoute && Address == TEST_BASE + TEST_SMR(0) &&
      ++FirstRouteReads == 2) Registers[TEST_SMR(0) / 4] ^= 1;
  return Registers[(Address - TEST_BASE) / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  assert(Address >= TEST_BASE && Address < TEST_BASE + sizeof(Registers));
  assert((Address & 3U) == 0);
  Writes++;
  if (Address == TEST_BASE + ARM_SMMU_GR0_sTLBGSYNC) Syncs++;
  if (Address == RejectAddress) return Value;
  if (FaultQuirk && Address >= TEST_BASE + TEST_S2CR(0) &&
      Address < TEST_BASE + TEST_S2CR(128)) {
    if ((Value & 0x30000U) == 0x20000U) return Value;
    if ((Value & 0x30000U) == 0x10000U)
      Value = (Value & ~0x30000U) | 0x20000U;
  }
  Registers[(Address - TEST_BASE) / 4] = Value;
  return Value;
}

static VOID Init(VOID)
{
  memset(Registers, 0, sizeof(Registers));
  memset(&Region, 0, sizeof(Region));
  Region.Address = TEST_BASE;
  Region.Length = sizeof(Registers);
  LookupStatus = EFI_SUCCESS;
  Registers[ARM_SMMU_GR0_ID0 / 4] = ARM_SMMU_ID0_SMS | 16;
  Registers[ARM_SMMU_GR0_ID1 / 4] = 0x60020008; /* 128 4K pages, 8 CBs, 2 S2 CBs */
  for (UINTN bank = 0; bank < 8; bank++) {
    Registers[(0x1000 + bank * 4) / 4] = 0x1f300;
    Registers[TEST_CB(bank) / 4] = 0x1027;
    Registers[(TEST_CB(bank) + 0x20) / 4] = 0x12340000 + (UINT32)bank * 0x1000;
    Registers[(TEST_CB(bank) + 0x24) / 4] = 0x10000 + (UINT32)bank;
    Registers[(TEST_CB(bank) + 0x30) / 4] = 0x803520;
  }
  Reads = Writes = Syncs = Delays = FirstRouteReads = 0;
  RejectAddress = 0;
  Timeout = FaultQuirk = ChangeRoute = ExpectHalt = FALSE;
  CaptureEnabled = FALSE;
  CaptureSize = BankReads = 0;
  Capture[0] = 0;
  KeepSize = 2;
  KeepList[0] = 0x02;
  KeepList[1] = 0x28;
}
static VOID Route(UINTN Index, UINT16 Sid, UINT16 Mask, UINT32 Type, UINT32 Bank)
{
  Registers[TEST_SMR(Index) / 4] = SMR_VALID | ((UINT32)Mask << 16) | Sid;
  Registers[TEST_S2CR(Index) / 4] = (Type << 16) | Bank;
}
static VOID OneRetiringRoute(VOID)
{
  Init();
  Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 2);
  memcpy(Before, Registers, sizeof(Registers));
}
static VOID AssertBankIntact(UINTN Bank)
{
  assert(memcmp(Registers + TEST_CB(Bank) / 4, Before + TEST_CB(Bank) / 4,
                0x1000) == 0);
  assert(Registers[(0x1000 + Bank * 4) / 4] == Before[(0x1000 + Bank * 4) / 4]);
}
static VOID AssertNoWrites(VOID)
{
  assert(Writes == 0 && Syncs == 0);
  assert(memcmp(Registers, Before, sizeof(Registers)) == 0);
}

static VOID RunCapture(VOID)
{
  memcpy(Before, Registers, sizeof(Registers));
  CaptureEnabled = ExpectHalt = TRUE;
  if (setjmp(Halt) == 0) {
    PlatformInitialize();
    assert(0 && "capture must halt before detach and DXE");
  }
  assert(Writes == 0 && Syncs == 0 && Delays == 0);
  assert(strstr(Capture, "BEGIN read-only SID=00e0 capture"));
  assert(strstr(Capture, "HALT before detach; no SMMU writes, no DXE/EBS"));
}

static VOID TestCapture(VOID)
{
  OneRetiringRoute();
  Registers[(0x1800 + 2 * 4) / 4] = 1;
  Registers[(TEST_CB(2) + ARM_SMMU_CB_TTBR0) / 4] = 0xdeadb000;
  Registers[(TEST_CB(2) + ARM_SMMU_CB_TTBR0 + 4) / 4] = 0x00350008;
  Registers[(TEST_CB(2) + ARM_SMMU_CB_MAIR0) / 4] = 0xff;
  Registers[(TEST_CB(2) + ARM_SMMU_CB_FSR) / 4] = 0x40000002;
  Registers[(TEST_CB(2) + ARM_SMMU_CB_FAR) / 4] = 0x10003000;
  Route(1, 0x1234, 0, S2CR_TYPE_TRANS, 2);
  RunCapture(); AssertNoWrites();
  assert(strstr(Capture, "S1=translation S2=bypass"));
  assert(strstr(Capture, "TTBR0=00350008deadb000"));
  assert(strstr(Capture, "MAIR0=000000ff"));
  assert(strstr(Capture, "FSR=40000002 FAR=0000000010003000"));
  assert(strstr(Capture, "referencing-groups=2"));
  assert(strstr(Capture, "T0SZ=32 TG0=0 EPD0=0 EPD1=1 SH0=3 IRGN0=1 ORGN0=1"));
  assert(strstr(Capture, "matches=1 ambiguous=0 repeated-route-read-changed=0"));

  for (UINT32 Type = S2CR_TYPE_BYPASS; Type <= 3; Type++) {
    Init(); Route(0, 0xe0, 0, Type, 255);
    RunCapture(); AssertNoWrites();
    assert(BankReads == 0); /* BYPASS/FAULT/reserved do not select CBNDX. */
    assert(strstr(Capture, Type == 1 ? "route=bypass" :
        (Type == 2 ? "route=fault" : "route=reserved")));
  }
  Init(); Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 255);
  RunCapture(); AssertNoWrites();
  assert(BankReads == 0 && strstr(Capture, "invalid context bank"));

  OneRetiringRoute();
  Route(1, 0xc0, 0x20, S2CR_TYPE_TRANS, 2); /* Also matches E0. */
  RunCapture(); AssertNoWrites();
  assert(strstr(Capture, "matches=2 ambiguous=1"));
  CHAR8 *BankLine = strstr(Capture, "CB=2 CBAR=");
  assert(BankLine && !strstr(BankLine + 1, "CB=2 CBAR="));

  Init(); Route(0, 0xc0, 0x10, S2CR_TYPE_TRANS, 2); /* Does not match E0. */
  Route(1, 0xe0, 0, S2CR_TYPE_TRANS, 2);
  Registers[TEST_SMR(1) / 4] &= ~SMR_VALID;
  RunCapture(); AssertNoWrites();
  assert(BankReads == 0 && strstr(Capture, "matches=0 ambiguous=0"));

  OneRetiringRoute();
  Registers[TEST_CB(2) / 4] = 0x0a5f0000;
  RunCapture(); AssertNoWrites();
  assert(strstr(Capture, "S1=bypass S2=bypass"));
  assert(strstr(Capture, "WACFG=2 RACFG=2 SHCFG=1 MTCFG=1 MEMATTR=f"));

  Init(); Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 0);
  Registers[0x1000 / 4] = 0;
  RunCapture(); AssertNoWrites();
  assert(strstr(Capture, "dedicated-S2=1"));
  assert(strstr(Capture, "stage2/fault/nested format; raw registers only"));
  assert(!strstr(Capture, "S1=translation"));
  Init(); Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 7);
  Registers[0] = 0x401;
  RunCapture(); AssertNoWrites();
  assert(strstr(Capture, "final-bank=1"));
  assert(strstr(Capture, "CLIENTPD=1 USFCFG=1"));
  assert(strstr(Capture, "bank M alone does not prove active translation"));

  OneRetiringRoute(); ChangeRoute = TRUE;
  RunCapture();
  assert(strstr(Capture, "repeated-route-read-changed=1"));

  OneRetiringRoute(); LookupStatus = EFI_NOT_FOUND;
  RunCapture(); AssertNoWrites();
  assert(Reads == 0 && !strstr(Capture, "matches="));
  OneRetiringRoute(); Registers[ARM_SMMU_GR0_ID0 / 4] = ARM_SMMU_ID0_SMS | 129;
  RunCapture(); AssertNoWrites();
  assert(BankReads == 0 && !strstr(Capture, "matches="));
  puts("Pre-detach UFS capture: raw high TTBR/faults, wildcard/ambiguous/shared/final routes, bypass/stage2 decoding, masked DEBUG, changing state and error halt; zero SMMU writes passed");
}

int main(void)
{
  Init();
  Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 2);
  Route(1, 0x2800, 0x402, S2CR_TYPE_TRANS, 3); /* wildcard kept MDSS */
  Route(2, 0x56, 0, S2CR_TYPE_TRANS, 4);
  Route(3, 0x2802, 0, S2CR_TYPE_TRANS, 4); /* shares CB with retired route */
  Route(4, 0x100, 0, S2CR_TYPE_TRANS, 0); /* dedicated stage 2 */
  Route(5, 0x200, 0, S2CR_TYPE_TRANS, 5); /* nested/foreign bank */
  Registers[(0x1000 + 5 * 4) / 4] = 3U << CBAR_TYPE_SHIFT;
  Route(6, 0x300, 0, S2CR_TYPE_TRANS, 7); /* final Qualcomm bypass CB */
  Route(7, 0x400, 0, S2CR_TYPE_BYPASS, 6); /* CBNDX is irrelevant for BYPASS */
  Route(8, 0x500, 0, S2CR_TYPE_FAULT, 6);
  memcpy(Before, Registers, sizeof(Registers));
  assert(MmuDetach() == EFI_SUCCESS && Syncs == 1);
  for (UINTN i = 0; i < 9; i++) {
    BOOLEAN Removed = i == 0 || i == 2 || i == 7 || i == 8;
    assert(((Registers[TEST_SMR(i) / 4] & SMR_VALID) == 0) == Removed);
    if (!Removed) {
      assert(Registers[TEST_SMR(i) / 4] == Before[TEST_SMR(i) / 4]);
      assert(Registers[TEST_S2CR(i) / 4] == Before[TEST_S2CR(i) / 4]);
    }
  }
  for (UINTN bank = 0; bank < 8; bank++) {
    if (bank != 2) {
      AssertBankIntact(bank);
    } else {
      assert(Registers[TEST_CB(bank) / 4] == (Before[TEST_CB(bank) / 4] & ~SCTLR_M));
      assert(memcmp(Registers + TEST_CB(bank) / 4 + 1,
                    Before + TEST_CB(bank) / 4 + 1, 0x1000 - 4) == 0);
      assert(Registers[(0x1000 + bank * 4) / 4] == Before[(0x1000 + bank * 4) / 4]);
    }
  }
  assert(Registers[0] == Before[0]); /* no global reset/reconfiguration */

  OneRetiringRoute();
  FaultQuirk = TRUE;
  assert(MmuDetach() == EFI_SUCCESS);
  assert((Registers[TEST_S2CR(0) / 4] & 0x30000U) == 0x20000U);
  assert((Registers[TEST_CB(2) / 4] & SCTLR_M) == 0);

  OneRetiringRoute();
  RejectAddress = TEST_BASE + TEST_S2CR(0);
  assert(MmuDetach() == EFI_DEVICE_ERROR && Syncs == 0);
  assert(Registers[TEST_SMR(0) / 4] == Before[TEST_SMR(0) / 4]);
  AssertBankIntact(2);
  OneRetiringRoute();
  RejectAddress = TEST_BASE + TEST_SMR(0);
  assert(MmuDetach() == EFI_DEVICE_ERROR && Syncs == 0);
  AssertBankIntact(2);
  OneRetiringRoute();
  Timeout = TRUE;
  assert(MmuDetach() == EFI_DEVICE_ERROR && Syncs == 1 && Delays == 1000000);
  AssertBankIntact(2);
  OneRetiringRoute();
  RejectAddress = TEST_BASE + TEST_CB(2);
  assert(MmuDetach() == EFI_DEVICE_ERROR && Syncs == 1);
  AssertBankIntact(2);
  OneRetiringRoute();
  ChangeRoute = TRUE;
  assert(MmuDetach() == EFI_DEVICE_ERROR && Writes == 0);
  AssertBankIntact(2);

  Init();
  Route(0, 0x2800, 0x402, S2CR_TYPE_TRANS, 3);
  memcpy(Before, Registers, sizeof(Registers));
  assert(MmuDetach() == EFI_SUCCESS);
  AssertNoWrites(); /* a kept-only controller needs no maintenance */
  OneRetiringRoute();
  KeepSize = 1; KeepList[0] = 0; /* historical empty default */
  assert(MmuDetach() == EFI_SUCCESS);
  OneRetiringRoute();
  KeepSize = 3;
  assert(MmuDetach() == EFI_DEVICE_ERROR);
  AssertNoWrites();
  OneRetiringRoute();
  Route(0, 0xe0, 0, S2CR_TYPE_TRANS, 8);
  memcpy(Before, Registers, sizeof(Registers));
  assert(MmuDetach() == EFI_DEVICE_ERROR);
  AssertNoWrites();
  OneRetiringRoute();
  Route(0, 0xe0, 0, 3, 2);
  memcpy(Before, Registers, sizeof(Registers));
  assert(MmuDetach() == EFI_DEVICE_ERROR);
  AssertNoWrites();

  for (UINTN Case = 0; Case < 7; Case++) {
    OneRetiringRoute();
    switch (Case) {
      case 0: Registers[ARM_SMMU_GR0_ID0 / 4] &= ~ARM_SMMU_ID0_SMS; break;
      case 1: Registers[ARM_SMMU_GR0_ID0 / 4] = ARM_SMMU_ID0_SMS | 129; break;
      case 2: Registers[ARM_SMMU_GR0_ID1 / 4] &= ~ARM_SMMU_ID1_NUMCB_MASK; break;
      case 3: Registers[ARM_SMMU_GR0_ID1 / 4] = 0x00000008; break; /* only 2 pages */
      case 4: Registers[ARM_SMMU_GR0_ID1 / 4] = 0x60090008; break; /* S2 > all CBs */
      case 5: Registers[0] |= ARM_SMMU_sCR0_EXIDENABLE; break;
      case 6: Registers[ARM_SMMU_GR0_ID1 / 4] |= ARM_SMMU_ID1_PAGESIZE; break;
    }
    memcpy(Before, Registers, sizeof(Registers));
    assert(MmuDetach() == EFI_UNSUPPORTED);
    AssertNoWrites();
  }
  OneRetiringRoute();
  LookupStatus = EFI_NOT_FOUND;
  assert(MmuDetach() == EFI_NOT_FOUND && Reads == 0);
  OneRetiringRoute();
  Region.Address = MAX_UINTN - SIZE_1MB + 1;
  assert(MmuDetach() == EFI_INVALID_PARAMETER && Reads == 0);
  OneRetiringRoute();
  Region.Address++;
  assert(MmuDetach() == EFI_INVALID_PARAMETER && Reads == 0);
  OneRetiringRoute();
  Region.Length--;
  assert(MmuDetach() == EFI_INVALID_PARAMETER && Reads == 0);

  /* The actual SM8450 caller stops at a failed handoff before DXE can submit
   * more TLB commands. A successful handoff still returns normally. */
  OneRetiringRoute();
  PlatformInitialize();
  assert(Syncs == 1);
  OneRetiringRoute();
  Timeout = TRUE;
  ExpectHalt = TRUE;
  if (setjmp(Halt) == 0) {
    PlatformInitialize();
    assert(0 && "failed fence must stop before DXE");
  }
  assert(Syncs == 1);
  AssertBankIntact(2);
  puts("Early SMMU detach: kept/shared/protected banks, verified retirement, ignored writes, timeout and SM8450 stop passed");
  TestCapture();
  return 0;
}
