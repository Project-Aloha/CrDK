/* Actual private clock service/core used by native display integration tests. MIT */
#define MmioRead32 ClockFixtureRead32
#define MmioWrite32 ClockFixtureWrite32
#define MicroSecondDelay ClockFixtureDelay
#define MemoryFence ClockFixtureFence
#define SetMem ClockFixtureSetMem
#define CompareMem ClockFixtureCompareMem
#define DebugPrint ClockFixtureDebugPrint
#define CR_CLOCK_EXTERNAL_LEASE_FIXTURE_ONLY
#include "ClockExternalLeaseTest.c"
#undef MmioRead32
#undef MmioWrite32
#undef MicroSecondDelay
#undef MemoryFence
#undef SetMem
#undef CompareMem
#undef DebugPrint
#include <Protocol/EFIClockExternalCrProtocol.h>
#include <Protocol/EFIClockCrProtocol.h>
static CR_CLOCK_EXTERNAL_LEASE Lease;
static VOID *Storage;
static UINT32 FaultKind;
static EFI_STATUS FaultStatus;
static BOOLEAN ForeignIncarnation;
EFI_STATUS NativeProviderStep(VOID);
static EFI_STATUS ClockStep(UINT32 Kind){
  EFI_STATUS S=NativeProviderStep();
  if(S==EFI_SUCCESS&&Kind==FaultKind){FaultKind=0;return FaultStatus;}
  return S;
}
static EFI_STATUS EFIAPI Reserve(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,BOOLEAN *Owned){
  (void)P;EFI_STATUS S=ClockStep(1);return S==EFI_SUCCESS?CrClockExternalLeaseReserve(&Lease,O,Owned):S;}
static EFI_STATUS EFIAPI SetPlan(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT32 I,
 CONST CR_CLOCK_DSI_PLAN *V,BOOLEAN On,BOOLEAN *Owned){
  (void)P;EFI_STATUS S=ClockStep(On?2:3);return S==EFI_SUCCESS?CrClockExternalLeaseSetPlan(&Lease,O,I,V,On,Owned):S;}
static EFI_STATUS EFIAPI GetPlan(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT32 I,
 CR_CLOCK_DSI_PLAN *V,BOOLEAN *On,BOOLEAN *Pending,UINT64 *Inc){
  (void)P;EFI_STATUS S=ClockStep(4);if(S!=EFI_SUCCESS)return S;
  S=CrClockExternalLeaseGetPlan(&Lease,O,I,V,On,Pending,Inc);
  if(S==EFI_SUCCESS&&ForeignIncarnation){++*Inc;ForeignIncarnation=FALSE;}
  return S;}
static EFI_STATUS EFIAPI ReleaseExternal(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O){
  (void)P;EFI_STATUS S=ClockStep(5);return S==EFI_SUCCESS?CrClockExternalLeaseRelease(&Lease,O):S;}
static EFI_STATUS EFIAPI AcquireClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *ControllerName,
 CONST CHAR8 *Name,UINT64 Rate,BOOLEAN *Owned){
  (void)P;(void)ControllerName;EFI_STATUS S=NativeProviderStep();if(S!=EFI_SUCCESS)return S;
  ClockNode *N=NULL;S=GetClockNode(Lease.External.Core,Name,NULL,NULL,&N);if(S!=EFI_SUCCESS)return S;
  S=CrClockExternalLeaseGuard(&Lease,N);return S==EFI_SUCCESS?ClockAcquire(Lease.External.Core,N,Rate,Owned):S;}
static EFI_STATUS EFIAPI SetClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *ControllerName,
 CONST CHAR8 *Name,UINT64 Rate,BOOLEAN On){
  (void)P;(void)ControllerName;EFI_STATUS S=NativeProviderStep();if(S!=EFI_SUCCESS)return S;
  ClockNode *N=NULL;S=GetClockNode(Lease.External.Core,Name,NULL,NULL,&N);if(S!=EFI_SUCCESS)return S;
  S=CrClockExternalLeaseGuard(&Lease,N);return S==EFI_SUCCESS?ClockEnable(Lease.External.Core,N,Rate,On):S;}
static EFI_CLOCK_EXTERNAL_CR_PROTOCOL Ext={.Revision=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION,
  .Reserve=Reserve,.SetPlan=SetPlan,.GetPlan=GetPlan,.ReleaseOwner=ReleaseExternal};
static EFI_CLOCK_CR_PROTOCOL Core={.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.SetClock=SetClock,.AcquireClock=AcquireClock};
VOID NativeClockFixtureSetup(EFI_CLOCK_CR_PROTOCOL **C,EFI_CLOCK_EXTERNAL_CR_PROTOCOL **E){
  Storage=Fixture(&Lease);FaultKind=0;ForeignIncarnation=FALSE;*C=&Core;*E=&Ext;
}
VOID NativeClockFixtureFinish(VOID){
  assert(!Lease.External.Owner&&!Lease.Ready&&!Lease.CleanupPending);
  SM8450_CLOCK_STATE *S=Storage;for(UINTN I=0;I<S->NodeCount;++I)assert(!S->Nodes[I].ReferenceCount);
  assert(CrClockExternalUninit(&Lease.External)==CR_SUCCESS);free(Storage);Storage=NULL;Active=NULL;
}
BOOLEAN NativeClockFixtureOwned(VOID){return Lease.External.Owner!=NULL;}
VOID NativeClockFixtureFail(UINT32 Kind,EFI_STATUS Status){assert(Kind>=1&&Kind<=5);FaultKind=Kind;FaultStatus=Status;}
VOID NativeClockFixtureForeignIncarnation(VOID){ForeignIncarnation=TRUE;}
