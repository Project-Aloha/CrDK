/* Native normal-pad exclusive leases. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/gpio.h>
#define CR_GPIO_PIN_LEASE_CAPACITY 8U
typedef struct {
  VOID *Owner;
  GpioConfigParams Baseline,Requested;
  BOOLEAN Touched;
  UINT32 OriginalIrq,PolledIrq;
  BOOLEAN IrqTouched,EdgeReady;
} CR_GPIO_PIN_LEASE;
typedef struct {
  GpioDeviceContext *Context;
  CR_GPIO_PIN_LEASE Entries[CR_GPIO_PIN_LEASE_CAPACITY];
} CR_GPIO_PIN_LEASE_BANK;
/* Caller serializes all operations and fences provider legacy mutation/IRQ
 * registration plus retained failed IRQ claims. SD pins are provider-fenced.
 * Only ordinary AP-owned pads are supported. Failed configuration/restore
 * retains the exact owner and immutable snapshot; retry release before unload.
 * Only DriveStrength/OutputValue may be UNCHANGE on acquisition. They resolve
 * once against the locked, qualified original snapshot, including the latent
 * output latch for an input. The stored request is fully explicit. A retry
 * resolves those fields against the same immutable baseline; foreign owners
 * fail before snapshot/MMIO. Pull/function/direction still require explicit
 * valid values. Init is idempotent only for the identical context; it never
 * clears owners. */
CR_STATUS CrGpioPinLeaseInit(CR_GPIO_PIN_LEASE_BANK *,GpioDeviceContext *);
BOOLEAN CrGpioPinLeaseOwned(CONST CR_GPIO_PIN_LEASE_BANK *,UINT16);
CR_STATUS CrGpioPinLeaseAcquire(CR_GPIO_PIN_LEASE_BANK *,VOID *,CONST GpioConfigParams *,BOOLEAN *);
CR_STATUS CrGpioPinLeaseSetValue(CR_GPIO_PIN_LEASE_BANK *,VOID *,UINT16,UINT8);
CR_STATUS CrGpioPinLeaseReadValue(CR_GPIO_PIN_LEASE_BANK *,VOID *,UINT16,UINT8 *);
CR_STATUS CrGpioPinLeaseRelease(CR_GPIO_PIN_LEASE_BANK *,VOID *,UINT16);
/* Owner-scoped raw falling-edge latch, with CPU summary IRQ masked. Arm
 * once after device setup; Take acknowledges before the deferred transfer
 * so a later edge is retained. Release restores the captured IRQ config.
 * No callback or interrupt-controller registration is installed. */
CR_STATUS CrGpioPinLeaseArmFallingEdge(CR_GPIO_PIN_LEASE_BANK *,VOID *,UINT16);
CR_STATUS CrGpioPinLeaseTakeEdge(CR_GPIO_PIN_LEASE_BANK *,VOID *,UINT16,BOOLEAN *);
