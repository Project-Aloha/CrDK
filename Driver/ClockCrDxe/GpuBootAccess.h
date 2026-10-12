/* Internal provider state, same ClockCrDxe mutation lock. MIT */
#pragma once
#include <Library/clock.h>
#include <Library/UefiLib.h>
#include <Protocol/EFICrAdrenoBootAccess.h>
#include <Protocol/EFIRpmhCrProtocol.h>
typedef struct {
 ClockDriverContext *Core;EFI_LOCK *Lock;EFI_CR_ADRENO_BOOT_ACCESS Protocol;
 ClockNode *Nodes[10],*Gpll;EFI_RPMH_CR_PROTOCOL *Rpmh;
 VOID *Owner;CR_ADRENO_BOOT_ACCESS_LEASE *Identity,Lease;
 BOOLEAN Pinned,Prepared,Committed,Uncertain;UINT32 AttemptedMask,OwnedMask;
} CR_GPU_BOOT_CLOCK;
EFI_STATUS CrGpuBootClockInit(CR_GPU_BOOT_CLOCK *,ClockDriverContext *,EFI_LOCK *);
EFI_STATUS CrGpuBootClockGuard(CONST CR_GPU_BOOT_CLOCK *,CONST ClockNode *);
EFI_STATUS CrGpuBootResetGuard(CONST CR_GPU_BOOT_CLOCK *,CONST CHAR8 *,CONST CHAR8 *);
BOOLEAN CrGpuBootOutputAllowed(CONST CR_GPU_BOOT_CLOCK *,CONST VOID *,UINTN);
