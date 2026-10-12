/* Source: Linux gpucc-sm8450, sm8450.dtsi + existing qualified inherited
 * CrPepGpuPower profile. Native private incarnation, no GMU/GX new authority.
 * SPDX-License-Identifier: MIT */
#include "GpuBootAccess.h"
#include <Library/IoLib.h>
#include <Library/BaseLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
static CONST CHAR8 *Names[10]={"gpu_cx_gdsc","gpu_cc_ahb_clk","gpu_cc_cx_gmu_clk","gpu_cc_cxo_clk",
 "gcc_ddrss_gpu_axi_clk","gcc_gpu_memnoc_gfx_clk","gpu_cc_hub_cx_int_clk","gpu_cc_demet_clk",
 "gpu_cc_hlos1_vote_gpu_smmu_clk","gcc_gpu_snoc_dvm_gfx_clk"};
static CONST UINT64 Rates[10]={0,150000000,200000000,19200000,0,0,150000000,19200000,0,0};
static EFI_STATUS Final(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
/* Acquisition outputs must not modify the retained provider or each other,
 * including on a declined call. Validate the entire span before any write. */
static BOOLEAN Range(CONST VOID *Buffer,UINTN Bytes){
 return Buffer&&Bytes&&(UINTN)Buffer<=MAX_UINTN-Bytes;
}
static BOOLEAN Overlap(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){
 return (UINTN)A<(UINTN)B+Bn&&(UINTN)B<(UINTN)A+An;
}
BOOLEAN CrGpuBootOutputAllowed(CONST CR_GPU_BOOT_CLOCK *P,CONST VOID *Buffer,UINTN Bytes){
 return P&&Range(P,sizeof(*P))&&Range(Buffer,Bytes)&&!Overlap(P,sizeof(*P),Buffer,Bytes);
}
static BOOLEAN Protected(CONST CR_GPU_BOOT_CLOCK *P,CONST ClockNode *N){
 if(!P||!P->Core||!N)return FALSE;
 if(N->ParentController==&P->Core->ClockControllers[CC_TYPE_GPU_CC]||N==P->Gpll)return TRUE;
 return N->Name&&(!AsciiStrnCmp(N->Name,"gcc_gpu_",8)||!AsciiStrCmp(N->Name,"gcc_ddrss_gpu_axi_clk"));
}
EFI_STATUS CrGpuBootClockGuard(CONST CR_GPU_BOOT_CLOCK *P,CONST ClockNode *N){return P&&P->Pinned&&Protected(P,N)?EFI_ACCESS_DENIED:EFI_SUCCESS;}
EFI_STATUS CrGpuBootResetGuard(CONST CR_GPU_BOOT_CLOCK *P,CONST CHAR8 *Cc,CONST CHAR8 *Name){
 if(!Cc||!Name)return EFI_INVALID_PARAMETER;
 return P&&P->Pinned&&(!AsciiStrCmp(Cc,"gpucc")||!AsciiStrCmp(Cc,"gpu_cc")||
 !AsciiStrnCmp(Name,"gpu_cc_",7)||!AsciiStrnCmp(Name,"gcc_gpu_",8))?EFI_ACCESS_DENIED:EFI_SUCCESS;
}
static CR_GPU_BOOT_CLOCK *Provider(EFI_CR_ADRENO_BOOT_ACCESS *I){
 if(!I)return NULL;CR_GPU_BOOT_CLOCK *P=BASE_CR(I,CR_GPU_BOOT_CLOCK,Protocol);
 return &P->Protocol==I&&P->Core&&P->Lock&&I->Revision==1?P:NULL;
}
static EFI_STATUS Profile(CR_GPU_BOOT_CLOCK *P,BOOLEAN Held){
 if(!P->Rpmh||P->Rpmh->Revision<0x10004||!P->Rpmh->EnsureBootFloor)return EFI_NOT_READY;
 EFI_STATUS S=Final(P->Rpmh->EnsureBootFloor(P->Rpmh));if(S!=EFI_SUCCESS)return S;
 ClockController *K=&P->Core->ClockControllers[CC_TYPE_GPU_CC];UINTN B=K->Address;
 if(!K->MemMapped||K->Size!=0xa000||B!=0x03d90000)return EFI_UNSUPPORTED;
 CrLockAcquire(&P->Core->Lock);
 if((MmioRead32(B+0x931c)&0x371f)!=0x602||(MmioRead32(B+0x93f0)&0x371f)!=0x603||
 (MmioRead32(B+0x9318)&1)||(MmioRead32(B+0x93ec)&1)||(MmioRead32(B+0x9108)&0xf002)!=0x8000||
 (MmioRead32(B+0x9014)&0x371f)!=1||(MmioRead32(B+0x9010)&1)||
 (MmioRead32(B+0x9054)&15)||(MmioRead32(B+0x942c)&15)||(MmioRead32(B+0x9430)&15)){S=EFI_UNSUPPORTED;goto Done;}
 CR_FIXED_PLL_VOTE Probe={0};CR_FIXED_PLL_DESCRIPTION D=P->Gpll->FixedPll.Description;
 D.Base=P->Gpll->ParentController->Address;D.Size=P->Gpll->ParentController->Size;
 S=(EFI_STATUS)CrFixedPllInit(&Probe,&D);if(S!=EFI_SUCCESS)goto Done;Probe.Owned=Probe.Ready=TRUE;
 S=(EFI_STATUS)CrFixedPllCheck(&Probe);if(S!=EFI_SUCCESS)goto Done;
 if(Held){if((MmioRead32(B+0x9108)&0x801)!=0x800||!(MmioRead32(B+0x953c)&BIT31)){S=EFI_NOT_READY;goto Done;}
  for(UINT32 N=1;N<10;++N){ClockNode *C=P->Nodes[N];if(!C->ReferenceCount||C->ReleasePending||
  (MmioRead32(C->ParentController->Address+C->EnableRegister)&C->EnableMsk)!=C->EnableMsk){S=EFI_NOT_READY;goto Done;}}
 }
Done:CrLockRelease(&P->Core->Lock);return Final(S);
}
static BOOLEAN Match(CR_GPU_BOOT_CLOCK *P,VOID *Owner,CONST CR_ADRENO_BOOT_ACCESS_LEASE *L){return P->Owner==Owner&&P->Identity==L&&
 L&&L->Provider==P&&L->Incarnation==P->Lease.Incarnation&&L->OwnedMask==P->OwnedMask&&!L->Reserved;}
static EFI_STATUS EFIAPI Begin(EFI_CR_ADRENO_BOOT_ACCESS *I,VOID *Owner,CR_ADRENO_BOOT_ACCESS_LEASE *L,BOOLEAN *Owned){
 CR_GPU_BOOT_CLOCK *P=Provider(I);
 if(!P||!Owner||EfiGetCurrentTpl()>TPL_CALLBACK||
    !CrGpuBootOutputAllowed(P,L,sizeof(*L))||!CrGpuBootOutputAllowed(P,Owned,sizeof(*Owned))||
    Overlap(L,sizeof(*L),Owned,sizeof(*Owned)))return EFI_INVALID_PARAMETER;
 *Owned=FALSE;
 if(L->Provider||L->Incarnation||L->OwnedMask||L->Reserved)return EFI_INVALID_PARAMETER;
 EFI_STATUS S;EFI_RPMH_CR_PROTOCOL *R=NULL;S=gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid,NULL,(VOID **)&R);if(S!=EFI_SUCCESS)return Final(S);
 EfiAcquireLock(P->Lock);if(P->Pinned){S=EFI_ACCESS_DENIED;goto Done;}
 P->Rpmh=R;S=Profile(P,FALSE);if(S!=EFI_SUCCESS)goto Done;
 // No native reference or pending release may already claim a protected node.
 for(UINT32 K=0;K<P->Core->ClockControllerCount;++K){ClockController *Cc=&P->Core->ClockControllers[K];
  for(UINT32 N=0;N<Cc->ClkCount;++N){ClockNode *C=Cc->Clks[N];if(C&&C!=P->Gpll&&Protected(P,C)&&(C->ReferenceCount||C->ReleasePending)){S=EFI_ACCESS_DENIED;goto Done;}}
  for(UINT32 N=0;N<Cc->GdscCount;++N){ClockNode *C=Cc->Gdscs[N];if(C&&Protected(P,C)&&(C->ReferenceCount||C->ReleasePending)){S=EFI_ACCESS_DENIED;goto Done;}}
 }
 P->Pinned=TRUE;P->Owner=Owner;P->Identity=L;P->Lease=(CR_ADRENO_BOOT_ACCESS_LEASE){P,GetPerformanceCounter(),0,0};
 if(!P->Lease.Incarnation)P->Lease.Incarnation=1;*L=P->Lease;*Owned=TRUE;
 for(UINT32 N=0;N<10;++N){BOOLEAN Acquired=FALSE;P->AttemptedMask|=1U<<N;
  S=N?(EFI_STATUS)ClockAcquire(P->Core,P->Nodes[N],Rates[N],&Acquired):(EFI_STATUS)ClockGdscAcquire(P->Core,P->Nodes[N],&Acquired);
  if(Acquired){P->OwnedMask|=1U<<N;L->OwnedMask=P->OwnedMask;P->Lease.OwnedMask=P->OwnedMask;}
  if(S!=EFI_SUCCESS||!Acquired){P->Uncertain=TRUE;S=S==EFI_SUCCESS?EFI_DEVICE_ERROR:Final(S);goto Done;}
 }
 S=Profile(P,TRUE);if(S!=EFI_SUCCESS){P->Uncertain=TRUE;goto Done;}P->Prepared=TRUE;
Done:EfiReleaseLock(P->Lock);return S;
}
static EFI_STATUS EFIAPI Check(EFI_CR_ADRENO_BOOT_ACCESS *I,VOID *Owner,CONST CR_ADRENO_BOOT_ACCESS_LEASE *L){
 CR_GPU_BOOT_CLOCK *P=Provider(I);if(!P||EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_INVALID_PARAMETER;
 EfiAcquireLock(P->Lock);EFI_STATUS S=Match(P,Owner,L)&&P->Prepared&&!P->Committed&&!P->Uncertain?Profile(P,TRUE):EFI_NOT_READY;
 if(S!=EFI_SUCCESS&&P->Pinned)P->Uncertain=TRUE;EfiReleaseLock(P->Lock);return S;
}
static EFI_STATUS EFIAPI Commit(EFI_CR_ADRENO_BOOT_ACCESS *I,VOID *Owner,CONST CR_ADRENO_BOOT_ACCESS_LEASE *L){
 CR_GPU_BOOT_CLOCK *P=Provider(I);if(!P||EfiGetCurrentTpl()>TPL_NOTIFY)return EFI_INVALID_PARAMETER;
 EfiAcquireLock(P->Lock);EFI_STATUS S=Match(P,Owner,L)&&P->Prepared&&!P->Committed&&!P->Uncertain?Profile(P,TRUE):EFI_NOT_READY;
 if(S==EFI_SUCCESS)P->Committed=TRUE;else if(P->Pinned)P->Uncertain=TRUE;EfiReleaseLock(P->Lock);return S;
}
EFI_STATUS CrGpuBootClockInit(CR_GPU_BOOT_CLOCK *P,ClockDriverContext *C,EFI_LOCK *L){
 if(!P||!C||!L||P->Core||!C->LockInitialized||C->ClockControllerCount<=CC_TYPE_GPU_CC)return EFI_INVALID_PARAMETER;
 P->Core=C;P->Lock=L;
 for(UINT32 N=0;N<10;++N){EFI_STATUS S=N?(EFI_STATUS)GetClockNode(C,Names[N],NULL,NULL,&P->Nodes[N]):(EFI_STATUS)GetGdscNode(C,Names[N],&P->Nodes[N]);if(S!=EFI_SUCCESS)return Final(S);}
 EFI_STATUS S=(EFI_STATUS)GetClockNode(C,"gcc_gpll0",NULL,NULL,&P->Gpll);if(S!=EFI_SUCCESS)return Final(S);
 if(P->Gpll->Type!=CLOCK_NODE_TYPE_FIXED_PLL||C->ClockControllers[CC_TYPE_GPU_CC].Address!=0x03d90000)return EFI_UNSUPPORTED;
 P->Protocol=(EFI_CR_ADRENO_BOOT_ACCESS){1,Begin,Check,Commit};return EFI_SUCCESS;
}
