/* Exact LT9611 board panel power/reset and FIFO preparation owner. MIT */
#pragma once
#include <Protocol/EFIMdpDsiProtocol.h>
#define EFI_LT9611_PANEL_CR_PROTOCOL_REVISION 0x10000ULL
#define EFI_LT9611_PANEL_CR_PROTOCOL_GUID \
  {0x4f37c0bc,0x7d24,0x4c03,{0x9a,0xca,0xc2,0x7d,0x06,0x4f,0xe0,0x6a}}
extern EFI_GUID gEfiLt9611PanelCrProtocolGuid;
typedef struct _EFI_LT9611_PANEL_CR_PROTOCOL EFI_LT9611_PANEL_CR_PROTOCOL;
struct _EFI_LT9611_PANEL_CR_PROTOCOL {
  UINT64 Revision;
  /* Ready checks actual resident bridge/admission and exclusive owner identity;
     it permits an unowned power-off bridge for a later Acquire. All calls are
     serialized with that bridge's actual MCU worker and EBS gate. Acquire and
     Release commit/retain Owned until all actual pin writes/readbacks complete.
     GPIO9/109/107 identities come from the verified HDK target, not XML. */
  EFI_STATUS (EFIAPI *Ready)(EFI_LT9611_PANEL_CR_PROTOCOL *,VOID *StableOwner);
  EFI_STATUS (EFIAPI *Acquire)(EFI_LT9611_PANEL_CR_PROTOCOL *,VOID *StableOwner,BOOLEAN *Owned);
  EFI_STATUS (EFIAPI *Release)(EFI_LT9611_PANEL_CR_PROTOCOL *,VOID *StableOwner,BOOLEAN *Owned);
  EFI_STATUS (EFIAPI *Reset)(EFI_LT9611_PANEL_CR_PROTOCOL *,VOID *StableOwner,BOOLEAN High);
  /* After the explicit reset sequence, qualify FIFO/chip/firmware and program
     the real timing before DPU scanout. Does not start or own DSI DMA. */
  EFI_STATUS (EFIAPI *Prepare)(EFI_LT9611_PANEL_CR_PROTOCOL *,VOID *StableOwner,
    CONST EFI_MDP_DSI_TIMING *Timing);
};
