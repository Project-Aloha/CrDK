/* SM8450 HDK SDCC2 native SD/SDHC PIO host. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Protocol/MmcHost.h>
#include <Library/SdhciMsmLib.h>
#include <Library/CrDalLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#define CR_SD_NATIVE_BASE 0x08804000U
#define CR_SD_NATIVE_BYTES 0x1000U
typedef struct {
  EFI_MMC_HOST_PROTOCOL Host;
  CR_SDHC Core;
  CONST CR_TARGET_SD_CONTEXT *Target;
  EFI_HANDLE Handle;
  EFI_EVENT ExitEvent;
  BOOLEAN Acquired,Published,PublicationPending,Exiting,Fencing,Busy,Initialized,Application,ResponseValid;
  BOOLEAN DataPending,DataRead;
  BOOLEAN CardClockStopped;
  BOOLEAN InheritedDmaStopped;
  UINT16 DataLength;
  CR_SDHC_RESPONSE ResponseType;
  UINT32 Events,Errors,Clock;
  UINT32 LastCommand,LastArgument;
  UINT32 WaitPolls,FirstIrq,SeenIrq,TraceCommands;
  BOOLEAN LastApplication;
  CONST CHAR8 *RequestStage;
  UINT8 Response[16],Scratch[512];
} CR_SD_NATIVE;
extern CR_SD_NATIVE mSdHost;
EFI_STATUS CrSdHostInit(VOID);
EFI_STATUS CrSdHostStopClock(VOID);
EFI_STATUS CrSdHostQuiesce(VOID);
