/** @file Native SM8450 clock resource adapter. SPDX-License-Identifier: MIT */
#include <Library/CrTargetClockLib.h>
#include <Library/CrSm8450ClockLib.h>
#include <oskal/cr_string.h>

typedef struct {
  CR_DAL_CLOCK_RESOURCE_KIND  Kind;
  CONST CHAR8                *Id;
  CONST CHAR8                *NativeId;
  UINT32                      Flags;
} SM8450_NATIVE_CLOCK_RESOURCE;

/*
 * The Linux clock names below are the public Crane resource names. PCIe PHY
 * muxes use the same source-zero selection as Linux phy_mux_enable(); the
 * controller BCRs and PHY BCRs are programmed directly by CrTargetClockReset.
 */
STATIC CONST SM8450_NATIVE_CLOCK_RESOURCE mNativeClockResources[] = {
  { CrDalClockResourceClock, "gcc_usb3_prim_phy_pipe_clk",
    "gcc_usb3_prim_phy_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_0_pipe_clk_src",
    "gcc_pcie_0_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_0_pipe_clk",
    "gcc_pcie_0_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_pipe_clk_src",
    "gcc_pcie_1_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_pipe_clk",
    "gcc_pcie_1_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_phy_aux_clk_src",
    "gcc_pcie_1_phy_aux_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE },
  { CrDalClockResourceClock, "gcc_pcie_1_phy_aux_clk",
    "gcc_pcie_1_phy_aux_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE },
  { CrDalClockResourcePowerDomain, "pcie_0_gdsc",
    "pcie_0_gdsc", 0 },
  { CrDalClockResourcePowerDomain, "pcie_1_gdsc",
    "pcie_1_gdsc", 0 },
  { CrDalClockResourceReset, "gcc_pcie_0_bcr",
    "gcc_pcie_0_aux_clk", 0 },
  { CrDalClockResourceReset, "gcc_pcie_1_bcr",
    "gcc_pcie_1_aux_clk", 0 },
};

CR_STATUS
CrTargetClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  UINTN Index;

  if ((Controller == NULL) || (Id == NULL) ||
      (Kind >= CrDalClockResourceMax) || (NativeId == NULL) ||
      (Flags == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  *NativeId = NULL;
  *Flags    = 0;
  if ((cr_strcmp (Controller, "gcc") != 0) &&
      (cr_strcmp (Controller, "dispcc") != 0)) {
    return CR_UNSUPPORTED;
  }

  for (Index = 0; Index < ARRAY_SIZE (mNativeClockResources); Index++) {
    if ((mNativeClockResources[Index].Kind == Kind) &&
        (cr_strcmp (mNativeClockResources[Index].Id, Id) == 0)) {
      *NativeId = mNativeClockResources[Index].NativeId;
      *Flags    = mNativeClockResources[Index].Flags;
      return CR_SUCCESS;
    }
  }

  /* All other resources already use their target names. */
  *NativeId = Id;
  return CR_SUCCESS;
}


ClockDriverContext *CrTargetGetClockContext (VOID)
{
  return CrSm8450GetNativeClockContext ();
}

CR_STATUS CrTargetClockInit (ClockDriverContext *ClockContext)
{
  return CrSm8450ClockInit (ClockContext);
}

CR_STATUS CrTargetClockReset (CONST CHAR8 *Controller, CONST CHAR8 *Id,
                             BOOLEAN Assert)
{
  return CrSm8450ClockReset (CrTargetGetClockContext (), Controller, Id, Assert);
}
