/* Kernel API-shape regression for adapter-owned mappings and PDC bounds.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ntddk.h>
#include "../Library/ClockLib/clock.c"
#include "../Library/ClockLib/clock_config.c"
#include "../Library/PdcLib/pdc.c"

static unsigned LockInitializations;
static UINT32 Registers[512];
static unsigned PdcWrites;
UINT32 CrMmioRead32(UINTN Address) {
  assert(Address >= 0x1000 && Address < 0x1800 && (Address & 3) == 0);
  return Registers[(Address-0x1000)/4];
}
UINT32 CrMmioWrite32(UINTN Address, UINT32 Value) {
  assert(Address >= 0x1000 && Address < 0x1800 && (Address & 3) == 0);
  ++PdcWrites;
  return Registers[(Address-0x1000)/4] = Value;
}
VOID KeInitializeSpinLock(KSPIN_LOCK *Lock) {
  *Lock = 0;
  ++LockInitializations;
}

int main(void) {
  ClockController WindowsMappings[2] = {
    {.Address = 0x100000, .Size = 0x1000, .MemMapped = TRUE},
    {.Address = 0x200000, .Size = 0x2000, .MemMapped = TRUE}
  };
  ClockDriverContext Clock = {
    .ClockControllerCount = 2, .ClockControllers = WindowsMappings
  };
  ClockDriverContext *Owner = &Clock;
  assert(ClockLibInit(NULL) == CR_INVALID_PARAMETER);
  Owner = NULL;
  assert(ClockLibInit(&Owner) == CR_INVALID_PARAMETER);
  Owner = &Clock;
  WindowsMappings[1].MemMapped = FALSE;
  assert(ClockLibInit(&Owner) == CR_NOT_READY);
  assert(!Clock.LockInitialized && LockInitializations == 0);
  WindowsMappings[1].MemMapped = TRUE;
  WindowsMappings[1].Address = 0;
  assert(ClockLibInit(&Owner) == CR_NOT_READY);
  WindowsMappings[1].Address = 0x200000;
  assert(ClockLibInit(&Owner) == CR_SUCCESS);
  assert(Clock.LockInitialized && LockInitializations == 1);
  assert(ClockLibInit(&Owner) == CR_SUCCESS);
  assert(LockInitializations == 1);
  assert(ClockDeinit(NULL) == CR_INVALID_PARAMETER);
  assert(ClockDeinit(&Clock) == CR_SUCCESS);
  /* The adapter still owns both mappings after core teardown. No native GCD
     mapping functions are linked into this kernel executable. */
  assert(WindowsMappings[0].MemMapped && WindowsMappings[1].MemMapped);
  assert(WindowsMappings[0].Address == 0x100000);
  assert(WindowsMappings[1].Address == 0x200000);

  PdcDeviceContext Pdc = {.PdcRangeCount = 2,
    .PdcRegion = {{4, 20, 3}, {40, 200, 2}}};
  UINT16 Result = 0xeeee;
  assert(PdcGetGicIrqFromPdcPin(NULL, 4, &Result) == CR_INVALID_PARAMETER);
  assert(PdcGetGicIrqFromPdcPin(&Pdc, 4, NULL) == CR_INVALID_PARAMETER);
  assert(PdcGetPdcPinFromGicIrq(&Pdc, 52, NULL) == CR_INVALID_PARAMETER);
  assert(PdcGetGicIrqFromPdcPin(&Pdc, 6, &Result) == CR_SUCCESS && Result == 54);
  assert(PdcGetPdcPinFromGicIrq(&Pdc, 233, &Result) == CR_SUCCESS && Result == 41);
  assert(PdcGetGicIrqFromPdcPin(&Pdc, 7, &Result) == CR_NOT_FOUND);
  assert(PdcGetPdcPinFromGicIrq(&Pdc, 234, &Result) == CR_NOT_FOUND);
  Pdc.PdcRangeCount = PDC_RANGE_COUNT_MAX + 1;
  Result = 0xeeee;
  assert(PdcGetGicIrqFromPdcPin(&Pdc, 4, &Result) == CR_INVALID_PARAMETER);
  assert(PdcGetPdcPinFromGicIrq(&Pdc, 52, &Result) == CR_INVALID_PARAMETER);
  assert(Result == 0xeeee);
  PdcDeviceContext *PdcOwner = &Pdc;
  assert(PdcLibInit(&PdcOwner) == CR_INVALID_PARAMETER && PdcWrites == 0);
  Pdc.BaseAddress = 0x1000; Pdc.Size = sizeof(Registers); Pdc.PdcRangeCount = PDC_RANGE_COUNT_MAX;
  for (UINT16 I=0; I<PDC_RANGE_COUNT_MAX; ++I) Pdc.PdcRegion[I] = (PdcRange){I,I,1};
  Registers[4] = 0xffffffff;
  assert(PdcLibInit(&PdcOwner) == CR_SUCCESS && PdcWrites == 32 && Registers[4] == 0);
  // A short advertised table ignores stale nonzero entries after its count.
  Pdc.PdcRangeCount = 1; PdcWrites = 0; Registers[4] = 0xffffffff;
  assert(PdcLibInit(&PdcOwner) == CR_SUCCESS && PdcWrites == 1 && Registers[4] == 0xfffffffe);
  Pdc.PdcRangeCount = 2; Pdc.PdcRegion[1].GicSpiBase = 988; PdcWrites = 0;
  assert(PdcLibInit(&PdcOwner) == CR_INVALID_PARAMETER && PdcWrites == 0);
  Pdc.PdcRangeCount = 1; Pdc.Size = 0x10;
  assert(PdcLibInit(&PdcOwner) == CR_BAD_BUFFER_SIZE && PdcWrites == 0);
  Pdc.PdcRangeCount=1; Pdc.PdcRegion[0]=(PdcRange){4,20,3}; Pdc.Size=0x400;
  Pdc.SPIConfigBase=0x1600; Pdc.SPIConfigSize=4;
  CR_INTERRUPT_TRIGGER_TYPE GicType=CR_INTERRUPT_TRIGGER_LEVEL_LOW;
  Registers[0x120/4]=0xa0;
  assert(PdcGicSetType(&Pdc,4,CR_INTERRUPT_TRIGGER_EDGE_FALLING,&GicType)==CR_SUCCESS);
  assert(GicType==CR_INTERRUPT_TRIGGER_EDGE_RISING && Registers[0x120/4]==0xa2);
  PdcWrites=0;Pdc.SPIConfigSize=0;
  assert(PdcGicSetType(&Pdc,4,CR_INTERRUPT_TRIGGER_LEVEL_LOW,&GicType)==CR_BAD_BUFFER_SIZE);
  assert(PdcWrites==0 && GicType==CR_INTERRUPT_TRIGGER_EDGE_RISING);
  Pdc.SPIConfigSize=4;
  assert(PdcGicSetType(&Pdc,4,CR_INTERRUPT_TRIGGER_LEVEL_LOW,&GicType)==CR_SUCCESS);
  assert(GicType==CR_INTERRUPT_TRIGGER_LEVEL_HIGH && Registers[0x120/4]==0xa0);
  assert(Registers[0x600/4]==(1U<<20));
  PdcWrites=0;
  assert(PdcEnableInterrupt(&Pdc,7,TRUE)==CR_NOT_FOUND && PdcWrites==0);
  assert(PdcCheckInterruptEnabled(&Pdc,4,NULL)==CR_INVALID_PARAMETER);
  Pdc.Size=0x120;
  assert(PdcGicSetType(&Pdc,4,CR_INTERRUPT_TRIGGER_EDGE_RISING,NULL)==CR_BAD_BUFFER_SIZE);
  assert(PdcWrites==0);
  puts("Kernel clock mappings remain adapter-owned; PDC boundaries/null outputs passed");
  return 0;
}
