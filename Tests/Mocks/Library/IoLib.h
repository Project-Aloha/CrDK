/** @file
  Host-test I/O library declarations.

  SPDX-License-Identifier: MIT
**/

#ifndef CRANE_TEST_IO_LIB_H_
#define CRANE_TEST_IO_LIB_H_

#include <Uefi.h>

UINT8
EFIAPI
MmioRead8 (
  IN UINTN  Address
  );

UINT16
EFIAPI
MmioRead16 (
  IN UINTN  Address
  );

UINT32
EFIAPI
MmioRead32 (
  IN UINTN  Address
  );

UINT64
EFIAPI
MmioRead64 (
  IN UINTN  Address
  );

UINT8
EFIAPI
MmioWrite8 (
  IN UINTN  Address,
  IN UINT8  Value
  );

UINT16
EFIAPI
MmioWrite16 (
  IN UINTN   Address,
  IN UINT16  Value
  );

UINT32
EFIAPI
MmioWrite32 (
  IN UINTN   Address,
  IN UINT32  Value
  );

UINT64
EFIAPI
MmioWrite64 (
  IN UINTN   Address,
  IN UINT64  Value
  );

#endif
