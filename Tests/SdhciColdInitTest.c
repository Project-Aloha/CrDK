/* Actual portable v5 cold setup with inherited and failed MMIO. MIT */
#include <Library/SdhciMsmLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
#endif
typedef struct {
  UINT32 Vendor, Clock, Power, Step, FailAt, Writes, Barriers, Corrupt;
  CR_STATUS Failure, ReadyStatus;
} MODEL;
static CR_STATUS Step(MODEL *M) {
  return ++M->Step==M->FailAt?M->Failure:CR_SUCCESS;
}
static CR_STATUS Ready(VOID *P) { return ((MODEL *)P)->ReadyStatus; }
static CR_STATUS Read(VOID *P,UINT32 O,UINT8 W,UINT32 *V) {
  MODEL *M=P;CR_STATUS S=Step(M);if(S!=CR_SUCCESS)return S;
  if(O==0x2c){assert(W==2);*V=M->Clock;}
  else if(O==0x29){assert(W==1);*V=M->Power;}
  else {assert(O==0x20c&&W==4);*V=M->Vendor^(M->Writes==M->Corrupt?1:0);}
  return CR_SUCCESS;
}
static CR_STATUS Write(VOID *P,UINT32 O,UINT8 W,UINT32 V) {
  MODEL *M=P;assert(O==0x20c&&W==4);
  ++M->Writes;assert(V==(M->Writes==1?0xa9cU:0x8a9cU));
  /* Even a failed write may have reached hardware. */
  M->Vendor=V;return Step(M);
}
static CR_STATUS Stall(VOID *P,UINT32 Us){(VOID)P;(VOID)Us;assert(0);return CR_UNSUPPORTED;}
static CR_STATUS Barrier(VOID *P){MODEL *M=P;++M->Barriers;return Step(M);}
static void Init(CR_SDHC *C,MODEL *M){
  memset(M,0,sizeof(*M));M->Vendor=0xffffffffU;M->Failure=CR_DEVICE_ERROR;
  CR_SDHC_IO Io={M,Ready,Read,Write,Stall,Barrier,CR_SDHC_HOST_END,CR_SDHC_WINDOW_HOST};
  assert(CrSdhcInit(C,&Io)==CR_SUCCESS&&M->Step==0);
}
int main(void){
  CR_SDHC C;MODEL M;Init(&C,&M);
  C.DataActive=TRUE;
  assert(CrSdhcInit(&C,&C.Io)==CR_SUCCESS&&C.Io.Context==&M&&!C.DataActive);
  assert(CrSdhcColdInit(&C)==CR_SUCCESS&&M.Vendor==0x8a9c&&M.Writes==2&&M.Barriers==2&&!C.NeedsReset);
  for(UINT32 I=1;I<=8;++I){
    Init(&C,&M);M.FailAt=I;
    assert(CrSdhcColdInit(&C)==CR_DEVICE_ERROR&&M.Step==I);
    assert(C.NeedsReset==(I>=3));
    if(I>=3){UINT32 Before=M.Step;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&Before==M.Step);}
  }
  Init(&C,&M);M.FailAt=4;M.Failure=(CR_STATUS)1;
  assert(CrSdhcColdInit(&C)==(CR_STATUS)1&&M.Step==4&&C.NeedsReset);
  for(UINT32 I=1;I<=2;++I){Init(&C,&M);M.Corrupt=I;
    assert(CrSdhcColdInit(&C)==CR_DEVICE_ERROR&&C.NeedsReset&&M.Writes==I);}
  Init(&C,&M);M.Clock=4;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&!M.Writes);
  Init(&C,&M);M.Power=1;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&!M.Writes);
  Init(&C,&M);C.DataActive=TRUE;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&!M.Step);
  Init(&C,&M);C.NeedsReset=TRUE;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&!M.Step);
  Init(&C,&M);M.ReadyStatus=CR_NOT_READY;assert(CrSdhcColdInit(&C)==CR_NOT_READY&&!M.Step);
  Init(&C,&M);C.Io.Windows=CR_SDHC_WINDOW_POWER;
  assert(CrSdhcColdInit(&C)==CR_UNSUPPORTED&&!M.Step);
  assert(CrSdhcColdInit(NULL)==CR_NOT_READY);
  puts("SDHCI v5 cold init: inherited tuning, source POR/pad order, active-host rejection and every IO failure retained PASS");
  return 0;
}
