/* Whole cold command owner compared with unchanged Android StopTe helper bodies.
 * SPDX-License-Identifier: GPL-2.0-only */
#define main FullCommandSuite
#include "MdssCmdLifecycleTest.c"
#undef main
#include <AndroidStopTeOracle.h>
typedef struct {UINT32 Offset,Value;} ACTION;
static ACTION Actual[64],Expected[64];
static UINT32 ActualCount,ExpectedCount;
static BOOLEAN Selected(UINT32 O){return O==0x418||O==0x348||O==0x6aa90;}
static CR_STATUS CaptureWrite(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V){
  CR_STATUS S=Write(C,W,O,V);assert(S==CR_SUCCESS);
  if(W==CrMdssCmdDpu&&Selected(O)){
    assert(ActualCount<64);Actual[ActualCount++]=(ACTION){O,V};
  }
  return S;
}
typedef struct {UINT32 Status,WrCount,Start,ReadCount,Barriers;} REF;
static uint32_t RefRead(void *C,uint32_t O){
  REF *R=C;
  if(O==0x34c)return R->Status;
  if(O==0x6aa98)return R->ReadCount;
  if(O==0x6aaa0)return R->Start;
  if(O==0x6aa90)return R->WrCount;
  assert(0);return 0;
}
static void RefWrite(void *C,uint32_t O,uint32_t V){
  REF *R=C;assert(Selected(O)&&ExpectedCount<64);
  Expected[ExpectedCount++]=(ACTION){O,V};
  if(O==0x6aa90)R->WrCount=V;
}
static void RefBarrier(void *C){++((REF *)C)->Barriers;}
static void Case(BOOLEAN Pending){
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();
  ActualCount=ExpectedCount=0;D.Io.Write32=CaptureWrite;
  if(Pending)Queued();
  else {H.Dpu[0x418/4]=0x12;H.Dpu[0x348/4]=0x23;}
  H.Dpu[0x6aa90/4]=0xaabb0001;H.Dpu[0x6aa98/4]=0x12345678;
  assert(CrMdssCmdStop(&D)==CR_SUCCESS);
  REF R={.WrCount=0xaabb0001,.Start=2340,.ReadCount=0x12345678};
  ANDROID_STOP_TE_IO Io={&R,RefRead,RefWrite,RefBarrier};
  UINT32 S,ReadCount,WriteCount;
  R.Status=Pending?0x80:0;assert(!AndroidStopTeAutoStatus(&Io,2,&S)&&S==R.Status);
  if(Pending)for(UINT32 I=0;I<4;++I){
    R.Status=I==3?0:0x80;
    assert(!AndroidStopTeAutoStatus(&Io,2,&S)&&S==R.Status);
    assert(!AndroidStopTeResetTear(&Io,0x6a800,&ReadCount,&WriteCount));
    assert(ReadCount==0x12345678&&WriteCount==(0xaabb0000U|2340U));
  }
  assert(R.Barriers==(Pending?5U:1U));
  Expected[ExpectedCount++]=(ACTION){0x418,0x12};
  Expected[ExpectedCount++]=(ACTION){0x348,0x23};
  assert(ActualCount==ExpectedCount&&!memcmp(Actual,Expected,ActualCount*sizeof(ACTION)));
  assert(!D.DpuDebugTouched&&H.Dpu[0x418/4]==0x12&&H.Dpu[0x348/4]==0x23);
  Finished();
  printf("whole command queued%u: original Android %u-write autorefresh/tear trace plus retained restore PASS\n",Pending,ActualCount);
}
int main(void){Case(FALSE);Case(TRUE);return 0;}
