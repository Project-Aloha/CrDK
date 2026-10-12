/* Actual SM8250 TLMM target + GPIO register engine + leases. MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/GpioPinLeaseLib.h>
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
static UINT32 Regs[0x1000000/4];
static UINTN Writes,Drop,DropCount;
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=0xf000000&&A<0x10000000&&!(A&3));return Regs[(A-0xf000000)/4];}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){assert(A>=0xf000000&&A<0x10000000&&!(A&3));++Writes;if(DropCount&&A==Drop){--DropCount;return V;}Regs[(A-0xf000000)/4]=V;return V;}
int main(void){GpioDeviceContext *C=CrTargetGetGpioContext();CR_GPIO_PIN_LEASE_BANK B={0};int Owner,Other;BOOLEAN Owned;
 assert(GpioLibInit(&C)==CR_SUCCESS&&CrGpioPinLeaseInit(&B,C)==CR_SUCCESS);
 const GpioPinInfo *P=&C->GpioPins[38];UINTN Ctl=P->PinAddress,Io=Ctl+4,Int=Ctl+8;
 assert(Ctl==0xf526000); /* south tile + GPIO38 stride */
 MmioWrite32(Ctl,0xa5a50041);MmioWrite32(Io,0x5a5a0003);
 UINT32 CtlBefore=MmioRead32(Ctl),IoBefore=MmioRead32(Io);
 GpioConfigParams R={38,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW};
 MmioWrite32(Int,1);UINTN W=Writes;assert(CrGpioPinLeaseAcquire(&B,&Owner,&R,&Owned)==CR_BUSY&&!Owned&&Writes==W);MmioWrite32(Int,0);
 Drop=Ctl;DropCount=1;assert(CrGpioPinLeaseAcquire(&B,&Owner,&R,&Owned)==CR_DEVICE_ERROR&&Owned);
 W=Writes;assert(CrGpioPinLeaseAcquire(&B,&Other,&R,&Owned)==CR_BUSY&&!Owned&&Writes==W);
 assert(CrGpioPinLeaseRelease(&B,&Owner,38)==CR_SUCCESS&&MmioRead32(Ctl)==CtlBefore&&MmioRead32(Io)==IoBefore);
 assert(CrGpioPinLeaseAcquire(&B,&Owner,&R,&Owned)==CR_SUCCESS&&Owned);
 assert((MmioRead32(Ctl)&0x3ff)==0x2c3&&!(MmioRead32(Io)&2));
 assert((MmioRead32(Ctl)&~0x3ffU)==(CtlBefore&~0x3ffU));
 assert(CrGpioPinLeaseSetValue(&B,&Owner,38,1)==CR_SUCCESS&&(MmioRead32(Io)&2));
 Drop=Ctl;DropCount=1;assert(CrGpioPinLeaseRelease(&B,&Owner,38)==CR_DEVICE_ERROR&&CrGpioPinLeaseOwned(&B,38));
 assert(CrGpioPinLeaseRelease(&B,&Owner,38)==CR_SUCCESS&&MmioRead32(Ctl)==CtlBefore&&MmioRead32(Io)==IoBefore);
 UINTN IrqCtl=C->GpioPins[39].PinAddress;MmioWrite32(IrqCtl,0x80000001);MmioWrite32(IrqCtl+4,0xa5);UINT32 InputBefore=MmioRead32(IrqCtl+4);
 GpioConfigParams I={39,FALSE,GPIO_FUNC_NORMAL,GPIO_PULL_UP,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_HIGH};
 assert(CrGpioPinLeaseAcquire(&B,&Owner,&I,&Owned)==CR_SUCCESS&&Owned);UINT8 V=0;
 assert(CrGpioPinLeaseReadValue(&B,&Owner,39,&V)==CR_SUCCESS&&V==1);
 /* Public TLMM raw status latches a falling edge while summary is masked.
    A short pulse is still consumed after the input has returned high. */
 UINTN Irq=IrqCtl+8,Pending=IrqCtl+12;BOOLEAN Edge=FALSE;
 MmioWrite32(Irq,0xa5000020);MmioWrite32(Pending,1);
 W=Writes;assert(CrGpioPinLeaseArmFallingEdge(&B,&Other,39)==CR_BUSY&&Writes==W);
 Drop=Irq;DropCount=1;
 assert(CrGpioPinLeaseArmFallingEdge(&B,&Owner,39)==CR_DEVICE_ERROR);
 assert(CrGpioPinLeaseArmFallingEdge(&B,&Owner,39)==CR_SUCCESS);
 assert(MmioRead32(Irq)==0xa500007a&&MmioRead32(Pending)==0);
 assert(CrGpioPinLeaseTakeEdge(&B,&Owner,39,&Edge)==CR_SUCCESS&&!Edge);
 MmioWrite32(Pending,1);
 assert(CrGpioPinLeaseTakeEdge(&B,&Owner,39,&Edge)==CR_SUCCESS&&Edge&&MmioRead32(Pending)==0);
 assert(CrGpioPinLeaseTakeEdge(&B,&Owner,39,&Edge)==CR_SUCCESS&&!Edge);
 MmioWrite32(Irq,0xa500007b);W=Writes;
 assert(CrGpioPinLeaseTakeEdge(&B,&Owner,39,&Edge)==CR_NOT_READY&&!Edge&&Writes==W);
 Drop=Irq;DropCount=1;
 assert(CrGpioPinLeaseRelease(&B,&Owner,39)==CR_DEVICE_ERROR&&CrGpioPinLeaseOwned(&B,39));
 assert(CrGpioPinLeaseRelease(&B,&Owner,39)==CR_SUCCESS&&MmioRead32(IrqCtl)==0x80000001&&MmioRead32(IrqCtl+4)==InputBefore);
 assert(MmioRead32(Irq)==0xa5000020);
 MmioWrite32(IrqCtl,0x80000141);InputBefore=MmioRead32(IrqCtl+4);
 I.DriveStrength=GPIO_DRIVE_STRENGTH_UNCHANGE;I.OutputValue=GPIO_VALUE_UNCHANGE;
 assert(CrGpioPinLeaseAcquire(&B,&Owner,&I,&Owned)==CR_SUCCESS&&Owned);
 assert(MmioRead32(IrqCtl)==0x80000143&&MmioRead32(IrqCtl+4)==InputBefore);
 assert(CrGpioPinLeaseAcquire(&B,&Owner,&I,&Owned)==CR_SUCCESS&&Owned&&MmioRead32(IrqCtl+4)==InputBefore);
 assert(CrGpioPinLeaseRelease(&B,&Owner,39)==CR_SUCCESS&&MmioRead32(IrqCtl)==0x80000141&&MmioRead32(IrqCtl+4)==InputBefore);
 puts("GPIO pin leases + actual SM8250 MMIO: GPIO38/39 south tile, latent latch, checked reset, IRQ refusal, retained exact restore PASS");return 0;
}
