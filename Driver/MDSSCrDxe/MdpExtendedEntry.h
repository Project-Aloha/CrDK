/* Selected-panel caller of the operative extended GOP owner. GPL-2.0-only */
#pragma once
#include "MdpExtendedOutput.h"
#include <Protocol/EFIMdssPanelProfileCrProtocol.h>
typedef struct {
  MDP_EXTENDED_OUTPUT Output;
  EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *Source;
  EFI_MDSS_PANEL_SELECTION Selection;
  BOOLEAN SourceOwned, PanelOwned, Busy, Initialized;
} MDP_EXTENDED_ENTRY;
/* Caller owns permanent zeroed storage and external callback admission. The
   selected producer and all provider protocols are actually installed. Failed
   Start may retain Initialized/SourceOwned/Output: Close must complete before
   disposing storage. Published GOPs deliberately cannot be unloaded. */
EFI_STATUS MdpExtendedEntryStart(MDP_EXTENDED_ENTRY *,EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *);
EFI_STATUS MdpExtendedEntryClose(MDP_EXTENDED_ENTRY *);
