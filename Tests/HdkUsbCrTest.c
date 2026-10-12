/* Actual fixed-host journal + board profiles, host hardware boundary only.
 * Catches wrong secondary window/override, early publication on failure,
 * lost partial ownership, and release underneath a failed reset/halt.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/HdkUsbCrLib.h>
static CR_HDK_USB U;
static UINT32 C[0x100000/4],H[0x114/4],E;
static UINT32 Calls,Writes,FaultAt,ReleaseCalls,DelayUs,AttachCalls;
static UINT32 OverrideUpper,OverrideMismatch;
static BOOLEAN ReleaseFail,HaltFail,ResetFail,NoOwner;
static EFI_STATUS Step(VOID){return ++Calls==FaultAt?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS Access(VOID *X){assert(X==&U);return EFI_SUCCESS;}
static EFI_STATUS Floor(VOID *X){assert(X==&U);return Step();}
static EFI_STATUS Read(VOID *X,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 *V){
 assert(X==&U && V && !(O&3));EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;
 if(W==HdkUsbController){assert(O<sizeof C);*V=C[O/4];if(O==0x44 && HaltFail)*V&=~1U;}
 else if(W==HdkUsbHs){assert(O<U.Port->HsPhyBytes);*V=H[O/4];
  /* Public FEMTO tuning registers expose only PARAM_OVRD_MASK0xff even
   * though the bus access remains readl/writel. Model the reported repeated
   * upper lanes independently of the real tuning byte, then fault that byte. */
  if(O>=0x6c&&O<=0x78){*V=(*V&0xffU)|OverrideUpper;*V^=OverrideMismatch;}
 }
 else {assert(W==HdkUsbEud && O==0);*V=E;}return EFI_SUCCESS;
}
static EFI_STATUS Write(VOID *X,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 V){
 assert(X==&U && !(O&3));EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;++Writes;
 if(W==HdkUsbController){assert(O<sizeof C);if(O==0xc40c)C[O/4]-=V&0xfffc;else C[O/4]=V;
  if(O==0x40 && !(V&1) && !HaltFail)C[0x44/4]|=1;}
 else {assert(W==HdkUsbHs && O<U.Port->HsPhyBytes);H[O/4]=V;}return EFI_SUCCESS;
}
static VOID Delay(VOID *X,UINT32 N){assert(X==&U);DelayUs+=N;}
static EFI_STATUS Own(BOOLEAN On,BOOLEAN *O){
 assert(O);EFI_STATUS S=Step();if(On){*O=!NoOwner;return S;}
 ++ReleaseCalls;if(ReleaseFail)return EFI_DEVICE_ERROR;if(S==EFI_SUCCESS)*O=FALSE;return S;
}
static EFI_STATUS Path(VOID *X,UINT32 I,BOOLEAN On,BOOLEAN *O){assert(X==&U && I<2);return Own(On,O);}
static EFI_STATUS Domain(VOID *X,BOOLEAN On,BOOLEAN *O){assert(X==&U);return Own(On,O);}
static EFI_STATUS Clock(VOID *X,CONST CHAR8 *N,UINT64 R,BOOLEAN On,BOOLEAN *O){
 assert(X==&U && N);if(strstr(N,"master")||strstr(N,"cfg_noc")||strstr(N,"aggre"))assert(R==66666667);return Own(On,O);
}
static EFI_STATUS Rail(VOID *X,CONST CR_HDK_USB_RAIL *R,BOOLEAN On,BOOLEAN *O){
 assert(X==&U && R && R->Mode==7);return Own(On,O);
}
static EFI_STATUS Reset(VOID *X,CONST CHAR8 *N,BOOLEAN A){
 assert(X==&U && N);EFI_STATUS S=Step();if(ResetFail)return EFI_DEVICE_ERROR;
 if(S==EFI_SUCCESS && A && strstr(N,"usb30")){C[0x40/4]=0;C[0x44/4]=1;}
 return S;
}
static EFI_STATUS Gpio(VOID *X,UINT32 I,BOOLEAN On,BOOLEAN *O){assert(X==&U && I<2);return Own(On,O);}
static EFI_STATUS Attach(VOID *X){assert(X==&U && !U.Ready);++AttachCalls;return Step();}
static CONST CR_HDK_USB_IO Io={&U,Access,Floor,Read,Write,Delay,Path,Domain,Clock,Rail,Reset,Gpio,Attach,NULL};
static VOID Fresh(VOID){
 memset(&U,0,sizeof U);memset(C,0xa5,sizeof C);memset(H,0xa5,sizeof H);
 C[0xc120/4]=0x33313130;C[0xc140/4]=2;C[0xc1a0/4]=0x3139302a;C[0xc1a4/4]=0;
 C[0]=0x40;C[0x40/4]=0;C[0x44/4]=1;Calls=Writes=FaultAt=ReleaseCalls=DelayUs=AttachCalls=0;
 OverrideUpper=OverrideMismatch=0;
 ReleaseFail=HaltFail=ResetFail=NoOwner=FALSE;E=0;
 assert(CrHdkUsbInit(&U,CrHdkUsbGetTarget(),1,&Io)==EFI_SUCCESS);
}
static VOID Empty(VOID){
 assert(!U.Ready && !U.Domain);for(UINT32 I=0;I<8;I++)assert(!U.Clocks[I]&&!U.PhyClocks[I]);
 for(UINT32 I=0;I<5;I++)assert(!U.Rails[I]);for(UINT32 I=0;I<2;I++)assert(!U.Paths[I]&&!U.Gpios[I]);
}
static VOID PhyReadback(VOID){
 if(!CrHdkUsbGetTarget()->Ports[1].HsOverrideCount)return;
 CONST UINT32 Upper[]={0,0x43434300,0xa5ff0000,0xffffff00};
 for(UINT32 I=0;I<ARRAY_SIZE(Upper);I++){
  Fresh();OverrideUpper=Upper[I];EFI_STATUS S=CrHdkUsbStart(&U);
  if(S!=EFI_SUCCESS)fprintf(stderr,"FEMTO readback failure upper=%08x stage=%u window=%u offset=%x value=%08x stream-calls=%u\n",Upper[I],U.Stage,U.LastWindow,U.LastOffset,U.LastValue,AttachCalls);
  assert(S==EFI_SUCCESS&&U.Ready&&AttachCalls==1);
  assert(H[0x70/4]==0x43&&CrHdkUsbVerifyStopped(&U)==EFI_SUCCESS);
  assert(CrHdkUsbRelease(&U)==EFI_SUCCESS);Empty();
 }
 for(UINT32 Bit=0;Bit<8;Bit++){
  Fresh();OverrideUpper=0x43434300;OverrideMismatch=1U<<Bit;
  assert(CrHdkUsbStart(&U)==EFI_DEVICE_ERROR&&!U.Ready&&!AttachCalls);
  assert(U.LastWindow==HdkUsbHs&&U.LastOffset==0x70&&U.LastValue==(0x43434343U^(1U<<Bit)));
  assert(U.Domain&&U.Clocks[0]&&U.Rails[0]&&!U.UncertainOwner);
  OverrideMismatch=0;assert(CrHdkUsbRelease(&U)==EFI_SUCCESS);Empty();
 }
 puts("FEMTO actual USB core: 32-bit tuning readback0x43434343 qualifies byte0x43; all8 low-byte mismatch controls deny stream adoption PASS");
}
int main(int Count,char **Arguments){
 if(Count>1&&!strcmp(Arguments[1],"phy")){PhyReadback();return 0;}
 Fresh();assert(CrHdkUsbStart(&U)==EFI_SUCCESS && U.Ready);UINT32 Total=Calls;
 assert(U.Port->Controller==0xa800000 && U.Port->StreamId==0x20);
 assert((C[0xc110/4]&0x3008)==0x1008);assert((C[0xc200/4]&0x3c18)==0x2400);
 assert((C[0xf8808/4]&0x109)==9 && !(C[0x40/4]&5) && (C[0x44/4]&1));
 assert(H[0x70/4]==(U.Target->Soc==0x8250?0x43U:0xa5a5a5a5U));
 assert((H[0x94/4]&2)==0 && (H[0x50/4]&2)==0 && (H[0x64/4]&0xc)==4);
 assert(!U.Rails[3]&&!U.Rails[4]);assert(C[0xc410/4]==0xa5a5a5a5); /* no GSI ring1 write */
 assert(CrHdkUsbVerifyStopped(&U)==EFI_SUCCESS);
 C[0xc708/4]=1;assert(CrHdkUsbVerifyStopped(&U)==EFI_NOT_READY);C[0xc708/4]=0;
 C[0x40/4]=1;assert(CrHdkUsbVerifyStopped(&U)==EFI_NOT_READY);C[0x40/4]=0;
 assert(CrHdkUsbRelease(&U)==EFI_SUCCESS);Empty();
 /* Every actual acquisition / register boundary failure with an ownership
  * witness must leave a releasable journal, never a Ready child. */
 for(UINT32 I=1;I<=Total;I++){
  Fresh();FaultAt=I;EFI_STATUS S=CrHdkUsbStart(&U);assert(S!=EFI_SUCCESS&&!U.Ready);
  FaultAt=0;assert(CrHdkUsbRelease(&U)==EFI_SUCCESS);Empty();
 }
 Fresh();NoOwner=TRUE;assert(CrHdkUsbStart(&U)==EFI_NOT_READY&&U.UncertainOwner&&!U.Ready);
 assert(CrHdkUsbRelease(&U)==EFI_ACCESS_DENIED&&!ReleaseCalls);
 Fresh();assert(CrHdkUsbStart(&U)==EFI_SUCCESS);ResetFail=TRUE;
 UINT32 Before=ReleaseCalls;assert(CrHdkUsbRelease(&U)!=EFI_SUCCESS && ReleaseCalls==Before && U.Domain);
 ResetFail=FALSE;ReleaseFail=TRUE;assert(CrHdkUsbRelease(&U)!=EFI_SUCCESS && U.Domain);
 ReleaseFail=FALSE;assert(CrHdkUsbRelease(&U)==EFI_SUCCESS);Empty();
 Fresh();assert(CrHdkUsbStart(&U)==EFI_SUCCESS);HaltFail=TRUE;
 assert(CrHdkUsbFreeze(&U)==EFI_TIMEOUT && U.Closed && U.Domain && U.Clocks[0]);
 assert(CrHdkUsbStart(&U)==EFI_ACCESS_DENIED);HaltFail=FALSE;
 assert(CrHdkUsbFreeze(&U)==EFI_SUCCESS && U.Domain && U.Ready);assert(CrHdkUsbRelease(&U)==EFI_ACCESS_DENIED);
 /* Type-C/unknown PD and unsupported targets never enter hardware. */
 Fresh();memset(&U,0,sizeof U);UINT32 B=Calls;assert(CrHdkUsbInit(&U,CrHdkUsbGetTarget(),0,&Io)==EFI_UNSUPPORTED&&Calls==B);
 CR_HDK_USB_TARGET Bad=*CrHdkUsbGetTarget();Bad.Soc=0x8450;
 assert(CrHdkUsbInit(&U,&Bad,1,&Io)==EFI_UNSUPPORTED);
 PhyReadback();
 puts("HDK USB2 native journal: exact board windows/overrides, no SS/GSI writes, all-stage faults, retained cleanup and permanent EBS halt controls PASS");
 return 0;
}
