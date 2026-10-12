/* Production supply-graph ownership in native/NT builds. MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/RpmhRailLib.h>
#include <Library/RpmhRailVoteLib.h>
#include <Library/PepRailLib.h>

#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *Buffer,UINTN Length,UINT8 Value) {return memset(Buffer,Value,Length);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *Left,CONST CHAR8 *Right) {return strcmp(Left,Right);}
#endif

#ifdef _KERNEL_MODE
static KIRQL CurrentIrql;
KIRQL KeGetCurrentIrql(VOID) {return CurrentIrql;}
#endif
static UINT32 Calls,Lookups,FailCall,Hardware[4][3];
static BOOLEAN AliasLeaf;
static struct {UINT32 Address,Kind,Value;} Events[128];
static CR_STATUS Address(VOID *Context,CONST CHAR8 *Name,UINT32 *Output) {
  assert(Context==Hardware);++Lookups;
  if (!strcmp(Name,"parent")) *Output=0x100;
  else if (!strcmp(Name,"leaf")) *Output=AliasLeaf ? 0x100 : 0x200;
  else if (!strcmp(Name,"sibling")) *Output=0x300;
  else return CR_NOT_FOUND;
  return CR_SUCCESS;
}
static CR_STATUS Write(VOID *Context,CONST CR_RPMH_RAIL *Rail,CR_RPMH_RAIL_VOTE_KIND Kind,UINT32 Value) {
  assert(Context==Hardware && Rail->Address>=0x100 && Rail->Address<=0x300 && Kind<=CrRpmhRailEnable);
#ifdef _KERNEL_MODE
  assert(CurrentIrql==PASSIVE_LEVEL);
#endif
  assert(Calls<128);Events[Calls].Address=Rail->Address;Events[Calls].Kind=Kind;Events[Calls].Value=Value;++Calls;
  Hardware[Rail->Address/0x100][Kind]=Value; // hardware commits before error
  return Calls==FailCall ? CR_DEVICE_ERROR : CR_SUCCESS;
}
static CR_RPMH_RAIL_IO Io={Hardware,Address,Write};
static CR_RPMH_RAIL Parent,Leaf,Sibling;
static void Reset(void) {
  memset(Hardware,0,sizeof(Hardware));Calls=Lookups=FailCall=0;AliasLeaf=FALSE;
  Parent=(CR_RPMH_RAIL){.Name="parent"};
  Leaf=(CR_RPMH_RAIL){.Name="leaf",.Parent=&Parent,.ParentMv=1800,.ParentMode=7};
  Sibling=(CR_RPMH_RAIL){.Name="sibling",.Parent=&Parent,.ParentMv=1800,.ParentMode=7};
}
static void OwnerVotes(void) {
  Reset();
  CR_RPMH_RAIL Rails[2]={{.Name="parent",.Address=0x100},
    {.Name="leaf",.Address=0x200,.ParentMv=1800,.ParentMode=7}};
  Rails[1].Parent=&Rails[0];
  CR_RPMH_RAIL_VOTER Voter={0};CR_RPMH_RAIL_ENTRY Entries[2];UINT32 Owners[3];
  assert(CrRpmhRailVotesInit(&Voter,&Io,Rails,2,Entries,2)==CR_SUCCESS && Calls==0 && Lookups==0);
  assert(CrRpmhRailVotesValidate(&Voter,&Rails[1],1200,7)==CR_SUCCESS && Calls==0 && Lookups==0);
  Rails[0].Address=0;
  assert(CrRpmhRailVotesValidate(&Voter,&Rails[1],1200,7)==CR_NOT_READY && Lookups==0);
  Rails[0].Address=0x100;
  BOOLEAN Foreign;
  assert(CrRpmhRailAcquire(&Io,&Rails[1],1200,7,&Foreign)==CR_SUCCESS && Foreign);
  CR_RPMH_RAIL_VALUE On={.Present=TRUE,.Enabled=TRUE,.VoltageMv=1200,.Mode=7},Value;
  BOOLEAN Pending;UINT32 Before=Calls;
  for (UINTN O=0;O<2;++O) {
    assert(CrRpmhRailVotesSet(&Voter,&Owners[O],&Rails[1],&On)==CR_SUCCESS);
    assert(CrRpmhRailVotesSet(&Voter,&Owners[O],&Rails[1],&On)==CR_SUCCESS);
  }
  assert(Rails[1].ReferenceCount==3 && Rails[0].ReferenceCount==1 && Calls==Before);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[2],&Rails[1],&On)==CR_OUT_OF_RESOURCES);
  assert(CrRpmhRailVotesUninit(&Voter)==CR_BUSY);
  CR_RPMH_RAIL_VALUE Changed=On;Changed.VoltageMv=1500;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Changed)==CR_BUSY && Rails[1].ReferenceCount==2);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && Pending && Value.VoltageMv==1200);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[1],&Rails[1],&Value,&Pending)==CR_SUCCESS && !Pending);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_SUCCESS && Rails[1].ReferenceCount==3);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && !Pending);
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS && Rails[1].ReferenceCount==2);
  assert(CrRpmhRailVotesDrain(&Voter)==CR_SUCCESS && Rails[1].ReferenceCount==1 && Calls==Before);
  assert(CrRpmhRailRelease(&Io,&Rails[1])==CR_SUCCESS && !Hardware[1][2]);

  // A failed first acquire retains its cleanup lease, but Get reports the
  // original absent logical vote. Failed parent release remains this owner's.
  FailCall=Calls+6;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_DEVICE_ERROR && Entries[0].Held);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && Pending && !Value.Present);
  CR_RPMH_RAIL_VALUE Absent={0};FailCall=Calls+4;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Absent)==CR_DEVICE_ERROR);
  assert(Entries[0].Held && Rails[1].ParentOwned && !Rails[1].EnableRequested && Rails[0].EnableRequested);
  Before=Calls;FailCall=0;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Absent)==CR_SUCCESS && Calls==Before+3);
  assert(!Entries[0].Owner && !Rails[1].ParentOwned);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_SUCCESS);
  FailCall=Calls+1;
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_DEVICE_ERROR && Entries[0].Owner==&Owners[0]);
  assert(CrRpmhRailVotesUninit(&Voter)==CR_BUSY);
  FailCall=0;assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS);
  // Whole-policy capacity proof makes later absence and failed first acquire
  // unable to lose the stable owner's reserved slot.
  Before=Calls;
  assert(CrRpmhRailVotesRequireReservation(&Voter,&Owners[0],&Rails[1])==CR_NOT_FOUND);
  assert(CrRpmhRailVotesReserve(&Voter,&Owners[0],&Rails[1],1200,7)==CR_SUCCESS);
  assert(CrRpmhRailVotesReserve(&Voter,&Owners[0],&Rails[1],1500,7)==CR_SUCCESS);
  assert(CrRpmhRailVotesReserve(&Voter,&Owners[1],&Rails[1],1200,7)==CR_SUCCESS);
  assert(CrRpmhRailVotesReserve(&Voter,&Owners[2],&Rails[1],1200,7)==CR_OUT_OF_RESOURCES && Calls==Before && Lookups==0);
  // Another owner can retain a failed first acquire while this owner's
  // original vote remains absent. Restoring absence must not claim or await
  // the foreign cleanup lease; pure Get still gates hardware readiness.
  FailCall=Calls+1;
  assert(CrRpmhRailAcquire(&Io,&Rails[1],1200,7,&Foreign)==CR_DEVICE_ERROR && Foreign);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && !Value.Present && Pending);
  Before=Calls;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Absent)==CR_SUCCESS && Calls==Before && Rails[1].ReferenceCount==1);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && !Value.Present && Pending);
  FailCall=0;assert(CrRpmhRailRelease(&Io,&Rails[1])==CR_SUCCESS);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_SUCCESS);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Absent)==CR_SUCCESS);
  assert(CrRpmhRailVotesRequireReservation(&Voter,&Owners[0],&Rails[1])==CR_SUCCESS);
  assert(CrRpmhRailVotesUninit(&Voter)==CR_BUSY);
  FailCall=Calls+6;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_DEVICE_ERROR);
  FailCall=Calls+1;
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_DEVICE_ERROR);
  assert(CrRpmhRailVotesRequireReservation(&Voter,&Owners[0],&Rails[1])==CR_SUCCESS);
  FailCall=0;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&Absent)==CR_SUCCESS);
  assert(CrRpmhRailVotesRequireReservation(&Voter,&Owners[0],&Rails[1])==CR_SUCCESS);
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS);
  assert(CrRpmhRailVotesRequireReservation(&Voter,&Owners[0],&Rails[1])==CR_NOT_FOUND);
  assert(CrRpmhRailVotesReserve(&Voter,&Owners[2],&Rails[1],1200,7)==CR_SUCCESS);
  assert(CrRpmhRailVotesDrain(&Voter)==CR_SUCCESS);
#ifdef _KERNEL_MODE
  CurrentIrql=DISPATCH_LEVEL;Before=Calls;
  assert(CrRpmhRailVotesSet(&Voter,&Owners[0],&Rails[1],&On)==CR_INVALID_PARAMETER);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_INVALID_PARAMETER);
  assert(CrRpmhRailVotesDrain(&Voter)==CR_INVALID_PARAMETER && Calls==Before);
  CurrentIrql=PASSIVE_LEVEL;
#endif
  assert(CrRpmhRailVotesUninit(&Voter)==CR_SUCCESS);
  Rails[1].Parent=&Parent;
  assert(CrRpmhRailVotesInit(&Voter,&Io,Rails,2,Entries,2)==CR_INVALID_PARAMETER);
  Rails[1].Parent=&Rails[0];Rails[1].Address=0x100;
  assert(CrRpmhRailVotesInit(&Voter,&Io,Rails,2,Entries,2)==CR_INVALID_PARAMETER);
  puts("RPMh rail owners: absolute replay, foreign leases, conflicting-contract restore and retained parent cleanup passed");
}
static BOOLEAN LateRailFailure;
static CR_RPMH_RAIL *JournalRail(CR_RPMH_RAIL_VOTER *Voter,CONST CR_PEP_RAIL_BINDING *Binding) {
  for (UINT32 I=0;I<Voter->RailCount;++I)
    if (!strcmp(Voter->Rails[I].Name,Binding->HardwareName)) return &Voter->Rails[I];
  assert(FALSE);return NULL;
}
static CR_STATUS JournalValidate(VOID *Context,CONST CR_PEP_RAIL_BINDING *Binding,CONST CR_RPMH_RAIL_VALUE *Value) {
  return CrRpmhRailVotesValidate(Context,JournalRail(Context,Binding),Value->VoltageMv,Value->Mode);
}
static CR_STATUS JournalGet(VOID *Context,VOID *Owner,CONST CR_PEP_RAIL_BINDING *Binding,
                             CR_RPMH_RAIL_VALUE *Value,BOOLEAN *Pending) {
  CR_STATUS Status=CrRpmhRailVotesRequireReservation(Context,Owner,JournalRail(Context,Binding));
  return CR_ERROR(Status) ? Status : CrRpmhRailVotesGet(Context,Owner,JournalRail(Context,Binding),Value,Pending);
}
static CR_STATUS JournalSet(VOID *Context,VOID *Owner,CONST CR_PEP_RAIL_BINDING *Binding,
                             CONST CR_RPMH_RAIL_VALUE *Value) {
  CR_STATUS Status=CrRpmhRailVotesSet(Context,Owner,JournalRail(Context,Binding),Value);
  if (!CR_ERROR(Status) && LateRailFailure) {LateRailFailure=FALSE;return CR_DEVICE_ERROR;}
  return Status;
}
static void OwnerJournal(void) {
  Reset();
  CR_RPMH_RAIL Rails[2]={{.Name="parent",.Address=0x100},
    {.Name="leaf",.Address=0x200,.ParentMv=1800,.ParentMode=7}};
  Rails[1].Parent=&Rails[0];
  CR_RPMH_RAIL_VOTER Voter={0};CR_RPMH_RAIL_ENTRY Entries[4];UINT32 Owners[2];
  assert(CrRpmhRailVotesInit(&Voter,&Io,Rails,2,Entries,4)==CR_SUCCESS);
  CONST CR_PEP_RAIL_PROFILE Profiles[]={
    {.Arguments={1,1200000,1,7,0},.Value={.Present=TRUE,.Enabled=TRUE,.VoltageMv=1200,.Mode=7}},
    {.Arguments={1,1500000,1,7,0},.Value={.Present=TRUE,.Enabled=TRUE,.VoltageMv=1500,.Mode=7}},
    {.Arguments={1,0,0,7,0},.Value={.Present=TRUE,.Enabled=FALSE,.Mode=7}}
  };
  CR_PEP_RAIL_BINDING Binding={.Name={"pmic_leaf",9},.DriverName={"HLOS_DRV",8},.VoteType={"REQUIRED",8},
    .HardwareName="leaf",.Profiles=Profiles,.ProfileCount=3,.AllowImplicitQualifier=TRUE};
  CR_PEP_RAIL_IO ProviderIo={&Voter,JournalValidate,JournalGet,JournalSet};CR_PEP_RAIL_PROVIDER Provider={0};
  CR_PEP_RAIL_BINDING Duplicate[]={Binding,Binding};
  assert(CrPepRailProviderInit(&Provider,&ProviderIo,Duplicate,2)==CR_INVALID_PARAMETER);
  assert(CrPepRailProviderInit(&Provider,&ProviderIo,&Binding,1)==CR_SUCCESS && Calls==0 && Lookups==0);
  CR_PEP_JOURNAL_PROVIDER Backend;assert(CrPepRailProviderGetJournal(&Provider,&Backend)==CR_SUCCESS);
  CR_PEP_JOURNAL Journal={0};CR_PEP_JOURNAL_ENTRY Tx[8],Owned[4];
  assert(CrPepJournalInit(&Journal,&Owners[0],&Backend,Tx,8,Owned,4)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP On={.Kind=CrPepVreg,.Name={"pmic_leaf",9},.Arguments={1,1200000,1,7,0},.ArgumentCount=5};
  CR_PEP_RESOURCE_OP Bad=On;Bad.Arguments[4]=9;
  assert(CrPepJournalValidate(&Journal,&Bad)==CR_UNSUPPORTED && Calls==0 && Lookups==0);
  Bad=On;Bad.DriverName=(CR_PEP_TEXT){"OTHER",5};Bad.VoteType=Binding.VoteType;
  assert(CrPepJournalValidate(&Journal,&Bad)==CR_UNSUPPORTED);
  Bad=On;Bad.DriverName=Binding.DriverName;Bad.VoteType=Binding.VoteType;
  assert(CrPepJournalValidate(&Journal,&On)==CR_NOT_FOUND);
  CONST CR_PEP_RAIL_BINDING *Description=NULL;CR_RPMH_RAIL_VALUE DescribedValue;
  assert(CrPepRailProviderDescribe(&Provider,&Bad,&Description,&DescribedValue)==CR_SUCCESS &&
         Description==&Binding && DescribedValue.VoltageMv==1200 && DescribedValue.Mode==7 && Calls==0 && Lookups==0);
  CR_PEP_RESOURCE_OP Unqualified=On;Unqualified.Arguments[4]=9;
  assert(CrPepRailProviderDescribe(&Provider,&Unqualified,&Description,&DescribedValue)==CR_UNSUPPORTED && Description==NULL);
  for (UINTN O=0;O<2;++O) {
    assert(CrRpmhRailVotesReserve(&Voter,&Owners[O],&Rails[1],1200,7)==CR_SUCCESS);
    assert(CrRpmhRailVotesReserve(&Voter,&Owners[O],&Rails[1],1500,7)==CR_SUCCESS);
  }
  assert(CrPepJournalValidate(&Journal,&Bad)==CR_SUCCESS && Calls==0 && Lookups==0);
  assert(CrPepJournalValidate(&Journal,&On)==CR_SUCCESS);
  BOOLEAN Foreign;assert(CrRpmhRailAcquire(&Io,&Rails[0],1800,7,&Foreign)==CR_SUCCESS);
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS && CrPepJournalApply(&Journal,&On)==CR_SUCCESS);
  UINT32 Before=Calls;
  assert(CrPepJournalApply(&Journal,&On)==CR_SUCCESS && Calls==Before && Rails[1].ReferenceCount==1);
  assert(CrPepJournalCommit(&Journal)==CR_SUCCESS && Rails[0].ReferenceCount==2);
  assert(CrRpmhRailVotesSet(&Voter,&Owners[1],&Rails[1],&Profiles[0].Value)==CR_SUCCESS && Rails[1].ReferenceCount==2);
  CR_PEP_RESOURCE_OP High=On;High.Arguments[1]=1500000;
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS && CrPepJournalApply(&Journal,&High)==CR_BUSY);
  CR_RPMH_RAIL_VALUE Value;BOOLEAN Pending;
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && Pending && Value.VoltageMv==1200);
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS && Rails[1].ReferenceCount==2);
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[1])==CR_SUCCESS && Rails[1].ReferenceCount==1);
  Before=Calls;LateRailFailure=TRUE;
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS && CrPepJournalApply(&Journal,&High)==CR_DEVICE_ERROR && Calls>Before);
  assert(Hardware[2][0]==1500 && CrPepJournalRollback(&Journal)==CR_SUCCESS && Hardware[2][0]==1200);
  assert(Rails[0].ReferenceCount==2 && Rails[1].ReferenceCount==1);
  FailCall=Calls+1;
  assert(CrPepJournalClose(&Journal)==CR_DEVICE_ERROR && Journal.OwnedCount==1 && Entries[0].Held);
  assert(CrPepJournalBegin(&Journal)==CR_BUSY && CrPepJournalValidate(&Journal,&On)==CR_NOT_READY);
  FailCall=0;assert(CrPepJournalClose(&Journal)==CR_SUCCESS && Rails[0].ReferenceCount==1 && Hardware[1][2]==1);
  assert(CrRpmhRailVotesGet(&Voter,&Owners[0],&Rails[1],&Value,&Pending)==CR_SUCCESS && !Pending && !Value.Present);
  assert(CrPepJournalValidate(&Journal,&On)==CR_SUCCESS);
  assert(CrPepJournalUninit(&Journal)==CR_SUCCESS && CrPepRailProviderUninit(&Provider)==CR_SUCCESS);
  assert(CrRpmhRailVotesUninit(&Voter)==CR_BUSY);
  assert(CrRpmhRailVotesReleaseOwner(&Voter,&Owners[0])==CR_SUCCESS);
  assert(CrRpmhRailVotesUninit(&Voter)==CR_SUCCESS && CrRpmhRailRelease(&Io,&Rails[0])==CR_SUCCESS);
  puts("PEP regulator journal: full qualifier/tuple preflight, foreign parents, late hardware writes and retained Close retry passed");
}
int main(void) {
  BOOLEAN Owned,Foreign;
  Reset();
  assert(CrRpmhRailValidate(&Leaf)==CR_SUCCESS && Calls==0 && Lookups==0);
  Parent.Address=0x100;Leaf.Address=0x104;
  assert(CrRpmhRailValidate(&Leaf)==CR_COMPROMISED_DATA && Calls==0);
  Leaf.Address=0xfffffffcU;Leaf.Parent=NULL;
  assert(CrRpmhRailValidate(&Leaf)==CR_COMPROMISED_DATA && Calls==0);
  Reset();
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_SUCCESS && Owned && Leaf.Ready && Parent.Ready);
  assert(Calls==6 && Lookups==2 && Parent.ReferenceCount==1);
  UINT32 Before=Calls;
  assert(CrRpmhRailAcquire(&Io,&Parent,1800,7,&Foreign)==CR_SUCCESS && Foreign && Calls==Before);
  assert(CrRpmhRailAcquire(&Io,&Parent,1700,7,&Owned)==CR_BUSY && !Owned && Calls==Before);
  assert(CrRpmhRailAcquire(&Io,&Parent,1800,3,&Owned)==CR_BUSY && !Owned);
  assert(CrRpmhRailAcquire(&Io,&Sibling,1500,7,&Owned)==CR_SUCCESS && Parent.ReferenceCount==3);
  Before=Calls;
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && Parent.ReferenceCount==2 && !Leaf.ParentOwned);
  assert(Events[Before].Kind==CrRpmhRailEnable && Events[Before+1].Kind==CrRpmhRailMode &&
         Events[Before+2].Kind==CrRpmhRailVoltage && Hardware[1][CrRpmhRailEnable]==1);
  Before=Calls;
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && Calls==Before && Parent.ReferenceCount==2);
  assert(CrRpmhRailRelease(&Io,&Sibling)==CR_SUCCESS && Parent.ReferenceCount==1);
  assert(CrRpmhRailRelease(&Io,&Parent)==CR_SUCCESS && !Parent.ReferenceCount && !Hardware[1][0]);

  // Failed child enable and disable own their uncertain side effects; retry
  // cannot release a foreign parent's already dropped reference twice.
  Reset();FailCall=6;
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_DEVICE_ERROR && Owned && !Leaf.Ready);
  assert(Leaf.EnableRequested && Leaf.ReferenceCount==1 && Parent.Ready && Hardware[2][2]==1);
  assert(CrRpmhRailAcquire(&Io,&Parent,1800,7,&Foreign)==CR_SUCCESS && Parent.ReferenceCount==2);
  FailCall=Calls+1;
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_DEVICE_ERROR && Leaf.EnableRequested && Leaf.ParentOwned);
  assert(!Leaf.ReferenceCount && Parent.ReferenceCount==2);
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_NOT_READY && !Owned);
  FailCall=0;
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && Parent.ReferenceCount==1);
  Before=Calls;assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && Calls==Before && Parent.ReferenceCount==1);
  assert(CrRpmhRailRelease(&Io,&Parent)==CR_SUCCESS);

  Reset();assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_SUCCESS);
  FailCall=Calls+4; // child fully withdraws, parent enable withdrawal fails
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_DEVICE_ERROR && Leaf.ParentOwned && !Leaf.EnableRequested);
  assert(Parent.EnableRequested && !Parent.ReferenceCount && !Parent.Ready);
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_NOT_READY && !Owned);
  Before=Calls;FailCall=0;
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && Calls==Before+3 && !Leaf.ParentOwned);

  Reset();FailCall=1;
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_DEVICE_ERROR && Owned && Leaf.ParentOwned);
  assert(Parent.VoltageRequested && !Parent.Ready && Parent.ReferenceCount==1 && Hardware[1][0]==1800);
  FailCall=0;assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && !Hardware[1][0] && !Leaf.ParentOwned);

  // A malformed/lazily aliased graph is rejected before voting the child,
  // while its successfully acquired parent remains releasable.
  Reset();AliasLeaf=TRUE;
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_COMPROMISED_DATA && Owned);
  assert(Calls==3 && Leaf.Address==0 && Leaf.ParentOwned);
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_SUCCESS && !Parent.EnableRequested);
  Reset();Leaf.Address=Parent.Address=0x100;
  assert(CrRpmhRailValidate(&Leaf)==CR_COMPROMISED_DATA && Calls==0 && Lookups==0);
  Leaf.Address=0;Parent.Parent=&Leaf;Parent.ParentMv=1800;Parent.ParentMode=7;
  assert(CrRpmhRailValidate(&Leaf)==CR_COMPROMISED_DATA && Calls==0);
  Reset();Leaf.ParentMode=0;
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_COMPROMISED_DATA && !Owned && Calls==0);
  Reset();assert(CrRpmhRailAcquire(&Io,&Leaf,1200,5,&Owned)==CR_INVALID_PARAMETER && !Owned && Calls==0);
  Parent.ReferenceCount=~((UINTN)0);
  assert(CrRpmhRailAcquire(&Io,&Parent,1800,7,&Owned)==CR_OUT_OF_RESOURCES && !Owned);
#ifdef _KERNEL_MODE
  Reset();CurrentIrql=DISPATCH_LEVEL;
  assert(CrRpmhRailValidate(&Leaf)==CR_INVALID_PARAMETER);
  assert(CrRpmhRailAcquire(&Io,&Leaf,1200,7,&Owned)==CR_INVALID_PARAMETER && !Owned);
  assert(CrRpmhRailRelease(&Io,&Leaf)==CR_INVALID_PARAMETER);
  assert(CrRpmhRailVote(&Io,&Leaf,CrRpmhRailEnable,1)==CR_INVALID_PARAMETER && Calls==0 && Lookups==0);
  CurrentIrql=PASSIVE_LEVEL;
#endif
  OwnerVotes();
  OwnerJournal();
  puts("RPMh supply graph: fixed contracts, shared parents, committed errors, retained cleanup, pure qualification and aliases passed");
  return 0;
}
