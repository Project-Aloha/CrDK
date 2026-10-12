/* Immutable selected native panel and its real power/reset owner. GPL-2.0-only */
#pragma once
#include <Uefi.h>
#include <Library/MdssDsiPanelCrLib.h>
#define EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL_REVISION 0x10000ULL
#define EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL_GUID \
  {0x9b2d1f43,0x5b87,0x4f55,{0x95,0xd1,0x71,0x0b,0x7c,0x84,0x98,0x04}}
extern EFI_GUID gEfiMdssPanelProfileCrProtocolGuid;
typedef struct _EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL;
typedef struct {
  UINT32 Size, Revision;
  CONST CR_MDSS_PANEL_PROFILE *Profile;
  UINT64 MdpHz, AverageKBps[2], PeakKBps[2];
  CR_MDSS_PANEL_IO Panel;
} EFI_MDSS_PANEL_SELECTION;
struct _EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL {
  UINT64 Revision;
  /* PASSIVE/application or CALLBACK, externally serialized same stable owner.
     Acquire owns immutable arrays/payload/reset/provider callback storage until
     exact Release. Owned commits before any fallible operation; failed Acquire
     with Owned must be released before unloading its consumer. The actual
     Panel callbacks own only this panel's assigned power/reset/bridge resources;
     they do not grant MDSS clocks, MMIO or DMA. No private XML profile ID may
     supply guessed DSC parameters or a board GPIO binding. */
  EFI_STATUS (EFIAPI *Acquire)(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *,VOID *StableOwner,
    EFI_MDSS_PANEL_SELECTION *Selection,BOOLEAN *Owned);
  EFI_STATUS (EFIAPI *Ready)(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *,VOID *StableOwner);
  EFI_STATUS (EFIAPI *Release)(EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *,VOID *StableOwner,BOOLEAN *Owned);
};
