/* Explicit panel selection within the shared MDSS frontend. MIT */
#pragma once
#include <Uefi.h>
#define MDP_SELECTED_R66451_COMMAND 1U
#define MDP_SELECTED_LT9611_VIDEO 2U
EFI_STATUS MdpSelectedPanelStart(UINT32 Profile);
EFI_STATUS MdpSelectedPanelUnload(VOID);
BOOLEAN MdpSelectedPanelRetained(VOID);
