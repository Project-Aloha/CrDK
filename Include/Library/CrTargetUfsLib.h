/* Immutable native UFS resources for the qualified QMP v4/v5 boot profile.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/gpio.h>

#define CR_TARGET_UFS_REVISION 5U
#define CR_TARGET_UFS_MAX_CLOCKS 10U
#define CR_TARGET_UFS_MAX_RAILS 5U
#define CR_TARGET_UFS_MAX_SIDS 2U
#define CR_TARGET_UFS_MAX_WRITES 256U

typedef struct { UINT16 Offset; UINT8 Value; } CR_TARGET_UFS_PHY_WRITE;
typedef struct {
  CONST CR_TARGET_UFS_PHY_WRITE *Writes;
  UINT32 Count;
} CR_TARGET_UFS_PHY_TABLE;
typedef struct { CONST CHAR8 *Name; UINT64 RateHz; } CR_TARGET_UFS_CLOCK;
typedef struct { CONST CHAR8 *Name; UINT32 VoltageMv; UINT8 Mode; } CR_TARGET_UFS_RAIL;
/* Board-qualified RPMh wiring and selectable voltage domain. Roles use
 * CR_UFS_POWER_ROLE bits. PreferredUv is a requested vote, not a readback of
 * the bootloader's state. The FDT may narrow these constraints. */
typedef struct {
  CONST CHAR8 *Name;
  UINT32 Roles, MinUv, MaxUv, StepUv, BaseUv, PreferredUv;
  UINT8 Mode;
} CR_TARGET_UFS_POWER_RULE;
typedef struct {
  UINT32 Revision, Size;
  UINT64 GdscBase;
  BOOLEAN RequireHob;
  UINT32 FixedVccq2Uv;
  UINT32 RuleCount;
  CONST CR_TARGET_UFS_POWER_RULE *Rules;
  CONST CHAR8 *VccqParent;
  /* Revision 1: qualify the existing fixed-voltage topology.
   * Revision 2: zero means an optional FDT-declared enable-only supplier;
   * requires boot FDT and forbids deriving voltage/mode from init metadata. */
  UINT32 VccqParentUv;
  /* These board roles need a provider-owned retained VRM vote. */
  UINT32 RetainedRoles;
} CR_TARGET_UFS_POWER_POLICY;
typedef struct {
  CONST CHAR8 *Provider;
  UINT32 Source, Destination;
  UINT64 Average, Peak; /* Linux kB/s. */
} CR_TARGET_UFS_PATH;
typedef enum {
  CrUfsLineCfgWaipioFullWrite = 1,
  CrUfsLineCfgAndroidConditional = 2
} CR_TARGET_UFS_LINECFG;
typedef struct {
  UINT32 Revision, Size;
  UINT64 HciBase, PhyBase;
  UINT32 HciBytes, PhyBytes;
  UINT16 AllowedHciMajors;
  UINT8 PhyGeneration, MaxHsGear, Lanes;
  CONST CHAR8 *ClockController, *Gdsc, *Reset;
  CONST CR_TARGET_UFS_CLOCK *Clocks;
  UINT32 ClockCount, PrePhyClockCount;
  CONST CR_TARGET_UFS_RAIL *Rails;
  UINT32 RailCount;
  CR_TARGET_UFS_PATH Paths[2];
  GpioConfigParams DeviceReset;
  UINT64 SmmuBase;
  CONST UINT16 *Sids;
  UINT32 SidCount;
  UINT16 PhyStart, PhyPower, PhySwReset, PhyReady, PhyLineCfg;
  CR_TARGET_UFS_PHY_TABLE Base, Gear4, RateB;
  UINT16 RxInterfaceOffsets[2];
  UINT8 RxInterfaceCount;
  CR_TARGET_UFS_LINECFG LineCfgPolicy;
  BOOLEAN SkipIceForNonScsi;
  CONST CR_TARGET_UFS_POWER_POLICY *PowerPolicy;
  UINT32 RetainedRailMask; /* indices in Rails; separate from consumer leases */
  /* CPU WB common buffers require coherent SMMU domains for every UFS SID. */
  BOOLEAN DmaCoherent;
  /* Populated only after boot-FDT qualification; provider-retained enable. */
  CONST CHAR8 *RetainedEnableSupply;
  /* Explicit stopped ownership acquisition; only SM8250/8350 opt in. */
  BOOLEAN AcquireInheritedSmmu;
} CR_TARGET_UFS_CONTEXT;

/* Pointer storage is trusted immutable provider-owned metadata, not a byte
 * stream from firmware. Validate the complete profile before provider/MMIO
 * use. The first three named clocks are AXI/ICE/UniPro, each 300 MHz. HCI major
 * 4 is the only implemented timer layout for that rate; this is an admission
 * limit, not an assertion about the silicon found on a board. Tables already
 * contain both lanes, in exact write order. */
STATIC inline BOOLEAN CrTargetUfsString(CONST CHAR8 *S) {
  if (S == NULL || !S[0]) return FALSE;
  for (UINT32 I=1; I<64; ++I) if (!S[I]) return TRUE;
  return FALSE;
}
STATIC inline BOOLEAN CrTargetUfsSame(CONST CHAR8 *A, CONST CHAR8 *B) {
  for (UINT32 I=0; I<64; ++I) {
    if (A[I]!=B[I]) return FALSE;
    if (!A[I]) return TRUE;
  }
  return FALSE;
}
STATIC inline BOOLEAN CrTargetUfsOffset(UINT32 Bytes, UINT16 Offset) {
  return !(Offset&3) && Offset <= Bytes-4;
}
STATIC inline BOOLEAN CrTargetUfsTable(UINT32 Bytes, CONST CR_TARGET_UFS_PHY_TABLE *T) {
  if (T->Writes==NULL || !T->Count || T->Count>CR_TARGET_UFS_MAX_WRITES) return FALSE;
  for (UINT32 I=0; I<T->Count; ++I)
    if (!CrTargetUfsOffset(Bytes,T->Writes[I].Offset)) return FALSE;
  return TRUE;
}
STATIC inline BOOLEAN CrTargetUfsValid(CONST CR_TARGET_UFS_CONTEXT *C) {
  if (C==NULL || C->Revision!=CR_TARGET_UFS_REVISION || C->Size!=sizeof(*C) ||
      !C->HciBase || (C->HciBase&0xfff) || C->HciBytes!=0x3000 ||
      !C->PhyBase || (C->PhyBase&0xfff) || C->PhyBytes!=0xe00 ||
      C->HciBase>(UINT64)(~(UINTN)0)-C->HciBytes ||
      C->PhyBase>(UINT64)(~(UINTN)0)-C->PhyBytes ||
      !(C->HciBase+C->HciBytes<=C->PhyBase || C->PhyBase+C->PhyBytes<=C->HciBase) ||
      C->AllowedHciMajors!=(1U<<4) || (C->PhyGeneration!=4 && C->PhyGeneration!=5) ||
      C->MaxHsGear!=4 || C->Lanes!=2 ||
      !CrTargetUfsString(C->ClockController) || !CrTargetUfsString(C->Gdsc) || !CrTargetUfsString(C->Reset) ||
      C->Clocks==NULL || C->ClockCount!=CR_TARGET_UFS_MAX_CLOCKS || C->PrePhyClockCount!=7 ||
      C->Rails==NULL || !C->RailCount || C->RailCount>CR_TARGET_UFS_MAX_RAILS ||
      (C->RetainedRailMask & ~((1U<<C->RailCount)-1)) ||
      !C->SmmuBase || (C->SmmuBase&0xfff) || C->SmmuBase>(UINT64)(~(UINTN)0)-0x100000 ||
      !(C->SmmuBase+0x100000<=C->HciBase || C->HciBase+C->HciBytes<=C->SmmuBase) ||
      !(C->SmmuBase+0x100000<=C->PhyBase || C->PhyBase+C->PhyBytes<=C->SmmuBase) ||
      C->Sids==NULL || !C->SidCount || C->SidCount>CR_TARGET_UFS_MAX_SIDS ||
      C->DeviceReset.PinNumber>=GPIO_PINS_MAX || C->DeviceReset.OutputEnable!=TRUE ||
      C->DeviceReset.OutputValue!=GPIO_VALUE_LOW || C->DeviceReset.FunctionSel!=GPIO_FUNC_UNCHANGE ||
      (C->DeviceReset.Pull!=GPIO_PULL_NONE && C->DeviceReset.Pull!=GPIO_PULL_DOWN &&
       C->DeviceReset.Pull!=GPIO_PULL_UNCHANGE) ||
      (C->DeviceReset.DriveStrength!=GPIO_DRIVE_STRENGTH_UNCHANGE && C->DeviceReset.DriveStrength>7) ||
      C->RxInterfaceCount>2 || C->SkipIceForNonScsi>1 || C->DmaCoherent>1 ||
      (C->LineCfgPolicy!=CrUfsLineCfgWaipioFullWrite && C->LineCfgPolicy!=CrUfsLineCfgAndroidConditional)) return FALSE;
  if (C->RetainedEnableSupply && !CrTargetUfsString(C->RetainedEnableSupply)) return FALSE;
  CONST UINT16 Registers[]={C->PhyStart,C->PhyPower,C->PhySwReset,C->PhyReady,C->PhyLineCfg};
  for (UINT32 I=0; I<5; ++I) {
    if (!CrTargetUfsOffset(C->PhyBytes,Registers[I])) return FALSE;
    for (UINT32 J=0; J<I; ++J) if (Registers[I]==Registers[J]) return FALSE;
  }
  if (!CrTargetUfsTable(C->PhyBytes,&C->Base) || !CrTargetUfsTable(C->PhyBytes,&C->Gear4) ||
      !CrTargetUfsTable(C->PhyBytes,&C->RateB)) return FALSE;
  for (UINT32 I=0; I<C->ClockCount; ++I) {
    if (!CrTargetUfsString(C->Clocks[I].Name) ||
        C->Clocks[I].RateHz!=(I<3 ? 300000000ULL : I==5 ? 19200000ULL : 0)) return FALSE;
    for (UINT32 J=0; J<I; ++J) if (CrTargetUfsSame(C->Clocks[I].Name,C->Clocks[J].Name)) return FALSE;
  }
  for (UINT32 I=0; I<C->RailCount; ++I) {
    if (!CrTargetUfsString(C->Rails[I].Name) || !C->Rails[I].VoltageMv ||
        C->Rails[I].VoltageMv>5000 || C->Rails[I].Mode!=7) return FALSE;
    if (C->RetainedEnableSupply && CrTargetUfsSame(C->Rails[I].Name,C->RetainedEnableSupply)) return FALSE;
    for (UINT32 J=0; J<I; ++J) if (CrTargetUfsSame(C->Rails[I].Name,C->Rails[J].Name)) return FALSE;
  }
  for (UINT32 I=0; I<2; ++I)
    if (!CrTargetUfsString(C->Paths[I].Provider) || C->Paths[I].Source==C->Paths[I].Destination ||
        !C->Paths[I].Peak || C->Paths[I].Peak<C->Paths[I].Average) return FALSE;
  for (UINT32 I=0; I<C->SidCount; ++I) {
    if (C->Sids[I]==0xffff) return FALSE;
    for (UINT32 J=0; J<I; ++J) if (C->Sids[I]==C->Sids[J]) return FALSE;
  }
  for (UINT32 I=0; I<C->RxInterfaceCount; ++I) {
    if (!CrTargetUfsOffset(C->PhyBytes,C->RxInterfaceOffsets[I])) return FALSE;
    for (UINT32 J=0; J<5; ++J) if (C->RxInterfaceOffsets[I]==Registers[J]) return FALSE;
    for (UINT32 J=0; J<I; ++J) if (C->RxInterfaceOffsets[I]==C->RxInterfaceOffsets[J]) return FALSE;
  }
  return TRUE;
}
CONST CR_TARGET_UFS_CONTEXT *CrTargetGetUfsContext(VOID);
