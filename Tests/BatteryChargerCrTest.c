/** Actual portable core, independent Android SM8450 legacy wire frames. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/BatteryChargerCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
#endif
static CR_BATTERY_CHARGER B;
static UINT8 Sent[24]; static UINT32 Sends,PresentSends;
static CR_STATUS SendStatus;
static BOOLEAN Immediate, Reenter;
/* Captured ABI layout: owner0x800a,type1,get0x30/property2/value/retcode0. */
static UINT8 Reply[24]={0x0a,0x80,0,0, 1,0,0,0, 0x30,0,0,0, 2,0,0,0, 1,0,0,0, 0,0,0,0};
static VOID Put(UINT8 *P,UINT32 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24);}
static UINT32 U32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static CR_STATUS Send(VOID *C,CONST VOID *Data,UINTN N){
 assert(C==&B && N==24);memcpy(Sent,Data,N);++Sends;
 if(U32(Sent+8)==0x30&&U32(Sent+16)==2)++PresentSends;
 assert(U32(Sent)==32778 && U32(Sent+4)==1 && U32(Sent+12)==0 && U32(Sent+20)==0);
 if(Reenter)assert(CrBatteryChargerTick(&B,10)==CR_BUSY);
 if(Immediate){Put(Reply+8,U32(Sent+8));Put(Reply+12,U32(Sent+16));Put(Reply+16,1);CrBatteryChargerReceive(&B,Reply,24,10);}
 return SendStatus;
}
static VOID Reset(VOID){
 Sends=PresentSends=0;SendStatus=CR_SUCCESS;Immediate=Reenter=FALSE;
 assert(CrBatteryChargerInit(&B,Send,&B)==CR_SUCCESS);
 CrBatteryChargerOnline(&B,TRUE,7,9);Put(Reply,32778);Put(Reply+4,1);Put(Reply+20,0);
}
static VOID Answer(UINT32 Value,UINT64 Time){
 Put(Reply+8,U32(Sent+8));Put(Reply+12,U32(Sent+16));Put(Reply+16,Value);
 CrBatteryChargerReceive(&B,Reply,sizeof(Reply),Time);
}
static VOID Property(UINT32 I,UINT32 Raw){
 Reset();B.RefreshMask=1U<<I;B.NextIndex=I;
 assert(CrBatteryChargerTick(&B,100)==CR_SUCCESS);Answer(Raw,101);
}
static VOID Metadata(VOID){
 Reset();UINT32 Technology=0xdeadbeef;assert(CrBatteryChargerTechnology(&B,100,&Technology)==CR_NOT_READY&&Technology==0xdeadbeef);
 CrBatteryChargerEnableMetadata(&B);B.RefreshMask=128;B.NextIndex=7;
 assert(CrBatteryChargerTick(&B,100)==CR_SUCCESS&&U32(Sent+16)==13&&U32(Sent+8)==0x30);
 Answer(2,101);assert(CrBatteryChargerTechnology(&B,102,&Technology)==CR_SUCCESS&&Technology==2&&B.State.ValidMask==0);
 CR_BATTERY_CHARGER_SNAPSHOT S;assert(CrBatteryChargerSnapshot(&B,102,&S)==CR_NOT_READY&&S.ValidMask==0&&S.Revision==1&&S.Size==sizeof(S));
 assert(CrBatteryChargerTechnology(&B,5000102,&Technology)==CR_NOT_READY);
 for(UINT32 T=0;T<=7;++T){B.RefreshMask=128;B.NextIndex=7;assert(CrBatteryChargerTick(&B,200+T*100)==CR_SUCCESS);Answer(T,201+T*100);assert(CrBatteryChargerTechnology(&B,202+T*100,&Technology)==(T<=6?CR_SUCCESS:CR_NOT_READY));if(T<=6)assert(Technology==T);}
 B.RefreshMask=128;B.NextIndex=7;CrBatteryChargerTick(&B,1000);CrBatteryChargerTick(&B,1001000);assert(B.AwaitLateMask==128&&!B.TechnologyValid);
 Put(Reply+8,0x30);Put(Reply+12,13);Put(Reply+16,2);CrBatteryChargerReceive(&B,Reply,24,1001001);assert(!B.AwaitLateMask&&!B.TechnologyValid);
 CrBatteryChargerOnline(&B,FALSE,7,9);assert(CrBatteryChargerTechnology(&B,1001002,&Technology)==CR_NOT_READY);
 Reset();assert(!B.MetadataEnabled); /* Native default seven-property ABI. */
}
static VOID Statistics(VOID){
 CR_BATTERY_STATISTICS S={0};
 Reset();assert(CrBatteryChargerStatistics(&B,0,&S)==CR_NOT_READY&&!S.ValidMask);
 CrBatteryChargerEnableMetadata(&B);assert(!B.StatisticsEnabled);
 CrBatteryChargerEnableStatistics(&B);assert(B.MetadataEnabled&&B.StatisticsEnabled&&B.RefreshMask==1023);
 /* Actual 24-byte Android property identifiers, disjoint from technology. */
 for(UINT32 I=8;I<10;++I){
  B.RefreshMask=1U<<I;B.NextIndex=I;
  assert(CrBatteryChargerTick(&B,100+I)==CR_SUCCESS&&U32(Sent+8)==0x30&&U32(Sent+16)==(I==8?15U:20U));
  Answer(I==8?342:7200,120+I);
 }
 assert(CrBatteryChargerStatistics(&B,200,&S)==CR_SUCCESS&&S.ValidMask==3&&S.CycleCount==342&&S.TimeToEmptySeconds==7200);
 assert(!B.State.ValidMask&&!B.TechnologyValid); /* No alias into mandatory fields. */
 assert(CrBatteryChargerStatistics(&B,5000129,&S)==CR_SUCCESS&&S.ValidMask==2);
 assert(CrBatteryChargerStatistics(&B,5000130,&S)==CR_NOT_READY&&!S.ValidMask);
 assert(CrBatteryChargerStatistics(&B,0,&S)==CR_NOT_READY&&!S.ValidMask);
 for(UINT32 I=8;I<10;++I){
  CONST UINT32 Values[]={0,0x7fffffffU,0x80000000U,0xffffffffU};
  for(UINT32 V=0;V<4;++V){
   Reset();CrBatteryChargerEnableStatistics(&B);B.RefreshMask=1U<<I;B.NextIndex=I;
   CrBatteryChargerTick(&B,100);Answer(Values[V],101);
   assert(CrBatteryChargerStatistics(&B,102,&S)==(V<2?CR_SUCCESS:CR_NOT_READY));
   assert(S.ValidMask==(V<2?(1U<<(I-8)):0));
  }
  /* Unsupported reply never fences the mandatory GET queue. */
  Reset();CrBatteryChargerEnableStatistics(&B);B.RefreshMask=1U<<I;B.NextIndex=I;
  CrBatteryChargerTick(&B,100);Put(Reply+20,1);Answer(1,101);Put(Reply+20,0);
  assert(!B.State.Pending&&!B.AwaitLateMask&&!B.Statistics.ValidMask);
  B.RefreshMask=1;B.NextIndex=0;CrBatteryChargerTick(&B,102);assert(U32(Sent+16)==2);Answer(1,103);
  assert(B.State.ValidMask==CR_BATTERY_VALID_PRESENT);
  /* Timeout and each same-ADSP transition retain optional identities too. */
  for(UINT32 Transition=0;Transition<5;++Transition){
   Reset();CrBatteryChargerEnableStatistics(&B);B.RefreshMask=1U<<I;B.NextIndex=I;
   CrBatteryChargerTick(&B,100);
   if(Transition==0)CrBatteryChargerTick(&B,1000100);
   else if(Transition==1)CrBatteryChargerOnline(&B,FALSE,7,9);
   else CrBatteryChargerOnline(&B,TRUE,Transition==2?7:Transition==3?0:6,10);
   assert((B.AwaitLateMask&(1U<<I))&&!B.Statistics.ValidMask);
   CrBatteryChargerOnline(&B,TRUE,7,11);B.RefreshMask=1U<<I;B.NextIndex=I;
   UINT32 Prior=Sends;CrBatteryChargerTick(&B,1000101);assert(Sends==Prior);
   Put(Reply+8,0x30);Put(Reply+12,I==8?15:20);Put(Reply+16,111);
   CrBatteryChargerReceive(&B,Reply,24,1000102);assert(!(B.AwaitLateMask&(1U<<I))&&!B.Statistics.ValidMask);
   CrBatteryChargerTick(&B,1000103);assert(Sends==Prior+1);Answer(111,1000104);
   assert(CrBatteryChargerStatistics(&B,1000105,&S)==CR_SUCCESS&&S.ValidMask==(1U<<(I-8)));
   CrBatteryChargerOnline(&B,TRUE,8,12);assert(!B.Statistics.ValidMask&&!B.AwaitLateMask);
  }
 }
 puts("Optional battery statistics: exact cycle-count15/TTE20 GETs, signed bounds, independent age, server errors, timeout/PDR/offline/epoch fences PASS");
}
static VOID EpochRecovery(VOID){
 /* Every real property, including optional technology, has the same
    untagged identity across PDR/offline. Independent original wire replies
    retire fences; no stale value can become a sample for a new request. */
 for(UINT32 I=0;I<8;++I)for(UINT32 Transition=0;Transition<4;++Transition){
  Reset();CrBatteryChargerEnableMetadata(&B);B.RefreshMask=1U<<I;B.NextIndex=I;
  CrBatteryChargerTick(&B,100);assert(B.State.Pending&&B.PendingIndex==I);
  UINT32 Op=U32(Sent+8),Prop=U32(Sent+16);
  if(Transition==0)CrBatteryChargerOnline(&B,TRUE,7,10);
  else CrBatteryChargerOnline(&B,Transition==3,Transition==1?7:Transition==2?0:6,10);
  assert(B.AdspGeneration==7&&!B.State.Pending&&B.AwaitLateMask==(1U<<I)&&!B.State.ValidMask&&!B.TechnologyValid);
  if(Transition){assert(!B.State.PeerReady&&CrBatteryChargerTick(&B,101)==CR_NOT_READY&&Sends==1);}
  CrBatteryChargerOnline(&B,TRUE,7,11);B.RefreshMask=1U<<I;B.NextIndex=I;
  assert(CrBatteryChargerTick(&B,102)==CR_SUCCESS&&Sends==1&&!B.State.Pending);
  Put(Reply+8,Op);Put(Reply+12,Prop);Put(Reply+16,1);
  CrBatteryChargerReceive(&B,Reply,24,103);
  assert(!B.AwaitLateMask&&!B.State.ValidMask&&!B.TechnologyValid);
  assert(CrBatteryChargerTick(&B,104)==CR_SUCCESS&&Sends==2&&B.PendingIndex==I);
  Answer(1,105);assert(I==7?B.TechnologyValid:(B.State.ValidMask&(1U<<I))!=0);
 }
 Reset();CrBatteryChargerTick(&B,0);CrBatteryChargerTick(&B,1000000);
 assert(B.AwaitLateMask==1&&B.PendingIndex==1);
 CrBatteryChargerOnline(&B,TRUE,7,10);assert(B.AwaitLateMask==3&&!B.State.Pending);
 CrBatteryChargerOnline(&B,FALSE,0,0);assert(B.AdspGeneration==7&&B.AwaitLateMask==3);
 CrBatteryChargerOnline(&B,TRUE,6,11);assert(!B.State.PeerReady&&B.AwaitLateMask==3);
 CrBatteryChargerOnline(&B,TRUE,8,12);assert(B.State.PeerReady&&!B.AwaitLateMask);
 CrBatteryChargerTick(&B,1000001);assert(PresentSends==2&&B.PendingIndex==0);
}
int main(void){
 Metadata();
 Statistics();
 EpochRecovery();
 CR_BATTERY_CHARGER_SNAPSHOT S;
 assert(CrBatteryChargerInit(NULL,Send,NULL)==CR_INVALID_PARAMETER);
 assert(CrBatteryChargerInit(&B,NULL,NULL)==CR_INVALID_PARAMETER);
 Reset();CrBatteryChargerOnline(&B,FALSE,7,9);
 assert(CrBatteryChargerTick(&B,0)==CR_NOT_READY&&Sends==0);
 assert(CrBatteryChargerSnapshot(&B,0,&S)==CR_NOT_READY&&S.ValidMask==0);
 CrBatteryChargerOnline(&B,TRUE,8,10);
 CONST UINT32 Props[7]={2,0,4,7,9,12,0},Values[7]={1,1,8765,4212345,0xfffe1dc0,2534,1};
 for(UINT32 I=0;I<7;++I){
  assert(CrBatteryChargerTick(&B,100+I*100)==CR_SUCCESS&&B.State.Pending);
  assert(U32(Sent+8)==(I==6?0x32U:0x30U)&&U32(Sent+16)==Props[I]);
  assert(CrBatteryChargerTick(&B,101+I*100)==CR_SUCCESS&&Sends==I+1);
  Answer(Values[I],102+I*100);assert(!B.State.Pending);
 }
 assert(CrBatteryChargerSnapshot(&B,1000,&S)==CR_SUCCESS&&S.ValidMask==127);
 assert(S.SocHundredthsPercent==8765&&S.VoltageUv==4212345&&S.CurrentUa==-123456&&S.TemperatureMilliC==25340&&S.UsbOnline&&S.Present&&S.Status==CrBatteryCharging);
 assert(CrBatteryChargerTick(&B,999999)==CR_SUCCESS&&Sends==7);
 assert(CrBatteryChargerSnapshot(&B,6001000,&S)==CR_NOT_READY&&S.ValidMask==0);
 /* Every SOC value is interpreted as hundredths, including actual0/100%. */
 for(UINT32 V=0;V<=10000;++V){Property(2,V);assert(B.State.ValidMask==4&&B.State.SocHundredthsPercent==V);}
 Property(2,10001);assert(!B.State.ValidMask&&B.State.LastError==CR_COMPROMISED_DATA);
 Property(0,2);assert(!B.State.ValidMask);Property(1,5);assert(!B.State.ValidMask);Property(6,2);assert(!B.State.ValidMask);
 Property(5,(UINT32)-2534);assert(B.State.TemperatureMilliC==-25340);
 Property(5,214748364);assert(B.State.TemperatureMilliC==2147483640);
 Property(5,(UINT32)-214748364);assert(B.State.TemperatureMilliC==-2147483640);
 Property(5,214748365);assert(!B.State.ValidMask);Property(5,(UINT32)-214748365);assert(!B.State.ValidMask);
 Property(4,0x80000000);assert(B.State.CurrentUa==(-2147483647-1));
 /* A timeout fences only that property; no same-property request is reused
    until its exact original late reply is observed. Other fields progress. */
 Reset();assert(CrBatteryChargerTick(&B,0)==CR_SUCCESS);
 assert(CrBatteryChargerTick(&B,1000000)==CR_SUCCESS&&B.AwaitLateMask==1&&B.State.Timeouts==1&&U32(Sent+16)==0);
 UINT32 Active=B.PendingIndex;Put(Reply+8,0x30);Put(Reply+12,2);Put(Reply+16,1);
 CrBatteryChargerReceive(&B,Reply,24,1000001);
 assert(B.AwaitLateMask==0&&B.State.Pending&&B.PendingIndex==Active&&!(B.State.ValidMask&1));
 Answer(1,1000002);assert(!B.State.Pending);
 B.RefreshMask=1;B.NextIndex=0;assert(CrBatteryChargerTick(&B,1000003)==CR_SUCCESS&&U32(Sent+16)==2);
 Answer(1,1000004);assert(B.State.ValidMask&1);
 Reset();CrBatteryChargerTick(&B,0);CrBatteryChargerTick(&B,1000000);
 B.State.Pending=FALSE;B.RefreshMask=1;B.NextIndex=0;
 for(UINT32 I=0;I<10;++I)CrBatteryChargerTick(&B,2000000+I*1000000ULL);
 assert(PresentSends==1&&(B.AwaitLateMask&1)); /* ten cycles never resend timed-out property */
 CrBatteryChargerOnline(&B,FALSE,7,9);assert(B.State.ValidMask==0&&!B.State.Pending&&(B.AwaitLateMask&1));
 CrBatteryChargerReceive(&B,Reply,24,100000000);assert(B.State.ValidMask==0);
 CrBatteryChargerOnline(&B,TRUE,8,10);CrBatteryChargerTick(&B,100000001);assert(PresentSends==2);
 /* Late response observed before Tick also retires exact original request,
    without publishing an expired value; fresh sampling can resume. */
 Reset();CrBatteryChargerTick(&B,0);Answer(1,1000000);assert(!B.State.ValidMask&&!B.State.Pending&&!B.AwaitLateMask);
 Reset();CrBatteryChargerTick(&B,0);Put(Reply+8,0x30);Put(Reply+12,99);
 CrBatteryChargerReceive(&B,Reply,24,1);assert(B.State.Pending&&B.State.Malformed==1);
 Put(Reply+12,2);CrBatteryChargerReceive(&B,Reply,23,2);assert(B.State.Pending&&B.State.Malformed==2);
 Put(Reply,32779);CrBatteryChargerReceive(&B,Reply,24,3);assert(B.State.Pending&&B.State.Malformed==2);Put(Reply,32778);
 Put(Reply+20,1);Answer(1,4);assert(!B.State.Pending&&!B.State.ValidMask&&B.State.LastError==CR_DEVICE_ERROR);Put(Reply+20,0);
 Reset();SendStatus=CR_NOT_READY;assert(CrBatteryChargerTick(&B,0)==CR_NOT_READY&&!B.State.Pending&&B.RefreshMask==127);
 SendStatus=CR_SUCCESS;assert(CrBatteryChargerTick(&B,1)==CR_SUCCESS&&Sends==2);
 Reset();SendStatus=CR_DEVICE_ERROR;assert(CrBatteryChargerTick(&B,0)==CR_DEVICE_ERROR&&!B.State.Pending&&B.State.Faulted);
 Reset();Immediate=Reenter=TRUE;assert(CrBatteryChargerTick(&B,0)==CR_SUCCESS&&!B.State.Pending&&B.State.ValidMask==1);
 UINT32 G=B.State.Generation;CrBatteryChargerOnline(&B,TRUE,7,9);assert(B.State.Generation==G&&B.State.ValidMask==1);
 CrBatteryChargerOnline(&B,TRUE,7,10);assert(B.State.Generation==G+1&&B.State.ValidMask==0);
 assert(CrBatteryChargerSnapshot(&B,0,NULL)==CR_INVALID_PARAMETER);
 puts("Battery actual native/NT core: exact7 wire properties, all10001 SOC values, signed units, malformed/late/fenced/servererror/offline/generation/reentry/stale sample PASS");
 return 0;
}
