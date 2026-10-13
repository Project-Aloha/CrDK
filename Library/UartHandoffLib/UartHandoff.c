/* No allocation/OS/hardware writes. SPDX-License-Identifier: MIT */
#include <Library/UartHandoffLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>

STATIC BOOLEAN Zero(CONST VOID *P,UINTN N) {
  CONST UINT8 *B=P;for(UINTN I=0;I<N;++I)if(B[I])return FALSE;return TRUE;
}
STATIC INTN Compare(CONST UINT8 *A,CONST UINT8 *B) {
  for(UINTN I=0;I<16;++I){if(A[I]<B[I])return -1;if(A[I]>B[I])return 1;}return 0;
}
STATIC BOOLEAN Range(UINT64 A,UINT64 N) {return A && N && A<=CR_MAX_UINT64-N;}
STATIC BOOLEAN Reserved(CONST CR_UART_HANDOFF *H,UINT64 Physical) {
  return Range(Physical,4096) && !(Physical&4095) &&
    Range(H->RecordDescriptorBase,H->RecordDescriptorSize) &&
    !((H->RecordDescriptorBase|H->RecordDescriptorSize)&4095) &&
    Physical>=H->RecordDescriptorBase &&
    Physical-H->RecordDescriptorBase<=H->RecordDescriptorSize &&
    4096<=H->RecordDescriptorSize-(Physical-H->RecordDescriptorBase) &&
    (H->RecordDescriptorType==0 || H->RecordDescriptorType==10) &&
    /* Core's reserved EFI map attributes may be GCD capabilities (observed
     * WP|allcache bits), not active PTE permission. Producer independently
     * checks active normal read/write GCD attributes; OS checks RAM exclusion.
     * Never mistake a capability bit for active write protection here. */
    (H->RecordDescriptorAttributes&(2ULL|4ULL|8ULL));
}
STATIC BOOLEAN Idle(CONST CR_UART_HANDOFF *H) {
  return H->Protocol==2 && !(H->GeniStatus&0x1001U) && !H->TxWords && !H->RxWords &&
    H->RxDepth && H->RxDepth<=63 && !H->MasterIrqEnable && !H->SlaveIrqEnable &&
    !H->SeIrqEnable && !H->GsiEventEnable && !H->DmaMode &&
    !H->DmaTxIrqEnable && !H->DmaRxIrqEnable;
}
CR_STATUS CrUartCoverageValidate(CONST CR_UART_FIRMWARE_COVERAGE *C) {
  if(!C || C->Revision!=1 || !C->Count || C->Count>CR_UART_HANDOFF_IMAGES ||
     Zero(C->Digest,sizeof(C->Digest)))return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<C->Count;++I){
    CONST CR_UART_FIRMWARE_BUILD_IMAGE *P=&C->Images[I];
    if(Zero(P->Guid,16) || P->SerialKind>CR_UART_IMAGE_GATED_SERIAL || P->Reserved ||
       P->PeBytes<96 || P->PeBytes>32U*1024U*1024U ||
       (I && Compare(C->Images[I-1].Guid,P->Guid)>=0))return CR_INVALID_PARAMETER;
  }
  return Zero(C->Images+C->Count,(CR_UART_HANDOFF_IMAGES-C->Count)*sizeof(C->Images[0])) ?
    CR_SUCCESS : CR_INVALID_PARAMETER;
}
CR_STATUS CrUartCoverageResolve(CONST CR_UART_FIRMWARE_COVERAGE *C,
    CONST UINT8 Guid[16],CR_UART_FIRMWARE_IMAGE *Out) {
  if(!Guid || !Out || CrUartCoverageValidate(C)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<C->Count;++I)if(!Compare(Guid,C->Images[I].Guid)){
    CR_UART_FIRMWARE_IMAGE P;cr_memset(&P,0,sizeof(P));cr_memcpy(P.Guid,Guid,16);
    P.SerialKind=C->Images[I].SerialKind;cr_memcpy(Out,&P,sizeof(P));return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
UINT32 CrUartHandoffChecksum(CONST CR_UART_HANDOFF *H) {
  if(!H)return 0;
  UINT32 C=~0U;CONST UINT8 *P=(CONST UINT8 *)H;
  for(UINTN I=0;I<offsetof(CR_UART_HANDOFF,Checksum);++I){
    C^=P[I];for(UINTN B=0;B<8;++B)C=(C>>1)^((0U-(C&1U))&0xedb88320U);
  }return ~C;
}
CR_STATUS CrUartHandoffBuild(CR_UART_HANDOFF *Out,CONST CR_UART_HANDOFF_INPUT *In) {
  if(!Out || !In || !In->Retirement || !In->Generation ||
     In->LoadedCount>CR_UART_HANDOFF_IMAGES || (In->LoadedCount&&!In->Loaded) ||
     CrUartCoverageValidate(In->Coverage)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  CR_UART_HANDOFF H;cr_memset(&H,0,sizeof(H));
  H.Signature=CR_UART_HANDOFF_SIGNATURE;H.Revision=1;H.Size=sizeof(H);
  H.Platform=CR_UART_HANDOFF_PLATFORM;H.Generation=In->Generation;
  H.PublishSequence=2;H.Phase=CR_UART_HANDOFF_UNCERTAIN;
  H.NativeExitStatus=In->NativeExitStatus;H.UartBase=CR_UART_HANDOFF_BASE;H.UartSize=CR_UART_HANDOFF_SPAN;
  cr_memcpy(H.CoverageDigest,In->Coverage->Digest,sizeof(H.CoverageDigest));
  H.BuildRuntimeCount=In->Coverage->Count;H.LoadedRuntimeCount=In->LoadedCount;
  H.RecordDescriptorBase=In->DescriptorBase;H.RecordDescriptorSize=In->DescriptorSize;
  H.RecordDescriptorType=In->DescriptorType;H.RecordDescriptorAttributes=In->DescriptorAttributes;
  H.Flags=CR_UART_HANDOFF_LINK_COVERAGE;
  CONST CR_UART_RETIRE_OBSERVATION *O=In->Retirement;
  H.GateState=O->GateState;H.Active=O->Active;H.Bindings=O->Bindings;H.RuntimeBindings=O->RuntimeBindings;
  H.Protocol=O->Protocol;H.GeniStatus=O->GeniStatus;H.TxWords=O->TxWords;
  H.RxWords=O->RxWords;H.RxDepth=O->RxDepth;H.MasterIrqEnable=O->MasterIrqEnable;
  H.SlaveIrqEnable=O->SlaveIrqEnable;H.SeIrqEnable=O->SeIrqEnable;H.GsiEventEnable=O->GsiEventEnable;
  H.DmaMode=O->DmaMode;H.DmaTxIrqEnable=O->DmaTxIrqEnable;H.DmaRxIrqEnable=O->DmaRxIrqEnable;
  BOOLEAN Covered=In->LoadedCount!=0;
  for(UINT32 I=0;I<In->LoadedCount;++I){
    if(In->Loaded[I].Reserved || In->Loaded[I].SerialKind>CR_UART_IMAGE_UNKNOWN ||
       (Zero(In->Loaded[I].Guid,16) && In->Loaded[I].SerialKind!=CR_UART_IMAGE_UNKNOWN))
      return CR_INVALID_PARAMETER;
    cr_memcpy(H.Images[I].Guid,In->Loaded[I].Guid,16);H.Images[I].SerialKind=CR_UART_IMAGE_UNKNOWN;
    for(UINT32 J=0;J<In->Coverage->Count;++J)if(!Compare(In->Loaded[I].Guid,In->Coverage->Images[J].Guid) &&
       In->Loaded[I].SerialKind==In->Coverage->Images[J].SerialKind){
      H.Images[I].SerialKind=In->Coverage->Images[J].SerialKind;break;
    }
    if(H.Images[I].SerialKind==CR_UART_IMAGE_UNKNOWN)Covered=FALSE;
    if(H.Images[I].SerialKind==CR_UART_IMAGE_GATED_SERIAL)++H.LoadedGatedCount;
  }
  for(UINT32 I=1;I<In->LoadedCount;++I)for(UINT32 J=I;J && Compare(H.Images[J-1].Guid,H.Images[J].Guid)>0;--J){
    CR_UART_FIRMWARE_IMAGE T;cr_memcpy(&T,&H.Images[J],sizeof(T));
    cr_memcpy(&H.Images[J],&H.Images[J-1],sizeof(T));cr_memcpy(&H.Images[J-1],&T,sizeof(T));
  }
  if(Covered && H.RuntimeBindings>=H.LoadedGatedCount)H.Flags|=CR_UART_HANDOFF_LOADED_COVERAGE;
  if(H.GateState==CR_UART_ADMISSION_CLOSED && !H.Active)H.Flags|=CR_UART_HANDOFF_CLOSED;
  if(O->Complete && O->Status==CR_SUCCESS && Idle(&H))H.Flags|=CR_UART_HANDOFF_HARDWARE_RETIRED;
  if(Reserved(&H,In->RecordPhysical))H.Flags|=CR_UART_HANDOFF_RESERVED_MEMORY;
  if(H.Flags==CR_UART_HANDOFF_ALL_FLAGS && !H.NativeExitStatus && H.Bindings>=H.RuntimeBindings)
    H.Phase=CR_UART_HANDOFF_RETIRED;
  H.Checksum=CrUartHandoffChecksum(&H);cr_memcpy(Out,&H,sizeof(H));
  return H.Phase==CR_UART_HANDOFF_RETIRED?CR_SUCCESS:CR_NOT_READY;
}
CR_STATUS CrUartHandoffValidate(CONST CR_UART_HANDOFF *H,UINT64 Physical,UINT64 Generation) {
  if(!H || H->Signature!=CR_UART_HANDOFF_SIGNATURE || H->Revision!=1 || H->Size!=sizeof(*H) ||
     H->Platform!=CR_UART_HANDOFF_PLATFORM || !Generation || H->Generation!=Generation ||
     !H->PublishSequence || (H->PublishSequence&1) || H->PublishSequence!=2 ||
     (H->Phase!=CR_UART_HANDOFF_RETIRED && H->Phase!=CR_UART_HANDOFF_UNCERTAIN) ||
     H->UartBase!=CR_UART_HANDOFF_BASE || H->UartSize!=CR_UART_HANDOFF_SPAN ||
     H->Reserved0 || H->Reserved1 || H->Reserved2 ||
     (H->Flags&~CR_UART_HANDOFF_ALL_FLAGS) || Zero(H->CoverageDigest,32) ||
     !H->BuildRuntimeCount || H->BuildRuntimeCount>CR_UART_HANDOFF_IMAGES ||
     H->LoadedRuntimeCount>CR_UART_HANDOFF_IMAGES || H->LoadedGatedCount>H->LoadedRuntimeCount ||
     H->Bindings<H->RuntimeBindings || H->Active>1 ||
     (H->GateState!=CR_UART_ADMISSION_OPEN && H->GateState!=CR_UART_ADMISSION_CLOSED) ||
     H->Checksum!=(UINT64)CrUartHandoffChecksum(H))return CR_INVALID_PARAMETER;
  UINT32 Gated=0,Unique=0;BOOLEAN Known=H->LoadedRuntimeCount!=0;
  for(UINT32 I=0;I<H->LoadedRuntimeCount;++I){
    CONST CR_UART_FIRMWARE_IMAGE *P=&H->Images[I];
    if((Zero(P->Guid,16) && P->SerialKind!=CR_UART_IMAGE_UNKNOWN) || P->Reserved || P->SerialKind>CR_UART_IMAGE_UNKNOWN ||
       (I && Compare(H->Images[I-1].Guid,P->Guid)>0) ||
       (I && !Compare(H->Images[I-1].Guid,P->Guid) && H->Images[I-1].SerialKind!=P->SerialKind))
      return CR_INVALID_PARAMETER;
    if(P->SerialKind==CR_UART_IMAGE_GATED_SERIAL)++Gated;
    if(P->SerialKind==CR_UART_IMAGE_UNKNOWN)Known=FALSE;
    if(!I || Compare(H->Images[I-1].Guid,P->Guid))++Unique;
  }
  if(((H->Flags&CR_UART_HANDOFF_LOADED_COVERAGE) && Unique>H->BuildRuntimeCount) ||
     Gated!=H->LoadedGatedCount || !Zero(H->Images+H->LoadedRuntimeCount,
     (CR_UART_HANDOFF_IMAGES-H->LoadedRuntimeCount)*sizeof(H->Images[0])) ||
     ((H->Flags&CR_UART_HANDOFF_LOADED_COVERAGE) && (!Known || H->RuntimeBindings<Gated)) ||
     ((H->Flags&CR_UART_HANDOFF_CLOSED) && (H->GateState!=CR_UART_ADMISSION_CLOSED || H->Active)) ||
     ((H->Flags&CR_UART_HANDOFF_HARDWARE_RETIRED) && !Idle(H)) ||
     ((H->Flags&CR_UART_HANDOFF_RESERVED_MEMORY) && !Reserved(H,Physical)))return CR_INVALID_PARAMETER;
  if(H->Phase==CR_UART_HANDOFF_RETIRED &&
     (H->Flags!=CR_UART_HANDOFF_ALL_FLAGS || H->NativeExitStatus))return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
CR_STATUS CrUartHandoffCanClaim(CONST CR_UART_HANDOFF *H,UINT64 Physical,UINT64 Generation) {
  CR_STATUS S=CrUartHandoffValidate(H,Physical,Generation);
  return S==CR_SUCCESS && H->Phase!=CR_UART_HANDOFF_RETIRED ? CR_NOT_READY : S;
}
CR_STATUS CrUartHandoffSnapshot(CR_UART_HANDOFF *Out,CONST volatile CR_UART_HANDOFF *Source,
    UINT64 Physical,UINT64 Generation) {
  if(!Out || !Source || ((UINTN)Source&7) ||
     (UINTN)Out>~(UINTN)0-sizeof(*Out) || (UINTN)Source>~(UINTN)0-sizeof(*Source) ||
     ((UINTN)Out<(UINTN)Source+sizeof(*Source) && (UINTN)Source<(UINTN)Out+sizeof(*Out)))
    return CR_INVALID_PARAMETER;
  UINT64 Before=Source->PublishSequence;CrMemoryFence();
  if(Before!=2)return CR_NOT_READY;
  CR_UART_HANDOFF H;UINT8 *D=(UINT8 *)&H;CONST volatile UINT8 *P=(CONST volatile UINT8 *)Source;
  for(UINTN I=0;I<sizeof(H);++I)D[I]=P[I];CrMemoryFence();
  if(Source->PublishSequence!=Before || H.PublishSequence!=Before)return CR_NOT_READY;
  CR_STATUS S=CrUartHandoffValidate(&H,Physical,Generation);
  if(S==CR_SUCCESS)cr_memcpy(Out,&H,sizeof(H));return S;
}
