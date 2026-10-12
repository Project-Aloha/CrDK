/** HDK fixed framebuffer owner, current RAM grant and GCD effect journal. MIT */
#include "DpReservation.h"
#include <Library/BaseMemoryLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFICrRamRange.h>

STATIC EFI_HANDLE mReservationImage;
STATIC EFI_CR_RAM_RANGE_PROTOCOL *mReservationRam;
STATIC UINT64 mReservationBase;
STATIC BOOLEAN mReservationClaimAttempted,mReservationFreeAttempted,mReservationUnknownAddress;
STATIC EFI_STATUS FinalStatus(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC BOOLEAN ProfileName(CONST CHAR8 *Name){
  CONST CHAR8 Expected[]=CR_DP_RESERVATION_NAME;
  return CompareMem(Name,Expected,sizeof(Expected))==0;
}
EFI_STATUS CrDpReservationInit(EFI_HANDLE ImageHandle){
  if(!ImageHandle)return EFI_INVALID_PARAMETER;
  if(mReservationImage)return mReservationImage==ImageHandle?EFI_SUCCESS:EFI_ACCESS_DENIED;
  if(!gBS||!gBS->LocateProtocol||!gDS||!gDS->GetMemorySpaceDescriptor||
    !gDS->AllocateMemorySpace||!gDS->FreeMemorySpace||!gDS->SetMemorySpaceAttributes)return EFI_UNSUPPORTED;
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map=GetPlatformMemoryMap(),*Selected=NULL;
  if(!Map)return EFI_NOT_FOUND;
  UINTN I;
  for(I=0;I<MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT&&Map[I].Length;++I){
    if(ProfileName(Map[I].Name)){if(Selected)return EFI_COMPROMISED_DATA;Selected=&Map[I];}
  }
  if(I==MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT)return EFI_COMPROMISED_DATA;
  if(!Selected)return EFI_UNSUPPORTED; /* No address fallback on another target. */
  if(Selected->Length!=CR_DP_RESERVATION_BYTES||!Selected->Address||
    (Selected->Address&0xffffULL)||Selected->Address>MAX_UINT64-Selected->Length||
    Selected->HobOption!=AddMem||Selected->ResourceType!=MEM_RES||Selected->MemoryType!=Reserv||
    (Selected->ResourceAttribute&(SYS_MEM_CAP))!=(SYS_MEM_CAP)||
    Selected->ArmAttributes!=UNCACHED_UNBUFFERED_XN)return EFI_COMPROMISED_DATA;
  for(UINTN J=0;J<I;++J){
    if(&Map[J]==Selected)continue;
    if(Map[J].Address>MAX_UINT64-Map[J].Length)return EFI_COMPROMISED_DATA;
    if(Map[J].Address<Selected->Address+Selected->Length&&
      Selected->Address<Map[J].Address+Map[J].Length)return EFI_COMPROMISED_DATA;
  }
  EFI_CR_RAM_RANGE_PROTOCOL *Ram=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&gEfiCrRamRangeProtocolGuid,NULL,(VOID **)&Ram);
  if(S!=EFI_SUCCESS)return FinalStatus(S);
  if(!Ram||Ram->Revision!=EFI_CR_RAM_RANGE_PROTOCOL_REVISION||!Ram->CheckAppsReadWriteRange)return EFI_UNSUPPORTED;
  S=Ram->CheckAppsReadWriteRange(Ram,Selected->Address,Selected->Length);
  if(S!=EFI_SUCCESS)return FinalStatus(S);
  mReservationImage=ImageHandle;mReservationRam=Ram;mReservationBase=Selected->Address;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Descriptor(EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  EFI_STATUS S=gDS->GetMemorySpaceDescriptor(mReservationBase,D);
  if(S!=EFI_SUCCESS)return FinalStatus(S);
  if(D->GcdMemoryType!=EfiGcdMemoryTypeReserved||D->BaseAddress>mReservationBase||
    D->BaseAddress>MAX_UINT64-D->Length||D->Length<CR_DP_RESERVATION_BYTES||
    mReservationBase-D->BaseAddress>D->Length-CR_DP_RESERVATION_BYTES)return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}
EFI_STATUS CrDpReservationCheck(EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  if(!D)return EFI_INVALID_PARAMETER;
  if(!mReservationImage||!mReservationClaimAttempted||mReservationFreeAttempted||mReservationUnknownAddress)return EFI_NOT_READY;
  EFI_STATUS S=mReservationRam->CheckAppsReadWriteRange(mReservationRam,mReservationBase,CR_DP_RESERVATION_BYTES);
  if(S!=EFI_SUCCESS)return FinalStatus(S);
  S=Descriptor(D);if(S!=EFI_SUCCESS)return S;
  if(D->ImageHandle!=mReservationImage||D->DeviceHandle!=NULL)return EFI_ACCESS_DENIED;
  if((D->Capabilities&(CR_DP_RESERVED_STATE_CAPS|EFI_MEMORY_WC|EFI_MEMORY_XP))!=
    (CR_DP_RESERVED_STATE_CAPS|EFI_MEMORY_WC|EFI_MEMORY_XP)||
    (D->Attributes&(EFI_MEMORY_RP|EFI_MEMORY_RO|EFI_MEMORY_WP)))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
EFI_STATUS CrDpReservationAcquire(EFI_PHYSICAL_ADDRESS *Base,UINT64 *Bytes){
  if(!Base||!Bytes)return EFI_INVALID_PARAMETER;
  if(!mReservationImage)return EFI_NOT_READY;
  if(mReservationFreeAttempted||mReservationUnknownAddress)return EFI_ACCESS_DENIED;
  EFI_STATUS S=mReservationRam->CheckAppsReadWriteRange(mReservationRam,mReservationBase,CR_DP_RESERVATION_BYTES);
  if(S!=EFI_SUCCESS)return FinalStatus(S);
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  S=Descriptor(&D);if(S!=EFI_SUCCESS)return S;
  if(!mReservationClaimAttempted||D.ImageHandle==NULL){
    if(D.ImageHandle||D.DeviceHandle)return EFI_ACCESS_DENIED;
    EFI_PHYSICAL_ADDRESS Address=mReservationBase;
    mReservationClaimAttempted=TRUE; /* The call may commit before failure. */
    S=gDS->AllocateMemorySpace(EfiGcdAllocateAddress,EfiGcdMemoryTypeReserved,16,
      CR_DP_RESERVATION_BYTES,&Address,mReservationImage,NULL);
    if(Address!=mReservationBase)mReservationUnknownAddress=TRUE;
    if(S!=EFI_SUCCESS)return FinalStatus(S);
    if(mReservationUnknownAddress)return EFI_DEVICE_ERROR;
  }
  S=CrDpReservationCheck(&D);if(S!=EFI_SUCCESS)return S;
  *Base=mReservationBase;*Bytes=CR_DP_RESERVATION_BYTES;return EFI_SUCCESS;
}
BOOLEAN CrDpReservationAllocation(EFI_PHYSICAL_ADDRESS *Base,UINT64 *Bytes){
  if(!Base||!Bytes||!mReservationClaimAttempted)return FALSE;
  *Base=mReservationBase;*Bytes=CR_DP_RESERVATION_BYTES;return TRUE;
}
EFI_STATUS CrDpReservationRelease(VOID){
  if(!mReservationClaimAttempted)return EFI_SUCCESS;
  if(mReservationUnknownAddress)return EFI_ACCESS_DENIED;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=Descriptor(&D);if(S!=EFI_SUCCESS)return S;
  if(D.DeviceHandle|| (D.ImageHandle&&D.ImageHandle!=mReservationImage))return EFI_ACCESS_DENIED;
  if(D.ImageHandle){
    mReservationFreeAttempted=TRUE;
    S=gDS->FreeMemorySpace(mReservationBase,CR_DP_RESERVATION_BYTES);
    if(S!=EFI_SUCCESS)return FinalStatus(S);
    S=Descriptor(&D);if(S!=EFI_SUCCESS)return S;
    if(D.ImageHandle||D.DeviceHandle)return EFI_DEVICE_ERROR;
  }
  /* An exact fresh unowned descriptor resolves a failed claim or a release
   * that committed before returning nonfinal. The physical reservation stays. */
  mReservationClaimAttempted=mReservationFreeAttempted=FALSE;
  return EFI_SUCCESS;
}
