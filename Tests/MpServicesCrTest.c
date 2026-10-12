/** Actual MP provider, controlled PSCI/UEFI boundaries. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#undef NULL
#include "../Driver/MpServicesCrDxe/MpServicesCrDxe.c"

EFI_GUID gArmMpCoreInfoGuid,gEfiEventExitBootServicesGuid,gEfiLoadedImageProtocolGuid,gEfiMpServiceProtocolGuid;
STATIC EFI_BOOT_SERVICES Bs;
EFI_BOOT_SERVICES *gBS=&Bs;
STATIC struct {EFI_HOB_GUID_TYPE Header;ARM_CORE_INFO Core[8];} Hob;
STATIC EFI_LOADED_IMAGE_PROTOCOL Loaded;
STATIC EFI_TPL Tpl;
STATIC UINT64 CurrentAffinity;
STATIC UINT32 CurrentEl=1;
STATIC UINTN AllocCalls,AttrCalls,FreeCalls,CreateCalls,CloseCalls,OnCalls,PoolCalls;
STATIC UINTN FailAlloc,FailAttr;
STATIC BOOLEAN FailEvent,FailInstall,FailPool,UnsupportedPsci,BadFp;
STATIC BOOLEAN ExecuteAllowed=TRUE,OnDenied,OffDenied;
STATIC BOOLEAN FaultOnRun;
STATIC UINTN ReadyDelay[8],OffDelay[8],Wait[8];
STATIC INT32 Power[8];
STATIC UINTN UserCalls,MaxRunning,Running;
STATIC VOID *Pages[64];
STATIC UINTN PageCount,LivePages;
STATIC BOOLEAN WeakCache;
STATIC CR_MP_AP_FRAME Physical[8];
STATIC UINTN CacheReads,CacheWrites;
__asm__(".section .rodata\n.balign 4096\n.global MpCrTrampolineStart\n"
        "MpCrTrampolineStart:\n.space 4096\n.global MpCrTrampolineEnd\n"
        "MpCrTrampolineEnd:\n.previous\n");
VOID EFIAPI MpCrCaptureRegisters(CR_MP_AP_FRAME *F){
 F->El=CurrentEl;F->Tcr=32;F->Mair=0xff;F->Ttbr0=0x1234000;F->Ttbr1=0;
 F->Sctlr=5;F->FpControl=BadFp?0x400:0x300000;
 if(CurrentEl==2&&!BadFp)F->FpControl=0;
}
VOID EFIAPI MpCrWake(VOID){}
VOID EFIAPI ArmDataSynchronizationBarrier(VOID){}
UINTN EFIAPI ArmReadMpidr(VOID){return CurrentAffinity;}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){
 assert(P&&N);
 if(WeakCache){for(UINTN I=1;I<8;++I)if(P==mMp.Cpu[I].Frame){
  /* A writeback from a stale BSP line must not erase cache-off READY/DONE. */
  if(Power[I]!=1)assert(mMp.Cpu[I].Frame->Stage==Physical[I].Stage);
  memcpy(&Physical[I],P,N<sizeof(Physical[I])?N:sizeof(Physical[I]));++CacheWrites;
 }}
 return P;
}
VOID *EFIAPI InvalidateInstructionCacheRange(VOID *P,UINTN N){assert(P&&N);return P;}
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *P,UINTN N){
 assert(P&&(N==64||N==sizeof(CR_MP_AP_FRAME)));
 if(WeakCache){for(UINTN I=1;I<8;++I)if(P==mMp.Cpu[I].Frame){
  memcpy(P,&Physical[I],N);++CacheReads;
 }}
 return P;
}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN N){(VOID)N;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return Tpl;}
VOID *EFIAPI GetFirstGuidHob(CONST EFI_GUID *G){assert(G==&gArmMpCoreInfoGuid);return &Hob;}
RETURN_STATUS EFIAPI ArmSetMemoryAttributes(EFI_PHYSICAL_ADDRESS B,UINT64 N,UINT64 A,UINT64 M){
 assert(B&&N&&M==0);assert(A==EFI_MEMORY_WB||A==(EFI_MEMORY_WB|EFI_MEMORY_XP));
 ++AttrCalls;return AttrCalls==FailAttr?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
STATIC VOID Tick(VOID){
 for(UINTN I=1;I<8;++I){CR_MP_AP_FRAME *F=mMp.Cpu[I].Frame;if(!F||Power[I]==1)continue;
  if(WeakCache)F=&Physical[I];
  if(Power[I]==2){if(Wait[I]){--Wait[I];continue;}Power[I]=0;F->Stage=CR_MP_STAGE_READY;}
  if(F->Command==CR_MP_COMMAND_RUN&&F->Stage==CR_MP_STAGE_READY&&ExecuteAllowed){
   F->Stage=CR_MP_STAGE_RUNNING;++Running;if(Running>MaxRunning)MaxRunning=Running;
   if(FaultOnRun){F->Fault=0x8000000096000004ULL;F->Stage=CR_MP_STAGE_FAULT;}
   else {UINT64 Old=CurrentAffinity;CurrentAffinity=I<<8;F->Dispatch(mMp.Cpu[I].Frame);CurrentAffinity=Old;F->Stage=CR_MP_STAGE_DONE;}
   --Running;Wait[I]=OffDelay[I];
  }
  if(F->Command==CR_MP_COMMAND_CLOSE||F->Stage==CR_MP_STAGE_DONE||F->Stage==CR_MP_STAGE_FAULT){
   if(OffDenied){F->OffResult=(UINT64)-3;F->Stage=CR_MP_STAGE_OFF_FAILED;continue;}
   if(Wait[I]){--Wait[I];continue;}Power[I]=1;
  }
 }
}
VOID EFIAPI ArmCallSmc(ARM_SMC_ARGS *A){
 /* Required unused SMCCC input registers must never expose stack garbage. */
 assert(!A->Arg4&&!A->Arg5&&!A->Arg6&&!A->Arg7&&!A->Arg8&&!A->Arg9&&!A->Arg10&&!A->Arg11&&!A->Arg12&&!A->Arg13&&!A->Arg14&&!A->Arg15&&!A->Arg16&&!A->Arg17);
 UINT32 Id=A->Arg0;
 if(Id==ARM_SMC_ID_PSCI_VERSION){assert(!A->Arg1&&!A->Arg2&&!A->Arg3);A->Arg0=0x10000;return;}
 if(Id==ARM_SMC_ID_PSCI_FEATURES){assert(!A->Arg2&&!A->Arg3);A->Arg0=UnsupportedPsci?(UINT64)-1:0;return;}
 UINTN I=A->Arg1>>8;assert(I<8&&A->Arg1==(I<<8));
 if(Id==ARM_SMC_ID_PSCI_AFFINITY_INFO_AARCH64){assert(!A->Arg2&&!A->Arg3);A->Arg0=Power[I];return;}
 assert(Id==ARM_SMC_ID_PSCI_CPU_ON_AARCH64&&I!=0);++OnCalls;
 assert(Power[I]==1&&A->Arg2==mMp.Entry&&A->Arg3==(UINTN)mMp.Cpu[I].Frame);
 if(WeakCache)assert(Physical[I].Magic==CR_MP_FRAME_MAGIC&&Physical[I].StackTop&&Physical[I].Command==CR_MP_COMMAND_WAIT);
 CR_MP_ENTRY_SLOT *S=(VOID *)(UINTN)(mMp.Entry+0x600);assert(S[I].Affinity==(I<<8)&&S[I].Frame==A->Arg3);
 if(OnDenied){A->Arg0=(UINT64)-3;return;}
 Power[I]=2;Wait[I]=ReadyDelay[I];A->Arg0=0;
}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T){EFI_TPL O=Tpl;assert(T>=O);Tpl=T;return O;}
STATIC VOID EFIAPI Restore(EFI_TPL T){assert(T<=Tpl);Tpl=T;}
STATIC EFI_STATUS EFIAPI Stall(UINTN N){assert(N==CR_MP_POLL_US);Tick();return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Allocate(EFI_ALLOCATE_TYPE T,EFI_MEMORY_TYPE M,UINTN N,EFI_PHYSICAL_ADDRESS *Out){
 assert(T==AllocateAnyPages&&M==EfiReservedMemoryType&&N>0);++AllocCalls;
 if(AllocCalls==FailAlloc)return EFI_OUT_OF_RESOURCES;
 VOID *P=aligned_alloc(4096,N*4096);assert(P);memset(P,0,N*4096);
 Pages[PageCount++]=P;++LivePages;*Out=(UINTN)P;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Free(EFI_PHYSICAL_ADDRESS B,UINTN N){
 assert(N==1||N==4);
 for(UINTN I=1;I<8;++I)if(mMp.Cpu[I].MayBeOn){
  assert(B!=mMp.Entry&&B!=(UINTN)mMp.Cpu[I].Frame&&B!=mMp.Cpu[I].Stack);
 }
 for(UINTN I=0;I<PageCount;++I)if(Pages[I]==(VOID *)(UINTN)B){free(Pages[I]);Pages[I]=NULL;++FreeCalls;--LivePages;return EFI_SUCCESS;}
 assert(0);return EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS EFIAPI AllocatePoolMock(EFI_MEMORY_TYPE T,UINTN N,VOID **Out){
 assert(T==EfiBootServicesData);++PoolCalls;if(FailPool)return EFI_OUT_OF_RESOURCES;
 *Out=malloc(N);assert(*Out);return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FreePoolMock(VOID *P){assert(P);free(P);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Notify,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){
 assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&Notify==Exit&&!C&&G==&gEfiEventExitBootServicesGuid);
 ++CreateCalls;if(FailEvent)return EFI_OUT_OF_RESOURCES;*Out=(VOID *)0x9870;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E==(VOID *)0x9870);++CloseCalls;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){assert(G==&gEfiMpServiceProtocolGuid&&!R);(VOID)Out;return EFI_NOT_FOUND;}
STATIC EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **Out){assert(H==(VOID *)0x42&&G==&gEfiLoadedImageProtocolGuid);*Out=&Loaded;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
 assert(Loaded.Unload==NoUnload); /* publication callbacks already see pin */
 va_list A;va_start(A,H);assert(va_arg(A,EFI_GUID *)==&gEfiMpServiceProtocolGuid);
 assert(va_arg(A,VOID *)==&mMp.Protocol);assert(!va_arg(A,VOID *));va_end(A);
 if(FailInstall)return EFI_DEVICE_ERROR;*H=(VOID *)0x8000;return EFI_SUCCESS;
}
STATIC VOID Setup(VOID){
 Bs.RaiseTPL=Raise;Bs.RestoreTPL=Restore;Bs.Stall=Stall;
 Bs.AllocatePages=Allocate;Bs.FreePages=Free;Bs.AllocatePool=AllocatePoolMock;Bs.FreePool=FreePoolMock;
 Bs.CreateEventEx=Create;Bs.CloseEvent=Close;Bs.LocateProtocol=Locate;Bs.HandleProtocol=Handle;Bs.InstallMultipleProtocolInterfaces=Install;
 Hob.Header.Header.HobLength=sizeof(Hob);for(UINTN I=0;I<8;++I){Hob.Core[I].Mpidr=I<<8;Power[I]=1;}
 Loaded.ImageBase=&Loaded;Loaded.ImageSize=sizeof(Loaded);Tpl=TPL_APPLICATION;
}
STATIC VOID EFIAPI Work(VOID *Arg){
 UINTN I=99;assert(Who(&mMp.Protocol,&I)==EFI_SUCCESS&&I>0&&I<8);
 assert(ThisAp(&mMp.Protocol,Work,I,NULL,0,Arg,NULL)==EFI_DEVICE_ERROR);
 assert(mMp.Cpu[I].Frame->El==CurrentEl&&mMp.Cpu[I].Frame->Ttbr0==0x1234000);
 ++((UINTN *)Arg)[I];++UserCalls;
}
STATIC VOID Normal(VOID){
 assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_SUCCESS);
 assert(OnCalls==7&&LivePages==15&&CloseCalls==0&&Loaded.Unload((VOID *)0x42)==EFI_ACCESS_DENIED);
 UINTN T=0,E=0;assert(Count(&mMp.Protocol,&T,&E)==EFI_SUCCESS&&T==8&&E==8);
 EFI_PROCESSOR_INFORMATION P;memset(&P,0xa5,sizeof(P));
 assert(Info(&mMp.Protocol,7|CPU_V2_EXTENDED_TOPOLOGY,&P)==EFI_SUCCESS);
 assert(P.ProcessorId==0x700&&P.Location.Core==7&&P.ExtendedInformation.Location2.Core==7);
 assert(Enable(&mMp.Protocol,(UINTN)-1,FALSE,NULL)==EFI_NOT_FOUND);
 UINTN Result=55;CurrentAffinity=0x800;assert(Who(&mMp.Protocol,&Result)==EFI_NOT_FOUND&&Result==55);CurrentAffinity=0;
 UINTN Before=OnCalls;BOOLEAN Finished=TRUE;UINTN *Failed=(VOID *)0x456;
 UINTN A[8]={0};
 assert(ThisAp(&mMp.Protocol,Work,1,NULL,1,A,&Finished)==EFI_UNSUPPORTED&&Finished);
 assert(All(&mMp.Protocol,Work,FALSE,(VOID *)1,0,A,&Failed)==EFI_UNSUPPORTED&&Failed==(VOID *)0x456);
 assert(OnCalls==Before&&!UserCalls);
 FailPool=TRUE;assert(All(&mMp.Protocol,Work,FALSE,NULL,0,A,&Failed)==EFI_OUT_OF_RESOURCES&&Failed==(VOID *)0x456&&OnCalls==Before);FailPool=FALSE;
 assert(All(&mMp.Protocol,Work,FALSE,NULL,0,A,&Failed)==EFI_SUCCESS&&!Failed&&UserCalls==7);
 assert(All(&mMp.Protocol,Work,TRUE,NULL,0,A,NULL)==EFI_SUCCESS&&UserCalls==14);
 assert(ThisAp(&mMp.Protocol,Work,2,NULL,0,A,&Finished)==EFI_SUCCESS&&Finished&&A[2]==3);
 assert(Enable(&mMp.Protocol,3,FALSE,NULL)==EFI_SUCCESS);
 assert(ThisAp(&mMp.Protocol,Work,3,NULL,0,A,NULL)==EFI_INVALID_PARAMETER);
 assert(Enable(&mMp.Protocol,3,TRUE,NULL)==EFI_SUCCESS);
 Tpl=TPL_NOTIFY;Exit(NULL,NULL);Tpl=TPL_APPLICATION;Before=OnCalls;
 assert(Count(&mMp.Protocol,&T,&E)==EFI_UNSUPPORTED);
 assert(ThisAp(&mMp.Protocol,Work,1,NULL,0,A,NULL)==EFI_UNSUPPORTED&&OnCalls==Before);
 for(UINTN I=1;I<8;++I)assert(Power[I]==1&&!mMp.Cpu[I].MayBeOn);
}
int main(int argc,char **argv){
 assert(argc>=2);Setup();CONST char *Mode=argv[1];
 if(!strcmp(Mode,"normal")){Normal();}
 else if(!strcmp(Mode,"weak-cache")){WeakCache=TRUE;Normal();assert(CacheReads>=OnCalls&&CacheWrites>=OnCalls*2);}
 else if(!strcmp(Mode,"el2")){CurrentEl=2;Normal();}
 else if(!strcmp(Mode,"alloc")||!strcmp(Mode,"attr")){
  assert(argc==3);if(!strcmp(Mode,"alloc"))FailAlloc=strtoul(argv[2],NULL,0);else FailAttr=strtoul(argv[2],NULL,0);
  assert(EFI_ERROR(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)));assert(!LivePages&&!OnCalls&&!mMp.ExitEvent);
 }
 else if(!strcmp(Mode,"event")||!strcmp(Mode,"install")){
  FailEvent=!strcmp(Mode,"event");FailInstall=!strcmp(Mode,"install");
  assert(EFI_ERROR(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)));assert(!LivePages&&!mMp.ExitEvent);
  assert(CloseCalls==(FailInstall?1U:0U));
 }
 else if(!strcmp(Mode,"psci")||!strcmp(Mode,"fp")||!strcmp(Mode,"topology")){
  UnsupportedPsci=!strcmp(Mode,"psci");BadFp=!strcmp(Mode,"fp");if(!strcmp(Mode,"topology"))Hob.Core[7].Mpidr=6<<8;
  assert(EFI_ERROR(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)));assert(!AllocCalls&&!OnCalls);
 }
 else if(!strcmp(Mode,"foreign")){
  Power[4]=0;assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_SUCCESS);
  assert(OnCalls==6&&!mMp.Cpu[4].Available&&Power[4]==0);
 }
 else if(!strcmp(Mode,"late")){
  ReadyDelay[1]=200000;assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_SUCCESS);
  assert(mMp.Cpu[1].MayBeOn&&!mMp.Cpu[1].Available&&mMp.Cpu[1].Frame->Command==CR_MP_COMMAND_CLOSE);
  assert(!mMp.Cpu[1].Frame->Dispatch&&!UserCalls);
  ReadyDelay[1]=0;Wait[1]=0;Tick();assert(Power[1]==1&&!UserCalls);
  assert(WaitOff(&mMp.Cpu[1])==EFI_SUCCESS);
 }
 else if(!strcmp(Mode,"off")||!strcmp(Mode,"on")){
  assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_SUCCESS);
  UINTN A[8]={0};OffDenied=!strcmp(Mode,"off");OnDenied=!strcmp(Mode,"on");BOOLEAN Finished=TRUE;
  EFI_STATUS S=ThisAp(&mMp.Protocol,Work,1,NULL,0,A,&Finished);
  assert(S==(OffDenied?EFI_TIMEOUT:EFI_DEVICE_ERROR)&&!Finished&&!mMp.Cpu[1].Enabled);
  assert(UserCalls==(OffDenied?1U:0U));if(OffDenied)assert(mMp.Cpu[1].MayBeOn&&LivePages==15&&mMp.Cpu[1].Frame->OffResult==(UINT64)-3);
 }
 else if(!strcmp(Mode,"fault-cache")){
  WeakCache=TRUE;assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_SUCCESS);
  FaultOnRun=TRUE;UINTN A[8]={0};BOOLEAN Finished=TRUE;
  assert(ThisAp(&mMp.Protocol,Work,1,NULL,0,A,&Finished)==EFI_DEVICE_ERROR&&!Finished);
  assert(!UserCalls&&!mMp.Cpu[1].Enabled&&!mMp.Cpu[1].MayBeOn&&mMp.Cpu[1].Frame->Fault==0x8000000096000004ULL);
 }
 else if(!strcmp(Mode,"retained-init")){
  OffDenied=TRUE;assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_UNSUPPORTED);
  assert(mMp.Retained&&LivePages==15&&CloseCalls==1&&!mMp.ExitEvent&&!UserCalls);
  UINTN Before=OnCalls;assert(MpServicesCrDxeEntryPoint((VOID *)0x42,NULL)==EFI_NOT_READY&&OnCalls==Before);
 }else assert(0);
 printf("MP actual provider mode %s PASS: accepted PSCI lifetime, no unsupported dispatch, exact OFF and retained failures\n",Mode);
 return 0;
}
