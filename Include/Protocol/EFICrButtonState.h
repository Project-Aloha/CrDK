/* Live physical PMIC buttons, separate from repeated navigation keystrokes.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#define EFI_CR_BUTTON_STATE_PROTOCOL_REVISION 2U
#define EFI_CR_BUTTON_STATE_PROTOCOL_GUID \
 {0xd74e4f66,0x317e,0x4401,{0xb7,0xe3,0x89,0x43,0x37,0x1a,0x92,0x9d}}
extern EFI_GUID gEfiCrButtonStateProtocolGuid;
#define CR_BUTTON_STATE_POWER BIT0
#define CR_BUTTON_STATE_VOLUME_UP BIT1
#define CR_BUTTON_STATE_VOLUME_DOWN BIT2
#define EFI_CR_BUTTON_SNAPSHOT_REVISION 1U
typedef struct {
 UINT32 Size, Revision, Pressed, Reserved;
 UINT64 PowerPressSequence;
} EFI_CR_BUTTON_SNAPSHOT;
typedef struct _EFI_CR_BUTTON_STATE_PROTOCOL EFI_CR_BUTTON_STATE_PROTOCOL;
struct _EFI_CR_BUTTON_STATE_PROTOCOL {
 UINT32 Revision;
 /* <=TPL_CALLBACK. Direct owned hardware sample, no IRQ/queue mutation.
    Success commits all three buttons; failure leaves Pressed unchanged.
    No state is valid after the owner's EBS/Stop gate closes. */
 EFI_STATUS (EFIAPI *GetState)(EFI_CR_BUTTON_STATE_PROTOCOL *This,UINT32 *Pressed);
 /* Revision 2: same current physical sample plus monotonic observed power
    presses, independent of debounce/key-repeat/dequeue. A released short tap
    may wake a consumer; this sequence never certifies a current held level.
    Snapshot commits only after all current levels were read successfully. */
 EFI_STATUS (EFIAPI *GetSnapshot)(EFI_CR_BUTTON_STATE_PROTOCOL *This,EFI_CR_BUTTON_SNAPSHOT *Snapshot);
};
