/* Shared MPROC SEND register owner, kernel interface. SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#define WDF_IPCC_SEND_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_IPCC_SEND_CR_INTERFACE,0x31bdb5d9,0xa93f,0x455c,0x81,0xb7,0xaf,0x59,0x34,0x35,0xbc,0x51);
typedef struct {PVOID Provider;UINT64 Generation;} CR_IPCC_SEND_LEASE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID StableOwner,CR_IPCC_SEND_LEASE *);
  NTSTATUS (*End)(PVOID,PVOID StableOwner,CR_IPCC_SEND_LEASE *);
  NTSTATUS (*Send)(PVOID,PVOID StableOwner,CONST CR_IPCC_SEND_LEASE *,UINT32 ClientSignal);
} WDF_IPCC_SEND_INTERFACE;
/* Referenced permanent PEP provider. Begin/End PASSIVE, lease storage unmoved
 * and zeroed before Begin; exact owner/lease pins provider D0 and SEND mapping.
 * Send <=DISPATCH, bounded to AOSS0:0 and LPASS3:0, serialized under one spin
 * lock for every producer. It orders prior shared-memory stores before the
 * doorbell. Drain consumer IRQ/work/SMEM writers before End. Failed End retains
 * Provider/Generation; publication, removal or a query ref alone cannot retire
 * the real mapping. Consumer never maps/writes SEND itself. */
