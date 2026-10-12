/* Actual native ACPI/final publication and real ARC qualification. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <PiDxe.h>
#include <Library/PILCrHandoffUefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
EFI_GUID gEfiAcpiTableProtocolGuid;
EFI_BOOT_SERVICES *gBS;
EFI_DXE_SERVICES *gDS;
STATIC CR_PIL_UEFI_HANDOFF mState;
STATIC CR_BUS_PIL_LOCATOR mTable;
STATIC CR_PIL_CONTEXT mPil;
STATIC CR_PIL_CLIENTS mClients;
STATIC EFI_RPMH_CR_PROTOCOL mRpmh;
STATIC EFI_RPMH_CR_ARC_SNAPSHOT mArc;
STATIC CR_PIL_TARGET mTarget={.PasId=1,.MemoryBase=0x85e00000,.MemorySize=0x2100000,
 .QmpBase=0xc300000,.QmpSize=0x400,.IpccSend=0xed1800c,.IpccClient=3};
STATIC UINT32 mQmp[256];
STATIC UINTN mPages,mPools,mInstalls,mSeals,mMaps,mFences,mCacheWrites;
extern UINT64 gCurrentMonotonicCount;
EFI_STATUS EFIAPI GetNextMonotonicCount(UINT64 *Count);
STATIC UINTN mMonotonicCalls;
STATIC BOOLEAN mZeroCounter;
STATIC EFI_STATUS mLocateError,mAllocateError,mPoolError,mInstallError,mUninstallError,mFreeError,mFreePoolError,mSealError,mMapError;
STATIC BOOLEAN mEbs,mBadRam,mBadCarveout,mBadRecord,mMapSmall,mMissingCarveout,mMonotonicError,mPartialOverlap,mWrappingMap,mUnalignedMap;
STATIC VOID *mRecord;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
VOID EFIAPI MemoryFence(VOID){mFences++;}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){assert((UINTN)P>=(UINTN)mRecord && (UINTN)P+N<=(UINTN)mRecord+4096);mCacheWrites++;if(mEbs){UINT64 Seq=((CR_BUS_PIL_HANDOFF*)mRecord)->PublishSequence;assert(Seq==1||Seq==2);}return P;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}
STATIC EFI_STATUS EFIAPI Install(EFI_ACPI_TABLE_PROTOCOL *P,VOID *B,UINTN N,UINTN *K){(VOID)P;assert(!mEbs && N==68 && !mInstalls);if(EFI_ERROR(mInstallError))return mInstallError;memcpy(&mTable,B,N);assert(CalculateCheckSum8(B,N)==0);mInstalls++;*K=77;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_ACPI_TABLE_PROTOCOL *P,UINTN K){(VOID)P;assert(!mEbs && K==77 && mInstalls==1);if(EFI_ERROR(mUninstallError))return mUninstallError;mInstalls--;return EFI_SUCCESS;}
STATIC EFI_ACPI_TABLE_PROTOCOL mAcpi={Install,Uninstall};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){assert(!mEbs && G==&gEfiAcpiTableProtocolGuid && !R);if(EFI_ERROR(mLocateError))return mLocateError;*P=&mAcpi;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Monotonic(UINT64 *N){assert(!mEbs);++mMonotonicCalls;if(mMonotonicError)return EFI_DEVICE_ERROR;if(mZeroCounter){*N=0;return EFI_SUCCESS;}return GetNextMonotonicCount(N);}
STATIC EFI_STATUS EFIAPI Pages(EFI_ALLOCATE_TYPE A,EFI_MEMORY_TYPE T,UINTN N,EFI_PHYSICAL_ADDRESS *P){assert(!mEbs && !mPages && A==AllocateAnyPages && T==EfiReservedMemoryType && N==1);if(EFI_ERROR(mAllocateError))return mAllocateError;mRecord=aligned_alloc(4096,4096);assert(mRecord);memset(mRecord,0xa5,4096);*P=(UINTN)mRecord;mPages++;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI FreePages(EFI_PHYSICAL_ADDRESS P,UINTN N){assert(!mEbs && mPages==1 && P==(UINTN)mRecord && N==1 && !mInstalls);if(EFI_ERROR(mFreeError))return mFreeError;free(mRecord);mRecord=NULL;mPages--;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Pool(EFI_MEMORY_TYPE T,UINTN N,VOID **P){assert(!mEbs && !mPools && T==EfiBootServicesData && N>=65536);if(EFI_ERROR(mPoolError))return mPoolError;*P=malloc(N);assert(*P);mPools++;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI FreePool(VOID *P){assert(!mEbs && mPools==1 && !mInstalls);if(EFI_ERROR(mFreePoolError))return mFreePoolError;free(P);mPools--;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Gcd(EFI_PHYSICAL_ADDRESS P,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){memset(D,0,sizeof(*D));D->BaseAddress=P;D->Length=4096;D->GcdMemoryType=EfiGcdMemoryTypeSystemMemory;D->Attributes=mBadRam?EFI_MEMORY_UC:EFI_MEMORY_WB;if(P==mTarget.MemoryBase){D->GcdMemoryType=EfiGcdMemoryTypeReserved;D->Length=mTarget.MemorySize;D->Attributes=EFI_MEMORY_UC;}return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Map(UINTN *N,EFI_MEMORY_DESCRIPTOR *D,UINTN *K,UINTN *S,UINT32 *V){mMaps++;*S=48;*V=1;*K=123;if(!D||mMapSmall){*N=mMapSmall?0x100000:96;return EFI_BUFFER_TOO_SMALL;}if(EFI_ERROR(mMapError))return mMapError;assert(*N>=144);memset(D,0,144);D->Type=mBadRecord?EfiBootServicesData:EfiReservedMemoryType;D->PhysicalStart=(UINTN)mRecord;D->NumberOfPages=1;D->Attribute=0x100f;D=(VOID*)((UINT8*)D+48);D->Type=mBadCarveout?EfiConventionalMemory:EfiReservedMemoryType;D->PhysicalStart=mMissingCarveout?0:mTarget.MemoryBase;D->NumberOfPages=mTarget.MemorySize/4096;D->Attribute=EFI_MEMORY_UC;*N=96;if(mPartialOverlap||mWrappingMap||mUnalignedMap){D=(VOID*)((UINT8*)D+48);D->Type=EfiBootServicesData;D->PhysicalStart=mWrappingMap?0xfffffffffffff000ULL:(mUnalignedMap?0x80000001:mTarget.MemoryBase+4096);D->NumberOfPages=mWrappingMap?2:1;*N=144;}return EFI_SUCCESS;}
STATIC UINT32 Read(VOID *C,UINT32 A){assert(mEbs && C==&mPil && A>=0xc300000 && A<0xc300400 && !(A&3));return mQmp[(A-0xc300000)/4];}
STATIC VOID Fence(VOID *C){assert(C==&mPil);mFences++;}
STATIC EFI_STATUS EFIAPI Seal(EFI_RPMH_CR_PROTOCOL *P,EFI_RPMH_CR_ARC_SNAPSHOT *O){assert(mEbs && P==&mRpmh);mSeals++;if(mSealError!=EFI_SUCCESS)return mSealError;*O=mArc;return EFI_SUCCESS;}
STATIC CR_STATUS Quiet(VOID *C){(VOID)C;abort();}
STATIC VOID Reset(VOID){
 assert(!mPages&&!mPools&&!mInstalls);memset(&mState,0,sizeof(mState));memset(&mPil,0,sizeof(mPil));memset(&mClients,0,sizeof(mClients));memset(&mArc,0,sizeof(mArc));memset(mQmp,0,sizeof(mQmp));
 mLocateError=mAllocateError=mPoolError=mInstallError=mUninstallError=mFreeError=mFreePoolError=mSealError=mMapError=EFI_SUCCESS;
 mEbs=mBadRam=mBadCarveout=mBadRecord=mMapSmall=mMissingCarveout=mMonotonicError=mPartialOverlap=mWrappingMap=mUnalignedMap=FALSE;mSeals=mMaps=mFences=mCacheWrites=0;mMonotonicCalls=0;mZeroCounter=FALSE;gCurrentMonotonicCount=100;
 STATIC EFI_BOOT_SERVICES B={.LocateProtocol=Locate,.GetNextMonotonicCount=Monotonic,.AllocatePages=Pages,.FreePages=FreePages,.AllocatePool=Pool,.FreePool=FreePool,.GetMemoryMap=Map};
 STATIC EFI_DXE_SERVICES D={.GetMemorySpaceDescriptor=Gcd};gBS=&B;gDS=&D;
 mPil.Target=&mTarget;mPil.Io.Context=&mPil;mPil.Io.Read32=Read;mPil.Io.Fence=Fence;mPil.StartGeneration=7;mPil.NativeShutdownGeneration=7;mPil.NativeShutdownEl=2;mPil.SmcBase=0x42000000;mPil.QmpOffset=0x80;
 mPil.Shutdown=(CR_PIL_SHUTDOWN_EVIDENCE){.Generation=7,.FunctionId=0x42000206,.CallStatus=CR_SUCCESS,.Attempted=TRUE,.Returned=TRUE,.Complete=TRUE};
 mClients.Frozen=TRUE;mClients.Count=1;mClients.Entries[0]=(CR_PIL_CLIENT){&mClients,NULL,Quiet,TRUE};
 mRpmh.Revision=EFI_RPMH_CR_PROTOCOL_REVISION;mRpmh.SealArcSnapshot=Seal;
 mQmp[0]=0x4d41494c;mQmp[1]=1;mQmp[0x34/4]=128;mQmp[0x38/4]=0x80;mQmp[0x24/4]=mQmp[0x28/4]=mQmp[0x2c/4]=mQmp[0x30/4]=0xffff0000;
 mArc.Size=sizeof(mArc);mArc.Revision=1;mArc.Count=6;mArc.OwnerCount=1;mArc.Flags=7;
 CONST char *Names[]={"cx.lvl","mx.lvl","lcx.lvl","lmx.lvl","mmcx.lvl","xo.lvl"};
 for(UINT32 I=0;I<6;I++){strcpy(mArc.Resources[I].Name,Names[I]);mArc.Resources[I].Address=0x30000+0x10*I;mArc.Resources[I].Kind=I==5?CrRpmhArcXo:CrRpmhArcPower;mArc.Resources[I].LevelCount=I==5?0:2;mArc.Resources[I].Levels[1]=I==5?0:16;mArc.Corners[I]=I==5?3:1;}
}
STATIC VOID Prepare(VOID){assert(CrPilHandoffPrepare(&mState)==EFI_SUCCESS);assert(mPages==1&&mPools==1&&mInstalls==1&&mState.Installed);assert(mTable.RecordPhysical==mState.Record&&mTable.RecordLength==1024&&mTable.RecordGeneration==mState.Generation);assert(((CR_BUS_PIL_HANDOFF*)mRecord)->PublishSequence==0);mEbs=TRUE;}
STATIC EFI_STATUS Publish(VOID){return CrPilHandoffPublish(&mState,&mPil,&mClients,&mRpmh);}
STATIC VOID Release(VOID){mEbs=FALSE;mState.Published=FALSE;assert(CrPilHandoffRelease(&mState)==EFI_SUCCESS);assert(!mPages&&!mPools&&!mInstalls);}
STATIC VOID Denied(VOID){assert(EFI_ERROR(Publish()));assert(!mState.Published&&((CR_BUS_PIL_HANDOFF*)mRecord)->PublishSequence==0);Release();}
int main(int Argc,char **Argv){
 Reset();Prepare();UINTN Maps=mMaps;assert(Publish()==EFI_SUCCESS&&mState.Published);CR_BUS_PIL_HANDOFF *H=mRecord;assert(H->PublishSequence==2&&H->Generation==mState.Generation&&H->FirmwareEl==2&&H->MemoryDescriptorAttributes==EFI_MEMORY_UC&&H->Arc[5].Corner==3);assert(H->Checksum==CalculateCrc32(H,1016));assert(CrPilHandoffRelease(&mState)==EFI_ACCESS_DENIED);assert(Publish()==EFI_SUCCESS&&mMaps==Maps+1&&mSeals==1);if(Argc==2){FILE *F=fopen(Argv[1],"wb");assert(F&&fwrite(H,1,sizeof(*H),F)==sizeof(*H));fclose(F);}Release();
 Reset();gCurrentMonotonicCount=0;Prepare();assert(mState.Generation==1&&mMonotonicCalls==2);assert(Publish()==EFI_SUCCESS);Release();
 Reset();Prepare();mQmp[0x24/4]=mQmp[0x28/4]=mQmp[0x2c/4]=mQmp[0x30/4]=0xffff;assert(Publish()==EFI_SUCCESS);assert(((CR_BUS_PIL_HANDOFF*)mRecord)->QmpLocalLink==0xffff&&((CR_BUS_PIL_HANDOFF*)mRecord)->QmpLocalChannel==0xffff);Release();
 Reset();Prepare();mPil.QmpEstablished=TRUE;Denied();
 Reset();mZeroCounter=TRUE;assert(CrPilHandoffPrepare(&mState)==EFI_NOT_READY&&mMonotonicCalls==2&&!mPages&&!mInstalls);Release();
 Reset();Prepare();mPil.Shutdown.Complete=FALSE;Denied();
 Reset();Prepare();mPil.NativeShutdownGeneration=6;Denied();
 Reset();Prepare();mPil.NativeShutdownEl=3;Denied();
 Reset();Prepare();mPil.SmcInterrupted=TRUE;Denied();
 Reset();Prepare();mPil.PasOwned=TRUE;Denied();
 Reset();Prepare();mClients.Entries[0].Quiet=FALSE;Denied();
 Reset();Prepare();mClients.Dispatching=TRUE;Denied();
 Reset();Prepare();mQmp[0x80/4]=64;Denied();
 Reset();Prepare();mQmp[0x28/4]=0xffff;Denied();
 Reset();Prepare();mBadRecord=TRUE;Denied();
 Reset();Prepare();mBadCarveout=TRUE;Denied();
 Reset();Prepare();mMissingCarveout=TRUE;Denied();
 Reset();Prepare();mMapSmall=TRUE;Denied();
 Reset();Prepare();mPartialOverlap=TRUE;Denied();
 Reset();Prepare();mWrappingMap=TRUE;Denied();
 Reset();Prepare();mUnalignedMap=TRUE;Denied();
 Reset();Prepare();mSealError=EFI_WARN_WRITE_FAILURE;Denied();
 Reset();Prepare();mArc.Corners[0]=0;Denied();
 Reset();Prepare();mArc.Resources[1].Address=mArc.Resources[0].Address;Denied();
 Reset();Prepare();mArc.Resources[0].Levels[0]=16;Denied();
 Reset();mPoolError=EFI_OUT_OF_RESOURCES;assert(CrPilHandoffPrepare(&mState)==EFI_OUT_OF_RESOURCES&&mPages==1&&!mPools&&!mInstalls);mPoolError=EFI_SUCCESS;Prepare();assert(Publish()==EFI_SUCCESS);Release();
 Reset();mInstallError=EFI_OUT_OF_RESOURCES;assert(CrPilHandoffPrepare(&mState)==EFI_OUT_OF_RESOURCES&&mPages==1&&mPools==1&&!mInstalls);mInstallError=EFI_SUCCESS;Prepare();Release();
 Reset();Prepare();mEbs=FALSE;mUninstallError=EFI_ACCESS_DENIED;assert(CrPilHandoffRelease(&mState)==EFI_ACCESS_DENIED&&mPages&&mPools&&mInstalls);mUninstallError=EFI_SUCCESS;Release();
 Reset();Prepare();mEbs=FALSE;mFreeError=EFI_DEVICE_ERROR;assert(CrPilHandoffRelease(&mState)==EFI_DEVICE_ERROR&&mPages&&!mPools&&!mInstalls);mFreeError=EFI_SUCCESS;Release();
 Reset();mBadRam=TRUE;assert(CrPilHandoffPrepare(&mState)==EFI_ACCESS_DENIED&&mPages&&!mInstalls);mBadRam=FALSE;Prepare();Release();
 Reset();mMonotonicError=TRUE;assert(CrPilHandoffPrepare(&mState)==EFI_DEVICE_ERROR&&!mPages);Release();
 puts("PIL native ACPI locator/final-map evidence, strict retirement, immutable CRC publication and retained failures PASS");return 0;
}
