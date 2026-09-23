INF CranePkg/Driver/CrDALDxe/CrDALDxe.inf
INF CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf
# Resource and bus drivers use normal DEPEX dispatch after the platform's
# RPMh/QUP/GPI backends. APRIORI would bypass those dependencies and allow
# independent drivers to initialize the same hardware at the same time.

!if $(USE_UART_GENI_FOR_SERIAL_OUTPUT) == 1
  INF MdeModulePkg/Universal/SerialDxe/SerialDxe.inf
!else
  INF  CranePkg/Driver/DebugUartSerialCrDxe/DebugUartSerialCrDxe.inf
!endif
