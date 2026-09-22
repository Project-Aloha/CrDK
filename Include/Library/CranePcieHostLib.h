/** @file
  Shared cold-initialized PCIe host context used by the standard MU PCI
  host-bridge and PCI segment libraries.

  The generic PciHostBridgeDxe owns the UEFI protocols.  This library owns
  only the platform hardware context and the ECAM access path; it does not
  publish a private PCIe producer protocol.

  SPDX-License-Identifier: MIT
**/

#ifndef __CRANE_PCIE_HOST_LIB_H__
#define __CRANE_PCIE_HOST_LIB_H__

#include <Uefi.h>

#include <Library/pcie.h>

/**
  Convert one BDF/register tuple to the controller's ECAM address.

  This side-effect-free helper is shared by the runtime access path and host
  tests.  It validates the segment, bus aperture, alignment and the complete
  32-bit MMIO transaction before returning an address.
**/
static inline EFI_STATUS
CranePcieHostBuildConfigAddress (
  IN CONST PciePortRuntime  *Port,
  IN UINT16                  Segment,
  IN UINT8                   Bus,
  IN UINT8                   Device,
  IN UINT8                   Function,
  IN UINT16                  Register,
  IN UINTN                   Width,
  OUT UINT64                *ConfigAddress
  )
{
  UINT64 Offset;
  UINT64 AlignedOffset;
  UINT64 WindowBase;
  UINT64 WindowSize;
  BOOLEAN RootPort;

  if (Port == NULL || Port->Target == NULL || ConfigAddress == NULL ||
      ((Width != 1) && (Width != 2) && (Width != 4)) ||
      (Register >= 0x1000U) || (Register > 0x1000U - Width) ||
      (Register & (Width - 1U)) != 0 || Device > 31U || Function > 7U ||
      Segment != Port->Target->Domain || Bus < Port->Target->BusStart ||
      Bus > Port->Target->BusEnd) {
    return EFI_INVALID_PARAMETER;
  }

  /* Linux's DWC own-conf path exposes only the single root-port function on
   * the primary bus.  Other root-bus BDFs are absent devices, rather than
   * accesses into the downstream CFG aperture. */
  if (Bus == Port->Target->BusStart && (Device != 0 || Function != 0)) {
    return EFI_NOT_FOUND;
  }

  /* DesignWare exposes the root complex through its DBI aperture.  The
   * ECAM/config aperture is used only for buses downstream of that port. */
  RootPort = (Bus == Port->Target->BusStart) && (Device == 0) &&
             (Function == 0);
  WindowBase = RootPort ? Port->DbiBase : Port->ConfigBase;
  WindowSize = RootPort ? Port->DbiSize : Port->ConfigSize;
  Offset = RootPort ? (UINT64)Register
                    : (((UINT64)Bus - Port->Target->BusStart) << 20) +
                          ((UINT64)Device << 15) +
                          ((UINT64)Function << 12) + Register;
  AlignedOffset = Offset & ~3ULL;
  if (WindowSize < sizeof (UINT32) || AlignedOffset > WindowSize -
                                                   sizeof (UINT32) ||
      Offset > WindowSize - Width ||
      WindowBase > MAX_UINT64 - Offset) {
    return EFI_INVALID_PARAMETER;
  }
  *ConfigAddress = WindowBase + Offset;
  return EFI_SUCCESS;
}

/**
  Validate the provider route attached to a target clock entry.

  This helper is side-effect free so target-data tests can reject a PHY or
  RPMh clock accidentally routed to the GCC-only ClockCrDxe protocol.
**/
static inline BOOLEAN
CranePcieHostAsciiEqual (
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
  return (BOOLEAN)(*Left == *Right);
}

static inline BOOLEAN
CranePcieHostClockProviderValid (
  IN CONST PcieTargetClock  *Clock
  )
{

  if ((Clock == NULL) || (Clock->Controller == NULL) || (Clock->Id == NULL)) {
    return FALSE;
  }

  switch (Clock->Provider) {
    case PCIE_CLOCK_PROVIDER_GCC:
      return CranePcieHostAsciiEqual (Clock->Controller, "gcc");
    case PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS:
      return (CranePcieHostAsciiEqual (Clock->Controller, "pcie0_phy") &&
              CranePcieHostAsciiEqual (Clock->Id, "pcie_0_pipe_clk")) ||
             (CranePcieHostAsciiEqual (Clock->Controller, "pcie1_phy") &&
              CranePcieHostAsciiEqual (Clock->Id, "pcie_1_pipe_clk"));
    case PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON:
      return CranePcieHostAsciiEqual (Clock->Controller, "rpmhcc") &&
             CranePcieHostAsciiEqual (Clock->Id, "rpmh_cxo_clk");
    default:
      return FALSE;
  }

}

/**
  Cold-initialize all enabled PCIe root complexes described by the target.

  The target callback table is responsible for handing each resource to its
  owning driver (regulators/power domain, interconnect, clocks, resets, PHY,
  GPIO and SMMU).  PcieLib executes those callbacks in its required order.

  @retval EFI_SUCCESS       At least one root complex is ready.
  @retval EFI_NOT_FOUND     The target has no enabled root complexes.
  @retval EFI_UNSUPPORTED   The target data or callback table is incomplete.
  @retval EFI_DEVICE_ERROR  Cold initialization failed for every root complex.
**/
EFI_STATUS
EFIAPI
CranePcieHostInitialize (
  VOID
  );

/**
  Access one PCI configuration register through an initialized root complex.

  Address uses PCI_SEGMENT_LIB_ADDRESS() encoding.  Width must be 1, 2 or 4
  and must remain within the 4 KiB function configuration space.
**/
EFI_STATUS
EFIAPI
CranePcieHostConfigAccess (
  IN UINT64   Address,
  IN UINTN    Width,
  IN BOOLEAN  Write,
  IN OUT UINT32 *Value
  );

/** Return the initialized PcieLib context, or NULL before initialization. */
PcieDeviceContext *
EFIAPI
CranePcieHostGetContext (
  VOID
  );

/**
  Shut down initialized ports.  This is intended for a controlled teardown or
  test; normal DXE operation leaves the root complexes running.
**/
EFI_STATUS
EFIAPI
CranePcieHostShutdown (
  VOID
  );

#endif
