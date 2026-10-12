/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/interconnect.h>
#include <Library/PepBandwidthLib.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifdef _KERNEL_MODE
static KIRQL CurrentIrql;
KIRQL KeGetCurrentIrql(VOID) { return CurrentIrql; }
VOID KeInitializeMutex(KMUTEX *M, ULONG Level) { assert(Level==0);*M=1; }
NTSTATUS KeWaitForSingleObject(VOID *Object, KWAIT_REASON Reason,
    KPROCESSOR_MODE Mode, BOOLEAN Alertable, LARGE_INTEGER *Timeout) {
  KMUTEX *M=Object;
  assert(CurrentIrql==PASSIVE_LEVEL && *M==1 && Reason==Executive);
  assert(Mode==KernelMode && !Alertable && Timeout==NULL);
  *M=2;return STATUS_SUCCESS;
}
LONG KeReleaseMutex(KMUTEX *M, BOOLEAN Wait) {
  assert(CurrentIrql==PASSIVE_LEVEL && *M==2 && !Wait);*M=1;return 0;
}
#else
/* Host UEFI primitives; the production voter and topology are linked. */
VOID *EFIAPI CopyMem(OUT VOID *Destination, IN CONST VOID *Source, IN UINTN Length) {
  return memmove(Destination, Source, Length);
}

VOID *EFIAPI SetMem(OUT VOID *Buffer, IN UINTN Length, IN UINT8 Value) {
  return memset(Buffer, Value, Length);
}

INTN EFIAPI CompareMem(IN CONST VOID *Left, IN CONST VOID *Right, IN UINTN Length) {
  return memcmp(Left, Right, Length);
}

INTN EFIAPI AsciiStrCmp(IN CONST CHAR8 *Left, IN CONST CHAR8 *Right) {
  return strcmp(Left, Right);
}

VOID EFIAPI DebugPrint(IN UINTN ErrorLevel, IN CONST CHAR8 *Format, ...) {
  (void)ErrorLevel;
  (void)Format;
}

UINT32 EFIAPI InterlockedCompareExchange32(
    IN OUT volatile UINT32 *Value, IN UINT32 Compare, IN UINT32 Exchange) {
  UINT32 Old = *Value;
  if (Old == Compare) {
    *Value = Exchange;
  }
  return Old;
}

SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) {
  *Lock = 1;
  return Lock;
}

SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 1);
  *Lock = 2;
  return Lock;
}

SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) {
  assert(*Lock == 2);
  *Lock = 1;
  return Lock;
}
#endif

typedef struct {
  UINT32 Calls;
  UINT32 Commands;
  BOOLEAN FailNext;
  UINT32 FailCall;
  UINT32 FailFromCall;
  BOOLEAN CommitBeforeFailure;
  BOOLEAN VcdMode;
  UINT32 Unit;
  UINT32 Hardware[15];
  RpmhTcsCmd Last[INTERCONNECT_MAX_RPMH_COMMANDS];
} MOCK_RPMH;

static CR_STATUS
MockGetAddress(VOID *Context, CONST CHAR8 *Name, UINT32 *Address) {
  (void)Context;
  if (Name == NULL || Address == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (strcmp(Name, "SN7") == 0) *Address = 0x100;
  else if (strcmp(Name, "SH1") == 0) *Address = 0x104;
  else if (strcmp(Name, "SH0") == 0) *Address = 0x108;
  else if (strcmp(Name, "MC0") == 0) *Address = 0x10c;
  else if (strcmp(Name, "CN0") == 0) *Address = 0x110;
  else if (strcmp(Name, "SN2") == 0) *Address = 0x114;
  else if (strcmp(Name, "SN0") == 0) *Address = 0x118;
  else if (strcmp(Name, "MM0") == 0) *Address = 0x11c;
  else if (strcmp(Name, "MM1") == 0) *Address = 0x120;
  else if (strcmp(Name, "QUP1") == 0) *Address = 0x124;
  else if (strcmp(Name, "ACV") == 0) *Address = 0x128;
  else if (strcmp(Name, "QUP0") == 0) *Address = 0x12c;
  else if (strcmp(Name, "SN3") == 0) *Address = 0x130;
  else if (strcmp(Name, "CE0") == 0) *Address = 0x134;
  else if (strcmp(Name, "QUP2") == 0) *Address = 0x138;
  else return CR_NOT_FOUND;
  return CR_SUCCESS;
}

static CR_STATUS
MockGetAux(VOID *Context, CONST CHAR8 *Name, UINT8 *Data, UINT32 *Length) {
  MOCK_RPMH *Mock = (MOCK_RPMH *)Context;
  if (Name == NULL || Data == NULL || Length == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (*Length < 8 || MockGetAddress(NULL, Name, (UINT32[1]){0}) != CR_SUCCESS) {
    return CR_NOT_FOUND;
  }
  memset(Data, 0, 8);
  /* Linux's enable-mask BCMs do not use unit/width conversion.  SM8450 ACV
   * therefore legitimately carries zero values in those fields, while the
   * bandwidth BCMs use unit=1 and width=1 in this host fixture. */
  if (strcmp(Name, "ACV") != 0) {
    UINT32 Unit = Mock != NULL && Mock->Unit != 0 ? Mock->Unit : 1;
    for (UINT32 I = 0; I < 4; ++I) Data[I] = (UINT8)(Unit >> (8 * I));
    Data[4] = 1;
  }
  if (Mock != NULL && Mock->VcdMode) {
    if (strcmp(Name, "SH1") == 0) Data[6] = 1;
    else if (strcmp(Name, "SH0") == 0) Data[6] = 2;
    else if (strcmp(Name, "MC0") == 0) Data[6] = 3;
    else if (strcmp(Name, "CN0") == 0) Data[6] = 4;
    else if (strcmp(Name, "SN2") == 0) Data[6] = 5;
    else if (strcmp(Name, "SN0") == 0) Data[6] = 6;
    else if (strcmp(Name, "MM0") == 0) Data[6] = 7;
    else if (strcmp(Name, "MM1") == 0) Data[6] = 8;
    else if (strcmp(Name, "QUP1") == 0) Data[6] = 9;
    else if (strcmp(Name, "ACV") == 0) Data[6] = 10;
    else if (strcmp(Name, "QUP0") == 0) Data[6] = 11;
    else if (strcmp(Name, "SN3") == 0) Data[6] = 12;
    else if (strcmp(Name, "CE0") == 0) Data[6] = 13;
    else if (strcmp(Name, "QUP2") == 0) Data[6] = 14;
  }
  *Length = 8;
  return CR_SUCCESS;
}

static CR_STATUS
MockWrite(VOID *Context, RpmhTcsCmd *Commands, UINT32 Count) {
#ifdef _KERNEL_MODE
  assert(KeGetCurrentIrql()==PASSIVE_LEVEL);
#endif
  MOCK_RPMH *Mock = (MOCK_RPMH *)Context;
  if (Mock == NULL || Commands == NULL || Count == 0 ||
      Count > INTERCONNECT_MAX_RPMH_COMMANDS) {
    return CR_INVALID_PARAMETER;
  }
  Mock->Calls++;
  BOOLEAN Failed=Mock->FailNext || (Mock->FailCall!=0 && Mock->Calls==Mock->FailCall) ||
                 (Mock->FailFromCall!=0 && Mock->Calls>=Mock->FailFromCall);
  if (!Failed || Mock->CommitBeforeFailure) {
    for (UINT32 I=0;I<Count;++I) {
      assert(Commands[I].addr>=0x100 && Commands[I].addr<=0x138);
      Mock->Hardware[(Commands[I].addr-0x100)/4]=Commands[I].data&0x0fffffffU;
    }
  }
  if (Failed) {
    Mock->FailNext = FALSE;
    Mock->FailCall = 0;
    return CR_DEVICE_ERROR;
  }
  memcpy(Mock->Last, Commands, Count * sizeof(*Commands));
  Mock->Commands = Count;
  return CR_SUCCESS;
}

static UINT16
NodeIndex(CONST InterconnectTargetContext *Target, CONST CHAR8 *Name) {
  UINT16 Index;
  for (Index = 0; Index < Target->NodeCount; Index++) {
    if (strcmp(Target->Nodes[Index].Name, Name) == 0) {
      return Index;
    }
  }
  assert(0 && "target node missing");
  return 0;
}

static BOOLEAN
PathHasNode(CONST InterconnectDeviceContext *Context,
            CONST InterconnectPathRuntime *Path, CONST CHAR8 *Name) {
  UINT16 Index = NodeIndex(Context->Target, Name);
  return (Path->NodeMask[Index / 64] & (1ULL << (Index % 64))) != 0;
}

static BOOLEAN LateVoteFailure;
static CR_STATUS OwnerValidate(VOID *Context,VOID *Owner,CONST CR_ICB_PATH *Path) {
  return CrIcbVotesValidate(Context,Owner,Path);
}
static CR_STATUS OwnerGet(VOID *Context,VOID *Owner,CONST CR_ICB_PATH *Path,
                           CR_ICB_VOTE_VALUE *Value,BOOLEAN *Pending) {
  return CrIcbVotesGet(Context,Owner,Path,Value,Pending);
}
static CR_STATUS OwnerSet(VOID *Context,VOID *Owner,CONST CR_ICB_PATH *Path,
                           CONST CR_ICB_VOTE_VALUE *Value) {
  CR_STATUS Status=CrIcbVotesSet(Context,Owner,Path,Value);
  if (!CR_ERROR(Status) && LateVoteFailure) {LateVoteFailure=FALSE;return CR_DEVICE_ERROR;}
  return Status;
}
static void OwnerJournal(void) {
  InterconnectDeviceContext Storage={0},*Core=&Storage;
  MOCK_RPMH Mock={.VcdMode=TRUE,.Unit=1000};
  // One command per TCS forces separate VCD batches. With capacity16 even
  // distinct VCDs fit into a single write, so fail-call2 cannot hit the apply.
  InterconnectIoOps Io={MockGetAddress,MockGetAux,MockWrite,&Mock,&Mock,1};
  assert(InterconnectLibInit(&Core,CrTargetGetInterconnectContext(),&Io)==CR_SUCCESS);
  CR_ICB_VOTER Voter={0};CR_ICB_VOTE_ENTRY Entries[3];UINT32 Owners[3];
  CR_ICB_PATH Path={"qcom,sm8450-pcie",0x1000,0x1100};
  CR_ICB_PATH Config={"qcom,sm8450-dwc3",0x6002,0x6003};
  CR_ICB_PATH Bad=Path;Bad.DestinationId=0x9999;
  UINT32 Calls=Mock.Calls;
  assert(InterconnectValidatePath(Core,Path.Provider,Path.SourceId,Path.DestinationId)==CR_SUCCESS);
  assert(InterconnectValidatePath(Core,Bad.Provider,Bad.SourceId,Bad.DestinationId)==CR_NOT_FOUND);
  assert(Mock.Calls==Calls);
  INTERCONNECT_PATH_HANDLE Foreign;
  assert(InterconnectAcquirePath(Core,Path.Provider,Path.SourceId,Path.DestinationId,&Foreign)==CR_SUCCESS);
  assert(InterconnectSetBandwidth(Core,Foreign,300,500)==CR_SUCCESS);
  assert(CrIcbVotesInit(&Voter,Core,Entries,3)==CR_SUCCESS);
  assert(CrIcbVotesValidate(&Voter,&Owners[0],&Path)==CR_NOT_FOUND);
  assert(CrIcbVotesReserve(&Voter,&Owners[0],&Bad)==CR_NOT_FOUND);
  Calls=Mock.Calls;
  for (UINTN O=0;O<2;++O) {
    assert(CrIcbVotesReserve(&Voter,&Owners[O],&Path)==CR_SUCCESS);
    assert(CrIcbVotesReserve(&Voter,&Owners[O],&Path)==CR_SUCCESS);
  }
  assert(CrIcbVotesReserve(&Voter,&Owners[0],&Config)==CR_SUCCESS && Mock.Calls==Calls);
  assert(CrIcbVotesReserve(&Voter,&Owners[2],&Path)==CR_OUT_OF_RESOURCES);
  assert(CrIcbVotesUninit(&Voter)==CR_BUSY && CrIcbVotesRetireAfterDeinit(&Voter)==CR_BUSY);
  CR_PEP_BANDWIDTH_BINDING Bindings[]={
    {.Master={"master",6},.Slave={"memory",6},.Path=Path,.PolicyUnitsPerKbyte=1000},
    {.Master={"master",6},.Slave={"config",6},.Path=Config,.PolicyUnitsPerKbyte=1}
  };
  CR_PEP_BANDWIDTH_IO BackendIo={&Voter,OwnerValidate,OwnerGet,OwnerSet};
  CR_PEP_BANDWIDTH_PROVIDER Provider={0};
  CR_PEP_BANDWIDTH_BINDING Duplicates[]={Bindings[0],Bindings[0]};
  assert(CrPepBandwidthProviderInit(&Provider,&BackendIo,Duplicates,2)==CR_INVALID_PARAMETER);
  assert(CrPepBandwidthProviderInit(&Provider,&BackendIo,Bindings,2)==CR_SUCCESS);
  CR_PEP_JOURNAL_PROVIDER Backend;
  assert(CrPepBandwidthProviderGetJournal(&Provider,&Backend)==CR_SUCCESS);
  CR_PEP_JOURNAL Journal={0};CR_PEP_JOURNAL_ENTRY Tx[8],Owned[4];
  assert(CrPepJournalInit(&Journal,&Owners[0],&Backend,Tx,8,Owned,4)==CR_SUCCESS);
  CR_PEP_RESOURCE_OP Op={.Kind=CrPepBusArb,.Name={"master",6},.SecondaryName={"memory",6},
    .Arguments={3,100001,200001},.ArgumentCount=3};
  Calls=Mock.Calls;
  CONST CR_PEP_BANDWIDTH_BINDING *Description=NULL;CR_ICB_VOTE_VALUE DescribedValue;
  assert(CrPepBandwidthProviderDescribe(&Provider,&Op,&Description,&DescribedValue)==CR_SUCCESS &&
         Description==&Bindings[0] && DescribedValue.Average==101 && DescribedValue.Peak==201 && Calls==Mock.Calls);
  assert(CrPepJournalValidate(&Journal,&Op)==CR_SUCCESS && Calls==Mock.Calls);
  CR_PEP_RESOURCE_OP Invalid=Op;Invalid.Arguments[2]=10;
  assert(CrPepJournalValidate(&Journal,&Invalid)==CR_INVALID_PARAMETER && Calls==Mock.Calls);
  Invalid=Op;Invalid.Arguments[0]=2;
  assert(CrPepBandwidthProviderDescribe(&Provider,&Invalid,&Description,&DescribedValue)==CR_UNSUPPORTED && Description==NULL);
  assert(CrPepJournalValidate(&Journal,&Invalid)==CR_UNSUPPORTED);
  CR_ICB_VOTE_VALUE Value;BOOLEAN Pending;
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS && CrPepJournalApply(&Journal,&Op)==CR_SUCCESS);
  assert(CrIcbVotesGet(&Voter,&Owners[0],&Path,&Value,&Pending)==CR_SUCCESS &&
         Value.Present && Value.Average==101 && Value.Peak==201 && !Pending);
  UINT16 Node=NodeIndex(Core->Target,"xm_pcie3_0");
  assert(Core->NodeAverage[Node]==401 && Core->NodePeak[Node]==500);
  Calls=Mock.Calls;
  assert(CrPepJournalApply(&Journal,&Op)==CR_SUCCESS && Mock.Calls==Calls);
  assert(CrPepJournalCommit(&Journal)==CR_SUCCESS);
  CR_ICB_VOTE_VALUE Other={.Present=TRUE,.Average=200,.Peak=600};
  assert(CrIcbVotesSet(&Voter,&Owners[1],&Path,&Other)==CR_SUCCESS);
  assert(Core->NodeAverage[Node]==601 && Core->NodePeak[Node]==600);
  // Repeated-resource undo and a failure AFTER a real successful vote.
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS);
  Op.Arguments[1]=400000;Op.Arguments[2]=700000;
  assert(CrPepJournalApply(&Journal,&Op)==CR_SUCCESS);
  Op.Arguments[1]=500000;Op.Arguments[2]=800000;LateVoteFailure=TRUE;
  assert(CrPepJournalApply(&Journal,&Op)==CR_DEVICE_ERROR);
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS && Core->NodeAverage[Node]==601 && Core->NodePeak[Node]==600);
  // Hardware may have committed an earlier VCD before failure and rollback
  // can fail too. Pure qualification must not hide it by repairing hardware.
  Op.Arguments[1]=500000000;Op.Arguments[2]=800000000;
  Mock.CommitBeforeFailure=TRUE;Mock.FailFromCall=Mock.Calls+2;
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS);
  CR_STATUS FailedApply=CrPepJournalApply(&Journal,&Op);
  if (FailedApply!=CR_DEVICE_ERROR || !Core->RollbackPending) {
    fprintf(stderr,"partial apply status=%llx calls=%u failfrom=%u pending=%u rollback=%llx\n",
      (unsigned long long)FailedApply,Mock.Calls,Mock.FailFromCall,Core->RollbackPending,
      (unsigned long long)Core->LastRollbackStatus);
    for (UINTN I=0;I<Core->Target->BcmCount;++I)
      fprintf(stderr,"%s vcd=%u programmed=%llu/%llu\n",Core->Target->Bcms[I].Name,Core->Bcm[I].Vcd,
        (unsigned long long)Core->Bcm[I].ProgrammedX,(unsigned long long)Core->Bcm[I].ProgrammedY);
  }
  assert(FailedApply==CR_DEVICE_ERROR && Core->RollbackPending);
  Calls=Mock.Calls;
  assert(InterconnectValidatePath(Core,Path.Provider,Path.SourceId,Path.DestinationId)==CR_NOT_READY);
  assert(CrIcbVotesGet(&Voter,&Owners[0],&Path,&Value,&Pending)==CR_SUCCESS && Pending && Value.Average==101);
  assert(CrPepJournalValidate(&Journal,&Op)==CR_NOT_READY && Calls==Mock.Calls);
  assert(CrPepJournalRollback(&Journal)==CR_DEVICE_ERROR && Journal.TransactionCount==1);
  Mock.FailFromCall=0;
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS && !Core->RollbackPending);
  assert(Core->NodeAverage[Node]==601 && Core->Paths[Foreign-1].AverageBandwidth==300);
  // Failed Close retains an original snapshot and its private reserved handle.
  Mock.FailFromCall=Mock.Calls+1;
  assert(CrPepJournalClose(&Journal)==CR_DEVICE_ERROR && Journal.OwnedCount==1);
  assert(CrPepJournalBegin(&Journal)==CR_BUSY && Entries[0].Owner==&Owners[0]);
  Mock.FailFromCall=0;
  assert(CrPepJournalClose(&Journal)==CR_SUCCESS && Core->NodeAverage[Node]==500 && Core->NodePeak[Node]==600);
  assert(CrPepJournalValidate(&Journal,&Op)==CR_SUCCESS); // absent demand, still reserved
  assert(CrIcbVotesGet(&Voter,&Owners[0],&Path,&Value,&Pending)==CR_SUCCESS && !Value.Present);
  Op.SecondaryName=(CR_PEP_TEXT){"config",6};Op.Arguments[1]=7;Op.Arguments[2]=9;
  assert(CrPepJournalBegin(&Journal)==CR_SUCCESS && CrPepJournalApply(&Journal,&Op)==CR_SUCCESS);
  assert(CrIcbVotesGet(&Voter,&Owners[0],&Config,&Value,&Pending)==CR_SUCCESS && Value.Average==7 && Value.Peak==9);
  assert(CrPepJournalRollback(&Journal)==CR_SUCCESS);
  assert(CrPepJournalUninit(&Journal)==CR_SUCCESS && CrPepBandwidthProviderUninit(&Provider)==CR_SUCCESS);
#ifdef _KERNEL_MODE
  CurrentIrql=DISPATCH_LEVEL;Calls=Mock.Calls;
  assert(CrIcbVotesReserve(&Voter,&Owners[2],&Path)==CR_INVALID_PARAMETER);
  assert(CrIcbVotesGet(&Voter,&Owners[0],&Path,&Value,&Pending)==CR_INVALID_PARAMETER);
  assert(CrIcbVotesSet(&Voter,&Owners[0],&Path,&Other)==CR_INVALID_PARAMETER);
  assert(CrIcbVotesDrain(&Voter)==CR_INVALID_PARAMETER && Calls==Mock.Calls);
  CurrentIrql=PASSIVE_LEVEL;
#endif
  Mock.FailFromCall=Mock.Calls+1;
  assert(CrIcbVotesReleaseOwner(&Voter,&Owners[1])==CR_DEVICE_ERROR && Entries[1].Owner==&Owners[1]);
  Mock.FailFromCall=0;
  assert(CrIcbVotesReleaseOwner(&Voter,&Owners[1])==CR_SUCCESS && Core->NodeAverage[Node]==300);
  assert(CrIcbVotesDrain(&Voter)==CR_SUCCESS && CrIcbVotesUninit(&Voter)==CR_SUCCESS);
  // Core capacity includes foreign handles; pure qualification does not reserve.
  INTERCONNECT_PATH_HANDLE Handles[INTERCONNECT_MAX_PATHS-1];
  for (UINTN I=0;I<ARRAY_SIZE(Handles);++I)
    assert(InterconnectAcquirePath(Core,Path.Provider,Path.SourceId,Path.DestinationId,&Handles[I])==CR_SUCCESS);
  assert(CrIcbVotesInit(&Voter,Core,Entries,3)==CR_SUCCESS);
  Calls=Mock.Calls;
  assert(CrIcbVotesReserve(&Voter,&Owners[0],&Path)==CR_OUT_OF_RESOURCES && Calls==Mock.Calls);
  assert(CrIcbVotesUninit(&Voter)==CR_SUCCESS);
  for (UINTN I=0;I<ARRAY_SIZE(Handles);++I) assert(InterconnectReleasePath(Core,Handles[I])==CR_SUCCESS);
  assert(CrIcbVotesInit(&Voter,Core,Entries,3)==CR_SUCCESS);
  assert(CrIcbVotesReserve(&Voter,&Owners[0],&Path)==CR_SUCCESS);
  assert(CrIcbVotesSet(&Voter,&Owners[0],&Path,&Other)==CR_SUCCESS);
  Mock.FailFromCall=Mock.Calls+1;
  assert(InterconnectLibDeinit(Core)==CR_DEVICE_ERROR && CrIcbVotesRetireAfterDeinit(&Voter)==CR_BUSY);
  assert(Entries[0].Owner==&Owners[0]);Mock.FailFromCall=0;
  assert(InterconnectLibDeinit(Core)==CR_SUCCESS && CrIcbVotesRetireAfterDeinit(&Voter)==CR_SUCCESS);
  puts("ICB owners: reserved capacity, explicit units, foreign aggregation, late writes, uncertain rollback and journal Close retry passed");
}

int
main(void) {
  MOCK_RPMH Mock = {0};
  InterconnectDeviceContext *Context = NULL;
  InterconnectIoOps Io = {
      .GetAddress = MockGetAddress,
      .GetAuxData = MockGetAux,
      .WriteRpmh = MockWrite,
      .CmdDbContext = &Mock,
      .RpmhContext = &Mock,
      .MaxRpmhCommands = INTERCONNECT_MAX_RPMH_COMMANDS,
  };
  INTERCONNECT_PATH_HANDLE MemPath;
  INTERCONNECT_PATH_HANDLE CpuPath;
  INTERCONNECT_PATH_HANDLE UfsPath;
  INTERCONNECT_PATH_HANDLE UfsCfgPath;
  INTERCONNECT_PATH_HANDLE MdpPath;
  INTERCONNECT_PATH_HANDLE MdpCfgPath;
  INTERCONNECT_PATH_HANDLE QupPaths[3];
  UINT64 OldAverage;
  UINT64 OldPeak;
  CONST InterconnectTargetContext *Target;

  assert(InterconnectLibInit(&Context, CrTargetGetInterconnectContext(), &Io) == CR_SUCCESS);
  assert(Context != NULL && Context->Initialized);
  Target = Context->Target;
  static const char *ExpectedProviders[]={"qcom,sm8450-pcie","qcom,sm8450-ufs",
    "qcom,sm8450-mdp","qcom,sm8450-qup1","qcom,sm8450-qup0","qcom,sm8450-dwc3",
    "qcom,sm8450-pil","qcom,sm8450-sdhci","qcom,sm8450-debug-uart","qcom,sm8450-gpu"};
  assert(Target->ProviderCount == sizeof(ExpectedProviders)/sizeof(ExpectedProviders[0]));
  for (UINT16 I=0; I<Target->ProviderCount; ++I)
    assert(strcmp(Target->Providers[I].Compatible, ExpectedProviders[I]) == 0);
  assert(Context->Bcm[10].Unit == 0 && Context->Bcm[10].Width == 0);
  assert(strcmp(Target->Providers[0].Compatible, "qcom,sm8450-pcie") == 0);
  /* Only Linux keepalive BCMs are sent during initialization. */
  assert(Mock.Commands == 8);
  assert(Context->Bcm[2].ProgrammedX == 1 && Context->Bcm[2].ProgrammedY == 1);
  assert(Context->Bcm[3].ProgrammedX == 1 && Context->Bcm[3].ProgrammedY == 1);
  assert(Context->Bcm[4].ProgrammedX == 1 && Context->Bcm[4].ProgrammedY == 1);
  assert(Context->Bcm[7].ProgrammedX == 1 && Context->Bcm[7].ProgrammedY == 1);
  assert(Context->Bcm[9].ProgrammedX == 1 && Context->Bcm[9].ProgrammedY == 1);
  {
    INTERCONNECT_PATH_HANDLE UsbMemory, UsbConfig;
    assert(InterconnectAcquirePath(Context, "qcom,sm8450-dwc3", 0x6000, 0x6001,
                                   &UsbMemory) == CR_SUCCESS);
    assert(PathHasNode(Context, &Context->Paths[UsbMemory - 1], "xm_usb3_0"));
    assert(PathHasNode(Context, &Context->Paths[UsbMemory - 1], "qnm_aggre1_noc"));
    assert(!PathHasNode(Context, &Context->Paths[UsbMemory - 1], "xm_ufs_mem"));
    assert(InterconnectSetBandwidth(Context, UsbMemory, 1000000, 2500000) == CR_SUCCESS);
    assert(Context->Bcm[5].ProgrammedY != 0);
    assert(InterconnectAcquirePath(Context, "qcom,sm8450-dwc3", 0x6002, 0x6003,
                                   &UsbConfig) == CR_SUCCESS);
    assert(PathHasNode(Context, &Context->Paths[UsbConfig - 1], "qhs_usb3_0"));
    assert(InterconnectSetBandwidth(Context, UsbConfig, 0, 40000) == CR_SUCCESS);
    assert(InterconnectReleasePath(Context, UsbMemory) == CR_SUCCESS);
    assert(InterconnectReleasePath(Context, UsbConfig) == CR_SUCCESS);
    assert(Context->Bcm[5].ProgrammedY == 0);
  }

  /* Actual DT SDCC2 paths use A2NOC and CN0, independently of USB/UFS
     and crypto demand. A retained failed release must not drop a foreign
     A2NOC vote, or require the APSS configuration path to carry memory. */
  {
    INTERCONNECT_PATH_HANDLE SdMemory,SdConfig,Foreign;
    assert(InterconnectAcquirePath(Context,"qcom,sm8450-sdhci",0x8000,0x8001,&SdMemory)==CR_SUCCESS);
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"xm_sdc2"));
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"qns_a2noc_snoc"));
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"qnm_aggre2_noc"));
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"qnm_snoc_sf"));
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"qns_llcc"));
    assert(PathHasNode(Context,&Context->Paths[SdMemory-1],"ebi"));
    assert(!PathHasNode(Context,&Context->Paths[SdMemory-1],"qnm_aggre1_noc"));
    assert(InterconnectAcquirePath(Context,"qcom,sm8450-sdhci",0x8002,0x8003,&SdConfig)==CR_SUCCESS);
    assert(PathHasNode(Context,&Context->Paths[SdConfig-1],"qhs_sdc2"));
    assert(!PathHasNode(Context,&Context->Paths[SdConfig-1],"xm_sdc2"));
    assert(InterconnectSetBandwidth(Context,SdMemory,100000,200000)==CR_SUCCESS);
    assert(InterconnectSetBandwidth(Context,SdConfig,0,40000)==CR_SUCCESS);
    assert(Context->Bcm[12].ProgrammedY!=0 && Context->Bcm[10].ProgrammedY==8 && Context->Bcm[4].ProgrammedY==1);
    assert(Context->Bcm[5].ProgrammedY==0 && Context->Bcm[13].ProgrammedY==0);
    assert(InterconnectAcquirePath(Context,"qcom,sm8450-pil",0x7000,0x7001,&Foreign)==CR_SUCCESS);
    assert(InterconnectSetBandwidth(Context,Foreign,100000,200000)==CR_SUCCESS);
    UINT32 ForeignDemand=Context->Bcm[12].ProgrammedY;
    Mock.FailFromCall=Mock.Calls+1;
    assert(InterconnectReleasePath(Context,SdMemory)==CR_DEVICE_ERROR);
    assert(Context->Paths[SdMemory-1].InUse);
    Mock.FailFromCall=0;
    assert(InterconnectReleasePath(Context,SdMemory)==CR_SUCCESS && Context->Bcm[12].ProgrammedY==ForeignDemand);
    assert(InterconnectReleasePath(Context,SdConfig)==CR_SUCCESS);
    assert(InterconnectReleasePath(Context,Foreign)==CR_SUCCESS && Context->Bcm[12].ProgrammedY==0);
  }

  /* Authentication uses the Android MASTER_CRYPTO -> EBI route. Its CE0
     vote must stay independent of USB's A1NOC path and drop on release. */
  {
    INTERCONNECT_PATH_HANDLE Crypto;
    assert(InterconnectAcquirePath(Context, "qcom,sm8450-pil", 0x7000, 0x7001,
                                   &Crypto) == CR_SUCCESS);
    assert(PathHasNode(Context, &Context->Paths[Crypto - 1], "qxm_crypto"));
    assert(PathHasNode(Context, &Context->Paths[Crypto - 1], "qns_a2noc_snoc"));
    assert(PathHasNode(Context, &Context->Paths[Crypto - 1], "qnm_aggre2_noc"));
    assert(PathHasNode(Context, &Context->Paths[Crypto - 1], "qnm_snoc_sf"));
    assert(PathHasNode(Context, &Context->Paths[Crypto - 1], "ebi"));
    assert(!PathHasNode(Context, &Context->Paths[Crypto - 1], "qnm_aggre1_noc"));
    assert(InterconnectSetBandwidth(Context, Crypto, 0, 0xffffffffU) == CR_SUCCESS);
    assert(Context->Bcm[13].ProgrammedY != 0 && Context->Bcm[12].ProgrammedY != 0);
    assert(Context->Bcm[10].ProgrammedY == 8);
    assert(InterconnectReleasePath(Context, Crypto) == CR_SUCCESS);
    assert(Context->Bcm[13].ProgrammedY == 0 && Context->Bcm[12].ProgrammedY == 0);
    assert(Context->Bcm[10].ProgrammedY == 0);
  }

  /* A2NOC must serve QUP0 without accidentally selecting QUP1's A1NOC. */
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup0", 0x5100, 0x5101,
                                 &QupPaths[0]) == CR_SUCCESS);
  assert(InterconnectSetBandwidth(Context, QupPaths[0], 960, 960) == CR_SUCCESS);
  assert(Context->Bcm[11].ProgrammedX == 240 && Context->Bcm[11].ProgrammedY == 240);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup0", 0x5102, 0x5103,
                                 &QupPaths[1]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qnm_aggre2_noc"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qns_llcc"));
  assert(!PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qnm_aggre1_noc"));
  assert(InterconnectSetBandwidth(Context, QupPaths[1], 1, 1) == CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup0", 0x5104, 0x5105,
                                 &QupPaths[2]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "qhm_qup0"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "qns_a2noc_snoc"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, QupPaths[2], 1000, 1000) == CR_SUCCESS);
  assert(Context->Bcm[12].ProgrammedY != 0);
  for (UINTN Index = 0; Index < 3; ++Index)
    assert(InterconnectReleasePath(Context, QupPaths[Index]) == CR_SUCCESS);
  assert(Context->Bcm[11].ProgrammedY == 1 && Context->Bcm[12].ProgrammedY == 0);
  assert(Context->Bcm[6].ProgrammedX == 1 && Context->Bcm[6].ProgrammedY == 1);

  /* Debug UART has exactly the Linux QUP2 virtual core and APSS config
     paths; it must never acquire an invented QUP2-to-DDR data path. */
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-debug-uart", 0x5200,
                                 0x5201, &QupPaths[0]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[0] - 1], "qup2_core_master"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[0] - 1], "qup2_core_slave"));
  assert(InterconnectSetBandwidth(Context, QupPaths[0], 960, 960) == CR_SUCCESS);
  assert(Context->Bcm[14].ProgrammedX == 240 && Context->Bcm[14].ProgrammedY == 240);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-debug-uart", 0x5202,
                                 0x5203, &QupPaths[1]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "chm_apps"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qhs_qup2"));
  assert(!PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, QupPaths[1], 1, 1) == CR_SUCCESS);
  assert(Context->Bcm[4].ProgrammedY != 0);
  for (UINTN Index = 0; Index < 2; ++Index)
    assert(InterconnectReleasePath(Context, QupPaths[Index]) == CR_SUCCESS);
  assert(Context->Bcm[14].ProgrammedX == 1 && Context->Bcm[14].ProgrammedY == 1);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-debug-uart", 0x5200,
                                 0x5203, &QupPaths[0]) == CR_NOT_FOUND);

  /* Use the public Waipio QUP1 provider path, including its virtual core
     clock scale. A default 1000x scale would over-vote this clock. */
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup1", 0x5000, 0x5001,
                                 &QupPaths[0]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[0] - 1], "qup1_core_master"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[0] - 1], "qup1_core_slave"));
  assert(InterconnectSetBandwidth(Context, QupPaths[0], 960, 960) == CR_SUCCESS);
  assert(Context->Bcm[9].ProgrammedX == 240 && Context->Bcm[9].ProgrammedY == 240);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup1", 0x5002, 0x5003,
                                 &QupPaths[1]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qnm_aggre1_noc"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qns_gemnoc_sf"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qnm_snoc_sf"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "qns_llcc"));
  assert(!PathHasNode(Context, &Context->Paths[QupPaths[1] - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, QupPaths[1], 1, 1) == CR_SUCCESS);
  assert(Context->Bcm[5].ProgrammedX != 0 && Context->Bcm[5].ProgrammedY != 0);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-qup1", 0x5004, 0x5005,
                                 &QupPaths[2]) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "qhm_qup1"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "qns_a1noc_snoc"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "qnm_aggre1_noc"));
  assert(PathHasNode(Context, &Context->Paths[QupPaths[2] - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, QupPaths[2], 1000, 1000) == CR_SUCCESS);
  assert(Context->Bcm[10].ProgrammedX == 0 && Context->Bcm[10].ProgrammedY == 8);
  for (UINTN Index = 0; Index < 3; ++Index)
    assert(InterconnectReleasePath(Context, QupPaths[Index]) == CR_SUCCESS);
  assert(Context->Bcm[10].ProgrammedY == 0);
  assert(Context->Bcm[9].ProgrammedX == 1 && Context->Bcm[9].ProgrammedY == 1);

  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x1000, 0x1100,
                                 &MemPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "xm_pcie3_0"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_pcie_mem_noc"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qnm_pcie"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_llcc"));
  assert(PathHasNode(Context, &Context->Paths[MemPath - 1], "ebi"));
  assert(!PathHasNode(Context, &Context->Paths[MemPath - 1], "qns_pcie"));
  assert(InterconnectSetBandwidth(Context, MemPath, 100, 200) == CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].AverageBandwidth == 100);
  assert(Context->Bcm[0].VoteX != 0 && Context->Bcm[0].VoteY != 0);

  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x2000, 0x2100,
                                 &CpuPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "chm_apps"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "qns_pcie"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "qnm_gemnoc_pcie"));
  assert(PathHasNode(Context, &Context->Paths[CpuPath - 1], "xs_pcie_0"));
  assert(!PathHasNode(Context, &Context->Paths[CpuPath - 1], "qhs_pcie0_cfg"));
  assert(InterconnectSetBandwidth(Context, CpuPath, 10, 20) == CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-ufs", 0x3000, 0x3001,
                                 &UfsPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "xm_ufs_mem"));
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "qns_a1noc_snoc"));
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "qnm_aggre1_noc"));
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "qns_gemnoc_sf"));
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "qnm_snoc_sf"));
  assert(PathHasNode(Context, &Context->Paths[UfsPath - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, UfsPath, 50000, 100000) == CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-ufs", 0x3002, 0x3003,
                                 &UfsCfgPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[UfsCfgPath - 1], "qhs_ufs_mem_cfg"));
  assert(InterconnectSetBandwidth(Context, UfsCfgPath, 0, 819200) == CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-mdp", 0x4000, 0x4001,
                                 &MdpPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[MdpPath - 1], "qnm_mdp"));
  assert(PathHasNode(Context, &Context->Paths[MdpPath - 1], "qns_mem_noc_hf"));
  assert(PathHasNode(Context, &Context->Paths[MdpPath - 1], "qnm_mnoc_hf"));
  assert(PathHasNode(Context, &Context->Paths[MdpPath - 1], "ebi"));
  assert(InterconnectSetBandwidth(Context, MdpPath, 594000, 1188000) == CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-mdp", 0x4002, 0x4003,
                                 &MdpCfgPath) == CR_SUCCESS);
  assert(PathHasNode(Context, &Context->Paths[MdpCfgPath - 1], "qhs_display_cfg"));
  assert(InterconnectSetBandwidth(Context, MdpCfgPath, 0, 1000) == CR_SUCCESS);
  OldAverage = Context->Paths[MemPath - 1].AverageBandwidth;
  OldPeak = Context->Paths[MemPath - 1].PeakBandwidth;
  Mock.FailNext = TRUE;
  assert(InterconnectSetBandwidth(Context, MemPath, 300000, 400000) != CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].AverageBandwidth == OldAverage);
  assert(Context->Paths[MemPath - 1].PeakBandwidth == OldPeak);

  Mock.FailNext = TRUE;
  assert(InterconnectReleasePath(Context, MemPath) != CR_SUCCESS);
  assert(Context->Paths[MemPath - 1].InUse);
  assert(InterconnectReleasePath(Context, MemPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, CpuPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, UfsPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, UfsCfgPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, MdpPath) == CR_SUCCESS);
  assert(InterconnectReleasePath(Context, MdpCfgPath) == CR_SUCCESS);
  assert(Context->Bcm[0].ProgrammedX == 0 && Context->Bcm[0].ProgrammedY == 0);
  assert(Context->Bcm[1].ProgrammedX == 0 && Context->Bcm[1].ProgrammedY == 0);
  assert(Context->Bcm[2].ProgrammedX == 1 && Context->Bcm[2].ProgrammedY == 1);
  assert(Context->Bcm[3].ProgrammedX == 1 && Context->Bcm[3].ProgrammedY == 1);
  assert(Context->Bcm[4].ProgrammedX == 1 && Context->Bcm[4].ProgrammedY == 1);

  assert(InterconnectLibDeinit(Context) == CR_SUCCESS);
  assert(!Context->Initialized);

  {
    UINT32 Calls=Mock.Calls;
    Io.MaxRpmhCommands=1;
    assert(InterconnectLibInit(&Context,CrTargetGetInterconnectContext(),&Io)==CR_OUT_OF_RESOURCES);
    assert(Mock.Calls==Calls && !Context->Initialized && !Context->DeinitPending);
  }

  /* Give each BCM a separate VCD and fail the second batch.  The first batch
   * is already visible to RPMh when the second one fails; the library must
   * submit the restored votes before returning the error. */
  Mock.VcdMode = TRUE;
  Mock.FailNext = FALSE;
  Mock.FailCall = 0;
  Io.MaxRpmhCommands = 1;
  assert(InterconnectLibInit(&Context, CrTargetGetInterconnectContext(), &Io) ==
         CR_SUCCESS);
  assert(InterconnectAcquirePath(Context, "qcom,sm8450-pcie", 0x1000, 0x1100,
                                 &MemPath) == CR_SUCCESS);
  {
    UINT32 StartCalls = Mock.Calls;
    Mock.FailCall = StartCalls + 2;
    assert(InterconnectSetBandwidth(Context, MemPath, 100, 200) != CR_SUCCESS);
    assert(Context->Paths[MemPath - 1].AverageBandwidth == 0);
    assert(Context->Bcm[0].ProgrammedX == 0 && Context->Bcm[0].ProgrammedY == 0);
    assert(Context->Bcm[1].ProgrammedX == 0 && Context->Bcm[1].ProgrammedY == 0);
    assert(!Context->Bcm[0].Dirty && !Context->Bcm[1].Dirty);
  }
  assert(InterconnectReleasePath(Context, MemPath) == CR_SUCCESS);
  assert(InterconnectLibDeinit(Context) == CR_SUCCESS);
  assert(InterconnectLibInit(&Context, CrTargetGetInterconnectContext(), &Io)==CR_SUCCESS);
  assert(InterconnectAcquirePath(Context,"qcom,sm8450-pcie",0x1000,0x1100,&MemPath)==CR_SUCCESS);
#ifdef _KERNEL_MODE
  {
    UINT32 Calls=Mock.Calls;
    INTERCONNECT_PATH_HANDLE Other=0xabcdef;
    CurrentIrql=DISPATCH_LEVEL;
    assert(InterconnectSetBandwidth(Context,MemPath,100,200)==CR_INVALID_PARAMETER);
    assert(InterconnectReleasePath(Context,MemPath)==CR_INVALID_PARAMETER);
    assert(InterconnectAcquirePath(Context,"qcom,sm8450-pcie",0x1000,0x1100,&Other)==CR_INVALID_PARAMETER);
    assert(Other==0xabcdef && Mock.Calls==Calls && Context->Paths[MemPath-1].InUse);
    assert(InterconnectLibDeinit(Context)==CR_INVALID_PARAMETER);
    assert(InterconnectRecoverVotes(Context)==CR_INVALID_PARAMETER);
    assert(InterconnectGetProviderCount(Context)==0);
    assert(InterconnectLibInit(&Context,CrTargetGetInterconnectContext(),&Io)==CR_INVALID_PARAMETER);
    CurrentIrql=PASSIVE_LEVEL;
  }
#endif
  {
    UINT32 Saved[15];memcpy(Saved,Mock.Hardware,sizeof(Saved));
    Mock.CommitBeforeFailure=TRUE;Mock.FailNext=TRUE;
    assert(InterconnectSetBandwidth(Context,MemPath,100,200)==CR_DEVICE_ERROR);
    assert(memcmp(Saved,Mock.Hardware,sizeof(Saved))==0 && !Context->RollbackPending);
    Mock.FailFromCall=Mock.Calls+2;
    assert(InterconnectSetBandwidth(Context,MemPath,100,200)==CR_DEVICE_ERROR);
    assert(Context->RollbackPending && Context->LastRollbackStatus==CR_DEVICE_ERROR);
    assert(Context->Paths[MemPath-1].AverageBandwidth==0 && Context->Paths[MemPath-1].InUse);
    INTERCONNECT_PATH_HANDLE Another=0xabcdef;
    assert(InterconnectAcquirePath(Context,"qcom,sm8450-pcie",0x1000,0x1100,&Another)==CR_DEVICE_ERROR);
    assert(Another==0xabcdef);
    assert(InterconnectSetBandwidth(Context,MemPath,300,400)==CR_DEVICE_ERROR);
    assert(InterconnectReleasePath(Context,MemPath)==CR_DEVICE_ERROR && Context->Paths[MemPath-1].InUse);
    Mock.FailFromCall=0;
    assert(InterconnectRecoverVotes(Context)==CR_SUCCESS && !Context->RollbackPending);
    assert(memcmp(Saved,Mock.Hardware,sizeof(Saved))==0);
  }
  assert(InterconnectLibDeinit(Context)==CR_SUCCESS);
  Mock.FailFromCall=Mock.Calls+3;
  assert(InterconnectLibInit(&Context,CrTargetGetInterconnectContext(),&Io)==CR_DEVICE_ERROR);
  assert(Context->DeinitPending && !Context->Initialized && Context->Target!=NULL);
  assert(Context->Io.WriteRpmh==MockWrite && Context->LastRollbackStatus==CR_DEVICE_ERROR);
  assert(InterconnectLibInit(&Context,CrTargetGetInterconnectContext(),&Io)==CR_BUSY);
  assert(InterconnectLibDeinit(Context)==CR_DEVICE_ERROR && Context->DeinitPending);
  Mock.FailFromCall=0;
  assert(InterconnectLibDeinit(Context)==CR_SUCCESS && !Context->DeinitPending);
  for (UINTN I=0;I<ARRAY_SIZE(Mock.Hardware);++I) assert(Mock.Hardware[I]==0);
  OwnerJournal();
  puts("SM8450 ICB: Linux paths, BCM votes, uncertain hardware commit, retained rollback and init cleanup retry passed");
  return 0;
}
