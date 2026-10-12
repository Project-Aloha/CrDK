/* Actual RAM early-storage backend; modeled mapping and MMIO. MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Uefi.h>
#include <Library/MsWheaEarlyStorageLib.h>
#include <Library/MemoryMapHelperLib.h>
static UINT32 Capacity=256;
static UINT64 Base=0x100000, Length=0x8000;
static EFI_STATUS LookupStatus;
static UINTN Lookups, Reads, Writes, InvalidAccess;
static UINT8 Ram[256];
UINT32 EFIAPI LibPcdGet32(UINTN Token){(void)Token;return Capacity;}
UINT8 EFIAPI LibPcdGet8(UINTN Token){(void)Token;return 0xff;}
EFI_STATUS EFIAPI LocateMemoryMapAreaByName(CHAR8 *Name,ARM_MEMORY_REGION_DESCRIPTOR_EX *D){
 assert(!strcmp(Name,"Log Buffer"));++Lookups;memset(D,0,sizeof(*D));D->Address=Base;D->Length=Length;return LookupStatus;
}
UINT8 EFIAPI MmioRead8(UINTN A){++Reads;if(A<Base || A-Base>=Capacity || A-Base>=sizeof(Ram)){++InvalidAccess;return 0;}return Ram[A-Base];}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V){++Writes;if(A<Base || A-Base>=Capacity || A-Base>=sizeof(Ram)){++InvalidAccess;return V;}Ram[A-Base]=V;return V;}
VOID *EFIAPI ZeroMem(VOID *B,UINTN N){return memset(B,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
int main(int argc,char **argv){
 assert(argc==2);int N=atoi(argv[1]);UINT8 B[255]={0x5a};EFI_STATUS S;
 if(N==0){S=MsWheaEarlyStorageWrite(B,1,0);assert(S==EFI_SUCCESS&&Lookups==1&&Writes==1&&Ram[0x40]==0x5a&&!InvalidAccess);}
 if(N==1){S=MsWheaEarlyStorageClear(1,0);assert(S==EFI_SUCCESS&&Lookups==1&&Writes==1&&Ram[0x40]==0xff&&!InvalidAccess);}
 if(N==2){Capacity=80;assert(MsWheaEarlyStorageWrite(B,100,0)==EFI_INVALID_PARAMETER&&!Writes);}
 if(N==3){Length=128;assert(MsWheaEarlyStorageWrite(B,1,0)==EFI_BAD_BUFFER_SIZE&&!Writes);}
 if(N==4){LookupStatus=EFI_NOT_FOUND;assert(MsWheaEarlyStorageClear(1,0)==EFI_NOT_FOUND&&!Writes);}
 if(N==5){Base=0;assert(MsWheaEarlyStorageWrite(B,1,0)!=EFI_SUCCESS&&!Writes);}
 if(N==6){Base=MAX_UINT64-127;assert(MsWheaEarlyStorageWrite(B,1,0)!=EFI_SUCCESS&&!Writes);}
 if(N==7){Capacity=512;assert(MsWheaEarlyStorageGetMaxSize()==0&&!Reads&&!Writes);}
 if(N==8){assert(MsWheaEarlyStorageWrite(B,2,191)==EFI_INVALID_PARAMETER&&!Writes);assert(MsWheaEarlyStorageWrite(B,1,191)==EFI_SUCCESS&&Ram[255]==0x5a&&!InvalidAccess);}
 if(N==9){LookupStatus=EFI_WARN_UNKNOWN_GLYPH;assert(MsWheaEarlyStorageRead(B,1,0)==EFI_NOT_READY&&!Reads);}
 if(N==10){LookupStatus=EFI_NOT_FOUND;assert(MsWheaEarlyStorageRead(B,1,0)==EFI_NOT_FOUND);LookupStatus=EFI_SUCCESS;assert(MsWheaEarlyStorageWrite(B,1,0)==EFI_SUCCESS&&Lookups==2&&Ram[64]==0x5a);}
 if(N==11){assert(MsWheaEarlyStorageRead(NULL,1,0)==EFI_INVALID_PARAMETER&&!Reads&&!Lookups);assert(MsWheaEarlyStorageClear(0,0)==EFI_INVALID_PARAMETER&&!Writes&&!Lookups);}
 printf("early storage range case %d PASS\n",N);return 0;
}
