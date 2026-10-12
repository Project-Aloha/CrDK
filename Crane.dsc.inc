[LibraryClasses.common]
  # OS/Kernel Abstract Layer Libs
  OskInterruptLib|CranePkg/Library/OskInterruptLib/oskinterrupt.inf
  OskMemmapLib|CranePkg/Library/OskMemmapLib/oskmemmap.inf

  # Only CrDALDxe links the platform data provider. Consumers use CrDalLib.
  CrTargetLib|$(CR_TARGET_LIB_PATH)
  CrDalLib|CranePkg/Library/CrDalLib/CrDalLib.inf
  CrDalRegistryLib|CranePkg/Library/CrDalRegistryLib/CrDalRegistryLib.inf
  AndroidBootCrLib|CranePkg/Library/AndroidBootCrLib/AndroidBootCrLib.inf
  UfsPowerPolicyCrLib|CranePkg/Library/UfsPowerPolicyCrLib/UfsPowerPolicyCrLib.inf
  UfsPowerFdtCrLib|CranePkg/Library/UfsPowerFdtCrLib/UfsPowerFdtCrLib.inf

  # Generic libraries that interact with oskal
  CmdDBLib|CranePkg/Library/CmdDBLib/cmddb.inf
  ClockLib|CranePkg/Library/ClockLib/clock.inf
  Dwc3CrLib|CranePkg/Library/Dwc3CrLib/Dwc3CrLib.inf
  UsbMscCrLib|CranePkg/Library/UsbMscCrLib/UsbMscCrLib.inf
  UsbHandoffCrLib|CranePkg/Library/UsbHandoffCrLib/UsbHandoffCrLib.inf
  UsbDpPhyCrLib|CranePkg/Library/UsbDpPhyCrLib/UsbDpPhyCrLib.inf
  ClockExternalLib|CranePkg/Library/ClockExternalLib/ClockExternalLib.inf
  DebugUartLib|CranePkg/Library/DebugUartLib/debug_uart.inf
  GpioLib|CranePkg/Library/GpioLib/gpio.inf
  RpmhLib|CranePkg/Library/RpmhLib/rpmh.inf
  PdcLib|CranePkg/Library/PdcLib/pdc.inf
  PcieLib|CranePkg/Library/PcieLib/pcie.inf
  ICBLib|CranePkg/Library/ICBLib/icb.inf
  SmmuLib|CranePkg/Library/SmmuLib/smmu.inf
  SmemLib|CranePkg/Library/SmemLib/smem.inf
  RamPartitionCrLib|CranePkg/Library/RamPartitionCrLib/RamPartitionCrLib.inf
  PILCrLib|CranePkg/Library/PILCrLib/PILCrLib.inf
  MdssDpCrLib|CranePkg/Library/MdssDpCrLib/MdssDpCrLib.inf
  MdssCrLib|CranePkg/Library/MdssCrLib/MdssCrLib.inf
  MdssDscCrLib|CranePkg/Library/MdssDscCrLib/MdssDscCrLib.inf
  MdssDsiCmdCrLib|CranePkg/Library/MdssDsiCmdCrLib/MdssDsiCmdCrLib.inf
  MdssDsiCrLib|CranePkg/Library/MdssDsiCrLib/MdssDsiCrLib.inf
  MdssDualCrLib|CranePkg/Library/MdssDualCrLib/MdssDualCrLib.inf
  MdssHandoffCrLib|CranePkg/Library/MdssHandoffCrLib/MdssHandoffCrLib.inf
  MdssHandoffUefiLib|CranePkg/Library/MdssHandoffUefiLib/MdssHandoffUefiLib.inf
  MdssPlatformLib|CranePkg/Library/MdssPlatformLib/MdssPlatformLib.inf
  RpmhArcVoteLib|CranePkg/Library/RpmhArcVoteLib/RpmhArcVoteLib.inf
  Sm8450RailLib|CranePkg/Library/Sm8450RailLib/Sm8450RailLib.inf
  UcsiCrLib|CranePkg/Library/UcsiCrLib/UcsiCrLib.inf
!ifdef $(CR_UCSI_TARGET_LIB_PATH)
  UcsiTargetLib|$(CR_UCSI_TARGET_LIB_PATH)
!endif
  SpmiSharedCrLib|CranePkg/Library/SpmiSharedCrLib/SpmiSharedCrLib.inf
  PmicCcCrLib|CranePkg/Library/PmicCcCrLib/PmicCcCrLib.inf
  SpmiLib|CranePkg/Library/SpmiLib/spmi.inf
  PmicGpioLib|CranePkg/Library/PmicGpioLib/pmic_gpio.inf
  ButtonsCrLib|CranePkg/Library/ButtonsCrLib/ButtonsCrLib.inf
  SdhciMsmLib|CranePkg/Library/SdhciMsmLib/SdhciMsmLib.inf
  GpioPinLeaseLib|CranePkg/Library/GpioPinLeaseLib/GpioPinLeaseLib.inf
  GpioSdLib|CranePkg/Library/GpioSdLib/GpioSdLib.inf
  PciHostBridgeLib|CranePkg/Library/PciHostBridgeLib/PciHostBridgeLib.inf
  # General PCI consumers use the sole host driver's RootBridgeIo protocol.
  # Only PciHostBridgeDxe overrides this with the direct Crane hardware path.
  PciSegmentLib|MdePkg/Library/UefiPciSegmentLibPciRootBridgeIo/UefiPciSegmentLibPciRootBridgeIo.inf

[Components.common]
  CranePkg/Driver/CrDALDxe/CrDALDxe.inf
  CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
  CranePkg/Driver/SmemCrDxe/SmemCrDxe.inf
  CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf
  CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf
  CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf
  CranePkg/Driver/ICBCrDxe/ICBCrDxe.inf
  CranePkg/Driver/SmmuCrDxe/SmmuCrDxe.inf
  CranePkg/Driver/QUPCrDxe/QUPCrDxe.inf
  CranePkg/Driver/GpiCrDxe/GpiCrDxe.inf
  CranePkg/Driver/I2CCrDxe/I2CCrDxe.inf
  CranePkg/Driver/SPICrDxe/SPICrDxe.inf
