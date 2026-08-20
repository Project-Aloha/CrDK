/** @file
 *  Kernel query-interface ABI for the Crane RPMh interconnect driver.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <wdm.h>
#include <Library/interconnect.h>

#define WDF_INTERCONNECT_CR_INTERFACE_REVISION 0x1U

// cc20052d-be97-4588-920d-bc7934dc945b
DEFINE_GUID(GUID_DEVINTERFACE_INTERCONNECT_CR, 0xcc20052d, 0xbe97, 0x4588,
            0x92, 0x0d, 0xbc, 0x79, 0x34, 0xdc, 0x94, 0x5b);

// ecc0295b-1b8e-4ccb-86ad-92ee2a9a27f4
DEFINE_GUID(GUID_INTERCONNECT_CR_INTERFACE, 0xecc0295b, 0x1b8e, 0x4ccb,
            0x86, 0xad, 0x92, 0xee, 0x2a, 0x9a, 0x27, 0xf4);

typedef NTSTATUS (*INTERCONNECT_CR_ACQUIRE_PATH)(
    IN VOID *Context, IN CONST CHAR8 *ProviderCompatible, IN UINT32 SourceId,
    IN UINT32 DestinationId, OUT INTERCONNECT_PATH_HANDLE *Path);

typedef NTSTATUS (*INTERCONNECT_CR_SET_BANDWIDTH)(
    IN VOID *Context, IN INTERCONNECT_PATH_HANDLE Path,
    IN UINT64 AverageBandwidth, IN UINT64 PeakBandwidth);

typedef NTSTATUS (*INTERCONNECT_CR_RELEASE_PATH)(
    IN VOID *Context, IN INTERCONNECT_PATH_HANDLE Path);

typedef NTSTATUS (*INTERCONNECT_CR_GET_PROVIDER_COUNT)(
    IN VOID *Context, OUT UINT16 *ProviderCount);

typedef struct _WDF_INTERCONNECT_CR_INTERFACE {
  INTERFACE Header;
  INTERCONNECT_CR_ACQUIRE_PATH AcquirePath;
  INTERCONNECT_CR_SET_BANDWIDTH SetBandwidth;
  INTERCONNECT_CR_RELEASE_PATH ReleasePath;
  INTERCONNECT_CR_GET_PROVIDER_COUNT GetProviderCount;
} WDF_INTERCONNECT_CR_INTERFACE;
