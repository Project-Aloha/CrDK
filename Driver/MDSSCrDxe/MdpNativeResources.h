/* Shared native resource owner for command and video pipelines.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/CrTargetMdssLib.h>
#include <Library/MdssRetireCrLib.h>
#include <Protocol/EFIClockExternalCrProtocol.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/IoMmu.h>
#include <Pi/PiDxeCis.h>
typedef struct {
  EFI_CLOCK_CR_PROTOCOL *Clock;
  EFI_CLOCK_EXTERNAL_CR_PROTOCOL *External;
  EFI_RPMH_CR_PROTOCOL *Rpmh;
  EFI_INTERCONNECT_CR_PROTOCOL *Icb;
  EFI_SMMU_CR_PROTOCOL *Smmu;
  EDKII_IOMMU_PROTOCOL *Iommu;
  EFI_DXE_SERVICES *Dxe;
} MDP_NATIVE_PROVIDERS;
typedef struct {
  CONST CR_MDSS_TARGET *Target;
  UINT64 MdpHz,AverageKBps[2],PeakKBps[2];
  /* Keep the selected output in reset before disrupting inherited scanout.
   * Callback owns its GPIO/bridge lease and must be safe to retry. */
  EFI_STATUS (*BlankOutput)(VOID *);
  VOID *OutputContext;
} MDP_NATIVE_RESOURCE_CONFIG;
typedef struct {
  CONST CR_MDSS_HW_PROFILE *Hardware;
  MDP_NATIVE_PROVIDERS Providers;
  MDP_NATIVE_RESOURCE_CONFIG Config;
  CR_MDSS_RETIRE Retirement;
  EFI_SMMU_CR_DISPLAY_GROUP Group;
  EFI_SMMU_CR_DISPLAY_LEASE FrameLease,CommandLease;
  VOID *FrameCpu,*CommandCpu;
  UINTN FrameBytes;
  UINT64 FrameIova,CommandIova,OwnerGeneration,Incarnation;
  INTERCONNECT_PATH_HANDLE Paths[2];
  UINT32 ControllerMask,Stage,IccResetStage;
  BOOLEAN RailOwned[2],ClockOwned[7],ExternalOwned;
  BOOLEAN ClockReady[7],FunctionalReady;
  BOOLEAN Qualified,RailReady[2],PathReady[2],ExternalReady;
  BOOLEAN Initialized,AccessReady,WritersSealed,Closing;
  BOOLEAN OutputTouched,OutputBlanked;
} MDP_NATIVE_RESOURCES;
/* One stable native owner, serialized by its containing frontend. Admission
 * is pure; all MMIO windows are qualified before acquiring resources. Open
 * retains every partial journal on error. Once retirement is attempted,
 * Close retains storage/resources and cannot unload the typed group owner.
 * The resource owner is permanent until reset; frame backing stays Reserved.
 * Open retries supplier acquisition and the exact retained boot/reset stage;
 * a failed own supplier vote is drained before reacquisition. */
EFI_STATUS MdpNativeResourcesOpen(MDP_NATIVE_RESOURCES *,CONST MDP_NATIVE_PROVIDERS *,
  CONST MDP_NATIVE_RESOURCE_CONFIG *,UINT32 ControllerMask,EFI_PHYSICAL_ADDRESS,UINTN);
EFI_STATUS MdpNativeResourcesClose(MDP_NATIVE_RESOURCES *);
/* GPIO/bridge operations need the retained suppliers, but must precede
 * functional-clock rebuild and DMA admission. Does not grant MDSS MMIO. */
EFI_STATUS MdpNativeResourcesSuppliersReady(MDP_NATIVE_RESOURCES *);
/* Lightweight exact producer journal admission; no page-table walk. */
EFI_STATUS MdpNativeResourcesReady(MDP_NATIVE_RESOURCES *);
/* Physical mapping proof at Start/publication/freeze operation boundaries. */
EFI_STATUS MdpNativeResourcesCheck(MDP_NATIVE_RESOURCES *);
EFI_STATUS MdpNativeResourcesRead(MDP_NATIVE_RESOURCES *,CR_MDSS_RETIRE_WINDOW,UINT32,UINT32 *);
EFI_STATUS MdpNativeResourcesWrite(MDP_NATIVE_RESOURCES *,CR_MDSS_RETIRE_WINDOW,UINT32,UINT32);
EFI_STATUS MdpNativeResourcesFence(MDP_NATIVE_RESOURCES *);
EFI_STATUS MdpNativeResourcesDelay(MDP_NATIVE_RESOURCES *,UINT32);
EFI_STATUS MdpNativeResourcesSetDsiPlan(MDP_NATIVE_RESOURCES *,UINT32,
  CONST CR_CLOCK_DSI_PLAN *,BOOLEAN,BOOLEAN *);
EFI_STATUS MdpNativeResourcesPublishFrame(MDP_NATIVE_RESOURCES *,VOID *,UINTN);
EFI_STATUS MdpNativeResourcesPublishCommand(MDP_NATIVE_RESOURCES *,VOID *,UINTN);
