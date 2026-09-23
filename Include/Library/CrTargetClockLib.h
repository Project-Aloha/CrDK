#pragma once

#include <CrDalDevice.h>
#include <Library/clock.h>
#include <oskal/common.h>

ClockDriverContext   *CrTargetGetClockContext (VOID);
DebugccDriverContext *CrTargetGetDebugccContext (VOID);
CR_STATUS             CrTargetClockInit (ClockDriverContext *ClockContext);

CR_STATUS
CrTargetClockReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  );

CR_STATUS
CrTargetClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  );
