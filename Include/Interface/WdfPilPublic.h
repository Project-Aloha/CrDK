/* Referenced ADSP owner/readiness interface. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#define WDF_PIL_CR_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_DEVINTERFACE_PIL_CR,0x533eb14b,0x0bd5,0x45b9,0x9a,0x73,0x9f,0xcd,0x7b,0x17,0x20,0x30);
DEFINE_GUID(GUID_PIL_CR_INTERFACE,0x9b0c5818,0xed17,0x4c2d,0xaa,0x58,0x85,0x65,0xe1,0x17,0xf3,0x40);
typedef struct {
  ULONG Generation;
  BOOLEAN Ready, Owned, RecoveryPending;
  NTSTATUS LastError;
} WDF_PIL_CR_STATE;
/* PASSIVE only. Keep the query reference and stable nonpaged Owner until a
   successful Release. Acquire is idempotent for that Owner and pins the ADSP
   instance through consumer SMEM/GLink use. Release after IRQ/work/request and
   SMEM leases drain; failed Release retains ownership. Provider closes new
   requests before stop, refuses stop while consumer leases remain, and retains
   prerequisite mappings/query references when PAS shutdown fails. GetState
   verifies current SMP2P readiness/fatal state; a changed Generation invalidates
   consumer work from the older boot. Query may be published while not ready. */
typedef NTSTATUS (*PIL_CR_ACQUIRE)(PVOID,PVOID Owner);
typedef NTSTATUS (*PIL_CR_RELEASE)(PVOID,PVOID Owner);
typedef NTSTATUS (*PIL_CR_GET_STATE)(PVOID,WDF_PIL_CR_STATE *);
typedef struct {
  INTERFACE Header;
  PIL_CR_ACQUIRE Acquire;
  PIL_CR_RELEASE Release;
  PIL_CR_GET_STATE GetState;
} WDF_PIL_CR_INTERFACE;
