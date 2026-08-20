/** @file
 *  OS-independent Qualcomm RPMh interconnect voter.
 *
 *  The aggregation and BCM command encoding follow Linux icc-rpmh and
 *  bcm-voter. Platform-specific code supplies generated topology plus CmdDB
 *  and RPMh callbacks.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/CrTargetInterconnectLib.h>
#include <Library/interconnect.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_string.h>

#ifdef _KERNEL_MODE
#include "interconnect.tmh"
#endif

#define INTERCONNECT_BCM_AUX_SIZE       8
#define INTERCONNECT_BCM_VOTE_MASK      0x3FFFU
#define INTERCONNECT_BCM_COMMIT_BIT     BIT(30)
#define INTERCONNECT_BCM_VALID_BIT      BIT(29)
#define INTERCONNECT_BCM_VOTE_X_SHIFT  14
#define INTERCONNECT_VOTE_SCALE         1000U
#define INTERCONNECT_INVALID_NODE       0xFFFFU

STATIC InterconnectDeviceContext mInterconnectContext;

STATIC UINT16
ReadLe16(IN CONST UINT8 *Data)
{
  return (UINT16)(Data[0] | ((UINT16)Data[1] << 8));
}

STATIC UINT32
ReadLe32(IN CONST UINT8 *Data)
{
  return (UINT32)Data[0] | ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) | ((UINT32)Data[3] << 24);
}

STATIC UINT64
SaturatingAdd64(IN UINT64 Left, IN UINT64 Right)
{
  if (~Left < Right) {
    return ~0ULL;
  }
  return Left + Right;
}

STATIC UINT64
SaturatingMultiply64(IN UINT64 Left, IN UINT64 Right)
{
  if (Left != 0 && Right > (~0ULL / Left)) {
    return ~0ULL;
  }
  return Left * Right;
}

STATIC UINT64
DivideRoundUp64(IN UINT64 Value, IN UINT64 Divisor)
{
  if (Divisor == 0) {
    return ~0ULL;
  }
  if (Value == 0) {
    return 0;
  }
  return 1 + ((Value - 1) / Divisor);
}

STATIC BOOLEAN
TargetRangeValid(IN UINT16 Offset, IN UINT16 Count, IN UINT16 Limit)
{
  return Offset <= Limit && Count <= (UINT16)(Limit - Offset);
}

STATIC CR_STATUS
ValidateTarget(IN CONST InterconnectTargetContext *Target)
{
  UINT16 Index;
  UINT16 Inner;

  if (Target == NULL || Target->Nodes == NULL || Target->Bcms == NULL ||
      Target->Endpoints == NULL || Target->Providers == NULL ||
      Target->NodeCount == 0 || Target->NodeCount > INTERCONNECT_MAX_NODES ||
      Target->BcmCount == 0 || Target->BcmCount > INTERCONNECT_MAX_BCMS ||
      Target->ProviderCount == 0 || Target->EndpointCount == 0 ||
      (Target->LinkCount != 0 && Target->Links == NULL) ||
      (Target->BcmNodeCount != 0 && Target->BcmNodes == NULL) ||
      (Target->ProviderBcmCount != 0 && Target->ProviderBcms == NULL)) {
    return CR_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Target->NodeCount; Index++) {
    if (Target->Nodes[Index].Name == NULL || Target->Nodes[Index].Channels == 0 ||
        Target->Nodes[Index].BusWidth == 0 ||
        !TargetRangeValid(
            Target->Nodes[Index].LinkOffset, Target->Nodes[Index].LinkCount,
            Target->LinkCount)) {
      return CR_INVALID_PARAMETER;
    }
    for (Inner = 0; Inner < Target->Nodes[Index].LinkCount; Inner++) {
      if (Target->Links[Target->Nodes[Index].LinkOffset + Inner] >=
          Target->NodeCount) {
        return CR_INVALID_PARAMETER;
      }
    }
  }
  for (Index = 0; Index < Target->BcmCount; Index++) {
    if (Target->Bcms[Index].Name == NULL ||
        !TargetRangeValid(
            Target->Bcms[Index].NodeOffset, Target->Bcms[Index].NodeCount,
            Target->BcmNodeCount)) {
      return CR_INVALID_PARAMETER;
    }
    for (Inner = 0; Inner < Target->Bcms[Index].NodeCount; Inner++) {
      if (Target->BcmNodes[Target->Bcms[Index].NodeOffset + Inner] >=
          Target->NodeCount) {
        return CR_INVALID_PARAMETER;
      }
    }
  }
  for (Index = 0; Index < Target->EndpointCount; Index++) {
    if (Target->Endpoints[Index].NodeIndex >= Target->NodeCount) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->ProviderCount; Index++) {
    if (Target->Providers[Index].Compatible == NULL ||
        !TargetRangeValid(
            Target->Providers[Index].EndpointOffset,
            Target->Providers[Index].EndpointCount, Target->EndpointCount) ||
        !TargetRangeValid(
            Target->Providers[Index].BcmOffset,
            Target->Providers[Index].BcmCount, Target->ProviderBcmCount)) {
      return CR_INVALID_PARAMETER;
    }
    for (Inner = 0; Inner < Target->Providers[Index].BcmCount; Inner++) {
      if (Target->ProviderBcms[Target->Providers[Index].BcmOffset + Inner] >=
          Target->BcmCount) {
        return CR_INVALID_PARAMETER;
      }
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
InitializeBcms(IN OUT InterconnectDeviceContext *Context)
{
  UINT8     Aux[INTERCONNECT_BCM_AUX_SIZE];
  UINT32    AuxLength;
  UINT16    Index;
  CR_STATUS Status;

  for (Index = 0; Index < Context->Target->BcmCount; Index++) {
    InterconnectBcmRuntime *Runtime = &Context->Bcm[Index];
    CONST InterconnectTargetBcm *Target = &Context->Target->Bcms[Index];

    Status = Context->Io.GetAddress(
        Context->Io.CmdDbContext, Target->Name, &Runtime->Address);
    if (CR_ERROR(Status) || Runtime->Address == 0) {
      log_err(
          "Interconnect: CmdDB address missing for " CR_LOG_CHAR8_STR_FMT,
          Target->Name);
      return CR_NOT_FOUND;
    }

    AuxLength = sizeof(Aux);
    Status = Context->Io.GetAuxData(
        Context->Io.CmdDbContext, Target->Name, Aux, &AuxLength);
    if (CR_ERROR(Status) || AuxLength < sizeof(Aux)) {
      log_err(
          "Interconnect: CmdDB aux data missing for " CR_LOG_CHAR8_STR_FMT,
          Target->Name);
      return CR_NOT_FOUND;
    }

    Runtime->Unit = ReadLe32(Aux);
    Runtime->Width = ReadLe16(&Aux[4]);
    Runtime->Vcd = Aux[6];
    if (Runtime->Unit == 0 || Runtime->Width == 0) {
      log_err(
          "Interconnect: invalid CmdDB aux data for " CR_LOG_CHAR8_STR_FMT,
          Target->Name);
      return CR_DEVICE_ERROR;
    }

    Runtime->Programmed = !Target->KeepAlive;
    Runtime->Dirty = Target->KeepAlive;
  }
  return CR_SUCCESS;
}

STATIC UINT64
CalculateBcmVote(
    IN InterconnectDeviceContext *Context, IN UINT16 BcmIndex,
    IN BOOLEAN Peak)
{
  CONST InterconnectTargetBcm *Bcm = &Context->Target->Bcms[BcmIndex];
  InterconnectBcmRuntime *Runtime = &Context->Bcm[BcmIndex];
  UINT64 Vote = 0;
  UINT16 Index;

  for (Index = 0; Index < Bcm->NodeCount; Index++) {
    UINT16 NodeIndex = Context->Target->BcmNodes[Bcm->NodeOffset + Index];
    CONST InterconnectTargetNode *Node;
    UINT64 Bandwidth;
    UINT64 Denominator;
    UINT64 Candidate;

    if (NodeIndex >= Context->Target->NodeCount) {
      continue;
    }
    Node = &Context->Target->Nodes[NodeIndex];
    Bandwidth = Peak ? Context->NodePeak[NodeIndex]
                     : Context->NodeAverage[NodeIndex];
    Denominator = Node->BusWidth;
    if (!Peak) {
      Denominator = SaturatingMultiply64(Denominator, Node->Channels);
    }
    Candidate = DivideRoundUp64(
        SaturatingMultiply64(Bandwidth, Runtime->Width), Denominator);
    if (Candidate > Vote) {
      Vote = Candidate;
    }
  }

  return DivideRoundUp64(
      SaturatingMultiply64(Vote, INTERCONNECT_VOTE_SCALE), Runtime->Unit);
}

STATIC VOID
RecalculateVotes(IN OUT InterconnectDeviceContext *Context)
{
  UINT16 Index;
  UINT16 PathIndex;

  for (Index = 0; Index < Context->Target->NodeCount; Index++) {
    Context->NodeAverage[Index] = 0;
    Context->NodePeak[Index] = 0;
  }

  for (PathIndex = 0; PathIndex < INTERCONNECT_MAX_PATHS; PathIndex++) {
    InterconnectPathRuntime *Path = &Context->Paths[PathIndex];
    if (!Path->InUse) {
      continue;
    }
    for (Index = 0; Index < Context->Target->NodeCount; Index++) {
      if ((Path->NodeMask[Index / 64] & BIT(Index % 64)) == 0) {
        continue;
      }
      Context->NodeAverage[Index] = SaturatingAdd64(
          Context->NodeAverage[Index], Path->AverageBandwidth);
      if (Path->PeakBandwidth > Context->NodePeak[Index]) {
        Context->NodePeak[Index] = Path->PeakBandwidth;
      }
    }
  }

  for (Index = 0; Index < Context->Target->BcmCount; Index++) {
    CONST InterconnectTargetBcm *Target = &Context->Target->Bcms[Index];
    InterconnectBcmRuntime *Runtime = &Context->Bcm[Index];
    UINT64 VoteX;
    UINT64 VoteY;

    if (Target->EnableMask != 0) {
      UINT16 NodeIndex;
      BOOLEAN Active = FALSE;
      for (NodeIndex = 0; NodeIndex < Target->NodeCount; NodeIndex++) {
        UINT16 TargetNode = Context->Target->BcmNodes[
            Target->NodeOffset + NodeIndex];
        if (TargetNode < Context->Target->NodeCount &&
            (Context->NodeAverage[TargetNode] != 0 ||
             Context->NodePeak[TargetNode] != 0)) {
          Active = TRUE;
          break;
        }
      }
      VoteX = 0;
      VoteY = Active ? Target->EnableMask : 0;
      if (Target->KeepAlive) {
        VoteX = Target->EnableMask;
        VoteY = Target->EnableMask;
      }
    } else {
      VoteX = CalculateBcmVote(Context, Index, FALSE);
      VoteY = CalculateBcmVote(Context, Index, TRUE);
      if (Target->KeepAlive && VoteX == 0 && VoteY == 0) {
        VoteX = 1;
        VoteY = 1;
      }
    }

    Runtime->VoteX = VoteX;
    Runtime->VoteY = VoteY;
    Runtime->Dirty = !Runtime->Programmed ||
                     Runtime->ProgrammedX != VoteX ||
                     Runtime->ProgrammedY != VoteY;
  }
}

STATIC UINT32
EncodeBcmCommand(IN UINT64 VoteX, IN UINT64 VoteY, IN BOOLEAN Commit)
{
  UINT32 Data = Commit ? (UINT32)INTERCONNECT_BCM_COMMIT_BIT : 0;

  if (VoteX != 0 || VoteY != 0) {
    Data |= (UINT32)INTERCONNECT_BCM_VALID_BIT;
  }
  if (VoteX > INTERCONNECT_BCM_VOTE_MASK) {
    VoteX = INTERCONNECT_BCM_VOTE_MASK;
  }
  if (VoteY > INTERCONNECT_BCM_VOTE_MASK) {
    VoteY = INTERCONNECT_BCM_VOTE_MASK;
  }
  Data |= (UINT32)(VoteX << INTERCONNECT_BCM_VOTE_X_SHIFT);
  Data |= (UINT32)VoteY;
  return Data;
}

STATIC VOID
SortBcmsByVcd(
    IN InterconnectDeviceContext *Context, IN OUT UINT16 *Indices,
    IN UINT16 Count)
{
  UINT16 Index;

  for (Index = 1; Index < Count; Index++) {
    UINT16 Current = Indices[Index];
    UINT16 Position = Index;
    while (Position > 0 &&
           Context->Bcm[Indices[Position - 1]].Vcd >
               Context->Bcm[Current].Vcd) {
      Indices[Position] = Indices[Position - 1];
      Position--;
    }
    Indices[Position] = Current;
  }
}

STATIC CR_STATUS
WriteBcmBatch(
    IN OUT InterconnectDeviceContext *Context, IN CONST UINT16 *Indices,
    IN UINT16 Count)
{
  RpmhTcsCmd Commands[INTERCONNECT_MAX_RPMH_COMMANDS];
  UINT16 Index;
  CR_STATUS Status;

  for (Index = 0; Index < Count; Index++) {
    UINT16 BcmIndex = Indices[Index];
    InterconnectBcmRuntime *Runtime = &Context->Bcm[BcmIndex];
    BOOLEAN Commit = Index + 1 == Count ||
                     Runtime->Vcd != Context->Bcm[Indices[Index + 1]].Vcd;

    Commands[Index].addr = Runtime->Address;
    Commands[Index].data = EncodeBcmCommand(
        Runtime->VoteX, Runtime->VoteY, Commit);
    Commands[Index].wait = Commit;
    Commands[Index].response_required = Commit;
  }

  Status = Context->Io.WriteRpmh(
      Context->Io.RpmhContext, Commands, Count);
  if (CR_ERROR(Status)) {
    return Status;
  }
  for (Index = 0; Index < Count; Index++) {
    InterconnectBcmRuntime *Runtime = &Context->Bcm[Indices[Index]];
    Runtime->ProgrammedX = Runtime->VoteX;
    Runtime->ProgrammedY = Runtime->VoteY;
    Runtime->Programmed = TRUE;
    Runtime->Dirty = FALSE;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
CommitVotes(IN OUT InterconnectDeviceContext *Context)
{
  UINT16 Dirty[INTERCONNECT_MAX_BCMS];
  UINT16 DirtyCount = 0;
  UINT16 Index;
  UINT16 BatchStart;
  UINT16 Maximum;

  for (Index = 0; Index < Context->Target->BcmCount; Index++) {
    if (Context->Bcm[Index].Dirty) {
      Dirty[DirtyCount++] = Index;
    }
  }
  if (DirtyCount == 0) {
    return CR_SUCCESS;
  }
  SortBcmsByVcd(Context, Dirty, DirtyCount);

  Maximum = Context->Io.MaxRpmhCommands;
  if (Maximum == 0 || Maximum > INTERCONNECT_MAX_RPMH_COMMANDS) {
    Maximum = INTERCONNECT_MAX_RPMH_COMMANDS;
  }

  BatchStart = 0;
  while (BatchStart < DirtyCount) {
    UINT16 BatchEnd = BatchStart;
    UINT16 GroupEnd;

    while (BatchEnd < DirtyCount) {
      GroupEnd = (UINT16)(BatchEnd + 1);
      while (GroupEnd < DirtyCount &&
             Context->Bcm[Dirty[GroupEnd]].Vcd ==
                 Context->Bcm[Dirty[BatchEnd]].Vcd) {
        GroupEnd++;
      }
      if (GroupEnd - BatchStart > Maximum) {
        if (BatchEnd == BatchStart) {
          return CR_OUT_OF_RESOURCES;
        }
        break;
      }
      BatchEnd = GroupEnd;
    }

    {
      CR_STATUS Status = WriteBcmBatch(
          Context, &Dirty[BatchStart], (UINT16)(BatchEnd - BatchStart));
      if (CR_ERROR(Status)) {
        return Status;
      }
    }
    BatchStart = BatchEnd;
  }
  return CR_SUCCESS;
}

STATIC VOID
BestEffortClearVotes(IN OUT InterconnectDeviceContext *Context)
{
  UINT16 Index;
  CR_STATUS Status;

  /* Initialization may have committed only an earlier VCD batch.  Force a
     zero vote for every initialized BCM before the context is discarded. */
  for (Index = 0; Index < Context->Target->BcmCount; Index++) {
    Context->Bcm[Index].VoteX = 0;
    Context->Bcm[Index].VoteY = 0;
    Context->Bcm[Index].Dirty = TRUE;
  }
  Status = CommitVotes(Context);
  if (CR_ERROR(Status)) {
    log_err("Interconnect: BCM vote rollback failed, Status=0x%X", Status);
  }
}

STATIC CR_STATUS
FindProvider(
    IN InterconnectDeviceContext *Context, IN CONST CHAR8 *Compatible,
    OUT UINT16 *ProviderIndex)
{
  UINT16 Index;

  if (Compatible == NULL || ProviderIndex == NULL) {
    return CR_INVALID_PARAMETER;
  }
  for (Index = 0; Index < Context->Target->ProviderCount; Index++) {
    if (cr_strcmp(Context->Target->Providers[Index].Compatible, Compatible) == 0) {
      *ProviderIndex = Index;
      return CR_SUCCESS;
    }
  }
  return CR_NOT_FOUND;
}

STATIC CR_STATUS
FindEndpointNode(
    IN InterconnectDeviceContext *Context, IN UINT16 ProviderIndex,
    IN UINT32 EndpointId, OUT UINT16 *NodeIndex)
{
  CONST InterconnectTargetProvider *Provider;
  UINT16 Index;

  Provider = &Context->Target->Providers[ProviderIndex];
  for (Index = 0; Index < Provider->EndpointCount; Index++) {
    CONST InterconnectTargetEndpoint *Endpoint =
        &Context->Target->Endpoints[Provider->EndpointOffset + Index];
    if (Endpoint->Id == EndpointId &&
        Endpoint->NodeIndex < Context->Target->NodeCount) {
      *NodeIndex = Endpoint->NodeIndex;
      return CR_SUCCESS;
    }
  }
  return CR_NOT_FOUND;
}

STATIC CR_STATUS
BuildPath(
    IN InterconnectDeviceContext *Context, IN UINT16 Source,
    IN UINT16 Destination, OUT UINT64 *NodeMask)
{
  UINT16 Queue[INTERCONNECT_MAX_NODES];
  UINT16 Previous[INTERCONNECT_MAX_NODES];
  UINT16 Head = 0;
  UINT16 Tail = 0;
  UINT16 Index;

  for (Index = 0; Index < Context->Target->NodeCount; Index++) {
    Previous[Index] = INTERCONNECT_INVALID_NODE;
  }
  Previous[Source] = Source;
  Queue[Tail++] = Source;

  while (Head < Tail && Previous[Destination] == INTERCONNECT_INVALID_NODE) {
    UINT16 Current = Queue[Head++];
    CONST InterconnectTargetNode *Node = &Context->Target->Nodes[Current];
    UINT16 Link;

    for (Link = 0; Link < Node->LinkCount; Link++) {
      UINT16 Next = Context->Target->Links[Node->LinkOffset + Link];
      if (Next >= Context->Target->NodeCount ||
          Previous[Next] != INTERCONNECT_INVALID_NODE) {
        continue;
      }
      Previous[Next] = Current;
      Queue[Tail++] = Next;
      if (Next == Destination) {
        break;
      }
    }
  }
  if (Previous[Destination] == INTERCONNECT_INVALID_NODE) {
    return CR_NOT_FOUND;
  }

  Index = Destination;
  while (Index != Source) {
    NodeMask[Index / 64] |= BIT(Index % 64);
    Index = Previous[Index];
  }
  NodeMask[Source / 64] |= BIT(Source % 64);
  return CR_SUCCESS;
}

CR_STATUS
InterconnectLibInit(
    IN OUT InterconnectDeviceContext **Context,
    IN CONST InterconnectIoOps *Io)
{
  CR_STATUS Status;

  if (Context == NULL || Io == NULL || Io->GetAddress == NULL ||
      Io->GetAuxData == NULL || Io->WriteRpmh == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (*Context == NULL) {
    *Context = &mInterconnectContext;
  }
  if ((*Context)->DeinitPending) {
    return CR_BUSY;
  }
  if ((*Context)->Initialized) {
    return CR_SUCCESS;
  }

  cr_memset(*Context, 0, sizeof(**Context));
  (*Context)->Target = CrTargetGetInterconnectContext();
  (*Context)->Io = *Io;
  Status = ValidateTarget((*Context)->Target);
  if (CR_ERROR(Status)) {
    cr_memset(*Context, 0, sizeof(**Context));
    return Status;
  }
  CrLockInit(&(*Context)->Lock);
  Status = InitializeBcms(*Context);
  if (CR_ERROR(Status)) {
    cr_memset(*Context, 0, sizeof(**Context));
    return Status;
  }

  RecalculateVotes(*Context);
  Status = CommitVotes(*Context);
  if (CR_ERROR(Status)) {
    BestEffortClearVotes(*Context);
    cr_memset(*Context, 0, sizeof(**Context));
    return Status;
  }
  (*Context)->Initialized = TRUE;
  return CR_SUCCESS;
}

CR_STATUS
InterconnectLibDeinit(IN OUT InterconnectDeviceContext *Context)
{
  UINT16 Index;
  CR_STATUS Status;

  if (Context == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (!Context->Initialized && !Context->DeinitPending) {
    return CR_SUCCESS;
  }

  CrLockAcquire(&Context->Lock);
  if (!Context->Initialized && !Context->DeinitPending) {
    CrLockRelease(&Context->Lock);
    return CR_SUCCESS;
  }

  /* Keepalive votes must be removed during teardown as well. */
  cr_memset(Context->Paths, 0, sizeof(Context->Paths));
  for (Index = 0; Index < Context->Target->BcmCount; Index++) {
    Context->Bcm[Index].VoteX = 0;
    Context->Bcm[Index].VoteY = 0;
    Context->Bcm[Index].Dirty =
        !Context->Bcm[Index].Programmed ||
        Context->Bcm[Index].ProgrammedX != 0 ||
        Context->Bcm[Index].ProgrammedY != 0;
  }
  Context->DeinitPending = TRUE;
  Status = CommitVotes(Context);
  if (!CR_ERROR(Status)) {
    Context->Initialized = FALSE;
    Context->DeinitPending = FALSE;
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
InterconnectAcquirePath(
    IN InterconnectDeviceContext *Context, IN CONST CHAR8 *ProviderCompatible,
    IN UINT32 SourceId, IN UINT32 DestinationId,
    OUT INTERCONNECT_PATH_HANDLE *Path)
{
  UINT16 ProviderIndex;
  UINT16 Source;
  UINT16 Destination;
  UINT16 Slot;
  CR_STATUS Status;

  if (Context == NULL || !Context->Initialized || Context->DeinitPending ||
      Path == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = FindProvider(Context, ProviderCompatible, &ProviderIndex);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = FindEndpointNode(Context, ProviderIndex, SourceId, &Source);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = FindEndpointNode(Context, ProviderIndex, DestinationId, &Destination);
  if (CR_ERROR(Status)) {
    return Status;
  }

  CrLockAcquire(&Context->Lock);
  if (!Context->Initialized || Context->DeinitPending) {
    CrLockRelease(&Context->Lock);
    return CR_ABORT;
  }
  for (Slot = 0; Slot < INTERCONNECT_MAX_PATHS; Slot++) {
    if (!Context->Paths[Slot].InUse) {
      break;
    }
  }
  if (Slot == INTERCONNECT_MAX_PATHS) {
    CrLockRelease(&Context->Lock);
    return CR_OUT_OF_RESOURCES;
  }

  cr_memset(&Context->Paths[Slot], 0, sizeof(Context->Paths[Slot]));
  Status = BuildPath(
      Context, Source, Destination, Context->Paths[Slot].NodeMask);
  if (!CR_ERROR(Status)) {
    Context->Paths[Slot].ProviderIndex = ProviderIndex;
    Context->Paths[Slot].SourceId = SourceId;
    Context->Paths[Slot].DestinationId = DestinationId;
    Context->Paths[Slot].InUse = TRUE;
    *Path = (INTERCONNECT_PATH_HANDLE)(Slot + 1);
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
InterconnectSetBandwidth(
    IN InterconnectDeviceContext *Context,
    IN INTERCONNECT_PATH_HANDLE Path, IN UINT64 AverageBandwidth,
    IN UINT64 PeakBandwidth)
{
  InterconnectPathRuntime *Runtime;
  UINT64 PreviousAverage;
  UINT64 PreviousPeak;
  CR_STATUS Status;

  if (Context == NULL || !Context->Initialized || Context->DeinitPending ||
      Path == INTERCONNECT_INVALID_PATH || Path > INTERCONNECT_MAX_PATHS ||
      PeakBandwidth < AverageBandwidth) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Context->Lock);
  if (!Context->Initialized || Context->DeinitPending) {
    CrLockRelease(&Context->Lock);
    return CR_ABORT;
  }
  Runtime = &Context->Paths[Path - 1];
  if (!Runtime->InUse) {
    CrLockRelease(&Context->Lock);
    return CR_NOT_FOUND;
  }
  PreviousAverage = Runtime->AverageBandwidth;
  PreviousPeak = Runtime->PeakBandwidth;
  Runtime->AverageBandwidth = AverageBandwidth;
  Runtime->PeakBandwidth = PeakBandwidth;
  RecalculateVotes(Context);
  Status = CommitVotes(Context);
  if (CR_ERROR(Status)) {
    /* Restore the caller-visible vote while retaining dirty state for retry. */
    Runtime->AverageBandwidth = PreviousAverage;
    Runtime->PeakBandwidth = PreviousPeak;
    RecalculateVotes(Context);
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
InterconnectReleasePath(
    IN InterconnectDeviceContext *Context,
    IN INTERCONNECT_PATH_HANDLE Path)
{
  InterconnectPathRuntime *Runtime;
  CR_STATUS Status;

  if (Context == NULL || !Context->Initialized || Context->DeinitPending ||
      Path == INTERCONNECT_INVALID_PATH || Path > INTERCONNECT_MAX_PATHS) {
    return CR_INVALID_PARAMETER;
  }

  CrLockAcquire(&Context->Lock);
  if (!Context->Initialized || Context->DeinitPending) {
    CrLockRelease(&Context->Lock);
    return CR_ABORT;
  }
  Runtime = &Context->Paths[Path - 1];
  if (!Runtime->InUse) {
    CrLockRelease(&Context->Lock);
    return CR_NOT_FOUND;
  }
  {
    InterconnectPathRuntime Saved = *Runtime;
    cr_memset(Runtime, 0, sizeof(*Runtime));
    RecalculateVotes(Context);
    Status = CommitVotes(Context);
    if (CR_ERROR(Status)) {
      /* A failed release must not invalidate the caller's handle. */
      *Runtime = Saved;
      RecalculateVotes(Context);
    }
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

UINT16
InterconnectGetProviderCount(IN InterconnectDeviceContext *Context)
{
  UINT16 ProviderCount;

  if (Context == NULL || !Context->Initialized || Context->DeinitPending) {
    return 0;
  }
  CrLockAcquire(&Context->Lock);
  ProviderCount = Context->Initialized && !Context->DeinitPending
                      ? Context->Target->ProviderCount
                      : 0;
  CrLockRelease(&Context->Lock);
  return ProviderCount;
}
