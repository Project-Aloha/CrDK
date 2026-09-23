/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#include <Protocol/HardwareInterrupt2.h>

STATIC CR_INTERRUPT_ENTRY mCrIntTable[MAX_INTERRUPT_ENTRIES];
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL *mHwInterrupt2;

VOID EFIAPI CrInternalInterruptEntry(IN HARDWARE_INTERRUPT_SOURCE Source,
                                     IN EFI_SYSTEM_CONTEXT SystemContext) {
  CR_INTERRUPT_ENTRY *Entry = NULL;
  (VOID)SystemContext;
  for (UINTN i = 0; i < MAX_INTERRUPT_ENTRIES; i++) {
    if ((mCrIntTable[i].Source == Source) &&
        (mCrIntTable[i].Handler != NULL)) {
      Entry = &mCrIntTable[i];
      Entry->Handler(Entry->Param);
      break;
    }
  }
  // End of interrupt handling
  mHwInterrupt2->EndOfInterrupt(mHwInterrupt2, Source);
}

CR_STATUS
CrInterruptInit(VOID) {
  CR_STATUS Status;
  if (gBS == NULL || gBS->LocateProtocol == NULL) {
    return CR_NOT_FOUND;
  }
  // Locate Hardware Interrupt2 Protocol
  mHwInterrupt2 = NULL;
  Status = gBS->LocateProtocol(&gHardwareInterrupt2ProtocolGuid, NULL,
                               (VOID **)&mHwInterrupt2);
  if (CR_ERROR(Status)) {
    mHwInterrupt2 = NULL;
    log_err("HardwareInterrupt2 Protocol not found: %r", Status);
    return CR_NOT_FOUND;
  }
  if (mHwInterrupt2 == NULL ||
      mHwInterrupt2->RegisterInterruptSource == NULL ||
      mHwInterrupt2->EnableInterruptSource == NULL ||
      mHwInterrupt2->DisableInterruptSource == NULL ||
      mHwInterrupt2->EndOfInterrupt == NULL ||
      mHwInterrupt2->SetTriggerType == NULL) {
    mHwInterrupt2 = NULL;
    return CR_UNSUPPORTED;
  }
  return CR_SUCCESS;
}

STATIC inline EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE
CrInterruptTypeTanslate(IN CR_INTERRUPT_TRIGGER_TYPE Type) {
  switch (Type) {
  case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_LOW;
  case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH;
  case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_FALLING;
  case CR_INTERRUPT_TRIGGER_EDGE_RISING:
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_RISING;
  case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
    // Not supported directly
    return EFI_HARDWARE_INTERRUPT2_TRIGGER_EDGE_RISING;
  default:
    return 0xFF;
  }
}

STATIC
CR_INTERRUPT_ENTRY *
FindInterruptEntry(IN UINT32 InterruptNumber)
{
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES; Index++) {
    if (mCrIntTable[Index].Handler != NULL &&
        mCrIntTable[Index].Source == InterruptNumber) {
      return &mCrIntTable[Index];
    }
  }
  return NULL;
}

CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig)
{
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  CR_INTERRUPT_ENTRY *Entry;
  EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE TriggerType;

  if (InterruptConfig == NULL) {
    return CR_INVALID_PARAMETER;
  }
  if (InterruptConfig->Handler == NULL) {
    return CrUnregisterInterrupt(InterruptConfig);
  }
  if (mHwInterrupt2 == NULL && CR_ERROR(CrInterruptInit())) {
    return CR_NOT_FOUND;
  }

  TriggerType = CrInterruptTypeTanslate(InterruptConfig->TriggerType);
  if (TriggerType == 0xFF) {
    return CR_INVALID_PARAMETER;
  }
  if (FindInterruptEntry(InterruptConfig->InterruptNumber) != NULL) {
    return CR_BUSY;
  }

  Entry = NULL;
  for (UINTN Index = 0; Index < MAX_INTERRUPT_ENTRIES; Index++) {
    if (mCrIntTable[Index].Handler == NULL) {
      Entry = &mCrIntTable[Index];
      break;
    }
  }
  if (Entry == NULL) {
    return CR_OUT_OF_RESOURCES;
  }

  /* MU's RegisterInterruptSource may enable the interrupt immediately, so
   * publish the dispatch entry before installing the wrapper. */
  Entry->Handler = InterruptConfig->Handler;
  Entry->Param = InterruptConfig->Param;
  Entry->Source = InterruptConfig->InterruptNumber;
  Status = mHwInterrupt2->RegisterInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber, CrInternalInterruptEntry);
  if (Status == EFI_ALREADY_STARTED) {
    /* An unrelated driver owns this source.  Never unregister its callback. */
    Entry->Handler = NULL;
    Entry->Param = NULL;
    Entry->Source = 0;
    return Status;
  }
  if (CR_ERROR(Status)) {
    goto Rollback;
  }

  Status = mHwInterrupt2->SetTriggerType(
      mHwInterrupt2, InterruptConfig->InterruptNumber, TriggerType);
  if (CR_ERROR(Status)) {
    goto Rollback;
  }
  Status = mHwInterrupt2->EnableInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber);
  if (!CR_ERROR(Status)) {
    return CR_SUCCESS;
  }

Rollback:
  /* Keep the entry if removal fails.  The owning DXE image must then remain
   * resident until a later cleanup succeeds; losing the entry would leave a
   * hardware callback into an image which the dispatcher can unload. */
  CleanupStatus = CrUnregisterInterrupt(InterruptConfig);
  return CR_ERROR(CleanupStatus) ? CleanupStatus : Status;
}

CR_STATUS
CrUnregisterInterrupt(IN CR_INTERRUPT_CONFIG *InterruptConfig)
{
  EFI_STATUS Status;
  CR_INTERRUPT_ENTRY *Entry;

  if (InterruptConfig == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Entry = FindInterruptEntry(InterruptConfig->InterruptNumber);
  if (Entry == NULL) {
    return CR_NOT_FOUND;
  }
  if (InterruptConfig->Handler != NULL &&
      (Entry->Handler != InterruptConfig->Handler ||
       Entry->Param != InterruptConfig->Param)) {
    return CR_BUSY;
  }
  if (mHwInterrupt2 == NULL && CR_ERROR(CrInterruptInit())) {
    /* A retained entry still represents a callback ownership obligation. */
    return CR_DEVICE_ERROR;
  }

  Status = mHwInterrupt2->DisableInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = mHwInterrupt2->RegisterInterruptSource(
      mHwInterrupt2, InterruptConfig->InterruptNumber, NULL);
  if (CR_ERROR(Status)) {
    return Status;
  }

  Entry->Handler = NULL;
  Entry->Param = NULL;
  Entry->Source = 0;
  return CR_SUCCESS;
}
