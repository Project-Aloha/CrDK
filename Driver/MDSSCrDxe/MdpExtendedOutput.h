/* Retained native GOP owner for qualified extended panels. GPL-2.0-only */
#pragma once
#include "MdpExtendedNative.h"
#include <Protocol/GraphicsOutput.h>
#include <Protocol/EFIDisplayPwr.h>
#include <Protocol/EFIMdssHandoffProtocol.h>
#include <Configuration/DisplayDevicePath.h>
#include <Library/FrameBufferBltLib.h>
typedef struct {
  EFI_STATUS (*Ready)(VOID *);
  EFI_STATUS (*Publish)(VOID *,UINTN Offset,UINTN Bytes);
  EFI_STATUS (*Freeze)(VOID *);
  EFI_STATUS (*Close)(VOID *);
  EFI_STATUS (*SetPower)(VOID *,EFI_DISPLAY_POWER_STATE);
  EFI_STATUS (*GetPower)(VOID *,EFI_DISPLAY_POWER_STATE *);
} MDP_OUTPUT_PIPELINE;
typedef struct {
  MDP_EXTENDED_NATIVE Native;
  MDP_OUTPUT_PIPELINE Pipeline;
  VOID *PipelineContext;
  EFI_SMMU_CR_PROTOCOL *PrepareSmmu;
  BOOLEAN PipelineFrozen;
  EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
  EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE Mode;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
  EFI_DISPLAY_POWER_PROTOCOL Power;
  EFI_DISPLAY_DEVICE_PATH Path;
  CR_MDSS_DSI_MODE SelectedMode;
  EFI_MDSS_HANDOFF_PROTOCOL *Handoff;
  EFI_DXE_SERVICES *Dxe;
  EFI_EVENT ExitEvent;
  EFI_HANDLE Handle;
  FRAME_BUFFER_CONFIGURE *BltConfig;
  UINTN BltBytes, CacheBytes;
  UINT64 OriginalAttributes;
  EFI_STATUS FreezeStatus;
  BOOLEAN Initialized, Busy, Exiting, Frozen, Published, PublicationPending;
  BOOLEAN Typed, PrepareBound;
  BOOLEAN Registered, AttributesSaved, AttributesTouched, CacheReady;
} MDP_EXTENDED_OUTPUT;
/* Actual board-selected profile and stable owner storage, not a readiness
   flag. NativePanel owns reset/power; profile payloads stay alive through the
   retained owner. Publish is permanent: no unloading or reusing this storage
   after successful or uncertain GOP publication. Existing HDK LT9611 remains
   selected unless the platform explicitly calls this owner instead.
   Control/lifetime calls <=TPL_CALLBACK; GOP QueryMode/SetMode/Blt also
   support TPL_NOTIFY without starting/stopping a pipeline. Freeze/Snapshot
   are the handoff callbacks.
   The current wire cannot describe bonded/CPHY/DSC takeover completely, so
   its snapshot is explicitly nontransferable diagnostics until that wire is
   extended. EBS nevertheless closes callbacks and stops actual transient DMA. */
EFI_STATUS MdpExtendedOutputOpen(MDP_EXTENDED_OUTPUT *, CONST MDP_EXTENDED_PROVIDERS *,
  CONST MDP_EXTENDED_NATIVE_CONFIG *, CONST CR_MDSS_PANEL_PROFILE *,
  EFI_PHYSICAL_ADDRESS, UINTN, EFI_MDSS_HANDOFF_PROTOCOL *);
EFI_STATUS MdpExtendedOutputClose(MDP_EXTENDED_OUTPUT *);
EFI_STATUS EFIAPI MdpExtendedOutputFreeze(VOID *);
EFI_STATUS EFIAPI MdpExtendedOutputSnapshot(VOID *,CR_MDSS_HANDOFF_PARTICIPANT *,VOID **);

/* Alternate pipeline uses the same GOP callbacks/publication and exact
   retained framebuffer owner. Init is pure apart from its exit event;
   configure follows producer retirement/WC. Publish requires real ACTIVE. */
EFI_STATUS MdpExtendedOutputInitPipeline(MDP_EXTENDED_OUTPUT *,EFI_DXE_SERVICES *,
  CONST MDP_OUTPUT_PIPELINE *,VOID *,EFI_PHYSICAL_ADDRESS,UINT32 Width,UINT32 Height,UINT32 Stride);
EFI_STATUS MdpExtendedOutputConfigureFramebuffer(MDP_EXTENDED_OUTPUT *);
EFI_STATUS MdpExtendedOutputBindPrepare(MDP_EXTENDED_OUTPUT *,EFI_SMMU_CR_PROTOCOL *);
EFI_STATUS MdpExtendedOutputPublishPipeline(MDP_EXTENDED_OUTPUT *);
