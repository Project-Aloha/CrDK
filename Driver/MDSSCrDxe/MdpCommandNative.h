/* Command pipeline backend of the shared MDSS native owner. GPL-2.0-only */
#pragma once
#include "MdpNativeResources.h"
#include "MdpNativeEvents.h"
#include "MdpPanelPower.h"
#include <Library/MdssDsiCmdCrLib.h>
typedef struct {
  MDP_NATIVE_RESOURCE_CONFIG Resources;
  MDP_PANEL_POWER_CONFIG Panel;
  EFI_GPIO_CR_PROTOCOL *Gpio;
  VOID *NotifyContext;
  /* Called outside the core operation after the first real frame IRQ and
   * revalidation, or a terminal startup error. Only exact SUCCESS permits
   * GOP publication. Context/storage stay alive with this retained owner. */
  VOID (*StartupComplete)(VOID *,EFI_STATUS);
} MDP_COMMAND_NATIVE_CONFIG;
typedef struct {
  VOID *Self;
  MDP_NATIVE_RESOURCES Resources;
  CR_MDSS_CMD Core;
  MDP_NATIVE_EVENTS Events;
  MDP_PANEL_POWER Panel;
  MDP_COMMAND_NATIVE_CONFIG Config;
  MDP_NATIVE_PROVIDERS Providers;
  CR_MDSS_CMD_PROFILE Profile;
  EFI_PHYSICAL_ADDRESS FramePhysical;
  UINTN FrameBytes;
  UINT64 Generation;
  UINT32 Stage;
  EFI_STATUS StartupStatus,FreezeStatus;
  BOOLEAN Initialized,Busy,Closing,Frozen,ObserverBound,CompletionNotified;
} MDP_COMMAND_NATIVE;
/* Stable zeroed storage. Pure admission precedes effects; every partial Open
 * retains exact storage, suppliers and callback identity. No second GOP or
 * native DXE is produced here. IRQ and board power use descriptor identity. */
EFI_STATUS MdpCommandNativeOpen(MDP_COMMAND_NATIVE *,CONST MDP_NATIVE_PROVIDERS *,
  CONST MDP_COMMAND_NATIVE_CONFIG *,CONST CR_MDSS_CMD_PROFILE *,EFI_PHYSICAL_ADDRESS,UINTN);
/* Accepted kickoff returns NOT_READY. Caller returns to the dispatcher to
 * deliver the real frame IRQ; no blocking first-frame polling or timer proof. */
EFI_STATUS MdpCommandNativeStart(MDP_COMMAND_NATIVE *);
EFI_STATUS MdpCommandNativeReady(MDP_COMMAND_NATIVE *);
EFI_STATUS MdpCommandNativePublish(MDP_COMMAND_NATIVE *,UINTN Offset,UINTN Bytes);
EFI_STATUS MdpCommandNativeBrightness(MDP_COMMAND_NATIVE *,UINT32);
EFI_STATUS MdpCommandNativeClose(MDP_COMMAND_NATIVE *);
/* Registered with SMMU before Start. Actual DMA fence, no boot-service
 * allocation/free, no panel/off RPC, no event lifetime change or OS handoff. */
EFI_STATUS EFIAPI MdpCommandNativeFreeze(VOID *);
