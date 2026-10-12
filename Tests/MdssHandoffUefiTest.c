/* Actual coordinator, UEFI storage adapter and portable wire core. MIT */
#include <stdlib.h>
#include <stdarg.h>
#undef NULL
#define main ExistingWireMain
#include "MdssHandoffTest.c"
#undef main
#ifndef CR_MDSS_UEFI_SOURCE
#define CR_MDSS_UEFI_SOURCE "../Library/MdssHandoffUefiLib/MdssHandoffUefi.c"
#endif
#ifndef CR_MDSS_DRIVER_SOURCE
#define CR_MDSS_DRIVER_SOURCE "../Driver/MdssHandoffCrDxe/MdssHandoffCrDxe.c"
#endif
#include CR_MDSS_UEFI_SOURCE
#include CR_MDSS_DRIVER_SOURCE

EFI_GUID gEfiSmmuCrProtocolGuid, gEfiMdssHandoffProtocolGuid, gEfiAcpiTableProtocolGuid;
EFI_GUID gEfiEndOfDxeEventGroupGuid, gEfiEventReadyToBootGuid, gEfiEventExitBootServicesGuid;
EFI_BOOT_SERVICES *gBS;
EFI_DXE_SERVICES *gDS;
typedef struct { EFI_EVENT_NOTIFY Notify; VOID *Context; EFI_TPL Tpl; } TEST_EVENT;
STATIC TEST_EVENT *mEvents[8];
STATIC UINTN mEventCount, mClosedEvents, mCreateCalls, mFailCreate, mPages, mPools, mAcpiTables;
STATIC UINTN mPrepareCalls, mCompleteCalls, mQueries, mCacheCalls, mMonotonic, mCloseCalls, mFailClose;
STATIC UINTN mFreezeOrder[8], mFreezeCount;
STATIC UINT32 mOwners[4] = {1,2,4,16};
STATIC VOID *mPageAllocation, *mPoolAllocation;
STATIC CR_MDSS_NATIVE_LOCATOR mLocator;
STATIC EFI_SMMU_CR_EXIT_PREPARE mBefore;
STATIC EFI_SMMU_CR_EXIT_OBSERVER mAfter;
STATIC VOID *mBeforeContext, *mAfterContext;
STATIC EFI_TPL mTpl;
STATIC BOOLEAN mEbs, mSmmuDone, mNoMap, mRecordOverlap, mTableOverlap, mForeignMap, mBadCurrentCache;
STATIC BOOLEAN mInterruptPrepare, mInterfaceInstalled, mBadMap, mDuplicateDescriptor;
STATIC BOOLEAN mPixelTail, mShortGcdTail, mShortReservedTail;
STATIC UINT32 mBadGeometry, mGeometryRoute;
STATIC EFI_STATUS mGcdStatus, mMapStatus, mAcpiInstallStatus, mAcpiUninstallStatus;
STATIC EFI_STATUS mPageStatus, mPoolStatus, mFreePageStatus, mFreePoolStatus, mCounterStatus;
STATIC EFI_STATUS mPrepareRegisterStatus, mCompleteRegisterStatus, mUnregisterStatus, mInterfaceStatus, mNotifyStatus;
STATIC EFI_STATUS mFreezeFailure, mSnapshotFailure, mQueryStatus;
STATIC EFI_ACPI_TABLE_PROTOCOL mAcpiMock;
STATIC EFI_SMMU_CR_PROTOCOL mSmmuMock;

VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
UINT8 EFIAPI CalculateCheckSum8(CONST UINT8 *B,UINTN N){UINT8 V=0;for(UINTN I=0;I<N;++I)V+=B[I];return (UINT8)(0-V);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *B,UINTN N){
  assert((UINTN)B>=(UINTN)mPageAllocation && N<=8192 && (UINTN)B-(UINTN)mPageAllocation<=8192-N);
  if(mEbs){UINT32 Sequence=((CR_MDSS_NATIVE_HANDOFF*)mPageAllocation)->Sequence;assert(Sequence==1||Sequence==2);}
  ++mCacheCalls;return B;
}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
STATIC EFI_TPL EFIAPI MockRaise(EFI_TPL T){EFI_TPL Old=mTpl;assert(T>=Old);mTpl=T;return Old;}
STATIC VOID EFIAPI MockRestore(EFI_TPL T){assert(T<=mTpl);mTpl=T;}
STATIC EFI_STATUS EFIAPI MockCount(UINT64 *N){assert(!mEbs);*N=++mMonotonic;return mCounterStatus;}
STATIC EFI_STATUS EFIAPI MockPages(EFI_ALLOCATE_TYPE A,EFI_MEMORY_TYPE T,UINTN N,EFI_PHYSICAL_ADDRESS *P){
  assert(!mEbs && !mPages && A==AllocateAnyPages && T==EfiReservedMemoryType && N==2);
  if(mPageStatus!=EFI_SUCCESS)return mPageStatus;
  mPageAllocation=aligned_alloc(4096,8192);assert(mPageAllocation);memset(mPageAllocation,0xa5,8192);
  *P=(UINTN)mPageAllocation;++mPages;
  if(mInterruptPrepare){mInterruptPrepare=FALSE;EFI_TPL Old=MockRaise(TPL_NOTIFY);BeforeRevoke(&mHandoff);MockRestore(Old);mEbs=TRUE;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockFreePages(EFI_PHYSICAL_ADDRESS P,UINTN N){
  assert(!mEbs && P==(UINTN)mPageAllocation && N==2 && mPages && !mAcpiTables);
  if(mFreePageStatus!=EFI_SUCCESS)return mFreePageStatus;
  free(mPageAllocation);mPageAllocation=NULL;--mPages;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockPool(EFI_MEMORY_TYPE T,UINTN N,VOID **P){
  assert(!mEbs && T==EfiBootServicesData && N>=65536 && !mPools);
  if(mPoolStatus!=EFI_SUCCESS)return mPoolStatus;
  mPoolAllocation=malloc(N);assert(mPoolAllocation);*P=mPoolAllocation;++mPools;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockFreePool(VOID *P){
  assert(!mEbs && P==mPoolAllocation && mPools && !mAcpiTables);
  if(mFreePoolStatus!=EFI_SUCCESS)return mFreePoolStatus;
  free(P);mPoolAllocation=NULL;--mPools;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockGcd(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  memset(D,0,sizeof(*D));D->BaseAddress=A;D->Length=0x4000000;
  if(mShortGcdTail&&A==(mGeometryRoute?0xfd000000:0xfc800000))D->Length=8294404;
  D->GcdMemoryType=EfiGcdMemoryTypeReserved;D->Capabilities=EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB;
  D->Attributes=mBadCurrentCache?EFI_MEMORY_UC:EFI_MEMORY_WB;return mGcdStatus;
}
STATIC VOID Descriptor(EFI_MEMORY_DESCRIPTOR *D,UINT64 A,UINT64 N){
  D->Type=EfiReservedMemoryType;D->PhysicalStart=A;D->NumberOfPages=N/4096;
  /* The EFI map exposes cache capabilities; GCD above proves active cache. */
  D->Attribute=EFI_MEMORY_UC|EFI_MEMORY_WC|EFI_MEMORY_WT|EFI_MEMORY_WB;
}
STATIC EFI_STATUS EFIAPI MockMap(UINTN *N,EFI_MEMORY_DESCRIPTOR *D,UINTN *K,UINTN *Stride,UINT32 *V){
  *K=123;*Stride=sizeof(*D);*V=EFI_MEMORY_DESCRIPTOR_VERSION;
  if(!D){*N=5*sizeof(*D);return EFI_BUFFER_TOO_SMALL;}
  assert(*N>=6*sizeof(*D));memset(D,0,6*sizeof(*D));
  Descriptor(D,(UINTN)mPageAllocation,mRecordOverlap?0x800000:mTableOverlap?0x88000:8192);
  UINT64 FrameBytes=mPixelTail&&!mShortReservedTail?0x7ea000:0x7e9000;
  Descriptor(D+1,0xfc800000,FrameBytes);Descriptor(D+2,0xfd000000,FrameBytes);Descriptor(D+3,0xfe000000,0x88000);
  *N=4*sizeof(*D);
  if(mDuplicateDescriptor){Descriptor(D+4,(UINTN)mPageAllocation+4096,4096);*N=5*sizeof(*D);}
  if(mBadMap)D[0].Type=EfiBootServicesData;
  return mMapStatus;
}
STATIC EFI_STATUS EFIAPI MockAcpiInstall(EFI_ACPI_TABLE_PROTOCOL *T,VOID *B,UINTN N,UINTN *Key){
  assert(!mEbs && T==&mAcpiMock && N==64 && !mAcpiTables);
  if(mAcpiInstallStatus!=EFI_SUCCESS)return mAcpiInstallStatus;
  memcpy(&mLocator,B,N);assert(CalculateCheckSum8(B,N)==0);*Key=77;++mAcpiTables;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockAcpiUninstall(EFI_ACPI_TABLE_PROTOCOL *T,UINTN Key){
  assert(!mEbs && T==&mAcpiMock && Key==77 && mAcpiTables==1);
  if(mAcpiUninstallStatus!=EFI_SUCCESS)return mAcpiUninstallStatus;
  --mAcpiTables;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockLocate(EFI_GUID *G,VOID *Registration,VOID **P){
  assert(!Registration);
  if(G==&gEfiSmmuCrProtocolGuid)*P=&mSmmuMock;
  else{assert(!mEbs && G==&gEfiAcpiTableProtocolGuid);*P=&mAcpiMock;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockSetBefore(EFI_SMMU_CR_PROTOCOL *T,VOID *Owner,EFI_SMMU_CR_EXIT_PREPARE C,VOID *Context){
  assert(!mEbs && T==&mSmmuMock && Owner==&mHandoff);++mPrepareCalls;
  EFI_STATUS S=C?mPrepareRegisterStatus:mUnregisterStatus;if(S!=EFI_SUCCESS)return S;
  mBefore=C;mBeforeContext=Context;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockSetAfter(EFI_SMMU_CR_PROTOCOL *T,VOID *Owner,EFI_SMMU_CR_EXIT_OBSERVER C,VOID *Context){
  assert(!mEbs && T==&mSmmuMock && Owner==&mHandoff);++mCompleteCalls;
  EFI_STATUS S=C?mCompleteRegisterStatus:mUnregisterStatus;if(S!=EFI_SUCCESS)return S;
  mAfter=C;mAfterContext=Context;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockQuery(EFI_SMMU_CR_PROTOCOL *T,VOID *Map,EFI_SMMU_CR_MAPPING_EVIDENCE *E){
  assert(mEbs && mSmmuDone && T==&mSmmuMock);++mQueries;
  if(mQueryStatus!=EFI_SUCCESS)return mQueryStatus;
  UINT32 Index=Map==(VOID*)&mOwners[0]?0:1;MappingSnapshot();CR_MDSS_NATIVE_MAPPING M=P.Mappings[Index];
  memset(E,0,sizeof(*E));E->Size=sizeof(*E);E->Revision=1;E->MappingId=M.MappingId;E->StreamCount=M.StreamCount;
  E->HostAddress=Index?0xfd000000:mRecordOverlap?(UINTN)mPageAllocation:0xfc800000;
  E->NumberOfBytes=8294400+(mPixelTail?4:0);E->DeviceAddress=Index?0x10800000:0x10000000;
  E->PhysicalBase=E->HostAddress;E->IovaBase=E->DeviceAddress;E->PageBytes=mPixelTail?0x7ea000:0x7e9000;
  memcpy(E->Streams,M.Streams,sizeof(M.Streams));
  for(UINT32 I=0;I<4;++I)E->Streams[I].Bytes=E->PageBytes;
  if(mRecordOverlap&&!Index)for(UINT32 I=0;I<4;++I)E->Streams[I].Physical=E->HostAddress;
  if(mTableOverlap)for(UINT32 I=0;I<4;++I){E->Streams[I].Tables=(UINTN)mPageAllocation+I*0x22000;E->Streams[I].Ttbr0=E->Streams[I].Tables|((UINT64)E->Streams[I].Asid<<48);}
  if(mForeignMap)++E->HostAddress;
  if(Index==mGeometryRoute){
    if(mBadGeometry==1)E->PhysicalBase+=4096;
    if(mBadGeometry==2)E->IovaBase+=4096;
    if(mBadGeometry==3)E->PageBytes-=4096;
    if(mBadGeometry==4)E->PageBytes+=4096;
    if(mBadGeometry==5)E->PageBytes=E->NumberOfBytes;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Context,EFI_EVENT *Event){
  assert(!mEbs && Type==EVT_NOTIFY_SIGNAL && (Tpl==TPL_CALLBACK||Tpl==TPL_NOTIFY));
  if(++mCreateCalls==mFailCreate)return EFI_OUT_OF_RESOURCES;
  TEST_EVENT *E=malloc(sizeof(*E));assert(E && mEventCount<8);*E=(TEST_EVENT){Notify,Context,Tpl};
  mEvents[mEventCount++]=E;*Event=E;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MockCreateEx(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Context,CONST EFI_GUID *Group,EFI_EVENT *Event){
  assert(Group==&gEfiEndOfDxeEventGroupGuid||Group==&gEfiEventReadyToBootGuid||Group==&gEfiEventExitBootServicesGuid);
  return MockCreate(Type,Tpl,Notify,(VOID*)Context,Event);
}
STATIC EFI_STATUS EFIAPI MockClose(EFI_EVENT Event){
  assert(!mEbs);if(++mCloseCalls==mFailClose)return EFI_DEVICE_ERROR;
  for(UINTN I=0;I<mEventCount;++I)if(mEvents[I]==Event){free(Event);mEvents[I]=NULL;++mClosedEvents;return EFI_SUCCESS;}
  abort();
}
STATIC EFI_STATUS EFIAPI MockNotify(EFI_GUID *G,EFI_EVENT Event,VOID **Registration){
  assert(G==&gEfiAcpiTableProtocolGuid && Event==mAcpiEvent);*Registration=Event;return mNotifyStatus;
}
STATIC EFI_STATUS EFIAPI MockInstall(EFI_HANDLE *Handle,...){
  assert(!mEbs);if(mInterfaceStatus!=EFI_SUCCESS)return mInterfaceStatus;
  va_list Args;va_start(Args,Handle);assert(va_arg(Args,EFI_GUID*)==&gEfiMdssHandoffProtocolGuid);
  assert(va_arg(Args,VOID*)==&mProtocol && va_arg(Args,VOID*)==NULL);va_end(Args);
  *Handle=&mProtocol;mInterfaceInstalled=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FreezeOwner(VOID *Context){
  assert(mTpl==TPL_NOTIFY && mEbs);UINT32 Owner=*(UINT32*)Context;mFreezeOrder[mFreezeCount++]=Owner;
  return Owner==CR_MDSS_NATIVE_BRIDGE?mFreezeFailure:EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SnapshotOwner(VOID *Context,CR_MDSS_HANDOFF_PARTICIPANT *Output,VOID **Map){
  assert(mTpl==TPL_NOTIFY && mEbs && mSmmuDone);if(mSnapshotFailure!=EFI_SUCCESS)return mSnapshotFailure;
  UINT32 Owner=*(UINT32*)Context;
  if(Owner==1||Owner==2){RouteSnapshot(Owner,Owner==1?1:2);
    if(mPixelTail)P.Route.FrameBufferBytes+=4;
    if(mNoMap&&Owner==2)P.Route.DeviceAddress=0;
    if(mRecordOverlap&&Owner==1)P.Route.FrameBuffer=(UINTN)mPageAllocation;
    *Map=mNoMap&&Owner==2?NULL:Context;
  }else if(Owner==4){BridgeSnapshot();*Map=NULL;}else{PhySnapshot();*Map=NULL;}
  *Output=P;return EFI_SUCCESS;
}
/* Simulated reboot only: production never unfreezes or unloads a published image. */
STATIC VOID NewImage(VOID){
  free(mPageAllocation);free(mPoolAllocation);for(UINTN I=0;I<mEventCount;++I)free(mEvents[I]);
  memset(&mHandoff,0,sizeof(mHandoff));memset(&mLocator,0,sizeof(mLocator));memset(mEvents,0,sizeof(mEvents));
  mPageAllocation=mPoolAllocation=NULL;mEventCount=mClosedEvents=mCreateCalls=mFailCreate=mPages=mPools=mAcpiTables=0;
  mPrepareCalls=mCompleteCalls=mQueries=mCacheCalls=mMonotonic=mCloseCalls=mFailClose=mFreezeCount=0;
  mGcdStatus=mMapStatus=mAcpiInstallStatus=mAcpiUninstallStatus=mPageStatus=mPoolStatus=mFreePageStatus=mFreePoolStatus=mCounterStatus=EFI_SUCCESS;
  mPrepareRegisterStatus=mCompleteRegisterStatus=mUnregisterStatus=mInterfaceStatus=mNotifyStatus=mFreezeFailure=mSnapshotFailure=mQueryStatus=EFI_SUCCESS;
  mEbs=mSmmuDone=mNoMap=mRecordOverlap=mTableOverlap=mForeignMap=mBadCurrentCache=mInterruptPrepare=mInterfaceInstalled=mBadMap=mDuplicateDescriptor=FALSE;
  mPixelTail=mShortGcdTail=mShortReservedTail=FALSE;mBadGeometry=mGeometryRoute=0;
  mActive=mPreparing=mPrepareObserver=mCompleteObserver=FALSE;mBefore=NULL;mAfter=NULL;mBeforeContext=mAfterContext=NULL;
  mHandle=NULL;mAcpiEvent=mEndEvent=mReadyEvent=mExitEvent=NULL;mAcpiRegistration=NULL;mTpl=TPL_APPLICATION;
  mAcpiMock=(EFI_ACPI_TABLE_PROTOCOL){MockAcpiInstall,MockAcpiUninstall};
  mSmmuMock=(EFI_SMMU_CR_PROTOCOL){.Revision=EFI_SMMU_CR_EXIT_REVOCATION_REVISION,.SetExitObserver=MockSetAfter,
    .SetExitPrepareObserver=MockSetBefore,.QueryExitMapping=MockQuery,.QueryExitRevokedMapping=MockQuery};
  STATIC EFI_BOOT_SERVICES B={.RaiseTPL=MockRaise,.RestoreTPL=MockRestore,.LocateProtocol=MockLocate,
    .GetNextMonotonicCount=MockCount,.AllocatePages=MockPages,.FreePages=MockFreePages,.AllocatePool=MockPool,
    .FreePool=MockFreePool,.GetMemoryMap=MockMap,.CreateEvent=MockCreate,.CreateEventEx=MockCreateEx,
    .CloseEvent=MockClose,.RegisterProtocolNotify=MockNotify,.InstallMultipleProtocolInterfaces=MockInstall};
  STATIC EFI_DXE_SERVICES D={.GetMemorySpaceDescriptor=MockGcd};gBS=&B;gDS=&D;
}
STATIC VOID RegisterOwners(VOID){
  for(UINTN I=0;I<4;++I)assert(SetParticipant(&mProtocol,mOwners[I],&mOwners[I],FreezeOwner,SnapshotOwner)==EFI_SUCCESS);
}
STATIC VOID Start(VOID){assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && mInterfaceInstalled);RegisterOwners();assert(mHandoff.Installed);}
STATIC VOID Exit(BOOLEAN ExitFirst){
  mEbs=TRUE;EFI_TPL Old=MockRaise(TPL_NOTIFY);
  if(ExitFirst){OnExit(NULL,NULL);assert(!mHandoff.Published && mFreezeCount==4);}
  assert(mBefore && mAfter);mBefore(mBeforeContext);mSmmuDone=TRUE;mAfter(mAfterContext,EFI_SUCCESS);
  if(!ExitFirst)OnExit(NULL,NULL);
  MockRestore(Old);
}
STATIC VOID Unpublished(VOID){assert(!mHandoff.Published && ((CR_MDSS_NATIVE_HANDOFF*)mPageAllocation)->Sequence==0);}
int main(int Argc,char **Argv){
  assert(Argc>=2);CONST char *Mode=Argv[1];NewImage();
  if(!strcmp(Mode,"publication")){
    Start();Exit(TRUE);assert(mHandoff.Published && CrMdssHandoffCanClaim(mPageAllocation)==CR_SUCCESS);
    assert(mFreezeOrder[0]==2 && mFreezeOrder[1]==1 && mFreezeOrder[2]==4 && mFreezeOrder[3]==16);
    assert(mLocator.RecordLength==sizeof(CR_MDSS_NATIVE_HANDOFF) && mLocator.RecordPhysical==(UINTN)mPageAllocation);
    assert(CalculateCheckSum8((UINT8*)&mLocator,sizeof(mLocator))==0);
    UINTN Cache=mCacheCalls,Queries=mQueries;OnExit(NULL,NULL);assert(mCacheCalls==Cache && mQueries==Queries);
    assert(CrMdssUefiRelease(&mHandoff)==EFI_ACCESS_DENIED);
    if(Argc==4){FILE *F=fopen(Argv[2],"wb");assert(F && fwrite(mPageAllocation,1,sizeof(CR_MDSS_NATIVE_HANDOFF),F)==sizeof(CR_MDSS_NATIVE_HANDOFF));fclose(F);
      F=fopen(Argv[3],"wb");assert(F && fwrite(&mLocator,1,sizeof(mLocator),F)==sizeof(mLocator));fclose(F);}
    NewImage();Start();Exit(FALSE);assert(mHandoff.Published);
    NewImage();mNoMap=TRUE;Start();Exit(FALSE);assert(mHandoff.Published && mHandoff.Local.Routes[1].MappingState==CR_MDSS_NATIVE_MAPPING_NONE);
  }else if(!strcmp(Mode,"warnings")){
    mGcdStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiPrepare(&mHandoff)==EFI_NOT_READY && !mHandoff.Installed);
    NewImage();mCounterStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiPrepare(&mHandoff)==EFI_NOT_READY && !mPages);
    NewImage();mAcpiInstallStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiPrepare(&mHandoff)==EFI_NOT_READY && !mHandoff.Installed);
    NewImage();Start();mMapStatus=EFI_WARN_WRITE_FAILURE;Exit(FALSE);Unpublished();
    NewImage();Start();mAcpiUninstallStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiRelease(&mHandoff)==EFI_NOT_READY && mHandoff.Installed && mPages && mPools);
    NewImage();Start();mFreePoolStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiRelease(&mHandoff)==EFI_NOT_READY && mHandoff.MemoryMap && mHandoff.RecordOwned);
    NewImage();Start();mFreePageStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiRelease(&mHandoff)==EFI_NOT_READY && mHandoff.RecordOwned);
    NewImage();mPageStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiPrepare(&mHandoff)==EFI_NOT_READY && !mHandoff.RecordOwned && !mPages);
    NewImage();mPoolStatus=EFI_WARN_WRITE_FAILURE;assert(CrMdssUefiPrepare(&mHandoff)==EFI_NOT_READY && mHandoff.RecordOwned && !mHandoff.MemoryMap);
  }else if(!strcmp(Mode,"overlap")){
    Start();mRecordOverlap=TRUE;Exit(FALSE);Unpublished();
    NewImage();Start();mTableOverlap=TRUE;Exit(FALSE);Unpublished();
  }else if(!strcmp(Mode,"interrupted")){
    /* A prepare output commits before higher-TPL freeze; retain it, perform
       no later allocations/installations, and never publish this incarnation. */
    mInterruptPrepare=TRUE;assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS);
    assert(mHandoff.Frozen && mHandoff.RecordOwned && !mHandoff.Installed && !mPools);
  }else if(!strcmp(Mode,"entry")){
    for(UINTN I=1;I<=4;++I){NewImage();mFailCreate=I;assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_OUT_OF_RESOURCES && mClosedEvents==I-1 && !mBefore && !mAfter);}
    NewImage();mNotifyStatus=EFI_DEVICE_ERROR;assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && mClosedEvents==1);
    NewImage();mCompleteRegisterStatus=EFI_DEVICE_ERROR;assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && !mBefore && !mAfter && mClosedEvents==4);
    NewImage();mPrepareRegisterStatus=EFI_DEVICE_ERROR;assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR && !mBefore && !mAfter && mClosedEvents==4);
    NewImage();mInterfaceStatus=EFI_DEVICE_ERROR;mUnregisterStatus=EFI_ACCESS_DENIED;
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && mPrepareObserver && mCompleteObserver && !mActive && mBefore && mAfter);
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
    NewImage();mInterfaceStatus=EFI_DEVICE_ERROR;mFailClose=2;
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && mEndEvent && !mActive && !mBefore && !mAfter);
  }else if(!strcmp(Mode,"registration")){
    Start();assert(SetParticipant(&mProtocol,1,&mOwners[0],FreezeOwner,SnapshotOwner)==EFI_SUCCESS);
    assert(SetParticipant(&mProtocol,1,&mOwners[1],FreezeOwner,SnapshotOwner)==EFI_ALREADY_STARTED);
    assert(SetParticipant(&mProtocol,1,&mOwners[0],NULL,NULL)==EFI_SUCCESS);
    assert(SetParticipant(&mProtocol,1,&mOwners[0],NULL,NULL)==EFI_NOT_FOUND);
    assert(SetParticipant(&mProtocol,1,&mOwners[0],FreezeOwner,SnapshotOwner)==EFI_SUCCESS);
    Exit(FALSE);assert(SetParticipant(&mProtocol,1,&mOwners[0],NULL,NULL)==EFI_ACCESS_DENIED);
  }else if(!strcmp(Mode,"cache-map")){
    Start();mBadCurrentCache=TRUE;Exit(FALSE);Unpublished();
    NewImage();Start();mBadMap=TRUE;Exit(FALSE);Unpublished();
    NewImage();Start();mDuplicateDescriptor=TRUE;Exit(FALSE);Unpublished();
    NewImage();mPoolStatus=EFI_OUT_OF_RESOURCES;assert(CrMdssUefiPrepare(&mHandoff)==EFI_OUT_OF_RESOURCES && mPages && !mPools);
    mPoolStatus=EFI_SUCCESS;assert(CrMdssUefiPrepare(&mHandoff)==EFI_SUCCESS && mPages==1);
  }else if(!strcmp(Mode,"deferred")){
    mCounterStatus=EFI_NOT_READY;
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && mActive && mBefore && mAfter && !mPages);
    RegisterOwners();mCounterStatus=EFI_SUCCESS;
    TEST_EVENT *Event=mReadyEvent;EFI_TPL Old=MockRaise(Event->Tpl);Event->Notify(Event,Event->Context);MockRestore(Old);
    assert(mHandoff.Installed && mPages==1 && mPools==1 && mAcpiTables==1);Exit(TRUE);assert(mHandoff.Published);
    NewImage();mAcpiInstallStatus=EFI_OUT_OF_RESOURCES;
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && !mHandoff.Installed && mPages==1 && mPools==1);
    UINT64 Generation=mHandoff.Generation;UINTN Counts=mMonotonic;
    mAcpiInstallStatus=EFI_SUCCESS;Event=mAcpiEvent;Old=MockRaise(Event->Tpl);Event->Notify(Event,Event->Context);MockRestore(Old);
    assert(mHandoff.Installed && mPages==1 && mPools==1 && mHandoff.Generation==Generation && mMonotonic==Counts);
    RegisterOwners();Exit(FALSE);assert(mHandoff.Published);
  }else if(!strcmp(Mode,"release")){
    Start();mAcpiUninstallStatus=EFI_ACCESS_DENIED;
    assert(CrMdssUefiRelease(&mHandoff)==EFI_ACCESS_DENIED && mHandoff.Installed && mHandoff.RecordOwned && mHandoff.MemoryMap);
    mAcpiUninstallStatus=EFI_SUCCESS;assert(CrMdssUefiRelease(&mHandoff)==EFI_SUCCESS && !mPages && !mPools && !mAcpiTables);
    NewImage();Start();mFreePoolStatus=EFI_DEVICE_ERROR;
    assert(CrMdssUefiRelease(&mHandoff)==EFI_DEVICE_ERROR && !mHandoff.Installed && mHandoff.MemoryMap && mHandoff.RecordOwned);
    mFreePoolStatus=EFI_SUCCESS;assert(CrMdssUefiRelease(&mHandoff)==EFI_SUCCESS && !mPages && !mPools);
    NewImage();Start();mFreePageStatus=EFI_DEVICE_ERROR;
    assert(CrMdssUefiRelease(&mHandoff)==EFI_DEVICE_ERROR && !mHandoff.MemoryMap && mHandoff.RecordOwned);
    mFreePageStatus=EFI_SUCCESS;assert(CrMdssUefiRelease(&mHandoff)==EFI_SUCCESS && !mPages);
    NewImage();mInterfaceStatus=EFI_DEVICE_ERROR;mUnregisterStatus=EFI_ACCESS_DENIED;
    assert(MdssHandoffCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS && mBefore && mAfter);
    mUnregisterStatus=EFI_SUCCESS;assert(Rollback()==EFI_SUCCESS && !mBefore && !mAfter && mClosedEvents==4);
  }else if(!strcmp(Mode,"page-extents")){
    mPixelTail=TRUE;Start();Exit(FALSE);
    assert(mHandoff.Published && CrMdssHandoffCanClaim(mPageAllocation)==CR_SUCCESS);
    for(UINT32 Route=0;Route<2;++Route){
      NewImage();mPixelTail=mShortGcdTail=TRUE;mGeometryRoute=Route;Start();Exit(FALSE);Unpublished();
    }
    NewImage();mPixelTail=mShortReservedTail=TRUE;Start();Exit(FALSE);Unpublished();
  }else if(!strcmp(Mode,"mapping-extent")){
    for(UINT32 Route=0;Route<2;++Route)for(UINT32 Fault=1;Fault<=5;++Fault){
      NewImage();mPixelTail=TRUE;mGeometryRoute=Route;mBadGeometry=Fault;Start();Exit(FALSE);
      assert(mHandoff.Published && CrMdssHandoffCanClaim(mPageAllocation)==CR_NOT_READY);
      assert(mHandoff.Local.Routes[Route].MappingState==CR_MDSS_NATIVE_MAPPING_UNKNOWN);
      assert(mHandoff.Local.Routes[Route].MappingStatus==EFI_COMPROMISED_DATA);
    }
  }else if(!strcmp(Mode,"mapping")){
    Start();mForeignMap=TRUE;Exit(FALSE);assert(mHandoff.Published && CrMdssHandoffCanClaim(mPageAllocation)==CR_NOT_READY);
    NewImage();Start();mQueryStatus=EFI_WARN_WRITE_FAILURE;Exit(FALSE);assert(mHandoff.Published && CrMdssHandoffCanClaim(mPageAllocation)==CR_NOT_READY);
    NewImage();Start();mFreezeFailure=EFI_WARN_WRITE_FAILURE;Exit(FALSE);Unpublished();
    NewImage();Start();mSnapshotFailure=EFI_WARN_WRITE_FAILURE;Exit(FALSE);Unpublished();
  }else abort();
  NewImage();printf("Actual native MDSH coordinator %s PASS\n",Mode);return 0;
}
