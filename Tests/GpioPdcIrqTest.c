/* Actual native OSKAL + GPIO/PDC helpers with bounded MMIO and failing GIC.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Library/CrTargetGpioLib.h>
#ifndef CR_OSKAL_INTERRUPT_IMPLEMENTATION
#define CR_OSKAL_INTERRUPT_IMPLEMENTATION "../Library/OskInterruptLib/oskinterrupt.c"
#endif
#include CR_OSKAL_INTERRUPT_IMPLEMENTATION

EFI_GUID gHardwareInterrupt2ProtocolGuid;
EFI_BOOT_SERVICES *gBS;
static HARDWARE_INTERRUPT_HANDLER Hardware[1024];
static EFI_TPL Priority;
static BOOLEAN FailEnable,FailDisable,FailRemove;
static EFI_STATUS EnableResult,RemoveResult;
static UINTN Reads,Writes,Installs,Removes,Calls,Eois;
static UINTN Addresses[512],WordCount;
static UINT32 Words[512];
static PdcDeviceContext Pdc;
static GpioDeviceContext Gpio,Other;
static BOOLEAN TryMutation;
static CR_STATUS MutationStatus;

static UINT32 *Word(UINTN Address) {
  assert((Address&3)==0);
  assert((Address>=0xf100000 && Address<0xf500000) ||
         (Address>=0xb220000 && Address<0xb250000) ||
         (Address>=0x174000f0 && Address<=0x17400150));
  for(UINTN I=0;I<WordCount;++I) if(Addresses[I]==Address) return &Words[I];
  assert(WordCount<512);Addresses[WordCount]=Address;return &Words[WordCount++];
}
UINT32 EFIAPI MmioRead32(UINTN Address) {++Reads;return *Word(Address);}
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value) {++Writes;return *Word(Address)=Value;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) {(VOID)Level;(VOID)Format;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) {return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) {(VOID)Level;return FALSE;}
static EFI_TPL EFIAPI Raise(EFI_TPL New) {
 assert(New==TPL_HIGH_LEVEL && New>=Priority);EFI_TPL Old=Priority;Priority=New;return Old;
}
static VOID EFIAPI RestorePriority(EFI_TPL Old) {assert(Priority==TPL_HIGH_LEVEL);Priority=Old;}
static EFI_STATUS EFIAPI HwRegister(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source,HARDWARE_INTERRUPT_HANDLER Handler) {
 (VOID)This;assert(Source<1024);
 if(Handler==NULL) {++Removes;if(RemoveResult!=EFI_SUCCESS)return RemoveResult;if(FailRemove) return EFI_DEVICE_ERROR;assert(Hardware[Source]!=NULL);Hardware[Source]=NULL;return EFI_SUCCESS;}
 ++Installs;if(Hardware[Source]!=NULL) return EFI_ALREADY_STARTED;Hardware[Source]=Handler;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Enable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source) {
 (VOID)This;(VOID)Source;if(EnableResult!=EFI_SUCCESS)return EnableResult;return FailEnable?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Disable(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source) {
 (VOID)This;(VOID)Source;return FailDisable?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Eoi(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source) {
 (VOID)This;(VOID)Source;assert(Priority==TPL_HIGH_LEVEL);++Eois;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Trigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *This,HARDWARE_INTERRUPT_SOURCE Source,EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE Type) {
 (VOID)This;(VOID)Source;assert(Type<=EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_RISING);return EFI_SUCCESS;
}
static EFI_HARDWARE_INTERRUPT2_PROTOCOL Controller={.RegisterInterruptSource=HwRegister,
 .EnableInterruptSource=Enable,.DisableInterruptSource=Disable,.EndOfInterrupt=Eoi,.SetTriggerType=Trigger};
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid,VOID *Registration,VOID **Result) {
 (VOID)Guid;(VOID)Registration;
 assert(Priority<=TPL_NOTIFY); /* DXE protocol database locking requirement. */
 *Result=&Controller;return EFI_SUCCESS;
}
static EFI_BOOT_SERVICES Boot={.LocateProtocol=Locate,.RaiseTPL=Raise,.RestoreTPL=RestorePriority};
static VOID Handler(VOID *Param) {
 assert(Param==(VOID *)1 && Priority==TPL_HIGH_LEVEL);++Calls;
 if(TryMutation) {
  CR_INTERRUPT_GUARD Guard;
  assert(CrInterruptGuardAcquire(&Guard)==CR_BUSY && !Guard.Active);
  MutationStatus=PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH);
 }
}
static VOID EFIAPI Foreign(HARDWARE_INTERRUPT_SOURCE Source,EFI_SYSTEM_CONTEXT Context) {(VOID)Source;(VOID)Context;}
static VOID Fire(UINTN Source) {
 EFI_SYSTEM_CONTEXT Context={0};assert(Hardware[Source]!=NULL);
 Hardware[Source](Source,Context);assert(Priority==TPL_APPLICATION);
}
static VOID InitGpio(GpioDeviceContext *Ctx,UINT64 Base,UINT32 Source) {
 memset(Ctx,0,sizeof(*Ctx));Ctx->TlmmBaseAddress=Base;Ctx->PinCount=4;
 Ctx->RegCfg[0]=CrTargetGetGpioContext()->RegCfg[0];
 Ctx->InterruptConfig.InterruptNumber=Source;Ctx->InterruptConfig.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH;
 for(UINT16 Pin=0;Pin<4;++Pin) {
  Ctx->GpioPins[Pin].pRegCfg=&Ctx->RegCfg[0];Ctx->GpioPins[Pin].pTileInfo=&Ctx->Tiles[0];
  Ctx->GpioPins[Pin].PdcPinNumber=GPIO_PDC_PIN_NUMBER_INVALID;
 }
 Ctx->GpioPins[3].PdcPinNumber=1;
 assert(GpioLibInit(&Ctx)==CR_SUCCESS);
}
int main(void) {
 gBS=&Boot;Priority=TPL_APPLICATION;
 Pdc=(PdcDeviceContext){.BaseAddress=0xb220000,.Size=0x30000,
      .SPIConfigBase=0x174000f0,.SPIConfigSize=0x64,.PdcRangeCount=2,
      .PdcRegion={{0,480,12},{40,520,54}}};
 PdcDeviceContext *P=&Pdc;
 CR_INTERRUPT_GUARD First,Second;
 assert(CrInterruptGuardAcquire(&First)==CR_SUCCESS && Priority==TPL_HIGH_LEVEL);
 assert(CrInterruptGuardAcquire(&Second)==CR_SUCCESS);
 CrInterruptGuardRelease(&Second);assert(Priority==TPL_HIGH_LEVEL);
 CrInterruptGuardRelease(&First);assert(Priority==TPL_APPLICATION);
 assert(CrInterruptGuardAcquire(NULL)==CR_INVALID_PARAMETER);
 gBS=NULL;assert(CrInterruptGuardAcquire(&First)==CR_NOT_READY);gBS=&Boot;

 CR_INTERRUPT_CONFIG Existing={.InterruptNumber=513,.Handler=Handler,.Param=(VOID *)1,.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH};
 assert(CrRegisterInterrupt(&Existing)==CR_SUCCESS);
 UINTN OldWrites=Writes,OldRemoves=Removes;
 assert(PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_NOT_FOUND);
 assert(Writes==OldWrites && Removes==OldRemoves && CrInterruptIsRegistered(&Existing));
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_BUSY);
 assert(Writes==OldWrites && Reads==0 && Pdc.ActiveInterrupts==0);
 assert(CrUnregisterInterrupt(&Existing)==CR_SUCCESS);
 Hardware[513]=Foreign;
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_ALREADY_STARTED);
 assert(Hardware[513]==Foreign && Writes==OldWrites && Pdc.ActiveInterrupts==0);Hardware[513]=NULL;
 assert(PdcRegisterInterrupt(&Pdc,1,NULL,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_INVALID_PARAMETER);
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,NULL,CR_INTERRUPT_TRIGGER_TYPE_INVALID)==CR_INVALID_PARAMETER);
 UINTN OldInstalls=Installs;Pdc.SPIConfigSize=0x3c;
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,NULL,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_BAD_BUFFER_SIZE);
 assert(Installs==OldInstalls && Writes==OldWrites);Pdc.SPIConfigSize=0x64;
 FailEnable=FailRemove=TRUE;
 assert(CR_ERROR(PdcRegisterInterrupt(&Pdc,1,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)));
 assert(Pdc.ActiveInterrupts==1 && Writes==OldWrites);Fire(513);assert(Calls==0);
 assert(PdcLibInit(&P)==CR_BUSY && Writes==OldWrites);
 FailEnable=FailRemove=FALSE;
 assert(PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS && Pdc.ActiveInterrupts==0);

 *Word(Pdc.BaseAddress+0x114)=0xa5;*Word(Pdc.BaseAddress+0x10)=0xcafea502;
 *Word(Pdc.SPIConfigBase+60)=0xdeadbeef;
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 assert(Pdc.ActiveInterrupts==1 && (*Word(Pdc.BaseAddress+0x114)&7)==4);
 OldWrites=Writes;OldInstalls=Installs;
 assert(PdcRegisterInterrupt(&Pdc,1,Handler,NULL,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_BUSY);
 assert(Writes==OldWrites && Installs==OldInstalls);
 TryMutation=TRUE;Fire(513);TryMutation=FALSE;assert(Calls==1 && MutationStatus==CR_BUSY && Pdc.ActiveInterrupts==1);
 assert(PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_INVALID_PARAMETER && Writes==OldWrites);
 *Word(Pdc.BaseAddress+0x114)|=0x100;*Word(Pdc.SPIConfigBase+60)^=0x100;*Word(Pdc.BaseAddress+0x10)^=0x100;
 FailDisable=TRUE;
 assert(CR_ERROR(PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)) && Pdc.ActiveInterrupts==1);
 assert((*Word(Pdc.BaseAddress+0x10)&2)==0);FailDisable=FALSE;
 assert(PdcUnregisterInterrupt(&Pdc,1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 assert(*Word(Pdc.BaseAddress+0x114)==0x1a5 && *Word(Pdc.SPIConfigBase+60)==(0xdeadbeef^0x100));
 assert(*Word(Pdc.BaseAddress+0x10)==(0xcafea502^0x100) && Pdc.ActiveInterrupts==0);
 for(UINT16 I=0;I<16;++I) assert(PdcRegisterInterrupt(&Pdc,I<12?I:I+28,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_SUCCESS);
 OldWrites=Writes;assert(PdcRegisterInterrupt(&Pdc,44,Handler,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_OUT_OF_RESOURCES && Writes==OldWrites);
 for(UINT16 I=0;I<16;++I) assert(PdcUnregisterInterrupt(&Pdc,I<12?I:I+28,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_SUCCESS);
 assert(Pdc.ActiveInterrupts==0);

 /* Stable controller storage must survive both successful registration and
  * backend rollback failure. The prior stack-copy implementation fails the
  * first actual pin registration below when linked with exact-owner OSKAL. */
 GpioDeviceContext Retry;InitGpio(&Retry,0xf100000,242);
 CR_INTERRUPT_CONFIG SavedConfig=Retry.InterruptConfig;
 FailEnable=TRUE;
 assert(GpioInitIrq(&Retry)==CR_DEVICE_ERROR);
 assert(!CrInterruptIsRegistered(&Retry.InterruptConfig) && Hardware[242]==NULL);
 assert(memcmp(&SavedConfig,&Retry.InterruptConfig,sizeof(SavedConfig))==0);
 FailRemove=TRUE;
 assert(GpioInitIrq(&Retry)==CR_DEVICE_ERROR);
 assert(CrInterruptIsRegistered(&Retry.InterruptConfig) && Hardware[242]!=NULL);
 assert(Retry.InterruptConfig.Handler!=NULL && Retry.InterruptConfig.Param==&Retry);
 CR_INTERRUPT_CONFIG RetainedConfig=Retry.InterruptConfig;
 OldInstalls=Installs;OldRemoves=Removes;
 assert(GpioInitIrq(&Retry)==CR_BUSY);
 assert(Installs==OldInstalls && Removes==OldRemoves &&
        memcmp(&RetainedConfig,&Retry.InterruptConfig,sizeof(RetainedConfig))==0);
 FailEnable=FailRemove=FALSE;
 assert(CrUnregisterInterrupt(&Retry.InterruptConfig)==CR_SUCCESS);
 /* Unknown provider warnings are normalized by OSKAL and retain the same
  * storage whenever the backend cannot confirm removal. */
 SavedConfig=Retry.InterruptConfig;EnableResult=EFI_WARN_UNKNOWN_GLYPH;
 assert(GpioInitIrq(&Retry)==CR_NOT_READY && Hardware[242]==NULL);
 assert(memcmp(&SavedConfig,&Retry.InterruptConfig,sizeof(SavedConfig))==0);
 RemoveResult=EFI_WARN_UNKNOWN_GLYPH;
 assert(GpioInitIrq(&Retry)==CR_NOT_READY && CrInterruptIsRegistered(&Retry.InterruptConfig));
 assert(CrUnregisterInterrupt(&Retry.InterruptConfig)==CR_NOT_READY);
 assert(CrInterruptIsRegistered(&Retry.InterruptConfig) && Hardware[242]!=NULL);
 EnableResult=RemoveResult=EFI_SUCCESS;
 assert(CrUnregisterInterrupt(&Retry.InterruptConfig)==CR_SUCCESS);
 Hardware[242]=Foreign;
 SavedConfig=Retry.InterruptConfig;
 assert(GpioInitIrq(&Retry)==CR_ALREADY_STARTED && Hardware[242]==Foreign);
 assert(memcmp(&SavedConfig,&Retry.InterruptConfig,sizeof(SavedConfig))==0);
 Hardware[242]=NULL;
 assert(GpioInitIrq(&Retry)==CR_SUCCESS && CrInterruptIsRegistered(&Retry.InterruptConfig));
 assert(CrUnregisterInterrupt(&Retry.InterruptConfig)==CR_SUCCESS);
 InitGpio(&Gpio,0xf100000,240);InitGpio(&Other,0xf300000,241);
 OldWrites=Writes;OldInstalls=Installs;
 assert(GpioRegisterGpioIrq(NULL,&Pdc,0,Handler,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_INVALID_PARAMETER);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,4,Handler,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_INVALID_PARAMETER);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,NULL,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_INVALID_PARAMETER);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,Handler,NULL,CR_INTERRUPT_TRIGGER_TYPE_INVALID)==CR_INVALID_PARAMETER);
 Gpio.RegCfg[0].InterruptDetectionWidth=1;
 assert(GpioSetInterruptCfg(&Gpio,0,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==CR_UNSUPPORTED);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,Handler,NULL,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==CR_UNSUPPORTED);
 Gpio.RegCfg[0].InterruptDetectionWidth=2;
 assert(GpioSetInterruptCfg(&Gpio,0,CR_INTERRUPT_TRIGGER_TYPE_INVALID)==CR_INVALID_PARAMETER);
 assert(GpioEnableInterrupt(&Gpio,0,2)==CR_INVALID_PARAMETER && GpioMaskInterrupt(&Gpio,0,2)==CR_INVALID_PARAMETER);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,Handler,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_NOT_READY);
 assert(Writes==OldWrites && Installs==OldInstalls);
 assert(GpioInitIrq(&Gpio)==CR_SUCCESS && GpioInitIrq(&Other)==CR_SUCCESS);
 *Word(0xf100008)=0x12340000;*Word(0xf101008)=0x56780000;
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,1,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_SUCCESS);
 OldWrites=Writes;
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,0,Handler,NULL,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_BUSY && Writes==OldWrites);
 assert(GpioUnregisterGpioIrq(&Other,0,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_NOT_FOUND && Writes==OldWrites);
 GpioDeviceContext *G=&Gpio;assert(GpioLibInit(&G)==CR_BUSY && Writes==OldWrites);
 assert(GpioUnregisterGpioIrq(&Gpio,0,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS && *Word(0xf100008)==0x12340000);
 *Word(0xf10100c)=1;UINTN BeforeCalls=Calls;Fire(240);assert(Calls==BeforeCalls+1); // table hole
 assert(GpioRegisterGpioIrq(&Other,&Pdc,0,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 *Word(0xf30000c)=1;BeforeCalls=Calls;Fire(240);assert(Calls==BeforeCalls);Fire(241);assert(Calls==BeforeCalls+1);
 assert(GpioUnregisterGpioIrq(&Other,0,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 assert(GpioUnregisterGpioIrq(&Gpio,1,CR_INTERRUPT_TRIGGER_EDGE_RISING)==CR_SUCCESS && *Word(0xf101008)==0x56780000);
 assert(GpioRegisterGpioIrq(&Gpio,&Pdc,3,Handler,(VOID *)1,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS);
 assert(Gpio.ActiveInterrupts==1 && Pdc.ActiveInterrupts==1);
 FailRemove=TRUE;assert(CR_ERROR(GpioUnregisterGpioIrq(&Gpio,3,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)));
 assert(Gpio.ActiveInterrupts==1 && Pdc.ActiveInterrupts==1);FailRemove=FALSE;
 assert(GpioUnregisterGpioIrq(&Gpio,3,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_SUCCESS && Gpio.ActiveInterrupts==0 && Pdc.ActiveInterrupts==0);
 assert(CrUnregisterInterrupt(&Gpio.InterruptConfig)==CR_SUCCESS && CrUnregisterInterrupt(&Other.InterruptConfig)==CR_SUCCESS);
 assert(Priority==TPL_APPLICATION && Eois>=5);
 puts("Native GPIO/PDC IRQ: foreign-owner rejection before MMIO, retained cleanup, gated dispatch, inherited-bit restore, table holes and context isolation passed");
 return 0;
}
