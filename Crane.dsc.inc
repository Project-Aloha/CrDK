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

[Components.common]
  CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
  CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf
  CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf
  CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf
  CranePkg/Driver/DebugUartSerialCrDxe/DebugUartSerialCrDxe.inf
