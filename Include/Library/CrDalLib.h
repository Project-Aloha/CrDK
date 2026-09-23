/** @file
  Typed accessors for data published by CrDALDxe.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_DAL_LIB_H_
#define CR_DAL_LIB_H_

#include <Library/CrTargetButtonsLib.h>
#include <Library/CrTargetInterconnectLib.h>
#include <Library/CrTargetPcieLib.h>
#include <Library/CrTargetPmicGpioLib.h>
#include <Library/CrTargetQupLib.h>
#include <Library/CrTargetSmmuLib.h>
#include <Library/CrTargetTrngLib.h>
#include <Library/clock.h>
#include <Library/debug_uart.h>
#include <Library/gpio.h>
#include <Library/interconnect.h>
#include <Library/pcie.h>
#include <Library/pdc.h>
#include <Library/rpmh.h>
#include <Library/spmi.h>
#include <Protocol/EFICrDalProtocol.h>

/**
  Accessors borrow provider storage until ExitBootServices; callers must not
  free it. Payloads returned as CONST are immutable, including child tables.
  Writable legacy contexts are one shared instance whose resource driver
  owns mutation and synchronization. A NULL result means the requested
  device or a compatible payload is unavailable; the generic APIs preserve
  the detailed EFI_STATUS for callers which need it.
**/

EFI_STATUS
CrDalGetDeviceInfo (
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  );

EFI_STATUS
CrDalGetDeviceCount (
  OUT UINTN *DeviceCount
  );

EFI_STATUS
CrDalGetDeviceByIndex (
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  );

RpmhDeviceContext       *CrDalGetRpmhContext (VOID);
ClockDriverContext      *CrDalGetClockContext (VOID);
DebugccDriverContext    *CrDalGetDebugClockContext (VOID);
CrDebugUartContext      *CrDalGetDebugUartContext (VOID);
GpioDeviceContext       *CrDalGetGpioContext (VOID);
PdcDeviceContext        *CrDalGetPdcContext (VOID);
CONST CR_TRNG_CONFIG    *CrDalGetTrngConfig (VOID);
CONST PcieTargetContext *CrDalGetPcieContext (VOID);
CONST InterconnectTargetContext *CrDalGetInterconnectContext (VOID);
CONST CrTargetSmmuContext       *CrDalGetSmmuContext (VOID);
CONST CR_TARGET_QUP_CONTEXT     *CrDalGetQupContext (VOID);
SpmiDeviceContext               *CrDalGetSpmiContext (VOID);
CONST PmicGpioTargetContext     *CrDalGetPmicGpioContext (VOID);
CONST ButtonsTargetContext      *CrDalGetButtonsContext (VOID);
CONST CR_DAL_CMD_DB_CONFIG       *CrDalGetCmdDbConfig (VOID);
CONST CR_DAL_LT9611_CONFIG       *CrDalGetLt9611Config (VOID);

CR_STATUS
CrDalClockInit (
  IN ClockDriverContext *ClockContext
  );

CR_STATUS
CrDalClockReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  );

CR_STATUS
CrDalClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  );

EFI_STATUS
CrDalGetPcieIo (
  OUT PcieIoOps *Io
  );

#endif
