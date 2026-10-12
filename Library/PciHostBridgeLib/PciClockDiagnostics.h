/* Provider-described register observations only. SPDX-License-Identifier: MIT
 * No clock programming, polling, ownership or connector-frequency claim.
 * The published mapped controller is the sole address authority. */
#define PCIE_DIAGNOSTIC_CLOCK_NODES 32U
#define PCIE_DIAGNOSTIC_CLOCK_TABLE 1024U

STATIC BOOLEAN
CranePcieClockRegisterFits (CONST ClockController *C, UINT64 Offset)
{
  return C != NULL && C->MemMapped && C->Address != 0 &&
         !(C->Address & 3U) && C->Size >= 4U &&
         C->Address <= MAX_UINTN - C->Size && !(Offset & 3U) &&
         Offset <= C->Size - 4U;
}

STATIC CONST ClockNode *
CranePcieDiagnosticClock (CONST ClockDriverContext *Core, CONST CHAR8 *Controller,
                         CONST CHAR8 *Id, CR_DAL_CLOCK_RESOURCE_KIND Kind)
{
  CONST CHAR8 *NativeId = NULL;
  UINT32 Flags;
  if (Controller == NULL || Id == NULL ||
      CrDalClockResolveResource (Controller, Id, Kind, &NativeId, &Flags) != CR_SUCCESS ||
      NativeId == NULL) return NULL;
  for (UINTN I = 0; I < Core->ClockControllerCount; ++I) {
    CONST ClockController *C = &Core->ClockControllers[I];
    ClockNode *CONST *Nodes = Kind == CrDalClockResourcePowerDomain ? C->Gdscs : C->Clks;
    UINT32 Count = Kind == CrDalClockResourcePowerDomain ? C->GdscCount : C->ClkCount;
    if (Nodes == NULL || Count > PCIE_DIAGNOSTIC_CLOCK_TABLE) continue;
    for (UINT32 J = 0; J < Count; ++J)
      if (Nodes[J] != NULL && Nodes[J]->Name != NULL &&
          Nodes[J]->ParentController == C && AsciiStrCmp (Nodes[J]->Name, NativeId) == 0)
        return Nodes[J];
  }
  return NULL;
}

STATIC VOID
CranePcieQueueDiagnosticClock (CONST ClockNode **Nodes, UINTN *Count,
                               CONST ClockNode *Node)
{
  if (Node == NULL) return;
  for (UINTN I = 0; I < *Count; ++I) if (Nodes[I] == Node) return;
  if (*Count < PCIE_DIAGNOSTIC_CLOCK_NODES) Nodes[(*Count)++] = Node;
}

STATIC VOID
CranePcieClockHardwareNode (CONST PcieIoOps *Io, UINT16 Domain,
                           CONST ClockDriverContext *Core, CONST ClockNode *Node)
{
  CONST ClockController *C = Node->ParentController;
  UINT32 Offset[5], Value[5] = {0};
  UINTN Count = 0, I;
  if (Node->Name == NULL) return;
  for (I = 0; I < Core->ClockControllerCount; ++I)
    if (C == &Core->ClockControllers[I]) break;
  if (I == Core->ClockControllerCount) return;
  switch (Node->Type) {
    case CLOCK_NODE_TYPE_BRANCH:
    case CLOCK_NODE_TYPE_BRANCH_2:
      Offset[Count++] = Node->EnableRegister;
      Offset[Count++] = Node->HaltRegister;
      if (Node->HwcgMsk != 0) Offset[Count++] = Node->HwcgRegister;
      break;
    case CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2:
      if ((UINT64)Node->CmdRegister + Node->CmdRCfgOffset + 4U > MAX_UINT32) return;
      Offset[Count++] = Node->CmdRegister;
      Offset[Count++] = Node->CmdRegister + Node->CmdRCfgOffset + 4U;
      break;
    case CLOCK_NODE_TYPE_MUX:
    case CLOCK_NODE_TYPE_PHY_MUX:
      Offset[Count++] = Node->MuxRegister;
      break;
    case CLOCK_NODE_TYPE_FIXED_PLL: {
      CONST CR_FIXED_PLL_DESCRIPTION *D = &Node->FixedPll.Description;
      if (!Node->FixedPll.Initialized || D->Base != C->Address || D->Size != C->Size) return;
      Offset[Count++] = D->VoteRegister;
      Offset[Count++] = D->ModeRegister;
      Offset[Count++] = D->UserRegister;
      Offset[Count++] = D->LRegister;
      Offset[Count++] = D->AlphaRegister;
      break;
    }
    case CLOCK_NODE_TYPE_GDSC:
      Offset[Count++] = Node->GdscRegister;
      break;
    default:
      return;
  }
  for (I = 0; I < Count; ++I) {
    if (!CranePcieClockRegisterFits (C, Offset[I])) {
      DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw %a invalid register/mapping\n", Domain, Node->Name));
      return;
    }
  }
  for (I = 0; I < Count; ++I) Value[I] = Io->Read32 (Io->Context, C->Address + Offset[I]);
  if (Node->Type == CLOCK_NODE_TYPE_BRANCH || Node->Type == CLOCK_NODE_TYPE_BRANCH_2) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw %a gate-reg=0x%Lx gate=0x%x mask=0x%x halt-reg=0x%Lx halt=0x%x halt-policy=%u HWCG=0x%x mask=0x%x\n",
      Domain, Node->Name, (UINT64)C->Address + Offset[0], Value[0], Node->EnableMsk,
      (UINT64)C->Address + Offset[1], Value[1], Node->HaltCheckFlag, Value[2], Node->HwcgMsk));
  } else if (Node->Type == CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw %a cmd-reg=0x%Lx cmd=0x%x cfg=0x%x source=%u HID=%u\n",
      Domain, Node->Name, (UINT64)C->Address + Offset[0], Value[0], Value[1],
      (Value[1] >> 8) & 7U, Value[1] & 31U));
  } else if (Node->Type == CLOCK_NODE_TYPE_FIXED_PLL) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw %a vote=0x%x mode=0x%x user=0x%x L=0x%x alpha=0x%x\n",
      Domain, Node->Name, Value[0], Value[1], Value[2], Value[3], Value[4]));
  } else if (Node->Type == CLOCK_NODE_TYPE_GDSC) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u gdsc-hw %a reg=0x%Lx control=0x%x\n",
      Domain, Node->Name, (UINT64)C->Address + Offset[0], Value[0]));
  } else {
    DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw %a mux-reg=0x%Lx mux=0x%x mask=0x%x\n",
      Domain, Node->Name, (UINT64)C->Address + Offset[0], Value[0], Node->MuxMask));
  }
}

STATIC VOID
CranePcieDiagnoseClockHardware (CONST PcieIoOps *Io,
                               CONST PcieTargetController *Controller,
                               CONST PcieTargetPhy *Phy)
{
  CONST ClockDriverContext *Core = CrDalGetClockContext ();
  CONST ClockNode *Nodes[PCIE_DIAGNOSTIC_CLOCK_NODES];
  UINTN Count = 0;
  if (Io->Read32 == NULL || Core == NULL || !Core->LockInitialized ||
      Core->ClockControllers == NULL || Core->ClockControllerCount == 0 ||
      Core->ClockControllerCount > CC_TYPE_MAX) return;
  /* One node snapshot per call, including shared parents. Cycles and overly
     large supplier graphs cannot produce unbounded reads or logging. */
  for (UINTN Slice = 0; Slice < 2U; ++Slice) {
    UINT16 Offset = Slice == 0 ? Controller->ClockOffset : Phy == NULL ? 0 : Phy->ClockOffset;
    UINT16 Length = Slice == 0 ? Controller->ClockCount : Phy == NULL ? 0 : Phy->ClockCount;
    if (!CranePcieDiagnosticRangeValid (Offset, Length, mPcieTarget->ClockCount)) continue;
    for (UINT16 I = 0; I < Length; ++I) {
      CONST PcieTargetClock *Clock = &mPcieTarget->Clocks[Offset + I];
      CranePcieQueueDiagnosticClock (Nodes, &Count, CranePcieDiagnosticClock (
        Core, Clock->Controller, Clock->Id, CrDalClockResourceClock));
    }
  }
  CranePcieQueueDiagnosticClock (Nodes, &Count, CranePcieDiagnosticClock (Core,
    Controller->PowerDomainController, Controller->PowerDomainId, CrDalClockResourcePowerDomain));
  for (UINTN I = 0; I < Count; ++I) {
    CONST ClockNode *Node = Nodes[I];
    CranePcieClockHardwareNode (Io, Controller->Domain, Core, Node);
    if (Node->Parents == NULL || Node->ParentCount > 16U) continue;
    for (UINT16 J = 0; J < Node->ParentCount; ++J)
      CranePcieQueueDiagnosticClock (Nodes, &Count, Node->Parents[J]);
  }
  if (Count == PCIE_DIAGNOSTIC_CLOCK_NODES)
    DEBUG ((DEBUG_WARN, "PCIe: port %u clock-hw snapshot reached %u node bound\n",
      Controller->Domain, PCIE_DIAGNOSTIC_CLOCK_NODES));
}
