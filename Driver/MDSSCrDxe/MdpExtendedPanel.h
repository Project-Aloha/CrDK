/* Typed extended caller profile built from owned native XML. GPL-2.0-only */
#pragma once
#include "MdpPanel.h"
#include <Library/MdssDsiPanelCrLib.h>
typedef struct {
  CR_MDSS_PANEL_PROFILE Profile;
  CR_MDSS_PANEL_COMMAND On[CR_MDSS_PANEL_MAX_COMMANDS],Off[CR_MDSS_PANEL_MAX_COMMANDS];
} MDP_EXTENDED_PANEL;
/* Pure preflight. Output has stable caller-owned storage. Panel's owned
   command payloads and explicit board-qualified Reset array remain alive
   until the extended panel/display has stopped and Uninit succeeds.
   Compression is mandatory iff XML DSC is enabled: explicit verified bpc/
   bpp/version/geometry, never a translation guessed from private ProfileID.
   No pin/electrical/native/DMA grant follows from this conversion. */
EFI_STATUS MdpBuildExtendedPanel(CONST MDP_PANEL_CONFIG *,CONST CR_MDSS_DSC_PARAMS *,
    CONST UINT8 LaneMap[4],BOOLEAN ContinuousClock,CONST CR_MDSS_PANEL_RESET *,UINT32 ResetCount,
    BOOLEAN OffResetHigh,MDP_EXTENDED_PANEL *);
EFI_STATUS MdpBuildExtendedPanelForSoc(UINT32 Soc,CONST MDP_PANEL_CONFIG *,
    CONST CR_MDSS_DSC_PARAMS *,CONST UINT8 LaneMap[4],BOOLEAN ContinuousClock,
    CONST CR_MDSS_PANEL_RESET *,UINT32 ResetCount,BOOLEAN OffResetHigh,
    MDP_EXTENDED_PANEL *);
