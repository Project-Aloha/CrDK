/* Actual complete ESP backend with independent EFI filesystem/media faults.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
#include <Guid/Gpt.h>
#include <Guid/VariableFormat.h>
#include <Protocol/MmcHost.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/ScsiPassThru.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/SimpleFileSystem.h>
#include <Library/DebugLib.h>
#include "../Library/EspVariablePersistenceLib/EspVariablePersistence.c"

EFI_GUID gEfiAuthenticatedVariableGuid = EFI_AUTHENTICATED_VARIABLE_GUID;
EFI_GUID gEdkiiUfsHostControllerProtocolGuid = EDKII_UFS_HOST_CONTROLLER_PROTOCOL_GUID;
EFI_GUID gEfiPartTypeSystemPartGuid = EFI_PART_TYPE_EFI_SYSTEM_PART_GUID;
EFI_GUID gEfiBlockIoProtocolGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiExtScsiPassThruProtocolGuid = EFI_EXT_SCSI_PASS_THRU_PROTOCOL_GUID;
EFI_GUID gEfiScsiPassThruProtocolGuid = EFI_SCSI_PASS_THRU_PROTOCOL_GUID;
EFI_GUID gEmbeddedMmcHostProtocolGuid = EMBEDDED_MMC_HOST_PROTOCOL_GUID;
EFI_GUID gEfiPartitionInfoProtocolGuid = EFI_PARTITION_INFO_PROTOCOL_GUID;
EFI_GUID gEfiSimpleFileSystemProtocolGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;
#define STORE_SIZE 512U
#define FILE_SIZE (STORE_SIZE + sizeof (ESP_NV_HEADER))
typedef struct { UINT8 Disk[FILE_SIZE + 4]; UINTN Size; BOOLEAN Exists; } SNAPSHOT;
typedef struct {
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL Fs;
  EFI_PARTITION_INFO_PROTOCOL Partition;
  EFI_BLOCK_IO_PROTOCOL Block;
  EFI_BLOCK_IO_MEDIA Media;
  struct { EFI_DEVICE_PATH_PROTOCOL Node; UINT32 Key; EFI_DEVICE_PATH_PROTOCOL End; } Path;
  SNAPSHOT Files[2];
  BOOLEAN EfiDir;
  BOOLEAN CraneDir;
  BOOLEAN Present;
} VOLUME;
typedef struct {
  EFI_FILE_PROTOCOL Protocol;
  VOLUME *Volume;
  UINTN Kind;
  UINTN Position;
  UINT8 Work[FILE_SIZE + 4];
  UINTN Size;
  BOOLEAN Write;
} FILE_HANDLE;
STATIC VOLUME mVolumes[3];
STATIC BOOLEAN mRuntime, mNested, mWarning, mShort, mNoLocate, mReverse;
STATIC UINTN mStep, mFail, mReads, mWrites, mFsOps, mLiveHandles, mPools, mAllocations, mFailAllocation, mConnections;
STATIC EFI_TPL mTpl = TPL_APPLICATION;
STATIC UINT8 mStore[STORE_SIZE];
STATIC EFI_STATUS Step (VOID) {
  assert (!mRuntime && mTpl <= TPL_CALLBACK);
  ++mFsOps;
  if (++mStep == mFail) { return mWarning ? EFI_WARN_WRITE_FAILURE : EFI_DEVICE_ERROR; }
  return EFI_SUCCESS;
}
VOID *EFIAPI CopyMem (VOID *D, CONST VOID *S, UINTN N) { return memcpy (D,S,N); }
VOID *EFIAPI ZeroMem (VOID *D, UINTN N) { return memset (D,0,N); }
INTN EFIAPI CompareMem (CONST VOID *A, CONST VOID *B, UINTN N) { return memcmp (A,B,N); }
BOOLEAN EFIAPI CompareGuid (CONST GUID *A, CONST GUID *B) { return memcmp (A,B,16)==0; }
VOID *EFIAPI AllocatePool (UINTN N) { assert (!mRuntime); if (++mAllocations==mFailAllocation) return NULL; VOID *P=malloc(N);assert(P);++mPools;return P; }
VOID *EFIAPI AllocateCopyPool (UINTN N, CONST VOID *P) { VOID *D=AllocatePool(N);if(D)memcpy(D,P,N);return D; }
VOID EFIAPI FreePool (VOID *P) { assert(!mRuntime && P && mPools);--mPools;free(P); }
BOOLEAN EFIAPI EfiAtRuntime (VOID) { return mRuntime; }
EFI_TPL EFIAPI EfiGetCurrentTpl (VOID) { assert (!mRuntime);return mTpl; }
STATIC EFI_TPL EFIAPI Raise (EFI_TPL T) { assert(!mRuntime && T>=mTpl);EFI_TPL Old=mTpl;mTpl=T;return Old; }
STATIC VOID EFIAPI Restore (EFI_TPL T) { assert(!mRuntime && T<=mTpl);mTpl=T; }
BOOLEAN EFIAPI IsDevicePathValid (CONST EFI_DEVICE_PATH_PROTOCOL *P, UINTN N) { assert(N==ESP_NV_MAX_PATH);return P && P->Length[0]==8; }
UINTN EFIAPI GetDevicePathSize (CONST EFI_DEVICE_PATH_PROTOCOL *P) { assert(P);return sizeof(mVolumes[0].Path); }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN L) { (VOID)L;return FALSE; }
VOID EFIAPI DebugPrint (UINTN L, CONST CHAR8 *Format, ...) { (VOID)L;(VOID)Format; }
STATIC EFI_STATUS EFIAPI Close (EFI_FILE_PROTOCOL *P) { EFI_STATUS S=Step();assert(mLiveHandles);--mLiveHandles;free(P);return S; }
STATIC EFI_STATUS EFIAPI Delete (EFI_FILE_PROTOCOL *P) {
  FILE_HANDLE *F=(VOID*)P;EFI_STATUS S=Step();assert(F->Kind>=3 && F->Kind<=4);
  if(S==EFI_SUCCESS)memset(&F->Volume->Files[F->Kind-3],0,sizeof(SNAPSHOT));
  assert(mLiveHandles);--mLiveHandles;free(F);return S;
}
STATIC EFI_STATUS EFIAPI Read (EFI_FILE_PROTOCOL *P,UINTN *N,VOID *B) {
  FILE_HANDLE *F=(VOID*)P;EFI_STATUS S=Step();++mReads;if(S!=EFI_SUCCESS)return S;
  UINTN Count=MIN(*N,F->Size-F->Position);memcpy(B,F->Work+F->Position,Count);F->Position+=Count;*N=Count;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Write (EFI_FILE_PROTOCOL *P,UINTN *N,VOID *B) {
  FILE_HANDLE *F=(VOID*)P;EFI_STATUS S=Step();++mWrites;if(S!=EFI_SUCCESS)return S;
  assert(F->Write && F->Position+*N<=sizeof(F->Work));
  if(mNested){assert(VariablePersistenceBegin()==EFI_ACCESS_DENIED);assert(VariablePersistenceLoad(mStore,STORE_SIZE)==EFI_ACCESS_DENIED);}
  UINTN Count=*N;if(mShort && Count)Count--;memcpy(F->Work+F->Position,B,Count);F->Position+=Count;F->Size=MAX(F->Size,F->Position);*N=Count;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Seek (EFI_FILE_PROTOCOL *P, UINT64 N) { EFI_STATUS S=Step();if(S==EFI_SUCCESS){assert(N<=sizeof(((FILE_HANDLE*)P)->Work));((FILE_HANDLE*)P)->Position=(UINTN)N;}return S; }
STATIC EFI_STATUS EFIAPI Flush (EFI_FILE_PROTOCOL *P) {
  FILE_HANDLE *F=(VOID*)P;EFI_STATUS S=Step();if(S==EFI_SUCCESS && F->Kind>=3){SNAPSHOT *D=&F->Volume->Files[F->Kind-3];memcpy(D->Disk,F->Work,F->Size);D->Size=F->Size;D->Exists=TRUE;}return S;
}
STATIC EFI_STATUS EFIAPI Open (EFI_FILE_PROTOCOL *P,EFI_FILE_PROTOCOL **Out,CHAR16 *Name,UINT64 Mode,UINT64 Attr);
STATIC FILE_HANDLE *NewFile(VOLUME *V,UINTN Kind,UINT64 Mode) {
  FILE_HANDLE *F=calloc(1,sizeof(*F));assert(F);++mLiveHandles;F->Volume=V;F->Kind=Kind;F->Write=(Mode&EFI_FILE_MODE_WRITE)!=0;
  F->Protocol=(EFI_FILE_PROTOCOL){.Revision=EFI_FILE_PROTOCOL_REVISION,.Open=Open,.Close=Close,.Delete=Delete,.Read=Read,.Write=Write,.SetPosition=Seek,.Flush=Flush};
  if(Kind>=3){SNAPSHOT *D=&V->Files[Kind-3];memcpy(F->Work,D->Disk,D->Size);F->Size=D->Size;}
  return F;
}
STATIC EFI_STATUS EFIAPI Open (EFI_FILE_PROTOCOL *P,EFI_FILE_PROTOCOL **Out,CHAR16 *Name,UINT64 Mode,UINT64 Attr) {
  FILE_HANDLE *F=(VOID*)P;EFI_STATUS S=Step();if(S!=EFI_SUCCESS)return S;UINTN Kind=0;
  if(F->Kind==0 && Name[0]=='E'){Kind=1;if(!F->Volume->EfiDir && !(Mode&EFI_FILE_MODE_CREATE))return EFI_NOT_FOUND;F->Volume->EfiDir=TRUE;}
  else if(F->Kind==1 && Name[0]=='C'){Kind=2;if(!F->Volume->CraneDir && !(Mode&EFI_FILE_MODE_CREATE))return EFI_NOT_FOUND;F->Volume->CraneDir=TRUE;}
  else if(F->Kind==2 && Name[0]=='N'){Kind=3+(Name[6]=='1');if(!F->Volume->Files[Kind-3].Exists && !(Mode&EFI_FILE_MODE_CREATE))return EFI_NOT_FOUND;}
  else assert(0);
  assert((Kind<3 && (Attr==0 || Attr==EFI_FILE_DIRECTORY)) || (Kind>=3 && Attr==0));*Out=&NewFile(F->Volume,Kind,Mode)->Protocol;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI VolumeOpen (EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *P,EFI_FILE_PROTOCOL **Out) {
  EFI_STATUS S=Step();if(S==EFI_SUCCESS)*Out=&NewFile((VOLUME*)P,0,EFI_FILE_MODE_READ)->Protocol;return S;
}
STATIC EFI_STATUS EFIAPI HandleProtocol (EFI_HANDLE H,EFI_GUID *G,VOID **Out) {
  assert(!mRuntime && mTpl<=TPL_CALLBACK);VOLUME *V=H;assert(V>=mVolumes && V<mVolumes+3);if(!V->Present)return EFI_NOT_FOUND;
  if(G==&gEfiSimpleFileSystemProtocolGuid)*Out=&V->Fs;
  else if(G==&gEfiPartitionInfoProtocolGuid)*Out=&V->Partition;
  else if(G==&gEfiBlockIoProtocolGuid)*Out=&V->Block;
  else if(G==&gEfiDevicePathProtocolGuid)*Out=&V->Path;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Locate (EFI_LOCATE_SEARCH_TYPE Search,EFI_GUID *G,VOID *Key,UINTN *Count,EFI_HANDLE **Out) {
  assert(!mRuntime && mTpl<=TPL_CALLBACK && Search==ByProtocol && !Key);
  if(mNoLocate)return EFI_NOT_FOUND;
  if(G!=&gEfiSimpleFileSystemProtocolGuid && G!=&gEfiBlockIoProtocolGuid && G!=&gEmbeddedMmcHostProtocolGuid && G!=&gEfiExtScsiPassThruProtocolGuid && G!=&gEfiScsiPassThruProtocolGuid && G!=&gEdkiiUfsHostControllerProtocolGuid)return EFI_NOT_FOUND;
  *Count=3;*Out=AllocatePool(3*sizeof(EFI_HANDLE));if(!*Out)return EFI_OUT_OF_RESOURCES;
  (*Out)[0]=&mVolumes[mReverse?2:0];(*Out)[1]=&mVolumes[1];(*Out)[2]=&mVolumes[mReverse?0:2];return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Connect (EFI_HANDLE H,EFI_HANDLE *Driver,EFI_DEVICE_PATH_PROTOCOL *Remaining,BOOLEAN Recursive) {
  (VOID)H;assert(!mRuntime && !Driver && !Remaining && Recursive);++mConnections;return EFI_SUCCESS;
}
STATIC VOID FreshStore(UINT8 *Store,UINT8 Value) {
  memset(Store,0xff,STORE_SIZE);VARIABLE_STORE_HEADER *H=(VOID*)Store;H->Signature=gEfiAuthenticatedVariableGuid;H->Size=STORE_SIZE;H->Format=VARIABLE_STORE_FORMATTED;H->State=VARIABLE_STORE_HEALTHY;H->Reserved=H->Reserved1=0;
  AUTHENTICATED_VARIABLE_HEADER *V=(VOID*)(Store+28);memset(V,0,sizeof(*V));V->StartId=VARIABLE_DATA;V->State=VAR_ADDED;V->Attributes=VARIABLE_ATTRIBUTE_NV_BS_RT_AT;V->MonotonicCount=0x1234567890abcdefULL;V->TimeStamp.Year=2026;V->TimeStamp.Month=10;V->TimeStamp.Day=7;V->PubKeyIndex=42;V->NameSize=6;V->DataSize=16;memcpy((UINT8*)V+sizeof(*V),L"db",6);memset((UINT8*)V+sizeof(*V)+6,Value,16);assert(EspStoreValid(Store,STORE_SIZE));
}
STATIC VOID Dispose (VOID) {
  assert(!mLiveHandles);mRuntime=FALSE;mTpl=TPL_APPLICATION;
  if(mEspNv.Path)FreePool(mEspNv.Path);if(mEspNv.Work)FreePool(mEspNv.Work);if(mEspNv.LastStore)FreePool(mEspNv.LastStore);memset(&mEspNv,0,sizeof(mEspNv));assert(!mPools);
}
STATIC VOID Reset (BOOLEAN ClearDisk) {
  Dispose();mStep=mFail=mReads=mWrites=mFsOps=mAllocations=mFailAllocation=mConnections=0;mNested=mWarning=mShort=mNoLocate=mReverse=FALSE;
  if(ClearDisk){memset(mVolumes,0,sizeof(mVolumes));for(UINTN I=0;I<3;I++){VOLUME *V=&mVolumes[I];V->Fs=(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL){EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION,VolumeOpen};V->Partition.Revision=EFI_PARTITION_INFO_PROTOCOL_REVISION;V->Partition.Type=PARTITION_TYPE_GPT;V->Partition.System=1;V->Partition.Info.Gpt.PartitionTypeGUID=gEfiPartTypeSystemPartGuid;V->Partition.Info.Gpt.UniquePartitionGUID.Data1=(UINT32)(I+1);V->Media.MediaId=1;V->Media.MediaPresent=TRUE;V->Media.LogicalPartition=TRUE;V->Media.RemovableMedia=I==0;V->Block.Media=&V->Media;V->Path.Node.Length[0]=8;V->Path.Key=(UINT32)I;V->Path.End.Type=END_DEVICE_PATH_TYPE;V->Path.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;V->Path.End.Length[0]=4;V->Present=TRUE;}mVolumes[2].Partition.Type=PARTITION_TYPE_OTHER;}
  STATIC EFI_BOOT_SERVICES Bs={.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateHandleBuffer=Locate,.HandleProtocol=HandleProtocol,.ConnectController=Connect};gBS=&Bs;FreshStore(mStore,0x11);
}
STATIC VOID Load (VOID) { assert(VariablePersistenceLoad(mStore,STORE_SIZE)==EFI_SUCCESS);assert(!mEspNv.Busy && mEspNv.Loaded && !mLiveHandles); }
STATIC EFI_STATUS Commit (VOID) { assert(VariablePersistenceBegin()==EFI_SUCCESS);EFI_STATUS S=VariablePersistenceCommit(mStore,STORE_SIZE);VariablePersistenceEnd();assert(!mEspNv.Busy && !mLiveHandles);return S; }
STATIC VOID Seed (VOID) {Reset(TRUE);Load();assert(mEspNv.Handle==&mVolumes[1] && mConnections==15);assert(Commit()==EFI_SUCCESS && mEspNv.Generation==1 && mEspNv.Slot==0);}
STATIC VOID Rechecksum(SNAPSHOT *S){ESP_NV_HEADER *H=(VOID*)S->Disk;H->PayloadCrc=EspCrc(S->Disk+sizeof(*H),STORE_SIZE);H->HeaderCrc=0;H->HeaderCrc=EspCrc(H,sizeof(*H));}
int main(void) {
  Seed();UINT8 First[STORE_SIZE];memcpy(First,mStore,STORE_SIZE);FreshStore(mStore,0x22);mNested=TRUE;assert(Commit()==EFI_SUCCESS && mEspNv.Generation==2 && mEspNv.Slot==1);mNested=FALSE;Reset(FALSE);Load();assert(mEspNv.Generation==2 && ((AUTHENTICATED_VARIABLE_HEADER*)(mStore+28))->PubKeyIndex==42 && memcmp(mStore,mEspNv.LastStore,STORE_SIZE)==0);UINTN Writes=mWrites;assert(Commit()==EFI_SUCCESS && mWrites==Writes);
  // Fully CRC-valid hostile store bounds must still fall back to older snapshot.
  SNAPSHOT *New=&mVolumes[1].Files[1];AUTHENTICATED_VARIABLE_HEADER *V=(VOID*)(New->Disk+64+28);V->DataSize=MAX_UINT32;Rechecksum(New);Reset(FALSE);Load();assert(mEspNv.Generation==1 && memcmp(mStore,First,STORE_SIZE)==0);
  for(UINTN I=0;I<9;I++){Seed();FreshStore(mStore,0x22);assert(Commit()==EFI_SUCCESS);SNAPSHOT *S=&mVolumes[1].Files[1];ESP_NV_HEADER *H=(VOID*)S->Disk;V=(VOID*)(S->Disk+64+28);switch(I){case 0:H->HeaderCrc^=1;break;case 1:S->Disk[90]^=1;break;case 2:S->Size--;break;case 3:S->Size++;break;case 4:V->NameSize=MAX_UINT32;Rechecksum(S);break;case 5:V->NameSize=3;Rechecksum(S);break;case 6:S->Disk[64+28+sizeof(*V)+4]=1;Rechecksum(S);break;case 7:S->Disk[FILE_SIZE-1]=0;Rechecksum(S);break;case 8:H->Generation=0;H->HeaderCrc=0;H->HeaderCrc=EspCrc(H,sizeof(*H));break;}Reset(FALSE);Load();assert(mEspNv.Generation==1 && memcmp(mStore,First,STORE_SIZE)==0);}
  // Every fallible filesystem operation in actual commit: preserve active slot,
  // do not acknowledge generation, and release every definitely live handle.
  Seed();FreshStore(mStore,0x22);mStep=0;assert(Commit()==EFI_SUCCESS);UINTN Phases=mStep;
  for(UINTN Warn=0;Warn<2;Warn++)for(UINTN I=1;I<=Phases;I++){Seed();SNAPSHOT Old=mVolumes[1].Files[0];FreshStore(mStore,0x22);mStep=0;mFail=I;mWarning=Warn;EFI_STATUS S=Commit();assert(EFI_ERROR(S) && mEspNv.Generation==1 && mEspNv.Slot==0 && memcmp(&Old,&mVolumes[1].Files[0],sizeof(Old))==0);mFail=0;if(!mEspNv.FailedClose){assert(Commit()==EFI_SUCCESS && mEspNv.Generation==2);}else{UINTN Ops=mFsOps;assert(Commit()==EFI_DEVICE_ERROR && mFsOps==Ops);}}
  Seed();FreshStore(mStore,0x22);assert(Commit()==EFI_SUCCESS);FreshStore(mStore,0x33);mStep=0;assert(Commit()==EFI_SUCCESS);UINTN ReusePhases=mStep;
  for(UINTN Warn=0;Warn<2;Warn++)for(UINTN I=1;I<=ReusePhases;I++){Seed();FreshStore(mStore,0x22);assert(Commit()==EFI_SUCCESS);SNAPSHOT Old=mVolumes[1].Files[1];FreshStore(mStore,0x33);mStep=0;mFail=I;mWarning=Warn;EFI_STATUS S=Commit();assert(EFI_ERROR(S) && mEspNv.Generation==2 && mEspNv.Slot==1 && memcmp(&Old,&mVolumes[1].Files[1],sizeof(Old))==0);mFail=0;if(!mEspNv.FailedClose)assert(Commit()==EFI_SUCCESS && mEspNv.Generation==3);}
  // No partially read store is ever copied over formatted defaults. Fail each
  // read/close in two valid files, including warning statuses and freed handles.
  Seed();FreshStore(mStore,0x22);assert(Commit()==EFI_SUCCESS);Reset(FALSE);Load();UINTN ReadPhases=mStep;
  for(UINTN Warn=0;Warn<2;Warn++)for(UINTN I=1;I<=ReadPhases;I++){Seed();FreshStore(mStore,0x22);assert(Commit()==EFI_SUCCESS);Reset(FALSE);mFail=I;mWarning=Warn;Load();assert(EspStoreValid(mStore,STORE_SIZE) && mEspNv.Generation<=2);if(mEspNv.HaveSnapshot)assert(memcmp(mStore,mEspNv.LastStore,STORE_SIZE)==0);else assert(memcmp(mStore,First,STORE_SIZE)==0);}
  Seed();V=(VOID*)(mStore+28);V->NameSize=2;*((CHAR16*)((UINT8*)V+sizeof(*V)))=0;assert(!EspStoreValid(mStore,STORE_SIZE));assert(Commit()==EFI_INVALID_PARAMETER);
  // Selection is independent of enumeration order; internal wins over lower
  // removable paths, and exact path ties use the GPT partition identity.
  Reset(TRUE);mVolumes[2].Partition.Type=PARTITION_TYPE_GPT;mVolumes[2].Path.Key=mVolumes[1].Path.Key;mVolumes[2].Partition.Info.Gpt.UniquePartitionGUID.Data1=0x1000;mVolumes[1].Partition.Info.Gpt.UniquePartitionGUID.Data1=0x2000;Load();assert(mEspNv.Handle==&mVolumes[2]);Reset(FALSE);mReverse=TRUE;Load();assert(mEspNv.Handle==&mVolumes[2]);
  Seed();FreshStore(mStore,0x22);mShort=TRUE;assert(Commit()==EFI_DEVICE_ERROR && mEspNv.Generation==1);mShort=FALSE;assert(Commit()==EFI_SUCCESS);
  Seed();FreshStore(mStore,0x22);mVolumes[1].Present=FALSE;UINTN Ops=mFsOps;assert(Commit()==EFI_NO_MEDIA && mEspNv.Handle==&mVolumes[1] && mFsOps==Ops);
  Seed();FreshStore(mStore,0x22);mVolumes[1].Media.MediaId++;assert(Commit()==EFI_NO_MEDIA && mEspNv.Generation==1);
  Seed();FreshStore(mStore,0x22);mVolumes[1].Media.ReadOnly=TRUE;assert(Commit()==EFI_WRITE_PROTECTED && mEspNv.Generation==1);
  Reset(TRUE);for(UINTN I=0;I<3;I++)mVolumes[I].Partition.Type=PARTITION_TYPE_OTHER;Load();assert(!mEspNv.Selected && memcmp(mStore,First,STORE_SIZE)==0);Ops=mFsOps;assert(Commit()==EFI_SUCCESS && mFsOps==Ops && !mWrites);
  Reset(TRUE);mNoLocate=TRUE;Load();assert(!mEspNv.Selected);assert(Commit()==EFI_SUCCESS);
  CONST UINTN AllocFaults[]={1,2,8,9,10};for(UINTN I=0;I<ARRAY_SIZE(AllocFaults);I++){Reset(TRUE);mFailAllocation=AllocFaults[I];assert(VariablePersistenceLoad(mStore,STORE_SIZE)==EFI_OUT_OF_RESOURCES);assert(!mEspNv.Busy && !mEspNv.Loaded);mFailAllocation=0;Load();}
  Seed();mTpl=TPL_NOTIFY;Ops=mFsOps;assert(VariablePersistenceBegin()==EFI_UNSUPPORTED && !mEspNv.Busy);assert(VariablePersistenceCommit(mStore,STORE_SIZE)==EFI_UNSUPPORTED && mFsOps==Ops);mTpl=TPL_APPLICATION;
  assert(VariablePersistenceBegin()==EFI_SUCCESS);assert(VariablePersistenceBegin()==EFI_ACCESS_DENIED);VariablePersistenceEnd();
  // Poison boot pointers to demonstrate runtime returns before touching them.
  mRuntime=TRUE;EFI_BOOT_SERVICES *Saved=gBS;gBS=(VOID*)1;assert(VariablePersistenceBegin()==EFI_SUCCESS);assert(VariablePersistenceCommit((VOID*)1,MAX_UINTN)==EFI_SUCCESS);VariablePersistenceEnd();assert(VariablePersistenceLoad((VOID*)1,MAX_UINTN)==EFI_UNSUPPORTED);gBS=Saved;mRuntime=FALSE;
  Seed();mEspNv.Generation=MAX_UINT64;FreshStore(mStore,0x22);assert(Commit()==EFI_OUT_OF_RESOURCES);
  // Bounded random byte/length corruption executes real parser under sanitizers.
  UINT32 Random=1;for(UINTN I=0;I<10000;I++){FreshStore(mStore,0x11);Random=Random*1664525U+1013904223U;mStore[Random%STORE_SIZE]^=(UINT8)(Random>>24);(VOID)EspStoreValid(mStore,STORE_SIZE);}
  Dispose();printf("ESP persistence actual backend PASS: %zu fresh + %zu reuse commit and %zu read fault phases x2, CRC/structural fallback, exact auth metadata, deterministic GPT identity, no-ESP/runtime/TPL/reentry gates\n",(size_t)Phases,(size_t)ReusePhases,(size_t)ReadPhases);return 0;
}
