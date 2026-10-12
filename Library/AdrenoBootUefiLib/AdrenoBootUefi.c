/* Native early stopped-route producer, GPL-2.0-only. Does not start firmware. */
#include <PiDxe.h>
#include <Library/AdrenoBootUefiLib.h>
#include <Library/AdrenoBootCrLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/BaseLib.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <Protocol/EFICrAdrenoBootAccess.h>
static EFI_GUID AccessGuid=EFI_CR_ADRENO_BOOT_ACCESS_GUID;
typedef struct _NATIVE NATIVE;
typedef struct {NATIVE *Owner;} BACKEND;
struct _NATIVE {
 BACKEND Backend;
 EFI_CR_ADRENO_BOOT_ACCESS *Access;CR_ADRENO_BOOT_ACCESS_LEASE Lease;BOOLEAN Owned,Attempted,Committed;
 CR_SM8450_GPU_LAYOUT Layout;CR_ADRENO_STOP_IO StopIo;CR_ADRENO_STOP_CONTEXT Stop;
 CR_ADRENO_BOOT_IO Io;CR_ADRENO_BOOT_CONTEXT Boot;CR_ADRENO_ROUTE_FACTS Route;
 EFI_PHYSICAL_ADDRESS RecordPage;CR_ADRENO_HANDOFF_RECORD *Record;
 VOID *Iort;UINT32 IortBytes;UINTN LocatorKey,IortKey;
};
/* AcpiPlatform requests image unload. The permanent ClockCrDxe identity and
 * partial-effect journal therefore live outside this library image. No IO
 * callback is published or scheduled; every operation ends before return. */
static NATIVE *mNative;
static BOOLEAN mAttempted;
static EFI_STATUS Final(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_DEVICE_ERROR;}
static UINT32 Le32(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static BOOLEAN Name(CONST UINT8 *P,UINTN N,CONST CHAR8 *S){UINTN L=AsciiStrLen(S)+1;return N>=L&&CompareMem(P,S,L)==0;}
static EFI_STATUS Candidate(CONST UINT8 *B,UINTN N,BOOLEAN *Yes){
 if(N<48||N>65536||Le32(B)!=SIGNATURE_32('I','O','R','T')||Le32(B+4)!=N)return EFI_COMPROMISED_DATA;
 // Private Qualcomm rev0 remains unrelated; this unit only accepts standard5.
 if(B[8]!=5){*Yes=FALSE;return EFI_SUCCESS;}
 UINT32 Count=Le32(B+36),At=Le32(B+40);if(!Count||Count>64||At<48||At>N)return EFI_COMPROMISED_DATA;
 BOOLEAN Found=FALSE;for(UINT32 I=0;I<Count;++I){if(At>N-16)return EFI_COMPROMISED_DATA;
 UINT32 Size=B[At+1]|((UINT32)B[At+2]<<8);if(Size<16||Size>N-At)return EFI_COMPROMISED_DATA;
 if(B[At]==3&&Size>=32&&Le32(B+At+16)==0x03da0000&&Le32(B+At+20)==0)Found=TRUE;
 if(B[At]==1&&Size>29&&(Name(B+At+29,Size-29,CR_ADRENO_ROUTE_CP_PATH)||Name(B+At+29,Size-29,CR_ADRENO_ROUTE_GMU_PATH)))Found=TRUE;
 At+=Size;}
 if(At!=N)return EFI_COMPROMISED_DATA;*Yes=Found;return EFI_SUCCESS;
}
static CR_STATUS Check(VOID *P,UINT64 Epoch){BACKEND *B=P;NATIVE *N=B?B->Owner:NULL;
 if(!mNative||B!=&mNative->Backend||N!=mNative||!N->Owned||N->Committed||!Epoch||Epoch!=N->Lease.Incarnation)return CR_NOT_READY;
 return (CR_STATUS)Final(N->Access->Check(N->Access,N,&N->Lease));
}
static CR_STATUS Read(VOID *P,UINT64 E,UINT32 At,UINT32 *V){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;
 if((At&3)||At>0x3fffc||!V)return CR_INVALID_PARAMETER;*V=MmioRead32(0x03da0000+At);return Check(P,E);}
static CR_STATUS Write(VOID *P,UINT64 E,UINT32 At,UINT32 V){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;
 if((At&3)||At>0x3fffc)return CR_INVALID_PARAMETER;MmioWrite32(0x03da0000+At,V);return Check(P,E);}
static CR_STATUS Barrier(VOID *P,UINT64 E){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;MemoryFence();return Check(P,E);}
static CR_STATUS Delay(VOID *P,UINT64 E,UINT32 Us){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;
 if(Us>1000)return CR_INVALID_PARAMETER;MicroSecondDelay(Us);return Check(P,E);}
static CR_STATUS StopAddress(NATIVE *N,CR_ADRENO_STOP_REGISTER Reg,BOOLEAN Writing,UINTN *At){
 CR_ADRENO_STOP_REGISTER_INFO Info;CR_SM8450_GPU_REGISTER_SPAN Span;
 CR_STATUS S=CrAdrenoSm8450StopDescribe(Reg,&Info);if(S!=CR_SUCCESS)return S;
 if(!(Info.Access&(Writing?CR_ADRENO_STOP_REG_WRITE:CR_ADRENO_STOP_REG_READ)))return CR_UNSUPPORTED;
 S=CrAdrenoSm8450StopResolve(&N->Layout,Reg,&Span);if(S!=CR_SUCCESS)return S;
 if(Span.Relation!=CR_SM8450_GPU_RELATION_DEDICATED||Span.Bytes!=4||Span.View==CrSm8450GpuViewGpucc)return CR_UNSUPPORTED;
 // GX-dependent registers are touched only after fresh powered branch evidence.
 if(Info.Access&CR_ADRENO_STOP_REG_GX){CR_SM8450_GPU_REGISTER_SPAN Gx;
 S=CrAdrenoSm8450StopResolve(&N->Layout,CrAdrenoStopGxStatus,&Gx);if(S!=CR_SUCCESS)return S;
 if(MmioRead32((UINTN)Gx.PhysicalAddress)&3)return CR_NOT_READY;}
 *At=(UINTN)Span.PhysicalAddress;return CR_SUCCESS;
}
static CR_STATUS StopRead(VOID *P,UINT64 E,CR_ADRENO_STOP_REGISTER R,UINT32 *V){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;
 UINTN At;S=StopAddress(((BACKEND *)P)->Owner,R,FALSE,&At);if(S!=CR_SUCCESS)return S;*V=MmioRead32(At);return Check(P,E);}
static CR_STATUS StopWrite(VOID *P,UINT64 E,CR_ADRENO_STOP_REGISTER R,UINT32 V){CR_STATUS S=Check(P,E);if(S!=CR_SUCCESS)return S;
 UINTN At;S=StopAddress(((BACKEND *)P)->Owner,R,TRUE,&At);if(S!=CR_SUCCESS)return S;MmioWrite32(At,V);return Check(P,E);}
static EFI_STATUS MmioSpan(UINT64 Base,UINT64 Bytes){
 EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=gDS->GetMemorySpaceDescriptor(Base,&D);if(S!=EFI_SUCCESS)return Final(S);
 if(D.GcdMemoryType!=EfiGcdMemoryTypeMemoryMappedIo||D.BaseAddress>Base||D.Length<Bytes||
 Base-D.BaseAddress>D.Length-Bytes)return EFI_UNSUPPORTED;
 return EFI_SUCCESS;
}
static EFI_STATUS Reserved(EFI_PHYSICAL_ADDRESS Page){
 EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=gDS->GetMemorySpaceDescriptor(Page,&D);if(S!=EFI_SUCCESS)return Final(S);
 if(D.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory||D.BaseAddress>Page||D.Length<4096||Page-D.BaseAddress>D.Length-4096)return EFI_UNSUPPORTED;
 UINTN Bytes=0,Key=0,Step=0;UINT32 Version=0;S=gBS->GetMemoryMap(&Bytes,NULL,&Key,&Step,&Version);
 if(S!=EFI_BUFFER_TOO_SMALL||Step<sizeof(EFI_MEMORY_DESCRIPTOR)||!Bytes||Bytes>1024*1024)return EFI_DEVICE_ERROR;
 if(Step>4096||Bytes>1024*1024-8*Step)return EFI_COMPROMISED_DATA;
 Bytes+=8*Step;UINTN Capacity=Bytes;EFI_MEMORY_DESCRIPTOR *Map=AllocatePool(Bytes);if(!Map)return EFI_OUT_OF_RESOURCES;
 S=gBS->GetMemoryMap(&Bytes,Map,&Key,&Step,&Version);if(S!=EFI_SUCCESS){FreePool(Map);return Final(S);}
 if(Step<sizeof(EFI_MEMORY_DESCRIPTOR)||Step>4096||Bytes>Capacity||Bytes%Step||Version!=EFI_MEMORY_DESCRIPTOR_VERSION){FreePool(Map);return EFI_COMPROMISED_DATA;}
 UINT32 Matches=0;for(UINTN At=0;At+Step<=Bytes;At+=Step){EFI_MEMORY_DESCRIPTOR *M=(VOID *)((UINT8 *)Map+At);
 if(M->NumberOfPages>MAX_UINT64/4096||M->PhysicalStart>MAX_UINT64-M->NumberOfPages*4096){S=EFI_COMPROMISED_DATA;break;}
 UINT64 End=M->PhysicalStart+M->NumberOfPages*4096;if(Page<End&&M->PhysicalStart<Page+4096){
 if(M->Type!=EfiReservedMemoryType||M->PhysicalStart>Page||End<Page+4096){S=EFI_UNSUPPORTED;break;}++Matches;}}
 FreePool(Map);return S==EFI_SUCCESS&&Matches==1?EFI_SUCCESS:S==EFI_SUCCESS?EFI_UNSUPPORTED:S;
}
static EFI_STATUS NoExistingGpuOrIort(VOID){EFI_ACPI_SDT_PROTOCOL *Sdt=NULL;
 EFI_STATUS S=gBS->LocateProtocol(&gEfiAcpiSdtProtocolGuid,NULL,(VOID **)&Sdt);if(S!=EFI_SUCCESS||!Sdt||!Sdt->GetAcpiTable)return S==EFI_SUCCESS?EFI_UNSUPPORTED:Final(S);
 for(UINTN I=0;I<256;++I){EFI_ACPI_SDT_HEADER *T=NULL;EFI_ACPI_TABLE_VERSION V=0;UINTN Key=0;S=Sdt->GetAcpiTable(I,&T,&V,&Key);
 if(S==EFI_NOT_FOUND)return EFI_SUCCESS;if(S!=EFI_SUCCESS||!T)return S==EFI_SUCCESS?EFI_COMPROMISED_DATA:Final(S);
 if(T->Signature==SIGNATURE_32('I','O','R','T')||T->Signature==CR_ADRENO_GPUH_SIGNATURE)return EFI_ALREADY_STARTED;}
 return EFI_OUT_OF_RESOURCES;
}
EFI_STATUS CrAdrenoBootInstallIort(EFI_ACPI_TABLE_PROTOCOL *A,CONST VOID *Table,UINTN Bytes,BOOLEAN *Handled,UINTN *Key){
 if(!A||!A->InstallAcpiTable||!Table||!Handled||!Key)return EFI_INVALID_PARAMETER;
 *Handled=TRUE;BOOLEAN Found=FALSE;EFI_STATUS S=Candidate(Table,Bytes,&Found);if(S!=EFI_SUCCESS)return S;
 if(!Found){*Handled=FALSE;return EFI_SUCCESS;}
 if(mAttempted)return EFI_ALREADY_STARTED;mAttempted=TRUE;
 NATIVE *N=AllocateZeroPool(sizeof(*N));if(!N)return EFI_OUT_OF_RESOURCES;mNative=N;N->Attempted=TRUE;
 S=NoExistingGpuOrIort();if(S!=EFI_SUCCESS)return S;
 N->Iort=AllocateCopyPool(Bytes,Table);if(!N->Iort)return EFI_OUT_OF_RESOURCES;N->IortBytes=(UINT32)Bytes;
 S=(EFI_STATUS)CrAdrenoRouteParseIort(N->Iort,N->IortBytes,&N->Route);if(S!=EFI_SUCCESS)return Final(S);
 static CONST CR_SM8450_GPU_VIEW_DESCRIPTOR Views[]={
 {1,1,0x03d00000,0x40000},{2,1,0x03d61000,0x800},{3,1,0x03d6a000,0x26000},
 {4,1,0x03d9a000,0x5000},{5,1,0x03d50000,0x10000},{6,1,0x0b290000,0x10000},{7,2,0x03d90000,0xa000}};
 S=(EFI_STATUS)CrSm8450GpuResourcesBuild(Views,7,&N->Layout);if(S!=EFI_SUCCESS)return Final(S);
 for(UINT32 I=0;I<7;++I){S=MmioSpan(Views[I].Base,Views[I].Length);if(S!=EFI_SUCCESS)return S;}
 S=MmioSpan(0x03da0000,0x40000);if(S!=EFI_SUCCESS)return S;
 S=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,1,&N->RecordPage);if(S!=EFI_SUCCESS)return Final(S);
 S=Reserved(N->RecordPage);if(S!=EFI_SUCCESS)return S;N->Record=(VOID *)(UINTN)N->RecordPage;ZeroMem(N->Record,4096);
 S=gBS->LocateProtocol(&AccessGuid,NULL,(VOID **)&N->Access);if(S!=EFI_SUCCESS)return Final(S);
 if(!N->Access||N->Access->Revision!=1||!N->Access->Begin||!N->Access->Check||!N->Access->Commit)return EFI_UNSUPPORTED;
 S=N->Access->Begin(N->Access,N,&N->Lease,&N->Owned);if(S!=EFI_SUCCESS||!N->Owned)return S==EFI_SUCCESS?EFI_DEVICE_ERROR:Final(S);
 N->Backend.Owner=N;
 N->StopIo=(CR_ADRENO_STOP_IO){&N->Backend,sizeof(N->Backend),N->Lease.Incarnation,StopRead,StopWrite,Barrier,Delay};
 S=(EFI_STATUS)CrAdrenoStopInit(&N->Stop,&N->StopIo,N->Lease.Incarnation);if(S!=EFI_SUCCESS)return Final(S);
 N->Io=(CR_ADRENO_BOOT_IO){&N->Backend,sizeof(N->Backend),N->Lease.Incarnation,Check,Read,Write,Barrier,Delay};
 S=(EFI_STATUS)CrAdrenoBootInit(&N->Boot,&N->Io,&N->Stop);if(S!=EFI_SUCCESS)return Final(S);
 S=(EFI_STATUS)CrAdrenoBootRetire(&N->Boot);if(S!=EFI_SUCCESS)return Final(S);
 S=(EFI_STATUS)CrAdrenoBootRecord(&N->Boot,&N->Route,N->RecordPage,N->Record);if(S!=EFI_SUCCESS)return Final(S);
 S=N->Access->Commit(N->Access,N,&N->Lease);if(S!=EFI_SUCCESS)return Final(S);N->Committed=TRUE;
 // The immutable record and all hardware ownership are final BEFORE either
 // Windows-visible table. On any install uncertainty retain, never uninstall.
 MemoryFence();CR_ADRENO_HANDOFF_LOCATOR L={0};L.Signature=CR_ADRENO_GPUH_SIGNATURE;L.Length=sizeof(L);L.Revision=1;
 CopyMem(L.OemId,"ALOHA ",6);CopyMem(L.OemTableId,"GPUHDK84",8);L.OemRevision=1;L.CreatorId=SIGNATURE_32('A','L','H','A');L.CreatorRevision=1;
 L.WireVersion=1;L.RecordPhysical=N->RecordPage;L.RecordBytes=sizeof(*N->Record);L.Incarnation=N->Lease.Incarnation;
 L.Checksum=CalculateCheckSum8((UINT8 *)&L,sizeof(L));
 S=(EFI_STATUS)CrAdrenoHandoffValidate(&L,N->Record,&N->Route);if(S!=EFI_SUCCESS)return Final(S);
 S=A->InstallAcpiTable(A,&L,sizeof(L),&N->LocatorKey);if(S!=EFI_SUCCESS)return Final(S);
 S=A->InstallAcpiTable(A,N->Iort,N->IortBytes,&N->IortKey);if(S==EFI_SUCCESS)*Key=N->IortKey;return Final(S);
}
