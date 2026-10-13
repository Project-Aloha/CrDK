/* Portable IRQ-woken physical button states for HID consumers. MIT */
#pragma once
#include <Library/CrTargetButtonsLib.h>
#include <oskal/cr_status.h>
typedef struct {
 VOID *Context;
 CR_STATUS (*Read)(VOID *,UINT8 Sid,UINT16 Address,UINT8 *,UINTN);
 CR_STATUS (*ReadGpio)(VOID *,VOID *Owner,UINT16 Pin,BOOLEAN *Level);
 /* Acquire installs its fence before fallible configuration. Every attempt
    requires Release, including errors. No PON line is an ordinary GPIO. */
 CR_STATUS (*AcquireGpio)(VOID *,VOID *Owner,UINT16 Pin);
 CR_STATUS (*ReleaseGpio)(VOID *,VOID *Owner,UINT16 Pin);
 CR_STATUS (*Register)(VOID *,CONST ButtonTarget *,CR_INTERRUPT_HANDLER,VOID *);
 CR_STATUS (*Enable)(VOID *,CONST ButtonTarget *,BOOLEAN);
 /* Success drains callbacks; failure retains caller context/registration. */
 CR_STATUS (*Unregister)(VOID *,CONST ButtonTarget *);
 VOID (*Wake)(VOID *);
 CR_STATUS (*Submit)(VOID *,UINT8 PressedMask);
} CR_BUTTON_INPUT_IO;
typedef struct {
 BOOLEAN Stable,Candidate,Armed,Registered;
 UINT64 Since;
} CR_BUTTON_INPUT_STATE;
typedef struct {
 CR_BUTTON_INPUT_IO Io;
 CONST ButtonsTargetContext *Target;
 CR_BUTTON_INPUT_STATE State[BUTTON_CODE_MAX];
 volatile UINT32 Dirty;
 UINT8 Reported;
 BOOLEAN Initialized,Stopping,GpioAttempted,ReportPending;
} CR_BUTTON_INPUT;
/* Caller serializes Start/Service/Stop. Wake may run <=DISPATCH; it schedules
    the serialized service path. No reads while idle: one-shot debounce only.
    Init is pure and validates the complete target before hardware access. */
CR_STATUS CrButtonInputInit(CR_BUTTON_INPUT *,CONST ButtonsTargetContext *,CONST CR_BUTTON_INPUT_IO *);
CR_STATUS CrButtonInputStart(CR_BUTTON_INPUT *,UINT64 NowUs);
CR_STATUS CrButtonInputService(CR_BUTTON_INPUT *,UINT64 NowUs);
CR_STATUS CrButtonInputStop(CR_BUTTON_INPUT *);
UINT32 CrButtonInputNextUs(CONST CR_BUTTON_INPUT *,UINT64 NowUs);
