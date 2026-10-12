/* Immutable native SDCC2 PIO resources. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/GpioSdLib.h>

/* Revision 2 adds the per-pad active drive profile. */
#define CR_TARGET_SD_REVISION 2U
#define CR_TARGET_SDHC_V5 5U
typedef struct {
  CONST CHAR8 *Name;
  UINT32 VoltageMv;
  UINT8 Mode;
} CR_TARGET_SD_RAIL;
typedef struct {
  CONST CHAR8 *Provider;
  UINT32 Source,Destination;
  UINT64 Average,Peak; /* Linux kB/s, bounded supported SDR ceiling. */
} CR_TARGET_SD_PATH;
typedef struct {
  UINT32 Revision,Size,Variant;
  UINT64 Base;
  UINT32 Bytes,PowerInterrupt;
  CR_INTERRUPT_TRIGGER_TYPE PowerTrigger;
  CONST CHAR8 *ClockController;
  CONST CHAR8 *Clocks[2]; /* AHB then APPS; the host selects supported rates. */
  CR_TARGET_SD_RAIL Rails[2]; /* VDD then VDD-IO; exact reverse withdrawal. */
  CR_TARGET_SD_PATH Paths[2]; /* Data then configuration; acquired before clocks. */
  CR_GPIO_SD_PROFILE Gpio;
  BOOLEAN CardDetectActiveLow; /* ReadSdCardDetect continues returning raw HIGH. */
} CR_TARGET_SD_CONTEXT;

/* Metadata preflight only. No allocation, protocol acquisition or MMIO.
 * The provider owns immutable strings/storage through ExitBootServices.
 * GPIO additionally checks the actual pin/register table before any access.
 * This ABI supports the known v5, 4-bit SDR/PIO topology only. */
STATIC inline BOOLEAN CrTargetSdString(CONST CHAR8 *S) {
  if(S==NULL||S[0]==0)return FALSE;
  for(UINT32 I=1;I<64;++I)if(S[I]==0)return TRUE;
  return FALSE;
}
STATIC inline BOOLEAN CrTargetSdValid(CONST CR_TARGET_SD_CONTEXT *C) {
  if(C==NULL||C->Revision!=CR_TARGET_SD_REVISION||C->Size!=sizeof(*C)||
     C->Variant!=CR_TARGET_SDHC_V5||!C->Base||(C->Base&0xfff)||C->Bytes!=0x1000||
     C->Base>(UINT64)(~(UINTN)0)-C->Bytes||C->PowerInterrupt<32||C->PowerInterrupt>=1020||
     C->PowerTrigger!=CR_INTERRUPT_TRIGGER_LEVEL_HIGH||C->CardDetectActiveLow>1||
     !CrTargetSdString(C->ClockController)||!CrGpioSdProfileDrivesValid(&C->Gpio)||C->Gpio.CdPin>=GPIO_PINS_MAX||
     C->Gpio.FirstPad>GPIO_PINS_MAX-3||!C->Gpio.PadControlOffset||(C->Gpio.PadControlOffset&3)||
     (C->Gpio.CdPin>=C->Gpio.FirstPad&&C->Gpio.CdPin<C->Gpio.FirstPad+3))return FALSE;
  for(UINT32 I=0;I<2;++I)if(!CrTargetSdString(C->Clocks[I])||
     !CrTargetSdString(C->Rails[I].Name)||!C->Rails[I].VoltageMv||C->Rails[I].VoltageMv>5000||
     C->Rails[I].Mode!=7||!CrTargetSdString(C->Paths[I].Provider)||
     C->Paths[I].Source==C->Paths[I].Destination||!C->Paths[I].Average||
     C->Paths[I].Peak<C->Paths[I].Average)return FALSE;
  return TRUE;
}
CONST CR_TARGET_SD_CONTEXT *CrTargetGetSdContext(VOID);
