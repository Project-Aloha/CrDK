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
#include <oskal/cr_string.h>

#ifdef _KERNEL_MODE
#include "pcie.tmh"
#endif

#define PCIE_PARF_SYS_CTRL                    0x000
#define PCIE_PARF_PM_CTRL                     0x020
#define PCIE_PARF_PHY_CTRL                    0x040
#define PCIE_PARF_MHI_CLOCK_RESET_CTRL        0x174
#define PCIE_PARF_AXI_MSTR_WR_ADDR_HALT_V2    0x1A8
#define PCIE_PARF_LTSSM                       0x1B0
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
#define PCIE_DBI_LINK_WIDTH_SPEED_CONTROL     0x80C
#define PCIE_DBI_MISC_CONTROL_1               0x8BC

#define PCIE_DBI_COMMAND_IO                   BIT(0)
#define PCIE_DBI_COMMAND_MEMORY               BIT(1)
#define PCIE_DBI_COMMAND_MASTER               BIT(2)
#define PCIE_DBI_COMMAND_SERR                 BIT(8)
#define PCIE_DBI_CLASS_BRIDGE_PCI             0x0604U
#define PCIE_DBI_RO_WRITE_ENABLE              BIT(0)
#define PCIE_DBI_PORT_FAST_LINK_MODE          BIT(7)
#define PCIE_DBI_PORT_LINK_MODE_MASK          GEN_MSK(21, 16)
#define PCIE_DBI_LINK_WIDTH_MASK              GEN_MSK(12, 8)
#define PCIE_DBI_SPEED_CHANGE                 BIT(17)

#define PCIE_CAP_ID_EXPRESS                   0x10U
#define PCIE_CAP_NEXT_MASK                    0xFCU
#define PCIE_EXP_LINK_STATUS                  0x12U
#define PCIE_EXP_SLOT_CAPABILITIES            0x14U
#define PCIE_EXP_LINK_ACTIVE                  BIT(13)
#define PCIE_EXP_SLOT_NO_COMMAND_COMPLETED    BIT(18)

#define PCIE_ATU_REGION_STRIDE                0x200
#define PCIE_ATU_REGION_CTRL1                 0x000
#define PCIE_ATU_REGION_CTRL2                 0x004
#define PCIE_ATU_LOWER_BASE                   0x008
#define PCIE_ATU_UPPER_BASE                   0x00C
#define PCIE_ATU_LIMIT                        0x010
#define PCIE_ATU_LOWER_TARGET                 0x014
#define PCIE_ATU_UPPER_TARGET                 0x018
#define PCIE_ATU_ENABLE                       BIT(31)
#define PCIE_ATU_CFG_SHIFT_MODE               BIT(28)
#define PCIE_ATU_TYPE_MEMORY                  0x00U
#define PCIE_ATU_TYPE_IO                      0x02U
#define PCIE_ATU_TYPE_CFG0                    0x04U

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
        Target->Clocks[Index].Id == NULL) {
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
  for (Index = 0; Index < Target->GpioCount; Index++) {
    if (Target->Gpios[Index].Role == NULL ||
        Target->Gpios[Index].Controller == NULL ||
        Target->Gpios[Index].Polarity == NULL ||
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

    if (Controller->Label == NULL || Controller->Compatible == NULL ||
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
        (Controller->PowerDomainId != NULL && Io->SetPowerDomain == NULL)) {
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

STATIC CR_STATUS
SetClockRange(
    IN PcieDeviceContext *Context, IN UINT16 Offset, IN UINT16 Count,
    IN BOOLEAN Enable)
{
  UINT16 Index;
  CR_STATUS Status;

  if (!Enable) {
    for (Index = Count; Index > 0; Index--) {
      (VOID)Context->Io.SetClock(
          Context->Io.Context, &Context->Target->Clocks[Offset + Index - 1],
          FALSE);
    }
    return CR_SUCCESS;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = Context->Io.SetClock(
        Context->Io.Context, &Context->Target->Clocks[Offset + Index], TRUE);
    if (CR_ERROR(Status)) {
      while (Index > 0) {
        Index--;
        (VOID)Context->Io.SetClock(
            Context->Io.Context, &Context->Target->Clocks[Offset + Index],
            FALSE);
      }
      return Status;
    }
  }
  return CR_SUCCESS;
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
      return Status;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
ConfigureGpios(
    IN PcieDeviceContext *Context, IN CONST PcieTargetController *Controller,
    IN BOOLEAN AssertPerst)
{
  UINT16 Index;

  for (Index = 0; Index < Controller->GpioCount; Index++) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index];
    CR_STATUS Status;

    if (cr_strcmp(Gpio->Role, "wake") == 0) {
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
      return Status;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
SetPerst(
    IN PcieDeviceContext *Context, IN CONST PcieTargetController *Controller,
    IN BOOLEAN Assert)
{
  UINT16 Index;

  for (Index = 0; Index < Controller->GpioCount; Index++) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index];
    if (cr_strcmp(Gpio->Role, "perst") == 0) {
      return Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, Assert);
    }
  }
  return CR_NOT_FOUND;
}

STATIC VOID
ReleaseGpios(
    IN PcieDeviceContext *Context, IN CONST PcieTargetController *Controller)
{
  UINT16 Index;

  for (Index = Controller->GpioCount; Index > 0; Index--) {
    CONST PcieTargetGpio *Gpio =
        &Context->Target->Gpios[Controller->GpioOffset + Index - 1U];

    if (cr_strcmp(Gpio->Role, "perst") == 0) {
      (VOID)Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, TRUE);
    } else if (cr_strcmp(Gpio->Role, "enable") == 0) {
      (VOID)Context->Io.SetGpio(
          Context->Io.Context, Gpio, PCIE_GPIO_OUTPUT, FALSE);
    }
  }
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

STATIC CR_STATUS
InitializePhy(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port)
{
  CONST PcieTargetPhy *Phy = Port->Phy;
  UINT64 Pcs = Phy->Base + Phy->BlockOffsets.Pcs;
  UINT32 Elapsed;
  CR_STATUS Status;

  Port->PhyResetsManaged = TRUE;
  Status = SetResetRange(
      Context, Phy->ResetOffset, Phy->ResetCount, TRUE);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_PHY_RESET_DELAY_US);
  Status = SetResetRange(
      Context, Phy->ResetOffset, Phy->ResetCount, FALSE);
  if (CR_ERROR(Status)) {
    return Status;
  }

  Status = SetClockRange(
      Context, Phy->ClockOffset, Phy->ClockCount, TRUE);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Port->PhyClocksEnabled = TRUE;

  PcieRmw32(
      Context, Pcs + Phy->PowerDownControl, 0,
      Phy->PowerDownControlValue);
  Status = ProgramPhyTable(Context, Phy, PCIE_PHY_PHASE_BASE);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = ProgramPhyTable(Context, Phy, PCIE_PHY_PHASE_RC);
  if (CR_ERROR(Status)) {
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
      return CR_SUCCESS;
    }
    Context->Io.DelayUs(Context->Io.Context, PCIE_PHY_POLL_DELAY_US);
  }
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
      (UINT32)PCIE_PARF_MAC_PHY_POWERDOWN_MUX, 0);
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
  UINT8 Capability;
  UINT16 Guard;

  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_MISC_CONTROL_1, 0,
      (UINT32)PCIE_DBI_RO_WRITE_ENABLE);

  Value = PcieRead32(Context, Port->DbiBase + PCIE_DBI_PORT_LINK_CONTROL);
  Value &= ~((UINT32)PCIE_DBI_PORT_FAST_LINK_MODE |
             (UINT32)PCIE_DBI_PORT_LINK_MODE_MASK);
  Value |= PortLinkMode(Port->Target->Lanes);
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
  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_PRIMARY_BUS, 0x00FFFFFFU,
      0x00FF0100U);
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
      PcieRmw32(
          Context,
          Port->DbiBase + Capability + PCIE_EXP_SLOT_CAPABILITIES, 0,
          (UINT32)PCIE_EXP_SLOT_NO_COMMAND_COMPLETED);
      break;
    }
    Capability = PcieRead8(Context, Port->DbiBase + Capability + 1U) &
                 PCIE_CAP_NEXT_MASK;
  }

  PcieRmw32(
      Context, Port->DbiBase + PCIE_DBI_MISC_CONTROL_1,
      (UINT32)PCIE_DBI_RO_WRITE_ENABLE, 0);
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
      Port->ConfigSize, (UINT32)PCIE_ATU_CFG_SHIFT_MODE);
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
    }
  } else {
    Context->LinkMask &= ~(UINT32)BIT(PortIndex);
    if (Port->State == PCIE_PORT_LINK_UP) {
      Port->State = PCIE_PORT_READY_NO_LINK;
    }
  }
  return LinkUp;
}

STATIC VOID
ReleasePortResources(
    IN PcieDeviceContext *Context, IN OUT PciePortRuntime *Port)
{
  if (Port->ControllerClocksEnabled) {
    PcieRmw32(
        Context, Port->ParfBase + PCIE_PARF_LTSSM,
        (UINT32)PCIE_PARF_LTSSM_ENABLE, 0);
  }
  if (Port->GpiosConfigured) {
    ReleaseGpios(Context, Port->Target);
    Port->GpiosConfigured = FALSE;
  }
  if (Port->ControllerResetsManaged) {
    (VOID)SetResetRange(
        Context, Port->Target->ResetOffset, Port->Target->ResetCount, TRUE);
    Port->ControllerResetsManaged = FALSE;
  }
  if (Port->PhyResetsManaged) {
    (VOID)SetResetRange(
        Context, Port->Phy->ResetOffset, Port->Phy->ResetCount, TRUE);
    Port->PhyResetsManaged = FALSE;
  }
  if (Port->PhyClocksEnabled) {
    (VOID)SetClockRange(
        Context, Port->Phy->ClockOffset, Port->Phy->ClockCount, FALSE);
    Port->PhyClocksEnabled = FALSE;
  }
  if (Port->ControllerClocksEnabled) {
    (VOID)SetClockRange(
        Context, Port->Target->ClockOffset, Port->Target->ClockCount, FALSE);
    Port->ControllerClocksEnabled = FALSE;
  }
  if (Port->PowerEnabled) {
    (VOID)Context->Io.SetPowerDomain(
        Context->Io.Context, Port->Target, FALSE);
    Port->PowerEnabled = FALSE;
  }
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
  Status = ValidateTarget(Target, Io);
  if (CR_ERROR(Status)) {
    return Status;
  }
  cr_memset(&mPcieContext, 0, sizeof(mPcieContext));
  mPcieContext.Target = Target;
  mPcieContext.Io = *Io;
  for (Index = 0; Index < Target->ControllerCount; Index++) {
    CONST PcieTargetController *Controller = &Target->Controllers[Index];
    CONST PcieTargetRegion *Region;
    PciePortRuntime *Port = &mPcieContext.Ports[Index];

    Port->Target = Controller;
    Port->Phy = &Target->Phys[Controller->PhyIndex];
    Region = FindRegion(Target, Controller, "parf");
    Port->ParfBase = Region->Base;
    Region = FindRegion(Target, Controller, "dbi");
    Port->DbiBase = Region->Base;
    Region = FindRegion(Target, Controller, "atu");
    Port->AtuBase = Region->Base;
    Region = FindRegion(Target, Controller, "config");
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

  if (Context == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  Port = &Context->Ports[PortIndex];
  if (Port->State == PCIE_PORT_LINK_UP ||
      Port->State == PCIE_PORT_READY_NO_LINK) {
    return CR_SUCCESS;
  }
  Port->State = PCIE_PORT_INITIALIZING;

  /* A callback can fail after configuring an earlier GPIO.  Mark the group
   * owned before starting so the common error path restores every line. */
  Port->GpiosConfigured = TRUE;
  Status = ConfigureGpios(Context, Port->Target, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  if (Port->Target->PowerDomainId != NULL) {
    Status = Context->Io.SetPowerDomain(
        Context->Io.Context, Port->Target, TRUE);
    if (CR_ERROR(Status)) {
      goto Error;
    }
    Port->PowerEnabled = TRUE;
  }
  Status = SetClockRange(
      Context, Port->Target->ClockOffset, Port->Target->ClockCount, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Port->ControllerClocksEnabled = TRUE;

  Port->ControllerResetsManaged = TRUE;
  Status = SetResetRange(
      Context, Port->Target->ResetOffset, Port->Target->ResetCount, TRUE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_CONTROLLER_RESET_DELAY_US);
  Status = SetResetRange(
      Context, Port->Target->ResetOffset, Port->Target->ResetCount, FALSE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, PCIE_CONTROLLER_RESET_DELAY_US);

  ConfigureParf(Context, Port);
  Status = InitializePhy(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  ConfigureRootPort(Context, Port);
  Status = ConfigureIommuMap(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Status = ConfigureAtu(Context, Port);
  if (CR_ERROR(Status)) {
    goto Error;
  }

  Context->Io.DelayUs(Context->Io.Context, PCIE_PERST_MIN_ASSERT_US);
  Status = SetPerst(Context, Port->Target, FALSE);
  if (CR_ERROR(Status)) {
    goto Error;
  }
  Context->Io.DelayUs(Context->Io.Context, 1500U);
  PcieRmw32(
      Context, Port->ParfBase + PCIE_PARF_LTSSM, 0,
      (UINT32)PCIE_PARF_LTSSM_ENABLE);

  Port->State = PCIE_PORT_READY_NO_LINK;
  Context->InitializedMask |= (UINT32)BIT(PortIndex);
  for (Elapsed = 0; Elapsed < PCIE_LINK_TIMEOUT_US;
       Elapsed += PCIE_LINK_POLL_DELAY_US) {
    if (PcieIsLinkUp(Context, PortIndex)) {
      break;
    }
    Context->Io.DelayUs(Context->Io.Context, PCIE_LINK_POLL_DELAY_US);
  }
  Port->LastStatus = CR_SUCCESS;
  log_info(
      "PCIe: port %u initialized, link " CR_LOG_CHAR8_STR_FMT, PortIndex,
      Port->State == PCIE_PORT_LINK_UP ? "up" : "down");
  return CR_SUCCESS;

Error:
  Port->LastStatus = Status;
  Port->State = PCIE_PORT_FAILED;
  ReleasePortResources(Context, Port);
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

  if (Context == NULL || !Context->Initialized ||
      PortIndex >= Context->Target->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  Port = &Context->Ports[PortIndex];
  ReleasePortResources(Context, Port);
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
