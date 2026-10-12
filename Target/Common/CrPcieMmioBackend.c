/* Native PCIe MMIO transport for SM8250/SM8350. SPDX-License-Identifier: MIT.
 * All controller addresses come from target data. MU host adapters own every
 * clock, regulator, GPIO and SMMU transaction. */
#include <Library/pcie.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <oskal/cr_memory.h>
#include <oskal/common.h>
#include <oskal/cr_string.h>
#include <oskal/cr_time.h>
#ifndef _KERNEL_MODE
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

  Io->Context = NULL;
  return CR_SUCCESS;
#endif
}
