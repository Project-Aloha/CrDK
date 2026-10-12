/* Actual fetch-control source and independently modeled DPU registers. MIT */
#include <Library/MdssDpu2dCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);abort();}}while(0)
#define DMA 0x24000U
#define CTL 0x15000U
#define INTF 0x35000U
static UINT32 Reg[0x40000/4],Phase,Fail,Reads,Writes,Delays,Pending,Stuck,DropOp,DropFlush;
static CR_STATUS Failure;
static CR_STATUS Step(void){++Phase;return Phase==Fail?Failure:CR_SUCCESS;}
static CR_STATUS Ready(VOID *C){CHECK(C==Reg);return Step();}
static CR_STATUS Read(VOID *C,UINT32 O,UINT32 *V){CHECK(C==Reg&&O<sizeof(Reg)&&!(O&3));++Reads;CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;*V=Reg[O/4];return S;}
static CR_STATUS Write(VOID *C,UINT32 O,UINT32 V){CHECK(C==Reg&&(O==DMA+0x38||O==CTL+0x18));++Writes;CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;if(O==DMA+0x38){if(!DropOp)Reg[O/4]=V;}else{CHECK(V==(1U<<11));if(!DropFlush){Reg[O/4]=V;Pending=1;}}return S;}
static CR_STATUS Fence(VOID *C){CHECK(C==Reg);return Step();}
static CR_STATUS Delay(VOID *C,UINT32 U){CHECK(C==Reg&&U==50);++Delays;CR_STATUS S=Step();if(S!=CR_SUCCESS)return S;if(Pending&&!Stuck){Reg[(CTL+0x18)/4]=0;Reg[(INTF+0xac)/4]++;Pending=0;}return S;}
static CR_MDSS_MODE Mode(void){return (CR_MDSS_MODE){.Soc=CR_MDSS_SOC_SM8450,.Width=1920,.Height=1080,.Stride=7680,.HSync=44,.HBackPorch=148,.HFrontPorch=88,.VSync=5,.VBackPorch=36,.VFrontPorch=4,.RefreshQ16=60U<<16,.Lanes=4};}
static CR_MDSS_DPU_FETCH Fresh(void){
 memset(Reg,0,sizeof(Reg));Phase=Fail=Reads=Writes=Delays=Pending=Stuck=DropOp=DropFlush=0;Failure=CR_DEVICE_ERROR;
 CR_MDSS_MODE M=Mode();Reg[DMA/4]=Reg[(DMA+0xc)/4]=M.Width|(M.Height<<16);Reg[(DMA+0x14)/4]=0x10800000;
 Reg[(DMA+0x24)/4]=M.Stride;Reg[(DMA+0x30)/4]=0x236ff;Reg[(DMA+0x34)/4]=0x03020001;
 Reg[(DMA+0x38)/4]=(1U<<31)|(1U<<17); /* preserve source-defined IGC control */
 Reg[CTL/4]=(1U<<24)|(1U<<18);Reg[(CTL+0xfc)/4]=1;Reg[(CTL+0xf4)/4]=2;
 Reg[INTF/4]=Reg[(INTF+0x26c)/4]=1;Reg[(INTF+0xac)/4]=0xffffffff;
 CR_MDSS_DPU_FETCH_IO Io={Reg,0xae01000,sizeof(Reg),Ready,Read,Write,Fence,Delay};CR_MDSS_DPU_FETCH D={0};
 CHECK(CrMdssDpuFetchInit(&D,&Io,&M,0x10800000,0x2b00000)==CR_SUCCESS&&Phase==0);return D;
}
static void Stop(CR_MDSS_DPU_FETCH *D){Reg[INTF/4]=Reg[(INTF+0x26c)/4]=0;Fail=0;CHECK(CrMdssDpuFetchRetire(D)==CR_SUCCESS&&!D->Initialized);}
int main(void){
 CR_MDSS_DPU_FETCH D=Fresh();UINT32 T=99;CHECK(CrMdssDpuFetchQuery(&D,&T)==CR_SUCCESS&&T==0&&!Writes);
 CHECK(CrMdssDpuFetchSet(&D,3)==CR_SUCCESS&&D.Confirmed&&!D.CleanupPending&&D.Transform==3&&Writes==2&&Delays==1);
 CHECK(Reg[(DMA+0x38)/4]==((1U<<31)|(1U<<17)|(3U<<13))&&Reg[(INTF+0xac)/4]==0);
 UINT32 N=Writes;CHECK(CrMdssDpuFetchSet(&D,3)==CR_SUCCESS&&Writes==N);
 CHECK(CrMdssDpuFetchSet(&D,1)==CR_SUCCESS&&CrMdssDpuFetchSet(&D,2)==CR_SUCCESS&&CrMdssDpuFetchSet(&D,0)==CR_SUCCESS);
 CHECK(CrMdssDpuFetchRetire(&D)==CR_BUSY&&D.Initialized);Stop(&D);
 D=Fresh();Phase=0;CHECK(CrMdssDpuFetchSet(&D,3)==CR_SUCCESS);UINT32 Max=Phase,Faults=0;
 for(UINT32 F=1;F<=Max;++F)for(UINT32 K=0;K<3;++K){
  D=Fresh();Fail=F;Failure=K==0?CR_DEVICE_ERROR:K==1?(CR_STATUS)1:(CR_STATUS)0x103;
  CHECK(CrMdssDpuFetchSet(&D,3)!=CR_SUCCESS);Fail=0;UINT32 Prior=Writes;
  if(D.Touched){CHECK(D.CleanupPending&&CrMdssDpuFetchSet(&D,0)==CR_NOT_READY&&Writes==Prior);T=55;CHECK(CrMdssDpuFetchQuery(&D,&T)==CR_NOT_READY&&T==55);}
  Stop(&D);++Faults;
 }
 const UINT32 Off[]={DMA,DMA+8,DMA+0xc,DMA+0x10,DMA+0x14,DMA+0x18,DMA+0x1c,DMA+0x20,DMA+0x24,DMA+0x30,DMA+0x34,DMA+0x170,CTL,CTL+0xfc,CTL+0xf4,INTF,INTF+0x26c};
 for(UINT32 I=0;I<sizeof(Off)/sizeof(*Off);++I){D=Fresh();Reg[Off[I]/4]^=1;T=66;CHECK(CrMdssDpuFetchQuery(&D,&T)!=CR_SUCCESS&&T==66&&!Writes);CHECK(CrMdssDpuFetchSet(&D,3)!=CR_SUCCESS&&!Writes);}
 D=Fresh();Reg[(CTL+0x18)/4]=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_BUSY&&!Writes);
 D=Fresh();Reg[(DMA+0x38)/4]|=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_UNSUPPORTED&&!Writes);
 D=Fresh();DropOp=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_DEVICE_ERROR&&Writes==1&&D.CleanupPending);Stop(&D);
 D=Fresh();DropOp=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_DEVICE_ERROR&&D.CleanupPending);
 CR_MDSS_DPU_FETCH Retained=D;CR_MDSS_MODE Again=Mode();UINT32 P=Phase;
 CHECK(CrMdssDpuFetchInit(&D,&Retained.Io,&Again,0x10800000,0x2b00000)==CR_BUSY&&Phase==P&&!memcmp(&D,&Retained,sizeof(D)));Stop(&D);
 D=Fresh();DropFlush=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_TIMEOUT&&D.CleanupPending&&Delays==5000);Stop(&D);
 D=Fresh();Stuck=1;CHECK(CrMdssDpuFetchSet(&D,3)==CR_TIMEOUT&&D.CleanupPending);Stop(&D);
 D=Fresh();CHECK(CrMdssDpuFetchSet(&D,3)==CR_SUCCESS);Reg[(DMA+0x38)/4]&=~(1U<<13);T=9;CHECK(CrMdssDpuFetchQuery(&D,&T)==CR_NOT_READY&&T==9);N=Writes;CHECK(CrMdssDpuFetchSet(&D,0)==CR_NOT_READY&&Writes==N);Stop(&D);
 D=Fresh();CHECK(CrMdssDpuFetchSet(&D,4)==CR_INVALID_PARAMETER&&!Writes);
 CR_MDSS_DPU_FETCH Bad;memset(&Bad,0x55,sizeof(Bad));CR_MDSS_DPU_FETCH Saved=Bad;CR_MDSS_DPU_FETCH_IO Io=D.Io;CR_MDSS_MODE M=Mode();Io.Bytes=INTF+0x26f;
 CHECK(CrMdssDpuFetchInit(&Bad,&Io,&M,0x10800000,0x2b00000)==CR_BAD_BUFFER_SIZE&&!memcmp(&Bad,&Saved,sizeof(Bad)));
 Io=D.Io;M.Controller=1;CHECK(CrMdssDpuFetchInit(&Bad,&Io,&M,0x10800000,0x2b00000)==CR_UNSUPPORTED);
 /* Independent odd-raster pixel oracle and padding conservation. */
 UINT8 Pixels[36],Want[36];for(UINT32 I=0;I<36;++I)Pixels[I]=(UINT8)I;
 memcpy(Want,Pixels,36);M=Mode();M.Width=3;M.Height=3;M.Stride=12;
 for(UINT32 I=0;I<9;++I)for(UINT32 K=0;K<4;++K)Want[I*4+K]=(UINT8)((8-I)*4+K);
 CHECK(CrMdssDpuReversePixels(Pixels,&M,36)==CR_SUCCESS&&!memcmp(Pixels,Want,36));
 CHECK(CrMdssDpuReversePixels(Pixels,&M,36)==CR_SUCCESS);for(UINT32 I=0;I<36;++I)CHECK(Pixels[I]==I);
 UINT8 Padded[60],Oracle[60];for(UINT32 I=0;I<60;++I)Padded[I]=Oracle[I]=(UINT8)(I+2);M.Stride=20;
 for(UINT32 Y=0;Y<3;++Y)for(UINT32 X=0;X<3;++X)for(UINT32 K=0;K<4;++K)Oracle[Y*20+X*4+K]=Padded[(2-Y)*20+(2-X)*4+K];
 CHECK(CrMdssDpuReversePixels(Padded,&M,60)==CR_SUCCESS&&!memcmp(Padded,Oracle,60));memcpy(Oracle,Padded,60);
 CHECK(CrMdssDpuReversePixels(Padded,&M,59)==CR_BAD_BUFFER_SIZE&&!memcmp(Padded,Oracle,60));
 printf("Actual DMA0 reflections, preserved controls, frame/flush completion, %u callback failures and stopped retirement: PASS\n",Faults);return 0;
}
