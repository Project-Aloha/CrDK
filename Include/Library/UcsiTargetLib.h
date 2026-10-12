/* Board-qualified native ADSP/PMIC transport adapter. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
typedef struct {
  UINT32 Soc,Board;
  UINTN IpccBase;
  UINT32 IpccSource;
  UINT16 OrientationGpio;
  BOOLEAN ReverseHigh,LeaseOrientation;
  /* NULL: public board binds PD to its ADSP PMIC service and describes no AP
   * local negotiation rail. This never authorizes a guessed PMIC power vote. */
  CONST CHAR8 *PdRail;
  UINT32 PdMillivolts;
  UINT8 PdMode;
} CR_UCSI_TARGET;
CONST CR_UCSI_TARGET *CrUcsiGetTarget(VOID);
