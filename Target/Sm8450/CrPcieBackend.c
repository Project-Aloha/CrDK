/** @file
 * SM8450 PCIe native callbacks. The NT default supplies no physical MMIO.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetPcieLib.h>
#include <Library/pcie.h>
#include <oskal/cr_status.h>

#ifndef _KERNEL_MODE
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#include <oskal/cr_time.h>

STATIC UINT32
TargetPcieRead32(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return CrMmioRead32((UINTN)Address);
}

STATIC VOID
TargetPcieWrite32(IN VOID *Context, IN UINT64 Address, IN UINT32 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)CrMmioWrite32((UINTN)Address, Value);
}

STATIC UINT8
TargetPcieRead8(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return MmioRead8((UINTN)Address);
}

STATIC UINT16
TargetPcieRead16(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return MmioRead16((UINTN)Address);
}

STATIC VOID
TargetPcieWrite8(IN VOID *Context, IN UINT64 Address, IN UINT8 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)MmioWrite8((UINTN)Address, Value);
}

STATIC VOID
TargetPcieWrite16(IN VOID *Context, IN UINT64 Address, IN UINT16 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)MmioWrite16((UINTN)Address, Value);
}

STATIC VOID
TargetPcieDelayUs(IN VOID *Context, IN UINT32 Microseconds)
{
  UNREFERENCED_PARAMETER(Context);
  cr_sleep(Microseconds);
}

STATIC UINT64
TargetPcieGetTimeUs(IN VOID *Context)
{
  UNREFERENCED_PARAMETER(Context);
#ifdef _KERNEL_MODE
  return KeQueryInterruptTime() / 10ULL;
#else
  return GetTimeInNanoSecond(GetPerformanceCounter()) / 1000ULL;
#endif
}

STATIC BOOLEAN
TargetPcieDiagnosePhy(
  IN CONST PcieIoOps *Io,
  IN CONST PcieTargetPhy *Phy,
  IN UINT16 Domain)
{
  /* Android waipio-pcie.dtsi, pcie1 qcom,phy-debug-reg. These are the exact
     read-only snapshot offsets consumed by pci-msm.c:msm_pcie_reg_copy().
     Do not use PARF's write-selected testbus here. Bound every address before
     the first read, including the mapped region's end and address overflow. */
  STATIC CONST UINT32 Offsets[] = {
    0x1068, 0x1140, 0x1144, 0x1148, 0x114c,
    0x1150, 0x1160, 0x1178, 0x00b8, 0x08b8,
    0x00c4, 0x08c4, 0x0478, 0x0c78, 0x1800,
    0x1c00, 0x1804, 0x1c04, 0x1808, 0x1c08,
    0x180c, 0x1c0c, 0x1810, 0x1c10, 0x1814,
    0x1c14, 0x1818, 0x1c18, 0x1a20, 0x1e20,
    0x1214, 0x1218, 0x121c, 0x1220, 0x1224,
    0x1228, 0x122c, 0x1230, 0x1234, 0x1238,
    0x123c, 0x1400, 0x1404
  };
  UINTN Index;

  if (Io == NULL || Io->Read32 == NULL || Phy == NULL || Domain != 1 ||
      Phy->Size < sizeof(UINT32) || (Phy->Base & 3U) != 0 ||
      Phy->Base > CR_MAX_UINT64 - Phy->Size) {
    return FALSE;
  }
  for (Index = 0; Index < ARRAY_SIZE(Offsets); ++Index) {
    if ((Offsets[Index] & 3U) != 0 ||
        Offsets[Index] > Phy->Size - sizeof(UINT32)) {
      return FALSE;
    }
  }
  for (Index = 0; Index < ARRAY_SIZE(Offsets); ++Index) {
    UINT64 Address = Phy->Base + Offsets[Index];
    log_warn("PCIe: port %u link timeout DT-PHY+0x%04X @" CR_LOG_HEX64_FMT "=0x%X",
             Domain, Offsets[Index], Address,
             Io->Read32(Io->Context, Address));
  }
  return TRUE;
}

STATIC VOID
TargetPcieDiagnoseLink(
  IN CONST PcieIoOps *Io,
  IN CONST PcieTargetController *Controller)
{
  /* gcc-sm8450.c: readable GCC vote/branch/mux/RCG/GDSC/BCR registers.
     Snapshot after the controller/PHY resets, with no writes or votes. */
  STATIC CONST struct { CONST CHAR8 *Name; UINT32 Offset[2]; } Registers[] = {
    { "vote0",         { 0x62000, 0x62000 } },
    { "vote2",         { 0x62008, 0x62008 } },
    { "bcr",           { 0x7b000, 0x9d000 } },
    { "gdsc",          { 0x7b004, 0x9d004 } },
    { "phy-bcr",       { 0x7c01c, 0x9e01c } },
    { "phy-com-bcr",   { 0x7c028, 0x9e000 } },
    { "nocsr-com-bcr", { 0x7c020, 0x9e020 } },
    { "link-down-bcr", { 0x7c014, 0x9e014 } },
    { "refclk",        { 0x9c004, 0x9c008 } },
    { "pipe-mux",      { 0x7b060, 0x9d064 } },
    { "pipe-cbcr",     { 0x7b03c, 0x9d040 } },
    { "aux-cbcr",      { 0x7b034, 0x9d030 } },
    { "aux-cmd",       { 0x7b064, 0x9d068 } },
    { "aux-cfg",       { 0x7b068, 0x9d06c } },
    { "rchng-cbcr",    { 0x7b044, 0x9d048 } },
    { "rchng-cmd",     { 0x7b048, 0x9d04c } },
    { "rchng-cfg",     { 0x7b04c, 0x9d050 } },
    { "cfg-cbcr",      { 0x7b030, 0x9d02c } },
    { "mstr-cbcr",     { 0x7b028, 0x9d024 } },
    { "slv-cbcr",      { 0x7b020, 0x9d01c } },
    { "q2a-cbcr",      { 0x7b01c, 0x9d018 } },
    { "phy-aux-cbcr",  { 0,       0x9d038 } },
    { "phy-aux-mux",   { 0,       0x9d080 } }
  };
  UINTN Index;
  if (Io == NULL || Io->Read32 == NULL || Controller == NULL || Controller->Domain > 1) return;
  for (Index = 0; Index < ARRAY_SIZE(Registers); ++Index) {
    UINT32 Offset = Registers[Index].Offset[Controller->Domain];
    UINT64 Address = 0x00100000ULL + Offset;
    if (Offset == 0) continue;
    log_warn("PCIe: port %u link timeout GCC-" CR_LOG_CHAR8_STR_FMT " @0x%lX=0x%X",
             Controller->Domain, Registers[Index].Name, Address,
             Io->Read32(Io->Context, Address));
  }
  CONST PcieTargetContext *Target = CrTargetGetPcieContext();
  if (Controller->Domain == 1 && Controller->PhyIndex < Target->PhyCount) {
    if (!TargetPcieDiagnosePhy(Io, &Target->Phys[Controller->PhyIndex], Controller->Domain)) {
      log_warn("PCIe: port %u skipped invalid PHY snapshot region", Controller->Domain);
    }
  }
}

/* These placeholders keep the portable callback table structurally complete
 * when a caller uses CrTargetLib on its own.  CranePcieHostLib replaces them
 * with adapters for the already-present MU drivers before PcieLibInit(). */
STATIC CR_STATUS
TargetPcieSetClock(IN VOID *Context, IN CONST PcieTargetClock *Clock,
                   IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Clock);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetReset(IN VOID *Context, IN CONST PcieTargetReset *Reset,
                   IN BOOLEAN Assert)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Reset);
  UNREFERENCED_PARAMETER(Assert);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetPowerDomain(IN VOID *Context,
                         IN CONST PcieTargetController *Controller,
                         IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetSupply(IN VOID *Context, IN CONST PcieTargetSupply *Supply,
                    IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Supply);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetInterconnect(IN VOID *Context,
                          IN CONST PcieTargetController *Controller,
                          IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetIommu(IN VOID *Context,
                   IN CONST PcieTargetController *Controller,
                   IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetGpio(IN VOID *Context, IN CONST PcieTargetGpio *Gpio,
                  IN PCIE_GPIO_DIRECTION Direction, IN BOOLEAN Active)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Gpio);
  UNREFERENCED_PARAMETER(Direction);
  UNREFERENCED_PARAMETER(Active);
  return CR_UNSUPPORTED;
}

#endif /* native EFI callbacks */

CR_STATUS
CrTargetGetPcieIo(OUT struct _PcieIoOps *Io)
{
  if (Io == NULL) {
    return CR_INVALID_PARAMETER;
  }

#ifdef _KERNEL_MODE
  /* The portable core accepts explicit assigned-resource callbacks through
   * PcieLibInit. A default raw-physical backend cannot establish that lifetime. */
  return CR_UNSUPPORTED;
#else
  cr_memset(Io, 0, sizeof(*Io));

  /* Resource callbacks are replaced by CranePcieHostLib, which owns the
   * corresponding MU drivers.  The explicit stubs keep the ABI valid for
   * callers which inspect the table before installing those adapters. */
  Io->Read32 = TargetPcieRead32;
  Io->Write32 = TargetPcieWrite32;
  Io->Read8 = TargetPcieRead8;
  Io->Read16 = TargetPcieRead16;
  Io->Write8 = TargetPcieWrite8;
  Io->Write16 = TargetPcieWrite16;
  Io->DelayUs = TargetPcieDelayUs;
  Io->GetTimeUs = TargetPcieGetTimeUs;
  Io->SetClock = TargetPcieSetClock;
  Io->SetReset = TargetPcieSetReset;
  Io->SetPowerDomain = TargetPcieSetPowerDomain;
  Io->SetSupply = TargetPcieSetSupply;
  Io->SetInterconnect = TargetPcieSetInterconnect;
  Io->SetIommu = TargetPcieSetIommu;
  Io->SetGpio = TargetPcieSetGpio;
  Io->DiagnoseLink = TargetPcieDiagnoseLink;
  Io->Context = NULL;
  return CR_SUCCESS;
#endif
}
