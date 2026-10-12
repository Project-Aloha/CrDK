/** Real SM8450 clock graph behind the DWC3 role fixture. SPDX-License-Identifier: MIT */
#include "../Library/ClockLib/clock.c"
#include "../Library/ClockLib/clock_config.c"
#include "../Library/Sm8450ClockLib/Sm8450Clock.c"
#include "../Target/Sm8450/CrClockTarget.c"

static UINT32 UsbClockRegisters[0xA0000U / 4U];
static BOOLEAN UsbClockCoreReset;
static ClockNode *UsbClockNode(CONST CHAR8 *Name)
{
  for (UINTN I = 0; I < ARRAY_SIZE(mGccClocks); ++I)
    if (mGccClocks[I] != NULL && mGccClocks[I]->Name != NULL &&
        strcmp(mGccClocks[I]->Name, Name) == 0) return mGccClocks[I];
  assert(FALSE);
  return NULL;
}
VOID EFIAPI MemoryFence(VOID) {}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) { *L=1; return L; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L) { assert(*L==1); *L=2; return L; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L) { assert(*L==2); *L=1; return L; }
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  assert(Address >= 0x100000U && Address < 0x1A0000U);
  return UsbClockRegisters[(Address - 0x100000U) / 4U];
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{
  assert(Address >= 0x100000U && Address < 0x1A0000U);
  UINTN Offset = Address - 0x100000U;
  if (Offset == 0x49028 || Offset == 0x49040) {
    Value &= ~BIT0; /* completed RCG update */
    if (Value & BIT1) Value &= ~BIT31;
    else Value |= BIT31;
  } else if (Offset == 0x49018 || Offset == 0x49084 || Offset == 0x49088 ||
             Offset == 0x49024 || Offset == 0x49020 || Offset == 0x9C010) {
    if (Value & BIT0) Value &= ~BIT31;
    else Value |= BIT31;
    if (Offset == 0x49024 && UsbClockCoreReset) Value |= BIT31;
  }
  UsbClockRegisters[Offset / 4U] = Value;
  return Value;
}
static VOID UsbClockFixture(VOID)
{
  memset(UsbClockRegisters, 0, sizeof(UsbClockRegisters));
  UsbClockCoreReset=FALSE;
  InitializeSpinLock(&Sm8450ClockContext.Lock);
  Sm8450ClockContext.LockInitialized=TRUE;
  /* Use target parent edges and portable ownership logic without target
     platform init, which also touches unrelated DISPCC/UFS hardware. */
  ClockNode *Nodes[] = {&GccUsbMasterSrc, &GccUsbMockSrc, &GccUsbMaster,
                       &GccUsbCfg, &GccUsbAggre, &GccUsbMock, &GccUsbSleep, &GccUsbRef};
  for (UINTN I=0; I<ARRAY_SIZE(Nodes); ++I) {
    assert(Nodes[I]->ReferenceCount==0);
    Nodes[I]->RestoreControlEnabled=FALSE;
    Nodes[I]->ActiveRateHz=0;
    if (Nodes[I]->CmdRegister != 0)
      UsbClockRegisters[Nodes[I]->CmdRegister / 4U]=BIT31;
    else UsbClockRegisters[Nodes[I]->EnableRegister / 4U]=BIT31;
  }
  /* Boot firmware leaves the UTMI gate requested, even when its source is
     stopped. Native release must restore the request after core reset. */
  UsbClockRegisters[GccUsbMock.EnableRegister / 4U] |= BIT0;
}
static EFI_STATUS UsbNativeClock(CONST CHAR8 *Name, UINT64 Rate, BOOLEAN Enable)
{
  CR_STATUS Status=ClockEnable(&Sm8450ClockContext, UsbClockNode(Name), Rate, Enable);
  if (Status==CR_BUSY) return EFI_ACCESS_DENIED;
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
