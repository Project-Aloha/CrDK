/* Referenced fixed-role receive route. SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
#include <Library/IpccReceiveCrLib.h>
#include <guiddef.h>
DEFINE_GUID(GUID_IPCC_RECEIVE_CR_INTERFACE,0x57ed681b,0x205a,0x45c2,0xbb,0x3d,0x2a,0x6f,0x1b,0x85,0x10,0x93);
DEFINE_GUID(GUID_DEVINTERFACE_IPCC_RECEIVE_CR,0x7009fd06,0x80e9,0x47ba,0x93,0xec,0xb2,0x7a,0x9b,0x5e,0x80,0x12);
#define WDF_IPCC_RECEIVE_CR_REVISION 1U
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID,UINT32,CR_IPCC_RX_WAKE,PVOID,CR_IPCC_RX_LEASE *);
  NTSTATUS (*Observe)(PVOID,PVOID,CONST CR_IPCC_RX_LEASE *,CR_IPCC_RX_OBSERVATION *);
  NTSTATUS (*Complete)(PVOID,PVOID,CONST CR_IPCC_RX_LEASE *,UINT64);
  NTSTATUS (*Close)(PVOID,PVOID,CONST CR_IPCC_RX_LEASE *);
  NTSTATUS (*End)(PVOID,PVOID,CR_IPCC_RX_LEASE *);
  NTSTATUS (*ReferenceDependency)(PVOID,PVOID,CONST CR_IPCC_RX_LEASE *,PDEVICE_OBJECT *);
} WDF_IPCC_RECEIVE_CR_INTERFACE;
/* Query/header pins the permanent source; Begin reports ownership in the exact
 * lease even on a later failure. All methods run at PASSIVE_LEVEL. Wake runs
 * under the real parent interrupt guard at DIRQL and may only queue a DPC.
 * Observe returns current state even after closure. Complete accepts only the
 * exact latest sequence, after the consumer has handled its bounded work.
 * Close synchronously fences direct Wake callbacks; it neither drains consumer
 * DPC/work nor retires remote data. Consumer closes, drains its own queued uses,
 * then End releases the route, then header/dependency refs may be dropped.
 * There is no arbitrary signal, raw MMIO or SEND operation in this interface. */
