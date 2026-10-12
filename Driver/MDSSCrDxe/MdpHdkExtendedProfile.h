/* Explicit opt-in real HDK LT9611 profile producer and deferred caller. MIT */
#pragma once
#include <Uefi.h>
EFI_STATUS MdpHdkExtendedProfileStart(VOID);
EFI_STATUS MdpHdkExtendedProfileUnload(VOID);
BOOLEAN MdpHdkExtendedProfileRetained(VOID);
