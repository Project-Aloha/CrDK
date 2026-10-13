/** SM8450 legacy PMIC battery RPC, derived from qti_battery_charger.c.
 * Copyright (c) 2020-2021, The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/BatteryChargerCrLib.h>
#include <oskal/cr_string.h>

#define REQUEST_TIMEOUT_US 1000000ULL
#define CYCLE_US 1000000ULL
#define SAMPLE_MAX_AGE_US 5000000ULL
#define META_INDEX CR_BATTERY_PROPERTY_COUNT
#define STATS_INDEX (META_INDEX+1U)
#define REQUEST_COUNT (STATS_INDEX+2U)
#define NO_INDEX REQUEST_COUNT
/* Android SM8450: BATT_PRESENT/STATUS/CAPACITY/VOLT_NOW/CURR_NOW/TEMP;
 * USB_ONLINE. Do not use sc8280xp's different message/property layout. */
STATIC CONST UINT32 Properties[REQUEST_COUNT] = {2,0,4,7,9,12,0,13,15,20};
STATIC UINT32 Count(CONST CR_BATTERY_CHARGER *B) { return B->StatisticsEnabled ? REQUEST_COUNT : B->MetadataEnabled ? STATS_INDEX : CR_BATTERY_PROPERTY_COUNT; }
STATIC UINT32 Refresh(CONST CR_BATTERY_CHARGER *B) { return (1U<<Count(B))-1U; }
STATIC VOID Invalidate(CR_BATTERY_CHARGER *B, UINT32 Index) {
  if(Index>=STATS_INDEX)B->Statistics.ValidMask&=~(1U<<(Index-STATS_INDEX));
  else if(Index==META_INDEX)B->TechnologyValid=FALSE;
  else B->State.ValidMask&=~(1U<<Index);
}
STATIC UINT32 Opcode(UINT32 Index) { return Index == 6 ? 0x32U : 0x30U; }
STATIC UINT32 Get32(CONST UINT8 *P)
{ return (UINT32)P[0] | ((UINT32)P[1]<<8) | ((UINT32)P[2]<<16) | ((UINT32)P[3]<<24); }
STATIC VOID Put32(UINT8 *P, UINT32 V)
{ P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); P[2]=(UINT8)(V>>16); P[3]=(UINT8)(V>>24); }
STATIC UINT64 Later(UINT64 Now, UINT64 Delay)
{ return Now > CR_MAX_UINT64-Delay ? CR_MAX_UINT64 : Now+Delay; }
STATIC VOID Error(CR_BATTERY_CHARGER *B, CR_STATUS Status)
{ B->State.LastError=Status; B->State.Faulted=TRUE; ++B->State.Sequence; }

CR_STATUS CrBatteryChargerInit(CR_BATTERY_CHARGER *B, CR_BATTERY_SEND Send, VOID *Context)
{
  if(B==NULL || Send==NULL) return CR_INVALID_PARAMETER;
  cr_memset(B,0,sizeof(*B)); B->Send=Send; B->Context=Context;
  B->State.Size=sizeof(B->State); B->State.Revision=CR_BATTERY_SNAPSHOT_REVISION;
  B->State.LastError=CR_NOT_READY; B->PendingIndex=NO_INDEX;
  B->Initialized=TRUE; return CR_SUCCESS;
}
VOID CrBatteryChargerOnline(CR_BATTERY_CHARGER *B, BOOLEAN Online,
                            UINT32 AdspGeneration, UINT32 PeerGeneration)
{
  if(B==NULL || !B->Initialized) return;
  BOOLEAN ValidAdsp=AdspGeneration!=0 && AdspGeneration>=B->AdspGeneration;
  UINT32 QualifiedAdsp=ValidAdsp ? AdspGeneration : B->AdspGeneration;
  Online=Online && ValidAdsp && PeerGeneration!=0;
  if(B->State.PeerReady==Online && B->AdspGeneration==QualifiedAdsp &&
     B->PeerGeneration==PeerGeneration) return;
  BOOLEAN NewAdsp=ValidAdsp && AdspGeneration>B->AdspGeneration;
  UINT32 Await=NewAdsp ? 0 : B->AwaitLateMask;
  /* GLINK/PDR and an offline interval do not retire the remote's untagged
   * property request. Preserve every old identity until its original reply,
   * or a qualified increasing ADSP boot. Zero/lower snapshots cannot erase
   * the previous identity or its outstanding requests. */
  if(!NewAdsp && B->State.Pending && B->PendingIndex<REQUEST_COUNT)
    Await|=1U<<B->PendingIndex;
  B->AdspGeneration=QualifiedAdsp; B->PeerGeneration=PeerGeneration;
  ++B->State.Generation; ++B->State.Sequence;
  B->State.PeerReady=Online; B->State.Pending=FALSE; B->State.ValidMask=0;
  B->State.LastError=CR_NOT_READY; B->State.Faulted=FALSE;
  B->PendingIndex=NO_INDEX; B->NextIndex=0; B->AwaitLateMask=Await; B->TechnologyValid=FALSE;
  B->Statistics.ValidMask=0;
  B->RefreshMask=Online ? Refresh(B) : 0;
  B->DeadlineUs=B->NextCycleUs=0;
}
VOID CrBatteryChargerRefresh(CR_BATTERY_CHARGER *B)
{ if(B!=NULL && B->Initialized && B->State.PeerReady) B->RefreshMask=Refresh(B); }

CR_STATUS CrBatteryChargerTick(CR_BATTERY_CHARGER *B, UINT64 Now)
{
  UINT32 N, Index, Generation;
  UINT8 Request[24]={0}; CR_STATUS Status;
  if(B==NULL || !B->Initialized) return CR_INVALID_PARAMETER;
  if(!B->State.PeerReady) return CR_NOT_READY;
  if(B->Sending) return CR_BUSY;
  if(B->State.Pending) {
    if(Now < B->DeadlineUs) return CR_SUCCESS;
    Index=B->PendingIndex;
    B->AwaitLateMask|=1U<<Index; Invalidate(B,Index);
    B->State.Pending=FALSE; B->PendingIndex=NO_INDEX;
    ++B->State.Timeouts; Error(B,CR_TIMEOUT);
  }
  if(B->RefreshMask==0) {
    if(Now < B->NextCycleUs) return CR_SUCCESS;
    B->RefreshMask=Refresh(B);
  }
  Index=NO_INDEX;
  for(N=0;N<Count(B);++N) {
    UINT32 Candidate=(B->NextIndex+N)%Count(B);
    if((B->RefreshMask & ~B->AwaitLateMask & (1U<<Candidate))!=0) { Index=Candidate; break; }
  }
  if(Index==NO_INDEX) {
    B->RefreshMask=0; B->NextCycleUs=Later(Now,CYCLE_US); return CR_SUCCESS;
  }
  Put32(Request,CR_BATTERY_OWNER); Put32(Request+4,1); Put32(Request+8,Opcode(Index));
  /* battery_id at12 is zero, property_id at16; reserved value at20 is zero. */
  Put32(Request+16,Properties[Index]);
  B->State.Pending=TRUE; B->PendingIndex=Index;
  B->DeadlineUs=Later(Now,REQUEST_TIMEOUT_US); Generation=B->State.Generation;
  B->Sending=TRUE; Status=B->Send(B->Context,Request,sizeof(Request)); B->Sending=FALSE;
  if(B->State.Generation!=Generation || !B->State.PeerReady) return CR_NOT_READY;
  /* A nonzero transport status does not certify request acceptance. */
  if(Status!=CR_SUCCESS) {
    B->State.Pending=FALSE; B->PendingIndex=NO_INDEX;
    if(Status!=CR_NOT_READY) Error(B,Status);
    return Status;
  }
  B->RefreshMask&=~(1U<<Index); B->NextIndex=(Index+1)%Count(B);
  if(B->RefreshMask==0) B->NextCycleUs=Later(Now,CYCLE_US);
  return CR_SUCCESS;
}

STATIC BOOLEAN Value(CR_BATTERY_CHARGER *B, UINT32 Index, UINT32 Raw)
{
  INT64 Signed=(Raw & 0x80000000U)!=0 ? (INT64)Raw-4294967296LL : (INT64)Raw;
  switch(Index) {
  case 0: if(Raw>1) return FALSE; B->State.Present=(BOOLEAN)Raw; break;
  case 1: if(Raw>CrBatteryFull) return FALSE; B->State.Status=(UINT8)Raw; break;
  case 2: if(Raw>10000) return FALSE; B->State.SocHundredthsPercent=(UINT16)Raw; break;
  case 3: B->State.VoltageUv=Raw; break;
  case 4: B->State.CurrentUa=(INT32)Signed; break;
  case 5:
    if(Signed > 214748364LL || Signed < -214748364LL) return FALSE;
    B->State.TemperatureMilliC=(INT32)(Signed*10); break;
  case 6: if(Raw>1) return FALSE; B->State.UsbOnline=(BOOLEAN)Raw; break;
  case META_INDEX: if(Raw>6) return FALSE; B->Technology=Raw; break;
  case STATS_INDEX:
  case STATS_INDEX+1U:
    /* Linux exports these through a signed power_supply intval. Negative
     * sentinels (including -1 for unavailable TTE) are not measurements. */
    if(Raw>0x7fffffffU) return FALSE;
    if(Index==STATS_INDEX)B->Statistics.CycleCount=Raw;
    else B->Statistics.TimeToEmptySeconds=Raw;
    break;
  default: return FALSE;
  }
  return TRUE;
}
VOID CrBatteryChargerReceive(CR_BATTERY_CHARGER *B, CONST VOID *Data, UINTN Length, UINT64 Now)
{
  CONST UINT8 *P=Data; UINT32 I, Index=NO_INDEX, Op, Property;
  if(B==NULL || !B->Initialized || !B->State.PeerReady || Data==NULL) return;
  if(Length<12 || Get32(P)!=CR_BATTERY_OWNER) return;
  Op=Get32(P+8);
  /* Notifications are hints only; sampling remains bounded and explicit. */
  if(Get32(P+4)==2 && Op==7 && Length==16) { CrBatteryChargerRefresh(B); return; }
  if(Get32(P+4)!=1 || (Op!=0x30 && Op!=0x32)) return;
  if(Length!=24) { ++B->State.Malformed; Error(B,CR_COMPROMISED_DATA); return; }
  Property=Get32(P+12);
  for(I=0;I<Count(B);++I)
    if(Opcode(I)==Op && Properties[I]==Property) { Index=I; break; }
  if(Index==NO_INDEX) { ++B->State.Malformed; Error(B,CR_COMPROMISED_DATA); return; }
  if((B->AwaitLateMask & (1U<<Index))!=0) {
    /* Retire the untagged old request; never publish its late sample. */
    B->AwaitLateMask&=~(1U<<Index); B->RefreshMask|=1U<<Index; return;
  }
  if(!B->State.Pending || B->PendingIndex!=Index) return;
  B->State.Pending=FALSE; B->PendingIndex=NO_INDEX;
  if(Now >= B->DeadlineUs) {
    Invalidate(B,Index); ++B->State.Timeouts;
    B->RefreshMask|=1U<<Index; Error(B,CR_TIMEOUT); return;
  }
  if(Get32(P+20)!=0) { Invalidate(B,Index); Error(B,CR_DEVICE_ERROR); return; }
  if(!Value(B,Index,Get32(P+16))) {
    Invalidate(B,Index); ++B->State.Malformed; Error(B,CR_COMPROMISED_DATA); return;
  }
  if(Index>=STATS_INDEX){B->Statistics.ValidMask|=1U<<(Index-STATS_INDEX);B->StatisticsSampleUs[Index-STATS_INDEX]=Now;}
  else if(Index==META_INDEX){B->TechnologyValid=TRUE;B->TechnologySampleUs=Now;}
  else {B->State.ValidMask|=1U<<Index; B->State.SampleUs[Index]=Now;}
  B->State.LastError=CR_SUCCESS; B->State.Faulted=FALSE; ++B->State.Sequence;
}
CR_STATUS CrBatteryChargerSnapshot(CONST CR_BATTERY_CHARGER *B, UINT64 Now,
                                   CR_BATTERY_CHARGER_SNAPSHOT *Out)
{
  UINT32 I;
  if(B==NULL || !B->Initialized || Out==NULL) return CR_INVALID_PARAMETER;
  *Out=B->State;
  if(!Out->PeerReady) Out->ValidMask=0;
  for(I=0;I<CR_BATTERY_PROPERTY_COUNT;++I)
    if(Now < Out->SampleUs[I] || Now-Out->SampleUs[I] > SAMPLE_MAX_AGE_US)
      Out->ValidMask&=~(1U<<I);
  return (Out->ValidMask&CR_BATTERY_VALID_SOC)!=0 ? CR_SUCCESS : CR_NOT_READY;
}

VOID CrBatteryChargerEnableMetadata(CR_BATTERY_CHARGER *B)
{ if(B!=NULL && B->Initialized){B->MetadataEnabled=TRUE;if(B->State.PeerReady)B->RefreshMask|=1U<<META_INDEX;} }
CR_STATUS CrBatteryChargerTechnology(CONST CR_BATTERY_CHARGER *B, UINT64 Now, UINT32 *Technology)
{
  if(B==NULL || !B->Initialized || Technology==NULL) return CR_INVALID_PARAMETER;
  if(!B->MetadataEnabled || !B->State.PeerReady || !B->TechnologyValid ||
     Now<B->TechnologySampleUs || Now-B->TechnologySampleUs>SAMPLE_MAX_AGE_US) return CR_NOT_READY;
  *Technology=B->Technology;return CR_SUCCESS;
}

VOID CrBatteryChargerEnableStatistics(CR_BATTERY_CHARGER *B)
{
  if(B==NULL || !B->Initialized)return;
  CrBatteryChargerEnableMetadata(B);B->StatisticsEnabled=TRUE;
  if(B->State.PeerReady)B->RefreshMask|=3U<<STATS_INDEX;
}
CR_STATUS CrBatteryChargerStatistics(CONST CR_BATTERY_CHARGER *B, UINT64 Now,
                                     CR_BATTERY_STATISTICS *Out)
{
  if(B==NULL || !B->Initialized || Out==NULL)return CR_INVALID_PARAMETER;
  *Out=B->Statistics;
  if(!B->StatisticsEnabled || !B->State.PeerReady)Out->ValidMask=0;
  for(UINT32 I=0;I<2;++I)
    if(Now<B->StatisticsSampleUs[I] || Now-B->StatisticsSampleUs[I]>SAMPLE_MAX_AGE_US)
      Out->ValidMask&=~(1U<<I);
  return Out->ValidMask!=0 ? CR_SUCCESS : CR_NOT_READY;
}
