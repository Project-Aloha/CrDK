/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#define EFI_CLOCK_CR_PROTOCOL_REVISION 0x0000000000010003

#define EFI_CLOCK_CR_PROTOCOL_GUID                                             \
  {0xa355879b, 0x66c2, 0x4901, {0xb9, 0xa5, 0xc3, 0x0f, 0x31, 0x91, 0x81, 0xc8}}

extern EFI_GUID                       gEfiClockCrProtocolGuid;
typedef struct _EFI_CLOCK_CR_PROTOCOL EFI_CLOCK_CR_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_SET_CLOCK)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT64 RateHz, IN BOOLEAN Enable);

typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_SET_GDSC)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN BOOLEAN Enable);

typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_SET_RESET)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN BOOLEAN Assert);

typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_CONFIGURE_EXTERNAL_SOURCE)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT32 RateHz, IN UINT32 Source,
    IN UINT32 Divider, IN UINT32 M, IN UINT32 N, IN UINT32 TwiceD);

typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_SET_DIVIDER)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT32 Divider);

/* Select an ordinary mux parent, including while its sole owned branch is
   enabled. Source is the target mux encoding. Shared or uncertain votes and
   RCG/PHY-gate nodes are rejected; no rate or gate state is changed. */
typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_SET_PARENT)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT32 Source);

/* Owned reports only the reference obtained by this call. A failed call
   can retain cleanup; the caller must release it with SetClock/SetGdsc FALSE
   before retrying acquisition or dropping its prerequisite resources. */
typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_ACQUIRE_CLOCK)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, IN UINT64 RateHz, OUT BOOLEAN *Owned);
typedef EFI_STATUS(EFIAPI *EFI_CLOCK_CR_ACQUIRE_GDSC)(
    IN EFI_CLOCK_CR_PROTOCOL *This, IN CONST CHAR8 *Controller,
    IN CONST CHAR8 *Id, OUT BOOLEAN *Owned);

typedef struct _EFI_CLOCK_CR_PROTOCOL {
  UINT64                  Revision;
  EFI_CLOCK_CR_SET_CLOCK  SetClock;
  EFI_CLOCK_CR_SET_GDSC   SetGdsc;
  EFI_CLOCK_CR_SET_RESET  SetReset;
  EFI_CLOCK_CR_CONFIGURE_EXTERNAL_SOURCE ConfigureExternalSource;
  EFI_CLOCK_CR_SET_DIVIDER SetDivider;
  EFI_CLOCK_CR_SET_PARENT SetParent;
  EFI_CLOCK_CR_ACQUIRE_CLOCK AcquireClock;
  EFI_CLOCK_CR_ACQUIRE_GDSC AcquireGdsc;
} EFI_CLOCK_CR_PROTOCOL;

STATIC inline EFI_STATUS CrAcquireClock (EFI_CLOCK_CR_PROTOCOL *Clock,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, UINT64 RateHz, BOOLEAN *Owned)
{
  if (Owned == NULL) return EFI_INVALID_PARAMETER;
  *Owned = FALSE;
  if (Clock == NULL || Clock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
      Clock->AcquireClock == NULL || Clock->SetClock == NULL) return EFI_UNSUPPORTED;
  return Clock->AcquireClock (Clock, Controller, Id, RateHz, Owned);
}

STATIC inline EFI_STATUS CrAcquireGdsc (EFI_CLOCK_CR_PROTOCOL *Clock,
  CONST CHAR8 *Controller, CONST CHAR8 *Id, BOOLEAN *Owned)
{
  if (Owned == NULL) return EFI_INVALID_PARAMETER;
  *Owned = FALSE;
  if (Clock == NULL || Clock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
      Clock->AcquireGdsc == NULL || Clock->SetGdsc == NULL) return EFI_UNSUPPORTED;
  return Clock->AcquireGdsc (Clock, Controller, Id, Owned);
}
