# Inactive optional selection. Include AFTER common Andromeda/Crane mappings.
# Keep SEC/PEI raw diagnostics; all surviving DXE copies join one runtime gate.
# SPDX-License-Identifier: MIT
!if $(CR_HDK_UART_HANDOFF) == 1
!if "$(TARGET_DEVICE)" != "qcom-hdk8450"
  !error UART handoff selection requires qcom-hdk8450
!endif
[LibraryClasses.common]
  UartAdmissionLib|CranePkg/Library/UartAdmissionLib/UartAdmissionLib.inf
  UartHandoffLib|CranePkg/Library/UartHandoffLib/UartHandoffLib.inf
  UartHandoffUefiLib|CranePkg/Library/UartHandoffUefiLib/UartHandoffUefiLib.inf

[LibraryClasses.common.DXE_CORE, LibraryClasses.common.DXE_DRIVER, LibraryClasses.common.UEFI_DRIVER, LibraryClasses.common.UEFI_APPLICATION]
  SerialPortLib|CranePkg/Library/QcomGeniGatedSerialPortLib/QcomGeniGatedSerialPortLib.inf

[LibraryClasses.common.DXE_RUNTIME_DRIVER]
  SerialPortLib|CranePkg/Library/QcomGeniGatedSerialPortLib/QcomGeniGatedSerialPortLibRuntime.inf

[Components.common]
  CranePkg/Driver/UartHandoffCrDxe/UartHandoffCrDxe.inf
!endif
