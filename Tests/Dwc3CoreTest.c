/** Actual DWC3 core revision/host register regression. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef CR_DWC3_CORE_SOURCE
#define CR_DWC3_CORE_SOURCE "../Driver/Dwc3CrDxe/Dwc3Core.c"
#endif
#include CR_DWC3_CORE_SOURCE

static UINT32 Registers[0x100000 / 4];
static UINTN RetryReads, RetryWrites, VersionReads, Writes;
static BOOLEAN FailHalt, HdkEventLayout;
static UINTN ExtraEventAccesses;
static BOOLEAN FailGapWrite, FailRetryWrite;
VOID EFIAPI MemoryFence(VOID) {}
UINTN EFIAPI MicroSecondDelay(UINTN N) { return N; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
UINT32 EFIAPI MmioRead32(UINTN A)
{
  assert(A >= CR_USB_BASE && A < CR_USB_BASE + sizeof(Registers) && !(A & 3));
  UINTN O = A - CR_USB_BASE;
  if (O >= 0xC410 && O < 0xC600) ++ExtraEventAccesses;
  if (HdkEventLayout && O >= 0xC440 && O < 0xC600) return 0;
  if (O == 0xC60C) ++RetryReads;
  if (O == 0xC1A0 || O == 0xC1A4) ++VersionReads;
  if (O == 0x44) return FailHalt ? 0 : BIT0;
  if (O == 0xC70C) return FailHalt ? 0 : BIT22;
  return Registers[O / 4];
}
UINT32 EFIAPI MmioWrite32(UINTN A, UINT32 V)
{
  assert(A >= CR_USB_BASE && A < CR_USB_BASE + sizeof(Registers) && !(A & 3));
  UINTN O = A - CR_USB_BASE;
  ++Writes;
  if (O >= 0xC410 && O < 0xC600) ++ExtraEventAccesses;
  if (HdkEventLayout && O >= 0xC440 && O < 0xC600) return V;
  if (O == 0xC60C) ++RetryWrites;
  if (O == 0xC60C && FailRetryWrite) V &= ~(UINT32)BIT16;
  if (O == 0xC11C && FailGapWrite) V &= ~(7U << 21);
  if (O == 0xC704) V &= ~(UINT32)BIT30;
  Registers[O / 4] = V;
  return V;
}
UINT32 EFIAPI MmioOr32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) | V); }
UINT32 EFIAPI MmioAnd32(UINTN A, UINT32 V) { return MmioWrite32(A, MmioRead32(A) & V); }
UINT32 EFIAPI MmioAndThenOr32(UINTN A, UINT32 M, UINT32 V)
{ return MmioWrite32(A, (MmioRead32(A) & M) | V); }

static VOID Fixture(UINT32 Id, UINT32 Revision)
{
  memset(Registers, 0, sizeof(Registers));
  Registers[0] = 0x40;
  Registers[0xC120 / 4] = Id;
  Registers[0xC140 / 4] = 2;
  Registers[0xC1A0 / 4] = Revision;
  Registers[0xC1A4 / 4] = 0x67610000;
  Registers[0xC11C / 4] = 0xA0808001 | (7U << 21);
  Registers[0xC60C / 4] = 0x80104002;
  RetryReads = RetryWrites = VersionReads = Writes = 0;
  FailHalt = HdkEventLayout = FALSE;
  ExtraEventAccesses = 0;
  FailGapWrite = FailRetryWrite = FALSE;
}
static VOID Check(UINT32 Id, UINT32 Rev, EFI_USB_TYPEC_DATA_ROLE Role,
                  BOOLEAN Gap, BOOLEAN Retry)
{
  Fixture(Id, Rev);
  UINT32 OldControl = Registers[0xC11C / 4], OldRetry = Registers[0xC60C / 4];
  assert(CrUsbCoreInit(Role, TRUE) == EFI_SUCCESS);
  UINT32 ClockMask = BIT26, GapMask = 7U << 21;
  UINT32 Expected = (OldControl & ~ClockMask);
  if (Gap) Expected = (Expected & ~GapMask) | (1U << 21);
  assert(Registers[0xC11C / 4] == Expected);
  assert(Registers[0xC60C / 4] == (OldRetry | (Retry ? BIT16 : 0)));
  assert(RetryWrites == (Retry ? 1U : 0U));
  assert(RetryReads == (Gap ? (Retry ? 2U : 1U) : 0U));
  assert(VersionReads == ((Id >> 16) == 0x3331 ? 2U : 0U));
}
int main(int argc, char **argv)
{
  if (argc == 2 && strcmp(argv[1], "hdk190") == 0) {
    Check(0x33313130, 0x3139302A, UsbTypeCHost, TRUE, TRUE);
    puts("actual HDK DWC31 1.90a Android host retry policy PASS");
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "events33") == 0) {
    Fixture(0x33313130,0x3139302A);
    Registers[0xC144/4]=33U<<15;
    assert(CrUsbCoreInit(UsbTypeCHost,TRUE)==EFI_SUCCESS && !ExtraEventAccesses);
    puts("native DWC3 interrupt count never drives event-bank access PASS");
    return 0;
  }
  if (argc == 2 && strcmp(argv[1], "events18") == 0) {
    Fixture(0x33313130,0x3139302A);
    Registers[0xC144/4]=18U<<15;
    HdkEventLayout=TRUE;
    EFI_STATUS Status=CrUsbCoreInit(UsbTypeCHost,TRUE);
    fprintf(stderr,"capture66 fixture: status=%llx extra-event-accesses=%zu\n",(unsigned long long)Status,(size_t)ExtraEventAccesses);
    assert(Status==EFI_SUCCESS);
    assert(!ExtraEventAccesses);
    puts("native HDK 18 interrupts with reserved event banks: managed ring0 only PASS");
    return 0;
  }
  assert(argc == 1);
  Check(0x33313130, 0x3137302A, UsbTypeCHost, TRUE, TRUE);
  Check(0x33313130, 0x3138302A, UsbTypeCHost, TRUE, TRUE);
  Check(0x33313130, 0x3139302A, UsbTypeCHost, TRUE, TRUE);
  Check(0x33313130, 0x3230302A, UsbTypeCHost, TRUE, FALSE);
  CONST UINT32 Unknown[] = {0, 0xFFFFFFFF, 0x33313130, 0x3136302A, 0x3137312A,
                            0x31383000, 0x3231302A};
  for (UINTN I = 0; I < ARRAY_SIZE(Unknown); ++I)
    Check(0x33313130, Unknown[I], UsbTypeCHost, FALSE, FALSE);
  Check(0x55333290, 0x3137302A, UsbTypeCHost, FALSE, FALSE);
  Check(0x33313130, 0x3137302A, UsbTypeCDevice, FALSE, FALSE);
  Check(0x33313130, 0x3139302A, UsbTypeCDevice, FALSE, FALSE);
  Fixture(0x33313130, 0x3137302A);
  assert(CrUsbCoreInit(UsbTypeCNone, TRUE) == EFI_INVALID_PARAMETER && !Writes);
  Fixture(0xDEAD0000, 0x3137302A);
  assert(CrUsbCoreInit(UsbTypeCHost, TRUE) == EFI_UNSUPPORTED && !Writes);
  Fixture(0x33313130, 0x3137302A); Registers[0xC140 / 4] = 0;
  assert(CrUsbCoreInit(UsbTypeCHost, TRUE) == EFI_UNSUPPORTED && !Writes);
  Fixture(0x33313130, 0x3137302A); FailHalt = TRUE;
  assert(CrUsbCoreInit(UsbTypeCHost, TRUE) == EFI_TIMEOUT);
  Fixture(0x33313130, 0x3139302A); FailGapWrite = TRUE;
  assert(CrUsbCoreInit(UsbTypeCHost, TRUE) == EFI_DEVICE_ERROR && !RetryReads && !RetryWrites);
  Fixture(0x33313130, 0x3139302A); FailRetryWrite = TRUE;
  assert(CrUsbCoreInit(UsbTypeCHost, TRUE) == EFI_DEVICE_ERROR && RetryReads == 2 && RetryWrites == 1);
  puts("actual DWC3 core: qualified USB2 gap/retry, unknown revisions, family/role isolation, halt failure PASS");
  return 0;
}
