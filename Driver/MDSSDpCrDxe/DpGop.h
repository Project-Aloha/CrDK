/** Stable native DP GOP, independent of the DSI framebuffer. MIT */
#pragma once
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#define CR_DP_GOP_WIDTH 1920U
#define CR_DP_GOP_HEIGHT 1080U
#define CR_DP_GOP_STRIDE (CR_DP_GOP_WIDTH * 4U)
EFI_STATUS CrDpGopPrepare(VOID);
EFI_STATUS CrDpGopGetFramebuffer(EFI_PHYSICAL_ADDRESS *,UINT32 *,UINT32 *,UINT32 *);
EFI_STATUS CrDpGopPublish(BOOLEAN *NewPublication);
EFI_STATUS CrDpGopConnect(VOID);
BOOLEAN CrDpGopPublished(VOID);
VOID CrDpGopExit(VOID);
BOOLEAN CrDpGopFrozen(VOID);
/* Data-only retained allocation observation; no DMA grant follows. */
BOOLEAN CrDpGopAllocation(EFI_PHYSICAL_ADDRESS *,UINT64 *Bytes,BOOLEAN *Prepared);
/* Published GOP storage remains valid through unplug/retrain and EBS. Once
 * published, unload refuses: console/raw-framebuffer clients may retain mode
 * and buffer pointers without an OpenProtocol record. No hotplug teardown. */
EFI_STATUS CrDpGopRelease(VOID);
