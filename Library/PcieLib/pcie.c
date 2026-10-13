/** @file
 *  OS-independent Qualcomm PCIe root-complex and QMP PHY support.
 *
 *  The register sequences follow Linux pcie-qcom (1.9.0/2.7.0), the
 *  DesignWare host controller, and QMP PCIe PHY v4 drivers.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/CrTargetPcieLib.h>
#include <Library/pcie.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>

#ifdef _KERNEL_MODE
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "pcie.tmh"
#endif
#endif

#define PCIE_PARF_SYS_CTRL                    0x000
#define PCIE_PARF_PM_CTRL                     0x020
#define PCIE_PARF_PHY_CTRL                    0x040
#define PCIE_PARF_MHI_CLOCK_RESET_CTRL        0x174
#define PCIE_PARF_AXI_MSTR_WR_ADDR_HALT_V2    0x1A8
#define PCIE_PARF_Q2A_FLUSH                   0x1AC
#define PCIE_PARF_LTSSM                       0x1B0
#define PCIE_PARF_INT_ALL_STATUS              0x224
#define PCIE_PARF_CLKREQ_OVERRIDE             0x2B0
#define PCIE_PARF_DBI_BASE_ADDR_V2            0x350
#define PCIE_PARF_DBI_BASE_ADDR_V2_HI         0x354
#define PCIE_PARF_SLV_ADDR_SPACE_SIZE_V2      0x358
#define PCIE_PARF_SLV_ADDR_SPACE_SIZE_V2_HI   0x35C
#define PCIE_PARF_ATU_BASE_ADDR               0x634
#define PCIE_PARF_ATU_BASE_ADDR_HI            0x638
#define PCIE_PARF_DEVICE_TYPE                 0x1000
#define PCIE_PARF_BDF_TO_SID_TABLE            0x2000
#define PCIE_PARF_BDF_TO_SID_CFG              0x2C00

#define PCIE_PARF_DEVICE_TYPE_RC              4U
#define PCIE_PARF_PHY_TEST_POWER_DOWN         BIT(0)
#define PCIE_PARF_MAC_PHY_POWERDOWN_MUX       BIT(29)
#define PCIE_PARF_MHI_BYPASS                  BIT(4)
#define PCIE_PARF_REQ_NOT_ENTER_L1            BIT(5)
#define PCIE_PARF_AXI_HALT_ENABLE             BIT(31)
#define PCIE_PARF_LTSSM_ENABLE                BIT(8)
#define PCIE_PARF_BDF_TO_SID_BYPASS           BIT(0)

#define PCIE_DBI_COMMAND_STATUS               0x004
#define PCIE_DBI_CLASS_REVISION               0x008
#define PCIE_DBI_BAR0                         0x010
#define PCIE_DBI_BAR1                         0x014
#define PCIE_DBI_PRIMARY_BUS                  0x018
#define PCIE_DBI_CAPABILITY_LIST              0x034
#define PCIE_DBI_INTERRUPT_LINE               0x03C
#define PCIE_DBI_PORT_LINK_CONTROL            0x710
#define PCIE_DBI_PORT_DEBUG0                  0x728
#define PCIE_DBI_PORT_DEBUG1                  0x72C
#define PCIE_DBI_LINK_WIDTH_SPEED_CONTROL     0x80C
#define PCIE_DBI_MISC_CONTROL_1               0x8BC

/* Synopsys Gen3 equalization controls used by Qualcomm's qcom-pcie
 * common_set_equalization() before LTSSM is enabled. */
#define PCIE_DBI_GEN3_RELATED                 0x890
#define PCIE_DBI_GEN3_EQ_CONTROL              0x8A8
#define PCIE_DBI_GEN3_EQ_FB_MODE_DIR_CHANGE   0x8AC
#define PCIE_GEN3_ZRXDC_NONCOMPL              BIT(0)
#define PCIE_GEN3_RATE_SHADOW_SEL             GEN_MSK(25, 24)
#define PCIE_GEN3_FMDC_T_MIN_PHASE23          GEN_MSK(4, 0)
#define PCIE_GEN3_FMDC_N_EVALS                GEN_MSK(9, 5)
#define PCIE_GEN3_FMDC_MAX_PRE_CURSOR_DELTA   GEN_MSK(13, 10)
#define PCIE_GEN3_FMDC_MAX_POST_CURSOR_DELTA  GEN_MSK(17, 14)
#define PCIE_GEN3_EQ_FB_MODE                 GEN_MSK(3, 0)
#define PCIE_GEN3_EQ_PHASE23_EXIT_MODE       BIT(4)
#define PCIE_GEN3_EQ_PSET_REQ_VEC            GEN_MSK(23, 8)
#define PCIE_GEN3_EQ_FOM_INC_INITIAL_EVAL    BIT(24)

#define PCIE_DBI_COMMAND_IO                   BIT(0)
#define PCIE_DBI_COMMAND_MEMORY               BIT(1)
#define PCIE_DBI_COMMAND_MASTER               BIT(2)
#define PCIE_DBI_COMMAND_SERR                 BIT(8)
#define PCIE_DBI_CLASS_BRIDGE_PCI             0x0604U
#define PCIE_DBI_RO_WRITE_ENABLE              BIT(0)
#define PCIE_DBI_PORT_FAST_LINK_MODE          BIT(7)
#define PCIE_DBI_PORT_LINK_DLL_ENABLE         BIT(5)
#define PCIE_DBI_PORT_LINK_MODE_MASK          GEN_MSK(21, 16)
#define PCIE_DBI_LINK_WIDTH_MASK              GEN_MSK(12, 8)
#define PCIE_DBI_SPEED_CHANGE                 BIT(17)

#define PCIE_CAP_ID_EXPRESS                   0x10U
#define PCIE_CAP_NEXT_MASK                    0xFCU
#define PCIE_EXP_LINK_CAPABILITIES            0x0CU
#define PCIE_EXP_LINK_CAP_WIDTH_MASK          GEN_MSK(9, 4)
#define PCIE_EXP_LINK_CONTROL                 0x10U
#define PCIE_EXP_LINK_CONTROL2                0x30U
#define PCIE_EXP_LINK_STATUS                  0x12U
#define PCIE_EXP_SLOT_CAPABILITIES            0x14U
#define PCIE_EXP_LINK_ACTIVE                  BIT(13)
#define PCIE_EXP_SLOT_NO_COMMAND_COMPLETED    BIT(18)
#define PCIE_EXP_SLOT_ATTENTION_BUTTON        BIT(0)

/* The Qualcomm driver exposes the LTSSM state in both PARF and ELBI.  Keep
 * the raw values in the timeout trace; the state encodings are useful when a
 * board reaches Polling/Recovery but never receives a receiver detect or
 * lane training completion. */
#define PCIE_PARF_LTSSM_STATE_MASK            GEN_MSK(5, 0)
#define PCIE_ELBI_SYS_STTS                    0x008
#define PCIE_ELBI_SYS_CTRL                    0x004
#define PCIE_ELBI_LTSSM_STATE_MASK            GEN_MSK(17, 12)
#define PCIE_DBI_LTSSM_STATE_MASK             GEN_MSK(5, 0)

#define PCIE_ATU_REGION_STRIDE                0x200
#define PCIE_ATU_REGION_CTRL1                 0x000
#define PCIE_ATU_REGION_CTRL2                 0x004
#define PCIE_ATU_LOWER_BASE                   0x008
#define PCIE_ATU_UPPER_BASE                   0x00C
#define PCIE_ATU_LIMIT                        0x010
#define PCIE_ATU_LOWER_TARGET                 0x014
#define PCIE_ATU_UPPER_TARGET                 0x018
#define PCIE_ATU_ENABLE                       BIT(31)
#define PCIE_ATU_TYPE_MEMORY                  0x00U
#define PCIE_ATU_TYPE_IO                      0x02U
#define PCIE_ATU_TYPE_CFG0                    0x04U
#define PCIE_ATU_TYPE_CFG1                    0x05U
#define PCIE_ATU_BUS(Value)                   ((UINT32)(Value) << 24)
#define PCIE_ATU_DEV(Value)                   ((UINT32)(Value) << 19)
#define PCIE_ATU_FUNC(Value)                  ((UINT32)(Value) << 16)

#define PCIE_QMP_SW_RESET                     BIT(0)
#define PCIE_QMP_SERDES_START                 BIT(0)
#define PCIE_QMP_PCS_START                    BIT(1)

#define PCIE_PHY_RESET_DELAY_US               250U
#define PCIE_CONTROLLER_RESET_DELAY_US        1500U
#define PCIE_PHY_START_DELAY_US               1200U
#define PCIE_PHY_POLL_DELAY_US                200U
#define PCIE_PHY_TIMEOUT_US                   10000U
#define PCIE_PERST_MIN_ASSERT_US              100000U
#define PCIE_LINK_POLL_DELAY_US               1000U
#define PCIE_LINK_TIMEOUT_US                  1000000U
#define PCIE_ATU_RETRIES                      5U
#define PCIE_ATU_RETRY_DELAY_US               9U
#define PCIE_BDF_SID_ENTRIES                  256U
#define PCIE_BDF_SID_CRC_POLYNOMIAL           0x07U

STATIC PcieDeviceContext mPcieContext;

STATIC BOOLEAN
TargetRangeValid(IN UINT16 Offset, IN UINT16 Count, IN UINT16 Limit)
{
  return Offset <= Limit && Count <= (UINT16)(Limit - Offset);
}

STATIC BOOLEAN
AddressRangeValid(IN UINT64 Base, IN UINT64 Size)
{
  return Size != 0 && Base <= ~0ULL - (Size - 1U);
}

STATIC BOOLEAN
PhyRegisterValid(
    IN CONST PcieTargetPhy *Phy, IN UINT32 BlockOffset,
    IN UINT32 RegisterOffset)
{
  UINT64 Offset;

  if ((BlockOffset & (sizeof(UINT32) - 1U)) != 0 ||
      (RegisterOffset & (sizeof(UINT32) - 1U)) != 0 ||
      (UINT64)BlockOffset > Phy->Size) {
    return FALSE;
  }
  Offset = (UINT64)BlockOffset + RegisterOffset;
  return Offset <= Phy->Size && sizeof(UINT32) <= Phy->Size - Offset;
}

STATIC BOOLEAN
PhyInitEntryValid(
    IN CONST PcieTargetPhy *Phy, IN CONST PciePhyInitEntry *Entry)
{
  UINT32 Primary;
  UINT32 Secondary;
  BOOLEAN PerLane;
  UINT8 AvailableLanes;

  Secondary = 0;
  PerLane   = FALSE;
  switch (Entry->Block) {
  case PCIE_PHY_BLOCK_SERDES:
    Primary = Phy->BlockOffsets.Serdes;
    break;
  case PCIE_PHY_BLOCK_TX:
    Primary   = Phy->BlockOffsets.Tx;
    Secondary = Phy->BlockOffsets.Tx2;
    PerLane   = TRUE;
    break;
  case PCIE_PHY_BLOCK_RX:
    Primary   = Phy->BlockOffsets.Rx;
    Secondary = Phy->BlockOffsets.Rx2;
    PerLane   = TRUE;
    break;
  case PCIE_PHY_BLOCK_PCS:
    Primary = Phy->BlockOffsets.Pcs;
    break;
  case PCIE_PHY_BLOCK_PCS_MISC:
    Primary = Phy->BlockOffsets.PcsMisc;
    break;
  default:
    return FALSE;
  }

  if (!PerLane) {
    return PhyRegisterValid(Phy, Primary, Entry->Offset);
  }
  AvailableLanes = (UINT8)((1U << Phy->Lanes) - 1U);
  if ((Entry->LaneMask & AvailableLanes) == 0 ||
      ((Entry->LaneMask & BIT(0)) != 0 &&
       !PhyRegisterValid(Phy, Primary, Entry->Offset))) {
    return FALSE;
  }
  return Phy->Lanes < 2 || (Entry->LaneMask & BIT(1)) == 0 ||
         (Secondary != 0 &&
          PhyRegisterValid(Phy, Secondary, Entry->Offset));
}

STATIC UINT32
PcieRead32(IN PcieDeviceContext *Context, IN UINT64 Address)
{
  return Context->Io.Read32(Context->Io.Context, Address);
}

STATIC VOID
PcieWrite32(
    IN PcieDeviceContext *Context, IN UINT64 Address, IN UINT32 Value)
{
  Context->Io.Write32(Context->Io.Context, Address, Value);
}

STATIC VOID
PcieRmw32(
    IN PcieDeviceContext *Context, IN UINT64 Address, IN UINT32 Clear,
    IN UINT32 Set)
{
  UINT32 Value = PcieRead32(Context, Address);
  PcieWrite32(Context, Address, (Value & ~Clear) | Set);
}

STATIC UINT8
PcieRead8(IN PcieDeviceContext *Context, IN UINT64 Address)
{
  UINT32 Shift = (UINT32)(Address & 3U) * 8U;
  return (UINT8)(PcieRead32(Context, Address & ~3ULL) >> Shift);
}

STATIC UINT16
PcieRead16(IN PcieDeviceContext *Context, IN UINT64 Address)
{
  UINT32 Shift = (UINT32)(Address & 2U) * 8U;
  return (UINT16)(PcieRead32(Context, Address & ~3ULL) >> Shift);
}

STATIC CONST PcieTargetRegion *
FindRegion(
    IN CONST PcieTargetContext *Target,
    IN CONST PcieTargetController *Controller, IN CONST CHAR8 *Name)
{
  UINT16 Index;

  for (Index = 0; Index < Controller->RegionCount; Index++) {
    CONST PcieTargetRegion *Region =
        &Target->Regions[Controller->RegionOffset + Index];
    if (Region->Name != NULL && cr_strcmp(Region->Name, Name) == 0) {
      return Region;
    }
  }
  return NULL;
}

STATIC CR_STATUS
ValidateTarget(IN CONST PcieTargetContext *Target, IN CONST PcieIoOps *Io)
{
  UINT16 Index;

  if (Target == NULL || Io == NULL || Io->Read32 == NULL ||
      Io->Write32 == NULL || Io->DelayUs == NULL ||
      Target->Controllers == NULL || Target->ControllerCount == 0 ||
      Target->ControllerCount > PCIE_MAX_CONTROLLERS ||
      Target->Regions == NULL || Target->RegionCount == 0 ||
      Target->Ranges == NULL || Target->Clocks == NULL ||
      Target->Resets == NULL || Target->Gpios == NULL ||
      Target->Phys == NULL || Target->PhyCount == 0 ||
      Target->PhyInit == NULL || Target->PhyInitCount == 0) {
    return CR_INVALID_PARAMETER;
  }
  /* A cold root-complex bring-up must explicitly vote the NoC and hand off
   * stream ownership.  Letting either operation disappear silently can
   * expose a DMA-capable endpoint with no fabric or SMMU contract. */
  if (Io->SetInterconnect == NULL ||
      (Target->SupplyCount != 0 && Io->SetSupply == NULL) ||
      (Target->IommuMapCount != 0 && Io->SetIommu == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  if ((Target->InterruptCount != 0 && Target->Interrupts == NULL) ||
      (Target->IommuMapCount != 0 && Target->IommuMaps == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  if ((Target->ClockCount != 0 && Io->SetClock == NULL) ||
      (Target->ResetCount != 0 && Io->SetReset == NULL) ||
      (Target->GpioCount != 0 && Io->SetGpio == NULL)) {
    return CR_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Target->RegionCount; Index++) {
    if (Target->Regions[Index].Name == NULL ||
        !AddressRangeValid(Target->Regions[Index].Base,
                           Target->Regions[Index].Size)) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->RangeCount; Index++) {
    CONST PcieTargetRange *Range = &Target->Ranges[Index];
    if (Range->Type > PCIE_RANGE_MEM64 ||
        !AddressRangeValid(Range->PciBase, Range->Size) ||
        !AddressRangeValid(Range->CpuBase, Range->Size) ||
        (Range->Type == PCIE_RANGE_MEM32 &&
         (Range->PciBase > (UINT32)~0U ||
          Range->PciBase + Range->Size - 1U > (UINT32)~0U))) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->ClockCount; Index++) {
    if (Target->Clocks[Index].Name == NULL ||
        Target->Clocks[Index].Controller == NULL ||
        Target->Clocks[Index].Id == NULL ||
        Target->Clocks[Index].Provider >= PCIE_CLOCK_PROVIDER_MAX) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->ResetCount; Index++) {
    if (Target->Resets[Index].Name == NULL ||
        Target->Resets[Index].Controller == NULL ||
        Target->Resets[Index].Id == NULL) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->SupplyCount; Index++) {
    if (Target->Supplies[Index].Name == NULL ||
        Target->Supplies[Index].Controller == NULL ||
        Target->Supplies[Index].Id == NULL) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->GpioCount; Index++) {
    if (Target->Gpios[Index].Role == NULL ||
        Target->Gpios[Index].Controller == NULL ||
        Target->Gpios[Index].Polarity == NULL ||
        Target->Gpios[Index].Function > 15U ||
        Target->Gpios[Index].DriveStrengthMa < 2U ||
        Target->Gpios[Index].DriveStrengthMa > 16U ||
        (Target->Gpios[Index].DriveStrengthMa & 1U) != 0 ||
        Target->Gpios[Index].Pull > PCIE_GPIO_PULL_UP ||
        (cr_strcmp(Target->Gpios[Index].Polarity, "GPIO_ACTIVE_LOW") != 0 &&
         cr_strcmp(Target->Gpios[Index].Polarity, "GPIO_ACTIVE_HIGH") != 0)) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->InterruptCount; Index++) {
    if (Target->Interrupts[Index].Name == NULL ||
        Target->Interrupts[Index].Trigger > CR_INTERRUPT_TRIGGER_EDGE_BOTH) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->IommuMapCount; Index++) {
    CONST PcieTargetIommuMap *Map = &Target->IommuMaps[Index];
    if (Map->Controller == NULL || Map->Count != 1) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->PhyInitCount; Index++) {
    if (Target->PhyInit[Index].Phase > PCIE_PHY_PHASE_RC ||
        Target->PhyInit[Index].Block > PCIE_PHY_BLOCK_PCS_MISC ||
        Target->PhyInit[Index].LaneMask == 0 ||
        (Target->PhyInit[Index].Offset & (sizeof(UINT32) - 1U)) != 0) {
      return CR_INVALID_PARAMETER;
    }
  }
  for (Index = 0; Index < Target->PhyCount; Index++) {
    CONST PcieTargetPhy *Phy = &Target->Phys[Index];
    if ((Phy->AuxMuxController == NULL) != (Phy->AuxMuxId == NULL) ||
        (Phy->AuxMuxId != NULL &&
         (Io->SetClockParent == NULL ||
          Phy->AuxReferenceSource == Phy->AuxPhySource))) {
      return CR_INVALID_PARAMETER;
    }
    if (Phy->Label == NULL || Phy->Compatible == NULL ||
        !AddressRangeValid(Phy->Base, Phy->Size) ||
        (Phy->Lanes != 1 && Phy->Lanes != 2) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Serdes, 0) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Tx, 0) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Rx, 0) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Pcs, 0) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.PcsMisc, 0) ||
        (Phy->Lanes == 2 &&
         (Phy->BlockOffsets.Tx2 == 0 || Phy->BlockOffsets.Rx2 == 0)) ||
        (Phy->Lanes == 2 &&
         (!PhyRegisterValid(Phy, Phy->BlockOffsets.Tx2, 0) ||
          !PhyRegisterValid(Phy, Phy->BlockOffsets.Rx2, 0))) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Pcs,
                          Phy->SwReset) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Pcs,
                          Phy->StartControl) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Pcs, Phy->Status) ||
        !PhyRegisterValid(Phy, Phy->BlockOffsets.Pcs,
                          Phy->PowerDownControl) ||
        !TargetRangeValid(Phy->InitOffset, Phy->InitCount,
                          Target->PhyInitCount) ||
        !TargetRangeValid(Phy->ClockOffset, Phy->ClockCount,
                          Target->ClockCount) ||
        !TargetRangeValid(Phy->ResetOffset, Phy->ResetCount,
                          Target->ResetCount) ||
        Phy->StatusMask == 0) {
      return CR_INVALID_PARAMETER;
    }
    {
      UINT16 EntryIndex;
      for (EntryIndex = 0; EntryIndex < Phy->InitCount; EntryIndex++) {
        if (!PhyInitEntryValid(
                Phy, &Target->PhyInit[Phy->InitOffset + EntryIndex])) {
          return CR_INVALID_PARAMETER;
        }
      }
    }
  }

  for (Index = 0; Index < Target->ControllerCount; Index++) {
    CONST PcieTargetController *Controller = &Target->Controllers[Index];
    CONST PcieTargetPhy *Phy;
    CONST PcieTargetRegion *Parf;
    CONST PcieTargetRegion *Dbi;
    CONST PcieTargetRegion *Atu;
    CONST PcieTargetRegion *Config;
    UINT16 Inner;
    UINT16 PerstCount;
    UINT8 RangeTypes;

    if (Controller->EndpointPower != NULL) {
      CONST PcieTargetEndpointPower *Endpoint = Controller->EndpointPower;
      CONST PcieTargetGpio *Gpios[] = { &Endpoint->EnableGpio, &Endpoint->XoGpio };
      if (Io->SetEndpointPower == NULL || Endpoint->Compatible == NULL ||
          Endpoint->SupplyCount == 0 || Endpoint->SupplyCount > PCIE_MAX_RESOURCES ||
          !TargetRangeValid(Endpoint->SupplyOffset, Endpoint->SupplyCount,
                            Target->SupplyCount) ||
          Endpoint->EnableGpio.Pin == Endpoint->XoGpio.Pin ||
          Endpoint->EnableGpio.Pin == Endpoint->BtEnablePin ||
          Endpoint->EnableGpio.Pin == Endpoint->SwControlPin ||
          Endpoint->XoGpio.Pin == Endpoint->BtEnablePin ||
          Endpoint->XoGpio.Pin == Endpoint->SwControlPin) {
        return CR_INVALID_PARAMETER;
      }
      for (Inner = 0; Inner < ARRAY_SIZE(Gpios); ++Inner) {
        CONST PcieTargetGpio *Gpio = Gpios[Inner];
        if (Gpio->Role == NULL || Gpio->Controller == NULL ||
            Gpio->Polarity == NULL || Gpio->Function > 15U ||
            Gpio->DriveStrengthMa < 2U || Gpio->DriveStrengthMa > 16U ||
            (Gpio->DriveStrengthMa & 1U) != 0 ||
            Gpio->Pull > PCIE_GPIO_PULL_UP ||
            (cr_strcmp(Gpio->Polarity, "GPIO_ACTIVE_LOW") != 0 &&
             cr_strcmp(Gpio->Polarity, "GPIO_ACTIVE_HIGH") != 0)) {
          return CR_INVALID_PARAMETER;
        }
      }
    }

    if (Controller->MaxLinkSpeed > 4 || Controller->Label == NULL || Controller->Compatible == NULL ||
        Controller->BusStart > Controller->BusEnd ||
        Controller->RangeCount == 0 || Controller->GpioCount == 0 ||
        Controller->PhyIndex >= Target->PhyCount ||
        !TargetRangeValid(Controller->RegionOffset, Controller->RegionCount,
                          Target->RegionCount) ||
        !TargetRangeValid(Controller->RangeOffset, Controller->RangeCount,
                          Target->RangeCount) ||
        !TargetRangeValid(Controller->ClockOffset, Controller->ClockCount,
                          Target->ClockCount) ||
        !TargetRangeValid(Controller->ResetOffset, Controller->ResetCount,
                          Target->ResetCount) ||
        !TargetRangeValid(Controller->SupplyOffset, Controller->SupplyCount,
                          Target->SupplyCount) ||
        !TargetRangeValid(Controller->GpioOffset, Controller->GpioCount,
                          Target->GpioCount) ||
        !TargetRangeValid(Controller->IommuMapOffset,
                          Controller->IommuMapCount,
                          Target->IommuMapCount) ||
        !TargetRangeValid(Controller->InterruptOffset,
                          Controller->InterruptCount,
                          Target->InterruptCount) ||
        !TargetRangeValid(Controller->IntxOffset, Controller->IntxCount,
                          Target->InterruptCount) ||
        Controller->Lanes == 0 || Controller->Lanes > 2) {
      return CR_INVALID_PARAMETER;
    }
    for (Inner = 0; Inner < Index; Inner++) {
      if (Target->Controllers[Inner].Domain == Controller->Domain) {
        return CR_INVALID_PARAMETER;
      }
    }
    Phy = &Target->Phys[Controller->PhyIndex];
    if (Phy->Lanes != Controller->Lanes) {
      return CR_INVALID_PARAMETER;
    }
    Parf = FindRegion(Target, Controller, "parf");
    Dbi = FindRegion(Target, Controller, "dbi");
    Atu = FindRegion(Target, Controller, "atu");
    Config = FindRegion(Target, Controller, "config");
    if (Parf == NULL || Dbi == NULL || Atu == NULL || Config == NULL ||
        Parf->Size < 0x3000 || Dbi->Size < 0x900 ||
        Atu->Size <
            (UINT64)(Controller->RangeCount + 1U) * PCIE_ATU_REGION_STRIDE ||
        Config->Size < 0x100000 ||
        Config->Base < Dbi->Base ||
        Config->Base - Dbi->Base > ~0ULL - Config->Size ||
        ((Controller->PowerDomainController == NULL) !=
         (Controller->PowerDomainId == NULL)) ||
        (Controller->PowerDomainId != NULL && Io->SetPowerDomain == NULL) ||
        (Controller->SupplyCount != 0 && Io->SetSupply == NULL)) {
      return CR_INVALID_PARAMETER;
    }

    RangeTypes = 0;
    for (Inner = 0; Inner < Controller->RangeCount; Inner++) {
      CONST PcieTargetRange *Range =
          &Target->Ranges[Controller->RangeOffset + Inner];
      UINT8 TypeBit = (UINT8)BIT(Range->Type);

      if ((RangeTypes & TypeBit) != 0) {
        return CR_INVALID_PARAMETER;
      }
      RangeTypes |= TypeBit;
    }

    PerstCount = 0;
    for (Inner = 0; Inner < Controller->GpioCount; Inner++) {
      CONST PcieTargetGpio *Gpio =
          &Target->Gpios[Controller->GpioOffset + Inner];
      if (cr_strcmp(Gpio->Role, "perst") == 0) {
        PerstCount++;
      } else if (cr_strcmp(Gpio->Role, "wake") != 0 &&
                 cr_strcmp(Gpio->Role, "clkreq") != 0 &&
                 cr_strcmp(Gpio->Role, "enable") != 0) {
        return CR_INVALID_PARAMETER;
      }
    }
    if (PerstCount != 1) {
      return CR_INVALID_PARAMETER;
    }

    if (Controller->IommuMapCount != 0) {
      CONST PcieTargetIommuMap *FirstMap =
          &Target->IommuMaps[Controller->IommuMapOffset];
      for (Inner = 0; Inner < Controller->IommuMapCount; Inner++) {
        CONST PcieTargetIommuMap *Map =
            &Target->IommuMaps[Controller->IommuMapOffset + Inner];
        if (cr_strcmp(Map->Controller, FirstMap->Controller) != 0 ||
            Map->StreamId < FirstMap->StreamId ||
            Map->StreamId - FirstMap->StreamId > 0xFFU) {
          return CR_INVALID_PARAMETER;
        }
      }
    }
  }
  return CR_SUCCESS;
}

/* PIPE is supplied by the QMP PLL. Its mux/gate must stay off until the
 * PHY register tables have been programmed, as in qmp_pcie_power_on(). */
STATIC BOOLEAN
IsPipeClock(IN CONST PcieTargetClock *Clock)
{
  return cr_strcmp(Clock->Name, "pipe") == 0 ||
         cr_strcmp(Clock->Name, "pipe_mux") == 0 ||
         cr_strcmp(Clock->Name, "phy_pipe") == 0;
}

STATIC CR_STATUS
SetClockRange(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port,
    IN UINT16 Offset, IN UINT16 Count, IN BOOLEAN Enable,
    IN BOOLEAN PipePhase)
{
  UINT16 Index;
  UINT16 Resource;
  CR_STATUS Status;
  CR_STATUS FirstError = CR_SUCCESS;

  for (Index = 0; Index < Count; Index++) {
    Resource = Enable ? Offset + Index : Offset + Count - Index - 1U;
    if (Enable) {
      if (IsPipeClock(&Context->Target->Clocks[Resource]) != PipePhase ||
          (Port->ClockOwnedMask & (1ULL << Resource)) != 0) {
        continue;
      }
      /* The owner retains a failed write for the one common rollback path. */
      Port->ClockOwnedMask |= 1ULL << Resource;
    } else if ((Port->ClockOwnedMask & (1ULL << Resource)) == 0) {
      continue;
    }
    Status = Context->Io.SetClock(
        Context->Io.Context, &Context->Target->Clocks[Resource], Enable);
    if (CR_ERROR(Status)) {
      /* Keep the resource identity in the trace.  The host adapter maps the
       * Linux clock name to a native GCC id, and a generic EFI_NOT_FOUND at
       * this point otherwise gives no indication which vote was missing. */
      log_err(
          "PCIe: clock %u (%a, controller=%a, id=%a) %a failed, phase=%a, Status=0x%X",
          Resource, Context->Target->Clocks[Resource].Name,
          Context->Target->Clocks[Resource].Controller,
          Context->Target->Clocks[Resource].Id, Enable ? "enable" : "disable",
          PipePhase ? "pipe" : "base", Status);
      if (Enable) {
        return Status;
      }
      if (!CR_ERROR(FirstError)) {
        FirstError = Status;
      }
    } else if (!Enable) {
      Port->ClockOwnedMask &= ~(1ULL << Resource);
    }
  }
  return FirstError;
}

STATIC CR_STATUS
SetSupplyRange(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port,
    IN UINT16 Offset, IN UINT16 Count, IN BOOLEAN Enable)
{
  UINT16 Index;
  UINT16 Resource;
  CR_STATUS Status;
  CR_STATUS FirstError = CR_SUCCESS;

  for (Index = 0; Index < Count; Index++) {
    Resource = Enable ? Offset + Index : Offset + Count - Index - 1U;
    if (Enable) {
      if ((Port->SupplyOwnedMask & (1ULL << Resource)) != 0) {
        continue;
      }
      Port->SupplyOwnedMask |= 1ULL << Resource;
    } else if ((Port->SupplyOwnedMask & (1ULL << Resource)) == 0) {
      continue;
    }
    Status = Context->Io.SetSupply(
        Context->Io.Context, &Context->Target->Supplies[Resource], Enable);
    if (CR_ERROR(Status)) {
      log_err(
          "PCIe: supply %u (%a) %a failed, Status=0x%X",
          Resource, Context->Target->Supplies[Resource].Id,
          Enable ? "enable" : "disable", Status);
      if (Enable) {
        return Status;
      }
      if (!CR_ERROR(FirstError)) {
        FirstError = Status;
      }
    } else if (!Enable) {
      Port->SupplyOwnedMask &= ~(1ULL << Resource);
    }
  }
  return FirstError;
}

STATIC CR_STATUS
SetResetRange(
    IN PcieDeviceContext *Context, IN UINT16 Offset, IN UINT16 Count,
    IN BOOLEAN Assert)
{
  UINT16 Index;
  CR_STATUS Status;

  for (Index = 0; Index < Count; Index++) {
    Status = Context->Io.SetReset(
        Context->Io.Context, &Context->Target->Resets[Offset + Index], Assert);
    if (CR_ERROR(Status)) {
      log_err(
          "PCIe: reset %u (%a, controller=%a, id=%a) %a failed, Status=0x%X",
          Offset + Index, Context->Target->Resets[Offset + Index].Name,
          Context->Target->Resets[Offset + Index].Controller,
          Context->Target->Resets[Offset + Index].Id,
          Assert ? "assert" : "deassert", Status);
      return Status;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
ConfigureGpios(
    IN PcieDeviceContext *Context, IN PciePortRuntime *Port,
    IN BOOLEAN AssertPerst)
{
  CONST PcieTargetController *Controller = Port->Target;
  UINT16 Index;

  for (Index = 0; Index < Controller->GpioCount; Index++) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index];
    CR_STATUS Status;

    if (cr_strcmp(Gpio->Role, "wake") == 0 ||
        cr_strcmp(Gpio->Role, "clkreq") == 0) {
      Status = Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_INPUT, FALSE);
    } else if (cr_strcmp(Gpio->Role, "perst") == 0) {
      Status = Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, AssertPerst);
    } else if (cr_strcmp(Gpio->Role, "enable") == 0) {
      Status = Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, TRUE);
    } else {
      return CR_UNSUPPORTED;
    }
    if (CR_ERROR(Status)) {
      log_err(
          "PCIe: GPIO %a (controller=%a, pin=%u) configure failed, Status=0x%X",
          Gpio->Role, Gpio->Controller, Gpio->Pin, Status);
      return Status;
    }
    if (cr_strcmp(Gpio->Role, "perst") == 0 && Context->Io.GetTimeUs != NULL) {
      UINT64 Now = Context->Io.GetTimeUs(Context->Io.Context);
      if (AssertPerst) {
        Port->PerstAssertTimeUs = Now;
        Port->TimingValidMask |= 1U;
      } else {
        Port->PerstReleaseTimeUs = Now;
        Port->TimingValidMask |= 2U;
      }
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
SetPerst(
    IN PcieDeviceContext *Context, IN PciePortRuntime *Port,
    IN BOOLEAN Assert)
{
  CONST PcieTargetController *Controller = Port->Target;
  UINT16 Index;
  CR_STATUS Status;

  for (Index = 0; Index < Controller->GpioCount; Index++) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index];
    if (cr_strcmp(Gpio->Role, "perst") == 0) {
      Status = Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, Assert);
      if (CR_ERROR(Status)) {
        log_err("PCIe: PERST GPIO %u (%a) %a failed, Status=0x%X",
                Gpio->Pin, Gpio->Role, Assert ? "assert" : "deassert",
                Status);
      } else if (Context->Io.GetTimeUs != NULL) {
        UINT64 Now = Context->Io.GetTimeUs(Context->Io.Context);
        if (Assert) {
          Port->PerstAssertTimeUs = Now;
          Port->TimingValidMask |= 1U;
        } else {
          Port->PerstReleaseTimeUs = Now;
          Port->TimingValidMask |= 2U;
        }
      }
      return Status;
    }
  }
  return CR_NOT_FOUND;
}

STATIC CR_STATUS
ReleaseGpios(
    IN PcieDeviceContext *Context, IN CONST PcieTargetController *Controller)
{
  UINT16 Index;
  CR_STATUS Status;
  CR_STATUS FirstError;

  FirstError = CR_SUCCESS;

  for (Index = Controller->GpioCount; Index > 0; Index--) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index - 1U];

    /* PERST is asserted by ReleasePortResources before SMMU revocation.
     * Keep it asserted while the remaining GPIOs are released, so a failed
     * enable-line operation cannot reopen the endpoint during teardown. */
    if (cr_strcmp(Gpio->Role, "enable") == 0) {
      Status = Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, FALSE);
      if (CR_ERROR (Status) && !CR_ERROR (FirstError)) {
        log_err(
            "PCIe: GPIO %a (controller=%a, pin=%u) release failed, Status=0x%X",
            Gpio->Role, Gpio->Controller, Gpio->Pin, Status);
        FirstError = Status;
      }
    }
  }
  return FirstError;
}

STATIC UINT64
PhyBlockBase(IN CONST PcieTargetPhy *Phy, IN PCIE_PHY_BLOCK Block)
{
  switch (Block) {
    case PCIE_PHY_BLOCK_SERDES:
      return Phy->Base + Phy->BlockOffsets.Serdes;
    case PCIE_PHY_BLOCK_TX:
      return Phy->Base + Phy->BlockOffsets.Tx;
    case PCIE_PHY_BLOCK_RX:
      return Phy->Base + Phy->BlockOffsets.Rx;
    case PCIE_PHY_BLOCK_PCS:
      return Phy->Base + Phy->BlockOffsets.Pcs;
    case PCIE_PHY_BLOCK_PCS_MISC:
      return Phy->Base + Phy->BlockOffsets.PcsMisc;
    default:
      return 0;
  }
}

STATIC CR_STATUS
ProgramPhyTable(
    IN PcieDeviceContext *Context, IN CONST PcieTargetPhy *Phy,
    IN PCIE_PHY_INIT_PHASE Phase)
{
  UINT16 Index;

  for (Index = 0; Index < Phy->InitCount; Index++) {
    CONST PciePhyInitEntry *Entry =
        &Context->Target->PhyInit[Phy->InitOffset + Index];
    UINT64 Base;

    if (Entry->Phase != Phase) {
      continue;
    }
    Base = PhyBlockBase(Phy, Entry->Block);
    if (Base == 0 || Base < Phy->Base ||
        Base - Phy->Base > Phy->Size ||
        Entry->Offset > Phy->Size - (Base - Phy->Base) ||
        sizeof(UINT32) >
            Phy->Size - (Base - Phy->Base) - Entry->Offset) {
      return CR_INVALID_PARAMETER;
    }
    if (Entry->Block == PCIE_PHY_BLOCK_TX ||
        Entry->Block == PCIE_PHY_BLOCK_RX) {
      if ((Entry->LaneMask & BIT(0)) != 0) {
        PcieWrite32(Context, Base + Entry->Offset, Entry->Value);
      }
      if (Phy->Lanes >= 2 && (Entry->LaneMask & BIT(1)) != 0) {
        Base = Phy->Base +
            (Entry->Block == PCIE_PHY_BLOCK_TX
                 ? Phy->BlockOffsets.Tx2
                 : Phy->BlockOffsets.Rx2);
        if (Base == Phy->Base || Base < Phy->Base ||
            Base - Phy->Base > Phy->Size ||
            Entry->Offset > Phy->Size - (Base - Phy->Base) ||
            sizeof(UINT32) >
                Phy->Size - (Base - Phy->Base) - Entry->Offset) {
          return CR_INVALID_PARAMETER;
        }
        PcieWrite32(Context, Base + Entry->Offset, Entry->Value);
      }
    } else {
      PcieWrite32(Context, Base + Entry->Offset, Entry->Value);
    }
  }
  return CR_SUCCESS;
}

STATIC UINT32
LogPhyConfiguration(IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  CONST PcieTargetPhy *Phy = Port->Phy;
  UINT16 Index, Next;
  UINT32 Checked = 0, Mismatches = 0;

  /* Compare only the last programmed value at each configuration address.
   * BASE entries may be intentionally overridden by the RC phase.  This
   * bounded, read-only timeout diagnostic includes both physical lanes. */
  for (Index = 0; Index < Phy->InitCount; Index++) {
    CONST PciePhyInitEntry *Entry =
        &Context->Target->PhyInit[Phy->InitOffset + Index];
    BOOLEAN LaneBlock = Entry->Block == PCIE_PHY_BLOCK_TX ||
                        Entry->Block == PCIE_PHY_BLOCK_RX;
    UINT8 Lane;
    for (Lane = 0; Lane < (LaneBlock ? Phy->Lanes : 1U); Lane++) {
      BOOLEAN Overridden = FALSE;
      UINT64 Address;
      UINT32 Actual;
      if (LaneBlock && (Entry->LaneMask & BIT(Lane)) == 0) continue;
      for (Next = 0; Next < Phy->InitCount; Next++) {
        CONST PciePhyInitEntry *Later =
            &Context->Target->PhyInit[Phy->InitOffset + Next];
        if (Later->Block == Entry->Block && Later->Offset == Entry->Offset &&
            (!LaneBlock || (Later->LaneMask & BIT(Lane)) != 0) &&
            (Later->Phase > Entry->Phase ||
             (Later->Phase == Entry->Phase && Next > Index))) {
          Overridden = TRUE;
          break;
        }
      }
      if (Overridden) continue;
      Address = Lane == 0 ? PhyBlockBase(Phy, Entry->Block) : Phy->Base +
          (Entry->Block == PCIE_PHY_BLOCK_TX ? Phy->BlockOffsets.Tx2 :
                                             Phy->BlockOffsets.Rx2);
      Address += Entry->Offset;
      Actual = PcieRead32(Context, Address);
      ++Checked;
      if (Actual != Entry->Value) {
        if (Mismatches < 8U)
          log_warn("PCIe: port %u PHY config +0x%X expected=0x%X actual=0x%X",
                   Port->Target->Domain, (UINT32)(Address - Phy->Base),
                   Entry->Value, Actual);
        ++Mismatches;
      }
    }
  }
  log_warn("PCIe: port %u PHY configuration readback checked=%u mismatches=%u",
           Port->Target->Domain, Checked, Mismatches);
  return Mismatches;
}

STATIC CR_STATUS
InitializePhy(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port)
{
  CONST PcieTargetPhy *Phy = Port->Phy;
  UINT64 Pcs = Phy->Base + Phy->BlockOffsets.Pcs;
  UINT32 Elapsed;
  CR_STATUS Status;

  if (Phy->AuxMuxId != NULL) {
    /* Public Qualcomm pci-msm.c keeps AUX on XO until pcie_phy_init has
       completed. The PHY-derived parent cannot satisfy a cold gate poll. */
    Port->AuxClockParentManaged = TRUE;
    Status = Context->Io.SetClockParent(
        Context->Io.Context, Phy, Phy->AuxReferenceSource);
    if (CR_ERROR(Status)) {
      log_err("PCIe: PHY %a AUX reference source=%u failed, Status=0x%X",
              Phy->Label, Phy->AuxReferenceSource, Status);
      return Status;
    }
    log_info("PCIe: PHY %a AUX reference source=%u selected",
             Phy->Label, Phy->AuxReferenceSource);
  }
  Port->PhyResetsManaged = TRUE;
  Status = SetResetRange(
      Context, Phy->ResetOffset, Phy->ResetCount, TRUE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a reset assert failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_PHY_RESET_DELAY_US);
  Status = SetResetRange(
      Context, Phy->ResetOffset, Phy->ResetCount, FALSE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a reset deassert failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }

  Port->PhyClocksEnabled = TRUE;
  Status = SetClockRange(
      Context, Port, Phy->ClockOffset, Phy->ClockCount, TRUE, FALSE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a base clocks failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }

  PcieRmw32(
      Context, Pcs + Phy->PowerDownControl, 0,
      Phy->PowerDownControlValue);
  Status = ProgramPhyTable(Context, Phy, PCIE_PHY_PHASE_BASE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a base register table failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }
  Status = ProgramPhyTable(Context, Phy, PCIE_PHY_PHASE_RC);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a RC register table failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }
  Status = SetClockRange(
      Context, Port, Port->Target->ClockOffset, Port->Target->ClockCount,
      TRUE, TRUE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a controller pipe clocks failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }
  Status = SetClockRange(
      Context, Port, Phy->ClockOffset, Phy->ClockCount, TRUE, TRUE);
  if (CR_ERROR(Status)) {
    log_err("PCIe: PHY %a PHY pipe clocks failed, Status=0x%X",
            Phy->Label, Status);
    return Status;
  }
  PcieRmw32(Context, Pcs + Phy->SwReset, PCIE_QMP_SW_RESET, 0);
  PcieRmw32(
      Context, Pcs + Phy->StartControl, 0,
      PCIE_QMP_SERDES_START | PCIE_QMP_PCS_START);
  Context->Io.DelayUs(Context->Io.Context, PCIE_PHY_START_DELAY_US);

  for (Elapsed = 0; Elapsed < PCIE_PHY_TIMEOUT_US;
       Elapsed += PCIE_PHY_POLL_DELAY_US) {
    if ((PcieRead32(Context, Pcs + Phy->Status) & Phy->StatusMask) == 0) {
      if (Phy->AuxMuxId != NULL) {
        Status = Context->Io.SetClockParent(
            Context->Io.Context, Phy, Phy->AuxPhySource);
        if (CR_ERROR(Status)) {
          log_err("PCIe: PHY %a AUX PHY source=%u failed, Status=0x%X",
                  Phy->Label, Phy->AuxPhySource, Status);
          return Status;
        }
        log_info("PCIe: PHY %a ready; AUX PHY source=%u selected",
                 Phy->Label, Phy->AuxPhySource);
      }
      return CR_SUCCESS;
    }
    Context->Io.DelayUs(Context->Io.Context, PCIE_PHY_POLL_DELAY_US);
  }
  log_err("PCIe: PHY %a ready timeout after %u us, Status=0x%X",
          Phy->Label, PCIE_PHY_TIMEOUT_US, CR_TIMEOUT);
  return CR_TIMEOUT;
}

STATIC VOID
ConfigureParf(IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_DEVICE_TYPE,
      PCIE_PARF_DEVICE_TYPE_RC);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_PHY_CTRL,
      (UINT32)PCIE_PARF_PHY_TEST_POWER_DOWN, 0);
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_DBI_BASE_ADDR_V2,
      (UINT32)Port->DbiBase);
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_DBI_BASE_ADDR_V2_HI,
      (UINT32)(Port->DbiBase >> 32));
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_ATU_BASE_ADDR,
      (UINT32)Port->AtuBase);
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_ATU_BASE_ADDR_HI,
      (UINT32)(Port->AtuBase >> 32));
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_SLV_ADDR_SPACE_SIZE_V2, 0);
  PcieWrite32(
      Context, Port->ParfBase + PCIE_PARF_SLV_ADDR_SPACE_SIZE_V2_HI,
      0x80000000U);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_SYS_CTRL,
      (UINT32)PCIE_PARF_MAC_PHY_POWERDOWN_MUX,
      Port->Target->ParfSysControlSetMask);
  log_info("PCIe: port %u PARF clock controls=0x%X CLKREQ-override=0x%X",
           Port->Target->Domain,
           PcieRead32(Context, Port->ParfBase + PCIE_PARF_SYS_CTRL),
           PcieRead32(Context, Port->ParfBase + PCIE_PARF_CLKREQ_OVERRIDE));
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_MHI_CLOCK_RESET_CTRL, 0,
      (UINT32)PCIE_PARF_MHI_BYPASS);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_PM_CTRL,
      (UINT32)PCIE_PARF_REQ_NOT_ENTER_L1, 0);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_AXI_MSTR_WR_ADDR_HALT_V2, 0,
      (UINT32)PCIE_PARF_AXI_HALT_ENABLE);

}

STATIC UINT32
PortLinkMode(IN UINT8 Lanes)
{
  return Lanes == 2 ? (3U << 16) : (1U << 16);
}

STATIC VOID
ConfigureRootPort(IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  UINT32 Value;
  UINT32 BusNumbers;
  UINT8 Capability;
  UINT16 Guard;
  UINT8 SecondaryBus;

  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_MISC_CONTROL_1, 0,
      (UINT32)PCIE_DBI_RO_WRITE_ENABLE);

  Value = PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_LINK_CONTROL);
  Value &= ~((UINT32)PCIE_DBI_PORT_FAST_LINK_MODE |
             (UINT32)PCIE_DBI_PORT_LINK_MODE_MASK);
  /* Qualcomm's 1.9.0 sequence explicitly enables the Data Link Layer
   * before LTSSM is started.  Leaving DLL_LINK_EN clear leaves the root port
   * in Detect/Detect.Quiet even when the PHY and endpoint are powered. */
  Value |= (UINT32)PCIE_DBI_PORT_LINK_DLL_ENABLE |
           PortLinkMode(Port->Target->Lanes);
  PcieWrite32(
      Context, Port->DbiBase + PCIE_DBI_PORT_LINK_CONTROL, Value);

  Value = PcieRead32(
      Context, Port->DbiBase + PCIE_DBI_LINK_WIDTH_SPEED_CONTROL);
  Value &= ~(UINT32)PCIE_DBI_LINK_WIDTH_MASK;
  Value |= ((UINT32)Port->Target->Lanes << 8) |
           (UINT32)PCIE_DBI_SPEED_CHANGE;
  PcieWrite32(
      Context, Port->DbiBase + PCIE_DBI_LINK_WIDTH_SPEED_CONTROL, Value);

  PcieWrite32(Context, Port->DbiBase + PCIE_DBI_BAR0, 0);
  PcieWrite32(Context, Port->DbiBase + PCIE_DBI_BAR1, 0);
  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_INTERRUPT_LINE, 0x0000FF00U,
      0x00000100U);

  /* Linux assigns the first downstream bus immediately after the root bus.
   * Keep the primary/secondary/subordinate fields derived from target data so
   * a platform using a non-zero bus aperture is initialized consistently.  A
   * root-only aperture is valid as well; in that case leave secondary equal
   * to primary instead of wrapping BusStart + 1 at 0xff. */
  SecondaryBus = Port->Target->BusStart;
  if (Port->Target->BusStart < Port->Target->BusEnd) {
    SecondaryBus = (UINT8)(Port->Target->BusStart + 1U);
  }
  BusNumbers = ((UINT32)Port->Target->BusEnd << 16) |
               ((UINT32)SecondaryBus << 8) |
               (UINT32)Port->Target->BusStart;
  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_PRIMARY_BUS, 0x00FFFFFFU,
      BusNumbers);
  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_COMMAND_STATUS, 0x0000FFFFU,
      (UINT32)(PCIE_DBI_COMMAND_IO | PCIE_DBI_COMMAND_MEMORY |
               PCIE_DBI_COMMAND_MASTER | PCIE_DBI_COMMAND_SERR));
  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_CLASS_REVISION, 0xFFFF0000U,
      PCIE_DBI_CLASS_BRIDGE_PCI << 16);

  Capability = PcieRead8(
      Context, Port->DbiBase + PCIE_DBI_CAPABILITY_LIST) &
      PCIE_CAP_NEXT_MASK;
  Guard = 0;
  while (Capability >= 0x40U && Guard++ < 48U) {
    UINT8 Id = PcieRead8(Context, Port->DbiBase + Capability);
    if (Id == PCIE_CAP_ID_EXPRESS) {
      /* Advertise the physical lane count at the capability-relative
       * Link Capabilities offset.  Writing DBI+0x0c would touch unrelated
       * root-port registers because the PCIe capability is not fixed. */
      PcieRmw32(
          Context, Port->DbiBase + Capability + PCIE_EXP_LINK_CAPABILITIES,
          (UINT32)PCIE_EXP_LINK_CAP_WIDTH_MASK,
          (UINT32)Port->Target->Lanes << 4);
      if (Port->Target->MaxLinkSpeed != 0) {
        PcieRmw32(
            Context, Port->DbiBase + Capability + PCIE_EXP_LINK_CAPABILITIES,
            0xFU, Port->Target->MaxLinkSpeed);
        PcieRmw32(
            Context, Port->DbiBase + Capability + PCIE_EXP_LINK_CONTROL2,
            0xFU, Port->Target->MaxLinkSpeed);
      }
      PcieRmw32(
          Context,
          Port->DbiBase + Capability + PCIE_EXP_SLOT_CAPABILITIES, 0,
          (UINT32)PCIE_EXP_SLOT_NO_COMMAND_COMPLETED);
      /* Qualcomm root ports cannot advertise an attention button.  Linux
       * clears ABP while retaining the no-command-completion workaround. */
      PcieRmw32(
          Context, Port->DbiBase + Capability + PCIE_EXP_SLOT_CAPABILITIES,
          (UINT32)PCIE_EXP_SLOT_ATTENTION_BUTTON, 0);
      break;
    }
    Capability = PcieRead8(Context, Port->DbiBase + Capability + 1U) &
                 PCIE_CAP_NEXT_MASK;
  }

  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_MISC_CONTROL_1,
      (UINT32)PCIE_DBI_RO_WRITE_ENABLE, 0);
}

STATIC VOID
ConfigureEqualization(
    IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  UINT8 Speed;
  UINT32 Value;

  /* Gen1/Gen2 links do not have the Gen3 equalization phases.  Qualcomm's
   * common DWC helper programs one shadow set for every supported rate from
   * 8.0 GT/s through the requested maximum; this is the same operation for
   * a Gen3 target and remains harmless for a Gen4 target's Gen3 shadow set. */
  if (Port->Target->MaxLinkSpeed < 3) {
    return;
  }
  for (Speed = 3; Speed <= Port->Target->MaxLinkSpeed && Speed <= 4;
       Speed++) {
    Value = PcieRead32(Context, Port->DbiBase + PCIE_DBI_GEN3_RELATED);
    Value &= ~PCIE_GEN3_ZRXDC_NONCOMPL;
    Value &= ~PCIE_GEN3_RATE_SHADOW_SEL;
    Value |= ((UINT32)(Speed - 3U) << 24);
    PcieWrite32(Context, Port->DbiBase + PCIE_DBI_GEN3_RELATED, Value);

    Value = PcieRead32(
        Context, Port->DbiBase + PCIE_DBI_GEN3_EQ_FB_MODE_DIR_CHANGE);
    Value &= ~(PCIE_GEN3_FMDC_T_MIN_PHASE23 |
               PCIE_GEN3_FMDC_N_EVALS |
               PCIE_GEN3_FMDC_MAX_PRE_CURSOR_DELTA |
               PCIE_GEN3_FMDC_MAX_POST_CURSOR_DELTA);
    Value |= 1U | (0xDU << 5) | (5U << 10) | (5U << 14);
    PcieWrite32(
        Context, Port->DbiBase + PCIE_DBI_GEN3_EQ_FB_MODE_DIR_CHANGE, Value);

    Value = PcieRead32(Context, Port->DbiBase + PCIE_DBI_GEN3_EQ_CONTROL);
    Value &= ~(PCIE_GEN3_EQ_FB_MODE |
               PCIE_GEN3_EQ_PHASE23_EXIT_MODE |
               PCIE_GEN3_EQ_FOM_INC_INITIAL_EVAL |
               PCIE_GEN3_EQ_PSET_REQ_VEC);
    PcieWrite32(Context, Port->DbiBase + PCIE_DBI_GEN3_EQ_CONTROL, Value);
  }
  CrMemoryFence();
}

STATIC CR_STATUS
ProgramAtuRegion(
    IN PcieDeviceContext *Context, IN PciePortRuntime *Port, IN UINT16 Index,
    IN UINT32 Type, IN UINT64 CpuBase, IN UINT64 PciBase, IN UINT64 Size,
    IN UINT32 Control2)
{
  UINT64 Base;
  UINT64 Limit;
  UINT32 Retry;

  if (Size == 0 || CpuBase + Size - 1 < CpuBase ||
      PciBase + Size - 1 < PciBase) {
    return CR_INVALID_PARAMETER;
  }
  Limit = CpuBase + Size - 1;
  if ((CpuBase >> 32) != (Limit >> 32)) {
    return CR_UNSUPPORTED;
  }
  Base = Port->AtuBase + (UINT64)Index * PCIE_ATU_REGION_STRIDE;
  PcieWrite32(Context, Base + PCIE_ATU_LOWER_BASE, (UINT32)CpuBase);
  PcieWrite32(Context, Base + PCIE_ATU_UPPER_BASE, (UINT32)(CpuBase >> 32));
  PcieWrite32(Context, Base + PCIE_ATU_LIMIT, (UINT32)Limit);
  PcieWrite32(Context, Base + PCIE_ATU_LOWER_TARGET, (UINT32)PciBase);
  PcieWrite32(
      Context, Base + PCIE_ATU_UPPER_TARGET, (UINT32)(PciBase >> 32));
  PcieWrite32(Context, Base + PCIE_ATU_REGION_CTRL1, Type);
  PcieWrite32(
      Context, Base + PCIE_ATU_REGION_CTRL2,
      (UINT32)PCIE_ATU_ENABLE | Control2);

  for (Retry = 0; Retry < PCIE_ATU_RETRIES; Retry++) {
    if ((PcieRead32(Context, Base + PCIE_ATU_REGION_CTRL2) &
         PCIE_ATU_ENABLE) != 0) {
      return CR_SUCCESS;
    }
    Context->Io.DelayUs(Context->Io.Context, PCIE_ATU_RETRY_DELAY_US);
  }
  return CR_TIMEOUT;
}

STATIC CR_STATUS
ConfigureAtu(IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  UINT16 Index;
  CR_STATUS Status;

  Status = ProgramAtuRegion(
      Context, Port, 0, PCIE_ATU_TYPE_CFG0, Port->ConfigBase, 0,
      Port->ConfigSize, 0);
  if (CR_ERROR(Status)) {
    return Status;
  }
  for (Index = 0; Index < Port->Target->RangeCount; Index++) {
    CONST PcieTargetRange *Range =
        &Context->Target->Ranges[Port->Target->RangeOffset + Index];
    UINT32 Type = Range->Type == PCIE_RANGE_IO
                      ? PCIE_ATU_TYPE_IO
                      : PCIE_ATU_TYPE_MEMORY;
    Status = ProgramAtuRegion(
        Context, Port, (UINT16)(Index + 1), Type, Range->CpuBase,
        Range->PciBase, Range->Size, 0);
    if (CR_ERROR(Status)) {
      return Status;
    }
  }
  return CR_SUCCESS;
}

STATIC UINT8
Crc8Byte(IN UINT8 Crc, IN UINT8 Data)
{
  UINT8 BitIndex;

  Crc ^= Data;
  for (BitIndex = 0; BitIndex < 8; BitIndex++) {
    Crc = (UINT8)((Crc << 1) ^
                  ((Crc & 0x80U) != 0 ? PCIE_BDF_SID_CRC_POLYNOMIAL : 0));
  }
  return Crc;
}

STATIC UINT8
BdfHash(IN UINT16 Bdf)
{
  UINT8 Hash = Crc8Byte(0, (UINT8)(Bdf >> 8));
  return Crc8Byte(Hash, (UINT8)Bdf);
}

STATIC CR_STATUS
ConfigureIommuMap(IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  CONST PcieTargetController *Controller = Port->Target;
  UINT32 SidBase;
  UINT16 Index;

  if (Controller->IommuMapCount == 0) {
    return CR_SUCCESS;
  }
  SidBase = Context->Target->IommuMaps[Controller->IommuMapOffset].StreamId;
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_BDF_TO_SID_CFG,
      (UINT32)PCIE_PARF_BDF_TO_SID_BYPASS, 0);
  for (Index = 0; Index < PCIE_BDF_SID_ENTRIES; Index++) {
    PcieWrite32(
        Context, Port->ParfBase + PCIE_PARF_BDF_TO_SID_TABLE +
                     (UINT64)Index * sizeof(UINT32),
        0);
  }

  for (Index = 0; Index < Controller->IommuMapCount; Index++) {
    CONST PcieTargetIommuMap *Map =
        &Context->Target->IommuMaps[Controller->IommuMapOffset + Index];
    UINT8 Hash;
    UINT16 Probes;
    UINT32 Value;

    if (Map->StreamId < SidBase || Map->StreamId - SidBase > 0xFFU) {
      log_err(
          "PCIe: port IOMMU map BDF=0x%X stream=%u outside SID base=%u, Status=0x%X",
          Map->Bdf, Map->StreamId, SidBase, CR_INVALID_PARAMETER);
      return CR_INVALID_PARAMETER;
    }
    Hash = BdfHash(Map->Bdf);
    for (Probes = 0; Probes < PCIE_BDF_SID_ENTRIES; Probes++) {
      UINT64 Address = Port->ParfBase + PCIE_PARF_BDF_TO_SID_TABLE +
                       (UINT64)Hash * sizeof(UINT32);
      Value = PcieRead32(Context, Address);
      if (Value == 0) {
        Value = ((UINT32)Map->Bdf << 16) |
                ((Map->StreamId - SidBase) << 8);
        PcieWrite32(Context, Address, Value);
        break;
      }
      if ((Value & 0xFFU) == 0) {
        PcieWrite32(Context, Address, Value | (UINT8)(Hash + 1U));
      }
      Hash++;
    }
    if (Probes == PCIE_BDF_SID_ENTRIES) {
      log_err(
          "PCIe: port IOMMU map BDF=0x%X SID table full, Status=0x%X",
          Map->Bdf, CR_OUT_OF_RESOURCES);
      return CR_OUT_OF_RESOURCES;
    }
  }
  return CR_SUCCESS;
}

STATIC UINT8
FindExpressCapability(
    IN PcieDeviceContext *Context, IN PciePortRuntime *Port)
{
  UINT8 Capability = PcieRead8(
      Context, Port->DbiBase + PCIE_DBI_CAPABILITY_LIST) &
      PCIE_CAP_NEXT_MASK;
  UINT16 Guard = 0;

  while (Capability >= 0x40U && Guard++ < 48U) {
    if (PcieRead8(Context, Port->DbiBase + Capability) ==
        PCIE_CAP_ID_EXPRESS) {
      return Capability;
    }
    Capability = PcieRead8(Context, Port->DbiBase + Capability + 1U) &
                 PCIE_CAP_NEXT_MASK;
  }
  return 0;
}

BOOLEAN
PcieIsLinkUp(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex)
{
  PciePortRuntime *Port;
  UINT8 Capability;
  BOOLEAN LinkUp;

  if (Context == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return FALSE;
  }
  Port = &Context->Ports[PortIndex];
  Capability = FindExpressCapability(Context, Port);
  LinkUp = Capability != 0 &&
      (PcieRead16(
           Context,
           Port->DbiBase + Capability + PCIE_EXP_LINK_STATUS) &
       PCIE_EXP_LINK_ACTIVE) != 0;
  if (LinkUp) {
    Context->LinkMask |= (UINT32)BIT(PortIndex);
    if (Port->State == PCIE_PORT_READY_NO_LINK) {
      Port->State = PCIE_PORT_LINK_UP;
      Port->LastStatus = CR_SUCCESS;
    }
  } else {
    Context->LinkMask &= ~(UINT32)BIT(PortIndex);
    if (Port->State == PCIE_PORT_LINK_UP) {
      Port->State = PCIE_PORT_READY_NO_LINK;
      Port->LastStatus = CR_TIMEOUT;
    }
  }
  return LinkUp;
}

CR_STATUS
PcieConfigAccess(
    IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex, IN UINT8 Bus,
    IN UINT8 Device, IN UINT8 Function, IN UINT16 Register, IN UINTN Width,
    IN BOOLEAN Write, IN OUT UINT32 *Value)
{
  PciePortRuntime *Port;
  UINT64           Address;
  UINT64           WindowOffset;
  UINT32           BusDev;
  UINT32           AtuType;
  BOOLEAN          RootPort;
  CR_STATUS        Status;

  if (Context == NULL || !Context->Initialized || Value == NULL ||
      PortIndex >= Context->Target->ControllerCount ||
      (Context->InitializedMask & BIT (PortIndex)) == 0 ||
      ((Width != 1) && (Width != 2) && (Width != 4)) ||
      (Register >= 0x1000U) || (Register > 0x1000U - Width) ||
      (Register & (Width - 1U)) != 0 || Device > 31U || Function > 7U) {
    return CR_INVALID_PARAMETER;
  }

  Port = &Context->Ports[PortIndex];
  if (Port->State == PCIE_PORT_OFF ||
      Port->State == PCIE_PORT_INITIALIZING ||
      Port->State == PCIE_PORT_FAILED) {
    return CR_NOT_FOUND;
  }
  if (Bus < Port->Target->BusStart || Bus > Port->Target->BusEnd) {
    return CR_INVALID_PARAMETER;
  }
  if ((Width == 1 && (Write ? Context->Io.Write8 == NULL :
                             Context->Io.Read8 == NULL)) ||
      (Width == 2 && (Write ? Context->Io.Write16 == NULL :
                             Context->Io.Read16 == NULL))) {
    return CR_UNSUPPORTED;
  }

  /* The DesignWare own-conf path exposes only function 0 of the root port.
   * A root-bus slot must not be sent as CFG0: there is no downstream bus
   * behind the root bus and Linux's dw_pcie_own_conf_map_bus() returns NULL
   * for this case. */
  if (Bus == Port->Target->BusStart && (Device != 0 || Function != 0)) {
    return CR_NOT_FOUND;
  }

  /* A PCIe root port has one Device 0 on its immediate downstream link.
   * Type-0 configuration ignores the Device field, so scanning other slots
   * can enumerate aliases of the same endpoint and multiply its BAR demand.
   * Linux's only_one_child() avoids these probes in the PCI scan layer; the
   * UEFI scanner probes all slots, so enforce the topology here instead.
   * Keep all functions of Device 0 and all Type-1 BDFs behind bridges. */
  if (Bus == (UINT8)(Port->Target->BusStart + 1U) && Device != 0) {
    return CR_NOT_FOUND;
  }

  RootPort = (Bus == Port->Target->BusStart) && (Device == 0) &&
             (Function == 0);
  if (RootPort) {
    WindowOffset = Register;
    if (Port->DbiSize < sizeof (UINT32) ||
        WindowOffset > Port->DbiSize - Width ||
        (WindowOffset & ~3ULL) > Port->DbiSize - sizeof (UINT32)) {
      return CR_INVALID_PARAMETER;
    }
    if (Port->DbiBase > ~0ULL - WindowOffset) {
      return CR_INVALID_PARAMETER;
    }
    Address = Port->DbiBase + WindowOffset;
  } else {
    /* SM8450 exposes a one-megabyte CPU-side CFG aperture.  The BDF is
     * carried by iATU region 0, so only the 4 KiB function offset is added
     * to ConfigBase. */
    WindowOffset = Register;
    if (Port->ConfigSize < sizeof (UINT32) ||
        WindowOffset > Port->ConfigSize - Width ||
        (WindowOffset & ~3ULL) > Port->ConfigSize - sizeof (UINT32)) {
      return CR_INVALID_PARAMETER;
    }
    if (Port->ConfigBase > ~0ULL - WindowOffset) {
      return CR_INVALID_PARAMETER;
    }
    Address = Port->ConfigBase + WindowOffset;
  }

  CrLockAcquire (&Context->ConfigLock);
  if (!RootPort) {
    /* Do not issue a CFG TLP while the link is down.  Apart from avoiding a
     * spurious SError this matches the DWC host driver's last-line check. */
    if (!PcieIsLinkUp (Context, PortIndex)) {
      CrLockRelease (&Context->ConfigLock);
      return CR_NOT_FOUND;
    }

    BusDev = PCIE_ATU_BUS (Bus) | PCIE_ATU_DEV (Device) |
             PCIE_ATU_FUNC (Function);
    /* The PCI configuration target is a 32-bit BDF address.  Guard the
     * addition before programming iATU so a larger-than-ECAM target window
     * cannot wrap at 0xffffffff for the last bus/device/function. */
    if (Port->ConfigSize == 0 ||
        Port->ConfigSize - 1U > (UINT64)CR_MAX_UINT32 - BusDev) {
      CrLockRelease (&Context->ConfigLock);
      return CR_INVALID_PARAMETER;
    }
    AtuType = (Bus == (UINT8)(Port->Target->BusStart + 1U))
                  ? PCIE_ATU_TYPE_CFG0
                  : PCIE_ATU_TYPE_CFG1;
    Status = ProgramAtuRegion (
        Context, Port, 0, AtuType, Port->ConfigBase, BusDev,
        /* BDF is in the target register, not ECAM address bits. */
        Port->ConfigSize, 0);
    if (CR_ERROR (Status)) {
      CrLockRelease (&Context->ConfigLock);
      return Status;
    }
  }

  if (Write) {
    if (Width == 1) {
      Context->Io.Write8 (Context->Io.Context, Address, (UINT8)*Value);
    } else if (Width == 2) {
      Context->Io.Write16 (Context->Io.Context, Address, (UINT16)*Value);
    } else {
      PcieWrite32 (Context, Address, *Value);
    }
    CrMemoryFence ();
  } else if (Width == 1) {
    *Value = Context->Io.Read8 (Context->Io.Context, Address);
  } else if (Width == 2) {
    *Value = Context->Io.Read16 (Context->Io.Context, Address);
  } else {
    *Value = PcieRead32 (Context, Address);
  }
  CrLockRelease (&Context->ConfigLock);
  return CR_SUCCESS;
}

STATIC BOOLEAN
PortResourcesOwned(IN CONST PciePortRuntime *Port)
{
  return Port != NULL &&
         (Port->ClockOwnedMask != 0 || Port->SupplyOwnedMask != 0 ||
          Port->GpiosConfigured || Port->SuppliesEnabled ||
          Port->PowerEnabled || Port->ControllerClocksEnabled ||
          Port->ControllerResetsManaged || Port->PhyClocksEnabled ||
          Port->PhyResetsManaged || Port->InterconnectEnabled ||
          Port->IommuAttached || Port->AuxClockParentManaged ||
          Port->EndpointPowerManaged);
}

STATIC CR_STATUS
ReleasePortResources(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port)
{
  CR_STATUS Status;

  /* Fence the endpoint before revoking its stream translation.  The SMMU
   * protocol requires BlockSegment() to run after the endpoint has stopped
   * issuing DMA; clearing LTSSM and asserting PERST provide that independent
   * fence while controller resources are still available. */
  if (Port->ControllerConfigured) {
    PcieRmw32(
        Context, Port->ParfBase + PCIE_PARF_LTSSM,
        (UINT32)PCIE_PARF_LTSSM_ENABLE, 0);
  }
  if (Port->GpiosConfigured) {
    Status = SetPerst (Context, Port, TRUE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage perst-assert failed, Status=0x%X", Status);
      return Status;
    }
  }

  /* Restore the reference before any PHY reset/gate/GDSC can remove the
     selected source. Failed restoration retains all prerequisites for retry. */
  if (Port->AuxClockParentManaged) {
    Status = Context->Io.SetClockParent(
        Context->Io.Context, Port->Phy, Port->Phy->AuxReferenceSource);
    if (CR_ERROR(Status)) {
      log_err("PCIe: cleanup stage aux-reference failed, Status=0x%X", Status);
      return Status;
    }
    Port->AuxClockParentManaged = FALSE;
  }

  /* Retaining IommuAttached on failure lets a caller retry instead of falsely
   * treating a still-owned stream as shut down. */
  if (Port->IommuAttached) {
    Status = Context->Io.SetIommu(
        Context->Io.Context, Port->Target, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage smmu-block failed, Status=0x%X", Status);
      return Status;
    }
    Port->IommuAttached = FALSE;
  }

  /* The board endpoint must stop before the controller/PHY supply references
     disappear. A partial power-on also reaches this path. Retain its state
     and all prerequisites if power-off needs a retry. */
  if (Port->EndpointPowerManaged) {
    Status = Context->Io.SetEndpointPower(&Context->Io, Port->Target, FALSE);
    if (CR_ERROR(Status)) {
      log_err("PCIe: cleanup stage endpoint-power-off failed, Status=0x%X", Status);
      return Status;
    }
    Port->EndpointPowerManaged = FALSE;
  }

  if (Port->GpiosConfigured) {
    Status = ReleaseGpios (Context, Port->Target);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage gpio-release failed, Status=0x%X", Status);
      return Status;
    } else {
      Port->GpiosConfigured = FALSE;
    }
  }
  if (Port->ControllerResetsManaged) {
    Status = SetResetRange(
        Context, Port->Target->ResetOffset, Port->Target->ResetCount, TRUE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage controller-reset-assert failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->ControllerResetsManaged = FALSE;
    }
  }
  if (Port->PhyResetsManaged) {
    Status = SetResetRange(
        Context, Port->Phy->ResetOffset, Port->Phy->ResetCount, TRUE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage phy-reset-assert failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->PhyResetsManaged = FALSE;
    }
  }
  if (Port->PhyClocksEnabled) {
    Status = SetClockRange(
        Context, Port, Port->Phy->ClockOffset, Port->Phy->ClockCount, FALSE, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage phy-clocks-disable failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->PhyClocksEnabled = FALSE;
    }
    if (!CR_ERROR(Status)) {
      Status = SetClockRange(
          Context, Port, Port->Phy->ClockOffset, Port->Phy->ClockCount,
          FALSE, TRUE);
      if (CR_ERROR(Status)) {
        log_err("PCIe: cleanup stage phy-pipe-clocks-disable failed, Status=0x%X",
                Status);
        return Status;
      }
    }
  }
  if (Port->ControllerClocksEnabled) {
    Port->ControllerConfigured = FALSE;
    Status = SetClockRange(
        Context, Port, Port->Target->ClockOffset, Port->Target->ClockCount, FALSE, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage controller-clocks-disable failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->ControllerClocksEnabled = FALSE;
      Port->ControllerConfigured = FALSE;
    }
    if (!CR_ERROR(Status)) {
      Status = SetClockRange(
          Context, Port, Port->Target->ClockOffset,
          Port->Target->ClockCount, FALSE, TRUE);
      if (CR_ERROR(Status)) {
        log_err("PCIe: cleanup stage controller-pipe-clocks-disable failed, Status=0x%X",
                Status);
        return Status;
      }
    }
  }
  if (Port->InterconnectEnabled) {
    Status = Context->Io.SetInterconnect(
        Context->Io.Context, Port->Target, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage interconnect-release failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->InterconnectEnabled = FALSE;
    }
  }
  if (Port->PowerEnabled) {
    Status = Context->Io.SetPowerDomain(
        Context->Io.Context, Port->Target, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage power-domain-disable failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->PowerEnabled = FALSE;
    }
  }
  if (Port->SuppliesEnabled) {
    Status = SetSupplyRange(
        Context, Port, Port->Target->SupplyOffset, Port->Target->SupplyCount, FALSE);
    if (CR_ERROR (Status)) {
      log_err("PCIe: cleanup stage supplies-disable failed, Status=0x%X",
              Status);
      return Status;
    } else {
      Port->SuppliesEnabled = FALSE;
    }
  }
  return CR_SUCCESS;
}

CR_STATUS
PcieLibInit(
    OUT PcieDeviceContext **Context, IN CONST PcieTargetContext *Target,
    IN CONST PcieIoOps *Io)
{
  UINT16 Index;
  CR_STATUS Status;

  if (Context == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (Target == NULL || Target->ClockCount > PCIE_MAX_RESOURCES ||
      Target->SupplyCount > PCIE_MAX_RESOURCES) {
    return CR_INVALID_PARAMETER;
  }
  Status = ValidateTarget(Target, Io);
  if (CR_ERROR(Status)) {
    return Status;
  }
  cr_memset(&mPcieContext, 0, sizeof(mPcieContext));
  mPcieContext.Target = Target;
  mPcieContext.Io = *Io;
  CrLockInit(&mPcieContext.ConfigLock);
  for (Index = 0; Index < Target->ControllerCount; Index++) {
    CONST PcieTargetController *Controller = &Target->Controllers[Index];
    CONST PcieTargetRegion *Region;
    PciePortRuntime *Port = &mPcieContext.Ports[Index];

    Port->Target = Controller;
    Port->Phy = &Target->Phys[Controller->PhyIndex];
    Region = FindRegion(Target, Controller, "parf");
    Port->ParfBase = Region->Base;
    Region = FindRegion(Target, Controller, "dbi");
    if (Region == NULL) {
      return CR_INVALID_PARAMETER;
    }
    Port->DbiBase = Region->Base;
    Port->DbiSize = Region->Size;
    Region = FindRegion(Target, Controller, "atu");
    if (Region == NULL) {
      return CR_INVALID_PARAMETER;
    }
    Port->AtuBase = Region->Base;
    Region = FindRegion(Target, Controller, "config");
    if (Region == NULL) {
      return CR_INVALID_PARAMETER;
    }
    Port->ConfigBase = Region->Base;
    Port->ConfigSize = Region->Size;
    Port->State = PCIE_PORT_OFF;
    Port->LastStatus = CR_SUCCESS;
  }
  mPcieContext.Initialized = TRUE;
  *Context = &mPcieContext;
  return CR_SUCCESS;
}

CR_STATUS
PcieInitializePort(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex)
{
  PciePortRuntime *Port;
  CR_STATUS Status;
  UINT32 Elapsed;
  UINT32 LastLtssm = CR_MAX_UINT32;
  UINT32 TraceCount = 0;
  CONST CHAR8 *FailureStage;

  if (Context == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  Port = &Context->Ports[PortIndex];
  if (Port->State == PCIE_PORT_LINK_UP ||
      Port->State == PCIE_PORT_READY_NO_LINK) {
    return CR_SUCCESS;
  }
  if ((Context->ResourceMask & BIT (PortIndex)) != 0) {
    /* A previous attempt retained resources after a failed revoke. */
    return CR_BUSY;
  }
  Port->State = PCIE_PORT_INITIALIZING;
  Port->TimingValidMask = 0;
  FailureStage = "unknown";

  /* A callback can fail after configuring an earlier GPIO.  Mark the group
   * owned before starting so the common error path restores every line. */
  Port->GpiosConfigured = TRUE;
  FailureStage = "gpio";
  Status = ConfigureGpios(Context, Port, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Port->SuppliesEnabled = TRUE;
  FailureStage = "supplies";
  Status = SetSupplyRange(
      Context, Port, Port->Target->SupplyOffset, Port->Target->SupplyCount, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  if (Port->Target->PowerDomainId != NULL) {
    Port->PowerEnabled = TRUE;
    FailureStage = "power-domain";
    Status = Context->Io.SetPowerDomain(
        Context->Io.Context, Port->Target, TRUE);
    if (CR_ERROR(Status)) {
      goto Error;
    }
  }
  Port->InterconnectEnabled = TRUE;
  FailureStage = "interconnect";
  Status = Context->Io.SetInterconnect(
      Context->Io.Context, Port->Target, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }

  /* The SMMU owns DMA isolation.  Attach every target stream while PERST is
   * asserted and before any controller clock can make the root complex issue
   * transactions. */
  if (Port->Target->IommuMapCount != 0) {
    Port->IommuAttached = TRUE;
    FailureStage = "smmu-attach";
    Status = Context->Io.SetIommu(
        Context->Io.Context, Port->Target, TRUE);
    if (CR_ERROR(Status)) {
      goto Error;
    }
  }
  Port->ControllerClocksEnabled = TRUE;
  FailureStage = "controller-clocks";
  /* Linux qcom_pcie_init_2_7_0() enables the complete controller clock
   * bulk before releasing the PCIe BCR.  Keep the pipe clock and mux on
   * during that reset sequence; the QMP PHY later takes its normal pipe
   * reference as well. */
  Status = SetClockRange(
      Context, Port, Port->Target->ClockOffset, Port->Target->ClockCount, TRUE, FALSE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Status = SetClockRange(
      Context, Port, Port->Target->ClockOffset, Port->Target->ClockCount, TRUE, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }

  Port->ControllerResetsManaged = TRUE;
  FailureStage = "controller-reset-assert";
  Status = SetResetRange(
      Context, Port->Target->ResetOffset, Port->Target->ResetCount, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_CONTROLLER_RESET_DELAY_US);
  FailureStage = "controller-reset-deassert";
  Status = SetResetRange(
      Context, Port->Target->ResetOffset, Port->Target->ResetCount, FALSE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_CONTROLLER_RESET_DELAY_US);

  Port->ControllerConfigured = TRUE;
  ConfigureParf(Context, Port);
  FailureStage = "phy";
  Status = InitializePhy(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  if (Port->Target->EndpointPower != NULL) {
    /* Mark before the callback: even a failed PMU vote or GPIO write may
       need rollback, while PERST is still asserted and DMA is isolated. */
    Port->EndpointPowerManaged = TRUE;
    FailureStage = "endpoint-power";
    Status = Context->Io.SetEndpointPower(&Context->Io, Port->Target, TRUE);
    if (CR_ERROR(Status)) {
      goto Error;
    }
  }
  ConfigureRootPort(Context, Port);
  FailureStage = "smmu-map";
  Status = ConfigureIommuMap(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  FailureStage = "atu";
  Status = ConfigureAtu(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  ConfigureEqualization(Context, Port);

  Context->Io.DelayUs(Context->Io.Context, PCIE_PERST_MIN_ASSERT_US);
  FailureStage = "perst-deassert";
  Status = SetPerst(Context, Port, FALSE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, 1500U);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_LTSSM, 0,
      (UINT32)PCIE_PARF_LTSSM_ENABLE);
  if (Context->Io.GetTimeUs != NULL) {
    Port->TrainingStartTimeUs = Context->Io.GetTimeUs(Context->Io.Context);
    Port->TimingValidMask |= 4U;
  }

  Port->State = PCIE_PORT_READY_NO_LINK;
  Context->InitializedMask |= (UINT32)BIT(PortIndex);
  Context->ResourceMask |= (UINT32)BIT(PortIndex);
  for (Elapsed = 0; Elapsed < PCIE_LINK_TIMEOUT_US;
       Elapsed += PCIE_LINK_POLL_DELAY_US) {
    /* Bound the trace even if a failing link repeatedly changes state.
     * DWC DEBUG0 is independent of the Qualcomm PARF state readback. */
    UINT32 Ltssm = PcieRead32(
        Context, Port->DbiBase + PCIE_DBI_PORT_DEBUG0) &
        PCIE_DBI_LTSSM_STATE_MASK;
    if (Ltssm != LastLtssm && TraceCount < 8U) {
      log_info("PCIe: port %u training +%u us DWC-LTSSM=0x%X DEBUG1=0x%X",
               PortIndex, Elapsed, Ltssm,
               PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_DEBUG1));
      ++TraceCount;
    }
    LastLtssm = Ltssm;
    if (PcieIsLinkUp(Context, PortIndex)) {
      break;
    }
    Context->Io.DelayUs(Context->Io.Context, PCIE_LINK_POLL_DELAY_US);
  }
  if (Port->State != PCIE_PORT_LINK_UP) {
    UINT8 Capability;
    UINT16 LinkStatus;
    UINT32 ParfLtssm;
    UINT32 ElbiStatus;
    UINT32 ElbiControl;
    UINT32 InterruptStatus;

    Capability = FindExpressCapability(Context, Port);
    LinkStatus = Capability == 0
                     ? 0
                     : PcieRead16(
                           Context,
                           Port->DbiBase + Capability + PCIE_EXP_LINK_STATUS);
    ParfLtssm = PcieRead32(Context, Port->ParfBase + PCIE_PARF_LTSSM);
    /* ELBI is a separately mapped register window; its fixed offset is
     * derived from the target region table rather than assuming a DBI alias. */
    {
      CONST PcieTargetRegion *Elbi =
          FindRegion(Context->Target, Port->Target, "elbi");
      ElbiStatus = Elbi == NULL || Elbi->Size < PCIE_ELBI_SYS_STTS + 4U
                       ? 0
                       : PcieRead32(Context, Elbi->Base + PCIE_ELBI_SYS_STTS);
      ElbiControl = Elbi == NULL || Elbi->Size < PCIE_ELBI_SYS_CTRL + 4U
                        ? 0
                        : PcieRead32(Context, Elbi->Base + PCIE_ELBI_SYS_CTRL);
    }
    InterruptStatus = PcieRead32(
        Context, Port->ParfBase + PCIE_PARF_INT_ALL_STATUS);
    Port->LastStatus = CR_TIMEOUT;
    log_warn(
        "PCIe: port %u link timeout after %u us, PARF-LTSSM=0x%X (state=0x%X) ELBI=0x%X (state=0x%X) DBI-LinkStatus=0x%04X INT=0x%X",
        PortIndex, PCIE_LINK_TIMEOUT_US, ParfLtssm,
        ParfLtssm & PCIE_PARF_LTSSM_STATE_MASK, ElbiStatus,
        (ElbiStatus & PCIE_ELBI_LTSSM_STATE_MASK) >> 12, LinkStatus,
        InterruptStatus);
    log_warn(
        "PCIe: port %u link timeout DWC-debug0=0x%X debug1=0x%X link-ctl=0x%X link-ctl2=0x%X ELBI-ctrl=0x%X PARF-phy=0x%X pm=0x%X clkreq=0x%X",
        PortIndex,
        PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_DEBUG0),
        PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_DEBUG1),
        Capability == 0 ? 0 : PcieRead16(
            Context, Port->DbiBase + Capability + PCIE_EXP_LINK_CONTROL),
        Capability == 0 ? 0 : PcieRead16(
            Context, Port->DbiBase + Capability + PCIE_EXP_LINK_CONTROL2),
        ElbiControl,
        PcieRead32(Context, Port->ParfBase + PCIE_PARF_PHY_CTRL),
        PcieRead32(Context, Port->ParfBase + PCIE_PARF_PM_CTRL),
        PcieRead32(Context, Port->ParfBase + PCIE_PARF_CLKREQ_OVERRIDE));
    log_warn(
        "PCIe: port %u link timeout PHY-status=0x%X reset=0x%X start=0x%X power=0x%X PARF-sys=0x%X q2a-flush=0x%X DBI-link-control=0x%X width-speed=0x%X",
        PortIndex,
        PcieRead32(Context, Port->Phy->Base + Port->Phy->BlockOffsets.Pcs +
                                Port->Phy->Status),
        PcieRead32(Context, Port->Phy->Base + Port->Phy->BlockOffsets.Pcs +
                                Port->Phy->SwReset),
        PcieRead32(Context, Port->Phy->Base + Port->Phy->BlockOffsets.Pcs +
                                Port->Phy->StartControl),
        PcieRead32(Context, Port->Phy->Base + Port->Phy->BlockOffsets.Pcs +
                                Port->Phy->PowerDownControl),
        PcieRead32(Context, Port->ParfBase + PCIE_PARF_SYS_CTRL),
        PcieRead32(Context, Port->ParfBase + PCIE_PARF_Q2A_FLUSH),
        PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_LINK_CONTROL),
        PcieRead32(Context,
                   Port->DbiBase + PCIE_DBI_LINK_WIDTH_SPEED_CONTROL));
    (VOID)LogPhyConfiguration(Context, Port);
    log_warn(
        "PCIe: port %u timeout ownership supply-mask=0x" CR_LOG_HEX64_FMT " clock-mask=0x" CR_LOG_HEX64_FMT " power=%u controller-clocks=%u PHY-clocks=%u ICB=%u SMMU=%u endpoint=%u",
        PortIndex, Port->SupplyOwnedMask, Port->ClockOwnedMask,
        Port->PowerEnabled, Port->ControllerClocksEnabled,
        Port->PhyClocksEnabled, Port->InterconnectEnabled,
        Port->IommuAttached, Port->EndpointPowerManaged);
    if (Port->TimingValidMask == 7U) {
      UINT64 Now = Context->Io.GetTimeUs(Context->Io.Context);
      log_warn(
          "PCIe: port %u PERST assert=" CR_LOG_UINT64_FMT " release=" CR_LOG_UINT64_FMT " LTSSM-enable=" CR_LOG_UINT64_FMT " snapshot=" CR_LOG_UINT64_FMT " us (asserted=" CR_LOG_UINT64_FMT " release-to-training=" CR_LOG_UINT64_FMT " training=" CR_LOG_UINT64_FMT ")",
          PortIndex, Port->PerstAssertTimeUs, Port->PerstReleaseTimeUs,
          Port->TrainingStartTimeUs, Now,
          Port->PerstReleaseTimeUs - Port->PerstAssertTimeUs,
          Port->TrainingStartTimeUs - Port->PerstReleaseTimeUs,
          Now - Port->TrainingStartTimeUs);
    }
    if (Context->Io.DiagnoseLink != NULL)
      Context->Io.DiagnoseLink(&Context->Io, Port->Target);
  } else {
    Port->LastStatus = CR_SUCCESS;
  }
  log_info(
      "PCIe: port %u root complex ready, link " CR_LOG_CHAR8_STR_FMT
      " (last-status=0x%X)", PortIndex,
      Port->State == PCIE_PORT_LINK_UP ? "up" : "down", Port->LastStatus);
  return CR_SUCCESS;

Error:
  {
    CR_STATUS ReleaseStatus;
    CR_STATUS OriginalStatus;

    OriginalStatus = Status;
    Port->LastStatus = Status;
    Port->State = PCIE_PORT_FAILED;
    log_err(
        "PCIe: port %u stage %a failed, Status=0x%X",
        PortIndex, FailureStage, OriginalStatus);
    ReleaseStatus = ReleasePortResources (Context, Port);
    if (CR_ERROR (ReleaseStatus)) {
      /* Keep ownership visible to the host shutdown path. */
      Context->ResourceMask |= (UINT32)BIT (PortIndex);
      /* The cleanup failure must not hide the stage that failed first. */
      Port->LastStatus = OriginalStatus;
      log_err(
          "PCIe: port %u initialization failed, Status=0x%X; cleanup Status=0x%X",
          PortIndex, OriginalStatus, ReleaseStatus);
      return OriginalStatus;
    }
  }
  Context->ResourceMask &= ~(UINT32)BIT (PortIndex);
  Context->InitializedMask &= ~(UINT32)BIT(PortIndex);
  Context->LinkMask &= ~(UINT32)BIT(PortIndex);
  log_err("PCIe: port %u initialization failed, Status=0x%X", PortIndex,
          Status);
  return Status;
}

CR_STATUS
PcieInitializeMask(
    IN OUT PcieDeviceContext *Context, IN UINT32 PortMask,
    OUT UINT32 *InitializedMask)
{
  UINT16 Index;
  CR_STATUS FirstError = CR_SUCCESS;

  if (Context == NULL || InitializedMask == NULL || !Context->Initialized ||
      (PortMask >> Context->Target->ControllerCount) != 0) {
    return CR_INVALID_PARAMETER;
  }
  for (Index = 0; Index < Context->Target->ControllerCount; Index++) {
    CR_STATUS Status;
    if ((PortMask & BIT(Index)) == 0) {
      continue;
    }
    Status = PcieInitializePort(Context, Index);
    if (CR_ERROR(Status) && !CR_ERROR(FirstError)) {
      FirstError = Status;
    }
  }
  *InitializedMask = Context->InitializedMask & PortMask;
  if (*InitializedMask != 0) {
    return CR_SUCCESS;
  }
  return CR_ERROR(FirstError) ? FirstError : CR_NOT_FOUND;
}

CR_STATUS
PcieShutdownPort(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex)
{
  PciePortRuntime *Port;
  CR_STATUS        Status;

  if (Context == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  Port = &Context->Ports[PortIndex];
  Status = ReleasePortResources (Context, Port);
  if (CR_ERROR (Status)) {
    /* Keep the initialized bit and all resource ownership flags intact so a
     * later controlled shutdown can retry the failed SMMU/fabric operation. */
    Port->State      = PCIE_PORT_FAILED;
    Port->LastStatus = Status;
    if (PortResourcesOwned (Port)) {
      Context->ResourceMask |= (UINT32)BIT (PortIndex);
    }
    return Status;
  }
  Context->ResourceMask &= ~(UINT32)BIT (PortIndex);
  Context->InitializedMask &= ~(UINT32)BIT(PortIndex);
  Context->LinkMask &= ~(UINT32)BIT(PortIndex);
  Port->State = PCIE_PORT_OFF;
  Port->LastStatus = CR_SUCCESS;
  return CR_SUCCESS;
}

CR_STATUS
PcieGetPortInfo(
    IN PcieDeviceContext *Context, IN UINT16 PortIndex,
    OUT CONST PciePortRuntime **Port)
{
  if (Context == NULL || Port == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  *Port = &Context->Ports[PortIndex];
  return CR_SUCCESS;
}
