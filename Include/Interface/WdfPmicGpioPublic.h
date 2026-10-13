/* Standalone PMIC GPIO discovery; typed owner query is shared. MIT */
#pragma once
#include <Interface/WdfGpioOwnerPublic.h>
DEFINE_GUID(GUID_DEVINTERFACE_PMIC_GPIO_CR,0x77394f59,0xf7a7,0x4ca7,0x9e,0x33,0x5b,0xa8,0x29,0xbb,0x18,0x0d);
/* GUID_GPIO_OWNER_INTERFACE revision1, Kind=CrGpioOwnerPmic, controller0.
 * PGCF schema1 HDK8450 qualifies PM8350 SID1/base8800/pads1..10. PON lines
 * have separate peripheral semantics and are never exposed as GPIO pads.
 * This policy provider has no GPIOClx interrupt transport. Only kernel typed
 * owner callbacks are published; raw SPMI writers must not target leased pads. */

/* The separate HDK button class may fence PM8350 pad6 using this interface.
 * Acquire/Release are PASSIVE. Read accepts <=DISPATCH and returns the physical
 * input level (the button consumer applies its ACPI polarity). The successful
 * query reference plus PowerRelations pins this provider through all calls.
 * Acquire installs the owner before fallible snapshot/configuration; a failed
 * acquire may retain that owner and MUST be paired with Release. Release must
 * follow successful IRQ mask/unregister/drain by the consumer. Failed release
 * retains the original snapshot and exact owner for retry. PON lines have no
 * identity in this interface. Only pad6 is qualified for this class boundary. */
#define WDF_PMIC_GPIO_INPUT_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_PMIC_GPIO_INPUT_INTERFACE,0xf8216b4c,0x9c75,0x4bf1,0x9e,0x98,0x11,0xad,0xb1,0x42,0x29,0x54);
typedef struct _WDF_PMIC_GPIO_INPUT_INTERFACE {
 INTERFACE Header;
 NTSTATUS (*Acquire)(PVOID Context,PVOID StableOwner,UINT32 Pin);
 NTSTATUS (*Read)(PVOID Context,PVOID StableOwner,UINT32 Pin,BOOLEAN *Level);
 NTSTATUS (*Release)(PVOID Context,PVOID StableOwner,UINT32 Pin);
} WDF_PMIC_GPIO_INPUT_INTERFACE;
