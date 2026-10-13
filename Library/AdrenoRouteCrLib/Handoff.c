/* Strict private GPUH record reader. SPDX-License-Identifier: MIT */
#include <Interface/CrAdrenoHandoff.h>
static BOOLEAN Equal(CONST VOID *A,CONST VOID *B,UINTN N){CONST UINT8 *P=A,*Q=B;for(UINTN I=0;I<N;++I)if(P[I]!=Q[I])return FALSE;return TRUE;}
CR_STATUS CrAdrenoHandoffValidateLocator(CONST CR_ADRENO_HANDOFF_LOCATOR *L){
 if(!L)return CR_INVALID_PARAMETER;UINT8 Sum=0;CONST UINT8 *P=(CONST UINT8 *)L;
 for(UINTN I=0;I<sizeof(*L);++I)Sum=(UINT8)(Sum+P[I]);
 if(L->Signature!=CR_ADRENO_GPUH_SIGNATURE||L->Length!=sizeof(*L)||L->Revision!=1||Sum||
 !Equal(L->OemId,"ALOHA ",6)||!Equal(L->OemTableId,"GPUHDK84",8)||L->OemRevision!=1||
 L->WireVersion!=CR_ADRENO_HANDOFF_VERSION||L->Reserved||!L->Incarnation||
 L->RecordBytes!=sizeof(CR_ADRENO_HANDOFF_RECORD)||L->RecordPhysical<0x100000ULL||
 (L->RecordPhysical&4095)||L->RecordPhysical>0x7ffffffffffff000ULL)return CR_UNSUPPORTED;
 return CR_SUCCESS;
}
CR_STATUS CrAdrenoHandoffValidate(CONST CR_ADRENO_HANDOFF_LOCATOR *L,
 CONST CR_ADRENO_HANDOFF_RECORD *R,CONST CR_ADRENO_ROUTE_FACTS *Installed){
 CR_STATUS S=CrAdrenoHandoffValidateLocator(L);if(S!=CR_SUCCESS)return S;
 if(!R||!Installed)return CR_INVALID_PARAMETER;
 if(CrAdrenoRouteValidateFacts(Installed)!=CR_SUCCESS)return CR_UNSUPPORTED;
 CR_ADRENO_HANDOFF_RECORD Copy=*R;Copy.Crc32=0;
 if(R->Signature!=CR_ADRENO_RECORD_SIGNATURE||R->Version!=CR_ADRENO_HANDOFF_VERSION||R->Size!=sizeof(*R)||
 R->Crc32!=CrAdrenoRouteCrc32(&Copy,sizeof(Copy))||R->Flags!=CR_ADRENO_HANDOFF_FLAGS||R->ChipId!=0x8450||
 R->Reserved[0]||R->Reserved[1]||R->ReservedTail[0]||R->ReservedTail[1]||
 R->Incarnation!=L->Incarnation||R->RecordPhysical!=L->RecordPhysical||!Equal(&R->Route,Installed,sizeof(*Installed)))return CR_UNSUPPORTED;
 CONST CR_ADRENO_STOP_OBSERVATION *O=&R->Stop;
 if(O->Version!=CR_ADRENO_STOP_VERSION||O->Size!=sizeof(*O)||O->Policy!=CR_ADRENO_STOP_POLICY_SM8450_ANDROID||
 O->StartIncarnation!=R->Incarnation||!O->Generation||O->GxState>3||
 (O->ReadMask>>CrAdrenoStopRegisterCount)||(O->AttemptedWrites>>CrAdrenoStopRegisterCount)||
 O->AcknowledgedWrites!=O->AttemptedWrites||
 O->Values[CrAdrenoStopGxStatus]%4!=O->GxState||O->Values[CrAdrenoStopCm3Reset]!=1||
 O->Values[CrAdrenoStopCxHalt]!=2||(O->Values[CrAdrenoStopCxAck]&2)!=2||
 (O->Values[CrAdrenoStopGmuIrqMask]&0xffff8007U)!=0xffff8007U||
 (O->Values[CrAdrenoStopAoIrqMask]&0x3f)!=0x3f||O->Values[CrAdrenoStopKeepalive]||
 !(O->Values[CrAdrenoStopQactive]&1)||O->Values[CrAdrenoStopAhbFence]||O->Values[CrAdrenoStopRsccBusy])return CR_UNSUPPORTED;
 UINT64 Required=((1ULL<<CrAdrenoStopRegisterCount)-1)&~((1ULL<<CrAdrenoStopAhbClear));
 if(O->GxState){Required&=~((1ULL<<CrAdrenoStopGpuIrqMask)|(1ULL<<CrAdrenoStopGxHalt)|
 (1ULL<<CrAdrenoStopGxAck)|(1ULL<<CrAdrenoStopGpuReset));}
 else if(O->Values[CrAdrenoStopGpuIrqMask]||O->Values[CrAdrenoStopGxHalt]!=1||
 !(O->Values[CrAdrenoStopGxAck]&1)||O->Values[CrAdrenoStopGpuReset]!=1)return CR_UNSUPPORTED;
 if((O->ReadMask&Required)!=Required)return CR_UNSUPPORTED;
 for(UINT32 I=0;I<10;++I)if(!(O->Values[CrAdrenoStopTcs0+I]&1))return CR_UNSUPPORTED;
 /* Non-secure visible architected inventory, not IRQ-count-as-bank-count.
  * Exact target DT caps mapping groups24; aperture caps 32 4KiB CB pages. */
 if((R->SmmuId[0]&0x48000000U)!=0x48000000U||(R->SmmuId[1]>>31)||
 !(R->SmmuId[2]&0x1000U)||(R->SmmuControl&9)||!(R->SmmuControl&0x400)||
 !R->StreamCount||R->StreamCount>24||R->StreamCount!=(R->SmmuId[0]&255)||
 !R->ContextCount||R->ContextCount>32||R->ContextCount!=(R->SmmuId[1]&255)||
 (R->SmmuId[1]&0xff0000U)||R->RetiredStreams!=0x3fU||
 (R->ContextCount<32&&(R->RetiredContexts>>R->ContextCount)))return CR_UNSUPPORTED;
 return CR_SUCCESS;
}
