/** @file
  Host regression coverage for ArmPciCpuIo2Dxe I/O address routing.

  SPDX-License-Identifier: MIT
**/

#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL

#include <Uefi.h>
#include <Library/BaseLib.h>

UINT64  mTestPciIoTranslation;

typedef struct {
  UINTN   Address;
  UINT64  Value;
  UINT8   Width;
  BOOLEAN Write;
} TEST_MMIO_ACCESS;

STATIC TEST_MMIO_ACCESS  mAccesses[16];
STATIC UINTN             mAccessCount;

STATIC
VOID
RecordAccess (
  IN UINTN    Address,
  IN UINT64   Value,
  IN UINT8    Width,
  IN BOOLEAN  Write
  )
{
  assert (mAccessCount < ARRAY_SIZE (mAccesses));
  mAccesses[mAccessCount].Address = Address;
  mAccesses[mAccessCount].Value   = Value;
  mAccesses[mAccessCount].Width   = Width;
  mAccesses[mAccessCount].Write   = Write;
  mAccessCount++;
}

UINT8
EFIAPI
MmioRead8 (
  IN UINTN  Address
  )
{
  RecordAccess (Address, 0x5a, sizeof (UINT8), FALSE);
  return 0x5a;
}

UINT16
EFIAPI
MmioRead16 (
  IN UINTN  Address
  )
{
  RecordAccess (Address, 0x5aa5, sizeof (UINT16), FALSE);
  return 0x5aa5;
}

UINT32
EFIAPI
MmioRead32 (
  IN UINTN  Address
  )
{
  RecordAccess (Address, 0x5aa55aa5, sizeof (UINT32), FALSE);
  return 0x5aa55aa5;
}

UINT64
EFIAPI
MmioRead64 (
  IN UINTN  Address
  )
{
  RecordAccess (Address, 0x5aa55aa55aa55aa5ULL, sizeof (UINT64), FALSE);
  return 0x5aa55aa55aa55aa5ULL;
}

UINT8
EFIAPI
MmioWrite8 (
  IN UINTN  Address,
  IN UINT8  Value
  )
{
  RecordAccess (Address, Value, sizeof (Value), TRUE);
  return Value;
}

UINT16
EFIAPI
MmioWrite16 (
  IN UINTN   Address,
  IN UINT16  Value
  )
{
  RecordAccess (Address, Value, sizeof (Value), TRUE);
  return Value;
}

UINT32
EFIAPI
MmioWrite32 (
  IN UINTN   Address,
  IN UINT32  Value
  )
{
  RecordAccess (Address, Value, sizeof (Value), TRUE);
  return Value;
}

UINT64
EFIAPI
MmioWrite64 (
  IN UINTN   Address,
  IN UINT64  Value
  )
{
  RecordAccess (Address, Value, sizeof (Value), TRUE);
  return Value;
}

UINT64
EFIAPI
LShiftU64 (
  IN UINT64  Operand,
  IN UINTN   Count
  )
{
  return Operand << Count;
}

UINT64
EFIAPI
RShiftU64 (
  IN UINT64  Operand,
  IN UINTN   Count
  )
{
  return Operand >> Count;
}

#include <ArmPkg/Drivers/ArmPciCpuIo2Dxe/ArmPciCpuIo2Dxe.c>

STATIC
VOID
ResetAccesses (
  VOID
  )
{
  memset (mAccesses, 0, sizeof (mAccesses));
  mAccessCount = 0;
}

STATIC
VOID
TestTranslatedHostReads (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Values[2];

  ResetAccesses ();
  mTestPciIoTranslation = 0;
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthUint32,
                        0x60200020,
                        ARRAY_SIZE (Values),
                        Values
                        );
  assert (Status == EFI_SUCCESS);
  assert (mAccessCount == 2);
  assert (mAccesses[0].Address == 0x60200020);
  assert (mAccesses[1].Address == 0x60200024);
  assert (!mAccesses[0].Write && (mAccesses[0].Width == sizeof (UINT32)));
  assert ((Values[0] == 0x5aa55aa5) && (Values[1] == 0x5aa55aa5));
}

STATIC
VOID
TestTranslatedHostWrites (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT16      Values[] = { 0x1234, 0xabcd };

  ResetAccesses ();
  mTestPciIoTranslation = 0;
  Status = mCpuIo2.Io.Write (
                         &mCpuIo2,
                         EfiCpuIoWidthUint16,
                         0x40200010,
                         ARRAY_SIZE (Values),
                         Values
                         );
  assert (Status == EFI_SUCCESS);
  assert (mAccessCount == 2);
  assert (mAccesses[0].Address == 0x40200010);
  assert (mAccesses[1].Address == 0x40200012);
  assert (mAccesses[0].Write && (mAccesses[0].Value == Values[0]));
  assert (mAccesses[1].Write && (mAccesses[1].Value == Values[1]));
}

STATIC
VOID
TestLegacyPcdTranslation (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  ResetAccesses ();
  mTestPciIoTranslation = 0x10000000;
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthUint8,
                        0x3f8,
                        1,
                        &Value
                        );
  assert (Status == EFI_SUCCESS);
  assert (mAccessCount == 1);
  assert (mAccesses[0].Address == 0x100003f8);
  assert (Value == 0x5a);
}

STATIC
VOID
TestInvalidRequests (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT64      Value;

  ResetAccesses ();
  mTestPciIoTranslation = 0;
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthUint64,
                        0x40200000,
                        1,
                        &Value
                        );
  assert (Status == EFI_INVALID_PARAMETER);
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthMaximum,
                        0x40200000,
                        1,
                        &Value
                        );
  assert (Status == EFI_INVALID_PARAMETER);
  assert (mAccessCount == 0);
}

STATIC
VOID
TestTranslationOverflow (
  VOID
  )
{
  EFI_STATUS  Status;
  UINT32      Values[2];

  ResetAccesses ();
  mTestPciIoTranslation = 0x100;
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthUint8,
                        MAX_UINT64 - 0x7f,
                        1,
                        Values
                        );
  assert (Status == EFI_UNSUPPORTED);

  mTestPciIoTranslation = 4;
  Status = mCpuIo2.Io.Read (
                        &mCpuIo2,
                        EfiCpuIoWidthUint32,
                        MAX_UINT64 - 7,
                        ARRAY_SIZE (Values),
                        Values
                        );
  assert (Status == EFI_UNSUPPORTED);
  assert (mAccessCount == 0);
}

int
main (
  void
  )
{
  TestTranslatedHostReads ();
  TestTranslatedHostWrites ();
  TestLegacyPcdTranslation ();
  TestInvalidRequests ();
  TestTranslationOverflow ();
  puts ("ArmPciCpuIo2: translated I/O routing, validation and overflow passed");
  return 0;
}
