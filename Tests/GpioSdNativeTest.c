/* Actual native protocol + typed pads + GPIO core + SM8450 pin table. MIT */
#include <Uefi.h>
#include "SdTargetTestProvider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/CrTargetGpioLib.h>
#include "../Driver/GpioCrDxe/GpioCrDxe.c"
EFI_BOOT_SERVICES *gBS;
STATIC UINT32 Regs[0x1000000/4];
STATIC UINTN Writes,Reads,DropAddress,DropCount,Guards,GuardDepth,IrqCalls;
STATIC BOOLEAN BusyGuard,RequireGuard;
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=0x0f000000&&A<0x10000000&&!(A&3));++Reads;return Regs[(A-0xf000000)/4];}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){assert(A>=0x0f000000&&A<0x10000000&&!(A&3));++Writes;if(RequireGuard)assert(GuardDepth);
  if(DropCount&&A==DropAddress){--DropCount;return V;}Regs[(A-0xf000000)/4]=V;return V;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
CR_STATUS CrInterruptGuardAcquire(CR_INTERRUPT_GUARD *G){G->Active=FALSE;if(BusyGuard)return CR_BUSY;
  G->Active=TRUE;++GuardDepth;++Guards;return CR_SUCCESS;}
VOID CrInterruptGuardRelease(CR_INTERRUPT_GUARD *G){assert(G->Active&&GuardDepth);G->Active=FALSE;--GuardDepth;}
CR_STATUS GpioRegisterGpioIrq(GpioDeviceContext *C,PdcDeviceContext *P,UINT16 Pin,CR_INTERRUPT_HANDLER H,VOID *V,CR_INTERRUPT_TRIGGER_TYPE T){
  (VOID)C;(VOID)P;(VOID)H;(VOID)V;(VOID)T;assert(Pin==92||Pin==38);++IrqCalls;return CR_DEVICE_ERROR;}
EFI_GUID gEfiGpioCrProtocolGuid=EFI_GPIO_CR_PROTOCOL_GUID;
STATIC PdcDeviceContext Pdc;
GpioDeviceContext *CrDalGetGpioContext(VOID){return CrTargetGetGpioContext();}
PdcDeviceContext *CrDalGetPdcContext(VOID){return &Pdc;}
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE T,UINT32 I,UINT32 R,UINTN Z,CONST CR_DAL_DEVICE_INFO **Out){
  (VOID)I;(VOID)R;(VOID)Z;assert(T==CrDalDeviceSd);STATIC CR_DAL_DEVICE_INFO D;
  D.Data=CrDalGetSdContext();*Out=&D;return D.Data?EFI_SUCCESS:EFI_NOT_FOUND;}
CR_STATUS PdcLibInit(PdcDeviceContext **C){(VOID)C;assert(0);return CR_DEVICE_ERROR;}
CR_STATUS GpioInitIrq(GpioDeviceContext *C){(VOID)C;assert(0);return CR_DEVICE_ERROR;}
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *C){(VOID)C;assert(0);return CR_DEVICE_ERROR;}
STATIC VOID Dummy(VOID *P){(VOID)P;}
int main(void){
  CR_TARGET_SD_CONTEXT BadTarget=*CrTargetGetSdContext();BadTarget.Paths[1].Peak=0;mTestSdTarget=&BadTarget;
  assert(GpioEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!Reads&&!Writes);
  BadTarget=*CrTargetGetSdContext();BadTarget.Gpio.CdPin=299;
  assert(GpioEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!Reads&&!Writes);
  mTestSdTarget=NULL;

  mSdTarget=CrTargetGetSdContext();
  mGpioContext=CrTargetGetGpioContext();assert(GpioLibInit(&mGpioContext)==CR_SUCCESS);
  assert(CrGpioSdQualify(mGpioContext)==CR_SUCCESS);
  UINTN CdCtl=mGpioContext->GpioPins[92].PinAddress+mGpioContext->GpioPins[92].pRegCfg->ControlRegOffset;
  UINTN CdIo=mGpioContext->GpioPins[92].PinAddress+mGpioContext->GpioPins[92].pRegCfg->IoRegOffset;
  UINTN CdInt=mGpioContext->GpioPins[92].PinAddress+mGpioContext->GpioPins[92].pRegCfg->IntCfgRegOffset;
  UINTN Pad=(UINTN)mGpioContext->TlmmBaseAddress+0x1d6000;
  GpioConfigParams Original={92,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_DOWN,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_HIGH};
  assert(GpioApplyConfigChecked(mGpioContext,&Original)==CR_SUCCESS);
  MmioWrite32(Pad,0xa5a5ffff);MmioWrite32(CdIo,MmioRead32(CdIo)|1);
  UINT32 PadBefore=MmioRead32(Pad),CtlBefore=MmioRead32(CdCtl),IoBefore=MmioRead32(CdIo);
  int Owner,Foreign;BOOLEAN Owned=TRUE,High=FALSE;
  UINTN Before=Writes;CONST CR_TARGET_SD_CONTEXT *Exact=mSdTarget;
  mSdTarget=NULL;assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==EFI_UNSUPPORTED&&!Owned&&Writes==Before);
  mSdTarget=Exact;CR_GPIO_SD_PROFILE Wrong=Exact->Gpio;Wrong.CdPin=8;
  CR_GPIO_SD_SNAPSHOT Bad={.Snapshot=TRUE,.OriginalCd={.PinNumber=92}};
  assert(CrGpioSdRestoreProfile(mGpioContext,&Wrong,&Bad)==EFI_INVALID_PARAMETER&&Writes==Before);
  Wrong=Exact->Gpio;Wrong.PadControlOffset+=4;
  assert(CrGpioSdSnapshotProfile(mGpioContext,&Wrong,&Bad)==EFI_INVALID_PARAMETER&&Writes==Before);
  BusyGuard=TRUE;
  assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==CR_BUSY&&!Owned&&Writes==Before);BusyGuard=FALSE;
  MmioWrite32(CdInt,1);Before=Writes;
  assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Writes==Before);MmioWrite32(CdInt,0);
  DropAddress=CdCtl;DropCount=1;
  assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==EFI_DEVICE_ERROR&&Owned&&mSdTouched&&mSdSnapshot.Snapshot);
  CR_GPIO_SD_SNAPSHOT Saved=mSdSnapshot;Before=Writes;
  assert(AcquireSd(&gGpioCrProtocol,&Foreign,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Writes==Before);
  assert(ReleaseSd(&gGpioCrProtocol,&Foreign)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(ReleaseSd(&gGpioCrProtocol,&Owner)==EFI_SUCCESS&&!mSdOwner&&MmioRead32(CdCtl)==CtlBefore&&MmioRead32(CdIo)==IoBefore&&MmioRead32(Pad)==PadBefore);
  assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==EFI_SUCCESS&&Owned);Saved=mSdSnapshot;
  Before=Writes;GpioConfigParams C;GpioInitConfigParams(&C,211);C.Pull=GPIO_PULL_DOWN;
  assert(ProtocolGpioConfig(&gGpioCrProtocol,&C)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(ProtocolGpioSetIOValue(&gGpioCrProtocol,92,0)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(ProtocolGpioRegisterGpioInterrupt(&gGpioCrProtocol,92,Dummy,NULL,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==EFI_ACCESS_DENIED&&Writes==Before&&!IrqCalls);
  assert(ProtocolGpioSetIOValue(&gGpioCrProtocol,8,1)==EFI_SUCCESS&&Writes>Before);
  assert(SetSdActive(&gGpioCrProtocol,&Owner,TRUE)==EFI_SUCCESS);
  assert((MmioRead32(Pad)&0xdfff)==0x1fff&&(MmioRead32(Pad)&~0xdfffU)==(PadBefore&~0xdfffU));
  assert(ReadSdCardDetect(&gGpioCrProtocol,&Owner,&High)==EFI_SUCCESS&&High);
  MmioWrite32(CdIo,MmioRead32(CdIo)&~1U);assert(ReadSdCardDetect(&gGpioCrProtocol,&Owner,&High)==EFI_SUCCESS&&!High);
  DropAddress=Pad;DropCount=1;
  assert(ReleaseSd(&gGpioCrProtocol,&Owner)==EFI_DEVICE_ERROR&&mSdOwner==&Owner&&mSdTouched);
  assert(!memcmp(&Saved,&mSdSnapshot,sizeof(Saved)));
  assert(ReleaseSd(&gGpioCrProtocol,&Owner)==EFI_SUCCESS&&!mSdOwner&&MmioRead32(Pad)==PadBefore&&MmioRead32(CdCtl)==CtlBefore);
  /* Appended pads-only contract cannot depend on or access CD metadata.
     Existing native combined qualification still requires that metadata. */
  CR_GPIO_SD_PADS_SNAPSHOT Only={0};CONST GpioConfigRegInfo *CdReg=mGpioContext->GpioPins[92].pRegCfg;
  mGpioContext->GpioPins[92].pRegCfg=NULL;
  assert(CrGpioSdPadsQualify(mGpioContext)==EFI_SUCCESS&&CrGpioSdQualify(mGpioContext)==EFI_INVALID_PARAMETER);
  assert(CrGpioSdPadsSnapshot(mGpioContext,&Only)==EFI_SUCCESS&&Only.Snapshot);
  assert(CrGpioSdPadsSetActive(mGpioContext,TRUE)==EFI_SUCCESS&&(MmioRead32(Pad)&0xdfff)==0x1fff);
  assert(CrGpioSdPadsRestore(mGpioContext,&Only)==EFI_SUCCESS&&MmioRead32(Pad)==PadBefore);
  assert(MmioRead32(CdCtl)==CtlBefore&&MmioRead32(CdIo)==(IoBefore&~1U));
  mGpioContext->GpioPins[92].pRegCfg=(GpioConfigRegInfo *)CdReg;
  /* A failed legacy IRQ setup has no public unregister path; reserve that
   * potential callback before fallible setup so SD never steals it. */
  assert(ProtocolGpioRegisterGpioInterrupt(&gGpioCrProtocol,92,Dummy,NULL,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==EFI_DEVICE_ERROR&&IrqCalls==1);
  Before=Writes;assert(AcquireSd(&gGpioCrProtocol,&Owner,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Writes==Before);
  /* A newly admitted ordinary lease must not race an unguarded legacy
   * write. Actual TLMM writes require provider exclusion even when no owner
   * existed at the start of the legacy call. */
  assert(CrGpioPinLeaseInit(&mPinLeases,mGpioContext)==EFI_SUCCESS);
  RequireGuard=TRUE;
  assert(ProtocolGpioSetIOValue(&gGpioCrProtocol,38,1)==EFI_SUCCESS);
  GpioConfigParams Pin={38,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW};
  GpioConfigParams Baseline={.PinNumber=38};GpioReadConfig(mGpioContext,&Baseline);
  Baseline.OutputValue=GpioReadOutputVal(mGpioContext,38);
  assert(AcquirePin(&gGpioCrProtocol,&Owner,&Pin,&Owned)==EFI_SUCCESS&&Owned);
  Before=Writes;
  assert(ProtocolGpioSetIOValue(&gGpioCrProtocol,38,1)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(ProtocolGpioConfig(&gGpioCrProtocol,&Pin)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(ProtocolGpioRegisterGpioInterrupt(&gGpioCrProtocol,38,Dummy,NULL,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==EFI_ACCESS_DENIED&&Writes==Before);
  assert(SetPinValue(&gGpioCrProtocol,&Foreign,38,1)!=EFI_SUCCESS&&Writes==Before);
  assert(SetPinValue(&gGpioCrProtocol,&Owner,38,1)==EFI_SUCCESS);
  assert(ReleasePin(&gGpioCrProtocol,&Foreign,38)!=EFI_SUCCESS);
  UINTN PinCtl=mGpioContext->GpioPins[38].PinAddress+mGpioContext->GpioPins[38].pRegCfg->ControlRegOffset;
  DropAddress=PinCtl;DropCount=1;
  assert(ReleasePin(&gGpioCrProtocol,&Owner,38)==EFI_DEVICE_ERROR&&CrGpioPinLeaseOwned(&mPinLeases,38));
  assert(ReleasePin(&gGpioCrProtocol,&Owner,38)==EFI_SUCCESS&&!CrGpioPinLeaseOwned(&mPinLeases,38));
  GpioConfigParams Restored={.PinNumber=38};GpioReadConfig(mGpioContext,&Restored);
  Restored.OutputValue=GpioReadOutputVal(mGpioContext,38);
  assert(Restored.FunctionSel==Baseline.FunctionSel&&Restored.Pull==Baseline.Pull&&
    Restored.DriveStrength==Baseline.DriveStrength&&Restored.OutputEnable==Baseline.OutputEnable&&
    Restored.OutputValue==Baseline.OutputValue);
  assert(ProtocolGpioRegisterGpioInterrupt(&gGpioCrProtocol,38,Dummy,NULL,CR_INTERRUPT_TRIGGER_EDGE_BOTH)==EFI_DEVICE_ERROR);
  Before=Writes;assert(AcquirePin(&gGpioCrProtocol,&Owner,&Pin,&Owned)==EFI_ACCESS_DENIED&&!Owned&&Writes==Before);
  assert(Guards>10&&!GuardDepth);
  puts("Actual native GPIO SD lease: foreign/legacy IRQ fences, CD polarity, immutable retained restore PASS");return 0;
}
