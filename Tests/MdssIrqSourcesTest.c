/* Read-only shared source qualification, independent register fixture. MIT */
#include <Library/MdssIrqSourcesCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks,Steps,Fail,Writes,Barriers;static CR_STATUS Failure;
static UINT32 LateParent;
#define CHECK(X) do { ++Checks;if(!(X)){fprintf(stderr,"%d: %s\n",__LINE__,#X);abort();} } while(0)
typedef struct { CR_MDSS_IRQ_WINDOW W;UINT32 O,V,EnableBits; } REG;
/* Literal masks are independently derived from the pinned DSI XML, DP
 * catalogue/HDCP and SM8450 DPU catalog; no production table is included. */
static REG R[]={
 {CrMdssIrqDpu,0x10,0,0xffffffff}, {CrMdssIrqDpu,0x14,0,0},
 {CrMdssIrqDpu,0x08,0,0xffffffff}, {CrMdssIrqDpu,0x1c,0,0xffffffff},
 {CrMdssIrqDpu,0x341c0,0,0xffffffff}, {CrMdssIrqDpu,0x351c0,0,0xffffffff},
 {CrMdssIrqDpu,0x361c0,0,0xffffffff}, {CrMdssIrqDpu,0x371c0,0,0xffffffff},
 {CrMdssIrqDpu,0x35800,0,0xffffffff}, {CrMdssIrqDpu,0x36800,0,0xffffffff},
 {CrMdssIrqDsi0,0x110,0,0xa222aa02}, {CrMdssIrqDsi1,0x110,0,0xa222aa02},
 {CrMdssIrqDpAhb,0x20,0,0x24924924}, {CrMdssIrqDpAhb,0x24,0,0x09120924},
 {CrMdssIrqDpAhb,0x28,0,0x4}, {CrMdssIrqDpAhb,0x30,0,0x1f},
 {CrMdssIrqDpAhb,0x34,0,0x24}, {CrMdssIrqDpAux,0x0c,0,0xf},
 {CrMdssIrqDpu,0x69350,0,0x21}, {CrMdssIrqDpu,0x6a350,0,0x21},
 {CrMdssIrqParent,0x10,0,0}
};
#define PARENT 20U
static CR_STATUS Read(VOID *C,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 *V){
 CHECK(C==R);++Steps;
 for(unsigned I=0;I<sizeof(R)/sizeof(R[0]);++I)if(R[I].W==W&&R[I].O==O){
  *V=R[I].V;return Steps==Fail?Failure:CR_SUCCESS;}
 CHECK(0);return CR_UNSUPPORTED;
}
static CR_STATUS Write(VOID *C,CR_MDSS_IRQ_WINDOW W,UINT32 O,UINT32 V){
 (VOID)C;(VOID)W;(VOID)O;(VOID)V;++Writes;CHECK(0);return CR_DEVICE_ERROR;
}
static CR_STATUS Barrier(VOID *C){CHECK(C==R);++Steps;
 if(++Barriers==2)R[PARENT].V|=LateParent;return Steps==Fail?Failure:CR_SUCCESS;
}
static CR_MDSS_IRQ_IO Io={R,Read,Write,Barrier};
static VOID Reset(VOID){for(unsigned I=0;I<sizeof(R)/sizeof(R[0]);++I)R[I].V=0;
 Steps=Fail=Writes=Barriers=LateParent=0;Failure=CR_DEVICE_ERROR;}
static VOID QuietAndSources(VOID){
 CR_MDSS_IRQ_SOURCE_OBSERVATION O;
 Reset();CHECK(CrMdssIrqSourcesQuiet(&Io,0,&O)==CR_SUCCESS);
 CHECK(O.Window==CrMdssIrqWindowCount&&!O.ParentStatus&&!O.Top0Enabled&&!O.Top0Pending&&!Writes&&Steps==23);
 /* Every bit in every readable bank: source enable bits refuse connection;
    status/ACK/reserved bits do not accidentally become source enables. */
 for(unsigned I=0;I<PARENT;++I)for(unsigned B=0;B<32;++B){
  Reset();R[I].V=1U<<B;CR_STATUS S=CrMdssIrqSourcesQuiet(&Io,0,&O);
  if(R[I].V&R[I].EnableBits){CHECK(S==CR_NOT_READY);CHECK(O.Window==R[I].W&&O.Offset==R[I].O&&O.Value==R[I].V&&
     (O.ForbiddenMask&R[I].V));}
  else CHECK(S==CR_SUCCESS);
  CHECK(!Writes);
 }
 for(unsigned B=0;B<32;++B){Reset();R[PARENT].V=1U<<B;
  CHECK(CrMdssIrqSourcesQuiet(&Io,0,&O)==CR_NOT_READY);
  CHECK(O.Window==CrMdssIrqParent&&O.ParentStatus==(1U<<B)&&O.Value==O.ParentStatus&&!Writes);}
 /* Disabled DSI/DP pending bits remain untouched, with zero parent. */
 Reset();R[1].V=0xffffffff;R[10].V=R[11].V=0x01110101;
 R[12].V=0x09249249;R[13].V=0x02448249;R[14].V=1;R[16].V=9;
 CHECK(CrMdssIrqSourcesQuiet(&Io,0,&O)==CR_SUCCESS&&!Writes);
 Reset();LateParent=1;CHECK(CrMdssIrqSourcesQuiet(&Io,0,&O)==CR_NOT_READY&&O.ParentStatus==1);
}
static VOID AllowedSources(VOID){
 CR_MDSS_IRQ_SOURCE_OBSERVATION O;
 CONST UINT32 Choices[]={0,0x10,0x08000000,0x08000010};
 for(unsigned A=0;A<4;++A)for(unsigned E=0;E<4;++E)for(unsigned P=0;P<4;++P)for(unsigned Top=0;Top<2;++Top){
  Reset();R[0].V=Choices[E];R[1].V=Choices[P];R[PARENT].V=Top;
  BOOLEAN Valid=!(Choices[E]&~Choices[A])&&(!Top||(Choices[E]&Choices[P]&Choices[A]));
  CHECK(CrMdssIrqSourcesQuiet(&Io,Choices[A],&O)==(Valid?CR_SUCCESS:CR_NOT_READY));CHECK(!Writes);
 }
 for(unsigned B=1;B<32;++B){Reset();R[0].V=R[1].V=0x08000010;R[PARENT].V=1U|(1U<<B);
  CHECK(CrMdssIrqSourcesQuiet(&Io,0x08000010,&O)==CR_NOT_READY&&O.Window==CrMdssIrqParent);}
 Reset();R[0].V=R[1].V=0x10;R[PARENT].V=1;R[11].V=0x2000000;
 CHECK(CrMdssIrqSourcesQuiet(&Io,0x10,&O)==CR_NOT_READY&&O.Window==CrMdssIrqDsi1);
}
static VOID Failures(VOID){
 CONST CR_STATUS Errors[]={CR_TIMEOUT,CR_DEVICE_ERROR,(CR_STATUS)1,(CR_STATUS)0x103};
 CR_MDSS_IRQ_SOURCE_OBSERVATION O,Old;memset(&Old,0xa5,sizeof(Old));
 for(unsigned E=0;E<4;++E)for(unsigned N=1;N<=23;++N){
  Reset();Failure=Errors[E];Fail=N;O=Old;
  CR_STATUS Want=CR_ERROR(Failure)?Failure:CR_NOT_READY;
  CHECK(CrMdssIrqSourcesQuiet(&Io,0,&O)==Want&&Steps==N&&!Writes&&!memcmp(&O,&Old,sizeof(O)));
 }
 Reset();O=Old;CHECK(CrMdssIrqSourcesQuiet(NULL,0,&O)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqSourcesQuiet(&Io,0,NULL)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqSourcesQuiet(&Io,1,&O)==CR_INVALID_PARAMETER);
 CR_MDSS_IRQ_IO Bad=Io;Bad.Read32=NULL;CHECK(CrMdssIrqSourcesQuiet(&Bad,0,&O)==CR_INVALID_PARAMETER);
 Bad=Io;Bad.Barrier=NULL;CHECK(CrMdssIrqSourcesQuiet(&Bad,0,&O)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqSourcesQuiet(&Io,0,(VOID *)&Io)==CR_INVALID_PARAMETER);
 CHECK(CrMdssIrqSourcesQuiet(&Io,0,(VOID *)(~(UINTN)0-3))==CR_INVALID_PARAMETER);
 CHECK(!Steps&&!memcmp(&O,&Old,sizeof(O)));
 Bad=Io;Bad.Write32=NULL;CHECK(CrMdssIrqSourcesQuiet(&Bad,0,&O)==CR_SUCCESS&&!Writes);
}
int main(void){QuietAndSources();AllowedSources();Failures();
 printf("PASS SM8450 IRQ source qualifier: %u checks, all enable bits, owned TOP0/parent combinations, exact callback failures, no writes\n",Checks);return 0;}
