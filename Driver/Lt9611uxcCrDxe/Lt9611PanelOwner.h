/* Retained exact HDK bridge panel owner; native provider boundary. MIT */
#pragma once
#include <Protocol/EFILt9611PanelCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
typedef struct {
  VOID *Context;
  EFI_STATUS (*Ready)(VOID *);
  BOOLEAN (*Lock)(VOID *);
  VOID (*Unlock)(VOID *);
  EFI_STATUS (*Prepare)(VOID *,CONST EFI_MDP_DSI_TIMING *);
  VOID (*Observe)(VOID *,UINT32 PinIndex,BOOLEAN High,EFI_STATUS);
} LT9611_PANEL_OWNER_IO;
typedef struct {
  EFI_LT9611_PANEL_CR_PROTOCOL Protocol;
  EFI_GPIO_CR_PROTOCOL *Gpio;
  LT9611_PANEL_OWNER_IO Io;
  VOID *Owner;
  UINT16 Pins[3];
  BOOLEAN RetainSupplies;
  BOOLEAN Initialized,Powered,Touched[3];
  /* Set before reset writes; a failed acknowledgment keeps MCU access gated. */
  BOOLEAN ResetHeld;
} LT9611_PANEL_OWNER;
/* The resident bridge has configured its verified GPIO9/109/107 outputs
   before Init. Confirm their actual level before publishing the service.
   No generic pin/config or guessed panel binding is accepted. A failed Init
   remains retained in this resident bridge's existing handoff journal. */
EFI_STATUS Lt9611PanelOwnerInit(LT9611_PANEL_OWNER *,EFI_GPIO_CR_PROTOCOL *,CONST LT9611_PANEL_OWNER_IO *);
/* Explicit immutable board routing, copied by value. RetainSupplies keeps
   shared enable outputs high during reset/off without withdrawing them. */
EFI_STATUS Lt9611PanelOwnerInitRoute(LT9611_PANEL_OWNER *,EFI_GPIO_CR_PROTOCOL *,
  CONST LT9611_PANEL_OWNER_IO *,CONST UINT16 Pins[3],BOOLEAN RetainSupplies);
