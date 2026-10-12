/* Production arbiter cores with bounded MMIO and IRQ ownership models.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/spmi.h>

static SpmiDeviceContext Context;
static UINTN Reads,Writes;
static CR_INTERRUPT_CONFIG *Registered;
static BOOLEAN FailDetach;
static UINT8 Pmic[4][256];
typedef struct { UINTN Address;UINT32 Value; } REG;
static REG Regs[128];static UINTN Count;
static UINT32 *Reg(UINTN Address) {
  for (UINTN I=0;I<Count;++I) if (Regs[I].Address==Address) return &Regs[I].Value;
  assert(Count<128);Regs[Count]=(REG){Address,0};return &Regs[Count++].Value;
}
static UINT32 Read(VOID *Unused,UINTN Address) { (VOID)Unused;++Reads;return *Reg(Address); }
static VOID Write(VOID *Unused,UINTN Address,UINT32 Value) {
  (VOID)Unused;++Writes;*Reg(Address)=Value;
  for (UINTN I=0;I<4;++I) {
    UINTN Stride=Context.Version>=SPMI_ARB_VERSION_8?0x200U:0x1000U;
    BOOLEAN WriteChannel=Address==Context.PmicArb.WriteAddr+Stride*I;
    BOOLEAN ReadChannel=Address==Context.PmicArb.ReadAddr+0x20U*I;
    if (WriteChannel || ReadChannel) {
      *Reg(Address+8)=1;
      UINT8 Offset=(UINT8)((Value>>4)&255),Length=(UINT8)((Value&7)+1);
      /* Model the PMIC IRQ enable aliases for actual readback qualification.
         Other transfer payloads remain explicitly injected by bounds cases. */
      if(Offset>=0x11 && Offset+Length<=0x19) {
        UINT8 Data[8]={0};
        if(WriteChannel) {
          UINT32 Low=*Reg(Address+0x10),High=*Reg(Address+0x14);
          memcpy(Data,&Low,4);memcpy(Data+4,&High,4);
          for(UINTN J=0;J<Length;J++) {
            UINT8 A=(UINT8)(Offset+J);
            if(A==0x14)Pmic[I][0x18]&=(UINT8)~Data[J];
            else if(A==0x15)Pmic[I][0x15]|=Data[J];
            else if(A==0x16)Pmic[I][0x15]&=(UINT8)~Data[J];
            else Pmic[I][A]=Data[J];
          }
        }else {
          memcpy(Data,&Pmic[I][Offset],Length);
          memcpy(Reg(Address+0x18),Data,4);memcpy(Reg(Address+0x1c),Data+4,4);
        }
      }
    }
  }
}
static VOID Stall(VOID *Unused,UINT32 Us) { (VOID)Unused;(VOID)Us; }
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A) { return Read(NULL,A); }
UINT32 CrMmioWrite32(UINTN A,UINT32 V) { Write(NULL,A,V);return V; }
VOID KeInitializeSpinLock(KSPIN_LOCK *L) { *L=1; }
VOID KeAcquireSpinLock(KSPIN_LOCK *L,KIRQL *Irql) { assert(*L==1);*L=2;*Irql=PASSIVE_LEVEL; }
VOID KeReleaseSpinLock(KSPIN_LOCK *L,KIRQL Irql) { assert(*L==2 && Irql==PASSIVE_LEVEL);*L=1; }
KIRQL KeGetCurrentIrql(VOID) { return PASSIVE_LEVEL; }
VOID KeStallExecutionProcessor(ULONG Us) { (VOID)Us; }
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE M,BOOLEAN A,LARGE_INTEGER *I)
{ (void)M;(void)A;(void)I;return STATUS_SUCCESS; }
#else
UINT32 EFIAPI MmioRead32(UINTN A) { return Read(NULL,A); }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { Write(NULL,A,V);return V; }
UINTN EFIAPI MicroSecondDelay(UINTN Us) { return Us; }
VOID EFIAPI MemoryFence(VOID) {}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) { (VOID)Level;(VOID)Format; }
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L) { *L=1;return L; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L) { assert(*L==1);*L=2;return L; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L) { assert(*L==2);*L=1;return L; }
VOID *EFIAPI SetMem(VOID *B,UINTN L,UINT8 V) { return memset(B,V,L); }
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B) { return strcmp(A,B); }
#endif
CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  assert(Registered==NULL);Registered=Config;return CR_SUCCESS;
}
BOOLEAN CrInterruptIsRegistered(CR_INTERRUPT_CONFIG *Config) { return Registered==Config; }
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  assert(Registered==Config);
  if (FailDetach) return CR_DEVICE_ERROR;
  Registered=NULL;return CR_SUCCESS;
}
static void Setup(void) {
  memset(&Context,0,sizeof(Context));memset(Pmic,0,sizeof(Pmic));Count=Reads=Writes=0;Registered=NULL;FailDetach=FALSE;
  for (UINTN I=0;I<5;++I)
    Context.Regions[I]=(SpmiMemoryRegion){(UINTN)0x12300000000ULL+I*0x1000000U,0x400000U};
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CORE].Size=0x3000;
  Context.Regions[SPMI_MEMORY_REGION_TYPE_INTERRUPT].Size=0x10000;
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].Size=0x10000;
  *Reg(Context.Regions[0].BaseAddress)=0x70010000U;
  *Reg(Context.Regions[0].BaseAddress+4)=4;
  for (UINTN I=0;I<4;++I) *Reg(Context.Regions[0].BaseAddress+0x2000+4U*I)=
    ((0x13U+(UINT32)I)<<8)|BIT(24);
  Context.Io=(SpmiIoOps){Read,Write,Stall,NULL};Context.HasInterrupt=TRUE;
}
static void Initialize(void) {
  SpmiDeviceContext *C=&Context;assert(SpmiLibInit(&C)==CR_SUCCESS);
  assert(Context.Initialized && Context.InterruptRegistered && Registered!=NULL);
}
static void Bounds(void) {
  Setup();Context.Regions[0].BaseAddress=(UINTN)-4;Context.Regions[0].Size=8;
  SpmiDeviceContext *C=&Context;
  assert(SpmiLibInit(&C)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.Regions[0].BaseAddress++;
  assert(SpmiLibInit(&C)==CR_INVALID_PARAMETER && Reads==0 && Writes==0);
  Setup();Context.Regions[0].Size=0x200f;
  assert(SpmiLibInit(&C)==CR_INVALID_PARAMETER && Reads==2 && Writes==0);
  Setup();Context.Regions[SPMI_MEMORY_REGION_TYPE_CONFIG].Size=15;
  assert(SpmiLibInit(&C)==CR_INVALID_PARAMETER && Reads==2 && Writes==0);
  Setup();Context.BusId=1;Context.Regions[0].Size=8;
  assert(SpmiLibInit(&C)==CR_INVALID_PARAMETER && Reads==1 && Writes==0);
  Setup();Initialize();
  UINT8 Buffer[8]={0};
  UINTN W=Writes,R=Reads;
  Context.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].Size=0x3f;
  assert(SpmiRead(&Context,0,0x1410,Buffer,8)==CR_INVALID_PARAMETER && Writes==W && Reads==R);
  for (UINTN I=0;I<8;++I) assert(Buffer[I]==0);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].Size=0x40;
  *Reg(Context.PmicArb.ReadAddr+0x20+0x18)=0x12345678U;
  *Reg(Context.PmicArb.ReadAddr+0x20+0x1c)=0x9abcdef0U;
  assert(SpmiRead(&Context,0,0x1410,Buffer,8)==CR_SUCCESS);
  assert(Buffer[0]==0x78 && Buffer[3]==0x12 && Buffer[4]==0xf0 && Buffer[7]==0x9a);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].Size=0x1017;W=Writes;R=Reads;
  assert(SpmiWrite(&Context,0,0x1410,Buffer,8)==CR_INVALID_PARAMETER && Writes==W && Reads==R);
  assert(SpmiMaskedWrite(&Context,0,0x1410,Buffer,Buffer,8)==CR_INVALID_PARAMETER && Writes==W && Reads==R);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].Size=0x400000;
  Context.Bus.PpidToApid[0x14]=(UINT16)SPMI_PMIC_ARB_APID_VALID|8191U;
  assert(SpmiRead(&Context,0,0x1410,Buffer,8)==CR_INVALID_PARAMETER && Writes==W && Reads==R);
  Context.Bus.PpidToApid[0x14]=(UINT16)SPMI_PMIC_ARB_APID_VALID|1U;
  Context.Bus.MinApid=0;Context.Bus.MaxApid=8191;
  assert(SpmiDispatchIrqs(&Context)==CR_INVALID_PARAMETER && Writes==W && Reads==R);
  Context.Bus.MinApid=4;Context.Bus.MaxApid=0;
  assert(SpmiLibDeinit(&Context)==CR_SUCCESS && !Context.Initialized && Registered==NULL);
}
#ifndef SPMI_BOUNDS_ONLY
static VOID Handler(VOID *Param) { assert(Param==&Context); }
static void IrqOwnership(void) {
  Setup();Initialize();UINTN W=Writes,R=Reads;
  assert(SpmiRegisterIrq(&Context,0,0x13,0,CR_INTERRUPT_TRIGGER_TYPE_INVALID,Handler,&Context)==CR_INVALID_PARAMETER);
  assert(Writes==W && Reads==R);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].Size=0x10b;
  assert(SpmiRegisterIrq(&Context,0,0x13,0,CR_INTERRUPT_TRIGGER_EDGE_BOTH,Handler,&Context)==CR_INVALID_PARAMETER);
  assert(Writes==W && Reads==R);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_SLAVES].Size=0x400000;
  UINTN Size=Context.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].Size;
  Context.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].Size=0x1b;
  assert(SpmiRegisterIrq(&Context,0,0x13,0,CR_INTERRUPT_TRIGGER_EDGE_BOTH,Handler,&Context)==CR_INVALID_PARAMETER);
  assert(Writes==W && Reads==R);
  Context.Regions[SPMI_MEMORY_REGION_TYPE_OBSERVER].Size=Size;
  assert(SpmiRegisterIrq(&Context,0,0x13,0,CR_INTERRUPT_TRIGGER_EDGE_BOTH,Handler,&Context)==CR_SUCCESS);
  assert(SpmiEnableIrq(&Context,0,0x13,0,TRUE)==CR_SUCCESS);
  W=Writes;FailDetach=TRUE;
  assert(SpmiLibDeinit(&Context)==CR_DEVICE_ERROR);
  assert(Context.Initialized && Context.InterruptRegistered && Context.IrqEntries[0].Handler==Handler && Writes==W);
  FailDetach=FALSE;
  assert(SpmiSetInterruptDelivery(&Context,FALSE)==CR_SUCCESS && Context.Initialized && !Context.InterruptRegistered);
  assert(Context.IrqEntries[0].Handler==Handler);
  assert(SpmiSetInterruptDelivery(&Context,TRUE)==CR_SUCCESS && Context.InterruptRegistered);
  assert(SpmiLibDeinit(&Context)==CR_SUCCESS && !Context.Initialized && Registered==NULL);
}
#endif
static void VersionEight(void) {
  Setup();SpmiDeviceContext *C=&Context;
  *Reg(Context.Regions[0].BaseAddress)=0x80010000U;
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_MAP]=(SpmiMemoryRegion){(UINTN)0x12305000000ULL,0x10000};
  Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_OWNER]=(SpmiMemoryRegion){(UINTN)0x12306000000ULL,0x10000};
  for (UINTN I=0;I<4;++I) *Reg(Context.Regions[SPMI_MEMORY_REGION_TYPE_CH_MAP].BaseAddress+4U*I)=
    (0x13U+(UINT32)I)|BIT(31);
  assert(SpmiLibInit(&C)==CR_SUCCESS && Context.Version==SPMI_ARB_VERSION_8);
  UINT8 Data[8]={0};*Reg(Context.PmicArb.ReadAddr+0x20+0x18)=0x12345678U;
  assert(SpmiRead(&Context,0,0x1410,Data,8)==CR_SUCCESS && Data[0]==0x78);
  assert(SpmiWrite(&Context,0,0x1410,Data,8)==CR_SUCCESS);
  assert(SpmiLibDeinit(&Context)==CR_SUCCESS);
}
int main(void) {
  Bounds();VersionEight();
#ifndef SPMI_BOUNDS_ONLY
  IrqOwnership();
#endif
  puts("SPMI: complete transfer/APID/IRQ bounds, no-MMIO rejection and IRQ-first retained teardown passed");
  return 0;
}
