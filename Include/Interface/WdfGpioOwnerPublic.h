/* Qualified absolute GPIO pin owners. MIT */
#pragma once
#include <wdm.h>
#include <Library/GpioOwnerLib.h>
#define WDF_GPIO_OWNER_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_GPIO_OWNER_INTERFACE,0x7ddcd681,0xe0d7,0x4ac8,0xa7,0xad,0xd4,0x56,0x38,0xb4,0x7d,0x90);
DEFINE_GUID(GUID_DEVINTERFACE_GPIO_OWNER_CR,0x84c3c360,0x4700,0x478c,0x88,0x53,0x70,0xc7,0x28,0x04,0x31,0x6a);
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Validate)(PVOID,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_CONFIG *);
  NTSTATUS (*Reserve)(PVOID,PVOID Owner,CONST CR_GPIO_OWNER_KEY *);
  NTSTATUS (*Get)(PVOID,PVOID Owner,CONST CR_GPIO_OWNER_KEY *,CR_GPIO_OWNER_VALUE *,BOOLEAN *Pending);
  NTSTATUS (*Set)(PVOID,PVOID Owner,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_VALUE *);
  NTSTATUS (*ReleaseOwner)(PVOID,PVOID Owner);
} WDF_GPIO_OWNER_INTERFACE;
/* PASSIVE externally serialized; stable nonpaged owner + interface reference
 * and provider D0 lease through every failed restore/release. Reserve is fixed
 * metadata only and fences GPIO-class/foreign ownership of the physical pad.
 * Every full AML tuple must map explicitly to typed values via board profile;
 * this API never silently ignores extra fields or assumes pin aliases/units.
 * Failed writes retain old logical aggregate and physical claim until replay.
 * Provider removal or unavailable D0 is failure, not restored GPIO state. */
