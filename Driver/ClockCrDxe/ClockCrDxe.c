/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/clock.h>
#include <Library/CrTargetClockLib.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_string.h>

#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

STATIC ClockDriverContext   *mClockContext   = NULL;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmhCrProtocol = NULL;

STATIC EFI_STATUS
EFIAPI
ProtocolSetClock(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT64 RateHz, IN BOOLEAN Enable)
{
  ClockNode *Node;
  CR_STATUS Status;

  (VOID)This;
  if (mClockContext == NULL || Controller == NULL || Id == NULL) {
    return EFI_NOT_READY;
  }
  /* The current target has one GCC owner.  Keep the controller argument in
   * the protocol so another target can route it to a different clock owner. */
  if (AsciiStrCmp(Controller, "gcc") != 0) {
    return EFI_UNSUPPORTED;
  }
  Node = NULL;
  Status = GetClockNode(mClockContext, Id, NULL, NULL, &Node);
  if (CR_ERROR(Status) || Node == NULL) {
    return EFI_NOT_FOUND;
  }
  Status = ClockEnable(mClockContext, Node, RateHz, Enable);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
ProtocolSetGdsc(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN BOOLEAN Enable)
{
  ClockNode *Node;
  CR_STATUS Status;

  (VOID)This;
  if (mClockContext == NULL || Controller == NULL || Id == NULL) {
    return EFI_NOT_READY;
  }
  if (AsciiStrCmp(Controller, "gcc") != 0) {
    return EFI_UNSUPPORTED;
  }
  Node = NULL;
  Status = GetGdscNode(mClockContext, Id, &Node);
  if (CR_ERROR(Status) || Node == NULL) {
    return EFI_NOT_FOUND;
  }
  Status = Enable ? ClockGdscEnable(mClockContext, Node)
                  : ClockGdscDisable(mClockContext, Node);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
ProtocolSetReset(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN BOOLEAN Assert)
{
  CR_STATUS Status;

  (VOID)This;
  Status = CrTargetClockReset(Controller, Id, Assert);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

EFI_CLOCK_CR_PROTOCOL gClockCrProtocol = {
    .Revision = EFI_CLOCK_CR_PROTOCOL_REVISION,
    .SetClock = ProtocolSetClock,
    .SetGdsc  = ProtocolSetGdsc,
    .SetReset = ProtocolSetReset,
};

CR_STATUS
ClockEntryPoint(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Locate Rpmh protocol
  Status = gBS->LocateProtocol(
      &gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mRpmhCrProtocol);
  if (EFI_ERROR(Status)) {
    log_err("Failed to locate Rpmh Protocol, Status=0x%X", Status);
    return Status;
  }

  if (CR_ERROR(ClockLibInit(&mClockContext)) || (mClockContext == NULL)) {
    log_err("ClockLibInit failed");
    return EFI_DEVICE_ERROR;
  }

  // Install protocol
  Status = gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEfiClockCrProtocolGuid, &gClockCrProtocol, NULL);
  if (EFI_ERROR(Status)) {
    log_err("Failed to install Clock CR Protocol, Status=0x%X", Status);
    return Status;
  }

  // Test case (on hdk8450)
#if 0
  {
    UINTN Freq = 0;

    // Test cases - measure and display clock frequencies
    log_info("Starting clock frequency measurements...");

    // Enable Clock
    ClockNode *TargetClockNode = NULL, *Node2 = NULL, *GdscNode = NULL;
    Status = GetClockNode(
        mClockContext, "gcc_pcie_0_aux_clk", NULL, NULL, &TargetClockNode);
    if (CR_ERROR(Status) || (TargetClockNode == NULL)) {
      log_err("Failed to get clock node for gcc_pcie_0_aux_clk");
      return EFI_NOT_FOUND;
    }

    Status = GetClockNode(
        mClockContext, "gcc_pcie_0_aux_clk_src", NULL, NULL, &Node2);
    if (CR_ERROR(Status) || (Node2 == NULL)) {
      log_err("Failed to get clock node for gcc_pcie_0_aux_clk_src");
      return EFI_NOT_FOUND;
    }

    Status =
        GetClockNode(mClockContext, "gcc_pcie0_gdsc", NULL, NULL, &GdscNode);
    if (CR_ERROR(Status) || (GdscNode == NULL)) {
      log_err("Failed to get clock node for gcc_pcie0_gdsc");
      return EFI_NOT_FOUND;
    }
    // Enable GDSC first
    Status = ClockGdscEnable(mClockContext, GdscNode);
    if (CR_ERROR(Status)) {
      log_err("Failed to enable GDSC for gcc_pcie0_gdsc");
      return EFI_DEVICE_ERROR;
    }

    // Check RCG2 Status
    BOOLEAN RCGSTA = ClockRcg2CheckEnable(mClockContext, Node2);
    log_info(
        "RCG2 Status before enabling clock: " CR_LOG_CHAR8_STR_FMT,
        RCGSTA ? "Enabled" : "Disabled");

    ClockEnable(
        mClockContext, TargetClockNode,
        19200000, // 19.2 MHz
        TRUE);

    RCGSTA = ClockRcg2CheckEnable(mClockContext, Node2);
    log_info(
        "RCG2 Status after enabling clock: " CR_LOG_CHAR8_STR_FMT,
        RCGSTA ? "Enabled" : "Disabled");

    // Check frequency after enabling
    Status = DebugccMeasureClockRate("gcc_pcie_0_aux_clk", &Freq);
    if (EFI_ERROR(Status)) {
      log_err("Failed to measure clock rates");
    }
    log_info("Measured gcc_pcie_0_aux_clk frequency: %u Hz", Freq);

    // Measure clocks test
    DebugccDumpAllClocksFreq();
  }
#endif

  /* Keep the owner mapped while consumers use the protocol.  The previous
   * implementation unmapped GCC immediately after installing an empty
   * protocol, which made a later cold PCIe operation unsafe. */
  return EFI_SUCCESS;
}
