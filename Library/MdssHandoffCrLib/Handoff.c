/* Data-only immutable display ownership record. SPDX-License-Identifier: MIT */
#include <Library/MdssHandoffCrLib.h>
#include <oskal/cr_memory.h>

STATIC VOID Copy(VOID *Out,CONST VOID *In,UINTN Bytes) {
  UINT8 *D=Out;CONST UINT8 *S=In;for(UINTN I=0;I<Bytes;++I)D[I]=S[I];
}
STATIC BOOLEAN Zero(CONST VOID *Data,UINTN Bytes) {
  CONST UINT8 *P=Data;for(UINTN I=0;I<Bytes;++I)if(P[I])return FALSE;return TRUE;
}
STATIC BOOLEAN Owner(UINT32 O) {return O && !(O&(O-1U)) && !(O&~CR_MDSS_NATIVE_ALL_OWNERS);}
STATIC BOOLEAN Flags(UINT32 F) {return !(F&~(CR_MDSS_NATIVE_OWNED|CR_MDSS_NATIVE_RETAINED_ERROR));}
STATIC BOOLEAN Range(UINT64 Base,UINT64 Bytes) {return Base && Bytes && Base<=CR_MAX_UINT64-Bytes;}
STATIC BOOLEAN Within(UINT64 Base,UINT64 Bytes,UINT64 Parent,UINT64 Span) {
  return Range(Base,Bytes) && Range(Parent,Span) && Base>=Parent && Base-Parent<=Span && Bytes<=Span-(Base-Parent);
}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 N,UINT64 B,UINT64 M) {
  return Range(A,N) && Range(B,M) && A<B+M && B<A+N;
}
STATIC BOOLEAN Name(CONST CHAR8 *P,UINTN N) {
  if(!N || !P[0])return FALSE;
  UINTN I=0;for(;I<N && P[I];++I)if((UINT8)P[I]<0x20 || (UINT8)P[I]>0x7e)return FALSE;
  return I<N && Zero(P+I,N-I);
}
STATIC BOOLEAN Equal(CONST VOID *A,CONST VOID *B,UINTN N) {
  CONST UINT8 *X=A,*Y=B;for(UINTN I=0;I<N;++I)if(X[I]!=Y[I])return FALSE;return TRUE;
}
STATIC UINT64 Rounded(UINT64 Bytes) {return Bytes && Bytes<=CR_MAX_UINT64-4095 ? (Bytes+4095)&~4095ULL : 0;}
STATIC BOOLEAN Mode(CONST CR_MDSS_NATIVE_ROUTE *R) {
  CONST CR_MDSS_NATIVE_MODE *M=&R->Mode;
  return M->Width && M->Height && M->Width<=65535 && M->Height<=65535 &&
    M->Stride%4==0 && (UINT64)M->Width*4<=M->Stride && M->PixelHz &&
    M->Format==CR_MDSS_NATIVE_FORMAT_BGRX8888 &&
    (UINT64)M->Width+M->HFront+M->HSync+M->HBack<=65535 &&
    (UINT64)M->Height+M->VFront+M->VSync+M->VBack<=65535 &&
    M->HSync && M->VSync && Range(R->FrameBuffer,R->FrameBufferBytes) &&
    !(R->FrameBuffer&4095) && (UINT64)M->Stride*M->Height<=R->FrameBufferBytes;
}
STATIC BOOLEAN Windows(CONST CR_MDSS_NATIVE_ROUTE *R) {
  /* The qualified takeover is the original HDK DSI0/DP0 topology. DSI1
     may be retained as an UNCERTAIN diagnostic with its actual INTF2
     address, including real revoked SMMU evidence. This is deliberately
     not a new claimable route or a bonded/DSC wire representation. */
  UINT32 LastController=R->Owner==CR_MDSS_NATIVE_DSI && R->State==CR_MDSS_NATIVE_UNCERTAIN?1:0;
  if(R->Dpu!=0x0ae01000ULL || R->DpuBytes!=0x8f000U || R->Controller>LastController)return FALSE;
  UINT64 Addresses[4]={R->InterfaceBase,R->PipeBase,R->CtlBase,R->MixerBase};
  UINT32 Spans[4]={R->InterfaceBytes,R->PipeBytes,R->CtlBytes,R->MixerBytes};
  UINT32 Expected[4];
  if(R->Owner==CR_MDSS_NATIVE_DSI){Expected[0]=0x35000+R->Controller*0x1000;Expected[1]=0x24000;Expected[2]=0x15000;Expected[3]=0x44000;}
  else{Expected[0]=0x34000;Expected[1]=0x26000;Expected[2]=0x16000;Expected[3]=0x46000;}
  for(UINT32 I=0;I<4;++I)if(Addresses[I]!=R->Dpu+Expected[I] || Spans[I]<0x400 ||
      (Spans[I]&3) || !Within(Addresses[I],Spans[I],R->Dpu,R->DpuBytes))return FALSE;
  if((R->LinkBase|R->PhyBase|R->PllBase|R->LinkBytes|R->PhyBytes|R->PllBytes)&3)return FALSE;
  return Range(R->LinkBase,R->LinkBytes) && Range(R->PhyBase,R->PhyBytes) && Range(R->PllBase,R->PllBytes);
}
STATIC BOOLEAN Evidence(CONST SmmuMappingEvidence *E,UINT32 State) {
  if(E->Base!=0x15000000ULL || !Within(E->BankBase,0x800,E->Base,0x100000) ||
     (E->BankBase&4095) || !Range(E->Tables,E->TableBytes) || (E->Tables&4095) ||
     E->TableBytes!=SMMU_TABLE_PAGES*SMMU_PAGE_SIZE || E->Bank>=256 || E->Stream>=128 ||
     !E->Asid || E->Asid>=256 || E->Smr!=(0x80000000U|E->Sid) || E->S2cr!=E->Bank ||
     (E->SctlrIgnoreMask&~0x1000U) || (E->Sctlr&~E->SctlrIgnoreMask)!=(0x1027U&~E->SctlrIgnoreMask) ||
     E->Cbar!=0x1f300 || E->Cba2r!=1 || (E->Tcr&~63U)!=0x803500U ||
     (E->Tcr&63U)<25 || (E->Tcr&63U)>32 ||
     (E->Tcr2&0x38010U)!=0x38010U || (E->Tcr2&15U)>5 ||
     E->Ttbr0!=(E->Tables|((UINT64)E->Asid<<48)) || E->Ttbr1!=((UINT64)E->Asid<<48) ||
     E->Mair0!=0x44 || E->Mair1 || !Range(E->Iova,E->Bytes) ||
     ((E->Iova|E->Bytes)&4095) || !Within(E->Iova,E->Bytes,SMMU_IOVA_BASE,SMMU_IOVA_SIZE))return FALSE;
  if(E->Sid!=0x2800 && E->Sid!=0x2802 && E->Sid!=0x2c00 && E->Sid!=0x2c02)return FALSE;
  return State==CR_MDSS_NATIVE_MAPPING_PRESERVED ?
    E->Access==SMMU_ACCESS_READ && Range(E->Physical,E->Bytes) && !(E->Physical&4095) :
    State==CR_MDSS_NATIVE_MAPPING_REVOKED && !E->Access && !E->Physical;
}
STATIC BOOLEAN Mapping(UINT32 State,UINT32 Id,UINT32 Count,UINT64 Status,CONST SmmuMappingEvidence *E) {
  if(State>CR_MDSS_NATIVE_MAPPING_NONE || Count>CR_MDSS_NATIVE_STREAMS ||
     !Zero(E+Count,(CR_MDSS_NATIVE_STREAMS-Count)*sizeof(*E)))return FALSE;
  if(State==CR_MDSS_NATIVE_MAPPING_UNKNOWN)return Count==0;
  if(State==CR_MDSS_NATIVE_MAPPING_NONE)return !Id && !Count && !Status;
  if(!Id || !Count || Status)return FALSE;
  for(UINT32 I=0;I<Count;++I){if(!Evidence(&E[I],State))return FALSE;
    for(UINT32 J=0;J<I;++J)if(E[I].Sid==E[J].Sid || E[I].Bank==E[J].Bank ||
       E[I].Stream==E[J].Stream || E[I].Asid==E[J].Asid ||
       Overlap(E[I].Tables,E[I].TableBytes,E[J].Tables,E[J].TableBytes))return FALSE;}
  return TRUE;
}
STATIC BOOLEAN Route(CONST CR_MDSS_NATIVE_ROUTE *R,UINT32 Expected,BOOLEAN Final) {
  if(R->Owner!=Expected || R->State>CR_MDSS_NATIVE_UNCERTAIN || !Flags(R->Flags) || R->Reserved)return FALSE;
  if(R->State==CR_MDSS_NATIVE_LIVE && !(R->Flags&CR_MDSS_NATIVE_OWNED))return FALSE;
  if(R->MappingOwner){if(R->MappingOwner!=CR_MDSS_NATIVE_SMMU ||
      !Mapping(R->MappingState,R->MappingId,R->StreamCount,R->MappingStatus,R->Streams))return FALSE;}
  else if(Final || R->MappingState || R->MappingStatus || R->StreamCount || !Zero(R->Streams,sizeof(R->Streams)))return FALSE;
  if(R->State==CR_MDSS_NATIVE_ABSENT || (R->State==CR_MDSS_NATIVE_STOPPED && !R->Mode.Width)){
    CR_MDSS_NATIVE_ROUTE Empty=*R;Empty.Owner=Empty.State=Empty.MappingOwner=Empty.MappingState=0;
    return Zero(&Empty,sizeof(Empty));
  }
  if(R->State==CR_MDSS_NATIVE_UNCERTAIN && !R->Mode.Width){
    if(!Zero(&R->Mode,sizeof(R->Mode)) ||
       (R->MappingState!=CR_MDSS_NATIVE_MAPPING_UNKNOWN && R->MappingState!=CR_MDSS_NATIVE_MAPPING_NONE))return FALSE;
    UINT64 Bases[8]={R->Dpu,R->InterfaceBase,R->PipeBase,R->CtlBase,R->MixerBase,R->LinkBase,R->PhyBase,R->PllBase};
    UINT32 Sizes[8]={R->DpuBytes,R->InterfaceBytes,R->PipeBytes,R->CtlBytes,R->MixerBytes,R->LinkBytes,R->PhyBytes,R->PllBytes};
    for(UINT32 I=0;I<8;++I)if((Bases[I] || Sizes[I]) && !Range(Bases[I],Sizes[I]))return FALSE;
    return (!R->FrameBuffer && !R->FrameBufferBytes) || Range(R->FrameBuffer,R->FrameBufferBytes);
  }
  if(!Mode(R) || !Windows(R))return FALSE;
  if(R->MappingOwner && R->MappingState!=CR_MDSS_NATIVE_MAPPING_UNKNOWN && R->MappingState!=CR_MDSS_NATIVE_MAPPING_NONE){
    UINT64 Bytes=Rounded(R->FrameBufferBytes);if(!Bytes || !R->DeviceAddress)return FALSE;
    for(UINT32 I=0;I<R->StreamCount;++I)if(R->Streams[I].Iova!=R->DeviceAddress || R->Streams[I].Bytes!=Bytes ||
        (R->MappingState==CR_MDSS_NATIVE_MAPPING_PRESERVED && R->Streams[I].Physical!=R->FrameBuffer))return FALSE;
  }
  if(R->MappingState==CR_MDSS_NATIVE_MAPPING_NONE && R->DeviceAddress)return FALSE;
  return TRUE;
}
STATIC BOOLEAN Bridge(CONST CR_MDSS_NATIVE_BRIDGE_STATE *B) {
  if(B->State>CR_MDSS_NATIVE_UNCERTAIN || !Flags(B->Flags) || B->Reserved)return FALSE;
  if(B->State==CR_MDSS_NATIVE_ABSENT)return Zero(B,sizeof(*B));
  if(B->I2cInstance!=10 || B->I2cAddress!=0x2b || B->I2cRateHz!=400000 || B->StretchUs!=500 ||
     B->PowerGpio0!=9 || B->PowerGpio1!=109 || B->ResetGpio!=107 || B->IrqGpio!=44 ||
     (B->HighPins&~7U))return FALSE;
  return B->State!=CR_MDSS_NATIVE_LIVE || (B->Flags&CR_MDSS_NATIVE_OWNED);
}
STATIC BOOLEAN Phy(CONST CR_MDSS_NATIVE_PHY_STATE *P) {
  if(P->State>CR_MDSS_NATIVE_UNCERTAIN || !Flags(P->Flags) || P->Reserved ||
     P->UsbOwners>1 || P->DpOwners>1 || (P->ReadyMask&~CR_MDSS_NATIVE_PHY_READY_MASK) ||
     (P->TouchedMask&~CR_MDSS_NATIVE_PHY_TOUCHED_MASK))return FALSE;
  if(P->State==CR_MDSS_NATIVE_ABSENT)return Zero(P,sizeof(*P));
  if(P->Base!=0x088e8000ULL || P->Bytes!=0x4000 || P->Orientation>2 ||
     (P->LaneCount!=0 && P->LaneCount!=1 && P->LaneCount!=2 && P->LaneCount!=4) ||
     !(P->Flags&CR_MDSS_NATIVE_OWNED))return FALSE;
  if(P->Status && (!(P->Status&(1ULL<<63)) || !(P->Flags&CR_MDSS_NATIVE_RETAINED_ERROR) ||
     P->State!=CR_MDSS_NATIVE_UNCERTAIN))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_USB) && (!P->UsbOwners || !(P->ReadyMask&CR_MDSS_NATIVE_PHY_COMMON)))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_DP) && (!P->DpOwners || !(P->ReadyMask&CR_MDSS_NATIVE_PHY_COMMON)))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_USB_SS) && !(P->ReadyMask&CR_MDSS_NATIVE_PHY_USB))return FALSE;
  if((P->TouchedMask&CR_MDSS_NATIVE_PHY_USB) && !P->UsbOwners)return FALSE;
  if((P->TouchedMask&CR_MDSS_NATIVE_PHY_DP) && !P->DpOwners)return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_COMMON) && !(P->TouchedMask&CR_MDSS_NATIVE_PHY_COMMON))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_USB) && !(P->TouchedMask&CR_MDSS_NATIVE_PHY_USB))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_DP) && !(P->TouchedMask&CR_MDSS_NATIVE_PHY_DP))return FALSE;
  if((P->ReadyMask&CR_MDSS_NATIVE_PHY_COMMON) ? !P->Orientation : (P->Orientation || P->LaneCount || P->LinkRate))return FALSE;
  if(P->LinkRate && ((P->LinkRate!=1620 && P->LinkRate!=2700 && P->LinkRate!=5400 && P->LinkRate!=8100) ||
     !P->LaneCount || !(P->ReadyMask&CR_MDSS_NATIVE_PHY_DP)))return FALSE;
  return P->State!=CR_MDSS_NATIVE_LIVE ||
    ((P->ReadyMask&CR_MDSS_NATIVE_PHY_COMMON) && (P->ReadyMask&CR_MDSS_NATIVE_PHY_FSA) &&
     (P->ReadyMask&(CR_MDSS_NATIVE_PHY_USB|CR_MDSS_NATIVE_PHY_DP)) && !P->Status &&
     !(P->Flags&CR_MDSS_NATIVE_RETAINED_ERROR));
}
STATIC BOOLEAN Clock(CONST CR_MDSS_NATIVE_CLOCK *C,UINT32 O) {
  return C->Owner==O && Flags(C->Flags) && (C->Flags&CR_MDSS_NATIVE_OWNED) &&
    Name(C->Controller,sizeof(C->Controller)) && Name(C->Name,sizeof(C->Name));
}
STATIC BOOLEAN Rail(CONST CR_MDSS_NATIVE_RAIL *R,UINT32 O) {
  return R->Owner==O && Flags(R->Flags) && (R->Flags&CR_MDSS_NATIVE_OWNED) &&
    Name(R->Name,sizeof(R->Name)) && R->Millivolts && R->Mode<=7;
}
STATIC BOOLEAN Path(CONST CR_MDSS_NATIVE_PATH *P,UINT32 O) {
  return P->Owner==O && Flags(P->Flags) && (P->Flags&CR_MDSS_NATIVE_OWNED) &&
    Name(P->Provider,sizeof(P->Provider)) && P->Source!=P->Destination && P->Peak>=P->Average;
}
STATIC BOOLEAN Domain(CONST CR_MDSS_NATIVE_DOMAIN *D,UINT32 O) {
  return D->Owner==O && Flags(D->Flags) && (D->Flags&CR_MDSS_NATIVE_OWNED) &&
    Name(D->Controller,sizeof(D->Controller)) && Name(D->Name,sizeof(D->Name));
}
#define ARRAYS(P,Pred,Field,Count,Capacity,O) do { \
  if((P)->Count>(Capacity) || !Zero((P)->Field+(P)->Count,((Capacity)-(P)->Count)*sizeof((P)->Field[0])))return FALSE; \
  for(UINT32 I=0;I<(P)->Count;++I)if(!Pred(&(P)->Field[I],O))return FALSE; \
} while(0)
STATIC BOOLEAN Demands(CONST CR_MDSS_HANDOFF_PARTICIPANT *P) {
  ARRAYS(P,Clock,Clocks,ClockCount,CR_MDSS_NATIVE_CLOCKS,P->Owner);
  ARRAYS(P,Rail,Rails,RailCount,CR_MDSS_NATIVE_RAILS,P->Owner);
  ARRAYS(P,Path,Paths,PathCount,CR_MDSS_NATIVE_PATHS,P->Owner);
  ARRAYS(P,Domain,Domains,DomainCount,CR_MDSS_NATIVE_DOMAINS,P->Owner);
  for(UINT32 I=0;I<P->ClockCount;++I)for(UINT32 J=0;J<I;++J)
    if(Equal(P->Clocks[I].Controller,P->Clocks[J].Controller,8) && Equal(P->Clocks[I].Name,P->Clocks[J].Name,48))return FALSE;
  for(UINT32 I=0;I<P->RailCount;++I)for(UINT32 J=0;J<I;++J)if(Equal(P->Rails[I].Name,P->Rails[J].Name,24))return FALSE;
  for(UINT32 I=0;I<P->PathCount;++I)for(UINT32 J=0;J<I;++J)
    if(P->Paths[I].Source==P->Paths[J].Source && P->Paths[I].Destination==P->Paths[J].Destination && Equal(P->Paths[I].Provider,P->Paths[J].Provider,24))return FALSE;
  for(UINT32 I=0;I<P->DomainCount;++I)for(UINT32 J=0;J<I;++J)
    if(Equal(P->Domains[I].Controller,P->Domains[J].Controller,8) && Equal(P->Domains[I].Name,P->Domains[J].Name,32))return FALSE;
  return TRUE;
}
STATIC BOOLEAN Header(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  return R && R->Signature==CR_MDSS_NATIVE_SIGNATURE && R->Revision==CR_MDSS_NATIVE_REVISION &&
    R->Size==sizeof(*R) && R->Platform==CR_MDSS_NATIVE_PLATFORM_SM8450_HDK &&
    R->RequiredOwners==CR_MDSS_NATIVE_ALL_OWNERS && !(R->RegisteredOwners&~R->RequiredOwners) &&
    !(R->FrozenOwners&~R->RegisteredOwners) && !R->Reserved0 && !R->Reserved1 &&
    R->Generation && R->TransferId && Range(R->RecordPhysical,R->RecordBytes) &&
    !(R->RecordPhysical&4095) && !(R->RecordBytes&4095) && R->RecordBytes>=sizeof(*R);
}
STATIC BOOLEAN Slice(UINT32 First,UINT32 Count,UINT32 Total) {return First<=Total && Count<=Total-First && (Count || !First);}
STATIC BOOLEAN Slices(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  UINT32 Used[4]={0};
  for(UINT32 N=0;N<CR_MDSS_NATIVE_ROUTES;++N){CONST CR_MDSS_NATIVE_ROUTE *P=&R->Routes[N];
    UINT32 First[4]={P->ClockFirst,P->RailFirst,P->PathFirst,P->DomainFirst};
    UINT32 Count[4]={P->ClockCount,P->RailCount,P->PathCount,P->DomainCount};
    UINT32 Total[4]={R->ClockCount,R->RailCount,R->PathCount,R->DomainCount};
    for(UINT32 K=0;K<4;++K){if(!Slice(First[K],Count[K],Total[K]))return FALSE;
      for(UINT32 I=0;I<Count[K];++I){UINT32 At=First[K]+I,O;
        if(Used[K]&(1U<<At))return FALSE;Used[K]|=1U<<At;
        O=K==0?R->Clocks[At].Owner:K==1?R->Rails[At].Owner:K==2?R->Paths[At].Owner:R->Domains[At].Owner;
        if(O!=P->Owner)return FALSE;}}
  }
  for(UINT32 K=0;K<4;++K){UINT32 Count=K==0?R->ClockCount:K==1?R->RailCount:K==2?R->PathCount:R->DomainCount;
    for(UINT32 I=0;I<Count;++I){UINT32 O=K==0?R->Clocks[I].Owner:K==1?R->Rails[I].Owner:K==2?R->Paths[I].Owner:R->Domains[I].Owner;
      if((O==CR_MDSS_NATIVE_DSI || O==CR_MDSS_NATIVE_DP) && !(Used[K]&(1U<<I)))return FALSE;}}
  return TRUE;
}
STATIC BOOLEAN Schema(CONST CR_MDSS_NATIVE_HANDOFF *R,BOOLEAN Final) {
  if(!Header(R) || R->ClockCount>CR_MDSS_NATIVE_CLOCKS || R->RailCount>CR_MDSS_NATIVE_RAILS ||
     R->PathCount>CR_MDSS_NATIVE_PATHS || R->DomainCount>CR_MDSS_NATIVE_DOMAINS)return FALSE;
  if(!Zero(R->Clocks+R->ClockCount,(CR_MDSS_NATIVE_CLOCKS-R->ClockCount)*sizeof(R->Clocks[0])) ||
     !Zero(R->Rails+R->RailCount,(CR_MDSS_NATIVE_RAILS-R->RailCount)*sizeof(R->Rails[0])) ||
     !Zero(R->Paths+R->PathCount,(CR_MDSS_NATIVE_PATHS-R->PathCount)*sizeof(R->Paths[0])) ||
     !Zero(R->Domains+R->DomainCount,(CR_MDSS_NATIVE_DOMAINS-R->DomainCount)*sizeof(R->Domains[0])))return FALSE;
  for(UINT32 K=0;K<4;++K){UINT32 Count=K==0?R->ClockCount:K==1?R->RailCount:K==2?R->PathCount:R->DomainCount;
    for(UINT32 I=0;I<Count;++I){UINT32 O=K==0?R->Clocks[I].Owner:K==1?R->Rails[I].Owner:K==2?R->Paths[I].Owner:R->Domains[I].Owner;
      if(!Owner(O) || !(R->FrozenOwners&O) || !(K==0?Clock(&R->Clocks[I],O):K==1?Rail(&R->Rails[I],O):K==2?Path(&R->Paths[I],O):Domain(&R->Domains[I],O)))return FALSE;
      for(UINT32 J=0;J<I;++J){UINT32 Other=K==0?R->Clocks[J].Owner:K==1?R->Rails[J].Owner:K==2?R->Paths[J].Owner:R->Domains[J].Owner;
        if(Other!=O)continue;
        if(K==0 && Equal(R->Clocks[I].Controller,R->Clocks[J].Controller,8) && Equal(R->Clocks[I].Name,R->Clocks[J].Name,48))return FALSE;
        if(K==1 && Equal(R->Rails[I].Name,R->Rails[J].Name,24))return FALSE;
        if(K==2 && R->Paths[I].Source==R->Paths[J].Source && R->Paths[I].Destination==R->Paths[J].Destination && Equal(R->Paths[I].Provider,R->Paths[J].Provider,24))return FALSE;
        if(K==3 && Equal(R->Domains[I].Controller,R->Domains[J].Controller,8) && Equal(R->Domains[I].Name,R->Domains[J].Name,32))return FALSE;}}}
  if(Final && (R->FrozenOwners!=R->RequiredOwners || R->RegisteredOwners!=R->RequiredOwners))return FALSE;
  for(UINT32 I=0;I<2;++I){UINT32 O=I==0?CR_MDSS_NATIVE_DSI:CR_MDSS_NATIVE_DP;
    if(R->FrozenOwners&O){if(!Route(&R->Routes[I],O,Final))return FALSE;}
    else{CR_MDSS_NATIVE_ROUTE P=R->Routes[I];
      if(R->FrozenOwners&CR_MDSS_NATIVE_SMMU){if(P.MappingOwner!=CR_MDSS_NATIVE_SMMU ||
          !Mapping(P.MappingState,P.MappingId,P.StreamCount,P.MappingStatus,P.Streams))return FALSE;}
      else if(P.MappingId || P.StreamCount || P.MappingState || P.MappingOwner || P.MappingStatus || !Zero(P.Streams,sizeof(P.Streams)))return FALSE;
      P.MappingId=P.StreamCount=P.MappingState=P.MappingOwner=0;P.MappingStatus=0;
      for(UINTN B=0;B<sizeof(P.Streams);++B)((UINT8 *)P.Streams)[B]=0;
      if(!Zero(&P,sizeof(P)))return FALSE;}}
  return Slices(R) && Bridge(&R->Bridge) && Phy(&R->Phy);
}
CR_STATUS CrMdssHandoffInit(CR_MDSS_NATIVE_HANDOFF *R,UINT64 Generation,UINT64 Transfer,
    UINT64 Physical,UINT64 Bytes,UINT32 Required,UINT32 Registered) {
  if(!R || !Generation || !Transfer || !Range(Physical,Bytes) || (Physical&4095) || (Bytes&4095) ||
      Bytes<sizeof(*R) || Required!=CR_MDSS_NATIVE_ALL_OWNERS || (Registered&~Required))return CR_INVALID_PARAMETER;
  if(!Zero(R,sizeof(*R)))return CR_ALREADY_STARTED;
  R->Signature=CR_MDSS_NATIVE_SIGNATURE;R->Revision=CR_MDSS_NATIVE_REVISION;R->Size=sizeof(*R);
  R->Sequence=1;R->Platform=CR_MDSS_NATIVE_PLATFORM_SM8450_HDK;R->RequiredOwners=Required;
  R->RegisteredOwners=Registered;R->Generation=Generation;R->TransferId=Transfer;R->RecordPhysical=Physical;R->RecordBytes=Bytes;
  return CR_SUCCESS;
}
CR_STATUS CrMdssHandoffAppend(CR_MDSS_NATIVE_HANDOFF *R,CONST CR_MDSS_HANDOFF_PARTICIPANT *P) {
  if(!R || !P)return CR_INVALID_PARAMETER;
  if(!Schema(R,FALSE) || R->Phase!=CR_MDSS_NATIVE_PENDING || !(R->Sequence&1) || R->Crc32)return CR_COMPROMISED_DATA;
  if(!Owner(P->Owner) || !(R->RegisteredOwners&P->Owner) || P->Reserved0 || P->Reserved1 || P->Reserved2 || !Demands(P))return CR_INVALID_PARAMETER;
  if(R->FrozenOwners&P->Owner)return CR_ALREADY_STARTED;
  if(P->ClockCount>CR_MDSS_NATIVE_CLOCKS-R->ClockCount || P->RailCount>CR_MDSS_NATIVE_RAILS-R->RailCount ||
     P->PathCount>CR_MDSS_NATIVE_PATHS-R->PathCount || P->DomainCount>CR_MDSS_NATIVE_DOMAINS-R->DomainCount)return CR_OUT_OF_RESOURCES;
  UINT32 At=P->Owner==CR_MDSS_NATIVE_DP?1:0;
  if(P->Owner==CR_MDSS_NATIVE_DSI || P->Owner==CR_MDSS_NATIVE_DP){
    CONST CR_MDSS_NATIVE_ROUTE *Q=&P->Route;
    if(!Route(Q,P->Owner,FALSE) || Q->MappingId || Q->MappingOwner || Q->ClockFirst || Q->ClockCount || Q->RailFirst || Q->RailCount ||
       Q->PathFirst || Q->PathCount || Q->DomainFirst || Q->DomainCount || !Zero(P->Mappings,sizeof(P->Mappings)) ||
       !Zero(&P->Bridge,sizeof(P->Bridge)) || !Zero(&P->Phy,sizeof(P->Phy)))return CR_INVALID_PARAMETER;
    if((Q->State==CR_MDSS_NATIVE_ABSENT || (Q->State==CR_MDSS_NATIVE_STOPPED && !Q->Mode.Width)) &&
        (P->ClockCount || P->RailCount || P->PathCount || P->DomainCount))return CR_INVALID_PARAMETER;
    if(R->FrozenOwners&CR_MDSS_NATIVE_SMMU){CR_MDSS_NATIVE_ROUTE Test=*Q;
      CONST CR_MDSS_NATIVE_ROUTE *M=&R->Routes[At];Test.MappingId=M->MappingId;Test.MappingOwner=M->MappingOwner;Test.MappingState=M->MappingState;
      Test.MappingStatus=M->MappingStatus;Test.StreamCount=M->StreamCount;Copy(Test.Streams,M->Streams,sizeof(Test.Streams));
      if(!Route(&Test,P->Owner,TRUE))return CR_INVALID_PARAMETER;}
  }else if(!Zero(&P->Route,sizeof(P->Route)))return CR_INVALID_PARAMETER;
  if(P->Owner==CR_MDSS_NATIVE_SMMU){
    if(P->ClockCount || P->RailCount || P->PathCount || P->DomainCount || !Zero(&P->Bridge,sizeof(P->Bridge)) || !Zero(&P->Phy,sizeof(P->Phy)))return CR_INVALID_PARAMETER;
    for(UINT32 I=0;I<2;++I){CONST CR_MDSS_NATIVE_MAPPING *M=&P->Mappings[I];UINT32 O=I==0?CR_MDSS_NATIVE_DSI:CR_MDSS_NATIVE_DP;
      if(M->Owner!=O || !Mapping(M->State,M->MappingId,M->StreamCount,M->Status,M->Streams))return CR_INVALID_PARAMETER;}
    for(UINT32 I=0;I<2;++I){UINT32 O=I==0?CR_MDSS_NATIVE_DSI:CR_MDSS_NATIVE_DP;
      if(R->FrozenOwners&O){CR_MDSS_NATIVE_ROUTE Test=R->Routes[I];CONST CR_MDSS_NATIVE_MAPPING *M=&P->Mappings[I];
        Test.MappingId=M->MappingId;Test.MappingOwner=P->Owner;Test.MappingState=M->State;Test.MappingStatus=M->Status;Test.StreamCount=M->StreamCount;
        Copy(Test.Streams,M->Streams,sizeof(Test.Streams));if(!Route(&Test,O,TRUE))return CR_INVALID_PARAMETER;}}
  }else if(!Zero(P->Mappings,sizeof(P->Mappings)))return CR_INVALID_PARAMETER;
  if(P->Owner==CR_MDSS_NATIVE_BRIDGE){if(!Bridge(&P->Bridge) || !Zero(&P->Phy,sizeof(P->Phy)))return CR_INVALID_PARAMETER;}
  else if(!Zero(&P->Bridge,sizeof(P->Bridge)))return CR_INVALID_PARAMETER;
  if(P->Owner==CR_MDSS_NATIVE_PHY){if(!Phy(&P->Phy))return CR_INVALID_PARAMETER;}
  else if(!Zero(&P->Phy,sizeof(P->Phy)))return CR_INVALID_PARAMETER;
  /* All fallible checks precede the caller-owned record mutation. */
  if(P->Owner==CR_MDSS_NATIVE_DSI || P->Owner==CR_MDSS_NATIVE_DP){
    CR_MDSS_NATIVE_ROUTE MappingPart=R->Routes[At];R->Routes[At]=P->Route;
    if(R->FrozenOwners&CR_MDSS_NATIVE_SMMU){R->Routes[At].MappingId=MappingPart.MappingId;R->Routes[At].MappingOwner=MappingPart.MappingOwner;R->Routes[At].MappingState=MappingPart.MappingState;
      R->Routes[At].MappingStatus=MappingPart.MappingStatus;R->Routes[At].StreamCount=MappingPart.StreamCount;Copy(R->Routes[At].Streams,MappingPart.Streams,sizeof(MappingPart.Streams));}
    CR_MDSS_NATIVE_ROUTE *Q=&R->Routes[At];
    Q->ClockFirst=P->ClockCount?R->ClockCount:0;Q->ClockCount=P->ClockCount;Q->RailFirst=P->RailCount?R->RailCount:0;Q->RailCount=P->RailCount;
    Q->PathFirst=P->PathCount?R->PathCount:0;Q->PathCount=P->PathCount;Q->DomainFirst=P->DomainCount?R->DomainCount:0;Q->DomainCount=P->DomainCount;
  }else if(P->Owner==CR_MDSS_NATIVE_SMMU){for(UINT32 I=0;I<2;++I){CONST CR_MDSS_NATIVE_MAPPING *M=&P->Mappings[I];CR_MDSS_NATIVE_ROUTE *Q=&R->Routes[I];
    Q->MappingId=M->MappingId;Q->MappingOwner=P->Owner;Q->MappingState=M->State;Q->MappingStatus=M->Status;Q->StreamCount=M->StreamCount;Copy(Q->Streams,M->Streams,sizeof(M->Streams));}}
  else if(P->Owner==CR_MDSS_NATIVE_BRIDGE)R->Bridge=P->Bridge;
  else if(P->Owner==CR_MDSS_NATIVE_PHY)R->Phy=P->Phy;
  Copy(R->Clocks+R->ClockCount,P->Clocks,P->ClockCount*sizeof(P->Clocks[0]));R->ClockCount+=P->ClockCount;
  Copy(R->Rails+R->RailCount,P->Rails,P->RailCount*sizeof(P->Rails[0]));R->RailCount+=P->RailCount;
  Copy(R->Paths+R->PathCount,P->Paths,P->PathCount*sizeof(P->Paths[0]));R->PathCount+=P->PathCount;
  Copy(R->Domains+R->DomainCount,P->Domains,P->DomainCount*sizeof(P->Domains[0]));R->DomainCount+=P->DomainCount;
  R->FrozenOwners|=P->Owner;return CR_SUCCESS;
}
UINT32 CrMdssHandoffCrc32(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  if(!R)return 0;CONST UINT8 *P=(CONST UINT8 *)R;UINT32 C=0xffffffffU;
  for(UINTN I=0;I<sizeof(*R);++I){UINT8 B=I>=16 && I<20?0:P[I];C^=B;
    for(UINT32 J=0;J<8;++J)C=(C>>1)^((C&1)?0xedb88320U:0);}
  return ~C;
}
CR_STATUS CrMdssHandoffFinalize(CR_MDSS_NATIVE_HANDOFF *R,UINT32 Sequence) {
  if(!R || !Sequence || (Sequence&1))return CR_INVALID_PARAMETER;
  if(R->Phase!=CR_MDSS_NATIVE_PENDING || !(R->Sequence&1) || R->Crc32)return CR_ALREADY_STARTED;
  if(!Schema(R,TRUE))return CR_NOT_READY;
  R->Phase=CR_MDSS_NATIVE_FINAL;R->Sequence=Sequence;R->Crc32=CrMdssHandoffCrc32(R);return CR_SUCCESS;
}
CR_STATUS CrMdssHandoffValidate(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  if(!R)return CR_INVALID_PARAMETER;
  if(R->Phase!=CR_MDSS_NATIVE_FINAL || !R->Sequence || (R->Sequence&1))return CR_NOT_READY;
  return Schema(R,TRUE) && R->Crc32==CrMdssHandoffCrc32(R) ? CR_SUCCESS : CR_COMPROMISED_DATA;
}
STATIC BOOLEAN CoreDomain(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  /* Both DSI and DP route through the single DISP_CC MDSS core GDSC.
     MdpSnapshotResources journals this exact retained native resource;
     DP has no separate domain lease and must not fabricate one. */
  STATIC CONST CHAR8 Controller[8]="dispcc";
  STATIC CONST CHAR8 Name[32]="disp_cc_mdss_core_gdsc";
  for(UINT32 I=0;I<R->DomainCount;++I)if(
      Equal(R->Domains[I].Controller,Controller,sizeof(Controller)) &&
      Equal(R->Domains[I].Name,Name,sizeof(Name)) &&
      R->Domains[I].Flags==CR_MDSS_NATIVE_OWNED)return TRUE;
  return FALSE;
}
CR_STATUS CrMdssHandoffCanClaim(CONST CR_MDSS_NATIVE_HANDOFF *R) {
  CR_STATUS S=CrMdssHandoffValidate(R);if(S!=CR_SUCCESS)return S;
  if(R->Bridge.State==CR_MDSS_NATIVE_UNCERTAIN || (R->Bridge.Flags&CR_MDSS_NATIVE_RETAINED_ERROR) ||
     R->Phy.State==CR_MDSS_NATIVE_UNCERTAIN || (R->Phy.Flags&CR_MDSS_NATIVE_RETAINED_ERROR))return CR_NOT_READY;
  for(UINT32 I=0;I<2;++I){CONST CR_MDSS_NATIVE_ROUTE *Q=&R->Routes[I];
    if(Q->State==CR_MDSS_NATIVE_UNCERTAIN || (Q->Flags&CR_MDSS_NATIVE_RETAINED_ERROR) || Q->MappingStatus ||
       Q->MappingState==CR_MDSS_NATIVE_MAPPING_UNKNOWN)return CR_NOT_READY;
    if(Q->State==CR_MDSS_NATIVE_LIVE && Q->MappingState!=CR_MDSS_NATIVE_MAPPING_PRESERVED)return CR_NOT_READY;
    if(Q->State!=CR_MDSS_NATIVE_LIVE && Q->MappingState==CR_MDSS_NATIVE_MAPPING_PRESERVED)return CR_NOT_READY;
    if((Q->MappingState==CR_MDSS_NATIVE_MAPPING_PRESERVED || Q->MappingState==CR_MDSS_NATIVE_MAPPING_REVOKED) && Q->StreamCount!=4)return CR_NOT_READY;
    if(Overlap(Q->FrameBuffer,Q->FrameBufferBytes,R->RecordPhysical,R->RecordBytes))return CR_COMPROMISED_DATA;
    for(UINT32 N=0;N<Q->StreamCount;++N)if(Overlap(Q->Streams[N].Tables,Q->Streams[N].TableBytes,R->RecordPhysical,R->RecordBytes) ||
      Overlap(Q->Streams[N].Tables,Q->Streams[N].TableBytes,Q->FrameBuffer,Q->FrameBufferBytes))return CR_COMPROMISED_DATA;
    if(Q->State==CR_MDSS_NATIVE_LIVE && (!Q->ClockCount || !CoreDomain(R) || !Q->PathCount ||
       (Q->Owner==CR_MDSS_NATIVE_DSI && (!Q->RailCount || R->Bridge.State!=CR_MDSS_NATIVE_LIVE))))return CR_NOT_READY;
  }
  for(UINT32 I=0;I<R->ClockCount;++I)if(R->Clocks[I].Flags&CR_MDSS_NATIVE_RETAINED_ERROR)return CR_NOT_READY;
  for(UINT32 I=0;I<R->RailCount;++I)if(R->Rails[I].Flags&CR_MDSS_NATIVE_RETAINED_ERROR)return CR_NOT_READY;
  for(UINT32 I=0;I<R->PathCount;++I)if(R->Paths[I].Flags&CR_MDSS_NATIVE_RETAINED_ERROR)return CR_NOT_READY;
  for(UINT32 I=0;I<R->DomainCount;++I)if(R->Domains[I].Flags&CR_MDSS_NATIVE_RETAINED_ERROR)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrMdssHandoffSnapshot(CONST volatile VOID *Record,UINTN Bytes,CR_MDSS_NATIVE_HANDOFF *Scratch,CR_MDSS_NATIVE_HANDOFF *Out) {
  if(!Record || !Scratch || !Out || Bytes<sizeof(*Scratch) ||
      ((UINTN)Record&3) || ((UINTN)Scratch&7) || ((UINTN)Out&7) ||
      (UINTN)Record>~(UINTN)0-sizeof(*Scratch) || (UINTN)Scratch>~(UINTN)0-sizeof(*Scratch) ||
      (UINTN)Out>~(UINTN)0-sizeof(*Out) ||
      Overlap((UINTN)Record,sizeof(*Scratch),(UINTN)Scratch,sizeof(*Scratch)) ||
      Overlap((UINTN)Record,sizeof(*Scratch),(UINTN)Out,sizeof(*Out)) ||
      Overlap((UINTN)Scratch,sizeof(*Scratch),(UINTN)Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  CONST volatile UINT32 *Sequence=(CONST volatile UINT32 *)((CONST volatile UINT8 *)Record+12);
  UINT32 Before=*Sequence;if(!Before || (Before&1))return CR_NOT_READY;CrMemoryFence();
  for(UINTN I=0;I<sizeof(*Scratch);++I)((UINT8 *)Scratch)[I]=((CONST volatile UINT8 *)Record)[I];
  CrMemoryFence();UINT32 After=*Sequence;
  if(Before!=After || Scratch->Sequence!=Before)return CR_NOT_READY;
  CR_STATUS S=CrMdssHandoffValidate(Scratch);if(S!=CR_SUCCESS)return S;
  Copy(Out,Scratch,sizeof(*Out));return CR_SUCCESS;
}
