/* Real QUP owner + I2C FIFO: missing SE CPU mapping must precede all effects.
 * Modeled GCD/MMIO/providers; no hardware claim. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Pi/PiDxeCis.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#define mQupContext mTargetQupContext
#include "../Target/Sm8350/CrQupTarget.c"
#undef mQupContext
#include "../Driver/QUPCrDxe/QUPCrDxe.c"
#include "../Driver/I2CCrDxe/I2CCrDxe.c"
STATIC EFI_BOOT_SERVICES Boot;
STATIC EFI_DXE_SERVICES Dxe;
EFI_BOOT_SERVICES *gBS=&Boot;
EFI_DXE_SERVICES *gDS=&Dxe;
EFI_GUID gEfiQupCrProtocolGuid,gEfiClockCrProtocolGuid,gEfiInterconnectCrProtocolGuid,gEfiGpioCrProtocolGuid,gEfiI2cCrProtocolGuid;
CONST CR_TARGET_QUP_CONTEXT *CrDalGetQupContext(VOID){return CrTargetGetQupContext();}
VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 C,UINT32 E){UINT32 Old=*P;if(Old==C)*P=E;return Old;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
STATIC UINTN GcdCalls,IccCalls,ClockCalls,PinCalls,MmioReads,MmioWrites;
STATIC UINT32 Fault,Registers[0x4000/4];
STATIC EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  ++GcdCalls;assert(A>=0x884000&&A<0x888000);memset(D,0,sizeof(*D));
  D->BaseAddress=A&~(UINT64)EFI_PAGE_MASK;D->Length=EFI_PAGE_SIZE;
  D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;D->Capabilities=EFI_MEMORY_UC;D->Attributes=EFI_MEMORY_UC;
  if(Fault==1)return EFI_NOT_FOUND;
  if(Fault==2&&A>=0x885000)D->GcdMemoryType=EfiGcdMemoryTypeNonExistent;
  if(Fault==3)D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;
  if(Fault==4)D->Attributes=EFI_MEMORY_WB;
  if(Fault==5)D->Attributes|=EFI_MEMORY_RP;
  if(Fault==6)D->Attributes|=EFI_MEMORY_RO;
  if(Fault==7)D->Length=0;
  if(Fault==8)D->BaseAddress=A+EFI_PAGE_SIZE;
  if(Fault==9){D->BaseAddress=MAX_UINT64-EFI_PAGE_SIZE;D->Length=EFI_PAGE_SIZE*2;}
  if(Fault==10)return EFI_WARN_UNKNOWN_GLYPH;
  if(Fault==11)D->Attributes|=EFI_MEMORY_WP;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI IccAcquire(EFI_INTERCONNECT_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 S,UINT32 D,INTERCONNECT_PATH_HANDLE *H){
  (VOID)P;assert(N&&S!=D);++IccCalls;*H=S+1;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI IccVote(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H,UINT64 A,UINT64 B){
  (VOID)P;assert(H&&A==B);++IccCalls;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI IccRelease(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H){
  (VOID)P;assert(H);++IccCalls;return EFI_SUCCESS;
}
STATIC EFI_INTERCONNECT_CR_PROTOCOL Icb={.Revision=EFI_INTERCONNECT_CR_PROTOCOL_REVISION,.AcquirePath=IccAcquire,.SetBandwidth=IccVote,.ReleasePath=IccRelease};
STATIC EFI_STATUS EFIAPI ClockAcquireMock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 Rate,BOOLEAN *Owned){
  (VOID)P;assert(C&&N);(VOID)Rate;++ClockCalls;*Owned=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ClockSetMock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 R,BOOLEAN On){
  (VOID)P;assert(C&&N&&!R&&!On);++ClockCalls;return EFI_SUCCESS;
}
STATIC EFI_CLOCK_CR_PROTOCOL Clock={.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.AcquireClock=ClockAcquireMock,.SetClock=ClockSetMock};
STATIC EFI_STATUS EFIAPI Pins(EFI_GPIO_CR_PROTOCOL *P,GpioConfigParams *C){(VOID)P;assert(C);++PinCalls;return EFI_SUCCESS;}
STATIC EFI_GPIO_CR_PROTOCOL Gpio={.ConfigGpio=Pins};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){
  assert(!R);if(G==&gEfiQupCrProtocolGuid)*P=&mQupProtocol;
  else if(G==&gEfiClockCrProtocolGuid)*P=&Clock;else if(G==&gEfiGpioCrProtocolGuid)*P=&Gpio;
  else return EFI_NOT_FOUND;return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN A){
  /* The supplied log faults on this exact first GENI read when the platform
     leaves its page unmapped. No failed admission may reach it. */
  assert(!Fault&&A>=0x884000&&A<0x888000);++MmioReads;return Registers[(A-0x884000)/4];
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){
  assert(!Fault&&A>=0x884000&&A<0x888000);++MmioWrites;UINT32 O=(UINT32)(A-0x884000);
  Registers[O/4]=V;if(O==SE_M_CMD0)Registers[SE_M_IRQ_STATUS/4]=SE_M_CMD_DONE;
  if(O==SE_M_IRQ_CLEAR)Registers[SE_M_IRQ_STATUS/4]&=~V;return V;
}
UINTN EFIAPI MicroSecondDelay(UINTN N){return N;}
STATIC VOID Reboot(VOID){
  memset(mSeOwners,0,sizeof(mSeOwners));memset(Registers,0,sizeof(Registers));mQupContext=CrTargetGetQupContext();mQupIcb=&Icb;
  mFifoQup=NULL;GcdCalls=IccCalls=ClockCalls=PinCalls=MmioReads=MmioWrites=Fault=0;
  Boot.LocateProtocol=Locate;Dxe.GetMemorySpaceDescriptor=Descriptor;gDS=&Dxe;
  Registers[SE_GENI_FW_REVISION_RO/4]=3U<<8;Registers[SE_HW_PARAM_0/4]=16U<<16;
}
int main(VOID){
  UINT8 Owner;UINTN Cases=0;
  for(UINT32 Mode=QupCrSeFifo;Mode<=QupCrSeGpi;++Mode)for(UINT32 F=1;F<=13;++F){
    Reboot();Fault=F;if(F==12)gDS=NULL;if(F==13)Dxe.GetMemorySpaceDescriptor=NULL;
    assert(QupAcquireSe(&mQupProtocol,2,1,&Owner,(EFI_QUP_CR_SE_MODE)Mode)!=EFI_SUCCESS);
    assert(!IccCalls&&!ClockCalls&&!PinCalls&&!MmioReads&&!MmioWrites&&!mSeOwners[2][1].Busy);++Cases;
  }
  for(UINT32 F=1;F<=11;++F){Reboot();Fault=F;I2C_CR_INSTANCE I={0};
    I.Target=&mI2c[0];I.Protocol.Instance=15;UINT8 B=0xff;UINT32 Done=99;
    MU_I2C_SLAVE_CONFIG C={.SlaveAddress=0x2b,.BusFrequencyKHz=400,.Mode=MU_I2C_MODE_I2C};
    MU_I2C_DESCRIPTOR V={&B,1,MU_I2C_FLAG_START|MU_I2C_FLAG_WRITE|MU_I2C_FLAG_STOP};
    assert(I2cCrFifoMuTransfer(&I.Protocol,&C,&V,1,NULL,NULL,0,&Done)!=EFI_SUCCESS);
    assert(!Done&&!IccCalls&&!ClockCalls&&!PinCalls&&!MmioReads&&!MmioWrites&&!mSeOwners[2][1].Busy);++Cases;
  }
  Reboot();assert(QupAcquireSe(&mQupProtocol,2,1,&Owner,QupCrSeGpi)==EFI_SUCCESS&&GcdCalls==4&&IccCalls==6);
  assert(QupReleaseSe(&mQupProtocol,2,1,&Owner,TRUE)==EFI_SUCCESS);
  Reboot();I2C_CR_INSTANCE I={0};I.Target=&mI2c[0];I.Protocol.Instance=15;
  UINT8 B=0xff;UINT32 Done=0;MU_I2C_SLAVE_CONFIG C={.SlaveAddress=0x2b,.BusFrequencyKHz=400,.Mode=MU_I2C_MODE_I2C};
  MU_I2C_DESCRIPTOR V={&B,1,MU_I2C_FLAG_START|MU_I2C_FLAG_WRITE|MU_I2C_FLAG_STOP};
  assert(I2cCrFifoMuTransfer(&I.Protocol,&C,&V,1,NULL,NULL,0,&Done)==EFI_SUCCESS&&Done==1);
  assert(GcdCalls==4&&ClockCalls==3&&PinCalls==2&&MmioReads&&MmioWrites&&!mSeOwners[2][1].Busy);
  printf("QUP/I2C15 actual admission: %zu missing/gapped/cached/protected/malformed/warning cases denied before effects; FIFO+GPI complete mapped span PASS\n",(size_t)Cases);return 0;
}
