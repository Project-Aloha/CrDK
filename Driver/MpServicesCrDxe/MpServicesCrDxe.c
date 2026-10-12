/** Bounded PSCI lifecycle, blocking procedure dispatch. SPDX-License-Identifier: MIT */
#include "MpCrInternal.h"
#include <Guid/ArmMpCoreInfo.h>
#include <Guid/EventGroup.h>
#include <IndustryStandard/ArmStdSmc.h>
#include <Library/ArmLib.h>
#include <Library/ArmMmuLib.h>
#include <Library/ArmSmcLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>

typedef struct {
  UINT64 Affinity;
  CR_MP_AP_FRAME *Frame;
  EFI_PHYSICAL_ADDRESS Stack;
  BOOLEAN Available,Enabled,Healthy,MayBeOn;
} CR_MP_CPU;
typedef struct {
  EFI_MP_SERVICES_PROTOCOL Protocol;
  CR_MP_CPU Cpu[CR_MP_MAX_CPUS];
  UINTN Count,Bsp;
  UINT32 El;
  EFI_PHYSICAL_ADDRESS Entry;
  EFI_EVENT ExitEvent;
  BOOLEAN Busy,Closed,Published,Retained;
} CR_MP_STATE;
STATIC CR_MP_STATE mMp;
STATIC BOOLEAN FpQualified(CONST CR_MP_AP_FRAME *F) {
  return F->El==1?(F->FpControl&0x300000U)==0x300000U:
         (F->El==2&&(F->FpControl&0x400U)==0);
}

STATIC INT32 Psci(UINT32 Id,UINT64 A1,UINT64 A2,UINT64 A3) {
  ARM_SMC_ARGS A;
  ZeroMem(&A,sizeof(A));
  A.Arg0=Id; A.Arg1=A1; A.Arg2=A2; A.Arg3=A3;
  ArmCallSmc(&A);
  return (INT32)(UINT32)A.Arg0;
}
STATIC UINT32 Stage(CR_MP_AP_FRAME *F) {
  /* CPU_ON's cache-off READY/CLOSE path writes physical memory directly.
     No BSP-dirty mailbox bytes exist while the AP may publish a stage. */
  WriteBackInvalidateDataCacheRange(F,sizeof(*F));
  UINT32 S=F->Stage;
  ArmDataSynchronizationBarrier();
  return S;
}
STATIC VOID Command(CR_MP_AP_FRAME *F,UINT32 C) {
  /* Refresh the line before writing Command, preserving a cache-off AP's
     newer Stage on the same cache line. All other BSP updates were cleaned. */
  WriteBackInvalidateDataCacheRange(F,64);
  ArmDataSynchronizationBarrier();
  F->Command=C;
  WriteBackDataCacheRange(F,64);
  MpCrWake();
}
STATIC EFI_STATUS EFIAPI Who(EFI_MP_SERVICES_PROTOCOL *This,UINTN *Out) {
  if(This!=&mMp.Protocol||Out==NULL)return EFI_INVALID_PARAMETER;
  UINT64 A=GET_MPIDR_AFFINITY_BITS(ArmReadMpidr());
  for(UINTN I=0;I<mMp.Count;++I)if(mMp.Cpu[I].Affinity==A){*Out=I;return EFI_SUCCESS;}
  return EFI_NOT_FOUND;
}
STATIC EFI_STATUS BspCall(EFI_MP_SERVICES_PROTOCOL *This) {
  UINTN I;
  EFI_STATUS S=Who(This,&I);
  if(S!=EFI_SUCCESS)return S;
  if(I!=mMp.Bsp)return EFI_DEVICE_ERROR;
  if(mMp.Closed)return EFI_UNSUPPORTED;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Begin(EFI_MP_SERVICES_PROTOCOL *This) {
  EFI_STATUS S=BspCall(This);
  if(S!=EFI_SUCCESS)return S;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);
  if(mMp.Busy||mMp.Closed)S=EFI_NOT_READY;
  else mMp.Busy=TRUE;
  gBS->RestoreTPL(T);
  return S;
}
STATIC VOID End(VOID) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);
  mMp.Busy=FALSE;
  gBS->RestoreTPL(T);
}
/* Success retires an accepted CPU_ON only after the actual firmware says OFF.
   No mailbox stage is a substitute for this probe. */
STATIC EFI_STATUS WaitOff(CR_MP_CPU *C) {
  for(UINTN N=0;N<=CR_MP_TRANSITION_TIMEOUT_US/CR_MP_POLL_US;++N){
    INT32 P=Psci(ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64,C->Affinity,0,0);
    if(P==ARM_SMC_ID_PSCI_AFFINITY_INFO_OFF){C->MayBeOn=FALSE;return EFI_SUCCESS;}
    if(P!=ARM_SMC_ID_PSCI_AFFINITY_INFO_ON&&P!=ARM_SMC_ID_PSCI_AFFINITY_INFO_ON_PENDING)
      return EFI_DEVICE_ERROR;
    if(N!=CR_MP_TRANSITION_TIMEOUT_US/CR_MP_POLL_US)gBS->Stall(CR_MP_POLL_US);
  }
  return EFI_TIMEOUT;
}
STATIC VOID DisableFailed(CR_MP_CPU *C) {
  C->Available=FALSE; C->Enabled=FALSE; C->Healthy=FALSE;
}
STATIC EFI_STATUS Start(CR_MP_CPU *C) {
  if(C->MayBeOn)return EFI_NOT_READY;
  INT32 P=Psci(ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64,C->Affinity,0,0);
  if(P!=ARM_SMC_ID_PSCI_AFFINITY_INFO_OFF)return EFI_NOT_READY;
  CR_MP_AP_FRAME *F=C->Frame;
  F->Command=CR_MP_COMMAND_WAIT; F->Stage=CR_MP_STAGE_STARTING;
  F->Fault=0; F->OffResult=0; F->Procedure=NULL; F->Argument=NULL; F->Dispatch=NULL;
  WriteBackDataCacheRange(F,sizeof(*F));
  ArmDataSynchronizationBarrier();
  P=Psci(ARM_SMC_ID_PSCI_CPU_ON_AARCH64,C->Affinity,mMp.Entry,(UINTN)F);
  if(P!=ARM_SMC_PSCI_RET_SUCCESS){Command(F,CR_MP_COMMAND_CLOSE);DisableFailed(C);return EFI_DEVICE_ERROR;}
  C->MayBeOn=TRUE;
  for(UINTN N=0;N<=CR_MP_TRANSITION_TIMEOUT_US/CR_MP_POLL_US;++N){
    UINT32 S=Stage(F);
    if(S==CR_MP_STAGE_READY)return EFI_SUCCESS;
    if(S==CR_MP_STAGE_FAULT||S==CR_MP_STAGE_OFF_FAILED)break;
    if(N!=CR_MP_TRANSITION_TIMEOUT_US/CR_MP_POLL_US)gBS->Stall(CR_MP_POLL_US);
  }
  /* An accepted but late CPU keeps its reserved mailbox/entry/stack. It sees
     CLOSE before ever reading a procedure or reclaimable image address. */
  Command(F,CR_MP_COMMAND_CLOSE);
  EFI_STATUS S=WaitOff(C);
  DisableFailed(C);
  return S==EFI_SUCCESS?EFI_TIMEOUT:S;
}
STATIC EFI_STATUS Probe(CR_MP_CPU *C) {
  EFI_STATUS S=Start(C);
  if(S!=EFI_SUCCESS)return S;
  Command(C->Frame,CR_MP_COMMAND_CLOSE);
  S=WaitOff(C);
  if(S!=EFI_SUCCESS){DisableFailed(C);return S;}
  C->Available=TRUE;C->Healthy=TRUE;
  return EFI_SUCCESS;
}
/* AP code has no Boot Services calls, allocation or logging. */
STATIC VOID EFIAPI Dispatch(VOID *Context) {
  CR_MP_AP_FRAME *F=Context;
  F->Procedure(F->Argument);
}
STATIC EFI_STATUS Launch(CR_MP_CPU *C,EFI_AP_PROCEDURE P,VOID *A) {
  EFI_STATUS S=Start(C);
  if(S!=EFI_SUCCESS)return S;
  CR_MP_AP_FRAME *F=C->Frame;
  MpCrCaptureRegisters(F);
  if(F->El!=mMp.El||(F->Sctlr&1U)==0||!FpQualified(F)){
    Command(F,CR_MP_COMMAND_CLOSE);S=WaitOff(C);DisableFailed(C);
    return S==EFI_SUCCESS?EFI_UNSUPPORTED:S;
  }
  F->Procedure=P;F->Argument=A;F->Dispatch=Dispatch;
  /* Clean image, immutable topology and caller inputs before enabling an AP's
     translation/cache. Mailbox accesses use explicit cache-off transition
     maintenance while preserving the platform's WB memory classification. */
  WriteBackDataCacheRange(F,sizeof(*F));
  WriteBackDataCacheRange(&mMp,sizeof(mMp));
  if(mMp.Closed){Command(F,CR_MP_COMMAND_CLOSE);S=WaitOff(C);return S==EFI_SUCCESS?EFI_ABORTED:S;}
  Command(F,CR_MP_COMMAND_RUN);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Finish(CR_MP_CPU *C) {
  CR_MP_AP_FRAME *F=C->Frame;
  /* Timeout 0 is intentionally unbounded procedure execution, as PI specifies.
     Finite-timeout and asynchronous requests are refused before any CPU_ON. */
  UINT32 S;
  do { S=Stage(F); if(S!=CR_MP_STAGE_DONE&&S!=CR_MP_STAGE_FAULT&&S!=CR_MP_STAGE_OFF_FAILED)
         gBS->Stall(CR_MP_POLL_US);
  } while(S!=CR_MP_STAGE_DONE&&S!=CR_MP_STAGE_FAULT&&S!=CR_MP_STAGE_OFF_FAILED);
  EFI_STATUS R=WaitOff(C);
  if(R!=EFI_SUCCESS||F->Fault!=0){DisableFailed(C);return R==EFI_SUCCESS?EFI_DEVICE_ERROR:R;}
  F->Procedure=NULL;F->Argument=NULL;F->Dispatch=NULL;
  WriteBackDataCacheRange(F,sizeof(*F));
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Count(EFI_MP_SERVICES_PROTOCOL *This,UINTN *Total,UINTN *Enabled) {
  if(Total==NULL||Enabled==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=BspCall(This);if(S!=EFI_SUCCESS)return S;
  UINTN E=0;for(UINTN I=0;I<mMp.Count;++I)if(mMp.Cpu[I].Enabled)++E;
  *Total=mMp.Count;*Enabled=E;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Info(EFI_MP_SERVICES_PROTOCOL *This,UINTN Number,EFI_PROCESSOR_INFORMATION *Out) {
  if(Out==NULL)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=BspCall(This);if(S!=EFI_SUCCESS)return S;
  BOOLEAN Extended=(Number&CPU_V2_EXTENDED_TOPOLOGY)!=0;
  Number&=~(UINTN)CPU_V2_EXTENDED_TOPOLOGY;
  if(Number>=mMp.Count)return EFI_NOT_FOUND;
  CR_MP_CPU *C=&mMp.Cpu[Number];EFI_PROCESSOR_INFORMATION P;ZeroMem(&P,sizeof(P));
  P.ProcessorId=C->Affinity;
  P.StatusFlag=(Number==mMp.Bsp?PROCESSOR_AS_BSP_BIT:0)|
    (C->Enabled?PROCESSOR_ENABLED_BIT:0)|(C->Healthy?PROCESSOR_HEALTH_STATUS_BIT:0);
  P.Location.Package=GET_MPIDR_AFF3(C->Affinity);
  P.Location.Core=(GET_MPIDR_AFF2(C->Affinity)<<8)|GET_MPIDR_AFF1(C->Affinity);
  P.Location.Thread=GET_MPIDR_AFF0(C->Affinity);
  if(Extended){P.ExtendedInformation.Location2.Package=P.Location.Package;
    P.ExtendedInformation.Location2.Core=P.Location.Core;
    P.ExtendedInformation.Location2.Thread=P.Location.Thread;}
  CopyMem(Out,&P,sizeof(P));return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI ThisAp(EFI_MP_SERVICES_PROTOCOL *This,EFI_AP_PROCEDURE P,
  UINTN I,EFI_EVENT Event,UINTN Timeout,VOID *Arg,BOOLEAN *Finished) {
  EFI_STATUS S=BspCall(This);if(S!=EFI_SUCCESS)return S;
  if(P==NULL)return EFI_INVALID_PARAMETER;
  if(I>=mMp.Count)return EFI_NOT_FOUND;
  if(I==mMp.Bsp||!mMp.Cpu[I].Enabled)return EFI_INVALID_PARAMETER;
  if(Event!=NULL||Timeout!=0)return EFI_UNSUPPORTED;
  S=Begin(This);if(S!=EFI_SUCCESS)return S;
  if(Finished!=NULL)*Finished=FALSE;
  S=Launch(&mMp.Cpu[I],P,Arg);
  if(S==EFI_SUCCESS)S=Finish(&mMp.Cpu[I]);
  if(S==EFI_SUCCESS&&Finished!=NULL)*Finished=TRUE;
  End();return S;
}
STATIC EFI_STATUS EFIAPI All(EFI_MP_SERVICES_PROTOCOL *This,EFI_AP_PROCEDURE P,
  BOOLEAN Sequential,EFI_EVENT Event,UINTN Timeout,VOID *Arg,UINTN **Failed) {
  EFI_STATUS S=BspCall(This);if(S!=EFI_SUCCESS)return S;
  if(P==NULL)return EFI_INVALID_PARAMETER;
  if(Event!=NULL||Timeout!=0)return EFI_UNSUPPORTED;
  S=Begin(This);if(S!=EFI_SUCCESS)return S;
  UINTN *Failures=NULL,FailureCount=0,Selected=0;
  BOOLEAN Running[CR_MP_MAX_CPUS];ZeroMem(Running,sizeof(Running));
  for(UINTN I=0;I<mMp.Count;++I)if(I!=mMp.Bsp&&mMp.Cpu[I].Enabled)++Selected;
  if(Selected==0){End();return EFI_NOT_STARTED;}
  /* Allocate caller-owned failure storage before starting any procedure. */
  if(Failed!=NULL){S=gBS->AllocatePool(EfiBootServicesData,(Selected+1)*sizeof(UINTN),(VOID **)&Failures);
    if(S!=EFI_SUCCESS){End();return S;}}
  EFI_STATUS Result=EFI_SUCCESS;
  for(UINTN I=0;I<mMp.Count;++I){
    if(I==mMp.Bsp||!mMp.Cpu[I].Enabled)continue;
    S=Launch(&mMp.Cpu[I],P,Arg);
    if(S==EFI_SUCCESS&&Sequential)S=Finish(&mMp.Cpu[I]);
    else if(S==EFI_SUCCESS)Running[I]=TRUE;
    if(S!=EFI_SUCCESS){if(Failures)Failures[FailureCount]=I;++FailureCount;Result=S;}
  }
  for(UINTN I=0;I<mMp.Count;++I)if(Running[I]){
    S=Finish(&mMp.Cpu[I]);
    if(S!=EFI_SUCCESS){if(Failures)Failures[FailureCount]=I;++FailureCount;Result=S;}
  }
  if(Failed!=NULL){if(FailureCount){Failures[FailureCount]=END_OF_CPU_LIST;*Failed=Failures;}
    else {gBS->FreePool(Failures);*Failed=NULL;}}
  End();return Result;
}
STATIC EFI_STATUS EFIAPI Switch(EFI_MP_SERVICES_PROTOCOL *This,UINTN I,BOOLEAN EnableOld) {
  (VOID)I;(VOID)EnableOld;
  EFI_STATUS S=BspCall(This);return S==EFI_SUCCESS?EFI_UNSUPPORTED:S;
}
STATIC EFI_STATUS EFIAPI Enable(EFI_MP_SERVICES_PROTOCOL *This,UINTN I,BOOLEAN On,UINT32 *Health) {
  EFI_STATUS S=BspCall(This);if(S!=EFI_SUCCESS)return S;
  if(I>=mMp.Count)return EFI_NOT_FOUND;
  if(I==mMp.Bsp)return EFI_INVALID_PARAMETER;
  S=Begin(This);if(S!=EFI_SUCCESS)return S;
  CR_MP_CPU *C=&mMp.Cpu[I];
  if(C->MayBeOn||!C->Available)S=EFI_NOT_READY;
  else if(On){S=Probe(C);if(S==EFI_SUCCESS)C->Enabled=TRUE;}
  else C->Enabled=FALSE;
  if(S==EFI_SUCCESS&&Health!=NULL)C->Healthy=(*Health&PROCESSOR_HEALTH_STATUS_BIT)!=0;
  End();return S;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  mMp.Closed=TRUE;ArmDataSynchronizationBarrier();
  for(UINTN I=0;I<mMp.Count;++I)if(I!=mMp.Bsp&&mMp.Cpu[I].Frame)
    Command(mMp.Cpu[I].Frame,CR_MP_COMMAND_CLOSE);
  for(UINTN I=0;I<mMp.Count;++I)if(I!=mMp.Bsp&&mMp.Cpu[I].MayBeOn){
    EFI_STATUS S=WaitOff(&mMp.Cpu[I]);
    if(S!=EFI_SUCCESS)DEBUG((DEBUG_ERROR,"MpCr: EBS CPU %lx not OFF: %r; reserved frame retained\n",mMp.Cpu[I].Affinity,S));
  }
}
STATIC EFI_STATUS EFIAPI NoUnload(EFI_HANDLE Image) {(VOID)Image;return EFI_ACCESS_DENIED;}

/* Init failures leave no event/protocol in a reclaimable image. A late accepted
   CPU_ON only reaches the reserved CLOSE path, so its objects remain reserved. */
STATIC VOID Cleanup(VOID) {
  if(mMp.ExitEvent){gBS->CloseEvent(mMp.ExitEvent);mMp.ExitEvent=NULL;}
  BOOLEAN Retained=FALSE;
  for(UINTN I=0;I<mMp.Count;++I){CR_MP_CPU *C=&mMp.Cpu[I];
    if(C->Frame)Command(C->Frame,CR_MP_COMMAND_CLOSE);
    if(C->MayBeOn&&WaitOff(C)!=EFI_SUCCESS){Retained=TRUE;mMp.Retained=TRUE;continue;}
    if(C->Frame){
      if(ArmSetMemoryAttributes((UINTN)C->Frame,EFI_PAGE_SIZE,EFI_MEMORY_WB|EFI_MEMORY_XP,0)!=EFI_SUCCESS||
         gBS->FreePages((UINTN)C->Frame,1)!=EFI_SUCCESS)mMp.Retained=TRUE;
      else C->Frame=NULL;
    }
    if(C->Stack){
      if(gBS->FreePages(C->Stack,CR_MP_STACK_PAGES)!=EFI_SUCCESS)mMp.Retained=TRUE;
      else C->Stack=0;
    }
  }
  if(mMp.Entry&&!Retained){
    if(ArmSetMemoryAttributes(mMp.Entry,EFI_PAGE_SIZE,EFI_MEMORY_WB|EFI_MEMORY_XP,0)!=EFI_SUCCESS||
       gBS->FreePages(mMp.Entry,1)!=EFI_SUCCESS)mMp.Retained=TRUE;
    else mMp.Entry=0;
  }
}
EFI_STATUS EFIAPI MpServicesCrDxeEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  (VOID)System;
  if(mMp.Published)return EFI_ALREADY_STARTED;
  if(mMp.Retained)return EFI_NOT_READY;
  ZeroMem(&mMp,sizeof(mMp));
  EFI_MP_SERVICES_PROTOCOL *Other=NULL;
  if(gBS->LocateProtocol(&gEfiMpServiceProtocolGuid,NULL,(VOID **)&Other)==EFI_SUCCESS)return EFI_ALREADY_STARTED;
  VOID *Hob=GetFirstGuidHob(&gArmMpCoreInfoGuid);
  if(!Hob||GET_GUID_HOB_DATA_SIZE(Hob)!=CR_MP_MAX_CPUS*sizeof(ARM_CORE_INFO))return EFI_UNSUPPORTED;
  CONST ARM_CORE_INFO *Core=GET_GUID_HOB_DATA(Hob);
  UINT64 Affinity=GET_MPIDR_AFFINITY_BITS(ArmReadMpidr());mMp.Bsp=CR_MP_MAX_CPUS;
  for(UINTN I=0;I<CR_MP_MAX_CPUS;++I){
    /* HDK8450's topology is source-qualified, not a dense AFF0 assumption. */
    if(Core[I].Mpidr!=(I<<8))return EFI_COMPROMISED_DATA;
    mMp.Cpu[I].Affinity=Core[I].Mpidr;
    if(Affinity==Core[I].Mpidr)mMp.Bsp=I;
  }
  if(mMp.Bsp==CR_MP_MAX_CPUS)return EFI_UNSUPPORTED;
  mMp.Count=CR_MP_MAX_CPUS;
  CR_MP_AP_FRAME Registers;ZeroMem(&Registers,sizeof(Registers));MpCrCaptureRegisters(&Registers);
  if((Registers.El!=1&&Registers.El!=2)||(Registers.Sctlr&1U)==0||!FpQualified(&Registers))return EFI_UNSUPPORTED;
  mMp.El=Registers.El;
  INT32 V=Psci(ARM_SMC_ID_PSCI_VERSION,0,0,0);
  if(V<0||(UINT32)V<0x10000U)return EFI_UNSUPPORTED;
  CONST UINT32 Services[]={ARM_SMC_ID_PSCI_CPU_ON_AARCH64,ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64,ARM_SMC_ID_PSCI_CPU_OFF};
  for(UINTN I=0;I<ARRAY_SIZE(Services);++I)
    if(Psci(ARM_SMC_ID_PSCI_FEATURES,Services[I],0,0)!=0)return EFI_UNSUPPORTED;
  EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
  EFI_STATUS S=gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded);
  if(S!=EFI_SUCCESS||Loaded==NULL)return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:S;
  if(!Loaded->ImageBase||!Loaded->ImageSize||
     (UINTN)Loaded->ImageBase>MAX_UINTN-(UINTN)Loaded->ImageSize)return EFI_COMPROMISED_DATA;
  UINTN Bytes=(UINTN)MpCrTrampolineEnd-(UINTN)MpCrTrampolineStart;
  if(Bytes!=EFI_PAGE_SIZE)return EFI_COMPROMISED_DATA;
  S=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,1,&mMp.Entry);
  if(S!=EFI_SUCCESS)return S;
  CopyMem((VOID *)(UINTN)mMp.Entry,MpCrTrampolineStart,Bytes);
  WriteBackDataCacheRange((VOID *)(UINTN)mMp.Entry,Bytes);
  InvalidateInstructionCacheRange((VOID *)(UINTN)mMp.Entry,Bytes);
  S=ArmSetMemoryAttributes(mMp.Entry,EFI_PAGE_SIZE,EFI_MEMORY_WB,0);
  if(S!=EFI_SUCCESS)goto Fail;
  for(UINTN I=0;I<mMp.Count;++I){
    CR_MP_CPU *C=&mMp.Cpu[I];
    if(I==mMp.Bsp){C->Available=C->Enabled=C->Healthy=TRUE;continue;}
    EFI_PHYSICAL_ADDRESS Page=0;
    S=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,1,&Page);if(S!=EFI_SUCCESS)goto Fail;
    C->Frame=(VOID *)(UINTN)Page;ZeroMem(C->Frame,EFI_PAGE_SIZE);
    WriteBackDataCacheRange(C->Frame,EFI_PAGE_SIZE);
    /* WB matches surrounding RAM, including the AArch64 64KiB cache-class
       rule for Reserved memory. Do not create a 4KiB NC island or fake GCD. */
    S=ArmSetMemoryAttributes(Page,EFI_PAGE_SIZE,EFI_MEMORY_WB|EFI_MEMORY_XP,0);if(S!=EFI_SUCCESS)goto Fail;
    S=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,CR_MP_STACK_PAGES,&C->Stack);if(S!=EFI_SUCCESS)goto Fail;
    S=ArmSetMemoryAttributes(C->Stack,EFI_PAGES_TO_SIZE(CR_MP_STACK_PAGES),EFI_MEMORY_WB|EFI_MEMORY_XP,0);if(S!=EFI_SUCCESS)goto Fail;
    C->Frame->Magic=CR_MP_FRAME_MAGIC;C->Frame->Revision=CR_MP_FRAME_REVISION;
    C->Frame->Index=I;C->Frame->Affinity=C->Affinity;C->Frame->El=mMp.El;
    C->Frame->StackTop=C->Stack+EFI_PAGES_TO_SIZE(CR_MP_STACK_PAGES);
  }
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,&gEfiEventExitBootServicesGuid,&mMp.ExitEvent);
  if(S!=EFI_SUCCESS)goto Fail;
  CR_MP_ENTRY_SLOT *Identity=(VOID *)(UINTN)(mMp.Entry+CR_MP_TABLE_OFFSET);
  for(UINTN I=0;I<mMp.Count;++I){Identity[I].Affinity=mMp.Cpu[I].Affinity;Identity[I].Frame=(UINTN)mMp.Cpu[I].Frame;}
  WriteBackDataCacheRange((VOID *)(UINTN)mMp.Entry,EFI_PAGE_SIZE);
  WriteBackDataCacheRange(Loaded->ImageBase,Loaded->ImageSize);
  WriteBackDataCacheRange(&mMp,sizeof(mMp));
  UINTN Available=0;
  for(UINTN I=0;I<mMp.Count;++I)if(I!=mMp.Bsp){S=Probe(&mMp.Cpu[I]);
    if(S==EFI_SUCCESS){mMp.Cpu[I].Enabled=TRUE;++Available;}
    else DEBUG((DEBUG_ERROR,"MpCr: CPU %lx unavailable: %r\n",mMp.Cpu[I].Affinity,S));}
  if(Available==0){S=EFI_UNSUPPORTED;goto Fail;}
  STATIC CONST EFI_MP_SERVICES_PROTOCOL Protocol={Count,Info,All,ThisAp,Switch,Enable,Who};
  CopyMem(&mMp.Protocol,&Protocol,sizeof(Protocol));
  EFI_HANDLE Handle=NULL;
  EFI_IMAGE_UNLOAD OriginalUnload=Loaded->Unload;
  Loaded->Unload=NoUnload;
  S=gBS->InstallMultipleProtocolInterfaces(&Handle,&gEfiMpServiceProtocolGuid,&mMp.Protocol,NULL);
  if(S!=EFI_SUCCESS){Loaded->Unload=OriginalUnload;goto Fail;}
  mMp.Published=TRUE;
  DEBUG((DEBUG_INIT,"MpCr: %u CPUs, %u enabled APs, BSP=%u EL%u; blocking timeout0\n",mMp.Count,Available,mMp.Bsp,mMp.El));
  return EFI_SUCCESS;
Fail:
  Cleanup();return S;
}
