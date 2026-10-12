/* Qualified immutable DSI panel command/power profile. GPL-2.0-only */
#pragma once
#include <Library/MdssDsiCrLib.h>
#define CR_MDSS_PANEL_PROFILE_REVISION 1U
#define CR_MDSS_PANEL_MAX_COMMANDS 256U
#define CR_MDSS_PANEL_MAX_RESETS 16U
typedef struct {
  UINT8 DataType, ControllerMask;
  BOOLEAN LowPower;
  UINT8 Reserved;
  UINT32 DelayMs;
  CONST UINT8 *Payload;
  UINTN Bytes;
} CR_MDSS_PANEL_COMMAND;
typedef struct { BOOLEAN High; UINT8 Reserved[3]; UINT32 DelayUs; } CR_MDSS_PANEL_RESET;
typedef struct {
  UINT32 Revision;
  CR_MDSS_DSI_MODE Mode;
  CONST CR_MDSS_PANEL_COMMAND *On, *Off;
  UINT32 OnCount, OffCount;
  CONST CR_MDSS_PANEL_RESET *Reset;
  UINT32 ResetCount;
  BOOLEAN OffResetHigh;
} CR_MDSS_PANEL_PROFILE;
typedef struct {
  VOID *Context;
  /* These callbacks operate only the exact panel's already assigned GPIO,
     rail and power resources. Acquire/Release retain Owned after a partial
     failure; Release is an actual physical off fence, not metadata removal.
     Native retirement, serial leases and provider D0 remain caller-owned. */
  CR_STATUS (*Ready)(VOID *);
  CR_STATUS (*Acquire)(VOID *,BOOLEAN *Owned);
  CR_STATUS (*Release)(VOID *,BOOLEAN *Owned);
  CR_STATUS (*Reset)(VOID *,BOOLEAN High);
  CR_STATUS (*DelayUs)(VOID *,UINT32);
  CR_STATUS (*PublishPixels)(VOID *,VOID *,UINTN);
} CR_MDSS_PANEL_IO;
typedef struct {
  CR_MDSS_PANEL_PROFILE Profile;
  CR_MDSS_PANEL_IO Io;
  /* Profile arrays/payloads are borrowed immutable nonpaged storage. Their
     lifetime extends through successful Uninit; caller serializes all entry
     points and closes/drains them before retiring a dependency. */
  CR_MDSS_DSI *Dsi;
  BOOLEAN Initialized,PowerOwned,ResetTouched,Prepared,CleanupPending;
} CR_MDSS_DSI_PANEL;
/* Pure preflight accepts video modes already supported by BuildPlan. DSC
   properties are explicit; private firmware profile IDs are not guessed. */
CR_STATUS CrMdssPanelProfileValidate(CONST CR_MDSS_PANEL_PROFILE *);
CR_STATUS CrMdssDsiPanelInit(CR_MDSS_DSI_PANEL *,CONST CR_MDSS_PANEL_PROFILE *,CONST CR_MDSS_PANEL_IO *);
/* Actual CR_MDSS_EXT_PANEL callbacks: power/reset -> named host commands;
   DSC PPS remains the compositor's subsequent step. Off commands run only
   on a healthy command host; poisoned DMA is fenced by physical reset/power
   off. Any failed physical fence retains the full panel journal. */
CR_STATUS CrMdssDsiPanelPrepare(VOID *,CR_MDSS_DSI *,BOOLEAN *Owned);
CR_STATUS CrMdssDsiPanelUnprepare(VOID *,CR_MDSS_DSI *,BOOLEAN *Owned);
CR_STATUS CrMdssDsiPanelPublish(VOID *,VOID *,UINTN);
CR_STATUS CrMdssDsiPanelUninit(CR_MDSS_DSI_PANEL *);
