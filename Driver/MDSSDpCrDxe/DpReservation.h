/** HDK-selected fixed Reserved backing; no general allocator. MIT */
#pragma once
#include <PiDxe.h>
#define CR_DP_RESERVATION_NAME "HDK8450 DP Framebuffer"
#define CR_DP_RESERVATION_BYTES 0x00800000ULL
/* MU's GCD carries these resource state bits in Capabilities. All three keep
 * the static tested Reserved HOB out of Core's untested-memory promotion. */
#define CR_DP_RESERVED_STATE_CAPS 0x0700000000000000ULL
EFI_STATUS CrDpReservationInit(EFI_HANDLE ImageHandle);
EFI_STATUS CrDpReservationAcquire(EFI_PHYSICAL_ADDRESS *Base, UINT64 *Bytes);
EFI_STATUS CrDpReservationCheck(EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor);
BOOLEAN CrDpReservationAllocation(EFI_PHYSICAL_ADDRESS *Base, UINT64 *Bytes);
/* Caller has stopped hardware and retired DMA. Never frees pages, removes a
 * GCD range, or changes its Reserved type. Failed/uncertain effects stay live. */
EFI_STATUS CrDpReservationRelease(VOID);
