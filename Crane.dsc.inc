[LibraryClasses.common]
  # OS/Kernel Abstract Layer Libs
  OskInterruptLib|CranePkg/Library/OskInterruptLib/oskinterrupt.inf
  OskMemmapLib|CranePkg/Library/OskMemmapLib/oskmemmap.inf

  # Only CrDALDxe links the platform data provider. Consumers use CrDalLib.
  CrTargetLib|$(CR_TARGET_LIB_PATH)
  CrDalLib|CranePkg/Library/CrDalLib/CrDalLib.inf
  CrDalRegistryLib|CranePkg/Library/CrDalRegistryLib/CrDalRegistryLib.inf

  # Generic libraries that interact with oskal
  CmdDBLib|CranePkg/Library/CmdDBLib/cmddb.inf
  ClockLib|CranePkg/Library/ClockLib/clock.inf
  DebugUartLib|CranePkg/Library/DebugUartLib/debug_uart.inf
  GpioLib|CranePkg/Library/GpioLib/gpio.inf
  RpmhLib|CranePkg/Library/RpmhLib/rpmh.inf
  PdcLib|CranePkg/Library/PdcLib/pdc.inf
  PcieLib|CranePkg/Library/PcieLib/pcie.inf
  ICBLib|CranePkg/Library/ICBLib/icb.inf
  SmmuLib|CranePkg/Library/SmmuLib/smmu.inf
  PciHostBridgeLib|CranePkg/Library/PciHostBridgeLib/PciHostBridgeLib.inf
  # General PCI consumers use the sole host driver's RootBridgeIo protocol.
  # Only PciHostBridgeDxe overrides this with the direct Crane hardware path.
  PciSegmentLib|MdePkg/Library/UefiPciSegmentLibPciRootBridgeIo/UefiPciSegmentLibPciRootBridgeIo.inf

[Components.common]
  CranePkg/Driver/CrDALDxe/CrDALDxe.inf
  CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
  CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf
  CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf
  CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf
  CranePkg/Driver/ICBCrDxe/ICBCrDxe.inf
  CranePkg/Driver/SmmuCrDxe/SmmuCrDxe.inf
  CranePkg/Driver/QUPCrDxe/QUPCrDxe.inf
  CranePkg/Driver/GpiCrDxe/GpiCrDxe.inf
  CranePkg/Driver/I2CCrDxe/I2CCrDxe.inf
  CranePkg/Driver/SPICrDxe/SPICrDxe.inf
  CranePkg/Driver/DebugUartSerialCrDxe/DebugUartSerialCrDxe.inf
