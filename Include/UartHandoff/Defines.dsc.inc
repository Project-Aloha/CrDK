# Optional HDK UART retirement profile. SPDX-License-Identifier: MIT
# Include within the platform [Defines] section; no active mapping by default.
!ifndef $(CR_HDK_UART_HANDOFF)
  DEFINE CR_HDK_UART_HANDOFF = 0
!endif
!if $(CR_HDK_UART_HANDOFF) != 0 && $(CR_HDK_UART_HANDOFF) != 1
  !error CR_HDK_UART_HANDOFF must be 0 or 1
!endif
!if $(CR_HDK_UART_HANDOFF) == 1
!if "$(TARGET_DEVICE)" != "qcom-hdk8450"
  !error CR_HDK_UART_HANDOFF is supported only on qcom-hdk8450
!endif
!endif
