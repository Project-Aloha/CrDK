/* Native display producers rendezvous before DMA handoff. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#include <Library/MdssHandoffCrLib.h>

#define EFI_MDSS_HANDOFF_PROTOCOL_GUID \
  {0x933c56f4,0x6f8d,0x42aa,{0xa2,0x28,0x21,0xee,0x6b,0x37,0x8b,0x42}}
#define EFI_MDSS_HANDOFF_PROTOCOL_REVISION 0x10000ULL
extern EFI_GUID gEfiMdssHandoffProtocolGuid;
typedef struct _EFI_MDSS_HANDOFF_PROTOCOL EFI_MDSS_HANDOFF_PROTOCOL;

/* Close every entry point to future work, leave ownership journals intact,
   and stop transient DMA. No allocation/free or event/protocol lifetime
   changes here. Idempotent: both EBS and the SMMU pre-revoke callback may
   reach this path. EFI_SUCCESS means software can no longer change the
   snapshot; an unsafe hardware stop is represented as UNCERTAIN in it. */
typedef EFI_STATUS (EFIAPI *EFI_MDSS_HANDOFF_FREEZE)(VOID *Context);
/* Called after Freeze and after SMMU's entire EBS pass. The opaque Map is a
   native-only token; only SMMU fills the portable mapping evidence. */
typedef EFI_STATUS (EFIAPI *EFI_MDSS_HANDOFF_SNAPSHOT)(VOID *Context,
  CR_MDSS_HANDOFF_PARTICIPANT *Snapshot, VOID **Map);
typedef EFI_STATUS (EFIAPI *EFI_MDSS_HANDOFF_SET_PARTICIPANT)(
  EFI_MDSS_HANDOFF_PROTOCOL *This, UINT32 Owner, VOID *Context,
  EFI_MDSS_HANDOFF_FREEZE Freeze OPTIONAL,
  EFI_MDSS_HANDOFF_SNAPSHOT Snapshot OPTIONAL);
struct _EFI_MDSS_HANDOFF_PROTOCOL {
  UINT64 Revision;
  /* Single owner bit DSI/DP/BRIDGE/PHY. SMMU is coordinator-owned.
     Both callbacks NULL unregister the same context before image unload;
     registration is immutable once EBS starts. */
  EFI_MDSS_HANDOFF_SET_PARTICIPANT SetParticipant;
};
