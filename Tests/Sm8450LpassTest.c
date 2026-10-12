/* SPDX-License-Identifier: GPL-2.0-only
 * Actual LPASS sequencer composed with actual PRM; hardware MMIO is modeled. */
#define main PrmOriginalMain
#include "AudioPrmCrTest.c"
#undef main
#include <Library/Sm8450LpassAudioCrLib.h>
#include <lpass-cdc-registers.h>
typedef struct {
 MODEL Firmware;CR_SM8450_LPASS P;
 UINT32 Reg[CR_LPASS_WINDOWS][0x20000/4],Before[CR_LPASS_WINDOWS][0x20000/4];
 UINT32 Reads,Writes,FailAt;
 BOOLEAN Acquired,Drained,AcquirePending,ReleaseError,IgnoreWrite;
 CR_LPASS_LEASE *Lease;
} REG_MODEL;
static CR_STATUS AcquireRegs(VOID *X,CONST VOID *Owner,CR_LPASS_LEASE *L){
 REG_MODEL *M=X;CHECK(Owner==&M->P);CHECK(CrAudioPrmCheck(&M->Firmware.P,31)==CR_SUCCESS);
 if(!M->Acquired){CHECK(!L->Provider&&!L->Token);M->Lease=L;M->Acquired=TRUE;*L=(CR_LPASS_LEASE){M,1};}
 CHECK(M->Lease==L&&L->Provider==M&&L->Token==1);
 return M->AcquirePending?CR_NOT_READY:CR_SUCCESS;
}
static void MatchRegs(REG_MODEL *M,CONST CR_LPASS_LEASE *L,UINT32 W,UINT32 O){
 CHECK(M->Acquired&&L==M->Lease&&L->Provider==M&&L->Token==1);
 CHECK(W<CR_LPASS_WINDOWS&&!(O&3)&&O<CrSm8450LpassWindows[W].Span);
 CHECK(CrAudioPrmCheck(&M->Firmware.P,31)==CR_SUCCESS);
}
static CR_STATUS ReadRegs(VOID *X,CONST CR_LPASS_LEASE *L,UINT32 W,UINT32 O,UINT32 *V){REG_MODEL *M=X;MatchRegs(M,L,W,O);++M->Reads;*V=M->Reg[W][O/4];return CR_SUCCESS;}
static CR_STATUS WriteRegs(VOID *X,CONST CR_LPASS_LEASE *L,UINT32 W,UINT32 O,UINT32 V){
 REG_MODEL *M=X;MatchRegs(M,L,W,O);++M->Writes;
 if(!M->IgnoreWrite)M->Reg[W][O/4]=V;
 return M->Writes==M->FailAt?CR_DEVICE_ERROR:CR_SUCCESS;
}
static CR_STATUS DrainedRegs(VOID *X,CONST CR_LPASS_LEASE *L){REG_MODEL *M=X;CHECK(M->Lease==L);return M->Drained?CR_SUCCESS:CR_BUSY;}
static CR_STATUS ReleaseRegs(VOID *X,CR_LPASS_LEASE *L){REG_MODEL *M=X;CHECK(M->Lease==L&&M->Acquired);if(M->ReleaseError)return CR_BUSY;M->Acquired=FALSE;*L=(CR_LPASS_LEASE){0};return CR_SUCCESS;}
static REG_MODEL *NewRegs(void){
 REG_MODEL *M=calloc(1,sizeof(*M));CHECK(M);Init(&M->Firmware);
 for(unsigned W=0;W<CrLpassRxMux;++W)for(unsigned O=0;O<CrSm8450LpassWindows[W].Span/4;++O)M->Reg[W][O]=0x10000000;
 memcpy(M->Before,M->Reg,sizeof(M->Reg));
 CR_LPASS_IO Io={M,AcquireRegs,ReadRegs,WriteRegs,DrainedRegs,ReleaseRegs,NULL};CHECK(CrSm8450LpassInit(&M->P,&Io,&M->Firmware.P)==CR_SUCCESS);
 return M;
}
static void Votes(REG_MODEL *M){CHECK(CrAudioPrmSetDesired(&M->Firmware.P,31)==CR_SUCCESS);for(unsigned I=0;I<5;++I){CHECK(CrAudioPrmStep(&M->Firmware.P,I*1000)==CR_NOT_READY);Ack(&M->Firmware);}}
static void StopRegs(REG_MODEL *M){M->Drained=TRUE;M->IgnoreWrite=FALSE;CHECK(CrSm8450LpassStop(&M->P)==CR_SUCCESS);CHECK(!M->Acquired&&!M->P.Count);CHECK(!memcmp(M->Reg,M->Before,sizeof(M->Reg)));free(M);}
int main(void){
 CHECK(LPASS_CDC_RX_CLK_RST_CTRL_MCLK_CONTROL-RX_START_OFFSET==0x100);
 CHECK(LPASS_CDC_RX_CLK_RST_CTRL_SWR_CONTROL-RX_START_OFFSET==0x108);
 CHECK(LPASS_CDC_VA_CLK_RST_CTRL_SWR_CONTROL-VA_START_OFFSET==8);
 CHECK(LPASS_CDC_WSA_TOP_FREQ_MCLK-WSA_START_OFFSET==0x88);
 REG_MODEL *M=NewRegs();CHECK(CrSm8450LpassPrepare(&M->P)==CR_NOT_READY&&!M->Acquired&&!M->Reads&&!M->Writes);Votes(M);
 CHECK(CrSm8450LpassPrepare(&M->P)==CR_SUCCESS);UINT32 Writes=M->Writes;
 CHECK((M->Reg[CrLpassVaMacro][0x80/4]&2)==2);CHECK((M->Reg[CrLpassWsaMacro][8/4]&0x1f)==0xd);
 CHECK((M->Reg[CrLpassLpi][14*0x1000/4]&0x3ff)==6);CHECK((M->Reg[CrLpassLpi][10*0x1000/4]&0x3ff)==8);
 for(unsigned I=0;I<3;++I){UINT32 Rate=7;CHECK(CrSm8450LpassClockHz(&M->P,I,&Rate)==CR_SUCCESS&&Rate==9600000);}
 CHECK(CrSm8450LpassStop(&M->P)==CR_BUSY&&M->P.Count);StopRegs(M);
 for(unsigned Fail=1;Fail<=Writes;++Fail){M=NewRegs();Votes(M);M->FailAt=Fail;CHECK(CrSm8450LpassPrepare(&M->P)==CR_DEVICE_ERROR&&M->P.Count);CHECK(CrSm8450LpassPrepare(&M->P)==CR_BUSY);StopRegs(M);}
 M=NewRegs();Votes(M);M->IgnoreWrite=TRUE;CHECK(CrSm8450LpassPrepare(&M->P)==CR_DEVICE_ERROR&&M->P.Count);StopRegs(M);
 for(unsigned I=CrLpassRxMux;I<CR_LPASS_WINDOWS;++I){M=NewRegs();Votes(M);M->Reg[I][0]=1;M->Before[I][0]=1;CHECK(CrSm8450LpassPrepare(&M->P)==CR_UNSUPPORTED&&!M->Writes);StopRegs(M);}
 M=NewRegs();Votes(M);M->Reg[CrLpassRxMacro][0x108/4]|=1;M->Before[CrLpassRxMacro][0x108/4]|=1;CHECK(CrSm8450LpassPrepare(&M->P)==CR_BUSY&&!M->Writes);StopRegs(M);
 M=NewRegs();Votes(M);M->AcquirePending=TRUE;CHECK(CrSm8450LpassPrepare(&M->P)==CR_NOT_READY&&M->Acquired&&!M->Reads);M->AcquirePending=FALSE;CHECK(CrSm8450LpassPrepare(&M->P)==CR_SUCCESS);M->ReleaseError=TRUE;M->Drained=TRUE;CHECK(CrSm8450LpassStop(&M->P)==CR_BUSY&&M->Acquired&&!M->P.Count);M->ReleaseError=FALSE;StopRegs(M);
 M=NewRegs();Votes(M);CHECK(CrSm8450LpassPrepare(&M->P)==CR_SUCCESS);++M->Reg[CrLpassVaMux][0];UINT32 Rate=7;CHECK(CrSm8450LpassClockHz(&M->P,1,&Rate)==CR_DEVICE_ERROR&&Rate==7);--M->Reg[CrLpassVaMux][0];StopRegs(M);
 M=NewRegs();Votes(M);CHECK(CrSm8450LpassPrepare(&M->P)==CR_SUCCESS);CHECK(CrAudioPrmSourceLost(&M->Firmware.P)==CR_SUCCESS);unsigned Reads=M->Reads;M->Drained=TRUE;
 CHECK(CrSm8450LpassStop(&M->P)==CR_NOT_READY&&M->P.Count&&M->Acquired&&M->Reads==Reads);
 /* Source loss has no retirement proof in this fixture. Stable owner/map
  * remains retained. No production source can use this host free as evidence. */
 free(M);
 printf("Actual LPASS + PRM: %u injected effect boundaries, %u checks PASS\n",Writes,Checks);return 0;
}
