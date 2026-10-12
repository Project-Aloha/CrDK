/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "ScsiDisk.h"
#include "ScsiWriteProtection.inc"

STATIC UINTN mCalls;
STATIC UINTN mTenCallsExpected, mUnitAttentionStart, mUnitAttentionCount;
STATIC UINT8 mUnitAttentionFormat;
STATIC UINT8 mRejectSense;
STATIC EFI_STATUS mStatus;
STATIC UINT8 mHost, mTarget;
STATIC UINT32 mLength;
STATIC BOOLEAN mProtected, mBadHeader, mTruncatedSense, mInvalidField;
STATIC UINTN mLiveBuffers, mAllocationCalls, mFailAllocation;

VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }
UINT64 EFIAPI MultU64x32 (UINT64 Value, UINT32 Multiplier) { return Value * Multiplier; }
VOID *EFIAPI AllocateAlignedPages (UINTN Pages, UINTN Alignment)
{
  VOID *Buffer = NULL;
  assert (Alignment == 4096 && Pages == 1);
  if (++mAllocationCalls == mFailAllocation) return NULL;
  assert (posix_memalign (&Buffer, Alignment, Pages * 4096) == 0);
  mLiveBuffers++;
  return Buffer;
}
VOID EFIAPI FreeAlignedPages (VOID *Buffer, UINTN Pages)
{
  assert (Buffer != NULL && Pages == 1 && mLiveBuffers != 0);
  mLiveBuffers--;
  free (Buffer);
}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }

STATIC EFI_STATUS EFIAPI Execute (EFI_SCSI_IO_PROTOCOL *This,
                                 EFI_SCSI_IO_SCSI_REQUEST_PACKET *Packet, EFI_EVENT Event)
{
  UINT8 *Cdb = Packet->Cdb, *Data = Packet->InDataBuffer, *Sense = Packet->SenseData;
  BOOLEAN Ten = mCalls < mTenCallsExpected;
  assert (((UINTN)Data & (This->IoAlign - 1)) == 0);
  assert (((UINTN)Sense & (This->IoAlign - 1)) == 0);
  assert (Event == NULL);
  assert (Packet->CdbLength == (Ten ? 10 : 6));
  assert (Cdb[0] == (Ten ? 0x5A : 0x1A));
  assert (Cdb[1] == 8 && Cdb[2] == 0x3f);
  assert (Cdb[Ten ? 8 : 4] == 64);
  assert (Packet->DataDirection == EFI_SCSI_IO_DATA_DIRECTION_READ);
  assert (Packet->Timeout == SCSI_DISK_TIMEOUT && Packet->InTransferLength == 64);
  assert (Packet->SenseDataLength >= 18 && Sense != NULL);
  for (UINTN Index = 0; Index < 64; Index++) assert (Data[Index] == 0);
  mCalls++;
  Packet->HostAdapterStatus = mHost;
  Packet->TargetStatus = mTarget;
  if (mCalls > mUnitAttentionStart && mCalls <= mUnitAttentionStart + mUnitAttentionCount) {
    Packet->TargetStatus = EFI_SCSI_IO_STATUS_TARGET_CHECK_CONDITION;
    if (mUnitAttentionFormat == 1) {
      Sense[0] = 0x70; Sense[2] = 6; Sense[7] = mInvalidField ? 0 : 10;
      Sense[12] = 0x29; Sense[13] = 2;
      Packet->SenseDataLength = mTruncatedSense ? 13 : 18;
    } else {
      Sense[0] = 0x72; Sense[1] = 6; Sense[2] = 0x29; Sense[3] = 2;
      Packet->SenseDataLength = mTruncatedSense ? 3 : 8;
    }
    return mStatus == EFI_SUCCESS ? EFI_DEVICE_ERROR : mStatus;
  }
  if (Ten && mRejectSense != 0) {
    Packet->TargetStatus = EFI_SCSI_IO_STATUS_TARGET_CHECK_CONDITION;
    if (mRejectSense == 1) {
      Sense[0] = 0x70;
      Sense[2] = 5;
      Sense[7] = 10;
      Sense[12] = mInvalidField ? 0x24 : 0x20;
      Packet->SenseDataLength = mTruncatedSense ? 13 : 18;
    } else {
      Sense[0] = 0x72;
      Sense[1] = 5;
      Sense[2] = mInvalidField ? 0x24 : 0x20;
      Packet->SenseDataLength = mTruncatedSense ? 3 : 8;
    }
    return mStatus == EFI_SUCCESS ? EFI_DEVICE_ERROR : mStatus;
  }
  Packet->SenseDataLength = 0;
  Packet->InTransferLength = mLength;
  Data[Ten ? 1 : 0] = mBadHeader ? 0 : (Ten ? 6 : 3);
  Data[Ten ? 3 : 2] = mProtected ? BIT7 : 0;
  Data[Ten ? 2 : 3] = mProtected ? 0 : BIT7; // WP is not in the adjacent byte.
  return mStatus;
}

STATIC VOID Reset (VOID)
{
  assert (mLiveBuffers == 0);
  mAllocationCalls = mFailAllocation = 0;
  mCalls = mRejectSense = mHost = mTarget = 0;
  mTenCallsExpected = 1; mUnitAttentionStart = mUnitAttentionCount = 0;
  mUnitAttentionFormat = 1;
  mStatus = EFI_SUCCESS;
  mLength = 8;
  mProtected = mBadHeader = mTruncatedSense = mInvalidField = FALSE;
}

int main (void)
{
  EFI_SCSI_IO_PROTOCOL Scsi = { .IoAlign = 4096, .ExecuteScsiCommand = Execute };
  SCSI_DISK_DEV Disk = { .ScsiIo = &Scsi };
  BOOLEAN Protected;
  for (UINT8 Format = 1; Format <= 2; Format++) {
    Reset (); mUnitAttentionFormat = Format; mUnitAttentionCount = 2; mTenCallsExpected = 3;
    mProtected = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_SUCCESS && Protected && mCalls == 3);
    Reset (); mUnitAttentionFormat = Format; mUnitAttentionCount = 9; mTenCallsExpected = 9;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && !Protected && mCalls == 3);
    Reset (); mUnitAttentionFormat = Format; mUnitAttentionCount = 2; mTruncatedSense = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
    Reset (); mUnitAttentionFormat = Format; mUnitAttentionCount = 2; mStatus = EFI_TIMEOUT;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_TIMEOUT && mCalls == 1);
    Reset (); mUnitAttentionFormat = Format; mUnitAttentionCount = 2; mHost = EFI_SCSI_IO_STATUS_HOST_ADAPTER_TIMEOUT;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
    Reset (); mRejectSense = Format; mUnitAttentionFormat = Format;
    mUnitAttentionStart = 1; mUnitAttentionCount = 2; mLength = 4;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_SUCCESS && mCalls == 4);
  }
  Reset (); mUnitAttentionCount = 2; mInvalidField = TRUE;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
  for (UINTN WriteProtected = 0; WriteProtected <= 1; WriteProtected++) {
    Reset ();
    mProtected = WriteProtected != 0;
    Protected = !mProtected;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_SUCCESS);
    assert (Protected == mProtected && mCalls == 1);
    for (UINT8 Format = 1; Format <= 2; Format++) {
      Reset ();
      mRejectSense = Format;
      mLength = 4;
      mProtected = WriteProtected != 0;
      assert (IsWriteProtected (&Disk, &Protected) == EFI_SUCCESS);
      assert (Protected == mProtected && mCalls == 2);
    }
  }
  for (UINT32 Length = 0; Length < 8; Length++) {
    Reset ();
    mLength = Length;
    Protected = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR);
    assert (!Protected && mCalls == 1);
  }
  Reset (); mLength = 65;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR);
  Reset (); mBadHeader = TRUE;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR);
  Reset (); mHost = EFI_SCSI_IO_STATUS_HOST_ADAPTER_DATA_OVERRUN_UNDERRUN;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_SUCCESS);
  Reset (); mHost = EFI_SCSI_IO_STATUS_HOST_ADAPTER_OTHER;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
  Reset (); mTarget = EFI_SCSI_IO_STATUS_TARGET_BUSY;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
  for (UINT8 Format = 1; Format <= 2; Format++) {
    Reset (); mRejectSense = Format; mTruncatedSense = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
    Reset (); mRejectSense = Format; mInvalidField = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
    Reset (); mRejectSense = Format; mStatus = EFI_TIMEOUT;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_TIMEOUT && mCalls == 1);
    Reset (); mRejectSense = Format; mHost = EFI_SCSI_IO_STATUS_HOST_ADAPTER_TIMEOUT;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 1);
    Reset (); mRejectSense = Format; mBadHeader = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_DEVICE_ERROR && mCalls == 2);
  }
  Reset (); mStatus = EFI_TIMEOUT;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_TIMEOUT && mCalls == 1);
  Reset (); mStatus = EFI_UNSUPPORTED;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_UNSUPPORTED && mCalls == 2);
  assert (IsWriteProtected (NULL, &Protected) == EFI_INVALID_PARAMETER);
  assert (IsWriteProtected (&Disk, NULL) == EFI_INVALID_PARAMETER);
  for (UINTN Allocation = 1; Allocation <= 2; Allocation++) {
    Reset (); mFailAllocation = Allocation; Protected = TRUE;
    assert (IsWriteProtected (&Disk, &Protected) == EFI_OUT_OF_RESOURCES);
    assert (mLiveBuffers == 0 && mCalls == 0 && !Protected);
  }
  Scsi.ExecuteScsiCommand = NULL;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_INVALID_PARAMETER);
  Disk.ScsiIo = NULL;
  assert (IsWriteProtected (&Disk, &Protected) == EFI_INVALID_PARAMETER);
  assert (mLiveBuffers == 0);
  puts ("SCSI write-protection passed: MODE SENSE(10)/(6), bounded Unit Attention retries, sense/response validation, aligned buffers and failure cleanup");
  return 0;
}
