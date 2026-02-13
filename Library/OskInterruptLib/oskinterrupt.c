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
  for (UINTN i = 0; i < MAX_INTERRUPT_ENTRIES; i++) {
    if ((mCrIntTable[i].Source == Source) && Source &&
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
  // Locate Hardware Interrupt2 Protocol
  Status = gBS->LocateProtocol(&gHardwareInterrupt2ProtocolGuid, NULL,
                               (VOID **)&mHwInterrupt2);
  if (CR_ERROR(Status)) {
    log_err("HardwareInterrupt2 Protocol not found: %r", Status);
    CR_ASSERT(FALSE);
    return CR_NOT_FOUND;
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

CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig) {
  EFI_STATUS Status;
  UINT32 InterruptNumber = InterruptConfig->InterruptNumber;
  CR_INTERRUPT_HANDLER InterruptHandler = InterruptConfig->Handler;
  VOID *Param = InterruptConfig->Param;
  CR_INTERRUPT_TRIGGER_TYPE CrTriggerType = InterruptConfig->TriggerType;

  // Assuming disabling interrupt
  if (InterruptHandler == NULL) {
    return CrUnregisterInterrupt(InterruptConfig);
  }

  // Initialize protocol if not done already
  if (mHwInterrupt2 == NULL)
    if (CrInterruptInit() != CR_SUCCESS)
      return CR_NOT_FOUND;

  // Translate trigger type
  EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE TriggerType =
      CrInterruptTypeTanslate(CrTriggerType);
  if (TriggerType == 0xFF) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid trigger type %d", __FUNCTION__,
            CrTriggerType);
    return CR_INVALID_PARAMETER;
  }

  // Find a free entry
  {
    UINTN i;
    for (i = 0; i < MAX_INTERRUPT_ENTRIES; i++) {
      if (mCrIntTable[i].Handler == NULL) {
        mCrIntTable[i].Handler = InterruptHandler;
        mCrIntTable[i].Param = Param;
        mCrIntTable[i].Source = InterruptNumber;
        break;
      }
    }
    if (i == MAX_INTERRUPT_ENTRIES) {
      log_err(CR_LOG_CHAR8_STR_FMT ": No free interrupt entries available",
              __FUNCTION__);
      return CR_OUT_OF_RESOURCES;
    }
  }

  // Register irq handler
  Status = mHwInterrupt2->RegisterInterruptSource(
      mHwInterrupt2, InterruptNumber, CrInternalInterruptEntry);
  if (CR_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,
           CR_LOG_CHAR8_STR_FMT
           ": Failed to register interrupt handler for interrupt %u: %r\n",
           __FUNCTION__, InterruptNumber, Status));
    return Status;
  }

  // Configure interrupt trigger type
  Status = mHwInterrupt2->SetTriggerType(mHwInterrupt2, InterruptNumber,
                                         TriggerType);
  if (CR_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,
           CR_LOG_CHAR8_STR_FMT
           ": Failed to set trigger type for interrupt %u: %r\n",
           __FUNCTION__, InterruptNumber, Status));
    return Status;
  }

  // Enable the interrupt source
  Status = mHwInterrupt2->EnableInterruptSource(mHwInterrupt2, InterruptNumber);
  if (CR_ERROR(Status)) {
    DEBUG((DEBUG_ERROR,
           CR_LOG_CHAR8_STR_FMT ": Failed to enable interrupt %u: %r\n",
           __FUNCTION__, InterruptNumber, Status));
    return Status;
  }

  return Status;
}

CR_STATUS
CrUnregisterInterrupt(IN CR_INTERRUPT_CONFIG *InterruptConfig) {
  EFI_STATUS Status;
  UINT32 InterruptNumber = InterruptConfig->InterruptNumber;
  if (mHwInterrupt2 == NULL)
    if (CrInterruptInit() != CR_SUCCESS)
      return CR_NOT_FOUND;

  // Find the entry
  {
    UINTN i;
    for (i = 0; i < MAX_INTERRUPT_ENTRIES; i++) {
      if ((mCrIntTable[i].Source == InterruptNumber) && InterruptNumber &&
          (mCrIntTable[i].Handler != NULL)) {
        // Clear the entry
        mCrIntTable[i].Handler = NULL;
        mCrIntTable[i].Param = NULL;
        mCrIntTable[i].Source = 0;
        break;
      }
    }
    if (i == MAX_INTERRUPT_ENTRIES) {
      log_err(CR_LOG_CHAR8_STR_FMT
              ": Interrupt entry for interrupt %u not found",
              __FUNCTION__, InterruptNumber);
      return CR_NOT_FOUND;
    }
    // Move other entries forward to fill the gap
    // TODO: Interrupt should be disabled when operation here.
    for (; i < MAX_INTERRUPT_ENTRIES - 1; i++) {
      if (mCrIntTable[i + 1].Handler != NULL) {
        mCrIntTable[i] = mCrIntTable[i + 1];
        // Clear the moved entry
        mCrIntTable[i + 1].Handler = NULL;
        mCrIntTable[i + 1].Param = NULL;
        mCrIntTable[i + 1].Source = 0;
      } else {
        break;
      }
    }
  }

  // Disable the interrupt source
  Status =
      mHwInterrupt2->DisableInterruptSource(mHwInterrupt2, InterruptNumber);
  if (CR_ERROR(Status)) {
    DEBUG((DEBUG_WARN,
           CR_LOG_CHAR8_STR_FMT ": Failed to disable interrupt %u: %r\n",
           __FUNCTION__, InterruptNumber, Status));
  }

  return Status;
}
