/** @file SPDX-License-Identifier: MIT **/
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/MdssCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P,V,N); }
#endif
typedef struct {
 UINT32 Dpu[0x45000/4],Dsi[0x200/4];
 UINTN Reads,Writes,Acquire,Release,Prepare,Unprepare,Publish,Transfer;
 BOOLEAN BlockProgress,BlockStop,BlockReset,FailAcquire,FailRelease,FailPanel,FailTransfer;
 UINT64 Device;
 CR_STATUS AcquireStatus,ReleaseStatus,PrepareStatus,UnprepareStatus,TransferStatus,IoFailure;
 UINTN IoCalls,IoFailAt;
} FIXTURE;
static FIXTURE F;
static UINT8 Fb[256],Source[256];
static UINT32 Read(VOID *Ctx,CR_MDSS_WINDOW W,UINT32 O) {
 FIXTURE *X=Ctx;++X->Reads;
 assert(!(O&3));
 if(W==CrMdssDsi) {assert(O<sizeof(X->Dsi));return X->Dsi[O/4];}
 assert(O<sizeof(X->Dpu));
 if(O==0x350AC && !X->BlockProgress) ++X->Dpu[O/4];
 return X->Dpu[O/4];
}
static VOID Write(VOID *Ctx,CR_MDSS_WINDOW Win,UINT32 O,UINT32 V) {
 FIXTURE *X=Ctx;++X->Writes;
 if(Win==CrMdssDsi) {assert(O<sizeof(X->Dsi));X->Dsi[O/4]=V;return;}
 assert(O<sizeof(X->Dpu));X->Dpu[O/4]=V;
 assert(O!=0x1512c&&O!=0x15130); /* DPU9-only, not SM8450 DPU8.1 */
 if(O==0x35000 && (V || !X->BlockStop)) X->Dpu[0x3526C/4]=V;
 if(O==0x15018 && !X->BlockProgress) X->Dpu[O/4]=0;
 if(O==0x15030 && !X->BlockReset) X->Dpu[O/4]=0;
}
static VOID Fence(VOID *C) {(void)C;}
static VOID Delay(VOID *C,UINT32 U) {(void)C;(void)U;}
static CR_STATUS Acquire(VOID *C,CONST CR_MDSS_MODE *M,UINT64 P,UINTN N,
 UINT64 *D,UINT32 *T,BOOLEAN *Owned) {
 FIXTURE *X=C;++X->Acquire;assert(P==0xB8000000&&N==sizeof(Fb)&&M->Width==8);
 *Owned=TRUE;*D=X->Device;*T=0x110;
 return X->FailAcquire?CR_DEVICE_ERROR:X->AcquireStatus;
}
static CR_STATUS Release(VOID *C) {FIXTURE *X=C;++X->Release;return X->FailRelease?CR_DEVICE_ERROR:X->ReleaseStatus;}
static CR_STATUS Prepare(VOID *C,CONST CR_MDSS_MODE *M) {FIXTURE *X=C;++X->Prepare;(void)M;return X->FailPanel?CR_DEVICE_ERROR:X->PrepareStatus;}
static CR_STATUS Unprepare(VOID *C,CONST CR_MDSS_MODE *M) {FIXTURE *X=C;++X->Unprepare;(void)M;return X->UnprepareStatus;}
static VOID Publish(VOID *C,VOID *P,UINTN N) {FIXTURE *X=C;++X->Publish;assert(P==Fb&&N==128);}
static CR_STATUS Transfer(VOID *C) {FIXTURE *X=C;++X->Transfer;return X->FailTransfer?CR_DEVICE_ERROR:X->TransferStatus;}
static CR_STATUS IoStep(VOID *C){FIXTURE *X=C;return ++X->IoCalls==X->IoFailAt?X->IoFailure:CR_SUCCESS;}
static CR_STATUS ReadChecked(VOID *C,CR_MDSS_WINDOW W,UINT32 O,UINT32 *V){CR_STATUS S=IoStep(C);if(S==CR_SUCCESS)*V=Read(C,W,O);return S;}
static CR_STATUS WriteChecked(VOID *C,CR_MDSS_WINDOW W,UINT32 O,UINT32 V){CR_STATUS S=IoStep(C);if(S==CR_SUCCESS)Write(C,W,O,V);return S;}
static CR_STATUS FenceChecked(VOID *C){return IoStep(C);}
static CR_STATUS DelayChecked(VOID *C,UINT32 Us){(void)Us;return IoStep(C);}
static CR_MDSS_MODE Mode(void) {
 CR_MDSS_MODE M={0};M.Soc=CR_MDSS_SOC_SM8450;M.Width=8;M.Height=4;M.Stride=32;
 M.HSync=2;M.HBackPorch=2;M.HFrontPorch=2;M.VSync=2;M.VBackPorch=2;M.VFrontPorch=3;
 M.RefreshQ16=60U<<16;M.Lanes=4;M.VideoFlags=CR_MDSS_VIDEO_LP_MASK;return M;
}
static CR_MDSS_IO Io(void) {
 CR_MDSS_IO I={.Context=&F,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay,
  .Acquire=Acquire,.PreparePanel=Prepare,.UnpreparePanel=Unprepare,.Release=Release,
  .PublishCpuWrites=Publish,.TransferToPost=Transfer};return I;
}
static VOID Reset(CR_MDSS *D) {
 CR_MDSS_MODE M=Mode();CR_MDSS_IO I=Io();memset(&F,0,sizeof(F));memset(Fb,0,sizeof(Fb));
 F.Dsi[0x1F4/4]=0x30000000;F.Device=0x80000000;
 assert(CrMdssInit(D,&I,&M,Fb,0xB8000000,sizeof(Fb))==CR_SUCCESS);
}
static VOID Checked(CR_MDSS *D){Reset(D);D->Io.Read32Checked=ReadChecked;
 D->Io.Write32Checked=WriteChecked;D->Io.FenceChecked=FenceChecked;D->Io.DelayUsChecked=DelayChecked;}
static VOID StatusAndIo(void){CR_MDSS D;UINTN StartCalls,StopCalls;
 for(UINTN N=0;N<2;++N){CR_STATUS Pending=N?0x103:1;
  Reset(&D);F.AcquireStatus=Pending;assert(CrMdssStart(&D)==CR_NOT_READY&&D.ResourcesOwned&&D.CleanupPending&&!F.Writes);
  F.AcquireStatus=0;assert(CrMdssStop(&D)==CR_SUCCESS&&CrMdssUninit(&D)==CR_SUCCESS);
  Reset(&D);F.PrepareStatus=Pending;assert(CrMdssStart(&D)==CR_NOT_READY&&D.HostTouched&&D.PanelAttempted&&!D.ScanoutTouched);
  F.PrepareStatus=0;assert(CrMdssStop(&D)==CR_SUCCESS);
  Reset(&D);assert(CrMdssStart(&D)==CR_SUCCESS);F.UnprepareStatus=Pending;
  assert(CrMdssStop(&D)==CR_NOT_READY&&D.HostTouched&&D.PanelAttempted&&D.ResourcesOwned&&!F.Release);
  F.UnprepareStatus=0;assert(CrMdssStop(&D)==CR_SUCCESS);
  Reset(&D);assert(CrMdssStart(&D)==CR_SUCCESS);F.ReleaseStatus=Pending;
  assert(CrMdssStop(&D)==CR_NOT_READY&&D.ResourcesOwned&&D.CleanupPending);
  F.ReleaseStatus=0;assert(CrMdssStop(&D)==CR_SUCCESS);
  Reset(&D);assert(CrMdssStart(&D)==CR_SUCCESS);F.TransferStatus=Pending;
  assert(CrMdssTransferToPost(&D)==CR_NOT_READY&&D.Initialized&&D.ResourcesOwned&&D.Active);
  F.TransferStatus=0;assert(CrMdssStop(&D)==CR_SUCCESS);
 }
 Checked(&D);assert(CrMdssStart(&D)==CR_SUCCESS);StartCalls=F.IoCalls;F.IoCalls=0;
 assert(CrMdssStop(&D)==CR_SUCCESS);StopCalls=F.IoCalls;
 for(UINTN N=0;N<3;++N)for(UINTN I=1;I<=StartCalls;++I){
  Checked(&D);F.IoFailure=N==0?CR_DEVICE_ERROR:N==1?1:0x103;F.IoFailAt=I;
  assert(CrMdssStart(&D)==(N?CR_NOT_READY:CR_DEVICE_ERROR)&&D.ResourcesOwned&&D.CleanupPending&&F.IoCalls==I);
  F.IoFailAt=0;assert(CrMdssStop(&D)==CR_SUCCESS&&CrMdssUninit(&D)==CR_SUCCESS);
 }
 for(UINTN N=0;N<3;++N)for(UINTN I=1;I<=StopCalls;++I){
  Checked(&D);assert(CrMdssStart(&D)==CR_SUCCESS);F.IoCalls=0;F.IoFailure=N==0?CR_DEVICE_ERROR:N==1?1:0x103;F.IoFailAt=I;
  assert(CrMdssStop(&D)==(N?CR_NOT_READY:CR_DEVICE_ERROR)&&D.ResourcesOwned&&D.CleanupPending&&F.IoCalls==I&&!F.Release);
  F.IoFailAt=0;assert(CrMdssStop(&D)==CR_SUCCESS&&CrMdssUninit(&D)==CR_SUCCESS);
 }
 Checked(&D);assert(CrMdssStart(&D)==CR_SUCCESS);F.IoCalls=0;F.IoFailAt=1;F.IoFailure=CR_DEVICE_ERROR;
 assert(CrMdssStart(&D)==CR_DEVICE_ERROR&&!D.Active&&D.CleanupPending&&F.IoCalls==1);
 F.IoFailAt=0;assert(CrMdssStop(&D)==CR_SUCCESS);
 CR_MDSS_IO Partial=Io();Partial.Read32Checked=ReadChecked;CR_MDSS_MODE M=Mode();
 assert(CrMdssInit(&D,&Partial,&M,Fb,0xb8000000,sizeof(Fb))==CR_INVALID_PARAMETER);
 printf("MDSS checked IO: %zu start/stop failure phases and exact-status ownership retention PASS\n",(size_t)(3*(StartCalls+StopCalls)));
}
static CR_STATUS PipelineStart(VOID *C,CONST CR_MDSS_MODE *M,UINT64 Pa,
 UINTN Bytes,UINT64 *Device,BOOLEAN *Owned){
 FIXTURE *X=C;assert(M->Width==8&&Pa==0xb8000000&&Bytes==sizeof(Fb));
 ++X->Acquire;*Device=X->Device;*Owned=TRUE;return X->AcquireStatus;
}
static CR_STATUS PipelineStop(VOID *C,BOOLEAN *Owned){
 FIXTURE *X=C;++X->Release;if(X->ReleaseStatus==CR_SUCCESS)*Owned=FALSE;return X->ReleaseStatus;
}
static VOID Pipelines(void){
 CR_MDSS D={0};CR_MDSS_MODE M=Mode();CR_MDSS_IO I={0};
 memset(&F,0,sizeof(F));F.Device=0x10000000;I.Context=&F;I.PublishCpuWrites=Publish;
 I.PipelineRevision=1;I.StartPipeline=PipelineStart;
 assert(CrMdssInit(&D,&I,&M,Fb,0xb8000000,sizeof(Fb))==CR_INVALID_PARAMETER&&!F.Acquire);
 I.StopPipeline=PipelineStop;I.PipelineRevision=2;
 assert(CrMdssInit(&D,&I,&M,Fb,0xb8000000,sizeof(Fb))==CR_INVALID_PARAMETER);
 I.PipelineRevision=1;I.TransferToPost=Transfer;
 assert(CrMdssInit(&D,&I,&M,Fb,0xb8000000,sizeof(Fb))==CR_SUCCESS);
 assert(CrMdssStart(&D)==CR_SUCCESS&&D.Active&&D.ResourcesOwned&&!F.Reads&&!F.Writes&&!F.Prepare);
 CR_MDSS_RECT R={0,0,8,4};assert(CrMdssPresent(&D,Source,sizeof(Source),32,NULL,0,&R,1)==CR_SUCCESS);
 assert(CrMdssTransferToPost(&D)==CR_SUCCESS&&!D.Initialized&&!F.Release);
 CONST CR_STATUS Status[]={CR_DEVICE_ERROR,1,0x103};
 for(UINTN N=0;N<3;++N){
  assert(CrMdssInit(&D,&I,&M,Fb,0xb8000000,sizeof(Fb))==CR_SUCCESS);
  F.AcquireStatus=Status[N];F.ReleaseStatus=Status[N];
  assert(CrMdssStart(&D)==(N?CR_NOT_READY:CR_DEVICE_ERROR)&&D.CleanupPending&&D.ResourcesOwned&&!D.Active);
  UINTN Before=F.Acquire;assert(CrMdssStart(&D)==CR_NOT_READY&&F.Acquire==Before);
  assert(CrMdssStop(&D)==(N?CR_NOT_READY:CR_DEVICE_ERROR)&&D.ResourcesOwned&&D.CleanupPending);
  F.ReleaseStatus=CR_SUCCESS;assert(CrMdssStop(&D)==CR_SUCCESS&&!D.ResourcesOwned);
  assert(CrMdssUninit(&D)==CR_SUCCESS&&!F.Reads&&!F.Writes&&!F.Unprepare);
 }
 F.AcquireStatus=CR_SUCCESS;
 assert(CrMdssInit(&D,&I,&M,Fb,0xb8000000,sizeof(Fb))==CR_SUCCESS);
 F.Device=0x100000000ULL;assert(CrMdssStart(&D)==CR_BAD_BUFFER_SIZE&&D.ResourcesOwned);
 assert(CrMdssStop(&D)==CR_SUCCESS&&CrMdssUninit(&D)==CR_SUCCESS);
 puts("MDSS pipeline delegation: paired preflight, actual-completion, retained ownership and shared CPU/POST PASS");
}
int main(void) {
 CR_MDSS D;CR_MDSS_MODE M=Mode();UINT32 Fetch;
 assert(CrMdssGetFetchStart(&M,&Fetch)==CR_SUCCESS&&Fetch==127);
 M.VideoFlags=1;assert(CrMdssValidateMode(&M,sizeof(Fb))==CR_INVALID_PARAMETER);
 M=Mode();M.Stride=0x10000;assert(CrMdssValidateMode(&M,(UINTN)-1)==CR_BAD_BUFFER_SIZE);
 Reset(&D);assert(F.Reads==0&&F.Writes==0&&F.Acquire==0);
 assert(CrMdssStart(&D)==CR_SUCCESS&&D.Active&&F.Acquire==1&&F.Prepare==1);
 assert(F.Dpu[0x24030/4]==0x236FF&&F.Dpu[0x35064/4]==(11U<<16|4));
 assert(CrMdssStart(&D)==CR_SUCCESS&&F.Acquire==1);
 assert(CrMdssUninit(&D)==CR_BUSY);
 for(UINTN I=0;I<128;++I)Source[I]=(UINT8)I;
 CR_MDSS_RECT Full={0,0,8,4};
 assert(CrMdssPresent(&D,Source,sizeof(Source),32,NULL,0,&Full,1)==CR_SUCCESS);
 assert(!memcmp(Fb,Source,128));
 CR_MDSS_MOVE Down={{1,1,7,4},0,0};UINT8 Want[128];memcpy(Want,Fb,128);
 for(UINTN Y=0;Y<3;++Y)memcpy(Want+(Y+1)*32+4,Source+Y*32,24);
 assert(CrMdssPresent(&D,NULL,0,0,&Down,1,NULL,0)==CR_SUCCESS&&!memcmp(Fb,Want,128));
 CR_MDSS_RECT Bad[]={Full,{-1,0,2,1}};memcpy(Want,Fb,128);
 assert(CrMdssPresent(&D,Source,sizeof(Source),32,NULL,0,Bad,2)==CR_INVALID_PARAMETER);
 assert(!memcmp(Fb,Want,128));
 assert(CrMdssPresent(&D,Source,127,32,NULL,0,&Full,1)==CR_BAD_BUFFER_SIZE);
 F.BlockStop=TRUE;assert(CrMdssStop(&D)==CR_TIMEOUT&&D.CleanupPending&&D.ResourcesOwned);
 assert(F.Release==0&&F.Unprepare==0);
 assert(CrMdssStart(&D)==CR_NOT_READY);
 F.BlockStop=FALSE;F.BlockReset=TRUE;
 assert(CrMdssStop(&D)==CR_TIMEOUT&&F.Release==0);
 F.BlockReset=FALSE;F.FailRelease=TRUE;
 assert(CrMdssStop(&D)==CR_DEVICE_ERROR&&D.ResourcesOwned&&F.Unprepare==1);
 F.FailRelease=FALSE;assert(CrMdssStop(&D)==CR_SUCCESS&&F.Unprepare==1&&F.Release==2);
 assert(CrMdssUninit(&D)==CR_SUCCESS);
 Reset(&D);F.FailAcquire=TRUE;
 assert(CrMdssStart(&D)==CR_DEVICE_ERROR&&D.ResourcesOwned&&F.Writes==0);
 assert(CrMdssStop(&D)==CR_SUCCESS&&F.Release==1);
 Reset(&D);F.Device=0x100000000ULL;
 assert(CrMdssStart(&D)==CR_BAD_BUFFER_SIZE&&F.Writes==0&&D.ResourcesOwned);
 assert(CrMdssStop(&D)==CR_SUCCESS);
 StatusAndIo();
 Pipelines();
 Reset(&D);assert(CrMdssTransferToPost(&D)==CR_NOT_READY&&F.Transfer==0);
 assert(CrMdssStart(&D)==CR_SUCCESS);UINTN Writes=F.Writes,Reads=F.Reads;
 F.FailTransfer=TRUE;assert(CrMdssTransferToPost(&D)==CR_DEVICE_ERROR&&D.Active&&D.ResourcesOwned);
 assert(F.Writes==Writes&&F.Reads==Reads&&F.Release==0&&F.Unprepare==0);
 F.FailTransfer=FALSE;assert(CrMdssTransferToPost(&D)==CR_SUCCESS&&!D.Initialized&&F.Transfer==2);
 assert(F.Writes==Writes&&F.Reads==Reads&&F.Release==0&&F.Unprepare==0);
 assert(CrMdssStop(&D)==CR_INVALID_PARAMETER);
 Reset(&D);F.FailPanel=TRUE;
 assert(CrMdssStart(&D)==CR_DEVICE_ERROR&&D.HostTouched&&D.PanelAttempted&&!D.ScanoutTouched);
 assert(CrMdssStop(&D)==CR_SUCCESS&&F.Unprepare==1);
 Reset(&D);F.BlockProgress=TRUE;
 assert(CrMdssStart(&D)==CR_TIMEOUT&&D.ScanoutTouched&&D.ResourcesOwned);
 assert(CrMdssStop(&D)==CR_SUCCESS);
 puts("MDSS actual DPU/DSI sequencing, retained cleanup and validated overlap present: PASS");
 return 0;
}
