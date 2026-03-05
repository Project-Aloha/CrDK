INF CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
INF CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf
INF CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf
INF CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf

!if $(USE_UART_GENI_FOR_SERIAL_OUTPUT) == 1
  INF MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
!else
  INF  CranePkg/Driver/DebugUartSerialCrDxe/DebugUartSerialCrDxe.inf
!endif
