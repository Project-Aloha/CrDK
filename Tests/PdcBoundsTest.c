/* Production PDC route/mapping preflight, native and NT.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/pdc.h>

static UINTN Reads, Writes, Addresses[512];
static UINT32 Values[512];
static UINTN Count;
static UINT32 *Reg(UINTN Address) {
  for (UINTN I=0;I<Count;++I) if (Addresses[I]==Address) return &Values[I];
  assert(Count<512);Addresses[Count]=Address;Values[Count]=0;return &Values[Count++];
}
static UINT32 Read(UINTN Address) { ++Reads;return *Reg(Address); }
static UINT32 Write(UINTN Address,UINT32 Value) { ++Writes;return *Reg(Address)=Value; }
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A) { return Read(A); }
UINT32 CrMmioWrite32(UINTN A,UINT32 V) { return Write(A,V); }
#else
UINT32 EFIAPI MmioRead32(UINTN A) { return Read(A); }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { return Write(A,V); }
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...) { (VOID)L;(VOID)F; }
#endif
static PdcDeviceContext Context;
static void Setup(void) {
  memset(&Context,0,sizeof(Context));Reads=Writes=Count=0;
  Context.BaseAddress=(UINTN)0x12300000000ULL;Context.Size=0x30000;
  Context.SPIConfigBase=(UINTN)0x12400000000ULL;Context.SPIConfigSize=0x64;
  Context.PdcRangeCount=2;
  Context.PdcRegion[0]=(PdcRange){0,480,12};
  Context.PdcRegion[1]=(PdcRange){40,520,54};
}
int main(void) {
  PdcDeviceContext *Owner=&Context;
  UINT16 Output;
  BOOLEAN Enabled;
  CR_INTERRUPT_TRIGGER_TYPE Type;
  Setup();Context.BaseAddress=(UINTN)-4;Context.Size=0x30000;
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.BaseAddress++;
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.Size=0x30001;
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.PdcRegion[1]=(PdcRange){8,600,4};
  Output=0xabcd;
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  assert(PdcGetGicIrqFromPdcPin(&Context,1,&Output)==CR_INVALID_PARAMETER && Output==0xabcd);
  assert(PdcGetPdcPinFromGicIrq(&Context,513,&Output)==CR_INVALID_PARAMETER && Output==0xabcd);
  Setup();Context.PdcRegion[1]=(PdcRange){40,484,4};
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.PdcRegion[1]=(PdcRange){0xfffe,520,4};
  assert(PdcLibInit(&Owner)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.PdcRegion[1]=(PdcRange){40,987,2};
  assert(PdcGetGicIrqFromPdcPin(&Context,1,&Output)==CR_INVALID_PARAMETER && Output==0xabcd);
  Setup();Context.Size=0x120;
  assert(PdcLibInit(&Owner)==CR_BAD_BUFFER_SIZE && Reads==0 && Writes==0);
  Setup();Context.SPIConfigBase++;
  Type=CR_INTERRUPT_TRIGGER_LEVEL_LOW;
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH,&Type)==CR_INVALID_PARAMETER);
  assert(Type==CR_INTERRUPT_TRIGGER_LEVEL_LOW && Reads==0 && Writes==0);
  Setup();Context.SPIConfigBase=(UINTN)-4;
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH,&Type)==CR_INVALID_PARAMETER);
  assert(Reads==0 && Writes==0);
  Setup();Context.SPIConfigSize=0x3c;
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH,&Type)==CR_BAD_BUFFER_SIZE);
  assert(Reads==0 && Writes==0);
  Setup();
  assert(PdcEnableInterrupt(&Context,1,2)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_TYPE_INVALID,&Type)==CR_INVALID_PARAMETER);
  assert(Reads==0 && Writes==0 && Type==CR_INTERRUPT_TRIGGER_LEVEL_LOW);
  *Reg(Context.BaseAddress+0x10)=0xffffffffU;
  assert(PdcLibInit(&Owner)==CR_SUCCESS && Writes==66);
  assert(*Reg(Context.BaseAddress+0x10)==0xfffff000U);
  assert(PdcGetGicIrqFromPdcPin(&Context,1,&Output)==CR_SUCCESS && Output==513);
  assert(PdcGetPdcPinFromGicIrq(&Context,513,&Output)==CR_SUCCESS && Output==1);
  assert(PdcEnableInterrupt(&Context,1,TRUE)==CR_SUCCESS);
  assert(PdcCheckInterruptEnabled(&Context,1,&Enabled)==CR_SUCCESS && Enabled);
  *Reg(Context.BaseAddress+0x114)=0xa0U;
  *Reg(Context.SPIConfigBase+60)=0x80000000U;
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_LEVEL_LOW,&Type)==CR_SUCCESS);
  assert(Type==CR_INTERRUPT_TRIGGER_LEVEL_HIGH && *Reg(Context.BaseAddress+0x114)==0xa0U);
  assert(*Reg(Context.SPIConfigBase+60)==0x80000002U);
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_EDGE_BOTH,&Type)==CR_SUCCESS);
  assert(Type==CR_INTERRUPT_TRIGGER_EDGE_RISING && *Reg(Context.BaseAddress+0x114)==0xa7U);
  assert(*Reg(Context.SPIConfigBase+60)==0x80000000U);
  Context.SPIConfigSize=0x3c;
  UINTN OldWrites=Writes,OldReads=Reads;
  assert(PdcGicSetType(&Context,1,CR_INTERRUPT_TRIGGER_EDGE_RISING,&Type)==CR_BAD_BUFFER_SIZE);
  assert(Writes==OldWrites && Reads==OldReads);
  /* Last valid word in address space is legal; one extra byte wraps. */
  Setup();Context.BaseAddress=(UINTN)-0x14;Context.Size=0;
  assert(PdcEnableInterrupt(&Context,1,FALSE)==CR_SUCCESS);
  assert(Addresses[0]==(UINTN)-4 && Writes==1);
  puts("PDC native/NT: full mapping/route preflight, overlap rejection and high-VA IRQ513 passed");
  return 0;
}
