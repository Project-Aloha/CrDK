/** @file
 *  Standalone SM8450 RPMh regulator adapter.
 *
 *  Crane owns the resource votes in this driver. CmdDB supplies the VRM
 *  addresses and Crane's RpmhLib submits the active-set commands through the
 *  local RPMh TCS driver. No Qualcomm NPA, ICB, or PMIC protocol is queried.
 *
 *  Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *  Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/rpmh.h>
#include <Library/RpmhRailLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>

#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

#define RPMH_CR_MAX_RAILS  PCIE_MAX_RESOURCES

typedef CR_RPMH_RAIL_VOTE_KIND RPMH_VOTE_KIND;
#define RpmhVoteVoltage CrRpmhRailVoltage
#define RpmhVoteMode CrRpmhRailMode
#define RpmhVoteEnable CrRpmhRailEnable
typedef CR_RPMH_RAIL RPMH_CR_RAIL;

STATIC EFI_CMD_DB_PROTOCOL  *mCmdDb;
STATIC RpmhDeviceContext    *mRpmh;
STATIC RPMH_CR_RAIL          mRails[RPMH_CR_MAX_RAILS];
STATIC UINTN                 mRailCount;
STATIC CR_RPMH_RAIL_RETAINED   mRetainedRails[RPMH_CR_MAX_RAILS];
STATIC EFI_LOCK              mRailLock;
STATIC EFI_LOCK              mArcLock;
STATIC CR_RPMH_ARC_RESOURCE  mArcResources[CR_RPMH_ARC_MAX_RESOURCES];
STATIC CR_RPMH_ARC_ENTRY     mArcEntries[128];
STATIC CR_RPMH_ARC_VOTER     mArcVoter;
STATIC UINT32                mArcFloorIdentity, mArcCount, mArcAcknowledged;
STATIC BOOLEAN               mArcFloorReady, mArcFloorAttempted, mArcSealed;
STATIC CONST CHAR8 *CONST mArcNames[EFI_RPMH_CR_ARC_RESOURCE_COUNT] = {
  "cx.lvl", "mx.lvl", "lcx.lvl", "lmx.lvl", "mmcx.lvl", "xo.lvl"
};

STATIC CONST CR_DAL_RPMH_ARC_ENTRY mDefaultArcPolicy[] = {
  {"cx.lvl",CR_DAL_RPMH_ARC_POWER,0,CR_DAL_RPMH_ARC_NO_PARENT},
  {"mx.lvl",CR_DAL_RPMH_ARC_POWER,0,CR_DAL_RPMH_ARC_NO_PARENT},
  {"lcx.lvl",CR_DAL_RPMH_ARC_POWER,0,CR_DAL_RPMH_ARC_NO_PARENT},
  {"lmx.lvl",CR_DAL_RPMH_ARC_POWER,0,CR_DAL_RPMH_ARC_NO_PARENT},
  {"mmcx.lvl",CR_DAL_RPMH_ARC_POWER,0,CR_DAL_RPMH_ARC_NO_PARENT},
  {"xo.lvl",CR_DAL_RPMH_ARC_XO,0,CR_DAL_RPMH_ARC_NO_PARENT}
};
STATIC UINT32 mArcFloors[CR_RPMH_ARC_MAX_RESOURCES];

STATIC
BOOLEAN
AsciiStringsEqual (
  IN CONST CHAR8  *Left,
  IN CONST CHAR8  *Right
  )
{
  if ((Left == NULL) || (Right == NULL)) {
    return FALSE;
  }

  while ((*Left != '\0') && (*Left == *Right)) {
    Left++;
    Right++;
  }

  return *Left == *Right;
}

STATIC
RPMH_CR_RAIL *
FindRail (
  IN CONST CHAR8  *Name
  )
{
  UINTN  Index;

  for (Index = 0; Index < mRailCount; Index++) {
    if (AsciiStringsEqual (mRails[Index].Name, Name)) {
      return &mRails[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
ValidateCmdDbProtocol (
  IN CONST EFI_CMD_DB_PROTOCOL  *Protocol
  )
{
  if ((Protocol == NULL) ||
      (Protocol->Revision < EFI_CMD_DB_PROTOCOL_REVISION) ||
      (Protocol->GetEntryAddressByName == NULL)) {
    return EFI_INCOMPATIBLE_VERSION;
  }

  return EFI_SUCCESS;
}

STATIC CR_STATUS ArcWrite (VOID *Context, UINT32 Address, UINT32 Corner)
{
  RpmhTcsCmd Command = { Address, Corner, 1, 1 };
  (VOID)Context;
  return RpmhWrite (mRpmh, &Command, 1);
}

STATIC EFI_STATUS InitializeArcDescriptions (VOID)
{
  UINT32 Index, Address, Length, Missing = 0;
  UINT8 Aux[64];
  EFI_STATUS Status;
  CR_RPMH_ARC_IO Io = { NULL, ArcWrite };
  CONST CR_DAL_DEVICE_INFO *Info = NULL;
  CONST CR_DAL_RPMH_ARC_ENTRY *Policy = mDefaultArcPolicy;
  UINT32 Count = ARRAY_SIZE (mDefaultArcPolicy);
  BOOLEAN Explicit = FALSE;
  if (mArcVoter.Initialized || mArcAcknowledged != 0) return EFI_ALREADY_STARTED;
  mArcCount = 0;
  Status = CrDalGetDeviceInfo (CrDalDeviceRpmhArcInventory, 0,
      CR_DAL_DATA_REVISION_1, sizeof (CR_DAL_RPMH_ARC_INVENTORY), &Info);
  if (Status == EFI_SUCCESS) {
    CONST CR_DAL_RPMH_ARC_INVENTORY *Inventory;
    if (Info == NULL || Info->Data == NULL ||
        Info->DataRevision != CR_DAL_DATA_REVISION_1 ||
        Info->DataSize != sizeof (*Inventory)) return EFI_COMPROMISED_DATA;
    Inventory = Info->Data;
    if (Inventory->Revision != CR_DAL_RPMH_ARC_REVISION ||
        Inventory->Size != sizeof (*Inventory) || Inventory->Count == 0 ||
        Inventory->Count > CR_RPMH_ARC_MAX_RESOURCES || Inventory->Entries == NULL)
      return EFI_COMPROMISED_DATA;
    Policy = Inventory->Entries; Count = Inventory->Count; Explicit = TRUE;
  } else if (Status != EFI_NOT_FOUND) {
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  /* Validate descriptor shape and dependency order before CmdDB lookup.
     CrRpmhArcQualify also validates name characters before hardware writes.
     Forward/self edges are rejected, making the published order acyclic. */
  for (Index = 0; Index < Count; ++Index) {
    UINT32 Length;
    for (Length = 0; Length < sizeof (Policy[Index].Name); ++Length)
      if (Policy[Index].Name[Length] == 0) break;
    if (Length == 0 || Length == sizeof (Policy[Index].Name) ||
        (Policy[Index].Kind != CR_DAL_RPMH_ARC_POWER && Policy[Index].Kind != CR_DAL_RPMH_ARC_XO) ||
        (Policy[Index].ParentIndex != CR_DAL_RPMH_ARC_NO_PARENT && Policy[Index].ParentIndex >= Index) ||
        Policy[Index].MinLevel > MAX_UINT16 ||
        (AsciiStringsEqual (Policy[Index].Name, "xo.lvl") != (Policy[Index].Kind == CR_DAL_RPMH_ARC_XO)) ||
        (Policy[Index].Kind == CR_DAL_RPMH_ARC_XO && (Policy[Index].MinLevel != 0 ||
          Policy[Index].ParentIndex != CR_DAL_RPMH_ARC_NO_PARENT))) return EFI_COMPROMISED_DATA;
    if (Policy[Index].ParentIndex != CR_DAL_RPMH_ARC_NO_PARENT &&
        Policy[Policy[Index].ParentIndex].Kind != CR_DAL_RPMH_ARC_POWER) return EFI_COMPROMISED_DATA;
    for (UINT32 Other = 0; Other < Index; ++Other)
      if (AsciiStringsEqual (Policy[Index].Name, Policy[Other].Name)) return EFI_COMPROMISED_DATA;
  }
  for (Index = 0; Index < Count; ++Index) {
    BOOLEAN Xo = Policy[Index].Kind == CR_DAL_RPMH_ARC_XO;
    Address = 0;
    Status = mCmdDb->GetEntryAddressByName (mCmdDb, Policy[Index].Name, &Address);
    if (Status == EFI_NOT_FOUND) { ++Missing; continue; }
    if (Status != EFI_SUCCESS) return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    if (mCmdDb->GetAuxDataByName == NULL) return EFI_INCOMPATIBLE_VERSION;
    Length = sizeof (Aux);
    Status = mCmdDb->GetAuxDataByName (mCmdDb, Policy[Index].Name, Aux, &Length);
    if (Xo && Status == EFI_NOT_FOUND) Length = 0;
    else if (Status != EFI_SUCCESS) return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    Status = (EFI_STATUS)CrRpmhArcQualify (&mArcResources[Index], Policy[Index].Name, Address,
        Xo ? CrRpmhArcXo : CrRpmhArcPower, Aux, Length);
    if (EFI_ERROR (Status)) return Status;
    mArcFloors[Index] = Xo ? 3 : mArcResources[Index].LevelCount - 1;
    if (!Xo && mArcResources[Index].Levels[mArcFloors[Index]] < Policy[Index].MinLevel)
      return EFI_UNSUPPORTED;
  }
  /* An omitted inventory preserves the rail-only legacy path. A target that
     explicitly requires ARC must not silently succeed when all are absent. */
  if (Missing == Count) return Explicit ? EFI_NOT_FOUND : EFI_SUCCESS;
  if (Missing != 0) return EFI_COMPROMISED_DATA;
  Status = (EFI_STATUS)CrRpmhArcVotesInit (&mArcVoter, &Io, mArcResources,
      Count, mArcEntries, ARRAY_SIZE (mArcEntries));
  if (!EFI_ERROR (Status)) mArcCount = Count;
  return Status;
}

STATIC EFI_STATUS EnsureArcFloor (VOID)
{
  UINT32 Index, Corners[CR_RPMH_ARC_MAX_RESOURCES];
  EFI_STATUS Status;
  if (mArcCount == 0 || mArcCount > CR_RPMH_ARC_MAX_RESOURCES) return EFI_NOT_FOUND;
  if (mArcFloorReady) return EFI_SUCCESS;
  /* Mainline rpmhpd retains the highest valid corner before sync_state. These
     are explicit new acknowledged unsynchronized boot demands, not a readback
     or recovered earlier-owner manifest. Reissue only the same maximum after
     uncertainty; a fresh voter rollback to logical zero would lower unknown
     boot demand. Import only after the complete max floor has been ACKed. */
  for (Index = 0; Index < mArcCount; ++Index) {
    Corners[Index] = mArcFloors[Index];
    if ((mArcAcknowledged & (1U << Index)) != 0) continue;
    mArcFloorAttempted = TRUE;
    Status = (EFI_STATUS)ArcWrite (NULL, mArcResources[Index].Address, Corners[Index]);
    if (Status != EFI_SUCCESS) return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    mArcAcknowledged |= 1U << Index;
  }
  if (mRpmh->TcsBusy != 0 || mRpmh->TcsSynchronous != 0 || mRpmh->TcsRetiring != 0)
    return EFI_NOT_READY;
  Status = (EFI_STATUS)CrRpmhArcImportBaseline (&mArcVoter, &mArcFloorIdentity, Corners, mArcCount);
  if (Status == EFI_SUCCESS) mArcFloorReady = TRUE;
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_NOT_READY;
}

EFI_STATUS EFIAPI ProtocolRpmhEnsureBootFloor (EFI_RPMH_CR_PROTOCOL *This)
{
  if (This == NULL || mRpmh == NULL || !mRpmh->Initialized) return EFI_NOT_READY;
  EfiAcquireLock (&mArcLock);
  EFI_STATUS Status = mArcSealed && !mArcFloorReady ? EFI_ACCESS_DENIED : EnsureArcFloor ();
  if (Status == EFI_SUCCESS) {
    if (mArcAcknowledged != ((1U << mArcCount) - 1) ||
        mRpmh->TcsBusy || mRpmh->TcsSynchronous || mRpmh->TcsRetiring) Status = EFI_NOT_READY;
    for (UINT32 Index = 0; Status == EFI_SUCCESS && Index < mArcCount; ++Index) {
      UINT32 Floor = mArcFloors[Index];
      if (!mArcVoter.States[Index].Known || mArcVoter.States[Index].Pending ||
          mArcVoter.States[Index].Applied != Floor) Status = EFI_NOT_READY;
    }
  }
  EfiReleaseLock (&mArcLock);
  return Status;
}

EFI_STATUS EFIAPI ProtocolRpmhVoteArc (EFI_RPMH_CR_PROTOCOL *This, VOID *Owner, UINT32 Address, UINT32 Corner)
{
  UINT32 Index, Slot;
  EFI_STATUS Status;
  BOOLEAN Absent = TRUE;
  CR_RPMH_ARC_VALUE Value = { Corner, Corner != 0 };
  (VOID)This;
  if (Owner == NULL || Owner == &mArcFloorIdentity || mRpmh == NULL || !mRpmh->Initialized)
    return EFI_INVALID_PARAMETER;
  EfiAcquireLock (&mArcLock);
  if (mArcSealed) { Status = EFI_ACCESS_DENIED; goto Exit; }
  Status = EnsureArcFloor (); if (EFI_ERROR (Status)) goto Exit;
  for (Index = 0; Index < mArcCount; ++Index) if (mArcResources[Index].Address == Address) break;
  if (Index == mArcCount) { Status = EFI_NOT_FOUND; goto Exit; }
  /* Reject invalid corner before committing a reservation. */
  if ((mArcResources[Index].Kind == CrRpmhArcXo && Corner != 0 && Corner != 3) ||
      (mArcResources[Index].Kind == CrRpmhArcPower && Corner >= mArcResources[Index].LevelCount)) {
    Status = EFI_INVALID_PARAMETER; goto Exit;
  }
  Status = (EFI_STATUS)CrRpmhArcReserve (&mArcVoter, Owner, mArcResources[Index].Name);
  if (EFI_ERROR (Status)) goto Exit;
  Status = (EFI_STATUS)CrRpmhArcSet (&mArcVoter, Owner, mArcResources[Index].Name, &Value);
  if (EFI_ERROR (Status) || Corner != 0) goto Exit;
  for (Slot = 0; Slot < mArcVoter.Capacity; ++Slot) if (mArcEntries[Slot].Owner == Owner &&
      (mArcEntries[Slot].Value.Present || mArcEntries[Slot].RecoveryPending)) { Absent = FALSE; break; }
  if (Absent) Status = (EFI_STATUS)CrRpmhArcReleaseOwner (&mArcVoter, Owner);
Exit:
  EfiReleaseLock (&mArcLock); return Status;
}

EFI_STATUS EFIAPI ProtocolRpmhSealArcSnapshot (EFI_RPMH_CR_PROTOCOL *This, EFI_RPMH_CR_ARC_SNAPSHOT *Out)
{
  UINT32 Index, Slot, FloorMask = 0;
  EFI_STATUS Status = EFI_NOT_READY;
  EFI_RPMH_CR_ARC_SNAPSHOT Snapshot = {0};
  (VOID)This;
  if (Out == NULL) return EFI_INVALID_PARAMETER;
  EfiAcquireLock (&mArcLock);
  /* PHOF v1 describes exactly this six-resource order. Additional resources
     cannot be omitted while claiming a complete sealed owner manifest. */
  if (mArcCount != 0) {
    if (mArcCount != EFI_RPMH_CR_ARC_RESOURCE_COUNT) { Status=EFI_UNSUPPORTED; goto Exit; }
    for (Index=0; Index<mArcCount; ++Index)
      if (!AsciiStringsEqual (mArcResources[Index].Name,mArcNames[Index]) ||
          mArcResources[Index].Kind != (Index==5?CrRpmhArcXo:CrRpmhArcPower)) {
        Status=EFI_UNSUPPORTED; goto Exit;
      }
  }
  if (!mArcFloorReady || !mArcVoter.Initialized || mArcVoter.Entries != mArcEntries ||
      mArcVoter.Resources != mArcResources || mArcVoter.Count != EFI_RPMH_CR_ARC_RESOURCE_COUNT ||
      mArcVoter.Capacity != ARRAY_SIZE (mArcEntries) || mArcCount != EFI_RPMH_CR_ARC_RESOURCE_COUNT || mRpmh == NULL || !mRpmh->Initialized ||
      mRpmh->TcsBusy || mRpmh->TcsSynchronous || mRpmh->TcsRetiring) goto Exit;
  for (Slot = 0; Slot < mArcVoter.Capacity; ++Slot) if (mArcEntries[Slot].Owner != NULL) {
    CR_RPMH_ARC_ENTRY *Entry = &mArcEntries[Slot];
    if (Entry->Owner != &mArcFloorIdentity || !Entry->Inherited || Entry->RecoveryPending ||
        !Entry->Value.Present || Entry->Resource >= mArcCount || (FloorMask & (1U << Entry->Resource)) != 0) goto Exit;
    UINT32 Floor = mArcFloors[Entry->Resource];
    if (Entry->Value.Corner != Floor) goto Exit;
    FloorMask |= 1U << Entry->Resource;
  }
  if (FloorMask != (1U << EFI_RPMH_CR_ARC_RESOURCE_COUNT) - 1) goto Exit;
  for (Index = 0; Index < mArcCount; ++Index) {
    UINT32 Floor = mArcFloors[Index];
    if (!mArcVoter.States[Index].Known || mArcVoter.States[Index].Pending || mArcVoter.States[Index].Applied != Floor) goto Exit;
    Snapshot.Resources[Index] = mArcResources[Index]; Snapshot.Corners[Index] = Floor;
  }
  Snapshot.Size = sizeof (Snapshot); Snapshot.Revision = EFI_RPMH_CR_ARC_SNAPSHOT_REVISION;
  Snapshot.Count = mArcCount; Snapshot.OwnerCount = 1;
  Snapshot.Flags = EFI_RPMH_CR_ARC_FLOOR_ACKNOWLEDGED | EFI_RPMH_CR_ARC_CONSUMERS_UNSYNCHRONIZED | EFI_RPMH_CR_ARC_REGISTRY_SEALED;
  mArcSealed = TRUE; *Out = Snapshot; Status = EFI_SUCCESS;
Exit:
  EfiReleaseLock (&mArcLock); return Status;
}

STATIC
EFI_STATUS
InitializeRailDescriptions (
  IN CONST PcieTargetContext  *Target
  )
{
  CONST PcieTargetSupply  *Supply;
  RPMH_CR_RAIL            *Rail;
  UINTN                    Index;

  mRailCount = 0;
  /* PCIe supplies are one optional input. Boards without a PCIe target
     still populate their independent CrDalDeviceRpmhRails inventory. */
  if (mCmdDb == NULL) return EFI_COMPROMISED_DATA;
  if (Target == NULL) return EFI_SUCCESS;
  if ((Target->Supplies == NULL) ||
      (Target->SupplyCount == 0) ||
      (Target->SupplyCount > RPMH_CR_MAX_RAILS)) {
    return EFI_COMPROMISED_DATA;
  }

  for (Index = 0; Index < Target->SupplyCount; Index++) {
    Supply = &Target->Supplies[Index];
    if ((Supply->Controller == NULL) ||
        (!AsciiStringsEqual (Supply->Controller, "rpmh") &&
         !AsciiStringsEqual (Supply->Controller, "rpmh-enable"))) {
      continue;
    }
    if ((Supply->Id == NULL) || (*Supply->Id == '\0')) {
      return EFI_COMPROMISED_DATA;
    }
    if (FindRail (Supply->Id) != NULL) {
      continue;
    }
    if (mRailCount >= RPMH_CR_MAX_RAILS) {
      return EFI_OUT_OF_RESOURCES;
    }

    Rail       = &mRails[mRailCount];
    *Rail      = (RPMH_CR_RAIL){ 0 };
    Rail->Name = Supply->Id;
    /* Each port qualifies its own resources on acquisition. A missing
       optional endpoint rail must not suppress RPMh for UFS or other RCs. */
    DEBUG ((
      DEBUG_INFO,
      "Crane RPMh: rail=%a CmdDB deferred address=0x%08x default=%u mV mode=%u\n",
      Rail->Name,
      Rail->Address,
      Supply->VoltageMv,
      Supply->Mode
      ));
    mRailCount++;
  }

  return (mRailCount == 0) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC
EFI_STATUS
InitializeAdditionalRails (VOID)
{
  CONST CR_DAL_DEVICE_INFO *Info;
  CONST CR_DAL_RPMH_RAILS *Rails;
  EFI_STATUS Status;
  UINTN Index;
  RPMH_CR_RAIL *Rail;
  Status = CrDalGetDeviceInfo (CrDalDeviceRpmhRails, 0, CR_DAL_DATA_REVISION_1,
                               sizeof(*Rails), &Info);
  if (Status == EFI_NOT_FOUND || Status == EFI_UNSUPPORTED) return EFI_SUCCESS;
  if (EFI_ERROR(Status)) return Status;
  if (Info == NULL || Info->Data == NULL) return EFI_COMPROMISED_DATA;
  Rails = Info->Data;
  if (Rails->Names == NULL || Rails->Count > RPMH_CR_MAX_RAILS)
    return EFI_COMPROMISED_DATA;
  for (Index = 0; Index < Rails->Count; ++Index) {
    if (Rails->Names[Index] == NULL || *Rails->Names[Index] == '\0')
      return EFI_COMPROMISED_DATA;
    if (FindRail(Rails->Names[Index]) != NULL) continue;
    if (mRailCount == RPMH_CR_MAX_RAILS) return EFI_OUT_OF_RESOURCES;
    Rail = &mRails[mRailCount];
    *Rail = (RPMH_CR_RAIL){0};
    Rail->Name = Rails->Names[Index];
    /* Resolve optional supplies when used, so an absent USB supply cannot
       suppress the existing PCIe/display RPMh provider at boot. */
    ++mRailCount;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InitializeRailTopology (VOID)
{
  CONST CR_DAL_DEVICE_INFO   *Info;
  CONST CR_DAL_RPMH_TOPOLOGY *Topology;
  CONST CR_DAL_RPMH_PARENT   *Edge;
  RPMH_CR_RAIL              *Child, *Parent, *Walk;
  EFI_STATUS                Status;
  UINTN                     Index, Other, Depth;

  Status = CrDalGetDeviceInfo (CrDalDeviceRpmhTopology, 0,
              CR_DAL_DATA_REVISION_1, sizeof (*Topology), &Info);
  if (Status == EFI_NOT_FOUND || Status == EFI_UNSUPPORTED) return EFI_SUCCESS;
  if (EFI_ERROR (Status)) return Status;
  if (Info == NULL || Info->Data == NULL) return EFI_COMPROMISED_DATA;
  Topology = Info->Data;
  if (Topology->Count > mRailCount ||
      (Topology->Count != 0 && Topology->Parents == NULL))
    return EFI_COMPROMISED_DATA;

  for (Index = 0; Index < Topology->Count; ++Index) {
    Edge = &Topology->Parents[Index];
    Child = FindRail (Edge->Child);
    Parent = FindRail (Edge->Parent);
    if (Child == NULL || Parent == NULL || Child == Parent || Child->Parent != NULL ||
        Edge->VoltageMv == 0 || Edge->VoltageMv > 0xFFFFU ||
        (Edge->Mode != 2 && Edge->Mode != 3 && Edge->Mode != 4 &&
         Edge->Mode != 6 && Edge->Mode != 7))
      return EFI_COMPROMISED_DATA;
    for (Other = 0; Other < Index; ++Other) {
      if (AsciiStringsEqual (Topology->Parents[Other].Parent, Edge->Parent) &&
          (Topology->Parents[Other].VoltageMv != Edge->VoltageMv ||
           Topology->Parents[Other].Mode != Edge->Mode))
        return EFI_COMPROMISED_DATA;
    }
    Child->Parent = Parent;
    Child->ParentMv = Edge->VoltageMv;
    Child->ParentMode = Edge->Mode;
  }
  /* Reject cycles before any votes are issued or recursive leases are used. */
  for (Index = 0; Index < mRailCount; ++Index) {
    Walk = &mRails[Index];
    for (Depth = 0; Walk != NULL && Depth < mRailCount; ++Depth) Walk = Walk->Parent;
    if (Walk != NULL) return EFI_COMPROMISED_DATA;
    if (mRails[Index].Parent != NULL)
      DEBUG ((DEBUG_INFO, "Crane RPMh: input rail=%a parent=%a %u mV mode=%u\n",
        mRails[Index].Name, mRails[Index].Parent->Name,
        mRails[Index].ParentMv, mRails[Index].ParentMode));
  }
  return EFI_SUCCESS;
}

STATIC CR_STATUS
RailAddress (VOID *Context, CONST CHAR8 *Name, UINT32 *Address)
{
  (VOID)Context;
  if (mCmdDb == NULL || mRpmh == NULL || !mRpmh->Initialized) return CR_NOT_READY;
  return (CR_STATUS)mCmdDb->GetEntryAddressByName(mCmdDb, Name, Address);
}

STATIC CR_STATUS
RailWrite (VOID *Context, CONST CR_RPMH_RAIL *Rail,
           CR_RPMH_RAIL_VOTE_KIND Kind, UINT32 Value)
{
  (VOID)Context;
  if (mRpmh == NULL || !mRpmh->Initialized) return CR_NOT_READY;
  CR_STATUS Status;
  switch (Kind) {
    case CrRpmhRailVoltage:
      Status = RpmhSetVregVoltage(mRpmh, Rail->Address, Value);break;
    case CrRpmhRailMode:
      Status = RpmhSetVregMode(mRpmh, Rail->Address, (UINT8)Value);break;
    case CrRpmhRailEnable:
      Status = RpmhEnableVreg(mRpmh, Rail->Address, Value != 0);break;
    default:
      return CR_INVALID_PARAMETER;
  }
  DEBUG ((CR_ERROR(Status) ? DEBUG_ERROR : DEBUG_INFO,
    "Crane RPMh: vote rail=%a address=0x%08x kind=%u value=%u status=%r\n",
    Rail->Name, Rail->Address, (UINT32)Kind, Value, (EFI_STATUS)Status));
  return Status;
}

STATIC CONST CR_RPMH_RAIL_IO mRailIo = {NULL, RailAddress, RailWrite};

STATIC EFI_STATUS
IssueVote (RPMH_CR_RAIL *Rail, RPMH_VOTE_KIND Kind, UINT32 Value, BOOLEAN *Requested)
{
  if (Requested == NULL || mRpmh == NULL || !mRpmh->Initialized) return EFI_NOT_READY;
  return (EFI_STATUS)CrRpmhRailVote(&mRailIo, Rail, Kind, Value);
}

/* Count the provider's baseline roots and each distinct owned parent edge
 * reachable from them. Shared ancestors are visited once, so a diamond does
 * not invent extra references. Ordinary clients may release only above this
 * immutable floor, including while a baseline is partially acknowledged. */
STATIC UINTN RetainedFloor(RPMH_CR_RAIL *Rail)
{
  BOOLEAN Reachable[RPMH_CR_MAX_RAILS]={0};
  UINTN Floor=0;
  for (UINTN I=0; I<mRailCount; ++I) {
    if (!mRetainedRails[I].Owned) continue;
    Reachable[I]=TRUE;
    if (mRetainedRails[I].Rail==Rail) ++Floor;
  }
  for (UINTN Pass=0; Pass<mRailCount; ++Pass)
    for (UINTN I=0; I<mRailCount; ++I) {
      if (!Reachable[I] || !mRails[I].ParentOwned) continue;
      for (UINTN J=0; J<mRailCount; ++J)
        if (mRails[I].Parent==&mRails[J]) Reachable[J]=TRUE;
    }
  for (UINTN I=0; I<mRailCount; ++I)
    if (Reachable[I] && mRails[I].ParentOwned && mRails[I].Parent==Rail) ++Floor;
  return Floor;
}
STATIC EFI_STATUS
ReleaseRail (RPMH_CR_RAIL *Rail)
{
  UINTN Floor=RetainedFloor(Rail);
  if (Floor && Rail->ReferenceCount<=Floor) return EFI_ACCESS_DENIED;
  return (EFI_STATUS)CrRpmhRailRelease(&mRailIo, Rail);
}
STATIC EFI_STATUS EFIAPI
ProtocolRpmhEnsureRetainedVreg(EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name,
                              UINT32 VoltageMv, UINT8 Mode)
{
  EFI_STATUS Status;
  if (!This || !Name) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mRailLock);
  RPMH_CR_RAIL *Rail=FindRail(Name);
  if (!Rail) Status=EFI_NOT_FOUND;
  else Status=(EFI_STATUS)CrRpmhRailRetain(&mRailIo,Rail,VoltageMv,Mode,
                                         &mRetainedRails[Rail-mRails]);
  EfiReleaseLock(&mRailLock);
  return Status;
}

STATIC EFI_STATUS
AcquireRail (RPMH_CR_RAIL *Rail, UINT32 VoltageMv, UINT8 Mode, BOOLEAN *Owned)
{
  return (EFI_STATUS)CrRpmhRailAcquire(&mRailIo, Rail, VoltageMv, Mode, Owned);
}

STATIC EFI_STATUS EFIAPI
ProtocolRpmhEnsureRetainedVregEnabled(EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name)
{
  EFI_STATUS Status;
  if (!This || !Name) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mRailLock);
  RPMH_CR_RAIL *Rail=FindRail(Name);
  if (!Rail) Status=EFI_NOT_FOUND;
  else Status=(EFI_STATUS)CrRpmhRailRetainEnable(&mRailIo,Rail,
                                               &mRetainedRails[Rail-mRails]);
  EfiReleaseLock(&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhWrite (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN RpmhTcsCmd            *TcsCmd,
  IN UINT32                 NumCmds
  )
{
  EFI_STATUS Status;
  UINT32 Command, Index;
  (VOID)This;
  if ((mRpmh == NULL) || !mRpmh->Initialized ||
      ((NumCmds != 0) && (TcsCmd == NULL))) {
    return EFI_NOT_READY;
  }
  EfiAcquireLock (&mArcLock);
  /* Validate the complete batch before submission. Known managed ARC words
     cannot bypass owner aggregation, including a misaligned alias. */
  for (Command = 0; Command < NumCmds; ++Command)
    for (Index = 0; Index < mArcCount; ++Index)
      if ((TcsCmd[Command].addr & ~3U) == mArcResources[Index].Address) {
        EfiReleaseLock (&mArcLock); return EFI_ACCESS_DENIED;
      }
  EfiAcquireLock (&mRailLock);
  for (Command = 0; Command < NumCmds; ++Command)
    for (Index = 0; Index < mRailCount; ++Index) {
      UINT32 Address=TcsCmd[Command].addr & ~3U;
      if (mRails[Index].Address && Address>=mRails[Index].Address &&
          Address-mRails[Index].Address<=8 && RetainedFloor(&mRails[Index])) {
        EfiReleaseLock (&mRailLock);
        EfiReleaseLock (&mArcLock); return EFI_ACCESS_DENIED;
      }
    }
  Status = (EFI_STATUS)RpmhWrite (mRpmh, TcsCmd, NumCmds);
  EfiReleaseLock (&mRailLock);
  EfiReleaseLock (&mArcLock); return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhEnableVreg (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN BOOLEAN                Enable
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if (Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }
  if (!Enable) {
    Status = ReleaseRail (Rail);
    goto Exit;
  }
  if (Rail->ReferenceCount == MAX_UINTN) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Exit;
  }
  if (Rail->ReferenceCount != 0) {
    if (!Rail->Ready) {
      Status = EFI_NOT_READY;
      goto Exit;
    }
    Rail->ReferenceCount++;
    Status = EFI_SUCCESS;
    goto Exit;
  }

  if (Rail->ParentOwned) {
    Status = EFI_NOT_READY;
    goto Exit;
  }
  Rail->ReferenceCount = 1;
  if (Rail->Parent != NULL) {
    Status = AcquireRail (Rail->Parent, Rail->ParentMv, Rail->ParentMode,
                          &Rail->ParentOwned);
    if (EFI_ERROR (Status)) goto Exit;
  }
  Status = IssueVote (Rail, RpmhVoteEnable, 1, &Rail->EnableRequested);
  if (!EFI_ERROR (Status)) Rail->Ready = TRUE;

Exit:
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregVoltage (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN CONST UINT32           VoltageMv
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if ((Name == NULL) || (VoltageMv == 0) || (VoltageMv > 0xFFFFU)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }
  if (Rail->ReferenceCount != 0) {
    Status = (Rail->VoltageMv == VoltageMv) ? EFI_SUCCESS : EFI_ACCESS_DENIED;
    goto Exit;
  }
  Status = IssueVote (Rail, RpmhVoteVoltage, VoltageMv,
                      &Rail->VoltageRequested);
  if (!EFI_ERROR (Status)) {
    Rail->VoltageMv = VoltageMv;
  }

Exit:
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregMode (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8           *Name,
  IN CONST UINT8            Mode
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if ((Name == NULL) || ((Mode != 2) && (Mode != 3) && (Mode != 4) &&
                         (Mode != 6) && (Mode != 7))) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }
  if (Rail->ReferenceCount != 0) {
    Status = (Rail->Mode == Mode) ? EFI_SUCCESS : EFI_ACCESS_DENIED;
    goto Exit;
  }
  Status = IssueVote (Rail, RpmhVoteMode, Mode, &Rail->ModeRequested);
  if (!EFI_ERROR (Status)) {
    Rail->Mode = Mode;
  }

Exit:
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_STATUS
EFIAPI
ProtocolRpmhAcquireVreg (
  IN EFI_RPMH_CR_PROTOCOL  *This,
  IN CONST CHAR8            *Name,
  IN UINT32                  VoltageMv,
  IN UINT8                   Mode,
  OUT BOOLEAN               *Owned
  )
{
  RPMH_CR_RAIL  *Rail;
  EFI_STATUS     Status;

  (VOID)This;
  if (Owned == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Owned = FALSE;
  if ((Name == NULL) || (VoltageMv == 0) || (VoltageMv > 0xFFFFU) ||
      ((Mode != 2) && (Mode != 3) && (Mode != 4) && (Mode != 6) &&
       (Mode != 7))) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mRailLock);
  Rail = FindRail (Name);
  if (Rail == NULL) {
    Status = EFI_NOT_FOUND;
    goto Exit;
  }
  Status = AcquireRail (Rail, VoltageMv, Mode, Owned);

Exit:
  EfiReleaseLock (&mRailLock);
  return Status;
}

EFI_RPMH_CR_PROTOCOL  gRpmhCrProtocol = {
  .Revision           = EFI_RPMH_CR_PROTOCOL_REVISION,
  .RpmhWrite          = ProtocolRpmhWrite,
  .RpmhEnableVreg     = ProtocolRpmhEnableVreg,
  .RpmhSetVregVoltage = ProtocolRpmhSetVregVoltage,
  .RpmhSetVregMode    = ProtocolRpmhSetVregMode,
  .AcquireVreg        = ProtocolRpmhAcquireVreg,
  .VoteArc            = ProtocolRpmhVoteArc,
  .SealArcSnapshot    = ProtocolRpmhSealArcSnapshot,
  .EnsureBootFloor    = ProtocolRpmhEnsureBootFloor,
  .EnsureRetainedVreg = ProtocolRpmhEnsureRetainedVreg,
  .EnsureRetainedVregEnabled = ProtocolRpmhEnsureRetainedVregEnabled,
};

EFI_STATUS
EFIAPI
RpmhEntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CONST PcieTargetContext  *Target;
  EFI_STATUS                Status;

  (VOID)SystemTable;
  /* Never reset graph storage backing an acknowledged or pending baseline. */
  for (UINTN I=0; I<ARRAY_SIZE(mRetainedRails); ++I)
    if (mRetainedRails[I].Rail) return EFI_ALREADY_STARTED;
  if ((gBS == NULL) || (gBS->LocateProtocol == NULL) ||
      (gBS->InstallMultipleProtocolInterfaces == NULL)) {
    return EFI_NOT_READY;
  }

  mCmdDb = NULL;
  Status = gBS->LocateProtocol (
             &gEfiCmdDBCrProtocolGuid,
             NULL,
             (VOID **)&mCmdDb
             );
  if (EFI_ERROR (Status)) {
    mCmdDb = NULL;
    DEBUG ((DEBUG_ERROR, "Crane RPMh: CmdDB protocol unavailable: %r\n", Status));
    return Status;
  }
  Status = ValidateCmdDbProtocol (mCmdDb);
  if (EFI_ERROR (Status)) {
    mCmdDb = NULL;
    return Status;
  }

  mRpmh = CrDalGetRpmhContext ();
  if (mRpmh == NULL) {
    mCmdDb = NULL;
    return EFI_NOT_FOUND;
  }
  Target = CrDalGetPcieContext ();
  Status = InitializeRailDescriptions (Target);
  if (!EFI_ERROR(Status)) Status = InitializeAdditionalRails();
  if (!EFI_ERROR(Status)) Status = InitializeRailTopology();
  if (!EFI_ERROR(Status)) Status = InitializeArcDescriptions();
  if (EFI_ERROR (Status)) {
    mRpmh = NULL;
    mCmdDb = NULL;
    mRailCount = 0;
    return Status;
  }

  EfiInitializeLock (&mRailLock, TPL_NOTIFY);
  EfiInitializeLock (&mArcLock, TPL_NOTIFY);
  Status = (EFI_STATUS)RpmhLibInit (&mRpmh);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane RPMh: local TCS init failed: %r\n", Status));
    /* No floor demand was submitted. Preserve an initialized context rather
       than erase ownership if a backend failure committed initialization. */
    if (mRpmh != NULL && mRpmh->Initialized) return EFI_SUCCESS;
    mRpmh = NULL; mCmdDb = NULL; return Status;
  }
  if (mArcCount != 0) {
    EfiAcquireLock (&mArcLock); Status = EnsureArcFloor (); EfiReleaseLock (&mArcLock);
    if (EFI_ERROR (Status)) DEBUG ((DEBUG_WARN,
        "Crane RPMh: unsynchronized ARC boot floor pending %r; retaining provider for exact maximum retry\n", Status));
  }
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiRpmhCrProtocolGuid,
                  &gRpmhCrProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    if (mArcFloorAttempted) {
      DEBUG ((DEBUG_ERROR, "Crane RPMh: protocol publication failed %r; retaining acknowledged/uncertain boot floor and code\n", Status));
      return EFI_SUCCESS;
    }
    mRailCount = 0;
    mRpmh      = NULL;
    mCmdDb     = NULL;
    return Status;
  }

  DEBUG ((
    DEBUG_INFO,
    "Crane RPMh: standalone CmdDB/TCS adapter ready rails=%u context=%p\n",
    (UINT32)mRailCount,
    mRpmh
    ));
  return EFI_SUCCESS;
}
