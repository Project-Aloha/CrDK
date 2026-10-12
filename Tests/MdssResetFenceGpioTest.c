/* Complete production checked-setter bodies, with register boundary models. MIT */
#include "MdssResetFenceHost.h"
#define GPIO_CFG_REG_TYPE_IO_REG 1U
#define SET_FIELD(v,m) (((UINT32)(v)<<__builtin_ctz(m))&(m))
typedef struct {UINT32 OutputEnableMsk,MuxSelMsk,PullMsk,DriveStrengthMsk,OutputMsk;} GpioConfigRegInfo;
typedef struct {BOOLEAN Reserved;UINT32 FuncMuxMap;GpioConfigRegInfo *pRegCfg;} GpioPinInfo;
typedef struct {UINTN TlmmBaseAddress;BOOLEAN PullUpNoKeeper;GpioPinInfo GpioPins[76];} GpioDeviceContext;
typedef struct {VOID *Owner;GpioConfigParams Requested;BOOLEAN Touched;} CR_GPIO_PIN_LEASE;
typedef struct {GpioDeviceContext *Context;CR_GPIO_PIN_LEASE Entry;} CR_GPIO_PIN_LEASE_BANK;
static GpioDeviceContext Device;
static CR_GPIO_PIN_LEASE_BANK Bank;
static GpioConfigRegInfo Reg={1,2,4,8,16};
static GpioConfigParams Actual;
static UINT32 Fault,InputValue;
static BOOLEAN GpioPinValid(GpioDeviceContext *C,UINT16 Pin){return C==&Device&&Pin==75&&C->GpioPins[Pin].pRegCfg;}
static UINTN GpioGetCfgRegByIndex(GpioDeviceContext *C,UINT32 Type,UINT16 Pin){assert(C==&Device&&Type==1&&Pin==75);return 0x1004;}
static UINT32 CrMmioRead32(UINTN Address){assert(Address==0x1004);return Actual.OutputValue<<4;}
static VOID CrMmioWrite32(UINTN Address,UINT32 Value){assert(Address==0x1004);Actual.OutputValue=(Value>>4)&1;}
static VOID GpioSetConfig(GpioDeviceContext *C,GpioConfigParams *Desired){
  assert(C==&Device);Actual=*Desired;
  switch(Fault){case 1:Actual.OutputValue^=1;break;case 2:Actual.OutputEnable^=1;break;
    case 3:Actual.FunctionSel^=1;break;case 4:Actual.Pull^=1;break;case 5:Actual.DriveStrength^=1;break;}
}
static VOID GpioInitConfigParams(GpioConfigParams *C,UINT16 Pin){memset(C,0,sizeof(*C));C->PinNumber=Pin;}
static VOID GpioReadConfig(GpioDeviceContext *C,GpioConfigParams *Out){assert(C==&Device);*Out=Actual;/* Input sample is not the output latch. */Out->OutputValue=InputValue;}
static UINT32 GpioReadOutputVal(GpioDeviceContext *C,UINT16 Pin){assert(C==&Device&&Pin==75);return Actual.OutputValue;}
static CR_GPIO_PIN_LEASE *Find(CR_GPIO_PIN_LEASE_BANK *B,UINT16 Pin){return B==&Bank&&Pin==75&&B->Entry.Owner?&B->Entry:NULL;}
#include "GpioChecked.inc"
int main(void){
  int Owner=0,Other=0;unsigned Checks=0;
  Device.TlmmBaseAddress=0x1000;Device.GpioPins[75]=(GpioPinInfo){FALSE,1,&Reg};
  Bank.Context=&Device;Bank.Entry.Owner=&Owner;Bank.Entry.Requested=(GpioConfigParams){75,TRUE,0,0,3,0};
  for(UINT8 High=0;High<2;++High){
    InputValue=!High;
    for(Fault=0;Fault<=5;++Fault){
      EFI_STATUS S=CrGpioPinLeaseSetValue(&Bank,&Owner,75,High);
      assert(S==(Fault?EFI_DEVICE_ERROR:EFI_SUCCESS));assert(Bank.Entry.Touched);++Checks;
    }
  }
  assert(CrGpioPinLeaseSetValue(&Bank,&Other,75,1)==CR_BUSY);++Checks;
  assert(CrGpioPinLeaseSetValue(&Bank,&Owner,75,2)==CR_INVALID_PARAMETER);++Checks;
  Bank.Entry.Requested.OutputEnable=FALSE;assert(CrGpioPinLeaseSetValue(&Bank,&Owner,75,1)==CR_UNSUPPORTED);++Checks;
  printf("PASS: actual GPIO lease and checked-config output verification, %u checks\n",Checks);
  return 0;
}
