/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../Library/PcieLib/pcie.c"
#include "../Target/Sm8450/CrPcieTarget.c"
#include "../Target/Sm8450/CrPcieBackend.c"

typedef struct { UINT64 Address; UINT32 Value; } REGISTER;
typedef struct { CONST CHAR8 *Id; UINTN Count; } VOTE;
STATIC REGISTER Registers[4096];
STATIC UINTN RegisterCount;
STATIC VOTE Clocks[64], Supplies[8];
STATIC UINTN Calls, FailAt, ReleaseCalls, FailReleaseAt;
STATIC BOOLEAN Power[2], Icb[2], Smmu[2], Perst[2], PhyProgrammed[2];
STATIC BOOLEAN Clkreq[2], NoLink;
STATIC BOOLEAN WlanEndpointReady;
STATIC UINT32 AuxSource, AuxParentChanges, FailAuxAt;
STATIC BOOLEAN FailAuxParent;
STATIC UINTN LinkDiagnosticReads;
STATIC UINT64 ClockDiagnosticReads;
STATIC UINT64 PhyDiagnosticReads;
STATIC BOOLEAN PhySnapshotActive;
STATIC UINTN DiagnosticCalls;
STATIC UINT64 DroppedPhyAddress;
STATIC UINTN DroppedPhyReads;
STATIC UINT64 DelayTotal, PerstStart[2];
STATIC BOOLEAN Stopping;
STATIC UINT64 W1cAddress;
STATIC UINTN ConfigReads, ConfigWrites32, ConfigWrites16, ConfigWrites8;
STATIC UINTN MmioReads, MmioWrites;
STATIC BOOLEAN AliasType0Endpoint;
/* Exercise a capability list that is not at the common 0x70 offset. */
STATIC UINT8 ExpressCapOffset = 0x70;

VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  (VOID)Level; (VOID)Format;
}
VOID *EFIAPI SetMem(VOID *Buffer, UINTN Size, UINT8 Value) {
  return memset(Buffer, Value, Size);
}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A, CONST CHAR8 *B) { return strcmp(A, B); }
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) { *Lock = 1; return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 1); *Lock = 2; return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) { assert(*Lock == 2); *Lock = 1; return Lock; }
VOID EFIAPI MemoryFence(VOID) { }

STATIC UINT32 Read(VOID *Cookie, UINT64 Address) {
  UINTN I;
  (VOID)Cookie;
  ++MmioReads;
  if (PhySnapshotActive && !(Address >= 0x00100000ULL && Address < 0x00200000ULL)) {
    assert(Address >= 0x01c0e000ULL && Address < 0x01c10000ULL);
  }
  if (PhySnapshotActive && Address >= 0x01c0e000ULL && Address < 0x01c10000ULL) {
    /* Independent Android PCIe1 qcom,phy-debug-reg allowlist. Diagnostics
       require live owners, released PERST and completed training timeout. */
    STATIC CONST UINT32 Offsets[] = {
      0x1068,0x1140,0x1144,0x1148,0x114c,0x1150,0x1160,0x1178,
      0x00b8,0x08b8,0x00c4,0x08c4,0x0478,0x0c78,0x1800,0x1c00,
      0x1804,0x1c04,0x1808,0x1c08,0x180c,0x1c0c,0x1810,0x1c10,
      0x1814,0x1c14,0x1818,0x1c18,0x1a20,0x1e20,0x1214,0x1218,
      0x121c,0x1220,0x1224,0x1228,0x122c,0x1230,0x1234,0x1238,
      0x123c,0x1400,0x1404
    };
    BOOLEAN Found = FALSE;
    assert(NoLink && !Stopping && Power[1] && Icb[1] && Smmu[1] &&
           !Perst[1] && PhyProgrammed[1]);
    for (UINTN N=0;N<ARRAY_SIZE(Offsets);++N) {
      if (Address == 0x01c0e000ULL + Offsets[N]) {
        assert((PhyDiagnosticReads & (1ULL<<N)) == 0);
        PhyDiagnosticReads |= 1ULL<<N;
        Found = TRUE;
        break;
      }
    }
    assert(Found);
  }
  /* A PCIe Type-0 TLP addresses the function on this link; changing its
     Device bits must not create 31 additional copies of the endpoint. */
  if (AliasType0Endpoint && Address == SM8450_PCIE0_CONFIG) return 0x110317cbU;
  if (AliasType0Endpoint && Address == SM8450_PCIE0_CONFIG + 0x10) return 0xffe00000U;
  if (Address >= 0x00100000ULL && Address < 0x00200000ULL) {
    /* Independent physical addresses from gcc-sm8450.c; require the final
       powered, reset-released training state and forbid arbitrary reads. */
    STATIC CONST UINT32 Addresses[] = {
      0x162000, 0x162008, 0x19d000, 0x19d004, 0x19e01c, 0x19e000,
      0x19e020, 0x19e014, 0x19c008, 0x19d064, 0x19d040, 0x19d030,
      0x19d068, 0x19d06c, 0x19d048, 0x19d04c, 0x19d050, 0x19d02c,
      0x19d024, 0x19d01c, 0x19d018, 0x19d038, 0x19d080
    };
    assert(NoLink && Power[1] && Icb[1] && Smmu[1] && !Perst[1] && PhyProgrammed[1]);
    for (UINTN N=0;N<ARRAY_SIZE(Addresses);++N) {
      if(Address==Addresses[N]) { ClockDiagnosticReads|=1ULL<<N; return 0xabc00000U+(UINT32)N; }
    }
    assert(FALSE);
  }
  if (DroppedPhyAddress != 0 && Address == DroppedPhyAddress) ++DroppedPhyReads;
  if (W1cAddress != 0 && Address == W1cAddress) ConfigReads++;
  if (Address == SM8450_PCIE1_PARF + PCIE_PARF_INT_ALL_STATUS ||
      Address == SM8450_PCIE1_ELBI + PCIE_ELBI_SYS_STTS) {
    LinkDiagnosticReads++;
  }
  for (I = 0; I < RegisterCount; I++) {
    if (Registers[I].Address == Address) return Registers[I].Value;
  }
  if (Address == SM8450_PCIE0_DBI + 0x34 || Address == SM8450_PCIE1_DBI + 0x34) return ExpressCapOffset;
  if (Address == SM8450_PCIE0_DBI + ExpressCapOffset || Address == SM8450_PCIE1_DBI + ExpressCapOffset) return 0x10;
  if (Address == ((SM8450_PCIE0_DBI + ExpressCapOffset + PCIE_EXP_LINK_CAPABILITIES) & ~3ULL) ||
      Address == ((SM8450_PCIE1_DBI + ExpressCapOffset + PCIE_EXP_LINK_CAPABILITIES) & ~3ULL)) return 0x4;
  if (Address == ((SM8450_PCIE0_DBI + ExpressCapOffset + PCIE_EXP_LINK_STATUS) & ~3ULL) ||
      Address == ((SM8450_PCIE1_DBI + ExpressCapOffset + PCIE_EXP_LINK_STATUS) & ~3ULL)) return NoLink ? 0 : 0x20000000;
  return 0;
}
STATIC VOID CheckPhyConfiguration(UINTN Port) {
  /* Physical offsets from the Linux QSERDES headers and Android Waipio
     sequence, independent of the native target's block/entry constants. */
  if (Port == 0) {
    assert(Read(NULL, 0x01c07164ULL) == 0xbf);
    assert(Read(NULL, 0x01c07168ULL) == 0x3f);
    assert(Read(NULL, 0x01c07194ULL) == 0x38);
    assert(Read(NULL, 0x01c07178ULL) == 0x5c);
    assert(Read(NULL, 0x01c0717cULL) == 0x34);
    assert(Read(NULL, 0x01c07190ULL) == 0x34);
    assert(Read(NULL, 0x01c06620ULL) == 0xc1);
  } else {
    assert(Read(NULL, 0x01c0f16cULL) == 0x04);
    assert(Read(NULL, 0x01c0f174ULL) == 0x60);
    assert(Read(NULL, 0x01c0f41cULL) == 0xc1);
    assert(Read(NULL, 0x01c0f40cULL) == 0x1d);
    assert(Read(NULL, 0x01c0e078ULL) == 0x05);
    assert(Read(NULL, 0x01c0e878ULL) == 0x05);
    assert(Read(NULL, 0x01c0e3b0ULL) == 0x7f);
    assert(Read(NULL, 0x01c0ebb0ULL) == 0x7f);
    assert(Read(NULL, 0x01c0e3b4ULL) == 0x20);
    assert(Read(NULL, 0x01c0ebb4ULL) == 0x20);
    assert(Read(NULL, 0x01c0e3fcULL) == 0x1f);
    assert(Read(NULL, 0x01c0ebfcULL) == 0x1f);
  }
}
STATIC VOID Write(VOID *Cookie, UINT64 Address, UINT32 Value) {
  UINTN I;
  (VOID)Cookie;
  ++MmioWrites;
  assert(Address < 0x00100000ULL || Address >= 0x00200000ULL); /* diagnostics cannot mutate GCC */
  for (I = 0; I < 2; I++) {
    if (Address == (I == 0 ? SM8450_PCIE0_PARF : SM8450_PCIE1_PARF) +
                       PCIE_PARF_LTSSM &&
        (Value & PCIE_PARF_LTSSM_ENABLE) != 0) {
      assert(Clkreq[I] && !Perst[I] && PhyProgrammed[I]);
      /* A populated HDK PCIe0 needs the WCN PMU as well as the RC/PHY. */
      if (I == 0) assert(WlanEndpointReady);
      if (!NoLink) CheckPhyConfiguration(I);
      assert((Read(NULL, (I == 0 ? SM8450_PCIE0_PARF : SM8450_PCIE1_PARF) +
                         PCIE_PARF_SYS_CTRL) & 0x365eU) == 0x365eU);
    }
    if (Address >= mPhys[I].Base && Address < mPhys[I].Base + mPhys[I].Size) {
      assert(Power[I] && Icb[I] && Smmu[I] && Perst[I]);
      assert((Read(NULL, (I == 0 ? SM8450_PCIE0_PARF : SM8450_PCIE1_PARF) +
                         PCIE_PARF_SYS_CTRL) & 0x2000365eU) == 0x365eU);
      PhyProgrammed[I] = TRUE;
    }
  }
  if (DroppedPhyAddress != 0 && Address == DroppedPhyAddress) return;
  for (I = 0; I < RegisterCount; I++) {
    if (Registers[I].Address == Address) {
      if (W1cAddress != 0 && Address == W1cAddress) {
        ConfigWrites32++;
        Value = (Registers[I].Value & 0xffff0000U & ~Value) | (Value & 0xffffU);
      }
      Registers[I].Value = Value;
      return;
    }
  }
  assert(RegisterCount < ARRAY_SIZE(Registers));
  Registers[RegisterCount++] = (REGISTER){Address, Value};
}
STATIC VOID InjectRegister(UINT64 Address, UINT32 Value) {
  /* Model corruption/repair by the register fixture after PERST release,
     without invoking the driver's guarded initialization write callback. */
  for (UINTN I = 0; I < RegisterCount; I++) {
    if (Registers[I].Address == Address) {
      Registers[I].Value = Value;
      return;
    }
  }
  assert(RegisterCount < ARRAY_SIZE(Registers));
  Registers[RegisterCount++] = (REGISTER){Address, Value};
}
STATIC UINT8 Read8(VOID *Cookie, UINT64 Address) {
  return (UINT8)(Read(Cookie, Address & ~3ULL) >> ((Address & 3U) * 8U));
}
STATIC UINT16 Read16(VOID *Cookie, UINT64 Address) {
  return (UINT16)(Read(Cookie, Address & ~3ULL) >> ((Address & 3U) * 8U));
}
STATIC VOID WriteNarrow(UINT64 Address, UINT32 Value, UINTN Width) {
  UINTN I;
  UINT32 Shift = (Address & 3U) * 8U;
  UINT32 Mask = (Width == 1 ? 0xffU : 0xffffU) << Shift;
  for (I = 0; I < RegisterCount; I++) {
    if (Registers[I].Address == (Address & ~3ULL)) {
      if (Registers[I].Address == W1cAddress && Shift >= 16) {
        Registers[I].Value &= ~((Value << Shift) & Mask);
      } else {
        Registers[I].Value = (Registers[I].Value & ~Mask) | ((Value << Shift) & Mask);
      }
      return;
    }
  }
  assert(FALSE);
}
STATIC VOID Write8(VOID *Cookie, UINT64 Address, UINT8 Value) {
  (VOID)Cookie;
  ConfigWrites8++;
  WriteNarrow(Address, Value, 1);
}
STATIC VOID Write16(VOID *Cookie, UINT64 Address, UINT16 Value) {
  (VOID)Cookie;
  ConfigWrites16++;
  WriteNarrow(Address, Value, 2);
}
STATIC VOID Delay(VOID *Cookie, UINT32 Microseconds) { (VOID)Cookie; DelayTotal += Microseconds; }
STATIC CR_STATUS Result(BOOLEAN Enable) {
  if (Enable) return ++Calls == FailAt ? CR_DEVICE_ERROR : CR_SUCCESS;
  return ++ReleaseCalls == FailReleaseAt ? CR_DEVICE_ERROR : CR_SUCCESS;
}
STATIC CR_STATUS Vote(VOTE *Table, UINTN Limit, CONST CHAR8 *Id, BOOLEAN Enable) {
  UINTN I;
  CR_STATUS Status = Result(Enable);
  for (I = 0; I < Limit; I++) {
    if (Table[I].Id == NULL || strcmp(Table[I].Id, Id) == 0) break;
  }
  assert(I < Limit);
  Table[I].Id = Id;
  if (Enable) Table[I].Count++;
  else if (!CR_ERROR(Status)) { assert(Table[I].Count > 0); Table[I].Count--; }
  return Status;
}
STATIC CR_STATUS Clock(VOID *Cookie, CONST PcieTargetClock *C, BOOLEAN Enable) {
  UINTN Port = strstr(C->Id, "_0_") != NULL ? 0 : 1;
  (VOID)Cookie;
  if (Enable) {
    assert(Smmu[0] || Smmu[1]);
    /* The Linux controller bulk enables the GCC pipe clock before the
       controller BCR reset is released.  The PHY output is then enabled
       again by qmp_pcie_power_on() after its register table is programmed. */
    if (IsPipeClock(C)) assert(Perst[Port]);
    if (strcmp(C->Id, "gcc_pcie_1_phy_aux_clk") == 0) assert(AuxSource == 2);
  }
  return Vote(Clocks, ARRAY_SIZE(Clocks), C->Id, Enable);
}
STATIC CR_STATUS ClockParent(VOID *Cookie, CONST PcieTargetPhy *Phy, UINT32 Source) {
  (VOID)Cookie;
  assert(Phy == &mPhys[1] && Power[1] && Icb[1] && Perst[1]);
  if (Source == 0) {
    assert(PhyProgrammed[1]);
    assert((Read(NULL, Phy->Base + Phy->BlockOffsets.Pcs + Phy->StartControl) & 3) == 3);
    assert((Read(NULL, Phy->Base + Phy->BlockOffsets.Pcs + Phy->Status) & Phy->StatusMask) == 0);
  } else assert(Source == 2);
  ++AuxParentChanges;
  if (FailAuxParent || AuxParentChanges == FailAuxAt) return CR_DEVICE_ERROR;
  AuxSource = Source;
  return CR_SUCCESS;
}
STATIC CR_STATUS Supply(VOID *Cookie, CONST PcieTargetSupply *S, BOOLEAN Enable) {
  (VOID)Cookie;
  assert((strncmp(S->Id, "ldo", 3) == 0 && S->VoltageMv != 0 && S->Mode == 7) ||
         (strcmp(S->Id, "smpb10") == 0 && S->VoltageMv == 1800 && S->Mode == 6));
  return Vote(Supplies, ARRAY_SIZE(Supplies), S->Id, Enable);
}
STATIC CR_STATUS State(BOOLEAN *Table, CONST PcieTargetController *C, BOOLEAN Enable) {
  CR_STATUS Status = Result(Enable);
  if (Enable || !CR_ERROR(Status)) Table[C->Domain] = Enable;
  return Status;
}
STATIC CR_STATUS Domain(VOID *Cookie, CONST PcieTargetController *C, BOOLEAN Enable) {
  (VOID)Cookie;
  if (Enable) assert(Perst[C->Domain] && Clkreq[C->Domain]);
  return State(Power, C, Enable);
}
STATIC CR_STATUS Interconnect(VOID *Cookie, CONST PcieTargetController *C, BOOLEAN Enable) {
  (VOID)Cookie;
  if (Enable) assert(Power[C->Domain]);
  return State(Icb, C, Enable);
}
STATIC CR_STATUS Iommu(VOID *Cookie, CONST PcieTargetController *C, BOOLEAN Enable) {
  (VOID)Cookie;
  assert(Perst[C->Domain]);
  if (Enable) assert(Icb[C->Domain]);
  return State(Smmu, C, Enable);
}
STATIC CR_STATUS EndpointPower(CONST PcieIoOps *Ops, CONST PcieTargetController *C, BOOLEAN Enable) {
  CR_STATUS Status;
  assert(Ops != NULL && C->Domain == 0 && C->EndpointPower != NULL);
  assert(Power[0] && Icb[0] && Perst[0] && PhyProgrammed[0]);
  assert((Read(NULL, SM8450_PCIE0_PARF + PCIE_PARF_LTSSM) & PCIE_PARF_LTSSM_ENABLE) == 0);
  assert(Enable ? Smmu[0] : !Smmu[0]);
  Status = Result(Enable);
  /* Even a failed power-on can have partially enabled the endpoint. */
  if (Enable || !CR_ERROR(Status)) WlanEndpointReady = Enable;
  return Status;
}
STATIC CR_STATUS Reset(VOID *Cookie, CONST PcieTargetReset *R, BOOLEAN Assert) {
  (VOID)Cookie; (VOID)R; (VOID)Assert;
  /* Both assertion and deassertion belong to bring-up until teardown. */
  return Result(!Stopping);
}
STATIC CR_STATUS Gpio(VOID *Cookie, CONST PcieTargetGpio *G, PCIE_GPIO_DIRECTION D, BOOLEAN Active) {
  UINTN Port = G->Pin <= 96 ? 0 : 1;
  (VOID)Cookie;
  if (strcmp(G->Role, "perst") == 0) {
    assert(D == PCIE_GPIO_OUTPUT && G->Function == 0);
    if (Active) PerstStart[Port] = DelayTotal;
    else assert(DelayTotal - PerstStart[Port] >= PCIE_PERST_MIN_ASSERT_US);
    Perst[Port] = Active;
  } else if (strcmp(G->Role, "clkreq") == 0) {
    assert(D == PCIE_GPIO_INPUT && G->Function == 1 &&
           G->DriveStrengthMa == 2 && G->Pull == PCIE_GPIO_PULL_UP);
    Clkreq[Port] = TRUE;
  }
  return CR_SUCCESS;
}
STATIC UINT64 TimeUs(VOID *Cookie) { (VOID)Cookie; return DelayTotal; }
STATIC VOID Diagnose(CONST PcieIoOps *Ops, CONST PcieTargetController *Controller) {
  UINTN Writes = MmioWrites;
  UINT64 Before = DelayTotal;
  assert(Controller->Domain == 1 && !Stopping);
  assert(mPcieContext.Ports[1].PowerEnabled &&
         mPcieContext.Ports[1].ControllerClocksEnabled &&
         mPcieContext.Ports[1].PhyClocksEnabled &&
         mPcieContext.Ports[1].InterconnectEnabled &&
         mPcieContext.Ports[1].IommuAttached &&
         mPcieContext.Ports[1].SupplyOwnedMask == 0x3cU &&
         mPcieContext.Ports[1].ClockOwnedMask != 0);
  PhySnapshotActive = TRUE;
  TargetPcieDiagnoseLink(Ops, Controller);
  PhySnapshotActive = FALSE;
  assert(MmioWrites == Writes && DelayTotal == Before);
  ++DiagnosticCalls;
}
STATIC PcieIoOps Io = {
  .Read32 = Read, .Write32 = Write, .DelayUs = Delay,
  .SetClock = Clock, .SetReset = Reset, .SetPowerDomain = Domain,
  .SetSupply = Supply, .SetInterconnect = Interconnect,
  .SetIommu = Iommu, .SetGpio = Gpio,
  .Read8 = Read8, .Read16 = Read16, .Write8 = Write8, .Write16 = Write16,
  .SetClockParent = ClockParent, .DiagnoseLink = Diagnose,
  .SetEndpointPower = EndpointPower, .GetTimeUs = TimeUs
};

STATIC VOID Clear(VOID) {
  memset(Registers, 0, sizeof(Registers));
  memset(Clocks, 0, sizeof(Clocks));
  memset(Supplies, 0, sizeof(Supplies));
  memset(Power, 0, sizeof(Power)); memset(Icb, 0, sizeof(Icb));
  memset(Smmu, 0, sizeof(Smmu)); memset(Perst, 0, sizeof(Perst));
  memset(Clkreq, 0, sizeof(Clkreq)); memset(PerstStart, 0, sizeof(PerstStart));
  memset(PhyProgrammed, 0, sizeof(PhyProgrammed));
  RegisterCount = Calls = ReleaseCalls = FailAt = FailReleaseAt = 0;
  Stopping = FALSE;
  W1cAddress = 0;
  ConfigReads = ConfigWrites32 = ConfigWrites16 = ConfigWrites8 = 0;
  MmioReads = MmioWrites = 0;
  AliasType0Endpoint = FALSE;
  DelayTotal = LinkDiagnosticReads = 0;
  ClockDiagnosticReads = 0;
  PhyDiagnosticReads = DiagnosticCalls = 0;
  PhySnapshotActive = FALSE;
  DroppedPhyAddress = DroppedPhyReads = 0;
  NoLink = FALSE;
  WlanEndpointReady = FALSE;
  AuxSource = AuxParentChanges = FailAuxAt = 0;
  FailAuxParent = FALSE;
  ExpressCapOffset = 0x70;
}
STATIC VOID Empty(VOID) {
  UINTN I;
  for (I = 0; I < ARRAY_SIZE(Clocks); I++) assert(Clocks[I].Count == 0);
  for (I = 0; I < ARRAY_SIZE(Supplies); I++) assert(Supplies[I].Count == 0);
  assert(!Power[0] && !Power[1] && !Icb[0] && !Icb[1] && !Smmu[0] && !Smmu[1]);
  assert(!WlanEndpointReady);
}
int main(void) {
  PcieDeviceContext *Context;
  PcieTargetContext *Target = CrTargetGetPcieContext();
  UINTN Steps, I;
  /* PCIe1 needs the schematic's M.2 S10B prerequisite, then the vendor
     qref/1p8/0p9 PHY rails. Check numeric tuples independently. */
  assert(Target->Controllers[0].SupplyCount == 2);
  assert(Target->Controllers[1].SupplyCount == 4);
  assert(Target->Controllers[0].EndpointPower == &mWcn6855Power);
  assert(Target->Controllers[1].EndpointPower == NULL);
  assert(mWcn6855Power.EnableGpio.Pin == 80 && mWcn6855Power.XoGpio.Pin == 204);
  assert(mWcn6855Power.BtEnablePin == 81 && mWcn6855Power.SwControlPin == 82);
  assert(strcmp(Target->Supplies[2].Id, "smpb10") == 0 &&
         Target->Supplies[2].VoltageMv == 1800 && Target->Supplies[2].Mode == 6);
  assert(strcmp(Target->Supplies[3].Id, "ldob5") == 0 &&
         Target->Supplies[3].VoltageMv == 880);
  assert(strcmp(Target->Supplies[4].Id, "ldob6") == 0 &&
         Target->Supplies[4].VoltageMv == 1200);
  assert(strcmp(Target->Supplies[5].Id, "ldoh2") == 0 &&
         Target->Supplies[5].VoltageMv == 912);
  {
    PcieIoOps MissingParent = Io;
    PcieTargetContext InvalidTarget = *Target;
    PcieTargetPhy InvalidPhys[2];
    MissingParent.SetClockParent = NULL;
    assert(PcieLibInit(&Context, Target, &MissingParent) == CR_INVALID_PARAMETER);
    MissingParent = Io;
    MissingParent.SetEndpointPower = NULL;
    assert(PcieLibInit(&Context, Target, &MissingParent) == CR_INVALID_PARAMETER);
    memcpy(InvalidPhys, Target->Phys, sizeof(InvalidPhys));
    InvalidTarget.Phys = InvalidPhys;
    InvalidPhys[1].AuxMuxController = NULL;
    assert(PcieLibInit(&Context, &InvalidTarget, &Io) == CR_INVALID_PARAMETER);
    InvalidPhys[1] = Target->Phys[1];
    InvalidPhys[1].AuxPhySource = InvalidPhys[1].AuxReferenceSource;
    assert(PcieLibInit(&Context, &InvalidTarget, &Io) == CR_INVALID_PARAMETER);
  }
  /* Prevalidate the whole snapshot before any read, including a truncated
     tail, arithmetic overflow and an unaligned mapping. */
  {
    PcieTargetPhy InvalidPhy = Target->Phys[1];
    UINTN Before = MmioReads;
    InvalidPhy.Size = 0x1e23; /* last 32-bit register is truncated */
    assert(!TargetPcieDiagnosePhy(&Io, &InvalidPhy, 1));
    InvalidPhy = Target->Phys[1];
    InvalidPhy.Base = CR_MAX_UINT64 - 0x100U;
    assert(!TargetPcieDiagnosePhy(&Io, &InvalidPhy, 1));
    InvalidPhy = Target->Phys[1];
    ++InvalidPhy.Base;
    assert(!TargetPcieDiagnosePhy(&Io, &InvalidPhy, 1));
    assert(!TargetPcieDiagnosePhy(&Io, &Target->Phys[0], 0));
    assert(MmioReads == Before);
  }
  Clear();
  Registers[RegisterCount++] = (REGISTER){SM8450_PCIE0_DBI + 0x0c, 0xa5a5a5a5};
  Registers[RegisterCount++] = (REGISTER){SM8450_PCIE1_DBI + 0x0c, 0x5a5a5a5a};
  Registers[RegisterCount++] = (REGISTER){SM8450_PCIE0_PARF + PCIE_PARF_SYS_CTRL, 0};
  Registers[RegisterCount++] = (REGISTER){SM8450_PCIE1_PARF + PCIE_PARF_SYS_CTRL, 0x2010361cU};
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 0) == CR_SUCCESS);
  Steps = Calls;
  assert(Context->Ports[0].State == PCIE_PORT_LINK_UP);
  assert(Read(NULL, SM8450_PCIE0_PARF + PCIE_PARF_SYS_CTRL) == 0x365eU);
  assert((Read(NULL, SM8450_PCIE0_DBI + PCIE_DBI_PORT_LINK_CONTROL) &
          PCIE_DBI_PORT_LINK_DLL_ENABLE) != 0);
  assert(Read(NULL, SM8450_PCIE0_DBI + 0x0c) == 0xa5a5a5a5);
  assert((Read(NULL, SM8450_PCIE0_DBI + 0x7c) & 15) == 2);
  assert(((Read(NULL, SM8450_PCIE0_DBI + 0x7c) >> 4) & 0x3f) == 1);
  assert((Read(NULL, SM8450_PCIE0_DBI + 0xa0) & 15) == 2);
  ExpressCapOffset = 0x90;
  assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
  assert(Read(NULL, SM8450_PCIE1_PARF + PCIE_PARF_SYS_CTRL) == 0x0010365eU);
  /* The vendor Waipio HDK DTB sets qcom,target-link-speed = <3>. */
  assert((Read(NULL, SM8450_PCIE1_DBI + ExpressCapOffset + PCIE_EXP_LINK_CAPABILITIES) & 15) == 3);
  assert(((Read(NULL, SM8450_PCIE1_DBI + ExpressCapOffset + PCIE_EXP_LINK_CAPABILITIES) >> 4) & 0x3f) == 2);
  assert(Read(NULL, SM8450_PCIE1_DBI + 0x0c) == 0x5a5a5a5a);
  assert((Read(NULL, SM8450_PCIE1_DBI + PCIE_DBI_PORT_LINK_CONTROL) &
          PCIE_DBI_PORT_LINK_DLL_ENABLE) != 0);
  /* The mock shares one capability-list selector; restore port 0's layout
     before exercising downstream config accesses on that port. */
  ExpressCapOffset = 0x70;
  {
    UINT32 Value;
    UINT64 Atu = Context->Ports[0].AtuBase;
    UINT64 Address;
    UINT8 Bus;

    for (Bus = 0; Bus <= 1; Bus++) {
      W1cAddress = 0;
      Address = (Bus == 0 ? Context->Ports[0].DbiBase : Context->Ports[0].ConfigBase) + 4;
      Write(NULL, Address, 0xf9000000U);
      W1cAddress = Address;
      ConfigReads = ConfigWrites32 = ConfigWrites16 = ConfigWrites8 = 0;
      Value = 7;
      assert(PcieConfigAccess(Context, 0, Bus, 0, 0, 4, 2, TRUE, &Value) == CR_SUCCESS);
      assert(ConfigReads == 0 && ConfigWrites32 == 0 && ConfigWrites16 == 1);
      assert(Read(NULL, Address) == 0xf9000007U);
      Value = 0x10;
      assert(PcieConfigAccess(Context, 0, Bus, 0, 0, 7, 1, TRUE, &Value) == CR_SUCCESS);
      assert(ConfigWrites8 == 1 && Read(NULL, Address) == 0xe9000007U);
      Value = 0;
      assert(PcieConfigAccess(Context, 0, Bus, 0, 0, 6, 2, FALSE, &Value) == CR_SUCCESS);
      assert(Value == 0xe900);
      assert(PcieConfigAccess(Context, 0, Bus, 0, 0, 4, 1, FALSE, &Value) == CR_SUCCESS);
      assert(Value == 7);
      Context->Io.Write16 = NULL;
      assert(PcieConfigAccess(Context, 0, Bus, 0, 0, 4, 2, TRUE, &Value) == CR_UNSUPPORTED);
      Context->Io.Write16 = Write16;
    }
    /* Per-BDF config uses target BDF fields and no ECAM shift mode. */
    Value = 0;
    assert(PcieConfigAccess(Context, 0, 1, 0, 1, 0, 4, FALSE, &Value) == CR_SUCCESS);
    assert(Read(NULL, Atu + PCIE_ATU_LOWER_TARGET) == (PCIE_ATU_BUS(1) | PCIE_ATU_FUNC(1)));
    assert(Read(NULL, Atu + PCIE_ATU_REGION_CTRL1) == PCIE_ATU_TYPE_CFG0);
    assert(Read(NULL, Atu + PCIE_ATU_REGION_CTRL2) == (UINT32)PCIE_ATU_ENABLE);
    W1cAddress = 0;

    /* Reproduce the firmware enumerator scanning every slot. Without the
       Type-0 slot guard, one 2 MiB BAR is counted 32 times (64 MiB), matching
       the capture's 65 MiB request including its root-port allocation. */
    AliasType0Endpoint = TRUE;
    UINTN Endpoints = 0;
    UINT64 BarBytes = 0;
    for (UINT8 Device = 0; Device < 32; ++Device) {
      CR_STATUS Status = PcieConfigAccess(Context, 0, 1, Device, 0, 0, 4, FALSE, &Value);
      if (Status == CR_NOT_FOUND) continue;
      assert(Status == CR_SUCCESS && Value == 0x110317cbU);
      ++Endpoints;
      Value = ~0U;
      assert(PcieConfigAccess(Context, 0, 1, Device, 0, 0x10, 4, TRUE, &Value) == CR_SUCCESS);
      assert(PcieConfigAccess(Context, 0, 1, Device, 0, 0x10, 4, FALSE, &Value) == CR_SUCCESS);
      BarBytes += (UINT32)(~(Value & ~15U) + 1U);
    }
    assert(Endpoints == 1 && BarBytes == 0x200000U);
    for (UINT8 Device = 1; Device < 32; ++Device) {
      UINTN Reads = MmioReads, Writes = MmioWrites;
      Value = ~0U;
      assert(PcieConfigAccess(Context, 0, 1, Device, 7, 0x10, 4, TRUE, &Value) == CR_NOT_FOUND);
      assert(PcieConfigAccess(Context, 0, 1, Device, 0, 0, 4, FALSE, &Value) == CR_NOT_FOUND);
      assert(MmioReads == Reads && MmioWrites == Writes);
    }
    /* Functions on Device 0 and nonzero slots behind a further bridge stay
       accessible. The latter use Type-1 and retain their BDF Device bits. */
    assert(PcieConfigAccess(Context, 0, 1, 0, 7, 0, 4, FALSE, &Value) == CR_SUCCESS);
    assert(Read(NULL, Atu + PCIE_ATU_LOWER_TARGET) == (PCIE_ATU_BUS(1) | PCIE_ATU_FUNC(7)));
    assert(PcieConfigAccess(Context, 0, 2, 31, 7, 0, 4, FALSE, &Value) == CR_SUCCESS);
    assert(Read(NULL, Atu + PCIE_ATU_REGION_CTRL1) == PCIE_ATU_TYPE_CFG1);
    assert(Read(NULL, Atu + PCIE_ATU_LOWER_TARGET) == (PCIE_ATU_BUS(2) | PCIE_ATU_DEV(31) | PCIE_ATU_FUNC(7)));
    AliasType0Endpoint = FALSE;
  }
  Stopping = TRUE;
  assert(PcieShutdownPort(Context, 0) == CR_SUCCESS);
  assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
  Empty();
  /* Endpoint release failure keeps the PMU and powered RC/PHY available
     after the SMMU fence. A retry must finish that release exactly once. */
  Clear();
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 0) == CR_SUCCESS);
  Stopping = TRUE;
  FailReleaseAt = 2; /* SMMU block, then endpoint power-off */
  assert(PcieShutdownPort(Context, 0) == CR_DEVICE_ERROR);
  assert(Context->ResourceMask == BIT(0) && Context->Ports[0].EndpointPowerManaged);
  assert(WlanEndpointReady && Power[0] && Icb[0] && !Smmu[0]);
  FailReleaseAt = 0;
  assert(PcieInitializePort(Context, 0) == CR_BUSY);
  assert(PcieShutdownPort(Context, 0) == CR_SUCCESS);
  Empty();

  /* Each failing acquisition is rolled back once, retaining the shared votes
     of another live root complex. This exercises the real target slices. */
  for (I = 1; I <= Steps; I++) {
    Clear();
    assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
    assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
    FailAt = Calls + I;
    assert(CR_ERROR(PcieInitializePort(Context, 0)));
    assert(Context->ResourceMask == BIT(1));
    FailAt = 0;
    Stopping = TRUE;
    assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
    Empty();
  }
  /* Failed clock release preserves that resource and its power prerequisites.
     Retrying must not drop previously released shared clock votes twice. */
  Clear();
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 0) == CR_SUCCESS);
  Stopping = TRUE;
  FailReleaseAt = 5;
  assert(PcieShutdownPort(Context, 0) == CR_DEVICE_ERROR);
  assert(Context->ResourceMask == BIT(0) && Power[0]);
  FailReleaseAt = 0;
  assert(PcieShutdownPort(Context, 0) == CR_SUCCESS);
  Empty();
  /* An empty slot keeps its root complex exposed, but a training timeout must
     remain observable instead of being reported as successful link-up. */
  Clear();
  NoLink = TRUE;
  DroppedPhyAddress = 0x01c0f41cULL; /* model a lost endpoint-refclk write */
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
  assert(Context->Ports[1].State == PCIE_PORT_READY_NO_LINK);
  assert(Context->Ports[1].LastStatus == CR_TIMEOUT);
  assert(Context->InitializedMask == BIT(1) && Context->LinkMask == 0);
  assert(LinkDiagnosticReads == 2);
  assert(ClockDiagnosticReads == ((1ULL<<23)-1));
  assert(PhyDiagnosticReads == ((1ULL<<43)-1) && DiagnosticCalls == 1);
  assert(Context->Ports[1].TimingValidMask == 7U);
  assert(Context->Ports[1].PerstAssertTimeUs == PerstStart[1]);
  assert(Context->Ports[1].PerstReleaseTimeUs - Context->Ports[1].PerstAssertTimeUs >= 100000U);
  assert(Context->Ports[1].TrainingStartTimeUs - Context->Ports[1].PerstReleaseTimeUs == 1500U);
  assert(DelayTotal - Context->Ports[1].TrainingStartTimeUs == 1000000U);
  assert(DroppedPhyReads > 0); /* timeout actually checks configuration */
  assert(LogPhyConfiguration(Context, &Context->Ports[1]) == 1);
  InjectRegister(0x01c0f16cULL, 0xff); /* another corrupted config register */
  assert(LogPhyConfiguration(Context, &Context->Ports[1]) == 2);
  DroppedPhyAddress = 0;
  InjectRegister(0x01c0f41cULL, 0xc1);
  InjectRegister(0x01c0f16cULL, 0x04);
  assert(LogPhyConfiguration(Context, &Context->Ports[1]) == 0);
  NoLink = FALSE;
  assert(PcieIsLinkUp(Context, 1));
  assert(Context->Ports[1].State == PCIE_PORT_LINK_UP);
  assert(Context->Ports[1].LastStatus == CR_SUCCESS);
  assert(Context->LinkMask == BIT(1));
  NoLink = TRUE;
  assert(!PcieIsLinkUp(Context, 1));
  assert(Context->Ports[1].State == PCIE_PORT_READY_NO_LINK);
  assert(Context->Ports[1].LastStatus == CR_TIMEOUT);
  assert(Context->LinkMask == 0);
  Stopping = TRUE;
  assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
  Empty();
  /* Adapters without a monotonic clock retain the original cold-init path;
     lack of diagnostic timestamps must not become a resource requirement. */
  {
    PcieIoOps NoTime = Io;
    Clear();
    NoLink = TRUE;
    NoTime.GetTimeUs = NULL;
    assert(PcieLibInit(&Context, Target, &NoTime) == CR_SUCCESS);
    assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
    assert(Context->Ports[1].LastStatus == CR_TIMEOUT);
    assert(Context->Ports[1].TimingValidMask == 0 && DiagnosticCalls == 1);
    Stopping = TRUE;
    assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
    Empty();
  }
  /* Rejecting either startup parent switch must unwind the PHY resources. */
  for (I = 1; I <= 2; ++I) {
    Clear();
    FailAuxAt = I;
    assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
    assert(PcieInitializePort(Context, 1) == CR_DEVICE_ERROR);
    assert(Context->ResourceMask == 0 && !Context->Ports[1].AuxClockParentManaged);
    assert(AuxSource == 2 && AuxParentChanges == I + 1);
    assert(PhyProgrammed[1] == (I == 2));
    Empty();
  }
  /* A rejected XO restoration must retain the live PHY's prerequisites. */
  Clear();
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
  assert(AuxSource == 0 && AuxParentChanges == 2);
  FailAuxParent = TRUE;
  Stopping = TRUE;
  assert(PcieShutdownPort(Context, 1) == CR_DEVICE_ERROR);
  assert(Context->Ports[1].AuxClockParentManaged && Power[1] && Icb[1] && Smmu[1]);
  FailAuxParent = FALSE;
  assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
  assert(AuxSource == 2 && !Context->Ports[1].AuxClockParentManaged);
  Empty();
  puts("PCIe: CLKREQ/PERST cold ordering, link timeout/recovery, rollback, exact-width configuration and per-BDF iATU passed");
  return 0;
}
