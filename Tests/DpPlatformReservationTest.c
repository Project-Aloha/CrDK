/** Actual HDK map -> platform HOB -> unchanged Core capabilities/projection. MIT */
#include <PiDxe.h>
#include <Library/BaseMemoryLib.h>
#include <Library/HobLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include "../Driver/MDSSDpCrDxe/DpReservation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static EFI_HOB_RESOURCE_DESCRIPTOR Resource;
static UINTN ResourceHobs,AllocationHobs;
VOID EFIAPI BuildResourceDescriptorHob(EFI_RESOURCE_TYPE T,EFI_RESOURCE_ATTRIBUTE_TYPE A,EFI_PHYSICAL_ADDRESS B,UINT64 N){
  ++ResourceHobs;Resource.ResourceType=T;Resource.ResourceAttribute=A;Resource.PhysicalStart=B;Resource.ResourceLength=N;
}
VOID EFIAPI BuildMemoryAllocationHob(EFI_PHYSICAL_ADDRESS B,UINT64 N,EFI_MEMORY_TYPE T){(VOID)B;(VOID)N;(VOID)T;++AllocationHobs;}
UINT64 EFIAPI RShiftU64(UINT64 V,UINTN N){return V>>N;}
#include "dp_reserved_core.inc"
int main(VOID){
  ARM_MEMORY_REGION_DESCRIPTOR_EX *Map=GetPlatformMemoryMap(),*Dp=NULL,*Dsi=NULL,*Rest=NULL;
  UINTN Count=0;
  for(;Map[Count].Length;++Count){
    assert(Count<MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT&&Map[Count].Address<=MAX_UINT64-Map[Count].Length);
    if(!strcmp(Map[Count].Name,CR_DP_RESERVATION_NAME)){assert(!Dp);Dp=&Map[Count];}
    if(!strcmp(Map[Count].Name,"Display Reserved")){assert(!Dsi);Dsi=&Map[Count];}
    if(Map[Count].Address==0xf1c00000)Rest=&Map[Count];
  }
  assert(Dp&&Dp->Address==0xf1400000&&Dp->Length==CR_DP_RESERVATION_BYTES&&!(Dp->Address&0xffff));
  assert(Dp->HobOption==AddMem&&Dp->ResourceType==MEM_RES&&Dp->MemoryType==Reserv&&Dp->ArmAttributes==UNCACHED_UNBUFFERED_XN);
  assert(Rest&&Rest->Length==0xe000000&&Rest->Address+Rest->Length==0xffc00000&&Rest->ResourceType==SYS_MEM&&Rest->MemoryType==Conv);
  assert(Dsi&&Dsi->Address==0xb8000000&&Dsi->Length==0x2b00000&&Dsi->MemoryType==Reserv);
  for(UINTN I=0;I<Count;++I)if(&Map[I]!=Dp)
    assert(Map[I].Address>=Dp->Address+Dp->Length||Map[I].Address+Map[I].Length<=Dp->Address);
  AddHob(Dp);assert(ResourceHobs==1&&!AllocationHobs&&Resource.ResourceType==EFI_RESOURCE_MEMORY_RESERVED);
  EFI_GCD_MEMORY_TYPE Type=ReservedHobType(Resource.ResourceType);
  assert(Type==EfiGcdMemoryTypeReserved);
  UINT64 Caps=CoreConvertResourceDescriptorHobAttributesToCapabilities(Type,Resource.ResourceAttribute);
  assert((Caps&(CR_DP_RESERVED_STATE_CAPS|EFI_MEMORY_WC|EFI_MEMORY_XP))==(CR_DP_RESERVED_STATE_CAPS|EFI_MEMORY_WC|EFI_MEMORY_XP));
  CR_TEST_GCD E={.BaseAddress=Dp->Address,.EndAddress=Dp->Address+Dp->Length-1,.GcdMemoryType=Type,
    .Capabilities=Caps,.Attributes=EFI_MEMORY_WC|EFI_MEMORY_XP};
  assert(!Promotable(&E));
  E.Capabilities&=~EFI_MEMORY_TESTED;assert(Promotable(&E));E.Capabilities=Caps;
  EFI_MEMORY_DESCRIPTOR M=ReservedProjection(E);
  assert(M.Type==EfiReservedMemoryType&&M.PhysicalStart==Dp->Address&&M.NumberOfPages==2048);
  assert((M.Attribute&EFI_MEMORY_WC)&&(M.Attribute&EFI_MEMORY_XP));
  puts("HDK map/HOB + unchanged Core conversion: 8 MiB Reserved, tested-state promotion refusal, whole EFI Reserved projection, DSI and RAM remainder PASS");
  return 0;
}
