/* Source-qualified native services, no post-EBS allocator. MIT */
#include <PiDxe.h>
#include <Library/UartHandoffUefiLib.h>
#include <Library/UartAdmissionUefiLib.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <oskal/cr_atomic.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/FirmwareVolumeBlock.h>
#include <Protocol/DevicePath.h>

#define MAP_MAX (1024U*1024U)
STATIC CONST EFI_GUID GateGuid=CR_UART_ADMISSION_TABLE_GUID;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS || EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC BOOLEAN Covers(UINT64 Base,UINT64 Bytes,UINT64 P,UINT64 N){
  return Base && Bytes && Base<=MAX_UINT64-Bytes && P>=Base && N && N<=Bytes && P-Base<=Bytes-N;
}
STATIC UINT64 Le64(CONST UINT8 *P){UINT64 V=0;for(UINTN I=0;I<8;++I)V|=(UINT64)P[I]<<(8*I);return V;}
STATIC EFI_STATUS Normal(UINT64 Base,UINT64 Bytes,BOOLEAN Write){
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS S=Exact(gDS->GetMemorySpaceDescriptor(Base,&D));if(S!=EFI_SUCCESS)return S;
  if(!Covers(D.BaseAddress,D.Length,Base,Bytes) ||
     (D.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory && D.GcdMemoryType!=EfiGcdMemoryTypeReserved) ||
     !(D.Attributes&(EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC)) ||
     (D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_UC|EFI_MEMORY_UCE)) ||
     (Write && (D.Attributes&(EFI_MEMORY_WP|EFI_MEMORY_RO))))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FindGate(CR_UART_UEFI_HANDOFF *S){
  UINTN Count=0;CR_UART_ADMISSION *Gate=NULL;EFI_SYSTEM_TABLE *T=S->System;
  if(T->NumberOfTableEntries>4096 || (T->NumberOfTableEntries&&!T->ConfigurationTable))return EFI_COMPROMISED_DATA;
  for(UINTN I=0;I<T->NumberOfTableEntries;++I)if(!CompareMem(&T->ConfigurationTable[I].VendorGuid,&GateGuid,sizeof(GateGuid))){
    ++Count;Gate=T->ConfigurationTable[I].VendorTable;
  }
  if(Count!=1 || !Gate || Gate->Signature!=CR_UART_ADMISSION_SIGNATURE ||
     Gate->Size!=sizeof(*Gate) || (S->Gate && S->Gate!=Gate))return EFI_NOT_READY;
  S->Gate=Gate;return EFI_SUCCESS;
}
STATIC EFI_STATUS Source(CR_UART_UEFI_HANDOFF *S){
  EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;EFI_FIRMWARE_VOLUME_BLOCK_PROTOCOL *Block=NULL;
  EFI_STATUS R=Exact(gBS->HandleProtocol(S->Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Image));
  if(R!=EFI_SUCCESS)return R;
  if(!Image || !Image->DeviceHandle || Image->ImageCodeType!=EfiBootServicesCode)return EFI_UNSUPPORTED;
  R=Exact(gBS->HandleProtocol(Image->DeviceHandle,&gEfiFirmwareVolumeBlockProtocolGuid,(VOID **)&Block));
  if(R!=EFI_SUCCESS)return R;
  if(!Block || !Block->GetPhysicalAddress || !Block->GetBlockSize)return EFI_UNSUPPORTED;
  EFI_PHYSICAL_ADDRESS Base=0;UINTN BlockSize=0,Blocks=0;
  R=Exact(Block->GetPhysicalAddress(Block,&Base));if(R!=EFI_SUCCESS)return R;
  R=Exact(Block->GetBlockSize(Block,0,&BlockSize,&Blocks));if(R!=EFI_SUCCESS)return R;
  if(!Base || Base>MAX_UINTN || BlockSize<56 || !Blocks || BlockSize>MAX_UINTN/Blocks)return EFI_COMPROMISED_DATA;
  R=Normal(Base,56,FALSE);if(R!=EFI_SUCCESS)return R;
  UINT64 Bytes=Le64((CONST UINT8 *)(UINTN)Base+32);
  if(Bytes<56 || Bytes>32U*1024U*1024U || Bytes>BlockSize*Blocks)return EFI_COMPROMISED_DATA;
  R=Normal(Base,Bytes,FALSE);if(R!=EFI_SUCCESS)return R;
  R=CrUartCoverageVerifyFv(&S->Coverage,(CONST VOID *)(UINTN)Base,(UINTN)Bytes);
  if(R!=EFI_SUCCESS)return R;
  if(S->SourceVolume && (S->SourceVolume!=Image->DeviceHandle || S->SourceFv!=Base || S->SourceBytes!=Bytes))return EFI_ACCESS_DENIED;
  S->SourceVolume=Image->DeviceHandle;S->SourceFv=Base;S->SourceBytes=(UINTN)Bytes;
  return EFI_SUCCESS;
}
EFI_STATUS CrUartHandoffUefiPrepare(CR_UART_UEFI_HANDOFF *S,EFI_HANDLE Image,
    EFI_SYSTEM_TABLE *System,CONST CR_UART_FIRMWARE_COVERAGE *Coverage){
  if(!S || !Image || !System || System->BootServices!=gBS ||
     CrUartCoverageValidate(Coverage)!=EFI_SUCCESS)return EFI_INVALID_PARAMETER;
  if(S->Final)return EFI_ACCESS_DENIED;
  if(S->Preparing)return EFI_NOT_READY;
  if((S->Image && S->Image!=Image) || (S->System && S->System!=System) ||
     (S->CoverageOwned && CompareMem(&S->Coverage,Coverage,sizeof(*Coverage))))return EFI_ACCESS_DENIED;
  S->Preparing=TRUE;S->Image=Image;S->System=System;
  if(!S->CoverageOwned){CopyMem(&S->Coverage,Coverage,sizeof(*Coverage));S->CoverageOwned=TRUE;}
  EFI_STATUS R=FindGate(S);if(R!=EFI_SUCCESS)goto End;
  R=Source(S);if(R!=EFI_SUCCESS)goto End;
  if(S->Installed){R=EFI_SUCCESS;goto End;}
  if(S->TableOwned){R=S->Status==EFI_SUCCESS?EFI_NOT_READY:S->Status;goto End;}
  if(!S->Acpi){R=Exact(gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID **)&S->Acpi));
    if(R!=EFI_SUCCESS)goto End;
    if(!S->Acpi || !S->Acpi->InstallAcpiTable || !S->Acpi->UninstallAcpiTable){R=EFI_UNSUPPORTED;goto End;}}
  for(UINTN Attempt=0;!S->Generation && Attempt<2;++Attempt){
    R=Exact(gBS->GetNextMonotonicCount(&S->Generation));if(R!=EFI_SUCCESS){S->Generation=0;goto End;}}
  if(!S->Generation){R=EFI_NOT_READY;goto End;}
  if(!S->RecordOwned){R=Exact(gBS->AllocatePages(AllocateAnyPages,EfiACPIMemoryNVS,1,&S->Record));
    if(R!=EFI_SUCCESS){S->Record=0;goto End;}S->RecordOwned=TRUE;}
  R=Normal(S->Record,EFI_PAGE_SIZE,TRUE);if(R!=EFI_SUCCESS)goto End;
  if(S->Record<0x100000 || S->Record>MAX_UINTN || (S->Record&EFI_PAGE_MASK)){R=EFI_UNSUPPORTED;goto End;}
  ZeroMem((VOID *)(UINTN)S->Record,EFI_PAGE_SIZE);WriteBackDataCacheRange((VOID *)(UINTN)S->Record,EFI_PAGE_SIZE);
  if(!S->Handles){R=Exact(gBS->AllocatePool(EfiBootServicesData,CR_UART_HANDOFF_LOADED_HANDLES*sizeof(EFI_HANDLE),(VOID **)&S->Handles));
    if(R!=EFI_SUCCESS){S->Handles=NULL;goto End;}}
  if(!S->Map){UINTN Bytes=0,Key=0,Stride=0;UINT32 Version=0;
    R=gBS->GetMemoryMap(&Bytes,NULL,&Key,&Stride,&Version);
    if(R!=EFI_BUFFER_TOO_SMALL || Stride<sizeof(EFI_MEMORY_DESCRIPTOR) || Stride>1024 ||
       Bytes>MAP_MAX-128*Stride){R=EFI_OUT_OF_RESOURCES;goto End;}
    Bytes+=128*Stride;if(Bytes<65536)Bytes=65536;
    R=Exact(gBS->AllocatePool(EfiBootServicesData,Bytes,(VOID **)&S->Map));
    if(R!=EFI_SUCCESS){S->Map=NULL;goto End;}S->MapCapacity=Bytes;
  }
  CR_UART_HANDOFF_LOCATOR Locator;ZeroMem(&Locator,sizeof(Locator));
  Locator.Signature=CR_UART_LOCATOR_SIGNATURE;Locator.Length=sizeof(Locator);Locator.Revision=1;Locator.WireRevision=1;
  CopyMem(Locator.OemId,"ALOHA ",6);CopyMem(Locator.OemTableId,"UART8450",8);Locator.OemRevision=1;
  Locator.CreatorId=SIGNATURE_32('C','R','D','K');Locator.CreatorRevision=1;
  Locator.RecordPhysical=S->Record;Locator.RecordLength=sizeof(CR_UART_HANDOFF);Locator.RecordGeneration=S->Generation;
  Locator.Checksum=CalculateCheckSum8((CONST UINT8 *)&Locator,sizeof(Locator));
  S->TableKey=0;
  R=Exact(S->Acpi->InstallAcpiTable(S->Acpi,&Locator,sizeof(Locator),&S->TableKey));
  /* The deployed AcpiTableDxe assigns a nonzero key when AddTableToList
   * commits, before PublishTables can fail. Retain every referenced byte
   * until exact successful removal, even when Install returned an error. */
  S->TableOwned=R==EFI_SUCCESS || S->TableKey!=0;
  if(R==EFI_SUCCESS){
    if(S->TableKey)S->Installed=TRUE;
    else R=EFI_COMPROMISED_DATA;
  }
End:
  S->Preparing=FALSE;S->Status=R;return R;
}
EFI_STATUS CrUartHandoffUefiRelease(CR_UART_UEFI_HANDOFF *S){
  if(!S)return EFI_INVALID_PARAMETER;
  if(S->Final)return EFI_ACCESS_DENIED;
  if(S->Preparing)return EFI_NOT_READY;
  EFI_STATUS R;
  if(S->TableOwned){R=Exact(S->Acpi->UninstallAcpiTable(S->Acpi,S->TableKey));if(R!=EFI_SUCCESS)return R;
    S->Installed=FALSE;S->TableOwned=FALSE;S->TableKey=0;}
  if(S->Map){R=Exact(gBS->FreePool(S->Map));if(R!=EFI_SUCCESS)return R;S->Map=NULL;S->MapCapacity=0;}
  if(S->Handles){R=Exact(gBS->FreePool(S->Handles));if(R!=EFI_SUCCESS)return R;S->Handles=NULL;}
  if(S->RecordOwned){R=Exact(gBS->FreePages(S->Record,1));if(R!=EFI_SUCCESS)return R;S->RecordOwned=FALSE;S->Record=0;}
  S->Generation=0;S->Acpi=NULL;return EFI_SUCCESS;
}
STATIC EFI_STATUS FinalMap(CR_UART_UEFI_HANDOFF *S,CR_UART_HANDOFF_INPUT *In){
  UINTN Bytes=S->MapCapacity,Key=0,Stride=0;UINT32 Version=0;
  EFI_STATUS R=Exact(gBS->GetMemoryMap(&Bytes,S->Map,&Key,&Stride,&Version));if(R!=EFI_SUCCESS)return R;
  if(Version!=EFI_MEMORY_DESCRIPTOR_VERSION || Stride<sizeof(EFI_MEMORY_DESCRIPTOR) ||
     Stride>1024 || (Stride&7) || Bytes>S->MapCapacity || Bytes%Stride)return EFI_COMPROMISED_DATA;
  BOOLEAN Found=FALSE;
  for(UINTN Offset=0;Offset<Bytes;Offset+=Stride){EFI_MEMORY_DESCRIPTOR D;CopyMem(&D,(UINT8 *)S->Map+Offset,sizeof(D));
    if(!D.NumberOfPages || D.NumberOfPages>MAX_UINT64/EFI_PAGE_SIZE || (D.PhysicalStart&EFI_PAGE_MASK))return EFI_COMPROMISED_DATA;
    UINT64 Length=D.NumberOfPages*EFI_PAGE_SIZE;
    if(D.PhysicalStart>MAX_UINT64-Length)return EFI_COMPROMISED_DATA;
    if(D.PhysicalStart<S->Record+EFI_PAGE_SIZE && S->Record<D.PhysicalStart+Length){
      if(Found || !Covers(D.PhysicalStart,Length,S->Record,EFI_PAGE_SIZE) ||
         (D.Type!=EfiReservedMemoryType && D.Type!=EfiACPIMemoryNVS))return EFI_ACCESS_DENIED;
      Found=TRUE;In->DescriptorBase=D.PhysicalStart;In->DescriptorSize=Length;
      In->DescriptorType=D.Type;In->DescriptorAttributes=D.Attribute;
    }
  }return Found?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC BOOLEAN FileGuid(CONST EFI_DEVICE_PATH_PROTOCOL *Path,UINT8 Guid[16]){
  if(!Path)return FALSE;
  CONST UINT8 *P=(CONST UINT8 *)Path;UINTN Position=0;BOOLEAN Found=FALSE;
  while(Position+4<=4096){UINT16 Length=(UINT16)(P[Position+2]|((UINT16)P[Position+3]<<8));
    if(Length<4 || Length>4096-Position)return FALSE;
    if(P[Position]==MEDIA_DEVICE_PATH && P[Position+1]==MEDIA_PIWG_FW_FILE_DP){
      if(Length!=20 || Found)return FALSE;CopyMem(Guid,P+Position+4,16);Found=TRUE;
    }
    if(P[Position]==END_DEVICE_PATH_TYPE)return Found && P[Position+1]==END_ENTIRE_DEVICE_PATH_SUBTYPE && Length==4;
    Position+=Length;
  }return FALSE;
}
STATIC EFI_STATUS Loaded(CR_UART_UEFI_HANDOFF *S){
  EFI_STATUS R=Source(S);if(R!=EFI_SUCCESS)return R;
  UINTN Bytes=CR_UART_HANDOFF_LOADED_HANDLES*sizeof(EFI_HANDLE);
  R=Exact(gBS->LocateHandle(ByProtocol,&gEfiLoadedImageProtocolGuid,NULL,&Bytes,S->Handles));
  if(R!=EFI_SUCCESS)return R;
  if(Bytes>CR_UART_HANDOFF_LOADED_HANDLES*sizeof(EFI_HANDLE) || Bytes%sizeof(EFI_HANDLE))return EFI_COMPROMISED_DATA;
  EFI_STATUS Result=EFI_SUCCESS;S->LoadedCount=0;ZeroMem(S->Loaded,sizeof(S->Loaded));
  for(UINTN I=0;I<Bytes/sizeof(EFI_HANDLE);++I){EFI_LOADED_IMAGE_PROTOCOL *Image=NULL;
    R=Exact(gBS->HandleProtocol(S->Handles[I],&gEfiLoadedImageProtocolGuid,(VOID **)&Image));
    if(R!=EFI_SUCCESS || !Image)return R!=EFI_SUCCESS?R:EFI_COMPROMISED_DATA;
    if(Image->ImageCodeType!=EfiRuntimeServicesCode)continue;
    if(S->LoadedCount==CR_UART_HANDOFF_IMAGES)return EFI_BUFFER_TOO_SMALL;
    CR_UART_FIRMWARE_IMAGE *P=&S->Loaded[S->LoadedCount++];P->SerialKind=CR_UART_IMAGE_UNKNOWN;
    if(!FileGuid(Image->FilePath,P->Guid) || Image->DeviceHandle!=S->SourceVolume){Result=EFI_ACCESS_DENIED;continue;}
    R=CrUartCoverageResolve(&S->Coverage,P->Guid,P);if(R!=EFI_SUCCESS){P->SerialKind=CR_UART_IMAGE_UNKNOWN;Result=R;}
  }
  return S->LoadedCount?Result:EFI_NOT_FOUND;
}
STATIC CR_STATUS Read(VOID *C,UINT32 Offset,UINT32 *Value){
  (VOID)C;if((Offset&3) || Offset>CR_UART_HANDOFF_SPAN-4)return EFI_INVALID_PARAMETER;
  *Value=MmioRead32((UINTN)CR_UART_HANDOFF_BASE+Offset);return EFI_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,UINT32 Offset,UINT32 Value){
  (VOID)C;if((Offset&3) || Offset>CR_UART_HANDOFF_SPAN-4)return EFI_INVALID_PARAMETER;
  MmioWrite32((UINTN)CR_UART_HANDOFF_BASE+Offset,Value);return EFI_SUCCESS;
}
STATIC VOID Delay(VOID *C,UINT32 Us){(VOID)C;MicroSecondDelay(Us);}
STATIC VOID Fence(VOID *C){(VOID)C;ArmDataSynchronizationBarrier();}
EFI_STATUS CrUartHandoffUefiFinalize(CR_UART_UEFI_HANDOFF *S){
  if(!S)return EFI_INVALID_PARAMETER;
  if(S->Final)return S->Status;
  if(S->Preparing || !S->Installed || !S->RecordOwned || !S->Map || !S->Handles || !S->Gate)return EFI_NOT_READY;
  S->Final=TRUE; /* exact state/page ownership is now permanent even on failure */
  CR_UART_HANDOFF_INPUT In;ZeroMem(&In,sizeof(In));CR_UART_HANDOFF H;
  In.Generation=S->Generation;In.RecordPhysical=S->Record;In.Coverage=&S->Coverage;In.Retirement=&S->Observation;
  EFI_STATUS Status=FinalMap(S,&In);
  EFI_STATUS Close=CrUartAdmissionClose(S->Gate);
  EFI_STATUS Runtime=Loaded(S);
  ZeroMem(&S->Observation,sizeof(S->Observation));
  if(Status==EFI_SUCCESS && Runtime==EFI_SUCCESS){
    CR_UART_RETIRE_IO Io={S,Read,Write,Delay,Fence};Status=CrUartAdmissionRetire(S->Gate,&Io,&S->Observation);
  }else{
    if(Status==EFI_SUCCESS)Status=Runtime;
    S->Observation.GateState=CrAtomicLoad32(&S->Gate->State);S->Observation.Active=CrAtomicLoad32(&S->Gate->Active);
    S->Observation.Bindings=CrAtomicLoad32(&S->Gate->Bindings);S->Observation.RuntimeBindings=CrAtomicLoad32(&S->Gate->RuntimeBindings);
    S->Observation.Status=Status;
  }
  (VOID)Close;In.NativeExitStatus=Status;In.Loaded=S->Loaded;In.LoadedCount=S->LoadedCount;
  EFI_STATUS Build=CrUartHandoffBuild(&H,&In);
  if(Build!=EFI_SUCCESS && Build!=EFI_NOT_READY){S->Status=Build;return Build;}
  /* Single final release publication. Compute CRC for even2, copy underodd1,
   * clean whole page, DSB, then final aligned sequence store + clean + DSB. */
  H.PublishSequence=1;CopyMem((VOID *)(UINTN)S->Record,&H,sizeof(H));
  WriteBackDataCacheRange((VOID *)(UINTN)S->Record,EFI_PAGE_SIZE);ArmDataSynchronizationBarrier();
  ((volatile CR_UART_HANDOFF *)(UINTN)S->Record)->PublishSequence=2;
  WriteBackDataCacheRange((VOID *)(UINTN)S->Record,EFI_PAGE_SIZE);ArmDataSynchronizationBarrier();
  S->Status=Status==EFI_SUCCESS?Build:Status;return S->Status;
}
