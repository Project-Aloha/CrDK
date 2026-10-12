/* Actual resource/PHY/bridge source fault tests. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/MdssPlatformLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V) {return memset(P,V,N);}
#endif
typedef struct {
 UINT32 Phy[0x800/4],Disp[0x10000/4]; UINT8 Bridge[256][256],Page;
 BOOLEAN ResourcesOnly;
 BOOLEAN Native,NativeFenced,InheritedLive,Map,Ext,Clock[10],Rail[3],Path[2],Power[2],Reset,Post;
 BOOLEAN BadQualification,FailLock,FailUnmap,FailExtOff,FailPost,FailReclaim,FailUnlock;
 BOOLEAN FailResetOff,FailReady,FailNativeFence,ForeignPhy,OverrunIova;
 UINT32 DropPhyOffset; CR_STATUS FailureStatus;
 UINTN Reads,Writes,SerialWrites,SerialReads,Mutations,FailMutation,Delay,Unlocks,Resets,NativeClaims,NativeFences;
 VOID *Permanent; UINT64 SerialIncarnation; UINT32 RemovedFact;
} FIXTURE;
static FIXTURE F;
static CR_STATUS Mutate(void) {++F.Mutations;return F.FailMutation==F.Mutations?(F.FailureStatus?F.FailureStatus:CR_DEVICE_ERROR):CR_SUCCESS;}
static VOID Stable(VOID *O) {assert(O && O==F.Permanent);}
static CR_STATUS Qualify(VOID *C,CONST CR_MDSS_PLATFORM_CONTRACT *M,CR_MDSS_PLATFORM_WITNESS *W) {
 assert(C==&F);memset(W,0,sizeof(*W));
 if(F.BadQualification)return CR_NOT_READY;
 W->Revision=1;W->Facts=CR_MDSS_QUAL_ALL&~F.RemovedFact;W->BootGeneration=M->BootGeneration;
 W->NativeTransferId=7;W->DmaIncarnation=11;W->SerialIncarnation=F.SerialIncarnation;
 W->ExternalClockIncarnation=13;W->PermanentStoreId=17;return CR_SUCCESS;
}
static CR_STATUS Claim(VOID *C,VOID *O,CONST CR_MDSS_PLATFORM_WITNESS *W,BOOLEAN *Owned) {
 assert(C==&F&&W->NativeTransferId==7&&!F.Native);F.Permanent=O;F.Native=TRUE;F.NativeFenced=FALSE;F.InheritedLive=TRUE;*Owned=TRUE;++F.NativeClaims;return Mutate();
}
static CR_STATUS NativeRelease(VOID *C,VOID *O) {assert(C==&F&&F.NativeFenced&&!F.InheritedLive);Stable(O);CR_STATUS S=Mutate();if(!CR_ERROR(S)){F.Native=FALSE;F.NativeFenced=FALSE;}return S;}
static CR_STATUS Ready(VOID *C,VOID *O) {assert(C==&F&&O);return F.FailReady?CR_NOT_READY:CR_SUCCESS;}
static CR_STATUS Clock(VOID *C,VOID *O,UINT32 I,BOOLEAN On) {
 assert(C==&F&&F.Native&&I<10&&(On||!F.InheritedLive));Stable(O);CR_STATUS S=Mutate();if(!CR_ERROR(S))F.Clock[I]=On;return S;
}
static CR_STATUS Rail(VOID *C,VOID *O,UINT32 I,BOOLEAN On) {
 assert(C==&F&&F.Native&&I<3&&(On||!F.InheritedLive));Stable(O);CR_STATUS S=Mutate();if(!CR_ERROR(S))F.Rail[I]=On;return S;
}
static CR_STATUS Bandwidth(VOID *C,VOID *O,UINT32 I,UINT64 Average,UINT64 Peak) {
 assert(C==&F&&F.Native&&I<2&&(Average||!F.InheritedLive));Stable(O);if(Average)assert(Average==(I?594000:1000)&&Peak==(I?1188000:1000));
 CR_STATUS S=Mutate();if(!CR_ERROR(S))F.Path[I]=Average!=0;return S;
}
static VOID Accessible(void) {assert(F.Native&&F.Clock[0]&&F.Clock[1]&&F.Clock[2]&&F.Clock[9]&&F.Rail[0]&&F.Rail[1]&&F.Rail[2]&&F.Path[0]&&F.Path[1]);}
static CR_STATUS NativeFence(VOID *C,VOID *O,CONST CR_MDSS_PLATFORM_WITNESS *W){
 assert(C==&F&&W->NativeTransferId==7);Stable(O);Accessible();for(UINT32 I=0;I<10;++I)assert(F.Clock[I]);++F.NativeFences;
 if(F.FailNativeFence)return CR_DEVICE_ERROR;CR_STATUS S=Mutate();if(!CR_ERROR(S)){F.InheritedLive=FALSE;F.NativeFenced=TRUE;}return S;
}
static CR_STATUS External(VOID *C,VOID *O,UINT64 Pixel,UINT64 Byte,BOOLEAN On) {
 assert(C==&F&&Pixel==148500000&&Byte==111375000&&F.NativeFenced&&!F.InheritedLive);Stable(O);Accessible();
 if(!On&&F.FailExtOff)return CR_DEVICE_ERROR;CR_STATUS S=Mutate();if(CR_ERROR(S))return S;
 F.Ext=On;if(On){F.Disp[0x8138/4]=0x201;F.Disp[0x80c0/4]=0x101;F.Disp[0x814c/4]=1;}return S;
}
static UINT32 Read(VOID *C,CR_MDSS_PLATFORM_WINDOW W,UINT32 O) {
 assert(C==&F&&!(O&3));Accessible();++F.Reads;
 if(W==CrMdssPlatformDispCc){assert(O<sizeof(F.Disp));if(O==0x9000)return 1U<<31;return F.Disp[O/4];}
 assert(W==CrMdssPlatformPhy&&O<sizeof(F.Phy));
 if(O==0x140)return 1;if(O==0x6b0&&!F.FailLock)return 1;
 if(O==0x3c&&F.ForeignPhy)return 1;return F.Phy[O/4];
}
static VOID Write(VOID *C,CR_MDSS_PLATFORM_WINDOW W,UINT32 O,UINT32 V) {
 assert(C==&F&&W==CrMdssPlatformPhy&&!(O&3)&&O<sizeof(F.Phy)&&F.NativeFenced&&!F.InheritedLive);Accessible();++F.Writes;if(F.DropPhyOffset!=O+1)F.Phy[O/4]=V;
}
static VOID Fence(VOID *C){assert(C==&F);}
static VOID Delay(VOID *C,UINT32 Us){assert(C==&F);F.Delay+=Us;}
static CR_STATUS Map(VOID *C,VOID *O,UINT64 Phys,UINTN Bytes,CONST UINT16 *Sids,UINT32 Count,UINT64 *Iova,BOOLEAN *Owned){
 assert(C==&F&&Phys==0xb8000000&&Bytes==0x800000&&Count==4&&(F.Ext||F.ResourcesOnly));Stable(O);
 CONST UINT16 Expected[]={0x2800,0x2802,0x2c00,0x2c02};assert(!memcmp(Sids,Expected,sizeof(Expected)));
 F.Map=TRUE;*Owned=TRUE;*Iova=F.OverrunIova?0xfff00000:0x81000000;return Mutate();
}
static CR_STATUS Unmap(VOID *C,VOID *O){assert(C==&F&&F.Map);Stable(O);if(F.FailUnmap)return CR_DEVICE_ERROR;CR_STATUS S=Mutate();if(!CR_ERROR(S))F.Map=FALSE;return S;}
static CR_STATUS Gpio(VOID *C,VOID *O,UINT32 Pin,BOOLEAN High){
 assert(C==&F&&F.Native&&F.Rail[2]);Stable(O);if(Pin==107&&!High&&F.FailResetOff)return CR_DEVICE_ERROR;
 CR_STATUS S=Mutate();if(CR_ERROR(S))return S;
 if(Pin==9)F.Power[0]=High;else if(Pin==109)F.Power[1]=High;else{assert(Pin==107);F.Reset=High;++F.Resets;}return S;
}
static CR_STATUS I2cWrite(VOID *C,VOID *O,UINT8 Reg,UINT8 Value){
 assert(C==&F&&F.Native&&F.Power[0]&&F.Power[1]&&F.Reset);Stable(O);++F.SerialWrites;
 if(Reg==0xff&&Value==0x80&&F.Page==0xd0&&F.FailUnlock)return CR_DEVICE_ERROR;
 if(Reg==0xee)assert(F.Page==0x80);
 /* Model an accepted transfer that subsequently reports an error. */
 if(Reg==0xff)F.Page=Value;else F.Bridge[F.Page][Reg]=Value;
 if(Reg==0xee&&Value==0)++F.Unlocks;return Mutate();
}
static CR_STATUS I2cRead(VOID *C,VOID *O,UINT8 Reg,UINT8 *Value){assert(C==&F&&F.Native&&F.Reset);Stable(O);++F.SerialReads;*Value=F.Bridge[F.Page][Reg];return Mutate();}
static CR_STATUS Post(VOID *C,VOID *O,CONST CR_MDSS_PLATFORM_CONTRACT *M,UINT64 Iova){
 assert(C==&F&&M->Mode.Width==1920&&Iova==0x81000000&&F.Map&&(F.Ext||F.ResourcesOnly));Stable(O);if(F.FailPost)return CR_DEVICE_ERROR;F.Post=TRUE;return CR_SUCCESS;
}
static CR_STATUS Reclaim(VOID *C,VOID *O){assert(C==&F&&F.Post);Stable(O);if(F.FailReclaim)return CR_DEVICE_ERROR;F.Post=FALSE;return CR_SUCCESS;}
static CR_MDSS_PLATFORM_CONTRACT Config(void){
 CR_MDSS_PLATFORM_CONTRACT C={0};C.Revision=1;C.Board=CR_MDSS_PLATFORM_HDK8450;C.BootGeneration=19;
 C.DpuPhysical=0xae01000;C.DsiPhysical=0xae94000;C.FrameBufferPhysical=0xb8000000;C.FrameBufferBytes=0x800000;
 CR_MDSS_MODE *M=&C.Mode;M->Soc=CR_MDSS_SOC_SM8450;M->Width=1920;M->Height=1080;M->Stride=7680;
 M->HSync=44;M->HBackPorch=148;M->HFrontPorch=88;M->VSync=5;M->VBackPorch=36;M->VFrontPorch=4;
 M->RefreshQ16=60U<<16;M->Lanes=4;M->TrafficMode=1;M->VideoFlags=CR_MDSS_VIDEO_LP_MASK;return C;
}
static CR_MDSS_PLATFORM_IO Io(void){
 CR_MDSS_PLATFORM_IO I={&F,Qualify,Claim,NativeFence,NativeRelease,Ready,Clock,Rail,Bandwidth,External,Read,Write,Fence,Delay,Map,Unmap,Gpio,I2cWrite,I2cRead,Post,Reclaim,0x800,0x10000};return I;
}
static VOID Reset(CR_MDSS_PLATFORM *P){
 memset(&F,0,sizeof(F));memset(P,0,sizeof(*P));F.SerialIncarnation=12;F.Phy[0]=0x20;F.Bridge[0xb0][0x21]=1;
 F.Bridge[0xd0][0x16]=0xa0;F.Bridge[0xd0][0x18]=0xb0;F.Bridge[0xd0][0x1a]=0xc0;
 CR_MDSS_PLATFORM_IO I=Io();CR_MDSS_PLATFORM_CONTRACT C=Config();assert(CrMdssPlatformInit(P,&I,&C)==CR_SUCCESS);
}
static VOID Acquire(CR_MDSS_PLATFORM *P,VOID *O){UINT64 D;UINT32 T;BOOLEAN Owned;assert(CrMdssPlatformAcquire(P,O,&D,&T,&Owned)==CR_SUCCESS&&Owned&&D==0x81000000&&T!=0);}
static VOID Empty(CR_MDSS_PLATFORM *P){assert(!F.Native&&!F.Map&&!F.Ext&&!F.Power[0]&&!F.Power[1]&&!F.Reset&&!P->Owner);for(UINTN I=0;I<10;++I)assert(!F.Clock[I]);for(UINTN I=0;I<3;++I)assert(!F.Rail[I]);for(UINTN I=0;I<2;++I)assert(!F.Path[I]);assert(CrMdssPlatformUninit(P)==CR_SUCCESS);}
int main(void){
 CR_MDSS_PLATFORM P={0};int Owner,Other;UINT64 D;UINT32 T;BOOLEAN Owned;CR_MDSS_PLATFORM_CONTRACT C;
 Reset(&P);assert(CrMdssPlatformQuery(&P,&C)==CR_SUCCESS&&F.Mutations==0&&F.Reads==0);
 CR_MDSS_PLATFORM_IO Bad=Io();CR_MDSS_PLATFORM Short={0};CR_MDSS_PLATFORM_CONTRACT Good=Config();
 Bad.PhyBytes=0x760;assert(CrMdssPlatformInit(&Short,&Bad,&Good)==CR_BAD_BUFFER_SIZE&&F.Reads==0);
 Bad=Io();Bad.DispCcBytes=0x9000;assert(CrMdssPlatformInit(&Short,&Bad,&Good)==CR_BAD_BUFFER_SIZE&&F.Reads==0);
 for(UINT32 Fact=1;Fact<=16;Fact<<=1){F.RemovedFact=Fact;assert(CrMdssPlatformQuery(&P,&C)==CR_NOT_READY&&F.Mutations==0);}F.RemovedFact=0;
 F.BadQualification=TRUE;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_NOT_READY&&!Owned&&F.Mutations==0);F.BadQualification=FALSE;
 Acquire(&P,&Owner);assert(F.Phy[0x760/4]==0x22&&F.Phy[0x03c/4]==1&&F.Delay>=500);
 UINTN Before=F.Mutations;Acquire(&P,&Owner);assert(F.Mutations==Before);
 assert(CrMdssPlatformAcquire(&P,&Other,&D,&T,&Owned)==CR_BUSY&&!Owned);
 assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_SUCCESS);assert(F.Bridge[0xd0][0x12]==0x98&&F.Bridge[0xd0][0x16]==0xa0&&F.Bridge[0xd0][0x18]==0xb0&&F.Bridge[0xd0][0x1a]==0xc0&&F.Unlocks==2&&F.Delay>=440000);
 assert(CrMdssPlatformRelease(&P,&Other)==CR_BUSY);assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 /* Every fallible native/rail/path/clock/external/map acquisition stage. */
 for(UINTN Failure=1;Failure<=19;++Failure){Reset(&P);F.FailMutation=Failure;assert(CR_ERROR(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned))&&Owned&&P.Owner==&Owner);F.FailMutation=0;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);}
 Reset(&P);F.FailNativeFence=TRUE;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_DEVICE_ERROR&&Owned&&P.NativeOwned&&!P.NativeFenced&&F.InheritedLive&&!F.Writes&&!F.Map&&!F.Ext);
 Before=F.Mutations;assert(CrMdssPlatformRelease(&P,&Owner)==CR_DEVICE_ERROR&&P.Owner==&Owner&&F.InheritedLive&&F.Clock[9]&&F.Rail[2]&&F.NativeFences==2&&!F.Writes);
 assert(F.Mutations>Before&&CrMdssPlatformTransferToPost(&P,&Owner)==CR_NOT_READY);F.FailNativeFence=FALSE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS&&!F.InheritedLive);Empty(&P);
 Reset(&P);F.ForeignPhy=TRUE;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_BUSY&&Owned&&F.Writes==0&&!P.PhyTouched);assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS&&F.Writes==0);Empty(&P);
 Reset(&P);F.FailLock=TRUE;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_TIMEOUT&&Owned&&P.PhyTouched&&F.Delay>=5501);F.FailLock=FALSE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 Reset(&P);F.OverrunIova=TRUE;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_UNSUPPORTED&&Owned&&P.MappingOwned);assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 Reset(&P);Acquire(&P,&Owner);F.FailReady=TRUE;Before=F.Writes;
 assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_NOT_READY&&F.SerialWrites==0);
 assert(CrMdssPlatformRelease(&P,&Owner)==CR_NOT_READY&&P.PhyTouched&&F.Writes==Before&&F.Clock[9]);
 F.FailReady=FALSE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 Reset(&P);Acquire(&P,&Owner);F.FailUnmap=TRUE;Before=F.Writes;assert(CrMdssPlatformRelease(&P,&Owner)==CR_DEVICE_ERROR&&P.MappingOwned&&F.Ext&&F.Clock[9]&&F.Writes==Before);F.FailUnmap=FALSE;F.FailExtOff=TRUE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_DEVICE_ERROR&&!P.MappingOwned&&F.Ext&&F.Writes==Before);F.FailExtOff=FALSE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 /* A dropped PHY-off write cannot release access/domain/rail ownership. */
 CONST UINT32 DropOffsets[]={0xa4,0x14,0x3c,0x1c,0x528,0x24,0x214,0x1ac,0xa0};
 for(UINT32 N=0;N<sizeof(DropOffsets)/sizeof(DropOffsets[0]);++N){
   Reset(&P);Acquire(&P,&Owner);F.Phy[DropOffsets[N]/4]|=0xff;F.DropPhyOffset=DropOffsets[N]+1;
   assert(CrMdssPlatformRelease(&P,&Owner)==CR_TIMEOUT&&P.PhyTouched&&P.CleanupPending&&P.Owner==&Owner);
   Accessible();assert(F.Native&&F.Clock[9]&&F.Rail[2]);F.DropPhyOffset=0;
   assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 }
 /* Every resource withdrawal can fail without retiring its permanent owner. */
 Reset(&P);Acquire(&P,&Owner);Before=F.Mutations;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);UINTN ReleaseCalls=F.Mutations-Before;Empty(&P);
 for(UINTN Failure=1;Failure<=ReleaseCalls;++Failure){Reset(&P);Acquire(&P,&Owner);F.FailMutation=F.Mutations+Failure;assert(CR_ERROR(CrMdssPlatformRelease(&P,&Owner))&&P.Owner==&Owner&&P.CleanupPending);F.FailMutation=0;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);}
 Reset(&P);Acquire(&P,&Owner);F.FailUnlock=TRUE;assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_DEVICE_ERROR&&P.McuLockPending);Before=F.SerialWrites;assert(CrMdssPlatformRelease(&P,&Owner)==CR_DEVICE_ERROR&&P.MappingOwned&&F.Ext&&F.SerialWrites==Before+1&&F.Reset);F.FailUnlock=FALSE;F.FailResetOff=TRUE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_DEVICE_ERROR&&!P.McuLockPending&&F.Map);F.FailResetOff=FALSE;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 /* Accepted writes followed by errors across GPIO/identity/MCU/video setup. */
 Reset(&P);Acquire(&P,&Owner);Before=F.Mutations;assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_SUCCESS);UINTN PanelCalls=F.Mutations-Before;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);
 for(UINTN Failure=1;Failure<=PanelCalls;++Failure){Reset(&P);Acquire(&P,&Owner);F.FailMutation=F.Mutations+Failure;assert(CR_ERROR(CrMdssPlatformPreparePanel(&P,&Owner))&&P.CleanupPending);F.FailMutation=0;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS);Empty(&P);}
 Reset(&P);Acquire(&P,&Owner);assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_SUCCESS);F.FailPost=TRUE;assert(CrMdssPlatformTransferToPost(&P,&Owner)==CR_DEVICE_ERROR&&P.Owner==&Owner&&!P.PostOwned);F.FailPost=FALSE;
 ++F.SerialIncarnation;assert(CrMdssPlatformTransferToPost(&P,&Owner)==CR_NOT_READY&&P.Owner==&Owner);--F.SerialIncarnation;
 assert(CrMdssPlatformTransferToPost(&P,&Owner)==CR_SUCCESS&&P.PostOwned&&P.Owner==NULL&&F.Native&&F.Map);assert(CrMdssPlatformUninit(&P)==CR_BUSY);
 ++F.SerialIncarnation;Before=F.Mutations;assert(CrMdssPlatformAcquire(&P,&Other,&D,&T,&Owned)==CR_NOT_READY&&!Owned&&P.PostOwned&&F.Mutations==Before);--F.SerialIncarnation;
 F.FailReclaim=TRUE;Before=F.Mutations;assert(CrMdssPlatformAcquire(&P,&Other,&D,&T,&Owned)==CR_DEVICE_ERROR&&!Owned&&P.PostOwned&&F.Mutations==Before);F.FailReclaim=FALSE;Acquire(&P,&Other);assert(!P.PostOwned&&F.NativeClaims==2);assert(CrMdssPlatformRelease(&P,&Other)==CR_SUCCESS);Empty(&P);
 /* Resource-only admission never writes PHY/external clocks and cannot be
    interchanged with legacy ownership, including retained POST. */
 Reset(&P);F.ResourcesOnly=TRUE;
 assert(CrMdssPlatformAcquireResources(&P,&Owner,&D,&Owned)==CR_SUCCESS&&Owned&&P.ResourcesOnly);
 assert(P.Ready&&P.NativeFenced&&F.Map&&!F.Ext&&!F.Writes&&!P.PhyTouched&&!P.ExternalTouched);
 Before=F.Mutations;assert(CrMdssPlatformAcquire(&P,&Owner,&D,&T,&Owned)==CR_BUSY&&F.Mutations==Before);
 assert(CrMdssPlatformAcquireResources(&P,&Other,&D,&Owned)==CR_BUSY&&!Owned);
 assert(CrMdssPlatformPreparePanel(&P,&Owner)==CR_SUCCESS);
 assert(CrMdssPlatformTransferToPost(&P,&Owner)==CR_SUCCESS&&P.PostOwned);
 Before=F.Mutations;assert(CrMdssPlatformAcquire(&P,&Other,&D,&T,&Owned)==CR_BUSY&&!Owned&&F.Mutations==Before);
 UINTN BeforePost=F.Mutations;
 assert(CrMdssPlatformAcquireResources(&P,&Other,&D,&Owned)==CR_BUSY&&!Owned&&P.PostOwned&&F.Mutations==BeforePost);
 F.FailReclaim=TRUE;assert(CrMdssPlatformReclaimPost(&P,&Other,&Owned)==CR_DEVICE_ERROR&&!Owned&&P.PostOwned);
 F.FailReclaim=FALSE;assert(CrMdssPlatformReclaimPost(&P,&Other,&Owned)==CR_SUCCESS&&Owned&&P.Owner==&Other&&F.Map);
 assert(CrMdssPlatformRelease(&P,&Other)==CR_SUCCESS&&!F.Writes);Empty(&P);
 for(UINTN Kind=0;Kind<3;++Kind)for(UINTN Failure=1;Failure<=18;++Failure){Reset(&P);F.ResourcesOnly=TRUE;F.FailMutation=Failure;F.FailureStatus=Kind==0?CR_DEVICE_ERROR:Kind==1?1:0x103;
   assert(CR_ERROR(CrMdssPlatformAcquireResources(&P,&Owner,&D,&Owned))&&Owned&&P.Owner==&Owner);
   F.FailMutation=0;assert(CrMdssPlatformRelease(&P,&Owner)==CR_SUCCESS&&!F.Writes);Empty(&P);
 }
 puts("MDSS platform: actual PHY/bridge, qualification, partial acquisition, retained cleanup and POST tests passed");return 0;
}
