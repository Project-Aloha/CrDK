/* SM8450 DPU8.1 bonded DSI scanout and DSC composition. GPL-2.0-only */
#pragma once
#include <Library/MdssDsiCrLib.h>
typedef struct {
 CR_MDSS_DSI *Dsi;
 CR_MDSS_DSC Dsc[2];
 UINT64 DeviceAddress;
 UINTN BufferBytes;
 BOOLEAN Initialized,Prepared,Active,Touched,CleanupPending;
} CR_MDSS_DPU_EXT;
/* Caller owns DMA0/1, CTL0, LM0/1, PP0/1, INTF1/2, corresponding full DCE
   and MDP split controls. This conflicts with any other owner of those exact
   resources. DeviceAddress is from the real DMA provider; Dpu Ready proves
   its lifetime together with the same exclusive hardware/power leases. */
CR_STATUS CrMdssDpuExtendedInit(CR_MDSS_DPU_EXT *,CR_MDSS_DSI *,UINT64 DeviceAddress,UINTN BufferBytes);
CR_STATUS CrMdssDpuExtendedPrepare(CR_MDSS_DPU_EXT *);
CR_STATUS CrMdssDpuExtendedEnable(CR_MDSS_DPU_EXT *);
CR_STATUS CrMdssDpuExtendedStop(CR_MDSS_DPU_EXT *);
CR_STATUS CrMdssDpuExtendedUninit(CR_MDSS_DPU_EXT *);
typedef struct {
 VOID *Context;
 /* Real panel/bridge GPIO/serial and command preparation. Owns its progress
    journal; partial errors may retain Owned. Commands use the prepared Dsi
    transport. An off callback must also handle a poisoned command engine by
    physically resetting/powering down its exact panel resources. */
 CR_STATUS (*Prepare)(VOID *,CR_MDSS_DSI *,BOOLEAN *Owned);
 CR_STATUS (*Unprepare)(VOID *,CR_MDSS_DSI *,BOOLEAN *Owned);
 CR_STATUS (*PublishPixels)(VOID *,VOID *Cpu,UINTN Bytes);
} CR_MDSS_EXT_PANEL;
typedef struct {
 CR_MDSS_DSI Dsi;
 CR_MDSS_DPU_EXT Dpu;
 CR_MDSS_EXT_PANEL Panel;
 VOID *Cpu;
 UINTN Bytes;
 BOOLEAN Initialized,PanelOwned,Active,CleanupPending;
} CR_MDSS_EXT_DISPLAY;
/* External caller serialization covers this complete stable object, every
   callback and quarantine/removal. Caller retains all qualified maps/leases
   after any error until Stop/Uninit succeeds. No generic board/panel readiness
   or native retirement is inferred from a mode, tuple or physical address. */
CR_STATUS CrMdssExtendedInit(CR_MDSS_EXT_DISPLAY *,CONST CR_MDSS_EXT_IO *,
 CONST CR_MDSS_EXT_PANEL *,CONST CR_MDSS_DSI_MODE *,VOID *Cpu,UINT64 Iova,UINTN Bytes,
 CONST CR_MDSS_DSI_COMMAND_MEMORY *Commands);
CR_STATUS CrMdssExtendedStart(CR_MDSS_EXT_DISPLAY *);
CR_STATUS CrMdssExtendedStop(CR_MDSS_EXT_DISPLAY *);
CR_STATUS CrMdssExtendedUninit(CR_MDSS_EXT_DISPLAY *);
