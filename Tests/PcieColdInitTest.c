/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../Library/PcieLib/pcie.c"
#include "../Target/Sm8450/CrPcieTarget.c"

typedef struct { UINT64 Address; UINT32 Value; } REGISTER;
typedef struct { CONST CHAR8 *Id; UINTN Count; } VOTE;
STATIC REGISTER Registers[4096];
STATIC UINTN RegisterCount;
STATIC VOTE Clocks[64], Supplies[8];
STATIC UINTN Calls, FailAt, ReleaseCalls, FailReleaseAt;
STATIC BOOLEAN Power[2], Icb[2], Smmu[2], Perst[2], PhyProgrammed[2];
STATIC BOOLEAN Stopping;
STATIC UINT64 W1cAddress;
STATIC UINTN ConfigReads, ConfigWrites32, ConfigWrites16, ConfigWrites8;

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
  if (W1cAddress != 0 && Address == W1cAddress) ConfigReads++;
  for (I = 0; I < RegisterCount; I++) {
    if (Registers[I].Address == Address) return Registers[I].Value;
  }
  if (Address == SM8450_PCIE0_DBI + 0x34 || Address == SM8450_PCIE1_DBI + 0x34) return 0x70;
  if (Address == SM8450_PCIE0_DBI + 0x70 || Address == SM8450_PCIE1_DBI + 0x70) return 0x10;
  if (Address == SM8450_PCIE0_DBI + 0x80 || Address == SM8450_PCIE1_DBI + 0x80) return 0x20000000;
  return 0;
}
STATIC VOID Write(VOID *Cookie, UINT64 Address, UINT32 Value) {
  UINTN I;
  (VOID)Cookie;
  for (I = 0; I < 2; I++) {
    if (Address >= mPhys[I].Base && Address < mPhys[I].Base + mPhys[I].Size) {
      assert(Power[I] && Icb[I] && Smmu[I] && Perst[I]);
      PhyProgrammed[I] = TRUE;
    }
  }
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
STATIC VOID Delay(VOID *Cookie, UINT32 Microseconds) { (VOID)Cookie; (VOID)Microseconds; }
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
    if (IsPipeClock(C)) assert(PhyProgrammed[Port]);
  }
  return Vote(Clocks, ARRAY_SIZE(Clocks), C->Id, Enable);
}
STATIC CR_STATUS Supply(VOID *Cookie, CONST PcieTargetSupply *S, BOOLEAN Enable) {
  (VOID)Cookie;
  assert(strncmp(S->Id, "ldo", 3) == 0 && S->VoltageMv != 0 && S->Mode == 7);
  return Vote(Supplies, ARRAY_SIZE(Supplies), S->Id, Enable);
}
STATIC CR_STATUS State(BOOLEAN *Table, CONST PcieTargetController *C, BOOLEAN Enable) {
  CR_STATUS Status = Result(Enable);
  if (Enable || !CR_ERROR(Status)) Table[C->Domain] = Enable;
  return Status;
}
STATIC CR_STATUS Domain(VOID *Cookie, CONST PcieTargetController *C, BOOLEAN Enable) {
  (VOID)Cookie;
  if (Enable) assert(Perst[C->Domain]);
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
STATIC CR_STATUS Reset(VOID *Cookie, CONST PcieTargetReset *R, BOOLEAN Assert) {
  (VOID)Cookie; (VOID)R; (VOID)Assert;
  /* Both assertion and deassertion belong to bring-up until teardown. */
  return Result(!Stopping);
}
STATIC CR_STATUS Gpio(VOID *Cookie, CONST PcieTargetGpio *G, PCIE_GPIO_DIRECTION D, BOOLEAN Active) {
  (VOID)Cookie; (VOID)D;
  if (strcmp(G->Role, "perst") == 0) Perst[G->Pin == 94 ? 0 : 1] = Active;
  return CR_SUCCESS;
}
STATIC PcieIoOps Io = {
  .Read32 = Read, .Write32 = Write, .DelayUs = Delay,
  .SetClock = Clock, .SetReset = Reset, .SetPowerDomain = Domain,
  .SetSupply = Supply, .SetInterconnect = Interconnect,
  .SetIommu = Iommu, .SetGpio = Gpio,
  .Read8 = Read8, .Read16 = Read16, .Write8 = Write8, .Write16 = Write16
};

STATIC VOID Clear(VOID) {
  memset(Registers, 0, sizeof(Registers));
  memset(Clocks, 0, sizeof(Clocks));
  memset(Supplies, 0, sizeof(Supplies));
  memset(Power, 0, sizeof(Power)); memset(Icb, 0, sizeof(Icb));
  memset(Smmu, 0, sizeof(Smmu)); memset(Perst, 0, sizeof(Perst));
  memset(PhyProgrammed, 0, sizeof(PhyProgrammed));
  RegisterCount = Calls = ReleaseCalls = FailAt = FailReleaseAt = 0;
  Stopping = FALSE;
  W1cAddress = 0;
  ConfigReads = ConfigWrites32 = ConfigWrites16 = ConfigWrites8 = 0;
}
STATIC VOID Empty(VOID) {
  UINTN I;
  for (I = 0; I < ARRAY_SIZE(Clocks); I++) assert(Clocks[I].Count == 0);
  for (I = 0; I < ARRAY_SIZE(Supplies); I++) assert(Supplies[I].Count == 0);
  assert(!Power[0] && !Power[1] && !Icb[0] && !Icb[1] && !Smmu[0] && !Smmu[1]);
}
int main(void) {
  PcieDeviceContext *Context;
  PcieTargetContext *Target = CrTargetGetPcieContext();
  UINTN Steps, I;
  Clear();
  assert(PcieLibInit(&Context, Target, &Io) == CR_SUCCESS);
  assert(PcieInitializePort(Context, 0) == CR_SUCCESS);
  Steps = Calls;
  assert(Context->Ports[0].State == PCIE_PORT_LINK_UP);
  assert((Read(NULL, SM8450_PCIE0_DBI + 0x7c) & 15) == 2);
  assert((Read(NULL, SM8450_PCIE0_DBI + 0xa0) & 15) == 2);
  assert(PcieInitializePort(Context, 1) == CR_SUCCESS);
  assert((Read(NULL, SM8450_PCIE1_DBI + 0xa0) & 15) == 4);
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
  }
  Stopping = TRUE;
  assert(PcieShutdownPort(Context, 0) == CR_SUCCESS);
  assert(PcieShutdownPort(Context, 1) == CR_SUCCESS);
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
  puts("PCIe: cold ordering, rollback, exact-width configuration and per-BDF iATU passed");
  return 0;
}
