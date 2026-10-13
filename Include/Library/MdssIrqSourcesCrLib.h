/* SM8450 shared MDSS IRQ source observation. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/MdssDpuInterruptCrLib.h>
#include <Library/MdssDpInterruptCrLib.h>

typedef struct {
  CR_MDSS_IRQ_WINDOW Window; /* WindowCount when no offending register. */
  UINT32 Offset;
  UINT32 Value;
  UINT32 ForbiddenMask;
  UINT32 ParentStatus;
  UINT32 Top0Enabled;
  UINT32 Top0Pending;
} CR_MDSS_IRQ_SOURCE_OBSERVATION;

/* Read-only observation, never a resource, power, or writer-retirement grant.
 * The caller holds all six exact SM8450 views powered, has retired foreign
 * writers, and serializes this call against its own enable/ack operations.
 * AllowedTop0 is a subset of CR_MDSS_IRQ_SUPPORTED; use zero before connecting
 * the physical interrupt. Other known source enables must be zero. Disabled
 * latched status is allowed; no W1C or mask register is written here.
 * CR_SUCCESS: all enables and final parent status meet that policy.
 * CR_NOT_READY with updated Out: first enabled foreign bank, or unexplained
 * final parent status. On callback failure (including a nonfinal successful
 * status normalized to CR_NOT_READY), Out is unchanged. Output is diagnostic,
 * not an authorization token. Write32 is not required or used.
 */
CR_STATUS CrMdssIrqSourcesQuiet(CONST CR_MDSS_IRQ_IO *Io,UINT32 AllowedTop0,
                              CR_MDSS_IRQ_SOURCE_OBSERVATION *Out);
/* Same actual interrupt lock and exact copied IO projection as Dp. Only that
 * registered, armed child's AUX masks/pending may be explained. No HPD/HDCP
 * allowance, generic DP enable mask or physical connect grant is introduced. */
CR_STATUS CrMdssIrqSourcesQuietWithDp(CONST CR_MDSS_IRQ_IO *,UINT32 AllowedTop0,
    CONST CR_MDSS_DP_IRQ_CORE *,CR_MDSS_IRQ_SOURCE_OBSERVATION *);
