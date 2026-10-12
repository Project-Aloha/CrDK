/* Private native-only boot clock/access guard. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#define EFI_CR_ADRENO_BOOT_ACCESS_GUID {0xe2cc80cd,0xb2ca,0x5aa1,{0x90,0x5a,0x80,0x53,0xc2,0x5f,0x47,0x67}}
#define EFI_CR_ADRENO_BOOT_ACCESS_REVISION 1ULL
typedef struct {VOID *Provider;UINT64 Incarnation;UINT32 OwnedMask,Reserved;} CR_ADRENO_BOOT_ACCESS_LEASE;
typedef struct _EFI_CR_ADRENO_BOOT_ACCESS EFI_CR_ADRENO_BOOT_ACCESS;
struct _EFI_CR_ADRENO_BOOT_ACCESS {
 UINT64 Revision;
 EFI_STATUS(EFIAPI *Begin)(EFI_CR_ADRENO_BOOT_ACCESS *,VOID *Owner,CR_ADRENO_BOOT_ACCESS_LEASE *,BOOLEAN *Owned);
 EFI_STATUS(EFIAPI *Check)(EFI_CR_ADRENO_BOOT_ACCESS *,VOID *Owner,CONST CR_ADRENO_BOOT_ACCESS_LEASE *);
 /* Commit irreversibly closes the native consumer after completed retirement;
  * retains all clock/dependency votes and writer guards through OS entry.
  * No Release/rollback after any Begin effect, no caller-selected reset/rate. */
 EFI_STATUS(EFIAPI *Commit)(EFI_CR_ADRENO_BOOT_ACCESS *,VOID *Owner,CONST CR_ADRENO_BOOT_ACCESS_LEASE *);
};
