/* Permanent platform broker discovery. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
DEFINE_GUID(GUID_DEVINTERFACE_BUS_CR,0xd6b3da12,0x8c34,0x4c57,0x98,0xad,0x12,0x4a,0x74,0x5c,0x8e,0x31);
/* All ACPI sibling consumers discover exactly one instance and retain its
 * referenced query ABI and target through failed cleanup. An _DEP package
 * establishes PnP ordering; it does not route a query to a sibling. */
