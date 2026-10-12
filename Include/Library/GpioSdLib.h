/* Source-qualified SM8450 HDK SDCC2 pad operations. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/gpio.h>
#define CR_GPIO_SD_CD_PIN 92U
#define CR_GPIO_SD_FIRST_PAD 211U
/* Explicit immutable native profile; never select a SoC by PinCount.
 * CLK/CMD/DATA are contiguous and use the fixed source-qualified masks and
 * sleep2mA, CLK no-pull and CMD/DATA pull-up policy. ActiveDriveMa
 * is CLK/CMD/DATA in mA; all-zero retains legacy16/16/16mA. */
typedef struct {
  UINT32 CdPin,FirstPad,PadControlOffset;
  UINT32 ActiveDriveMa[3];
} CR_GPIO_SD_PROFILE;
STATIC inline BOOLEAN CrGpioSdProfileDrivesValid(CONST CR_GPIO_SD_PROFILE *F) {
  if(F==NULL)return FALSE;
  if(!(F->ActiveDriveMa[0]|F->ActiveDriveMa[1]|F->ActiveDriveMa[2]))return TRUE;
  for(UINT32 I=0;I<3;++I)
    if(F->ActiveDriveMa[I]<2||F->ActiveDriveMa[I]>16||(F->ActiveDriveMa[I]&1U))return FALSE;
  return TRUE;
}
typedef struct {
  UINT32 OriginalPads;
  GpioConfigParams OriginalCd;
  BOOLEAN Snapshot;
} CR_GPIO_SD_SNAPSHOT;
/* Caller holds exclusive physical pad92/211/212/213 fences and provider D0.
 * Snapshot must precede every fallible write and stays immutable through
 * failed restores. Restore never changes bits outside the three SD pad fields.
 * No native ownership handoff, IRQ route or implicit provider lease is made. */
CR_STATUS CrGpioSdQualify(CONST GpioDeviceContext *);
CR_STATUS CrGpioSdSnapshot(GpioDeviceContext *,CR_GPIO_SD_SNAPSHOT *);
CR_STATUS CrGpioSdConfigureCd(GpioDeviceContext *,CONST CR_GPIO_SD_SNAPSHOT *);
CR_STATUS CrGpioSdSetActive(GpioDeviceContext *,BOOLEAN Active);
CR_STATUS CrGpioSdRestore(GpioDeviceContext *,CONST CR_GPIO_SD_SNAPSHOT *);
/* Native profile variants borrow one immutable profile through restore.
 * The owner retains that exact profile; snapshots record the exact CD pin,
 * so a different normal pin cannot redirect retained restore. */
CR_STATUS CrGpioSdQualifyProfile(CONST GpioDeviceContext *,CONST CR_GPIO_SD_PROFILE *);
CR_STATUS CrGpioSdSnapshotProfile(GpioDeviceContext *,CONST CR_GPIO_SD_PROFILE *,CR_GPIO_SD_SNAPSHOT *);
CR_STATUS CrGpioSdConfigureCdProfile(GpioDeviceContext *,CONST CR_GPIO_SD_PROFILE *,CONST CR_GPIO_SD_SNAPSHOT *);
CR_STATUS CrGpioSdSetActiveProfile(GpioDeviceContext *,CONST CR_GPIO_SD_PROFILE *,BOOLEAN Active);
CR_STATUS CrGpioSdRestoreProfile(GpioDeviceContext *,CONST CR_GPIO_SD_PROFILE *,CONST CR_GPIO_SD_SNAPSHOT *);

/* Windows class card detect is separate: these APIs never read/write CD92.
 * Caller owns only the three special-pad fences; all native combined APIs
 * above retain their original CD behavior. Snapshot before any pad write. */
typedef struct { UINT32 OriginalPads; BOOLEAN Snapshot; } CR_GPIO_SD_PADS_SNAPSHOT;
CR_STATUS CrGpioSdPadsQualify(CONST GpioDeviceContext *);
CR_STATUS CrGpioSdPadsSnapshot(GpioDeviceContext *,CR_GPIO_SD_PADS_SNAPSHOT *);
CR_STATUS CrGpioSdPadsSetActive(GpioDeviceContext *,BOOLEAN Active);
CR_STATUS CrGpioSdPadsRestore(GpioDeviceContext *,CONST CR_GPIO_SD_PADS_SNAPSHOT *);
