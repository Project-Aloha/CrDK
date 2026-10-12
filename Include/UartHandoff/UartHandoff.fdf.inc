# Include in FVMAIN, outside APRIORI; real ReadyToBoot retries service arrival.
# The parent build pipeline must reject any unvalidated qualified image.
# SPDX-License-Identifier: MIT
!if $(CR_HDK_UART_HANDOFF) == 1
!if "$(TARGET_DEVICE)" != "qcom-hdk8450"
  !error UART handoff FV requires qcom-hdk8450
!endif
  INF CranePkg/Driver/UartHandoffCrDxe/UartHandoffCrDxe.inf
!endif
