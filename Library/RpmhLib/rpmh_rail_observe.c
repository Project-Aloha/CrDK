/* No cached vote can replace a completed remote response. MIT */
#include <Library/RpmhRailObserveLib.h>
#include <oskal/cr_mutex.h>

STATIC BOOLEAN Empty(CONST VOID *P,UINTN N) {
  for(UINTN I=0;I<N;++I)if(((CONST UINT8 *)P)[I])return FALSE;
  return TRUE;
}
STATIC BOOLEAN Separate(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return A && B && X<=~((UINTN)0)-An && Y<=~((UINTN)0)-Bn &&
         (X+An<=Y || Y+Bn<=X);
}
STATIC CR_STATUS SourceCurrent(CR_RPMH_RAIL_OBSERVATION_SOURCE *S,BOOLEAN Open) {
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(!S || S->Self!=S || !S->Core || !S->Generation || !S->Held || S->Closed ||
     (Open && !S->Open) || !S->Core->Initialized || !S->Core->ObservationReferences ||
     S->Core->drv_base_address!=S->Mapping || S->Core->drv_size!=S->MappingSize ||
     S->Core->drv_registers!=S->Registers || S->Core->tcs_offset!=S->TcsOffset || S->Core->drv_id!=S->DrvId)
    return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS Current(CR_RPMH_RAIL_OBSERVER *O,BOOLEAN Open) {
  if(!O || O->Self!=O || !O->Held || O->Closed || !O->Rail ||
     !O->Address || O->Rail->Address!=O->Address || O->Rail->Name!=O->Name)return CR_NOT_READY;
  CR_STATUS Status=SourceCurrent(O->Source,Open);if(Status!=CR_SUCCESS)return Status;
  if(O->Source->Generation!=O->Generation || O->Source->DrvId!=O->DrvId ||
     !O->Source->ObserverCount)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveSourceOpen(CR_RPMH_RAIL_OBSERVATION_SOURCE *S,
                                     RpmhDeviceContext *C,UINT64 Generation) {
  if(CrMutexCheckLevel()!=CR_SUCCESS || !S || !C || !Generation || !C->Initialized ||
     !Separate(S,sizeof(*S),C,sizeof(*C)))return CR_INVALID_PARAMETER;
  if(!Empty(S,sizeof(*S)))
    return SourceCurrent(S,TRUE)==CR_SUCCESS && S->Core==C && S->Generation==Generation?
      CR_SUCCESS:CR_NOT_READY;
  if(C->ObservationReferences==~((UINT32)0))return CR_OUT_OF_RESOURCES;
  S->Self=S;S->Core=C;S->Generation=Generation;S->Mapping=C->drv_base_address;
  S->MappingSize=C->drv_size;S->Registers=C->drv_registers;S->TcsOffset=C->tcs_offset;
  S->DrvId=C->drv_id;S->Held=TRUE;S->Open=TRUE;++C->ObservationReferences;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveSourceClose(CR_RPMH_RAIL_OBSERVATION_SOURCE *S) {
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(S && S->Self==S && S->Closed && !S->Held)return CR_SUCCESS;
  CR_STATUS Status=SourceCurrent(S,FALSE);if(Status!=CR_SUCCESS)return Status;
  S->Open=FALSE;
  if(S->ObserverCount || CrAtomicLoad32(&S->Core->TcsBusy) ||
     CrAtomicLoad32(&S->Core->TcsRetiring) || CrAtomicLoad32(&S->Core->TcsReading))return CR_BUSY;
  --S->Core->ObservationReferences;S->Held=FALSE;S->Closed=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveOpen(CR_RPMH_RAIL_OBSERVATION_SOURCE *S,
                               CR_RPMH_RAIL_OBSERVER *O,CONST CR_RPMH_RAIL *R) {
  CR_STATUS Status=SourceCurrent(S,TRUE);if(Status!=CR_SUCCESS)return Status;
  if(!O || !R || !R->Name || !*R->Name || (R->Address&0xffff000fU)!=0x00040000U ||
     !Separate(O,sizeof(*O),S,sizeof(*S)) || !Separate(O,sizeof(*O),S->Core,sizeof(*S->Core)) ||
     !Separate(O,sizeof(*O),R,sizeof(*R)))return CR_INVALID_PARAMETER;
  if(!Empty(O,sizeof(*O)))return Current(O,TRUE)==CR_SUCCESS && O->Source==S && O->Rail==R?CR_SUCCESS:CR_NOT_READY;
  if(S->ObserverCount==~((UINT32)0))return CR_OUT_OF_RESOURCES;
  O->Self=O;O->Source=S;O->Rail=R;O->Name=R->Name;O->Generation=S->Generation;
  O->Address=R->Address;O->DrvId=S->DrvId;O->Held=TRUE;++S->ObserverCount;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveCapture(CR_RPMH_RAIL_OBSERVER *O,CR_RPMH_RAIL_OBSERVATION *Out) {
  CR_STATUS Status=Current(O,TRUE);
  if(Status!=CR_SUCCESS){if(O && O->Self==O && O->Held)O->HasSnapshot=FALSE;return Status;}
  if(!Out || !Separate(O,sizeof(*O),Out,sizeof(*Out)) ||
     !Separate(O->Source,sizeof(*O->Source),Out,sizeof(*Out)) ||
     !Separate(O->Source->Core,sizeof(*O->Source->Core),Out,sizeof(*Out)) ||
     !Separate(O->Rail,sizeof(*O->Rail),Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  O->HasSnapshot=FALSE;
  if(O->Sequence==~((UINT64)0)){O->LastError=CR_OUT_OF_RESOURCES;return O->LastError;}
  CR_RPMH_RAIL_OBSERVATION V={0};
  V.Size=sizeof(V);V.Revision=CR_RPMH_RAIL_OBSERVATION_REVISION;V.Owner=O;V.Source=O->Source;
  V.Generation=O->Generation;V.Sequence=O->Sequence+1;V.Address=O->Address;V.DrvId=O->DrvId;
  V.Provenance=CR_RPMH_RAIL_OWN_DRV_REQUEST;
  Status=RpmhRead(O->Source->Core,O->Address,&V.RawVoltage);
  if(Status==CR_SUCCESS)Status=Current(O,TRUE);
  if(Status==CR_SUCCESS)Status=RpmhRead(O->Source->Core,O->Address+4,&V.RawEnable);
  if(Status==CR_SUCCESS)Status=Current(O,TRUE);
  if(Status==CR_SUCCESS)Status=RpmhRead(O->Source->Core,O->Address+8,&V.RawMode);
  if(Status==CR_SUCCESS)Status=Current(O,TRUE);
  O->LastError=Status;
  if(Status!=CR_SUCCESS)return CR_ERROR(Status)?Status:CR_DEVICE_ERROR;
  V.CapturedMask=CR_RPMH_RAIL_CAPTURE_ALL;V.VoltageMv=V.RawVoltage&0x1fffU;
  V.Enabled=(BOOLEAN)(V.RawEnable&1U);V.RpmhMode=(UINT8)(V.RawMode&7U);
  O->Snapshot=V;O->Sequence=V.Sequence;O->HasSnapshot=TRUE;*Out=V;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveCheck(CR_RPMH_RAIL_OBSERVER *O,CONST CR_RPMH_RAIL_OBSERVATION *V) {
  CR_STATUS Status=Current(O,TRUE);if(Status!=CR_SUCCESS)return Status;
  if(!V || !O->HasSnapshot || V->Owner!=O || V->Source!=O->Source ||
     V->Generation!=O->Generation || V->Sequence!=O->Sequence)return CR_NOT_READY;
  CONST CR_RPMH_RAIL_OBSERVATION *S=&O->Snapshot;
  if(V->Size!=S->Size || V->Revision!=S->Revision || V->Address!=S->Address || V->DrvId!=S->DrvId ||
     V->CapturedMask!=S->CapturedMask || V->Provenance!=S->Provenance ||
     V->RawVoltage!=S->RawVoltage || V->RawEnable!=S->RawEnable || V->RawMode!=S->RawMode ||
     V->VoltageMv!=S->VoltageMv || V->RpmhMode!=S->RpmhMode || V->Enabled!=S->Enabled)return CR_NOT_READY;
  return CR_SUCCESS;
}
CR_STATUS CrRpmhRailObserveClose(CR_RPMH_RAIL_OBSERVER *O) {
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(O && O->Self==O && O->Closed && !O->Held)return CR_SUCCESS;
  CR_STATUS Status=Current(O,FALSE);if(Status!=CR_SUCCESS)return Status;
  O->HasSnapshot=FALSE;--O->Source->ObserverCount;O->Held=FALSE;O->Closed=TRUE;
  return CR_SUCCESS;
}
