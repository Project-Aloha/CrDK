/* Portable GPIO ACPI configuration validation.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PepCrLib.h>
#include <Library/gpio.h>

/* The adapter owns and serializes the immutable input trees and inactive
 * output contexts. Inputs may be flattened method-output lists or one outer
 * package. All tables and register bounds are checked before publishing any
 * output. NULL ReservedRanges means the optional GRGL method is absent.
 * RCFG entry zero uses per-pin offsets. Special entries describe an absolute
 * control-bank offset and bank-relative IO/interrupt offsets, as in the ASL
 * generator; those offsets are normalized to the core's TLMM-relative form.
 * No input pointers are retained; mappings and OSKAL ownership are unchanged.
 */
CR_STATUS CrGpioConfigLoad(CONST CR_PEP_VALUE *Registers,
                          CONST CR_PEP_VALUE *Tiles,
                          CONST CR_PEP_VALUE *Pins,
                          CONST CR_PEP_VALUE *PdcRanges,
                          CONST CR_PEP_VALUE *ReservedRanges,
                          UINTN TlmmSize, GpioDeviceContext *Gpio,
                          PdcDeviceContext *Pdc);

/* GPIO class bank layout: 64 physical pins per bank; one PDC pin at bit zero
 * in each subsequent bank. Padding and other virtual-bank bits are invalid.
 * Inputs are bounded before multiplication, shifts, narrowing or map access.
 */
CR_STATUS CrGpioBankLayout(UINT16 PinCount, UINT16 PdcPinCount,
                          UINT32 *PhysicalBanks, UINT32 *TotalPins);
CR_STATUS CrGpioBankMask(UINT16 PinCount, UINT16 PdcPinCount, UINT32 Bank,
                        UINT64 *ValidMask);
CR_STATUS CrGpioResolveBankPin(UINT16 PinCount, CONST UINT16 *PdcPins,
                              UINT16 PdcPinCount, UINT32 Bank, UINT32 Pin,
                              UINT16 *PhysicalPin);

/* Resolve every selected bit before the adapter performs any MMIO. Entries
 * for unselected bits are 0xffff. On error, PhysicalPins is unchanged.
 */
CR_STATUS CrGpioResolveBankMask(UINT16 PinCount, CONST UINT16 *PdcPins,
                               UINT16 PdcPinCount, UINT32 Bank, UINT64 Mask,
                               UINT16 PhysicalPins[64]);

/* A virtual interrupt route can originate at a TLMM pad or directly at a
 * peripheral. 0xffff PhysicalPin denotes the latter; it is never a GPIO index.
 * Physical banks have 0xffff PdcPin. The legacy physical-pin resolvers above
 * remain available for IO-only callers. */
typedef struct {
  UINT16 PhysicalPin;
  UINT16 PdcPin;
} CR_GPIO_BANK_ROUTE;

/* DirectPins is the optional GDPI integer package of non-TLMM PDC pins. Every
 * listed pin must be present in GicIrqs, and cannot alias a TLMM PDC mapping.
 * GicIrqs are the raw interrupt resources for subsequent virtual banks, in
 * resource order. GPIO class interrupt locks are per bank: independent routes
 * must have distinct enable and shared-SPI RMW words. Shared-word layouts are
 * rejected until an adapter supplies global DIRQL-safe synchronization.
 * Validate complete inputs before MMIO/publication; outputs stay unchanged on
 * error. Contexts/input trees are immutable and serialized by the adapter. */
CR_STATUS CrGpioPdcRoutesLoad(CONST GpioDeviceContext *Gpio,
                            PdcDeviceContext *Pdc,
                            CONST CR_PEP_VALUE *DirectPins,
                            CONST UINT16 *GicIrqs, UINT16 Count,
                            CR_GPIO_BANK_ROUTE *Routes);
CR_STATUS CrGpioResolveBankRoute(UINT16 PinCount,
                               CONST CR_GPIO_BANK_ROUTE *PdcRoutes,
                               UINT16 PdcPinCount, UINT32 Bank, UINT32 Pin,
                               CR_GPIO_BANK_ROUTE *Route);
CR_STATUS CrGpioResolveBankRouteMask(UINT16 PinCount,
                                   CONST CR_GPIO_BANK_ROUTE *PdcRoutes,
                                   UINT16 PdcPinCount, UINT32 Bank, UINT64 Mask,
                                   CR_GPIO_BANK_ROUTE Routes[64]);
