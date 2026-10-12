/** @file
*
*  Copyright (c) 2011-2020, ARM Limited. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <Library/BaseMemoryLib.h>

#include "Mmc.h"

EFI_STATUS
MmcNotifyState (
  IN MMC_HOST_INSTANCE  *MmcHostInstance,
  IN MMC_STATE          State
  )
{
  EFI_STATUS Status;
  Status = MmcHostInstance->MmcHost->NotifyState (MmcHostInstance->MmcHost, State);
  if (Status == EFI_SUCCESS) { MmcHostInstance->State = State; }
  return EFI_ERROR (Status) ? Status : (Status == EFI_SUCCESS ? EFI_SUCCESS : EFI_NOT_READY);
}

EFI_STATUS
EFIAPI
MmcGetCardStatus (
  IN MMC_HOST_INSTANCE  *MmcHostInstance
  )
{
  EFI_STATUS             Status;
  UINT32                 Response[4];
  UINTN                  CmdArg;
  EFI_MMC_HOST_PROTOCOL  *MmcHost;

  Status  = EFI_SUCCESS;
  MmcHost = MmcHostInstance->MmcHost;
  CmdArg  = 0;

  if (MmcHost == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  if (MmcHostInstance->State != MmcHwInitializationState) {
    // Get the Status of the card.
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "MmcGetCardStatus(MMC_CMD13): Error and Status = %r\n", Status));
      return Status;
    }

    // Read Response
    Status = MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
    if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_NOT_READY; }
    PrintResponseR1 (Response[0]);
  }

  return Status;
}

STATIC
EFI_STATUS
MmcResetWorker (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN BOOLEAN                ExtendedVerification
  )
{
  MMC_HOST_INSTANCE  *MmcHostInstance;

  MmcHostInstance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);

  if (MmcHostInstance->MmcHost == NULL) {
    // Nothing to do
    return EFI_SUCCESS;
  }

  // If a card is not present then clear all media settings
  if (!MmcHostInstance->MmcHost->IsCardPresent (MmcHostInstance->MmcHost)) {
    MmcHostInstance->Initialized = FALSE;
    MmcHostInstance->BlockIo.Media->MediaId++;
    MmcHostInstance->BlockIo.Media->MediaPresent = FALSE;
    MmcHostInstance->BlockIo.Media->LastBlock    = 0;
    MmcHostInstance->BlockIo.Media->BlockSize    = 512;  // Should be zero but there is a bug in DiskIo
    MmcHostInstance->BlockIo.Media->ReadOnly     = FALSE;

    // Indicate that the driver requires initialization
    MmcHostInstance->State = MmcHwInitializationState;
    MmcHostInstance->ChangePending = TRUE;

    return EFI_SUCCESS;
  }

  // Implement me. Either send a CMD0 (could not work for some MMC host) or just turn off/turn
  //      on power and restart Identification mode
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MmcReset (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN BOOLEAN ExtendedVerification
  )
{
  MMC_HOST_INSTANCE *Instance;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  Instance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((OldTpl > TPL_CALLBACK) || Instance->Stopping || Instance->Busy) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  Instance->Busy = TRUE;
  gBS->RestoreTPL (OldTpl);
  Status = MmcResetWorker (This, ExtendedVerification);
  gBS->RaiseTPL (TPL_NOTIFY);
  Instance->Busy = FALSE;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
MmcDetectCard (
  EFI_MMC_HOST_PROTOCOL  *MmcHost
  )
{
  if (!MmcHost->IsCardPresent (MmcHost)) {
    return EFI_NO_MEDIA;
  } else {
    return EFI_SUCCESS;
  }
}

EFI_STATUS
MmcStopTransmission (
  EFI_MMC_HOST_PROTOCOL  *MmcHost
  )
{
  EFI_STATUS  Status;
  UINT32      Response[4];

  // Command 12 - Stop transmission (ends read or write)
  // Normally only needed for streaming transfers or after error.
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD12, 0);
  if (Status == EFI_SUCCESS) {
    Status = MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1b, Response);
  }

  return EFI_ERROR (Status) ? Status : (Status == EFI_SUCCESS ? EFI_SUCCESS : EFI_NOT_READY);
}

#define MMCI0_BLOCKLEN  512
#define MMCI0_TIMEOUT   10000

STATIC
EFI_STATUS
MmcWaitForTransfer (
  IN MMC_HOST_INSTANCE *Instance,
  IN UINTN Attempts
  )
{
  EFI_STATUS Status;
  UINT32 Response[4];
  UINTN Index;
  EFI_MMC_HOST_PROTOCOL *Host;

  Host = Instance->MmcHost;
  for (Index = 0; Index < Attempts; ++Index) {
    if (!Host->IsCardPresent (Host)) { return EFI_NO_MEDIA; }
    Status = Host->SendCommand (Host, MMC_CMD13, (UINT32)Instance->CardInfo.RCA << 16);
    if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_NOT_READY; }
    Status = Host->ReceiveResponse (Host, MMC_RESPONSE_TYPE_R1, Response);
    if (Status != EFI_SUCCESS) { return EFI_ERROR (Status) ? Status : EFI_NOT_READY; }
    if ((Response[0] & MMC_R0_READY_FOR_DATA) &&
        (MMC_R0_CURRENTSTATE (Response) == MMC_R0_STATE_TRAN)) {
      return EFI_SUCCESS;
    }
    gBS->Stall (100);
  }
  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
MmcTransferBlock (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINTN                  Cmd,
  IN UINTN                  Transfer,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  EFI_STATUS             Status;
  UINTN                  CmdArg;
  MMC_HOST_INSTANCE      *MmcHostInstance;
  EFI_MMC_HOST_PROTOCOL  *MmcHost;

  MmcHostInstance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);
  MmcHost         = MmcHostInstance->MmcHost;

  if (MmcHostInstance->CardInfo.CardType != EMMC_CARD) {
    // Set command argument based on the card capacity
    // if 0 : SDSC card
    // if 1 : SDXC/SDHC
    if (MmcHostInstance->CardInfo.OCRData.AccessMode & SD_CARD_CAPACITY) {
      CmdArg = Lba;
    } else {
      CmdArg = MultU64x32 (Lba, This->Media->BlockSize);
    }
  } else {
    // Set command argument based on the card access mode (Byte mode or Block mode)
    if ((MmcHostInstance->CardInfo.OCRData.AccessMode & MMC_OCR_ACCESS_MASK) ==
        MMC_OCR_ACCESS_SECTOR)
    {
      CmdArg = Lba;
    } else {
      CmdArg = MultU64x32 (Lba, This->Media->BlockSize);
    }
  }

  if (CmdArg > MAX_UINT32) { return EFI_INVALID_PARAMETER; }
  Status = MmcHost->SendCommand (MmcHost, Cmd, (UINT32)CmdArg);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "%a(MMC_CMD%d): Error %r\n", __func__, Cmd, Status));
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }

  if (Transfer == MMC_IOBLOCKS_READ) {
    // Read Data
    Status = MmcHost->ReadBlockData (MmcHost, Lba, BufferSize, Buffer);
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_BLKIO, "%a(): Error Read Block Data and Status = %r\n", __func__, Status));
      MmcStopTransmission (MmcHost);
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }

    Status = MmcNotifyState (MmcHostInstance, MmcProgrammingState);
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "%a() : Error MmcProgrammingState\n", __func__));
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }
  } else {
    // Write Data
    Status = MmcHost->WriteBlockData (MmcHost, Lba, BufferSize, Buffer);
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_BLKIO, "%a(): Error Write Block Data and Status = %r\n", __func__, Status));
      MmcStopTransmission (MmcHost);
      return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
    }
  }

  /* A multi-block stream stays DATA/RCV until CMD12. Terminate it before
     requiring READY+TRAN; a successful status response alone is insufficient. */
  if (BufferSize > This->Media->BlockSize) {
    Status = MmcStopTransmission (MmcHost);
    if (Status != EFI_SUCCESS) { return Status; }
  }
  Status = MmcWaitForTransfer (MmcHostInstance, MMCI0_TIMEOUT);
  if (Status != EFI_SUCCESS) { return Status; }

  Status = MmcNotifyState (MmcHostInstance, MmcTransferState);
  if (Status != EFI_SUCCESS) {
    DEBUG ((DEBUG_ERROR, "MmcIoBlocks() : Error MmcTransferState\n"));
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }

  return Status;
}

STATIC
EFI_STATUS
MmcIoBlocksWorker (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINTN                  Transfer,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  EFI_STATUS             Status;
  UINTN                  Cmd;
  MMC_HOST_INSTANCE      *MmcHostInstance;
  EFI_MMC_HOST_PROTOCOL  *MmcHost;
  UINTN                  BytesRemainingToBeTransferred;
  UINTN                  BlockCount;
  UINTN                  ConsumeSize;
  UINT32                 MaxBlock;
  UINTN                  RemainingBlock;
  BOOLEAN                MultiBlock;

  BlockCount      = 1;
  MmcHostInstance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);
  ASSERT (MmcHostInstance != NULL);
  MmcHost = MmcHostInstance->MmcHost;
  ASSERT (MmcHost);

  if (This->Media->MediaId != MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((MmcHost == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Check if a Card is Present
  if (!MmcHostInstance->BlockIo.Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }

  // Reading 0 Byte is valid
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  // The buffer size must be an exact multiple of the block size
  if ((BufferSize % This->Media->BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  MultiBlock = MMC_HOST_HAS_ISMULTIBLOCK (MmcHost) && MmcHost->IsMultiBlock (MmcHost);
  if (MultiBlock) {
    BlockCount = BufferSize / This->Media->BlockSize;
  }

  // All blocks must be within the device
  if ((Lba > This->Media->LastBlock) ||
      ((BufferSize / This->Media->BlockSize - 1) > (This->Media->LastBlock - Lba))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Transfer == MMC_IOBLOCKS_WRITE) && (This->Media->ReadOnly == TRUE)) {
    return EFI_WRITE_PROTECTED;
  }

  // Check the alignment
  if ((This->Media->IoAlign > 2) && (((UINTN)Buffer & (This->Media->IoAlign - 1)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  // Max block number in single cmd is 65535 blocks.
  MaxBlock                      = 0xFFFF;
  RemainingBlock                = BlockCount;
  BytesRemainingToBeTransferred = BufferSize;
  while (BytesRemainingToBeTransferred > 0) {
    if (RemainingBlock <= MaxBlock) {
      BlockCount = RemainingBlock;
    } else {
      BlockCount = MaxBlock;
    }

    Status = MmcWaitForTransfer (MmcHostInstance, 20);
    if (Status != EFI_SUCCESS) { return Status; }

    if (Transfer == MMC_IOBLOCKS_READ) {
      if (BlockCount == 1) {
        // Read a single block
        Cmd = MMC_CMD17;
      } else {
        // Read multiple blocks
        Cmd = MMC_CMD18;
      }
    } else {
      if (BlockCount == 1) {
        // Write a single block
        Cmd = MMC_CMD24;
      } else {
        // Write multiple blocks
        Cmd = MMC_CMD25;
      }
    }

    ConsumeSize = BlockCount * This->Media->BlockSize;
    if (BytesRemainingToBeTransferred < ConsumeSize) {
      ConsumeSize = BytesRemainingToBeTransferred;
    }

    Status = MmcTransferBlock (This, Cmd, Transfer, MediaId, Lba, ConsumeSize, Buffer);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "%a(): Failed to transfer block and Status:%r\n", __func__, Status));
      return Status;
    }

    if (MultiBlock) { RemainingBlock -= BlockCount; }
    BytesRemainingToBeTransferred -= ConsumeSize;
    if (BytesRemainingToBeTransferred > 0) {
      Lba   += BlockCount;
      Buffer = (UINT8 *)Buffer + ConsumeSize;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
MmcIoBlocks (
  IN EFI_BLOCK_IO_PROTOCOL *This,
  IN UINTN Transfer,
  IN UINT32 MediaId,
  IN EFI_LBA Lba,
  IN UINTN BufferSize,
  OUT VOID *Buffer
  )
{
  MMC_HOST_INSTANCE *Instance;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  Instance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if ((OldTpl > TPL_CALLBACK) || Instance->Stopping || Instance->Busy) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  Instance->Busy = TRUE;
  gBS->RestoreTPL (OldTpl);
  Status = MmcIoBlocksWorker (This, Transfer, MediaId, Lba, BufferSize, Buffer);
  gBS->RaiseTPL (TPL_NOTIFY);
  Instance->Busy = FALSE;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

EFI_STATUS
EFIAPI
MmcReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  OUT VOID                  *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_READ, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This,
  IN UINT32                 MediaId,
  IN EFI_LBA                Lba,
  IN UINTN                  BufferSize,
  IN VOID                   *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_WRITE, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}
