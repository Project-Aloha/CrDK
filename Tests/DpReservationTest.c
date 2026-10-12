/** Actual fixed owner/GOP/current RAM query effects and refusal tests. MIT */
#define main DpLifecycleCompatibilityMain
#include "DpLifecycleTest.c"
#undef main
#include "../Driver/RamPartitionCrDxe/RamPartitionCrDxe.c"

EFI_GUID gEfiSmemCrProtocolGuid,gEfiRamPartitionProtocolGuid;
UINTN EFIAPI ArmGetPhysicalAddressBits(VOID){return 48;}
UINT64 EFIAPI ArmGetTCR(VOID){return 16;}
EFI_STATUS ArmSetMemoryAttributes(EFI_PHYSICAL_ADDRESS B,UINT64 N,UINT64 A,UINT64 M){
  (VOID)B;(VOID)N;(VOID)A;(VOID)M;assert(FALSE);return EFI_DEVICE_ERROR;
}
static EFI_STATUS RangeStatus,LocateStatus,ReserveStatus,ReleaseStatus,DescriptorStatus,SetStatus;
static BOOLEAN ReserveCommit,ReleaseCommit,SetCommit,ShortDescriptor,BadType;
static BOOLEAN ChangeReserveAddress;
static UINTN RangeChecks,ReserveCalls,ReleaseCalls;
static EFI_STATUS EFIAPI CurrentRange(EFI_CR_RAM_RANGE_PROTOCOL *This,UINT64 Base,UINT64 Bytes){
  ++RangeChecks;assert(Base==ReservationMap[0].Address&&Bytes==CR_DP_RESERVATION_BYTES);
  if(RangeStatus!=EFI_SUCCESS)return RangeStatus;
  return CheckAppsReadWriteRange(This,Base,Bytes);
}
static EFI_STATUS EFIAPI CurrentLocate(EFI_GUID *G,VOID *R,VOID **P){
  if(G!=&gEfiCrRamRangeProtocolGuid)return Locate(G,R,P);
  assert(!R);*P=&mRangeProtocol;return LocateStatus;
}
static EFI_STATUS EFIAPI CurrentReserve(EFI_GCD_ALLOCATE_TYPE T,EFI_GCD_MEMORY_TYPE M,UINTN A,
  UINT64 N,EFI_PHYSICAL_ADDRESS *B,EFI_HANDLE I,EFI_HANDLE D){
  ++ReserveCalls;
  if(ReserveStatus==EFI_SUCCESS||ReserveCommit)assert(ReserveTest(T,M,A,N,B,I,D)==EFI_SUCCESS);
  if(ChangeReserveAddress)*B+=0x10000;
  return ReserveStatus;
}
static EFI_STATUS EFIAPI CurrentRelease(EFI_PHYSICAL_ADDRESS B,UINT64 N){
  ++ReleaseCalls;
  if(ReleaseStatus==EFI_SUCCESS||ReleaseCommit)assert(ReleaseTest(B,N)==EFI_SUCCESS);
  return ReleaseStatus;
}
static EFI_STATUS EFIAPI CurrentDescriptor(EFI_PHYSICAL_ADDRESS B,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  if(DescriptorStatus!=EFI_SUCCESS)return DescriptorStatus;
  EFI_STATUS S=GcdGet(B,D);
  if(ShortDescriptor)--D->Length;
  if(BadType)D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;
  return S;
}
static EFI_STATUS EFIAPI CurrentSet(EFI_PHYSICAL_ADDRESS B,UINT64 N,UINT64 A){
  if(SetStatus==EFI_SUCCESS||SetCommit)assert(GcdSet(B,N,A)==EFI_SUCCESS);
  return SetStatus;
}
static VOID SetupReservation(VOID){
  Fixture();mRamInventoryReady=TRUE;mRamProtocolHandle=NULL;
  ZeroMem(&mRam,sizeof(mRam));mRam.BankCount=1;
  mRam.Banks[0]=(CR_RAM_PARTITION_ENTRY){.Base=ReservationMap[0].Address,.Length=CR_DP_RESERVATION_BYTES+65536,
    .AvailableLength=CR_DP_RESERVATION_BYTES,.Attribute=CR_RAM_PARTITION_READWRITE};
  mRangeProtocol=(EFI_CR_RAM_RANGE_PROTOCOL){EFI_CR_RAM_RANGE_PROTOCOL_REVISION,CurrentRange};
  Services.LocateProtocol=CurrentLocate;DxeServices.AllocateMemorySpace=CurrentReserve;
  DxeServices.FreeMemorySpace=CurrentRelease;DxeServices.GetMemorySpaceDescriptor=CurrentDescriptor;
  DxeServices.SetMemorySpaceAttributes=CurrentSet;
  RangeStatus=LocateStatus=ReserveStatus=ReleaseStatus=DescriptorStatus=SetStatus=EFI_SUCCESS;
  ReserveCommit=ReleaseCommit=SetCommit=ShortDescriptor=BadType=ChangeReserveAddress=FALSE;
  RangeChecks=ReserveCalls=ReleaseCalls=0;
}
static VOID Untouched(VOID){
  assert(!CacheTransitions&&!CacheCleans&&!CacheFlushes&&!mBltConfig&&!Maps);
  for(UINTN I=0;I<CR_DP_RESERVATION_BYTES;++I)assert(((UINT8 *)Backing)[I]==0xa5);
}
static VOID CleanupReservation(VOID){
  RangeStatus=DescriptorStatus=SetStatus=ReleaseStatus=EFI_SUCCESS;
  ShortDescriptor=BadType=GcdOverflow=FALSE;GcdProtected=0;BadCacheReadback=FALSE;
  mRam.Banks[0].AvailableLength=CR_DP_RESERVATION_BYTES;mRam.Banks[0].Attribute=CR_RAM_PARTITION_READWRITE;
  assert(CrDpGopRelease()==EFI_SUCCESS&&!mGopPages&&!mReservationClaimAttempted&&Allocations==Frees);
}
int main(VOID){
  SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
  assert(CrDpGopPrepare()==EFI_SUCCESS&&mGopPages==2048&&mGopMode.FrameBufferSize==0x7e9000&&ReserveCalls==1);
  assert(CacheCleans==1&&CacheTransitions==1&&RangeChecks>=4&&BackingOwner==(EFI_HANDLE)1);
  for(UINTN I=0;I<mGopMode.FrameBufferSize;++I)assert(((UINT8 *)Backing)[I]==0);
  for(UINTN I=mGopMode.FrameBufferSize;I<CR_DP_RESERVATION_BYTES;++I)assert(((UINT8 *)Backing)[I]==0xa5);
  UINTN Prior=CacheTransitions;assert(CrDpGopPrepare()==EFI_SUCCESS&&CacheTransitions==Prior&&ReserveCalls==1);
  CleanupReservation();assert(ReleaseCalls==1);
  /* All availability refusals occur before claiming, cache cleaning or pixels. */
  for(UINTN Case=0;Case<5;++Case){
    SetupReservation();if(Case==0)mRam.Banks[0].AvailableLength=0;
    if(Case==1)mRam.Banks[0].AvailableLength=CR_DP_RESERVATION_BYTES-4096;
    if(Case==2)mRam.Banks[0].Attribute=0;
    if(Case==3)mRamInventoryReady=FALSE;
    if(Case==4)mRam.Banks[0].Base+=4096;
    assert(CrDpReservationInit((EFI_HANDLE)1)!=EFI_SUCCESS&&!mReservationImage&&!ReserveCalls);Untouched();
  }
  for(UINTN Case=0;Case<5;++Case){
    SetupReservation();if(Case==0)ReservationMap[0].Name[0]='X';
    if(Case==1)ReservationMap[0].MemoryType=Conv;
    if(Case==2)ReservationMap[0].Address+=4096;
    if(Case==3)ReservationMap[0].ResourceType=SYS_MEM;
    if(Case==4)ReservationMap[0].ResourceAttribute&=~EFI_RESOURCE_ATTRIBUTE_TESTED;
    assert(CrDpReservationInit((EFI_HANDLE)1)!=EFI_SUCCESS&&!mReservationImage&&!RangeChecks);Untouched();
  }
  SetupReservation();LocateStatus=EFI_NOT_FOUND;
  assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_NOT_FOUND&&!mReservationImage);Untouched();
  SetupReservation();mRangeProtocol.Revision=0;
  assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_UNSUPPORTED&&!RangeChecks);Untouched();
  SetupReservation();mRangeProtocol.CheckAppsReadWriteRange=NULL;
  assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_UNSUPPORTED&&!RangeChecks);Untouched();
  CONST EFI_STATUS Nonfinal[]={EFI_WARN_UNKNOWN_GLYPH,EFI_NOT_READY,EFI_DEVICE_ERROR};
  for(UINTN I=0;I<ARRAY_SIZE(Nonfinal);++I){
    SetupReservation();LocateStatus=Nonfinal[I];assert(CrDpReservationInit((EFI_HANDLE)1)!=EFI_SUCCESS&&!mReservationImage);Untouched();
    SetupReservation();RangeStatus=Nonfinal[I];assert(CrDpReservationInit((EFI_HANDLE)1)!=EFI_SUCCESS&&!mReservationImage);Untouched();
    SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
    RangeStatus=Nonfinal[I];assert(CrDpGopPrepare()!=EFI_SUCCESS&&!ReserveCalls);Untouched();CleanupReservation();
  }
  for(UINTN Case=0;Case<6;++Case){
    SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
    if(Case==0)BackingOwner=(EFI_HANDLE)2;
    if(Case==1)ShortDescriptor=TRUE;
    if(Case==2)BadType=TRUE;
    if(Case==3)GcdCapabilities&=~CR_DP_RESERVED_STATE_CAPS;
    if(Case==4)GcdProtected=EFI_MEMORY_RO;
    if(Case==5)DescriptorStatus=EFI_WARN_UNKNOWN_GLYPH;
    assert(CrDpGopPrepare()!=EFI_SUCCESS);Untouched();
    if(Case==0)BackingOwner=NULL;
    CleanupReservation();
  }
  /* Each nonfinal mutation is tested both before and after its real effect. */
  for(UINTN I=0;I<ARRAY_SIZE(Nonfinal);++I)for(UINTN Commit=0;Commit<2;++Commit){
    SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
    ReserveStatus=Nonfinal[I];ReserveCommit=Commit;
    assert(CrDpGopPrepare()!=EFI_SUCCESS&&mReservationClaimAttempted&&!mGopPages);Untouched();
    EFI_PHYSICAL_ADDRESS B=0;UINT64 N=0;BOOLEAN Ready=TRUE;
    assert(CrDpGopAllocation(&B,&N,&Ready)&&B==(UINTN)Backing&&N==0x7e9000&&!Ready);
    ReserveStatus=EFI_SUCCESS;assert(CrDpGopPrepare()==EFI_SUCCESS&&ReserveCalls==(Commit?1:2));CleanupReservation();
    SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
    SetStatus=Nonfinal[I];SetCommit=Commit;
    assert(CrDpGopPrepare()!=EFI_SUCCESS&&mGopPages&&mGopAttributesAttempted&&!CacheFlushes&&!mBltConfig);
    SetStatus=EFI_SUCCESS;assert(CrDpGopPrepare()==EFI_SUCCESS);CleanupReservation();
    SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS&&CrDpGopPrepare()==EFI_SUCCESS);
    ReleaseStatus=Nonfinal[I];ReleaseCommit=Commit;
    assert(CrDpGopRelease()!=EFI_SUCCESS&&mReservationFreeAttempted&&mGopPages&&mBltConfig);
    Prior=CacheTransitions;ReleaseStatus=EFI_SUCCESS;assert(CrDpGopRelease()==EFI_SUCCESS&&CacheTransitions==Prior);
    assert(ReleaseCalls==(Commit?1:2)&&!mGopPages&&Allocations==Frees);
  }
  SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS&&CrDpGopPrepare()==EFI_SUCCESS);
  SetStatus=EFI_WARN_UNKNOWN_GLYPH;SetCommit=TRUE;
  assert(CrDpGopRelease()==EFI_NOT_READY&&mGopPages&&!ReleaseCalls&&mBltConfig);
  SetStatus=EFI_SUCCESS;CleanupReservation();
  SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS);
  ChangeReserveAddress=TRUE;assert(CrDpGopPrepare()==EFI_DEVICE_ERROR&&mReservationUnknownAddress&&!mGopPages);Untouched();
  assert(CrDpGopRelease()==EFI_ACCESS_DENIED&&BackingOwner==(EFI_HANDLE)1);
  /* Test-only reboot of permanently uncertain allocation output. */
  ChangeReserveAddress=FALSE;mReservationUnknownAddress=FALSE;CleanupReservation();
  SetupReservation();assert(CrDpReservationInit((EFI_HANDLE)1)==EFI_SUCCESS&&CrDpGopPrepare()==EFI_SUCCESS);
  mActive=TRUE;BOOLEAN New=FALSE;InstallStatus=EFI_WARN_UNKNOWN_GLYPH;CommitInstall=TRUE;
  assert(CrDpGopPublish(&New)==EFI_WARN_UNKNOWN_GLYPH&&!New&&CrDpGopPublished());
  assert(CrDpGopRelease()==EFI_ACCESS_DENIED&&BackingOwner==(EFI_HANDLE)1);
  mGopPublicationPending=FALSE;mGopHandle=NULL;CrDpGopExit();Prior=CacheTransitions;
  assert(CrDpGopRelease()==EFI_ACCESS_DENIED&&CacheTransitions==Prior&&BackingOwner==(EFI_HANDLE)1);
  mGopExiting=FALSE;mActive=FALSE;CleanupReservation();
  free(Backing);Backing=NULL;
  puts("DP fixed Reserved: whole current RW grant, exact HDK profile/owner/attributes, 8 MiB cache scope, partial mutation retry, publication/EBS retention PASS");
  return 0;
}
