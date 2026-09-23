/** @file
 *  EFI protocol exposing the platform interconnect path voter.
 *
 *  The protocol deliberately carries paths rather than raw BCM/node writes.
 *  ICBCrDxe adapts those paths to the platform's native ICB owner; consumers
 *  only describe the source, destination, and bandwidth they need.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/interconnect.h>

#define EFI_INTERCONNECT_CR_PROTOCOL_REVISION 0x0000000000010000ULL

#define EFI_INTERCONNECT_CR_PROTOCOL_GUID                                      \
  {0x7e5f9b16, 0x4c9a, 0x4e3a, {0x91, 0x62, 0x5a, 0xc8, 0x3f, 0x08, 0x7d, 0x22}}

extern EFI_GUID gEfiInterconnectCrProtocolGuid;

typedef struct _EFI_INTERCONNECT_CR_PROTOCOL EFI_INTERCONNECT_CR_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_INTERCONNECT_CR_ACQUIRE_PATH)(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN CONST CHAR8                  *ProviderCompatible,
    IN UINT32                        SourceId,
    IN UINT32                        DestinationId,
    OUT INTERCONNECT_PATH_HANDLE    *Path);

/** Bandwidth values use Linux interconnect kB/s (exactly 1000 bytes/second). */
typedef EFI_STATUS(EFIAPI *EFI_INTERCONNECT_CR_SET_BANDWIDTH)(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN INTERCONNECT_PATH_HANDLE     Path,
    IN UINT64                        AverageBandwidth,
    IN UINT64                        PeakBandwidth);

typedef EFI_STATUS(EFIAPI *EFI_INTERCONNECT_CR_RELEASE_PATH)(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN INTERCONNECT_PATH_HANDLE     Path);

typedef struct _EFI_INTERCONNECT_CR_PROTOCOL {
  UINT64                                  Revision;
  EFI_INTERCONNECT_CR_ACQUIRE_PATH        AcquirePath;
  EFI_INTERCONNECT_CR_SET_BANDWIDTH       SetBandwidth;
  EFI_INTERCONNECT_CR_RELEASE_PATH        ReleasePath;
} EFI_INTERCONNECT_CR_PROTOCOL;
