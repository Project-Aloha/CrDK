/** Production device-init polling, descriptor discovery and SCSI LUN iterator. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#undef NULL
#include "UfsPassThru.h"
static UINT8 mUfsTargetId[TARGET_MAX_BYTES];
static UINT32 Budget;
static UINT64 ClockTicks;
static UINT32 QueryLatencyUs=100;
static unsigned Delays;
static unsigned Polls, ClearAt, UnitReads, DeviceMode;
static BOOLEAN DownCounter, FailSet, FailRead;
static EFI_STATUS DescriptorFailure;
static unsigned FailUnit, DeviceReads;
#undef PcdGet32
#define PcdGet32(Token) Budget
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
VOID EFIAPI DebugAssert(CONST CHAR8 *F, UINTN L, CONST CHAR8 *D) { abort(); }
VOID *EFIAPI ZeroMem(VOID *P, UINTN N) { return memset(P,0,N); }
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P,V,N); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp(A,B,N); }
UINT64 EFIAPI DivU64x32(UINT64 A, UINT32 B) { return A/B; }
UINT64 EFIAPI GetPerformanceCounterProperties(UINT64 *Start, UINT64 *End) {
 *Start=DownCounter ? 1023 : 0; *End=DownCounter ? 0 : 1023; return 1000000;
}
UINT64 EFIAPI GetPerformanceCounter(VOID) { return DownCounter ? 1023-(ClockTicks%1024) : ClockTicks%1024; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks*1000; }
UINTN EFIAPI MicroSecondDelay(UINTN Us) { Delays++; ClockTicks+=Us; return Us; }
EFI_STATUS UfsSetFlag(UFS_PASS_THRU_PRIVATE_DATA *P, UINT8 Id) {
 assert(Id==UfsFlagDevInit); return FailSet ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
EFI_STATUS UfsReadFlag(UFS_PASS_THRU_PRIVATE_DATA *P, UINT8 Id, UINT8 *V) {
 assert(Id==UfsFlagDevInit); Polls++; ClockTicks+=QueryLatencyUs; *V=Polls==ClearAt ? 0 : 1;
 return FailRead ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
EFI_STATUS UfsRwDeviceDesc(UFS_PASS_THRU_PRIVATE_DATA *P, BOOLEAN Read,
 UINT8 Id, UINT8 Index, UINT8 Selector, VOID *Desc, UINT32 *Size) {
 assert(Read && Selector==0);
 if(Id==UfsUnitDesc) {
   UFS_UNIT_DESC *U=Desc;
   assert(Index==UnitReads++ && *Size==sizeof(*U));
   if(DescriptorFailure && Index==FailUnit) return DescriptorFailure;
   if(Index==0) { *Size=0; return EFI_INVALID_PARAMETER; }
   U->Length=sizeof(*U); U->DescType=UfsUnitDesc; U->UnitIdx=Index;
   if(Index==1) { U->LunEn=1; U->Length=4; *Size=4; }
   if(Index==2) { *Size=3; } /* no LunEn byte returned */
   if(Index==3) { U->LunEn=1; U->DescType=UfsDeviceDesc; }
   if(Index==4) { U->LunEn=1; U->Length=3; }
   if(Index==5) U->LunEn=1;
   return EFI_SUCCESS;
 }
 assert(Id==UfsDeviceDesc && Index==0 && *Size==sizeof(UFS_DEV_DESC));
 DeviceReads++;
 if(DescriptorFailure && FailUnit==8) return DescriptorFailure;
 UFS_DEV_DESC *D=Desc; D->Length=sizeof(*D); D->DescType=UfsDeviceDesc; D->SecurityLun=1;
 if(DeviceMode==1) *Size=OFFSET_OF(UFS_DEV_DESC,SecurityLun);
 if(DeviceMode==2) D->Length=OFFSET_OF(UFS_DEV_DESC,SecurityLun);
 if(DeviceMode==3) D->DescType=UfsUnitDesc;
 return EFI_SUCCESS;
}
#include "UfsEnumeration.inc"
int main(void) {
 UFS_PASS_THRU_PRIVATE_DATA P={.Signature=UFS_PASS_THRU_SIG};
 for(unsigned Down=0;Down<2;Down++) {
   DownCounter=Down;
   Budget=700; ClockTicks=900; Polls=0; ClearAt=2;
   assert(UfsFinishDeviceInitialization(&P)==EFI_SUCCESS && Polls==2);
   ClockTicks=900; Polls=0; ClearAt=0;
   assert(UfsFinishDeviceInitialization(&P)==EFI_TIMEOUT && Polls==2);
 }
 /* One slow READ_FLAG exhausts the deadline without any polling delay. */
 Budget=700; ClockTicks=0; Polls=0; ClearAt=0; Delays=0; QueryLatencyUs=800;
 assert(UfsFinishDeviceInitialization(&P)==EFI_TIMEOUT && Polls==1 && Delays==0);
 QueryLatencyUs=100;
 Budget=0; Polls=0; assert(UfsFinishDeviceInitialization(&P)==EFI_TIMEOUT && !Polls);
 Budget=700; FailSet=TRUE; assert(UfsFinishDeviceInitialization(&P)==EFI_DEVICE_ERROR && !Polls);
 FailSet=FALSE; FailRead=TRUE; assert(UfsFinishDeviceInitialization(&P)==EFI_DEVICE_ERROR && Polls==1); FailRead=FALSE;
 for(unsigned I=0;I<8;I++) P.Luns.Lun[I]=I;
 P.Luns.Lun[11]=UFS_WLUN_RPMB;
 for(DeviceMode=0;DeviceMode<4;DeviceMode++) {
   UINT8 TargetBytes[TARGET_MAX_BYTES], *Target=TargetBytes; UINT64 Lun=0;
   UnitReads=0; P.Luns.BitMask=0xfff; assert(UfsDiscoverLuns(&P)==EFI_SUCCESS);
   assert(UnitReads==8 && P.Luns.BitMask==(BIT1|BIT5|(DeviceMode==0 ? BIT11 : 0)));
   memset(TargetBytes,0xff,sizeof(TargetBytes));
   assert(UfsPassThruGetNextTargetLun(&P.ExtScsiPassThru,&Target,&Lun)==EFI_SUCCESS && Lun==0x100);
   assert(UfsPassThruGetNextTargetLun(&P.ExtScsiPassThru,&Target,&Lun)==EFI_SUCCESS && Lun==0x500);
   if(DeviceMode==0) assert(UfsPassThruGetNextTargetLun(&P.ExtScsiPassThru,&Target,&Lun)==EFI_SUCCESS && ((UINT8 *)&Lun)[0]==UFS_WLUN_PREFIX && ((UINT8 *)&Lun)[1]==(UFS_WLUN_RPMB&0x7f));
   assert(UfsPassThruGetNextTargetLun(&P.ExtScsiPassThru,&Target,&Lun)==EFI_NOT_FOUND);
 }
 EFI_STATUS Errors[]={EFI_TIMEOUT,EFI_DEVICE_ERROR,EFI_OUT_OF_RESOURCES,EFI_NOT_READY};
 for(unsigned E=0;E<ARRAY_SIZE(Errors);E++) {
   DescriptorFailure=Errors[E];
   for(FailUnit=0;FailUnit<=8;FailUnit++) {
     UnitReads=DeviceReads=0;
     assert(UfsDiscoverLuns(&P)==Errors[E]);
     assert(UnitReads==(FailUnit<8 ? FailUnit+1 : 8));
     assert(DeviceReads==(FailUnit==8));
   }
 }
 puts("UFS enumeration: elapsed init deadline, final clear, counter wrap, failed/short unit recovery, descriptor bounds and exposed LUNs PASS");
 return 0;
}
