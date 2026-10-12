/* Actual emulated-store initialization/parsing and DXE commit adapter. MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define _PCD_GET_MODE_32_PcdVariableStoreSize 4096U
#define _PCD_GET_MODE_32_PcdHwErrStorageSize 0U
#define _PCD_GET_MODE_32_PcdMaxVariableSize 1024U
#define _PCD_GET_MODE_32_PcdMaxAuthVariableSize 1024U
#define _PCD_GET_MODE_32_PcdMaxHardwareErrorVariableSize 1024U
#define _PCD_GET_MODE_32_PcdMaxUserNvVariableSpaceSize 0U
#define _PCD_GET_MODE_32_PcdBoottimeReservedNvVariableSpaceSize 0U
#define _PCD_GET_MODE_64_PcdEmuVariableNvStoreReserved ReservedStore
#define _PCD_GET_MODE_BOOL_PcdEmuVariableNvModeEnable TRUE
#define _PCD_GET_MODE_BOOL_PcdVariableCollectStatistics FALSE
static UINT64 ReservedStore;
#include "../Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe/VariableNonVolatile.c"
#include "../Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe/VariableParsing.c"
#include "../Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe/VariablePersistence.c"

VARIABLE_MODULE_GLOBAL Global;
VARIABLE_MODULE_GLOBAL *mVariableModuleGlobal=&Global;
VARIABLE_STORE_HEADER *mNvVariableCache;
EFI_FIRMWARE_VOLUME_HEADER *mNvFvHeaderCache;
VARIABLE_INFO_ENTRY *gVariableInfo;
EFI_GUID gEfiAuthenticatedVariableGuid=EFI_AUTHENTICATED_VARIABLE_GUID;
EFI_GUID gEfiVariableGuid=EFI_VARIABLE_GUID;
EFI_GUID gEfiSystemNvDataFvGuid=EFI_SYSTEM_NV_DATA_FV_GUID;
EFI_GUID gEdkiiFaultTolerantWriteGuid=EDKII_FAULT_TOLERANT_WRITE_GUID;
static UINTN Allocations,Frees,Loads,Begins,Ends,Commits,Mutations;
static EFI_STATUS LoadStatus,BeginStatus,CoreStatus,CommitStatus;
static BOOLEAN RestoreRecord,Admission,CoreLocked,FailAllocation;

VOID *EFIAPI AllocateRuntimePool(UINTN N){if(FailAllocation)return NULL;++Allocations;return malloc(N);}
VOID EFIAPI FreePool(VOID *P){++Frees;free(P);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
EFI_GUID *EFIAPI CopyGuid(EFI_GUID *D,CONST EFI_GUID *S){memcpy(D,S,sizeof(*D));return D;}
BOOLEAN EFIAPI CompareGuid(CONST EFI_GUID *A,CONST EFI_GUID *B){return memcmp(A,B,sizeof(*A))==0;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"%s:%zu %s\n",F,(size_t)L,D);abort();}

EFI_STATUS EFIAPI VariablePersistenceLoad(VOID *P,UINTN N){
  VARIABLE_STORE_HEADER *H=P;++Loads;
  assert(N==4096 && H->Size==4096 && H->Format==VARIABLE_STORE_FORMATTED);
  assert(mNvVariableCache==NULL && Global.NonVolatileLastVariableOffset==0);
  if(RestoreRecord){
    AUTHENTICATED_VARIABLE_HEADER *V=(VOID *)((UINT8 *)P+sizeof(*H));
    memset(V,0,sizeof(*V));V->StartId=VARIABLE_DATA;V->State=VAR_ADDED;
    V->Attributes=EFI_VARIABLE_NON_VOLATILE|EFI_VARIABLE_BOOTSERVICE_ACCESS;
    V->NameSize=4;V->DataSize=4;V->MonotonicCount=123;V->TimeStamp.Year=2026;
    UINT8 *Data=(VOID *)(V+1);Data[0]='A';Data[1]=Data[2]=Data[3]=0;
    memset(Data+4,0x51,4);
  }
  return LoadStatus;
}
EFI_STATUS EFIAPI VariablePersistenceBegin(VOID){++Begins;if(BeginStatus==EFI_SUCCESS){assert(!Admission);Admission=TRUE;}return BeginStatus;}
EFI_STATUS EFIAPI VariablePersistenceCommit(CONST VOID *P,UINTN N){
  assert(Admission&&!CoreLocked&&P==mNvVariableCache&&N==4096);++Commits;return CommitStatus;
}
VOID EFIAPI VariablePersistenceEnd(VOID){assert(Admission);Admission=FALSE;++Ends;}
EFI_STATUS EFIAPI VariableServiceSetVariable(CHAR16 *Name,EFI_GUID *Guid,UINT32 Attr,UINTN N,VOID *Data){
  (VOID)Name;(VOID)Guid;(VOID)Attr;(VOID)N;(VOID)Data;
  assert(Admission);CoreLocked=TRUE;++Mutations;CoreLocked=FALSE;return CoreStatus;
}
static void Reset(void){
  memset(&Global,0,sizeof(Global));mNvVariableCache=NULL;
  ReservedStore=0;Allocations=Frees=Loads=Begins=Ends=Commits=Mutations=0;
  RestoreRecord=Admission=CoreLocked=FailAllocation=FALSE;
  LoadStatus=BeginStatus=CoreStatus=CommitStatus=EFI_SUCCESS;
  mVariableEmuStoreLoad=VariablePersistenceLoad;
}
int main(void){
  Reset();RestoreRecord=TRUE;
  assert(InitNonVolatileVariableStore()==EFI_SUCCESS && Loads==1);
  assert(Global.VariableGlobal.AuthFormat&&Global.VariableGlobal.EmuNvMode);
  assert(Global.NonVolatileLastVariableOffset==sizeof(VARIABLE_STORE_HEADER)+sizeof(AUTHENTICATED_VARIABLE_HEADER)+8);
  assert(Global.CommonVariableTotalSize==sizeof(AUTHENTICATED_VARIABLE_HEADER)+8);
  AUTHENTICATED_VARIABLE_HEADER *V=(VOID *)(mNvVariableCache+1);
  assert(V->MonotonicCount==123&&V->TimeStamp.Year==2026);
  assert(VariablePersistenceSaveCurrentStore()==EFI_SUCCESS&&Commits==1&&Ends==1);
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_SUCCESS&&Mutations==1&&Commits==2&&Ends==2);
  CoreStatus=EFI_SECURITY_VIOLATION;
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_SECURITY_VIOLATION&&Mutations==2&&Commits==2&&Ends==3);
  CoreStatus=EFI_SUCCESS;CommitStatus=EFI_VOLUME_FULL;
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_VOLUME_FULL&&Mutations==3&&Commits==3&&Ends==4);
  CommitStatus=1;
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_DEVICE_ERROR&&Ends==5);
  BeginStatus=EFI_NOT_READY;
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_NOT_READY&&Mutations==4&&Ends==5);
  BeginStatus=1;
  assert(VariableServiceSetVariableWithPersistence(L"A",&gEfiVariableGuid,3,0,NULL)==EFI_NOT_READY&&Mutations==4&&Ends==5);
  FreePool(mNvVariableCache);
  Reset();mVariableEmuStoreLoad=NULL;
  assert(InitNonVolatileVariableStore()==EFI_SUCCESS&&Loads==0&&Global.NonVolatileLastVariableOffset==sizeof(VARIABLE_STORE_HEADER));
  FreePool(mNvVariableCache);
  Reset();LoadStatus=EFI_DEVICE_ERROR;
  assert(InitNonVolatileVariableStore()==EFI_DEVICE_ERROR&&Allocations==1&&Frees==1&&mNvVariableCache==NULL);
  Reset();LoadStatus=1;
  assert(InitNonVolatileVariableStore()==EFI_DEVICE_ERROR&&Frees==1&&mNvVariableCache==NULL);
  Reset();FailAllocation=TRUE;
  assert(InitNonVolatileVariableStore()==EFI_OUT_OF_RESOURCES&&Loads==0&&Allocations==0);
  puts("Actual variable restore/parser and completed-transaction adapter: metadata, quota, failures and no premature commit PASS");
  return 0;
}
