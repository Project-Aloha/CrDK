/* Actual native lease policy with lower-pad failure injection. MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/GpioPinLeaseLib.h>
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
UINT32 EFIAPI MmioRead32(UINTN A){(VOID)A;assert(0);return 0;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){(VOID)A;(VOID)V;assert(0);return 0;}
static GpioDeviceContext C;
static GpioConfigParams Actual[GPIO_PINS_MAX];
static UINTN Reads,Writes;
static BOOLEAN Irq,FailApply;
VOID GpioReadConfig(GpioDeviceContext *P,GpioConfigParams *D){assert(P==&C);++Reads;*D=Actual[D->PinNumber];}
GpioValueType GpioReadOutputVal(GpioDeviceContext *P,UINT16 N){assert(P==&C);++Reads;return Actual[N].OutputValue;}
GpioValueType GpioReadInputVal(GpioDeviceContext *P,UINT16 N){assert(P==&C&&N==39);++Reads;return GPIO_VALUE_HIGH;}
CR_STATUS GpioCheckInterruptEnabled(GpioDeviceContext *P,UINT16 N,BOOLEAN *V){assert(P==&C&&(N==38||N==39));++Reads;*V=Irq;return CR_SUCCESS;}
CR_STATUS GpioApplyConfigChecked(GpioDeviceContext *P,CONST GpioConfigParams *D){assert(P==&C);++Writes;Actual[D->PinNumber]=*D;return FailApply?CR_DEVICE_ERROR:CR_SUCCESS;}
static VOID Setup(CR_GPIO_PIN_LEASE_BANK *B){memset(&C,0,sizeof(C));memset(B,0,sizeof(*B));memset(Actual,0,sizeof(Actual));Reads=Writes=0;Irq=FailApply=FALSE;
 C.TlmmBaseAddress=0xf000000;C.PinCount=180;C.EgpioFunction=GPIO_FUNC_15;
 C.RegCfg[0]=(GpioConfigRegInfo){.MuxSelMsk=0x3c,.PullMsk=3,.DriveStrengthMsk=0x1c0,.OutputEnableMsk=0x200,.InputMsk=1,.OutputMsk=2,.InterruptEnMsk=1};
 for(UINT16 N=38;N<=39;++N){C.GpioPins[N]=(GpioPinInfo){.PinAddress=0xf000000+N*0x1000,.pRegCfg=&C.RegCfg[0],.pTileInfo=&C.Tiles[0],.FuncMuxMap=3};Actual[N]=(GpioConfigParams){N,FALSE,GPIO_FUNC_1,GPIO_PULL_DOWN,GPIO_DRIVE_STRENGTH_2MA,GPIO_VALUE_HIGH};}
 assert(CrGpioPinLeaseInit(B,&C)==CR_SUCCESS);
}
int main(void){CR_GPIO_PIN_LEASE_BANK B;int O,F;BOOLEAN Owned;UINT8 V=0;
 GpioConfigParams Reset={38,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW};
 Setup(&B);C.GpioPins[38].Reserved=TRUE;assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_BUSY&&!Owned&&!Reads&&!Writes);
 Setup(&B);Reset.PinNumber=300;assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_INVALID_PARAMETER&&!Reads&&!Writes);Reset.PinNumber=38;
 Setup(&B);Irq=TRUE;assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_BUSY&&!Owned&&!Writes);
 Setup(&B);Actual[38].FunctionSel=GPIO_FUNC_15;assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_UNSUPPORTED&&!Owned&&!Writes);
 Setup(&B);GpioConfigParams Original=Actual[38];FailApply=TRUE;
 assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_DEVICE_ERROR&&Owned&&CrGpioPinLeaseOwned(&B,38));
 UINTN W=Writes;assert(CrGpioPinLeaseAcquire(&B,&F,&Reset,&Owned)==CR_BUSY&&!Owned&&Writes==W);
 assert(CrGpioPinLeaseSetValue(&B,&F,38,1)==CR_BUSY&&Writes==W);
 assert(CrGpioPinLeaseRelease(&B,&F,38)==CR_BUSY&&Writes==W);
 assert(CrGpioPinLeaseInit(&B,&C)==CR_SUCCESS&&CrGpioPinLeaseOwned(&B,38));
 assert(CrGpioPinLeaseRelease(&B,&O,38)==CR_DEVICE_ERROR&&CrGpioPinLeaseOwned(&B,38));FailApply=FALSE;
 assert(CrGpioPinLeaseRelease(&B,&O,38)==CR_SUCCESS&&!CrGpioPinLeaseOwned(&B,38)&&!memcmp(&Actual[38],&Original,sizeof(Original)));
 assert(CrGpioPinLeaseAcquire(&B,&O,&Reset,&Owned)==CR_SUCCESS&&Owned);
 assert(CrGpioPinLeaseSetValue(&B,&O,38,1)==CR_SUCCESS&&Actual[38].OutputValue==GPIO_VALUE_HIGH);
 GpioConfigParams Input={39,FALSE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_HIGH};
 assert(CrGpioPinLeaseAcquire(&B,&O,&Input,&Owned)==CR_SUCCESS&&Owned);
 assert(CrGpioPinLeaseReadValue(&B,&O,39,&V)==CR_SUCCESS&&V==1);
 W=Writes;assert(CrGpioPinLeaseSetValue(&B,&O,39,1)==CR_UNSUPPORTED&&Writes==W);
 assert(CrGpioPinLeaseRelease(&B,&O,39)==CR_SUCCESS&&CrGpioPinLeaseRelease(&B,&O,38)==CR_SUCCESS);
 Setup(&B);Original=Actual[38];GpioConfigParams Preserve=Reset;Preserve.DriveStrength=GPIO_DRIVE_STRENGTH_UNCHANGE;Preserve.OutputValue=GPIO_VALUE_UNCHANGE;
 assert(CrGpioPinLeaseAcquire(&B,&O,&Preserve,&Owned)==CR_SUCCESS&&Owned&&Actual[38].DriveStrength==Original.DriveStrength&&Actual[38].OutputValue==Original.OutputValue);
 assert(B.Entries[0].Requested.DriveStrength==Original.DriveStrength&&B.Entries[0].Requested.OutputValue==Original.OutputValue);
 Reads=Writes=0;assert(CrGpioPinLeaseAcquire(&B,&F,&Preserve,&Owned)==CR_BUSY&&!Owned&&!Reads&&!Writes);
 /* Retry derives retained fields from the immutable original snapshot, even
  * if a modeled lower-pad fault changed those physical fields meanwhile. */
 Actual[38].DriveStrength=GPIO_DRIVE_STRENGTH_16MA;Actual[38].OutputValue=GPIO_VALUE_LOW;
 assert(CrGpioPinLeaseAcquire(&B,&O,&Preserve,&Owned)==CR_SUCCESS&&Owned&&Actual[38].DriveStrength==Original.DriveStrength&&Actual[38].OutputValue==Original.OutputValue);
 assert(CrGpioPinLeaseRelease(&B,&O,38)==CR_SUCCESS&&!memcmp(&Actual[38],&Original,sizeof(Original)));
 Setup(&B);Preserve.Pull=GPIO_PULL_UNCHANGE;assert(CrGpioPinLeaseAcquire(&B,&O,&Preserve,&Owned)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
 puts("GPIO pin lease actual core: snapshot, foreign/IRQ/EGPIO fences, partial ownership and retained restore PASS");return 0;
}
