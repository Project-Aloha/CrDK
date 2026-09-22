[LibraryClasses.common]
  # OS/Kernel Abstract Layer Libs
  OskInterruptLib|CranePkg/Library/OskInterruptLib/oskinterrupt.inf
  OskMemmapLib|CranePkg/Library/OskMemmapLib/oskmemmap.inf

  # Platform specific data
  CrTargetLib|QcomPkg/Library/CrTargetLib/CrTargetLib.inf

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
  PciSegmentLib|CranePkg/Library/PciSegmentLib/PciSegmentLib.inf

[Components.common]
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
