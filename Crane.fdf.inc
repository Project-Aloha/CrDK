# Continuation of [FV.FvMain]
INF CranePkg/Driver/CrDALDxe/CrDALDxe.inf
INF CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
INF CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf
INF CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf
INF CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf
INF CranePkg/Driver/ICBCrDxe/ICBCrDxe.inf
INF CranePkg/Driver/SmmuCrDxe/SmmuCrDxe.inf
INF CranePkg/Driver/QUPCrDxe/QUPCrDxe.inf
INF CranePkg/Driver/GpiCrDxe/GpiCrDxe.inf
INF CranePkg/Driver/I2CCrDxe/I2CCrDxe.inf
INF CranePkg/Driver/SPICrDxe/SPICrDxe.inf

!if $(USE_UART_GENI_FOR_SERIAL_OUTPUT) == 1
  INF MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
!else
  INF  CranePkg/Driver/DebugUartSerialCrDxe/DebugUartSerialCrDxe.inf
!endif
