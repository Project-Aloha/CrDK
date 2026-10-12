/* Charging UI state and raster drawing. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#define CR_CHARGER_IDLE_US 30000000ULL
#define CR_CHARGER_HOLD_US 3000000ULL
#define CR_CHARGER_ANIMATION_US 500000ULL
#define CR_CHARGER_BUTTON_GAP_US 250000ULL
#define CR_CHARGER_CANVAS_WIDTH 192U
#define CR_CHARGER_CANVAS_HEIGHT 288U
typedef struct {
  BOOLEAN ButtonsValid, PowerPressed, OtherPressed;
  BOOLEAN SocValid, Charging;
  UINT32 SocHundredths;
  UINT64 PowerPressSequence;
} CR_CHARGER_UI_INPUT;
typedef struct {
  VOID *Context;
  EFI_STATUS (*Power)(VOID *,BOOLEAN On);
  EFI_STATUS (*Draw)(VOID *,CONST CR_CHARGER_UI_INPUT *,UINT32 Animation);
  EFI_STATUS (*Reboot)(VOID *);
} CR_CHARGER_UI_IO;
typedef struct {
  CR_CHARGER_UI_IO Io;
  UINT64 LastTime, LastActivity, PowerSince, LastButtonTime, LastDraw;
  UINT32 LastSoc, Animation;
  UINT64 LastPowerPressSequence;
  BOOLEAN PressSequenceKnown;
  BOOLEAN Initialized, ScreenKnown, ScreenOn, PowerHeld, OtherHeld;
  BOOLEAN LastSocValid, LastCharging, Drawn, ResetIssued;
} CR_CHARGER_UI;
EFI_STATUS CrChargerUiInit(CR_CHARGER_UI *,CONST CR_CHARGER_UI_IO *,UINT64 NowUs);
EFI_STATUS CrChargerUiStep(CR_CHARGER_UI *,CONST CR_CHARGER_UI_INPUT *,UINT64 NowUs);
/* Caller owns exact bounded BGRX canvas. No fonts/bitmaps/assets or framebuffer
   assumptions; the app publishes through the selected physical GOP. */
EFI_STATUS CrChargerUiRender(EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Pixels,UINTN PixelCount,
    CONST CR_CHARGER_UI_INPUT *,UINT32 Animation);
