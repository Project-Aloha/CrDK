/* Actual shared combo-PHY under EFI and NT ABI, checked provider faults.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/UsbDpPhyCrLib.h>
#include <oskal/cr_atomic.h>
STATIC CR_USB_DP_PHY Phy;
STATIC UINT32 Qmp[CR_USB_DP_PHY_QMP_BYTES/4],Hs[CR_USB_DP_PHY_HS_BYTES/4];
STATIC UINT8 Fsa[32];
STATIC BOOLEAN ClockOwned[4],RailOwned[6],ComboReset,UsbReset;
STATIC UINT32 Calls,FailAt,FreezeAt,AfterFailure,Time,Reads,Writes,ClockOn[4],ClockOff[4],RailOn[6],RailOff[6];
STATIC BOOLEAN Commit,Failed,ReadyLost,PllFail,DropTune,Pending,FreezeSucceeds,BadClockAck,BadRailAck;
STATIC INT32 UsbOwner,DpOwner,ForeignOwner;
STATIC CONST CR_USB_DP_PHY_CONFIG Config={1,0x8450,1,{{0x100002000ULL,sizeof(Qmp)},{0x300004000ULL,sizeof(Hs)}}};
#ifdef _KERNEL_MODE
STATIC KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID){return Irql;}
VOID KeInitializeMutex(KMUTEX *M,ULONG L){(VOID)L;*M=0;}
NTSTATUS KeWaitForSingleObject(VOID *M,KWAIT_REASON R,KPROCESSOR_MODE K,BOOLEAN A,LARGE_INTEGER *T){(VOID)R;(VOID)K;(VOID)A;(VOID)T;assert(!*(KMUTEX*)M);*(KMUTEX*)M=1;return STATUS_SUCCESS;}
LONG KeReleaseMutex(KMUTEX *M,BOOLEAN W){(VOID)W;assert(*M==1);*M=0;return 0;}
VOID KeStallExecutionProcessor(ULONG U){Time+=U;}
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE M,BOOLEAN A,LARGE_INTEGER *I){(VOID)M;(VOID)A;Time+=(UINT32)(-I->QuadPart/10);return STATUS_SUCCESS;}
#else
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *L){*L=0;return L;}
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *L){assert(!*L);*L=1;return L;}
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *L){assert(*L==1);*L=0;return L;}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Old,UINT32 New){UINT32 V=*P;if(V==Old)*P=New;return V;}
VOID EFIAPI MemoryFence(VOID){}
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
UINTN EFIAPI MicroSecondDelay(UINTN U){Time+=(UINT32)U;return U;}
#endif
STATIC BOOLEAN Op(VOID){
 if(Failed)++AfterFailure;
 ++Calls;
 if(Calls==FreezeAt){assert(CrUsbDpPhyFreeze(&Phy)==CR_BUSY);return FreezeSucceeds;}
 if(Calls==FailAt){Failed=TRUE;return FALSE;}
 return TRUE;
}
STATIC CR_STATUS Result(BOOLEAN Ok){
 if(Ok)return CR_SUCCESS;
#ifdef _KERNEL_MODE
 if(Pending)return (CR_STATUS)0x103;
#endif
 return CR_DEVICE_ERROR;
}
STATIC CR_STATUS Ready(VOID *C){assert(C==&Phy);return ReadyLost?CR_NOT_READY:CR_SUCCESS;}
STATIC CR_STATUS Read(VOID *C,CR_USB_DP_PHY_WINDOW W,UINT32 O,UINT32 *V){
 assert(C==&Phy&&W<CrUsbDpPhyWindows&&!(O&3)&&O<=Config.View[W].Bytes-4);
 BOOLEAN Ok=Op();++Reads;
 if(!Ok)return Result(Ok);
 if(W==CrUsbDpPhyQmp&&(O==0x2178||O==0x2140||O==0x2adc)){*V=PllFail?0:O==0x2178?1:3;return CR_SUCCESS;}
 *V=(W==CrUsbDpPhyQmp?Qmp:Hs)[O/4];return CR_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,CR_USB_DP_PHY_WINDOW W,UINT32 O,UINT32 V){
 assert(C==&Phy&&W<CrUsbDpPhyWindows&&!(O&3)&&O<=Config.View[W].Bytes-4);
 BOOLEAN Ok=Op();++Writes;
 if(Ok||Commit){
  if(W==CrUsbDpPhyHs&&(O==0x6c||O==0x70||O==0x74)){
   assert(Hs[0x50/4]&2);assert(Hs[0x94/4]&2);
   if(DropTune)return Result(Ok);
  }
  (W==CrUsbDpPhyQmp?Qmp:Hs)[O/4]=V;
 }
 return Result(Ok);
}
STATIC UINT32 ClockIndex(CONST CHAR8 *N){
 STATIC CONST CHAR8 *Names[] = {"gcc_usb3_0_clkref_en","gcc_usb3_prim_phy_aux_clk","gcc_usb3_prim_phy_com_aux_clk","gcc_usb3_prim_phy_pipe_clk"};
 for(UINT32 I=0;I<4;++I)if(!strcmp(N,Names[I]))return I;
 assert(0);return 0;
}
STATIC UINT32 RailIndex(CONST CHAR8 *N){
 STATIC CONST CHAR8 *Names[]={"bobc1","ldob1","ldob6","ldob5","ldoc1","ldob2"};
 for(UINT32 I=0;I<6;++I)if(!strcmp(N,Names[I]))return I;
 assert(0);return 0;
}
STATIC CR_STATUS Clock(VOID *C,CONST CHAR8 *N,UINT32 Rate,BOOLEAN On,BOOLEAN *Owned){
 assert(C==&Phy);UINT32 I=ClockIndex(N);
 assert(Rate==(I==1||I==2?19200000U:0));
 if(On){assert(!ClockOwned[I]);assert(!ComboReset&&!UsbReset);++ClockOn[I];}
 else {assert(ClockOwned[I]&&*Owned);++ClockOff[I];}
 BOOLEAN Ok=Op();
 if(Ok||(Commit&&On))ClockOwned[I]=On;
 *Owned=BadClockAck&&On?FALSE:ClockOwned[I];return Result(Ok);
}
STATIC CR_STATUS Rail(VOID *C,CONST CHAR8 *N,UINT32 V,UINT8 M,BOOLEAN On,BOOLEAN *Owned){
 assert(C==&Phy);UINT32 I=RailIndex(N);STATIC CONST UINT32 Mv[]={3296,912,1200,880,1800,3072};
 assert(V==Mv[I]&&M==(I?7:6));
 if(On){assert(!RailOwned[I]);++RailOn[I];}else {assert(RailOwned[I]&&*Owned);++RailOff[I];}
 BOOLEAN Ok=Op();
 /* Failed off retains the provider lease even if its write committed. */
 if(Ok||(Commit&&On))RailOwned[I]=On;
 *Owned=BadRailAck&&On?FALSE:RailOwned[I];return Result(Ok);
}
STATIC CR_STATUS Reset(VOID *C,CONST CHAR8 *N,BOOLEAN Assert){
 assert(C==&Phy);BOOLEAN Ok=Op();
 if(Ok||Commit){if(!strcmp(N,"gcc_usb3_dp_phy_prim_bcr"))ComboReset=Assert;
 else if(!strcmp(N,"gcc_usb3_phy_prim_bcr"))UsbReset=Assert;
 else assert(!strcmp(N,"gcc_qusb2phy_prim_bcr"));}
 return Result(Ok);
}
STATIC CR_STATUS SwitchRead(VOID *C,UINT8 R,UINT8 *V){
 assert(C==&Phy&&R<32);BOOLEAN Ok=Op();if(Ok)*V=Fsa[R];return Result(Ok);
}
STATIC CR_STATUS SwitchWrite(VOID *C,UINT8 R,UINT8 V,BOOLEAN *Dispatched){
 assert(C==&Phy&&R<32);*Dispatched=TRUE;BOOLEAN Ok=Op();if(Ok||Commit)Fsa[R]=V;return Result(Ok);
}
STATIC CONST CR_USB_DP_PHY_IO Io={&Phy,Ready,Read,Write,Clock,Rail,Reset,SwitchRead,SwitchWrite};
STATIC CR_USB_DP_PHY_ROUTE Route(BOOLEAN Dp,BOOLEAN Reverse,BOOLEAN Four){
 CR_USB_DP_PHY_ROUTE R={0};R.PdReady=R.Connected=R.Usb3=TRUE;R.Orientation=Reverse?2:1;
 R.DpActive=Dp;R.DpLanes=Dp?(Four?4:2):0;R.PinAssignment=Dp?(Four?3:4):0;return R;
}
STATIC VOID New(VOID){
 memset(&Phy,0,sizeof(Phy));memset(Qmp,0,sizeof(Qmp));memset(Hs,0,sizeof(Hs));memset(Fsa,0,sizeof(Fsa));
 memset(ClockOwned,0,sizeof(ClockOwned));memset(RailOwned,0,sizeof(RailOwned));
 memset(ClockOn,0,sizeof(ClockOn));memset(ClockOff,0,sizeof(ClockOff));memset(RailOn,0,sizeof(RailOn));memset(RailOff,0,sizeof(RailOff));
 Fsa[0]=9;Hs[0x6c/4]=0x76543200;Hs[0x70/4]=0xfedcba00;Hs[0x74/4]=0xabcdef00;
 Calls=FailAt=FreezeAt=AfterFailure=Time=Reads=Writes=0;
 Commit=Failed=ReadyLost=PllFail=DropTune=Pending=ComboReset=UsbReset=FreezeSucceeds=BadClockAck=BadRailAck=FALSE;
 assert(CrUsbDpPhyInit(&Phy,&Config,&Io)==CR_SUCCESS);
}
STATIC VOID Clean(VOID){
 FailAt=FreezeAt=0;Failed=Commit=Pending=FALSE;
 if(Phy.Owner[0])assert(CrUsbDpPhyRelease(&Phy,&UsbOwner,CrUsbDpPhyUsb)==CR_SUCCESS);
 if(Phy.Owner[1])assert(CrUsbDpPhyRelease(&Phy,&DpOwner,CrUsbDpPhyDp)==CR_SUCCESS);
 assert(!Phy.CommonReady&&!Phy.CommonTouched&&!Phy.UsbTouched&&!Phy.DpTouched&&!Phy.SwitchTouched);
 for(UINT32 I=0;I<4;++I)assert(!Phy.Clocks[I].Owned&&!ClockOwned[I]);
 for(UINT32 I=0;I<6;++I)assert(!Phy.Rails[I].Owned&&!RailOwned[I]);
 assert(CrUsbDpPhyUninit(&Phy)==CR_SUCCESS&&!Phy.Initialized);
}
STATIC UINT64 Hash(VOID){UINT64 H=1469598103934665603ULL;
 for(UINT32 I=0;I<sizeof(Qmp)/4;++I){H^=Qmp[I];H*=1099511628211ULL;}
 for(UINT32 I=0;I<sizeof(Hs)/4;++I){H^=Hs[I];H*=1099511628211ULL;}
 for(UINT32 I=0;I<32;++I){H^=Fsa[I];H*=1099511628211ULL;}return H;}
int main(int Argc,char **Argv){
 BOOLEAN Owned;UINT32 Acquires=0,Powers=0,Releases=0;
 for(UINT32 Reverse=0;Reverse<2;++Reverse)for(UINT32 Four=0;Four<2;++Four)for(UINT32 Ss=0;Ss<2;++Ss)
 for(UINT32 First=0;First<2;++First)for(UINT32 Rate=1620;Rate<=8100;Rate=Rate==1620?2700:Rate==2700?5400:Rate==5400?8100:8101){
  New();CR_USB_DP_PHY_ROUTE R=Route(TRUE,(BOOLEAN)Reverse,(BOOLEAN)Four);
  for(UINT32 I=0;I<2;++I){CR_USB_DP_PHY_CLIENT C=(CR_USB_DP_PHY_CLIENT)(First^I);
   assert(CrUsbDpPhyAcquire(&Phy,C?&DpOwner:&UsbOwner,C,&R,(BOOLEAN)(C?0:Ss),&Owned)==CR_SUCCESS&&Owned);}
  assert(CrUsbDpPhyPowerDp(&Phy,&DpOwner,Rate,Four?4:2)==CR_SUCCESS);
  assert(CrUsbDpPhySetDpLevels(&Phy,&DpOwner,1,1)==CR_SUCCESS);
  assert(Hs[0x6c/4]==0x765432e6&&Hs[0x70/4]==0xfedcba0c&&Hs[0x74/4]==0xabcdef17);
  assert(Fsa[4]==(Four?0xe0:0xf8)&&Fsa[5]==((Four?0:0x18)|(Reverse?0x60:0)));
  if(Argc>1&&!strcmp(Argv[1],"parity"))printf("%u %u %u %u %u %016llx\n",Reverse,Four,Ss,First,Rate,(unsigned long long)Hash());
  Clean();
 }
 /* Every dispatched cold USB prerequisite/read/write failure retains
    the exact owner and permits reverse cleanup without duplicate votes. */
 New();CR_USB_DP_PHY_ROUTE R=Route(TRUE,FALSE,FALSE);
 assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_SUCCESS);UINT32 N=Calls,StartupCalls=N;Clean();
 for(UINT32 Committed=0;Committed<2;++Committed)for(UINT32 I=1;I<=N;++I){
  New();FailAt=I;Commit=(BOOLEAN)Committed;
  assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_DEVICE_ERROR&&Owned);
  assert(!Phy.Ready[0]&&Phy.Owner[0]==&UsbOwner&&AfterFailure==0);
  UINT32 Before=Calls;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_ALREADY_STARTED&&!Owned&&Calls==Before);
  Clean();++Acquires;
 }
 New();assert(CrUsbDpPhyAcquire(&Phy,&DpOwner,CrUsbDpPhyDp,&R,FALSE,&Owned)==CR_SUCCESS);
 Calls=0;assert(CrUsbDpPhyPowerDp(&Phy,&DpOwner,8100,2)==CR_SUCCESS);N=Calls;Clean();
 for(UINT32 I=1;I<=N;++I){New();assert(CrUsbDpPhyAcquire(&Phy,&DpOwner,CrUsbDpPhyDp,&R,FALSE,&Owned)==CR_SUCCESS);
  Calls=0;FailAt=I;Commit=TRUE;assert(CrUsbDpPhyPowerDp(&Phy,&DpOwner,8100,2)==CR_DEVICE_ERROR);
  assert(!Phy.DpRate&&!Phy.DpLanes&&Phy.Owner[1]==&DpOwner&&!AfterFailure);Clean();++Powers;}
 New();assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_SUCCESS);
 Calls=0;assert(CrUsbDpPhyRelease(&Phy,&UsbOwner,CrUsbDpPhyUsb)==CR_SUCCESS);N=Calls;Clean();
 for(UINT32 I=1;I<=N;++I){New();assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_SUCCESS);
  Calls=0;FailAt=I;assert(CrUsbDpPhyRelease(&Phy,&UsbOwner,CrUsbDpPhyUsb)==CR_DEVICE_ERROR);
  assert(Phy.Owner[0]==&UsbOwner&&!Phy.Ready[0]&&!AfterFailure);Clean();++Releases;}
 /* Foreign/invalid route operations have no effect on an existing owner. */
 New();assert(CrUsbDpPhyAcquire(&Phy,&DpOwner,CrUsbDpPhyDp,&R,FALSE,&Owned)==CR_SUCCESS);
 UINT32 Before=Calls;assert(CrUsbDpPhyRelease(&Phy,&ForeignOwner,CrUsbDpPhyDp)==CR_BUSY&&Calls==Before);
 assert(CrUsbDpPhyPowerDp(&Phy,&ForeignOwner,2700,2)==CR_BUSY&&Calls==Before);
 assert(CrUsbDpPhySetDpLevels(&Phy,&ForeignOwner,0,0)==CR_BUSY&&Calls==Before);
 R.Orientation=2;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_NOT_READY&&!Owned&&Calls==Before);R.Orientation=1;
 assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,(CR_USB_DP_PHY_CLIENT)-1,&R,TRUE,&Owned)==CR_INVALID_PARAMETER&&!Owned);
 assert(CrUsbDpPhyUninit(&Phy)==CR_BUSY);Clean();
 New();DropTune=TRUE;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_DEVICE_ERROR&&Phy.UsbTouched);DropTune=FALSE;Clean();
 New();PllFail=TRUE;assert(CrUsbDpPhyAcquire(&Phy,&DpOwner,CrUsbDpPhyDp,&R,FALSE,&Owned)==CR_SUCCESS);
 UINT32 Start=Time;assert(CrUsbDpPhyPowerDp(&Phy,&DpOwner,2700,2)==CR_TIMEOUT&&Time-Start==10000);PllFail=FALSE;Clean();
 New();ReadyLost=TRUE;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_NOT_READY&&!Calls&&!Phy.Ready[0]);ReadyLost=FALSE;Clean();
 New();BadClockAck=TRUE;
 assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_NOT_READY&&Owned);
 assert(Phy.Clocks[0].Owned&&Phy.Clocks[0].Status==CR_NOT_READY&&ClockOwned[0]&&!Phy.Ready[0]);BadClockAck=FALSE;Clean();
 New();BadRailAck=TRUE;
 assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_NOT_READY&&Owned);
 assert(Phy.Rails[0].Owned&&Phy.Rails[0].Status==CR_NOT_READY&&RailOwned[0]&&!Phy.Ready[0]);BadRailAck=FALSE;Clean();
 /* Freeze from an actual callback closes every subsequent callback, and
    keeps the partially committed lease until reboot/authoritative handoff. */
 New();FreezeAt=5;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned)==CR_DEVICE_ERROR&&Owned);
 Before=Calls;assert(CrUsbDpPhyFreeze(&Phy)==CR_SUCCESS);
 assert(CrUsbDpPhyRelease(&Phy,&UsbOwner,CrUsbDpPhyUsb)==CR_BUSY&&Calls==Before);
 assert(CrUsbDpPhyAcquire(&Phy,&DpOwner,CrUsbDpPhyDp,&R,FALSE,&Owned)==CR_BUSY&&!Owned&&Calls==Before);
 for(UINT32 I=1;I<=StartupCalls;++I){
  New();FreezeAt=I;FreezeSucceeds=TRUE;
  CR_STATUS S=CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,TRUE,&Owned);
  assert((S==CR_BUSY||S==CR_SUCCESS)&&Owned&&Calls==I&&!Phy.Active);
  Before=Calls;assert(CrUsbDpPhyFreeze(&Phy)==CR_SUCCESS);
  assert(CrUsbDpPhyRelease(&Phy,&UsbOwner,CrUsbDpPhyUsb)==CR_BUSY&&Calls==Before);
 }
 /* Invalid assignments never call IO or overwrite existing output. */
 for(UINT32 I=0;I<8;++I){CR_USB_DP_PHY X={0},Saved=X;CR_USB_DP_PHY_CONFIG C=Config;
  switch(I){case 0:C.View[0].Bytes--;break;case 1:C.View[1].Bytes--;break;case 2:C.View[0].Address++;break;case 3:C.View[1].Address=0;break;case 4:C.View[0].Address=(UINTN)-4;break;case 5:C.View[1].Address=C.View[0].Address+4;break;case 6:C.Soc=0;break;case 7:C.Board=0;break;}
  assert(CrUsbDpPhyInit(&X,&C,&Io)==CR_INVALID_PARAMETER&&!memcmp(&X,&Saved,sizeof(X)));}
#ifdef _KERNEL_MODE
 New();Pending=TRUE;FailAt=1;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_NOT_READY&&Owned);Clean();
 New();Irql=DISPATCH_LEVEL;assert(CrUsbDpPhyAcquire(&Phy,&UsbOwner,CrUsbDpPhyUsb,&R,FALSE,&Owned)==CR_INVALID_PARAMETER&&!Calls);
 assert(CrUsbDpPhyFreeze(&Phy)==CR_SUCCESS&&!Calls);Irql=PASSIVE_LEVEL;
#endif
 if(Argc==1)printf("PHY portable: %u startup/%u DP/%u release faults, shared/foreign owners, tuning/deadline/freeze/bounds/IRQL PASS\n",Acquires,Powers,Releases);
 return 0;
}
