/* EFI BlockIo adapter; automatic UFS hardware LUN0 plus present SD disk.
 * SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Protocol/DevicePath.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include "UsbMscBlock.h"

STATIC BOOLEAN Same(CONST VOID *A, CONST VOID *B, UINTN Bytes)
{ CONST UINT8 *P=A,*Q=B; while(Bytes--) if(*P++!=*Q++)return FALSE; return TRUE; }
typedef enum { MscUnknown, MscUfs, MscSd } MSC_DISK_KIND;

STATIC MSC_DISK_KIND Kind(EFI_DEVICE_PATH_PROTOCOL *Path, UINT64 *Identity)
{
  // SdhciCrDxe/MmcHost.c's native SDCC2 node (MmcDxe uses a vendor path).
  STATIC CONST EFI_GUID SdGuid={0x9d7d72ab,0x187c,0x4f29,{0x9d,0xa2,0x84,0x50,0x53,0x44,0x43,0x32}};
  MSC_DISK_KIND K=MscUnknown;
  UINTN Total=0;
  *Identity=14695981039346656037ULL;
  if(!Path)return MscUnknown;
  for(UINTN Node=0;Node<256;++Node){
    UINTN Len=(UINTN)Path->Length[0]+((UINTN)Path->Length[1]<<8);
    if(Len<sizeof(*Path)||Len>4096-Total)return MscUnknown;
    for(UINTN I=0;I<Len;++I)*Identity=(*Identity^((UINT8 *)Path)[I])*1099511628211ULL;
    if(Path->Type==END_DEVICE_PATH_TYPE)
      return Path->SubType==END_ENTIRE_DEVICE_PATH_SUBTYPE && Len==4 ? K : MscUnknown;
    if(Path->Type==MEDIA_DEVICE_PATH)return MscUnknown; // no partitions/file paths
    if(Path->Type==MESSAGING_DEVICE_PATH && Path->SubType==MSG_UFS_DP && Len==sizeof(UFS_DEVICE_PATH)){
      if(K!=MscUnknown || ((UFS_DEVICE_PATH *)Path)->Lun!=0)return MscUnknown; // exact hardware LUN0 only
      K=MscUfs;
    }else if((Path->Type==MESSAGING_DEVICE_PATH && Path->SubType==MSG_SD_DP && Len==sizeof(SD_DEVICE_PATH)) ||
             (Path->Type==HARDWARE_DEVICE_PATH && Path->SubType==HW_VENDOR_DP && Len==sizeof(VENDOR_DEVICE_PATH) &&
              Same(&((VENDOR_DEVICE_PATH *)Path)->Guid,&SdGuid,sizeof(SdGuid)))){
      if(K!=MscUnknown)return MscUnknown;
      K=MscSd;
    }
    Total+=Len;Path=(EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)Path+Len);
  }
  return MscUnknown;
}

STATIC EFI_STATUS Geometry(CONST EFI_BLOCK_IO_PROTOCOL *Io)
{
  CONST EFI_BLOCK_IO_MEDIA *M;
  if(!Io || !(M=Io->Media) || !Io->ReadBlocks || !Io->FlushBlocks)return EFI_UNSUPPORTED;
  if(M->LogicalPartition)return EFI_UNSUPPORTED;
  if(!M->MediaPresent)return EFI_NO_MEDIA;
  if(M->BlockSize<512 || M->BlockSize>CR_USB_MSC_TRANSFER_MAX || (M->BlockSize&(M->BlockSize-1)) ||
     M->LastBlock==MAX_UINT64 || M->IoAlign>CR_USB_MSC_TRANSFER_MAX ||
     (M->IoAlign && (M->IoAlign&(M->IoAlign-1))))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}

STATIC CR_STATUS MediaStatus(MSC_BLOCK *B)
{
  EFI_BLOCK_IO_MEDIA *M=B->Io->Media;
  if(B->Changed)return CR_ABORT;
  if(!M || !M->MediaPresent){B->Changed=TRUE;return CR_NOT_FOUND;}
  if(M->MediaId!=B->Media.MediaId || M->BlockSize!=B->Media.BlockSize || M->LastBlock!=B->Media.LastBlock ||
     M->IoAlign!=B->Media.IoAlign || M->LogicalPartition || M->ReadOnly!=B->Media.ReadOnly){
    B->Changed=TRUE;return CR_ABORT;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS IoStatus(MSC_BLOCK *B, EFI_STATUS S)
{
  if(S==EFI_SUCCESS)return MediaStatus(B);
  Print(L"UsbMscTest: block I/O: %r\n",S);
  if(S==EFI_NO_MEDIA || S==EFI_MEDIA_CHANGED){B->Changed=TRUE;return S==EFI_NO_MEDIA?CR_NOT_FOUND:CR_ABORT;}
  if(S==EFI_WRITE_PROTECTED)return CR_BUSY;
  if(S==EFI_NOT_READY)return CR_NOT_READY;
  return CR_DEVICE_ERROR; // warnings are not successful whole-block transfers
}

STATIC CR_STATUS Check(MSC_BLOCK *B, UINT64 Lba, UINT32 Blocks)
{
  CR_STATUS S=MediaStatus(B);
  if(S!=CR_SUCCESS)return S;
  if(!Blocks || Blocks>CR_USB_MSC_TRANSFER_MAX/B->Media.BlockSize || Lba>B->Media.LastBlock ||
     (UINT64)Blocks-1>B->Media.LastBlock-Lba)return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
STATIC CR_STATUS Read(VOID *Context,UINT64 Lba,UINT32 Blocks,VOID *Data)
{
  MSC_BLOCK *B=Context;CR_STATUS S=Check(B,Lba,Blocks);UINTN Bytes;
  if(S!=CR_SUCCESS)return S;
  Bytes=(UINTN)Blocks*B->Media.BlockSize;
  S=IoStatus(B,B->Io->ReadBlocks(B->Io,B->Media.MediaId,Lba,Bytes,B->Buffer));
  if(S==CR_SUCCESS)CopyMem(Data,B->Buffer,Bytes);
  return S;
}
STATIC CR_STATUS Flush(VOID *Context)
{
  // Read-only export has no host writes to flush, but removal must be reported.
  return MediaStatus((MSC_BLOCK *)Context);
}
STATIC CR_STATUS CheckMedia(VOID *Context)
{ return MediaStatus((MSC_BLOCK *)Context); }

STATIC EFI_STATUS OpenDisk(EFI_HANDLE Image, EFI_HANDLE Selected, MSC_DISK_KIND K,
                           UINT64 Identity, MSC_BLOCK *B, CR_USB_MSC_BLOCK_BACKEND *Backend)
{
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_STATUS S;
  UINTN Align;
  B->Agent=Image;
  S=gBS->HandleProtocol(Selected,&gEfiBlockIoProtocolGuid,(VOID **)&Io);
  if(S!=EFI_SUCCESS)return S;
  S=Geometry(Io);if(S!=EFI_SUCCESS)return S;
  B->Media=*Io->Media;B->Handle=Selected;
  // MU CoreOpenProtocol(EXCLUSIVE) disconnects BlockIo consumers first. Any
  // open filesystem which prevents detach makes this fail before USB starts.
  // Reconnect even if the exclusive open partially disconnected consumers.
  B->Reconnect=TRUE;
  S=gBS->OpenProtocol(Selected,&gEfiBlockIoProtocolGuid,(VOID **)&B->Io,Image,NULL,EFI_OPEN_PROTOCOL_EXCLUSIVE);
  if(S!=EFI_SUCCESS)return S;
  B->Opened=TRUE;
  S=Geometry(B->Io);if(S!=EFI_SUCCESS)return S;
  if(MediaStatus(B)!=CR_SUCCESS)return EFI_MEDIA_CHANGED;
  // Flush local writes after consumer detach, before publishing the disk.
  if(!B->Media.ReadOnly){S=B->Io->FlushBlocks(B->Io);if(S!=EFI_SUCCESS)return S;}
  if(MediaStatus(B)!=CR_SUCCESS)return EFI_MEDIA_CHANGED;
  Align=MAX((UINTN)B->Media.IoAlign,1);
  B->Allocation=AllocatePool(CR_USB_MSC_TRANSFER_MAX+Align-1);
  if(!B->Allocation)return EFI_OUT_OF_RESOURCES;
  B->Buffer=(VOID *)(((UINTN)B->Allocation+Align-1)&~(Align-1));
  Identity^=(B->Media.LastBlock^((UINT64)B->Media.BlockSize<<32));
  for(UINTN I=0;I<16;++I)B->Serial[I]=L"0123456789ABCDEF"[(Identity>>((15-I)*4))&15];
  Backend->Context=B;Backend->BlockSize=B->Media.BlockSize;Backend->LastBlock=B->Media.LastBlock;
  Backend->ReadOnly=TRUE;Backend->Read=Read;Backend->Write=NULL;Backend->Flush=Flush;Backend->Check=CheckMedia;
  Print(L"UsbMscTest: USB LUN%u = %s, block=%u last-LBA=%Lu, READ ONLY.\n",
        K==MscUfs?0U:1U,K==MscUfs?L"UFS LUN0":L"SD card",B->Media.BlockSize,B->Media.LastBlock);
  return EFI_SUCCESS;
}

EFI_STATUS MscAttachBlocks(EFI_HANDLE Image, MSC_BLOCK *Blocks,
                           CR_USB_MSC_BLOCK_BACKEND *Backends, UINTN *Count)
{
  EFI_HANDLE *Handles=NULL,Selected[MSC_DISK_MAX]={NULL,NULL};
  EFI_BLOCK_IO_PROTOCOL *Io;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  EFI_STATUS S;
  UINTN HandleCount=0,Slot;
  UINT64 Identity,Identities[MSC_DISK_MAX]={0,0};
  MSC_DISK_KIND K;
  if(!Blocks||!Backends||!Count)return EFI_INVALID_PARAMETER;
  ZeroMem(Blocks,sizeof(*Blocks)*MSC_DISK_MAX);
  ZeroMem(Backends,sizeof(*Backends)*MSC_DISK_MAX);*Count=0;
  S=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&HandleCount,&Handles);
  if(S!=EFI_SUCCESS)return S;
  for(UINTN I=0;I<HandleCount;++I){
    if(gBS->HandleProtocol(Handles[I],&gEfiBlockIoProtocolGuid,(VOID **)&Io)!=EFI_SUCCESS ||
       !Io || !Io->Media || Io->Media->LogicalPartition ||
       gBS->HandleProtocol(Handles[I],&gEfiDevicePathProtocolGuid,(VOID **)&Path)!=EFI_SUCCESS)continue;
    K=Kind(Path,&Identity);if(K==MscUnknown)continue;
    if(K==MscSd && !Io->Media->MediaPresent)continue;
    Slot=K==MscUfs?0:1;
    if(Selected[Slot]){
      Print(L"UsbMscTest: ambiguous %s disks; no storage exported.\n",Slot==0?L"UFS LUN0":L"SD");
      FreePool(Handles);return EFI_UNSUPPORTED;
    }
    Selected[Slot]=Handles[I];Identities[Slot]=Identity;
  }
  FreePool(Handles);
  if(!Selected[0]){Print(L"UsbMscTest: UFS hardware LUN0 is unavailable.\n");return EFI_NOT_FOUND;}
  for(Slot=0;Slot<MSC_DISK_MAX;++Slot){
    if(!Selected[Slot])break;
    S=OpenDisk(Image,Selected[Slot],Slot==0?MscUfs:MscSd,Identities[Slot],&Blocks[Slot],&Backends[Slot]);
    if(S!=EFI_SUCCESS){Print(L"UsbMscTest: USB LUN%u attach failed: %r\n",(UINT32)Slot,S);return S;}
    ++*Count;
  }
  if(*Count==1)Print(L"UsbMscTest: no SD card present; exporting UFS LUN0 only.\n");
  return EFI_SUCCESS;
}

EFI_STATUS MscReleaseBlock(MSC_BLOCK *B)
{
  EFI_STATUS S=EFI_SUCCESS,T;
  if(B->Opened){
    T=gBS->CloseProtocol(B->Handle,&gEfiBlockIoProtocolGuid,B->Agent,NULL);
    if(T!=EFI_SUCCESS)return T;
    B->Opened=FALSE;
  }
  if(B->Allocation){FreePool(B->Allocation);B->Allocation=B->Buffer=NULL;}
  if(B->Reconnect){
    T=gBS->ConnectController(B->Handle,NULL,NULL,TRUE);B->Reconnect=FALSE;
    if(T!=EFI_SUCCESS && T!=EFI_NOT_FOUND){Print(L"UsbMscTest: reconnect disk drivers: %r\n",T);if(S==EFI_SUCCESS)S=T;}
    Print(L"UsbMscTest: disk released; run map -r in Shell.\n");
  }
  return S;
}
