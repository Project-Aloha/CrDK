/* Actual portable DSC program/stop plus independent Android rate-control oracle.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/MdssDscCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
int AndroidBuild(const CR_MDSS_DSC_PARAMS *, CR_MDSS_DSC_CONFIG *);
void AndroidPps(const CR_MDSS_DSC_CONFIG *, UINT8 *, UINT8);
typedef struct {
  UINT32 *Regs, Calls, Fail, Dropped, Writes, LostAfter;
  CR_STATUS Failure;
  BOOLEAN Lost;
} HW;
static CR_STATUS Step(HW *H) {
  ++H->Calls;
  if (H->Fail && H->Calls==H->Fail) return H->Failure;
  return CR_SUCCESS;
}
static CR_STATUS Ready(VOID *C) { HW *H=C; if(H->Lost)return CR_NOT_READY;
  if(H->LostAfter && H->Calls>=H->LostAfter){H->Lost=TRUE;return CR_NOT_READY;}return Step(H); }
static CR_STATUS Read(VOID *C,UINT32 O,UINT32 *V) {HW *H=C;assert(!(O&3)&&O<0x82000&&!H->Lost);
  CR_STATUS S=Step(H);if(S==CR_SUCCESS)*V=H->Regs[O/4];return S;}
static CR_STATUS Write(VOID *C,UINT32 O,UINT32 V) {HW *H=C;assert(!(O&3)&&O<0x82000&&!H->Lost);
  CR_STATUS S=Step(H);if(S==CR_SUCCESS){++H->Writes;if(H->Dropped!=O)H->Regs[O/4]=V;}return S;}
static CR_STATUS Fence(VOID *C) {return Step(C);}
static CR_MDSS_DSC_PARAMS Params(UINT32 Minor,UINT32 Scr,UINT32 Bpc,UINT32 Bpp,UINT32 W,UINT32 H) {
  CR_MDSS_DSC_PARAMS P={0};P.Revision=1;P.MinorVersion=Minor;P.ScrVersion=Scr;
  P.BitsPerComponent=Bpc;P.BppQ4=Bpp*16;P.PictureWidth=W*2;P.PictureHeight=H*8;
  P.SliceWidth=W;P.SliceHeight=H;P.BlockPrediction=TRUE;return P;
}
static void Pure(void) {
  CR_MDSS_DSC_CONFIG C,A,Old;UINT8 P[132],Q[128];UINT32 Minor,Scr,Bpc,Bpp,W,H,Count=0;
  for(Minor=1;Minor<=2;++Minor)for(Scr=0;Scr<2;++Scr)for(Bpc=8;Bpc<=10;Bpc+=2)
  for(Bpp=8;Bpp<=10;Bpp+=2)for(W=120;W<=1440;W+=60)for(H=8;H<=64;H+=8){
    CR_MDSS_DSC_PARAMS I=Params(Minor,Scr,Bpc,Bpp,W,H);
    if(Bpc==8&&Bpp==10)continue;
    assert(CrMdssDscBuild(&I,&C)==CR_SUCCESS);assert(AndroidBuild(&I,&A)==0);
    assert(memcmp(&C,&A,sizeof(C))==0);assert(CrMdssDscValidate(&C)==CR_SUCCESS);
    memset(P,0xcc,sizeof(P));memset(Q,0xdd,sizeof(Q));
    assert(CrMdssDscPackPps(&C,37,P,128)==CR_SUCCESS);AndroidPps(&A,Q,37);
    /* Android RGB1.2 PPS88 reuses the last range byte. Linux requires zero
       for both native chroma flags; exclude that demonstrated oracle defect. */
    Q[88]=0;assert(memcmp(P,Q,128)==0);assert(P[128]==0xcc);++Count;
  }
  CR_MDSS_DSC_PARAMS I=Params(2,0,8,8,540,8);assert(CrMdssDscBuild(&I,&C)==CR_SUCCESS);
  assert(C.InitialTransmitDelay==512&&C.InitialDecodeDelay==526&&C.ChunkBytes==540);
  assert(C.NflBpg==3511&&C.FinalOffset==4336&&C.ScaleDecrement==7);
  assert(CrMdssDscPackPps(&C,0,P,127)==CR_BAD_BUFFER_SIZE);
  assert(CrMdssDscPackPps(&C,0,(UINT8 *)&C,128)==CR_INVALID_PARAMETER);
  Old=C;C.Ranges[5].MinQp^=1;assert(CrMdssDscValidate(&C)==CR_INVALID_PARAMETER);C=Old;
  memset(&A,0xa5,sizeof(A));Old=A;I.SliceHeight=1;
  assert(CrMdssDscBuild(&I,&A)==CR_INVALID_PARAMETER&&!memcmp(&A,&Old,sizeof(A)));
  I=Params(2,0,8,8,540,8);I.BppQ4=127;assert(CrMdssDscBuild(&I,&A)==CR_UNSUPPORTED);
  I.PictureWidth=1081;assert(CrMdssDscBuild(&I,&A)==CR_INVALID_PARAMETER);
  printf("DSC Android rate-control/PPS reference: %u vectors\n",Count);
}
static CR_MDSS_DSC_IO Io(HW *H) {
 CR_MDSS_DSC_IO I={H,0x82000,Ready,Read,Write,Fence};return I;
}
static void Setup(HW *H,CR_MDSS_DSC *D,UINT32 Encoder) {
 CR_MDSS_DSC_CONFIG C;CR_MDSS_DSC_PARAMS P=Params(2,0,8,8,540,8);
 CR_MDSS_DSC_ROUTE R={Encoder,Encoder,1,2,2,FALSE,FALSE,TRUE};CR_MDSS_DSC_IO I=Io(H);
 assert(CrMdssDscBuild(&P,&C)==CR_SUCCESS);assert(CrMdssDscInit(D,&I,&C,&R)==CR_SUCCESS);
}
static void Hardware(void) {
 HW H={0};CR_MDSS_DSC D;UINT32 StartCalls,StopCalls,Fault,Encoder;
 H.Regs=calloc(0x82000/4,4);assert(H.Regs);H.Dropped=0xffffffff;
 for(Encoder=0;Encoder<4;++Encoder){UINT32 Before=H.Writes;Setup(&H,&D,Encoder);assert(H.Writes==Before);
   assert(CrMdssDscStart(&D)==CR_SUCCESS&&D.Active);
   assert(H.Regs[(D.Base+D.EncoderOffset+0x34)/4]==((64U<<16)|1080U));
   assert(H.Regs[(D.Base+D.ControlOffset)/4]==Encoder);
   assert(H.Regs[(D.Base+D.ControlOffset+4)/4]==0x801);
   assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 }
 memset(H.Regs,0,0x82000);H.Calls=H.Writes=0;Setup(&H,&D,0);
 assert(CrMdssDscStart(&D)==CR_SUCCESS);StartCalls=H.Calls;H.Calls=0;
 assert(CrMdssDscStop(&D)==CR_SUCCESS);StopCalls=H.Calls;
 for(Fault=1;Fault<=StartCalls;++Fault){
  memset(H.Regs,0,0x82000);H.Calls=H.Writes=0;H.Fail=Fault;H.Failure=CR_DEVICE_ERROR;Setup(&H,&D,0);
  assert(CrMdssDscStart(&D)!=CR_SUCCESS);H.Fail=0;
  assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 }
 for(Fault=1;Fault<=StopCalls;++Fault){
  H.Fail=0;H.Calls=0;Setup(&H,&D,0);assert(CrMdssDscStart(&D)==CR_SUCCESS);
  H.Calls=0;H.Fail=Fault;H.Failure=CR_TIMEOUT;assert(CrMdssDscStop(&D)==CR_TIMEOUT);
  assert(D.Touched&&D.CleanupPending);
  assert(CrMdssDscStart(&D)==CR_NOT_READY);H.Fail=0;
  assert(CrMdssDscStop(&D)==CR_SUCCESS);assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 }
 H.Calls=0;H.Fail=0;Setup(&H,&D,0);H.Dropped=0x80f04;
 assert(CrMdssDscStart(&D)==CR_DEVICE_ERROR);assert(D.Touched);
 H.Dropped=0xffffffff;assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 H.Calls=0;H.LostAfter=40;Setup(&H,&D,0);assert(CrMdssDscStart(&D)==CR_NOT_READY);
 assert(CrMdssDscStop(&D)==CR_NOT_READY&&D.Touched);H.Lost=FALSE;H.LostAfter=0;
 assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 H.Calls=0;H.Fail=2;H.Failure=1;Setup(&H,&D,0);
 assert(CrMdssDscStart(&D)==CR_NOT_READY);H.Fail=0;assert(CrMdssDscUninit(&D)==CR_SUCCESS);
 free(H.Regs);printf("DSC actual encoder start %u/stop %u fault phases PASS\n",StartCalls,StopCalls);
}
int main(void){Pure();Hardware();return 0;}
