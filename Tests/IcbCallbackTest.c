/* Actual ICC adapter/core + RPMh + QUP/I2C15 composition.
 * TPL scheduler, IRQ/MMIO/CmdDB/clock/pin boundaries are modeled; no hardware.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#define NULL ((void *)0)
#include <Pi/PiDxeCis.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiLib.h>
#ifndef CR_ICB_DXE_SOURCE
#define CR_ICB_DXE_SOURCE "../Driver/ICBCrDxe/ICBCrDxe.c"
#endif
#include CR_ICB_DXE_SOURCE
#define mQupContext mTargetQupContext
#include "../Target/Sm8350/CrQupTarget.c"
#undef mQupContext
#include "../Driver/QUPCrDxe/QUPCrDxe.c"
#include "../Driver/I2CCrDxe/I2CCrDxe.c"

#define RPMH_BASE 0x18220000U
#define TCS_BASE (RPMH_BASE+0xd00U)
#define TCS_STRIDE 0x2a0U
STATIC EFI_BOOT_SERVICES Boot;
STATIC EFI_DXE_SERVICES Dxe;
EFI_BOOT_SERVICES *gBS=&Boot;
EFI_DXE_SERVICES *gDS=&Dxe;
EFI_GUID gEfiQupCrProtocolGuid,gEfiClockCrProtocolGuid,gEfiInterconnectCrProtocolGuid;
EFI_GUID gEfiGpioCrProtocolGuid,gEfiI2cCrProtocolGuid,gEfiCmdDBCrProtocolGuid;
STATIC CONST InterconnectTargetContext *Target;
STATIC RpmhDeviceContext Rpmh;
STATIC CR_DAL_DEVICE_INFO Info;
STATIC CR_INTERRUPT_CONFIG *Interrupt;
STATIC EFI_TPL CurrentTpl=TPL_APPLICATION;
STATIC BOOLEAN Pending,ArmCallback,InsideCallback,ArmRecursive;
STATIC UINT32 CallbackCount,IrqCount,Triggers,RecursiveCount,Hardware[INTERCONNECT_MAX_BCMS];
STATIC UINT32 SeRegisters[0x4000/4],RpmhRegisters[0x10000/4];
STATIC INTERCONNECT_PATH_HANDLE DisplayPath;

VOID *EFIAPI AllocateZeroPool(UINTN N){return calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 C,UINT32 E){UINT32 O=*P;if(O==C)*P=E;return O;}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L){*L=1;return L;}
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L){assert(*L==1 && "ICC callback reentered an owned spin lock");*L=2;return L;}
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L){assert(*L==2);*L=1;return L;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return TRUE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
UINTN EFIAPI MicroSecondDelay(UINTN N){return N;}
VOID CrTestSleep(UINT64 N){(VOID)N;}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return CurrentTpl;}
STATIC VOID DispatchCallback(VOID){
  if(!Pending||CurrentTpl>=TPL_CALLBACK||InsideCallback)return;
  Pending=FALSE;InsideCallback=TRUE;EFI_TPL Saved=CurrentTpl;CurrentTpl=TPL_CALLBACK;
  I2C_CR_INSTANCE I={0};I.Target=&mI2c[0];I.Protocol.Instance=15;
  UINT8 B=0xff;UINT32 Done=0;
  MU_I2C_SLAVE_CONFIG C={.SlaveAddress=0x2b,.BusFrequencyKHz=400,.Mode=MU_I2C_MODE_I2C};
  MU_I2C_DESCRIPTOR V={&B,1,MU_I2C_FLAG_START|MU_I2C_FLAG_WRITE|MU_I2C_FLAG_STOP};
  assert(I2cCrFifoMuTransfer(&I.Protocol,&C,&V,1,NULL,NULL,0,&Done)==EFI_SUCCESS&&Done==1);
  assert(!mSeOwners[2][1].Busy);++CallbackCount;CurrentTpl=Saved;InsideCallback=FALSE;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=T;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL T){assert(T<=CurrentTpl);CurrentTpl=T;DispatchCallback();}
STATIC UINT16 Bcm(CONST CHAR8 *N){for(UINT16 I=0;I<Target->BcmCount;++I)if(!strcmp(Target->Bcms[I].Name,N))return I;assert(0);return 0;}
STATIC EFI_STATUS EFIAPI Address(EFI_CMD_DB_PROTOCOL *P,CONST CHAR8 *N,UINT32 *A){(VOID)P;*A=0x100+4U*Bcm(N);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Aux(EFI_CMD_DB_PROTOCOL *P,CONST CHAR8 *N,UINT8 *D,UINT32 *S){
  (VOID)P;assert(*S>=8);UINT16 I=Bcm(N);memset(D,0,8);
  if(!Target->Bcms[I].EnableMask){D[0]=0xe8;D[1]=3;D[4]=1;}D[6]=(UINT8)(I%4);*S=8;return EFI_SUCCESS;
}
STATIC EFI_CMD_DB_PROTOCOL CmdDb={.GetEntryAddressByName=Address,.GetAuxDataByName=Aux};
CONST CR_TARGET_QUP_CONTEXT *CrDalGetQupContext(VOID){return CrTargetGetQupContext();}
CrDebugUartContext *CrDalGetDebugUartContext(VOID){return NULL;}
CONST InterconnectTargetContext *CrDalGetInterconnectContext(VOID){return Target;}
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE T,UINT32 I,UINT32 R,UINTN S,CONST CR_DAL_DEVICE_INFO **O){
  assert(T==CrDalDeviceRpmh&&!I&&R==CR_DAL_DATA_REVISION_1&&S==sizeof(Rpmh));Info.Data=&Rpmh;*O=&Info;return EFI_SUCCESS;
}
CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *C){assert(!Interrupt);Interrupt=C;return CR_SUCCESS;}
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *C){assert(Interrupt==C);Interrupt=NULL;return CR_SUCCESS;}
STATIC EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  assert(A>=0x884000&&A<0x888000);memset(D,0,sizeof(*D));D->BaseAddress=A&~(UINT64)EFI_PAGE_MASK;
  D->Length=EFI_PAGE_SIZE;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;D->Capabilities=D->Attributes=EFI_MEMORY_UC;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ClockAcquireMock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 R,BOOLEAN *O){
  (VOID)P;(VOID)R;assert(C&&N);*O=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ClockSetMock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 R,BOOLEAN On){
  (VOID)P;assert(C&&N&&!R&&!On);return EFI_SUCCESS;
}
STATIC EFI_CLOCK_CR_PROTOCOL Clock={.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.AcquireClock=ClockAcquireMock,.SetClock=ClockSetMock};
STATIC EFI_STATUS EFIAPI Pins(EFI_GPIO_CR_PROTOCOL *P,GpioConfigParams *C){(VOID)P;assert(C);return EFI_SUCCESS;}
STATIC EFI_GPIO_CR_PROTOCOL Gpio={.ConfigGpio=Pins};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){
  assert(!R);if(G==&gEfiCmdDBCrProtocolGuid)*P=&CmdDb;
  else if(G==&gEfiQupCrProtocolGuid)*P=&mQupProtocol;
  else if(G==&gEfiClockCrProtocolGuid)*P=&Clock;else if(G==&gEfiGpioCrProtocolGuid)*P=&Gpio;
  else if(G==&gEfiInterconnectCrProtocolGuid)*P=&mInterconnectProtocol;else return EFI_NOT_FOUND;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){(VOID)H;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){(VOID)H;return EFI_SUCCESS;}
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=0x884000&&A<0x888000);return SeRegisters[(A-0x884000)/4];}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){
  assert(A>=0x884000&&A<0x888000);UINT32 O=(UINT32)(A-0x884000);SeRegisters[O/4]=V;
  if(O==SE_M_CMD0)SeRegisters[SE_M_IRQ_STATUS/4]=SE_M_CMD_DONE;
  if(O==SE_M_IRQ_CLEAR)SeRegisters[SE_M_IRQ_STATUS/4]&=~V;return V;
}
UINT32 CrMmioRead32(UINTN A){assert(A>=RPMH_BASE&&A<RPMH_BASE+sizeof RpmhRegisters);return RpmhRegisters[(A-RPMH_BASE)/4];}
UINT32 CrMmioWrite32(UINTN A,UINT32 V){
  assert(A>=RPMH_BASE&&A<RPMH_BASE+sizeof RpmhRegisters);
  if(A==TCS_BASE+8){RpmhRegisters[(TCS_BASE+4-RPMH_BASE)/4]&=~V;return V;}
  RpmhRegisters[(A-RPMH_BASE)/4]=V;
  for(UINT32 T=0;T<2;++T){UINTN Base=TCS_BASE+T*TCS_STRIDE;
    if(A!=Base+0x14||(V&(BIT(16)|BIT(24)))!=(BIT(16)|BIT(24)))continue;
    ++Triggers;
    if(ArmCallback){ArmCallback=FALSE;Pending=TRUE;DispatchCallback();}
    if(ArmRecursive){ArmRecursive=FALSE;UINT32 Before=Triggers;
      assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,99,99)==EFI_NOT_READY);
      assert(Triggers==Before);++RecursiveCount;
    }
    UINT32 Enabled=CrMmioRead32(Base+0x1c);
    for(UINT32 I=0;I<16;++I)if(Enabled&BIT(I)){
      UINT32 AddressValue=CrMmioRead32(Base+0x34+I*0x14),Data=CrMmioRead32(Base+0x38+I*0x14);
      assert(AddressValue>=0x100&&(AddressValue-0x100)/4<Target->BcmCount);
      Hardware[(AddressValue-0x100)/4]=Data&0x0fffffff;
      RpmhRegisters[(Base+0x3c+I*0x14-RPMH_BASE)/4]=BIT(8)|BIT(16);
    }
    RpmhRegisters[(TCS_BASE+4-RPMH_BASE)/4]|=(UINT32)BIT(T);
    /* Actual RPMh ISR retires atomically, including while ICC owns its lock
       and notification delivery is suppressed. IRQs are not TPL callbacks. */
    assert(Interrupt&&Interrupt->Handler);++IrqCount;Interrupt->Handler(Interrupt->Param);
  }return V;
}
STATIC INTERCONNECT_PATH_HANDLE Acquire(CONST CHAR8 *P,UINT32 S,UINT64 Bw){
  INTERCONNECT_PATH_HANDLE H=0;assert(mInterconnectProtocol.AcquirePath(&mInterconnectProtocol,P,S,S+1,&H)==EFI_SUCCESS);
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,H,Bw,Bw)==EFI_SUCCESS);return H;
}
int main(int Argc,char **Argv){
  Target=CrTargetGetInterconnectContext();
  Boot.RaiseTPL=Raise;Boot.RestoreTPL=Restore;Boot.LocateProtocol=Locate;
  Boot.InstallMultipleProtocolInterfaces=Install;Boot.UninstallMultipleProtocolInterfaces=Uninstall;Dxe.GetMemorySpaceDescriptor=Descriptor;
  Rpmh.drv_base_address=RPMH_BASE;Rpmh.drv_size=sizeof RpmhRegisters;Rpmh.drv_id=2;Rpmh.tcs_offset=0xd00;
  Rpmh.tcs_config.active_tcs.tcs_count=2;Rpmh.tcs_config.sleep_tcs.tcs_count=3;Rpmh.tcs_config.wake_tcs.tcs_count=3;
  RpmhRegisters[0]=2U<<16|3U<<8;RpmhRegisters[0xc/4]=(8U<<12)|(16U<<27);
  SeRegisters[SE_GENI_FW_REVISION_RO/4]=3U<<8;SeRegisters[SE_HW_PARAM_0/4]=16U<<16;
  assert(InterconnectEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  mQupContext=CrTargetGetQupContext();mQupIcb=&mInterconnectProtocol;
  INTERCONNECT_PATH_HANDLE Ufs=Acquire("qcom,sm8350-ufs",0x3000,50000),Usb=Acquire("qcom,sm8350-dwc3",0x6000,70000);
  UINT32 Baseline[INTERCONNECT_MAX_BCMS];memcpy(Baseline,Hardware,sizeof Baseline);
  DisplayPath=Acquire("qcom,sm8350-mdp",0x4000,1);
  BOOLEAN CallbackCaller=Argc>1&&!strcmp(Argv[1],"callback");if(CallbackCaller)CurrentTpl=TPL_CALLBACK;
  ArmCallback=TRUE;
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,100000,100000)==EFI_SUCCESS);
  assert(CurrentTpl==(CallbackCaller?TPL_CALLBACK:TPL_APPLICATION));
  if(CallbackCaller){assert(Pending&&!CallbackCount);Restore(TPL_APPLICATION);}
  assert(CallbackCount==1&&!Pending&&IrqCount==Triggers&&Rpmh.TcsBusy==0);
  ArmRecursive=TRUE;
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,200000,200000)==EFI_SUCCESS&&RecursiveCount==1);
  /* The display reset retains its private handle while retiring just that
     client's bandwidth. Storage and USB must retain exactly their baseline. */
  UINT32 ActiveDisplay[INTERCONNECT_MAX_BCMS];memcpy(ActiveDisplay,Hardware,sizeof ActiveDisplay);
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,0,0)==EFI_SUCCESS);
  assert(!memcmp(Hardware,Baseline,sizeof Baseline)&&mInterconnect->Paths[DisplayPath-1].InUse);
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,200000,200000)==EFI_SUCCESS);
  assert(!memcmp(Hardware,ActiveDisplay,sizeof ActiveDisplay));
  UINT32 Before=Triggers;CurrentTpl=TPL_HIGH_LEVEL;
  assert(mInterconnectProtocol.SetBandwidth(&mInterconnectProtocol,DisplayPath,3,3)==EFI_ACCESS_DENIED&&Triggers==Before);
  CurrentTpl=TPL_APPLICATION;
  assert(mInterconnectProtocol.SetBandwidth(NULL,DisplayPath,3,3)==EFI_INVALID_PARAMETER&&Triggers==Before);
  assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,DisplayPath)==EFI_SUCCESS);
  assert(!memcmp(Hardware,Baseline,sizeof Baseline));
  DisplayPath=Acquire("qcom,sm8350-mdp",0x4000,400000);
  assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,DisplayPath)==EFI_SUCCESS&&!memcmp(Hardware,Baseline,sizeof Baseline));
  assert(mInterconnect->Paths[Ufs-1].InUse&&mInterconnect->Paths[Usb-1].InUse);
  assert(InterconnectUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);
  assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,Ufs)==EFI_SUCCESS);
  assert(mInterconnectProtocol.ReleasePath(&mInterconnectProtocol,Usb)==EFI_SUCCESS);
  assert(InterconnectUnload((EFI_HANDLE)1)==EFI_SUCCESS&&RpmhLibDeinit(&Rpmh)==CR_SUCCESS);
  puts("ICC/RPMh/QUP/I2C15 actual composition: callback completes after lock release; IRQ progress, recursive/high-TPL rejection, UFS+USB ownership across display zero/restore and recreation PASS");return 0;
}
