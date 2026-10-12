/** @file
  UfsPassThruDxe driver is used to produce EFI_EXT_SCSI_PASS_THRU protocol interface
  for upper layer application to execute UFS-supported SCSI cmds.

  Copyright (c) 2014 - 2022, Intel Corporation. All rights reserved.<BR>
  Copyright (c) Microsoft Corporation.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "UfsPassThru.h"

STATIC VOID UfsQueueOwnedRequest (UFS_PASS_THRU_PRIVATE_DATA *Private, UFS_PASS_THRU_TRANS_REQ *Request);
STATIC EFI_STATUS UfsFinishOwnedRequest (UFS_PASS_THRU_PRIVATE_DATA *Private, UFS_PASS_THRU_TRANS_REQ *Request);


/**
  Read 32bits data from specified UFS MMIO register.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Offset        The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[out] Value         The data buffer to store.

  @retval EFI_TIMEOUT       The operation is time out.
  @retval EFI_SUCCESS       The operation succeeds.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsMmioRead32 (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINTN                       Offset,
  OUT UINT32                         *Value
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EFI_STATUS                          Status;

  UfsHc = Private->UfsHostController;

  Status = UfsHc->Read (UfsHc, EfiUfsHcWidthUint32, Offset, 1, Value);

  return Status;
}

/**
  Write 32bits data to specified UFS MMIO register.

  @param[in] Private        The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in] Offset         The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[in] Value          The data to write.

  @retval EFI_TIMEOUT       The operation is time out.
  @retval EFI_SUCCESS       The operation succeeds.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsMmioWrite32 (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINTN                       Offset,
  IN  UINT32                      Value
  )
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL  *UfsHc;
  EFI_STATUS                          Status;

  UfsHc = Private->UfsHostController;

  Status = UfsHc->Write (UfsHc, EfiUfsHcWidthUint32, Offset, 1, &Value);

  return Status;
}


STATIC EFI_STATUS
UfsCheckControllerErrors (UFS_PASS_THRU_PRIVATE_DATA *Private)
{
  STATIC CONST UINT32 Offsets[] = {
    UFS_HC_STATUS_OFFSET, UFS_HC_ENABLE_OFFSET,
    UFS_HC_UTRLBA_OFFSET, UFS_HC_UTRLBAU_OFFSET,
    UFS_HC_UTRLDBR_OFFSET, UFS_HC_UTRLRSR_OFFSET,
    UFS_HC_CAP_OFFSET, UFS_HC_VER_OFFSET,
    UFS_HC_UTMRLBA_OFFSET, UFS_HC_UTMRLBAU_OFFSET,
    UFS_HC_UTMRLDBR_OFFSET, UFS_HC_UTMRLRSR_OFFSET,
    UFS_HC_UECPA_OFFSET, UFS_HC_UECDL_OFFSET, UFS_HC_UECN_OFFSET,
    UFS_HC_UECT_OFFSET, UFS_HC_UECDME_OFFSET
  };
  UINT32 Is, Value;
  UINTN Index;
  EFI_STATUS Status;
  if (Private->FatalError != 0) return EFI_DEVICE_ERROR;
  Status = UfsMmioRead32 (Private, UFS_HC_IS_OFFSET, &Is);
  if (EFI_ERROR (Status)) return Status;
  if ((Is & UFS_HC_FATAL_ERROR_MASK) == 0) return EFI_SUCCESS;
  Private->FatalError = Is & UFS_HC_FATAL_ERROR_MASK;
  Private->IoUnavailable = TRUE;
  // Read before HCE/UTRLCLR or SMMU revocation can alter the failing state.
  DEBUG ((DEBUG_ERROR, "UFS: fatal IS=%08x; controller submissions closed\n", Is));
  DEBUG ((DEBUG_ERROR, "UFS: fatal doorbell attempts=%u writes=%u last-mask=%08x\n",
          Private->DoorbellAttempts, Private->DoorbellWrites, Private->LastDoorbellMask));
  for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
    Value = 0;
    Status = UfsMmioRead32 (Private, Offsets[Index], &Value);
    DEBUG ((DEBUG_ERROR, "UFS: fatal reg[%02x]=%08x (%r)\n", Offsets[Index], Value, Status));
  }
  if (Private->UtpTrlBase != NULL) {
    for (Index = 0; Index < 32; Index++) {
      if ((Private->ReservedSlots & (1U << Index)) != 0) {
        UTP_TRD *Trd = (UTP_TRD *)Private->UtpTrlBase + Index;
        DEBUG ((DEBUG_ERROR, "UFS: fatal slot=%u OCS=%x UCD=%Lx response=%u/%u PRDT=%u/%u\n",
          (UINT32)Index, Trd->Ocs,
          ((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7),
          Trd->RuO * 4, Trd->RuL * 4, Trd->PrdtO * 4, Trd->PrdtL));
      }
    }
  }
  return EFI_DEVICE_ERROR;
}

// Read only CPU-owned descriptor bytes; never dereference the device IOVA.
// Bound boot traces to four requests so fDeviceInit polling stays practical.
STATIC VOID
UfsTraceRequest (UFS_PASS_THRU_PRIVATE_DATA *Private, UTP_TRD *Trd,
                 VOID *Host, UINT32 Size, UINT8 Slot)
{
  UINT32 Dw[8], Up[8];
  UINT64 Device;
  if (Private->TraceCount >= 4 || Host == NULL || Size < sizeof (Up)) return;
  Private->TraceCount++;
  CopyMem (Dw, Trd, sizeof (Dw));
  CopyMem (Up, Host, sizeof (Up));
  Device = ((UINT64)Trd->UcdBaU << 32) | ((UINT64)Trd->UcdBa << 7);
  DEBUG ((DEBUG_WARN, "UFS: prepared slot=%u host=%p UCD=%Lx bytes=%u slot-end=%Lx pool-end=%Lx\n",
          Slot, Host, Device, Size, Device + UFS_UCD_SLOT_SIZE,
          Private->UcdDeviceAddress + Private->UcdPoolSize));
  DEBUG ((DEBUG_WARN, "UFS: TRD %08x %08x %08x %08x %08x %08x %08x %08x\n",
          Dw[0], Dw[1], Dw[2], Dw[3], Dw[4], Dw[5], Dw[6], Dw[7]));
  DEBUG ((DEBUG_WARN, "UFS: UPIU %08x %08x %08x %08x %08x %08x %08x %08x\n",
          Up[0], Up[1], Up[2], Up[3], Up[4], Up[5], Up[6], Up[7]));
}

/**
  Wait for the value of the specified system memory set to the test value.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Offset        The offset within the UFS Host Controller MMIO space to start
                            the memory operation.
  @param[in]  MaskValue     The mask value of memory.
  @param[in]  TestValue     The test value of memory.
  @param[in]  Timeout       The time out value for wait memory set, uses 100ns as a unit.

  @retval EFI_TIMEOUT       The system memory setting is time out.
  @retval EFI_SUCCESS       The system memory is correct set.
  @retval Others            The operation fails.

**/
EFI_STATUS
UfsWaitMemSet (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINTN                       Offset,
  IN  UINT32                      MaskValue,
  IN  UINT32                      TestValue,
  IN  UINT64                      Timeout
  )
{
  UINT32      Value;
  UINT64      Delay;
  BOOLEAN     InfiniteWait;
  EFI_STATUS  Status;

  if (Timeout == 0) {
    InfiniteWait = TRUE;
  } else {
    InfiniteWait = FALSE;
  }

  Delay = DivU64x32 (Timeout, 10) + 1;

  do {
    //
    // Access PCI MMIO space to see if the value is the tested one.
    //
    Status = UfsMmioRead32 (Private, Offset, &Value);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // Completion polling must observe sticky fatal status even if DBR is
    // already clear. Cleanup polling still needs to verify the HCI stop.
    if (Offset == UFS_HC_UTRLDBR_OFFSET && TestValue == 0 &&
        !Private->CleanupInProgress) {
      Status = UfsCheckControllerErrors (Private);
      if (EFI_ERROR (Status)) return Status;
    }
    Value &= MaskValue;

    if (Value == TestValue) {
      return EFI_SUCCESS;
    }

    //
    // Stall for 1 microseconds.
    //
    MicroSecondDelay (1);

    Delay--;
  } while (InfiniteWait || (Delay > 0));

  return EFI_TIMEOUT;
}

/**
  Dump UIC command execution result for debugging.

  @param[in]   UicOpcode  The executed UIC opcode.
  @param[in]   Result     The result to be parsed.

**/
VOID
DumpUicCmdExecResult (
  IN  UINT8  UicOpcode,
  IN  UINT8  Result
  )
{
  if (UicOpcode <= UfsUicDmePeerSet) {
    switch (Result) {
      case 0x00:
        break;
      case 0x01:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - INVALID_MIB_ATTRIBUTE\n"));
        break;
      case 0x02:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - INVALID_MIB_ATTRIBUTE_VALUE\n"));
        break;
      case 0x03:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - READ_ONLY_MIB_ATTRIBUTE\n"));
        break;
      case 0x04:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - WRITE_ONLY_MIB_ATTRIBUTE\n"));
        break;
      case 0x05:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BAD_INDEX\n"));
        break;
      case 0x06:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - LOCKED_MIB_ATTRIBUTE\n"));
        break;
      case 0x07:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BAD_TEST_FEATURE_INDEX\n"));
        break;
      case 0x08:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - PEER_COMMUNICATION_FAILURE\n"));
        break;
      case 0x09:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - BUSY\n"));
        break;
      case 0x0A:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - DME_FAILURE\n"));
        break;
      default:
        DEBUG ((DEBUG_VERBOSE, "UIC configuration command fails - UNKNOWN_ERROR (0x%02x)\n", Result));
        break;
    }
  } else {
    switch (Result) {
      case 0x00:
        break;
      case 0x01:
        DEBUG ((DEBUG_VERBOSE, "UIC control command fails - FAILURE\n"));
        break;
      default:
        DEBUG ((DEBUG_VERBOSE, "UIC control command fails - UNKNOWN_ERROR (0x%02x)\n", Result));
        break;
    }
  }
}

/**
  Dump QUERY RESPONSE UPIU result for debugging.

  @param[in]   Result  The result to be parsed.

**/
VOID
DumpQueryResponseResult (
  IN  UINT8  Result
  )
{
  switch (Result) {
    case 0xF6:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Not Readable\n"));
      break;
    case 0xF7:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Not Writeable\n"));
      break;
    case 0xF8:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Parameter Already Written\n"));
      break;
    case 0xF9:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Length\n"));
      break;
    case 0xFA:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Value\n"));
      break;
    case 0xFB:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Selector\n"));
      break;
    case 0xFC:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Index\n"));
      break;
    case 0xFD:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Idn\n"));
      break;
    case 0xFE:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Invalid Opcode\n"));
      break;
    case 0xFF:
      DEBUG ((DEBUG_VERBOSE, "Query Response with General Failure\n"));
      break;
    default:
      DEBUG ((DEBUG_VERBOSE, "Query Response with Unknown Error (0x%02x)\n", Result));
      break;
  }
}

/**
  Swap little endian to big endian.

  @param[in, out] Buffer      The data buffer. In input, it contains little endian data.
                              In output, it will become big endian.
  @param[in]      BufferSize  The length of converted data.

**/
VOID
SwapLittleEndianToBigEndian (
  IN OUT UINT8   *Buffer,
  IN     UINT32  BufferSize
  )
{
  UINT32  Index;
  UINT8   Temp;
  UINT32  SwapCount;

  SwapCount = BufferSize / 2;
  for (Index = 0; Index < SwapCount; Index++) {
    Temp                           = Buffer[Index];
    Buffer[Index]                  = Buffer[BufferSize - 1 - Index];
    Buffer[BufferSize - 1 - Index] = Temp;
  }
}

/**
  Fill TSF field of QUERY REQUEST UPIU.

  @param[in, out] TsfBase      The base address of TSF field of QUERY REQUEST UPIU.
  @param[in]      Opcode       The opcode of request.
  @param[in]      DescId       The descriptor ID of request.
  @param[in]      Index        The index of request.
  @param[in]      Selector     The selector of request.
  @param[in]      Length       The length of transferred data. The maximum is 4.
  @param[in]      Value        The value of transferred data.

**/
VOID
UfsFillTsfOfQueryReqUpiu (
  IN OUT UTP_UPIU_TSF  *TsfBase,
  IN     UINT8         Opcode,
  IN     UINT8         DescId    OPTIONAL,
  IN     UINT8         Index     OPTIONAL,
  IN     UINT8         Selector  OPTIONAL,
  IN     UINT16        Length    OPTIONAL,
  IN     UINT32        Value     OPTIONAL
  )
{
  ASSERT (TsfBase != NULL);
  ASSERT (Opcode <= UtpQueryFuncOpcodeTogFlag);

  TsfBase->Opcode = Opcode;
  if (Opcode != UtpQueryFuncOpcodeNop) {
    TsfBase->DescId   = DescId;
    TsfBase->Index    = Index;
    TsfBase->Selector = Selector;

    if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeWrDesc)) {
      SwapLittleEndianToBigEndian ((UINT8 *)&Length, sizeof (Length));
      TsfBase->Length = Length;
    }

    if (Opcode == UtpQueryFuncOpcodeWrAttr) {
      SwapLittleEndianToBigEndian ((UINT8 *)&Value, sizeof (Value));
      TsfBase->Value = Value;
    }
  }
}

/**
  Initialize COMMAND UPIU.

  @param[in, out] Command         The base address of COMMAND UPIU.
  @param[in]      Lun             The Lun on which the SCSI command is executed.
  @param[in]      TaskTag         The task tag of request.
  @param[in]      Cdb             The cdb buffer containing SCSI command.
  @param[in]      CdbLength       The cdb length.
  @param[in]      DataDirection   The direction of data transfer.
  @param[in]      ExpDataTranLen  The expected transfer data length.

  @retval EFI_SUCCESS     The initialization succeed.

**/
EFI_STATUS
UfsInitCommandUpiu (
  IN OUT UTP_COMMAND_UPIU    *Command,
  IN     UINT8               Lun,
  IN     UINT8               TaskTag,
  IN     UINT8               *Cdb,
  IN     UINT8               CdbLength,
  IN     UFS_DATA_DIRECTION  DataDirection,
  IN     UINT32              ExpDataTranLen
  )
{
  UINT8  Flags;

  ASSERT ((Command != NULL) && (Cdb != NULL));

  //
  // Task attribute is hard-coded to Ordered.
  //
  if (DataDirection == UfsDataIn) {
    Flags = BIT0 | BIT6;
  } else if (DataDirection == UfsDataOut) {
    Flags = BIT0 | BIT5;
  } else {
    Flags = BIT0;
  }

  //
  // Fill UTP COMMAND UPIU associated fields.
  //
  Command->TransCode = 0x01;
  Command->Flags     = Flags;
  Command->Lun       = Lun;
  Command->TaskTag   = TaskTag;
  Command->CmdSet    = 0x00;
  SwapLittleEndianToBigEndian ((UINT8 *)&ExpDataTranLen, sizeof (ExpDataTranLen));
  Command->ExpDataTranLen = ExpDataTranLen;

  CopyMem (Command->Cdb, Cdb, CdbLength);

  return EFI_SUCCESS;
}

/**
  Initialize UTP PRDT for data transfer.

  @param[in] Prdt         The base address of PRDT.
  @param[in] Buffer       The buffer to be read or written.
  @param[in] BufferSize   The data size to be read or written.

  @retval EFI_SUCCESS     The initialization succeed.

**/
EFI_STATUS
UfsInitUtpPrdt (
  IN  UTP_TR_PRD  *Prdt,
  IN  VOID        *Buffer,
  IN  UINT32      BufferSize
  )
{
  UINT32  PrdtIndex;
  UINT32  RemainingLen;
  UINT8   *Remaining;
  UINTN   PrdtNumber;

  ASSERT (((UINTN)Buffer & (BIT0 | BIT1)) == 0);
  ASSERT ((BufferSize & (BIT1 | BIT0)) == 0);

  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  RemainingLen = BufferSize;
  Remaining    = Buffer;
  PrdtNumber   = (UINTN)DivU64x32 ((UINT64)BufferSize + UFS_MAX_DATA_LEN_PER_PRD - 1, UFS_MAX_DATA_LEN_PER_PRD);

  for (PrdtIndex = 0; PrdtIndex < PrdtNumber; PrdtIndex++) {
    if (RemainingLen < UFS_MAX_DATA_LEN_PER_PRD) {
      Prdt[PrdtIndex].DbCount = (UINT32)RemainingLen - 1;
    } else {
      Prdt[PrdtIndex].DbCount = UFS_MAX_DATA_LEN_PER_PRD - 1;
    }

    Prdt[PrdtIndex].DbAddr  = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 2);
    Prdt[PrdtIndex].DbAddrU = (UINT32)RShiftU64 ((UINT64)(UINTN)Remaining, 32);
    RemainingLen           -= UFS_MAX_DATA_LEN_PER_PRD;
    Remaining              += UFS_MAX_DATA_LEN_PER_PRD;
  }

  return EFI_SUCCESS;
}

/**
  Initialize QUERY REQUEST UPIU.

  @param[in, out] QueryReq      The base address of QUERY REQUEST UPIU.
  @param[in]      TaskTag       The task tag of request.
  @param[in]      Opcode        The opcode of request.
  @param[in]      DescId        The descriptor ID of request.
  @param[in]      Index         The index of request.
  @param[in]      Selector      The selector of request.
  @param[in]      DataSize      The data size to be read or written.
  @param[in]      Data          The buffer to be read or written.

  @retval EFI_SUCCESS           The initialization succeed.

**/
EFI_STATUS
UfsInitQueryRequestUpiu (
  IN OUT UTP_QUERY_REQ_UPIU  *QueryReq,
  IN     UINT8               TaskTag,
  IN     UINT8               Opcode,
  IN     UINT8               DescId,
  IN     UINT8               Index,
  IN     UINT8               Selector,
  IN     UINTN               DataSize   OPTIONAL,
  IN     UINT8               *Data      OPTIONAL
  )
{
  ASSERT (QueryReq != NULL);

  QueryReq->TransCode = 0x16;
  QueryReq->TaskTag   = TaskTag;
  if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeRdFlag) || (Opcode == UtpQueryFuncOpcodeRdAttr)) {
    QueryReq->QueryFunc = QUERY_FUNC_STD_READ_REQ;
  } else {
    QueryReq->QueryFunc = QUERY_FUNC_STD_WRITE_REQ;
  }

  if (Opcode == UtpQueryFuncOpcodeWrAttr) {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, 0, *(UINT32 *)Data);
  } else if ((Opcode == UtpQueryFuncOpcodeRdDesc) || (Opcode == UtpQueryFuncOpcodeWrDesc)) {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, (UINT16)DataSize, 0);
  } else {
    UfsFillTsfOfQueryReqUpiu (&QueryReq->Tsf, Opcode, DescId, Index, Selector, 0, 0);
  }

  if (Opcode == UtpQueryFuncOpcodeWrDesc) {
    CopyMem (QueryReq + 1, Data, DataSize);

    SwapLittleEndianToBigEndian ((UINT8 *)&DataSize, sizeof (UINT16));
    QueryReq->DataSegLen = (UINT16)DataSize;
  }

  return EFI_SUCCESS;
}

// Borrow the reserved slot's permanent UCD; the controller owns its mapping.
// No device address is ever used as a CPU pointer.
STATIC EFI_STATUS
UfsAcquireCommandDescriptor (
  IN UFS_PASS_THRU_PRIVATE_DATA *Private,
  IN UTP_TRD                   *Trd,
  OUT VOID                     **Host,
  OUT EFI_PHYSICAL_ADDRESS     *Device,
  OUT VOID                     **Mapping
  )
{
  UINTN Offset, Slot;
  *Host = NULL;
  *Device = 0;
  *Mapping = NULL;
  if (Private->UtpUcdBase == NULL || Private->UtpTrlBase == NULL || Private->Nutrs == 0)
    return EFI_NOT_READY;
  if ((UINTN)Trd < (UINTN)Private->UtpTrlBase) return EFI_INVALID_PARAMETER;
  Offset = (UINTN)Trd - (UINTN)Private->UtpTrlBase;
  if (Offset % sizeof (*Trd) != 0 || Offset / sizeof (*Trd) >= Private->Nutrs)
    return EFI_INVALID_PARAMETER;
  Slot = Offset / sizeof (*Trd);
  Offset = Slot * UFS_UCD_SLOT_SIZE;
  if (Offset > Private->UcdPoolSize || UFS_UCD_SLOT_SIZE > Private->UcdPoolSize - Offset)
    return EFI_COMPROMISED_DATA;
  *Host = (UINT8 *)Private->UtpUcdBase + Offset;
  *Device = Private->UcdDeviceAddress + Offset;
  // Reuse must remove stale response, PRDT and command payload bytes.
  ZeroMem (*Host, UFS_UCD_SLOT_SIZE);
  return EFI_SUCCESS;
}

/**
  Prepare COMMAND/RESPONSE UPIU in the reserved slot's permanent UCD.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Lun               The Lun on which the SCSI command is executed.
  @param[in]  Packet            The pointer to the EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       Borrowed CPU address; the controller owns its backing.
  @param[out] CmdDescMapping    Always NULL; the request does not own the pool mapping.

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.

**/
STATIC EFI_STATUS
UfsAlignedTransferLength (UINT32 Length, UINT32 *Aligned)
{
  if (Length > MAX_UINT32 - 3U) return EFI_BAD_BUFFER_SIZE;
  *Aligned = (Length + 3U) & ~3U;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
UfsValidateScsiResponse (UFS_PASS_THRU_PRIVATE_DATA *Private,
                         UFS_PASS_THRU_TRANS_REQ *Request)
{
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET *Packet = Request->Packet;
  UTP_COMMAND_UPIU *Command = Request->CmdDescHost;
  UTP_RESPONSE_UPIU *Response = (UTP_RESPONSE_UPIU *)
    ((UINT8 *)Request->CmdDescHost + Request->Trd->RuO * sizeof (UINT32));
  UINT16 SenseLength = Response->SenseDataLen;
  UINT16 SegmentLength = Response->DataSegLen;
  UINT16 CopyLength = 0;
  UINT32 Residual = Response->ResTranCount;
  UINT32 *Length = Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ ?
                   &Packet->InTransferLength : &Packet->OutTransferLength;
  UINT32 Expected = *Length;
  BOOLEAN Valid;
  EFI_STATUS Status = EFI_SUCCESS;
  SwapLittleEndianToBigEndian ((UINT8 *)&SenseLength, sizeof (SenseLength));
  SwapLittleEndianToBigEndian ((UINT8 *)&SegmentLength, sizeof (SegmentLength));
  SwapLittleEndianToBigEndian ((UINT8 *)&Residual, sizeof (Residual));
  Packet->TargetStatus = Response->Status;
  // Like Linux, classify the transport by OCS/type and the target by SCSI
  // status. A nonzero Response byte does not erase valid CHECK CONDITION.
  Valid = Request->Trd->Ocs == 0 && Response->TransCode == 0x21 && Response->EhsLen == 0 &&
          Response->TaskTag == Command->TaskTag && Response->Lun == Command->Lun;
  if (Valid && SegmentLength != 0) {
    if (SegmentLength < sizeof (UINT16) || SenseLength > SegmentLength - sizeof (UINT16)) {
      Valid = FALSE;
    } else if (Packet->SenseData != NULL) {
      // The host retains an 18-byte prefix; both wire and client may declare
      // longer buffers. Expose the available prefix without reading past it.
      CopyLength = (UINT16)MIN (SenseLength, sizeof (Response->SenseData));
      CopyLength = MIN (CopyLength, Packet->SenseDataLength);
      if (CopyLength != 0) CopyMem (Packet->SenseData, Response->SenseData, CopyLength);
    }
  }
  Packet->SenseDataLength = (UINT8)CopyLength;
  if (!Valid || (Response->Flags & BIT6) != 0 || Residual > Expected) {
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    *Length = 0; // No validated data may be copied from a bounce buffer.
    Status = EFI_DEVICE_ERROR;
  } else {
    // Some UFS devices omit the underflow flag even with a residual count.
    *Length -= Residual;
    Packet->HostAdapterStatus = EFI_EXT_SCSI_STATUS_HOST_ADAPTER_OK;
  }
  if (EFI_ERROR (Status) || Response->Status != 0 || Response->Response != 0) {
    DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_WARN,
            "UFS: SCSI op=%02x slot=%u request-lun=%x request-tag=%u lun=%x tag=%u OCS=%x type=%02x response=%02x status=%02x flags=%02x ehs=%u segment=%u sense=%u/%u residual=%u expected=%u: %r\n",
            Packet->Cdb != NULL && Packet->CdbLength != 0 ? ((UINT8 *)Packet->Cdb)[0] : 0,
            Request->Slot, Command->Lun, Command->TaskTag,
            Response->Lun, Response->TaskTag, Request->Trd->Ocs, Response->TransCode,
            Response->Response, Response->Status, Response->Flags, Response->EhsLen,
            SegmentLength, SenseLength, CopyLength, Residual, Expected, Status));
  }
  if (EFI_ERROR (Status)) {
    STATIC CONST UINTN Offsets[] = {
      UFS_HC_IS_OFFSET, UFS_HC_STATUS_OFFSET, UFS_HC_ENABLE_OFFSET,
      UFS_HC_UTRLBA_OFFSET, UFS_HC_UTRLBAU_OFFSET,
      UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLDBR_OFFSET,
      UFS_HC_UECPA_OFFSET, UFS_HC_UECDL_OFFSET, UFS_HC_UECN_OFFSET,
      UFS_HC_UECT_OFFSET, UFS_HC_UECDME_OFFSET
    };
    UINTN Index;
    UINT32 Value;
    EFI_STATUS ReadStatus;
    // Capture the failing transport before cleanup revokes its DMA mappings.
    DEBUG ((DEBUG_ERROR, "UFS: TRD command=%08x:%08x response=%u/%u PRDT=%u/%u\n",
            Request->Trd->UcdBaU, (UINT32)Request->Trd->UcdBa << 7,
            Request->Trd->RuO, Request->Trd->RuL,
            Request->Trd->PrdtO, Request->Trd->PrdtL));
    for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
      Value = 0;
      ReadStatus = UfsMmioRead32 (Private, Offsets[Index], &Value);
      DEBUG ((DEBUG_ERROR, "UFS: SCSI reg[%02x]=%08x (%r)\n", (UINT32)Offsets[Index], Value, ReadStatus));
    }
  }
  return Status;
}

EFI_STATUS
UfsCreateScsiCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA                  *Private,
  IN     UINT8                                       Lun,
  IN     EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     UTP_TRD                                     *Trd,
  OUT VOID                                           **CmdDescHost,
  OUT VOID                                           **CmdDescMapping
  )
{
  UINTN                 PrdtNumber;
  UTP_COMMAND_UPIU      *CommandUpiu;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  EFI_STATUS            Status;
  UINT32                DataLen;
  UINT32                AlignedLength;
  UFS_DATA_DIRECTION    DataDirection;

  ASSERT ((Private != NULL) && (Packet != NULL) && (Trd != NULL));

  if (Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
    DataLen       = Packet->InTransferLength;
    DataDirection = UfsDataIn;
  } else {
    DataLen       = Packet->OutTransferLength;
    DataDirection = UfsDataOut;
  }

  if (DataLen == 0) {
    DataDirection = UfsNoData;
  }

  // Fixed Android-sized PRDT capacity: report the supported transfer size.
  if (DataLen > UFS_MAX_TRANSFER_BYTES) {
    if (DataDirection == UfsDataIn) Packet->InTransferLength = UFS_MAX_TRANSFER_BYTES;
    else Packet->OutTransferLength = UFS_MAX_TRANSFER_BYTES;
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = UfsAlignedTransferLength (DataLen, &AlignedLength);
  if (EFI_ERROR (Status)) return Status;
  PrdtNumber = (UINTN)DivU64x32 ((UINT64)AlignedLength + UFS_MAX_DATA_LEN_PER_PRD - 1, UFS_MAX_DATA_LEN_PER_PRD);

  Status = UfsAcquireCommandDescriptor (Private, Trd, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  CommandUpiu = (UTP_COMMAND_UPIU *)*CmdDescHost;

  // Android/Linux use the reserved UTRL slot as the wire task tag.
  // An independent 8-bit counter can collide with an outstanding request
  // after wrap and disagrees with the HCI slot identity.
  UfsInitCommandUpiu (CommandUpiu, Lun, (UINT8)(Trd - (UTP_TRD *)Private->UtpTrlBase),
                     Packet->Cdb, Packet->CdbLength, DataDirection, DataLen);

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Response UPIU and the Physical Region Description Table
  // *MUST* be located at a 64-bit aligned boundary.
  //
  // Overwrite the whole header, including HCI crypto/EHS bits on slot reuse.
  ZeroMem (Trd, sizeof (*Trd));
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  Trd->Dd     = DataDirection;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  Trd->RuL    = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  Trd->RuO    = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  Trd->PrdtL  = (UINT16)PrdtNumber;
  Trd->PrdtO  = 2 * UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  return EFI_SUCCESS;
}

/**
  Prepare QUERY REQUEST/QUERY RESPONSE UPIU in the reserved slot's permanent UCD.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Packet            The pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       Borrowed CPU address; the controller owns its backing.
  @param[out] CmdDescMapping    Always NULL; the request does not own the pool mapping.

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.
  @retval EFI_INVALID_PARAMETER The parameter passed in is invalid.

**/
EFI_STATUS
UfsCreateDMCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN     UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  IN     UTP_TRD                               *Trd,
  OUT VOID                                     **CmdDescHost,
  OUT VOID                                     **CmdDescMapping
  )
{
  UTP_QUERY_REQ_UPIU    *QueryReqUpiu;
  UINT8                 Opcode;
  UINT32                DataSize;
  UINT8                 *Data;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  EFI_STATUS            Status;

  ASSERT ((Private != NULL) && (Packet != NULL) && (Trd != NULL));

  Opcode = Packet->Opcode;
  if ((Opcode > UtpQueryFuncOpcodeTogFlag) || (Opcode == UtpQueryFuncOpcodeNop)) {
    return EFI_INVALID_PARAMETER;
  }

  DataSize = Packet->TransferLength;
  Data     = Packet->DataBuffer;

  if ((Opcode == UtpQueryFuncOpcodeWrDesc) || (Opcode == UtpQueryFuncOpcodeRdDesc)) {
    // Android QUERY_DESC_MAX_SIZE is 255; its data segment fits in one UPIU.
    if ((DataSize == 0) || (DataSize > MAX_UINT8) || (Data == NULL)) {
      return EFI_INVALID_PARAMETER;
    }

  }
  Status = UfsAcquireCommandDescriptor (Private, Trd, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Initialize UTP QUERY REQUEST UPIU
  //
  QueryReqUpiu = (UTP_QUERY_REQ_UPIU *)*CmdDescHost;
  ASSERT (QueryReqUpiu != NULL);
  UfsInitQueryRequestUpiu (
    QueryReqUpiu,
    (UINT8)(Trd - (UTP_TRD *)Private->UtpTrlBase),
    Opcode,
    Packet->DescId,
    Packet->Index,
    Packet->Selector,
    DataSize,
    Data
    );

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Query Response UPIU *MUST* be located at a 64-bit aligned boundary.
  //
  // Overwrite the whole header, including HCI crypto/EHS bits on slot reuse.
  ZeroMem (Trd, sizeof (*Trd));
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  // Management data is carried in the Query UPIU, not through a PRDT.
  // UFSHCI requires no data transfer here (Linux devman uses DMA_NONE).
  Trd->Dd     = UfsNoData;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  Trd->RuL = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  Trd->RuO = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  // Android preserves the fixed PRDT offset even when length is zero.
  Trd->PrdtL = 0;
  Trd->PrdtO = 2 * UFS_UPIU_REGION_SIZE / sizeof (UINT32);

  return EFI_SUCCESS;
}

/**
  Prepare NOP IN and NOP OUT UPIU in the reserved slot's permanent UCD.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Trd               The pointer to the UTP Transfer Request Descriptor.
  @param[out] CmdDescHost       Borrowed CPU address; the controller owns its backing.
  @param[out] CmdDescMapping    Always NULL; the request does not own the pool mapping.

  @retval EFI_SUCCESS           The creation succeed.
  @retval EFI_DEVICE_ERROR      The creation failed.
  @retval EFI_OUT_OF_RESOURCES  The memory resource is insufficient.

**/
EFI_STATUS
UfsCreateNopCommandDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UTP_TRD                     *Trd,
  OUT VOID                           **CmdDescHost,
  OUT VOID                           **CmdDescMapping
  )
{
  UTP_NOP_OUT_UPIU      *NopOutUpiu;
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;

  ASSERT ((Private != NULL) && (Trd != NULL));

  Status = UfsAcquireCommandDescriptor (Private, Trd, CmdDescHost, &CmdDescPhyAddr, CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  NopOutUpiu = (UTP_NOP_OUT_UPIU *)*CmdDescHost;
  ASSERT (NopOutUpiu != NULL);
  NopOutUpiu->TaskTag = (UINT8)(Trd - (UTP_TRD *)Private->UtpTrlBase);

  //
  // Fill UTP_TRD associated fields
  // NOTE: Some UFS host controllers request the Nop Out UPIU *MUST* be located at a 64-bit aligned boundary.
  //
  // Overwrite the whole header, including HCI crypto/EHS bits on slot reuse.
  ZeroMem (Trd, sizeof (*Trd));
  Trd->Int    = UFS_INTERRUPT_COMMAND;
  Trd->Dd     = 0x00;
  Trd->Ct     = UFS_STORAGE_COMMAND_TYPE;
  Trd->Ocs    = UFS_HC_TRD_OCS_INIT_VALUE;
  Trd->UcdBa  = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 7);
  Trd->UcdBaU = (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32);
  Trd->RuL    = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  Trd->RuO    = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  // Android preserves the fixed PRDT offset even when length is zero.
  Trd->PrdtL = 0;
  Trd->PrdtO = 2 * UFS_UPIU_REGION_SIZE / sizeof (UINT32);

  return EFI_SUCCESS;
}

/**
  Find out available slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[out] Slot          The available slot.

  @retval EFI_SUCCESS       The available slot was found successfully.
  @retval EFI_NOT_READY     No slot is available at this moment.

**/
EFI_STATUS
UfsFindAvailableSlotInTrl (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  OUT UINT8                          *Slot
  )
{
  UINT8 Index, Nutrs;
  UINT32 Data;
  EFI_STATUS Status;
  EFI_TPL OldTpl;

  ASSERT ((Private != NULL) && (Slot != NULL));
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Data);
  if (!EFI_ERROR (Status)) {
    Data |= Private->ReservedSlots;
    Nutrs = (UINT8)((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTRS) + 1);
    Status = EFI_NOT_READY;
    for (Index = 0; Index < Nutrs; Index++) {
      if ((Data & (1U << Index)) == 0) {
        Private->ReservedSlots |= 1U << Index;
        *Slot = Index;
        Status = EFI_SUCCESS;
        break;
      }
    }
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

// A cleared doorbell is not sufficient: response processing and mapping
// cleanup still own the TRD until its request has been completely released.
VOID
UfsReleaseSlot (IN UFS_PASS_THRU_PRIVATE_DATA *Private, IN UINT8 Slot)
{
  EFI_TPL OldTpl;
  ASSERT (Slot < 32);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Private->ReservedSlots &= ~(1U << Slot);
  gBS->RestoreTPL (OldTpl);
}

/**
  Start specified slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Slot          The slot to be started.

**/
EFI_STATUS
UfsStartExecCmd (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       Slot
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  Status = UfsCheckControllerErrors (Private);
  if (EFI_ERROR (Status)) return Status;

  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((Data & UFS_HC_UTRLRSR) != UFS_HC_UTRLRSR) {
    Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  Private->LastDoorbellMask = 1U << Slot;
  Private->DoorbellAttempts++;
  if (Private->DoorbellAttempts <= 4) {
    DEBUG ((DEBUG_WARN, "UFS: doorbell attempt=%u slot=%u RSR-before=%08x\n",
            Private->DoorbellAttempts, Slot, Data));
  }
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLDBR_OFFSET, 1U << Slot);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Private->DoorbellWrites++;
  if (Private->DoorbellWrites <= 4) {
    DEBUG ((DEBUG_WARN, "UFS: doorbell write=%u mask=%08x accepted\n",
            Private->DoorbellWrites, Private->LastDoorbellMask));
  }

  return EFI_SUCCESS;
}

/**
  Stop specified slot in transfer list of a UFS device.

  @param[in]  Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Slot          The slot to be stop.

**/
EFI_STATUS
UfsStopExecCmd (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       Slot
  )
{
  UINT32 Data;
  EFI_STATUS Status;
  if (Slot >= 32) return EFI_INVALID_PARAMETER;
  Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Data);
  if (EFI_ERROR (Status)) return Status;
  if ((Data & (1U << Slot)) == 0) return EFI_SUCCESS;
  // UFSHCI: zero clears the selected slot, ones leave other slots alone.
  // Linux ufshcd_clear_cmd writes ~mask and then polls the doorbell.
  Status = UfsMmioRead32 (Private, UFS_HC_UTRLRSR_OFFSET, &Data);
  if (EFI_ERROR (Status)) return Status;
  if ((Data & UFS_HC_UTRLRSR) == 0) return EFI_NOT_READY;
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLCLR_OFFSET, ~(1U << Slot));
  if (EFI_ERROR (Status)) return Status;
  return UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, 1U << Slot, 0, UFS_TIMEOUT);
}

/**
  Extracts return data from query response upiu.

  @param[in] Packet     Pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET.
  @param[in] QueryResp  Pointer to the query response.

  @retval EFI_INVALID_PARAMETER Packet or QueryResp are empty or opcode is invalid.
  @retval EFI_DEVICE_ERROR      Data returned from device is invalid.
  @retval EFI_SUCCESS           Data extracted.

**/
EFI_STATUS
UfsGetReturnDataFromQueryResponse (
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  IN UTP_QUERY_RESP_UPIU                   *QueryResp
  )
{
  UINT16  ReturnDataSize;
  UINT32  ReturnData;

  if ((Packet == NULL) || (QueryResp == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  switch (Packet->Opcode) {
    case UtpQueryFuncOpcodeRdDesc:
      // Linux ufshcd_copy_query_response bounds/copies the actual UPIU data
      // segment, not the query OSF length, which cannot prove payload bytes.
      ReturnDataSize = QueryResp->DataSegLen;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnDataSize, sizeof (UINT16));
      //
      // Make sure the hardware device does not return more data than expected.
      //
      if (ReturnDataSize > Packet->TransferLength) {
        return EFI_DEVICE_ERROR;
      }

      CopyMem (Packet->DataBuffer, (QueryResp + 1), ReturnDataSize);
      Packet->TransferLength = ReturnDataSize;
      break;
    case UtpQueryFuncOpcodeWrDesc:
      ReturnDataSize = QueryResp->Tsf.Length;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnDataSize, sizeof (UINT16));
      Packet->TransferLength = ReturnDataSize;
      break;
    case UtpQueryFuncOpcodeRdFlag:
    case UtpQueryFuncOpcodeSetFlag:
    case UtpQueryFuncOpcodeClrFlag:
    case UtpQueryFuncOpcodeTogFlag:
      //
      // The 'FLAG VALUE' field is at byte offset 3 of QueryResp->Tsf.Value
      //
      *((UINT8 *)(Packet->DataBuffer)) = *((UINT8 *)&(QueryResp->Tsf.Value) + 3);
      break;
    case UtpQueryFuncOpcodeRdAttr:
    case UtpQueryFuncOpcodeWrAttr:
      ReturnData = QueryResp->Tsf.Value;
      SwapLittleEndianToBigEndian ((UINT8 *)&ReturnData, sizeof (UINT32));
      CopyMem (Packet->DataBuffer, &ReturnData, sizeof (UINT32));
      break;
    default:
      return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

/**
  Creates Transfer Request descriptor and sends Query Request to the device.

  @param[in] Private Pointer to the UFS_PASS_THRU_PRIVATE_DATA.
  @param[in] Packet  Pointer to the UFS_DEVICE_MANAGEMENT_REQUEST_PACKET.

  @retval EFI_SUCCESS           The device descriptor was read/written successfully.
  @retval EFI_INVALID_PARAMETER The DescId, Index and Selector fields in Packet are invalid
                                combination to point to a type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the device descriptor.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the device descriptor.

**/
STATIC VOID
UfsDumpQueryFailure (
  IN UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet,
  IN UINT8                               Slot,
  IN UTP_TRD                             *Trd
  )
{
  STATIC CONST UINT32 Offsets[] = {
    UFS_HC_IS_OFFSET, UFS_HC_STATUS_OFFSET, UFS_HC_UTRLDBR_OFFSET,
    UFS_HC_UECPA_OFFSET, UFS_HC_UECDL_OFFSET, UFS_HC_UECN_OFFSET,
    UFS_HC_UECT_OFFSET, UFS_HC_UECDME_OFFSET
  };
  UINTN      Index;
  UINT32     Value;
  EFI_STATUS Status;

  DEBUG ((DEBUG_ERROR, "UFS query stalled: opcode=%x IDN=%x index=%x slot=%u OCS=%x\n",
          Packet->Opcode, Packet->DescId, Packet->Index, Slot, Trd->Ocs));
  // Capture before aborting the slot or disabling HCE changes the evidence.
  for (Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
    Value = 0;
    Status = UfsMmioRead32 (Private, Offsets[Index], &Value);
    DEBUG ((DEBUG_ERROR, "UFS query reg[%02x]=%08x (%r)\n", Offsets[Index], Value, Status));
  }
}

EFI_STATUS
UfsSendDmRequestRetry (
  IN UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet
  )
{
  UINT8                               Slot;
  UTP_TRD                             *Trd;
  VOID                                *CmdDescHost;
  VOID                                *CmdDescMapping;
  UINT32                              CmdDescSize;
  UFS_PASS_THRU_TRANS_REQ             *Owner;
  EFI_TPL                            OldTpl;
  UTP_QUERY_RESP_UPIU                 *QueryResp;
  EFI_STATUS                          Status;

  if (Private->IoUnavailable || Private->CleanupInProgress) return EFI_NOT_READY;
  Owner = AllocateZeroPool (sizeof (*Owner));
  if (Owner == NULL) return EFI_OUT_OF_RESOURCES;
  Owner->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  //
  // Find out which slot of transfer request list is available.
  //
  Status = UfsFindAvailableSlotInTrl (Private, &Slot);
  if (EFI_ERROR (Status)) {
    FreePool (Owner);
    return Status;
  }

  Trd = ((UTP_TRD *)Private->UtpTrlBase) + Slot;
  //
  // Fill transfer request descriptor to this slot.
  //
  Status = UfsCreateDMCommandDesc (Private, Packet, Trd, &CmdDescHost, &CmdDescMapping);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to create DM command descriptor\n"));
    UfsReleaseSlot (Private, Slot);
    FreePool (Owner);
    return Status;
  }

  Owner->OwnsSlot = TRUE;
  Owner->Slot = Slot;
  Owner->CmdDescHost = CmdDescHost;
  Owner->CmdDescMapping = CmdDescMapping;
  Owner->CmdDescFromPool = TRUE;
  QueryResp = (UTP_QUERY_RESP_UPIU *)((UINT8 *)CmdDescHost + Trd->RuO * sizeof (UINT32));
  ASSERT (QueryResp != NULL);
  CmdDescSize = Trd->RuO * sizeof (UINT32) + Trd->RuL * sizeof (UINT32);

  //
  // Start to execute the transfer request.
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Private->IoUnavailable || Private->CleanupInProgress) {
    gBS->RestoreTPL (OldTpl);
    Status = EFI_NOT_READY;
    goto Exit;
  }
  UfsTraceRequest (Private, Trd, CmdDescHost, CmdDescSize, Slot);
  Owner->Submitted = TRUE;
  Status = UfsStartExecCmd (Private, Slot);
  gBS->RestoreTPL (OldTpl);
  if (EFI_ERROR (Status)) goto Exit;

  //
  // Wait for the completion of the transfer request.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, 1U << Slot, 0, Packet->Timeout);
  if (EFI_ERROR (Status)) {
    UfsDumpQueryFailure (Private, Packet, Slot, Trd);
    goto Exit;
  }
  Owner->DmaFenced = TRUE;

  // Linux ufshcd_dev_cmd_completion rejects unexpected transaction types.
  if (QueryResp->TransCode != 0x36 ||
      QueryResp->TaskTag != ((UTP_QUERY_REQ_UPIU *)CmdDescHost)->TaskTag) {
    Status = EFI_DEVICE_ERROR;
    goto Exit;
  }
  if ((Trd->Ocs != 0) || (QueryResp->QueryResp != UfsUtpQueryResponseSuccess)) {
    DEBUG ((DEBUG_ERROR, "Failed to send query request, OCS = %X, QueryResp = %X\n", Trd->Ocs, QueryResp->QueryResp));
    DumpQueryResponseResult (QueryResp->QueryResp);

    if ((QueryResp->QueryResp == UfsUtpQueryResponseInvalidSelector) ||
        (QueryResp->QueryResp == UfsUtpQueryResponseInvalidIndex) ||
        (QueryResp->QueryResp == UfsUtpQueryResponseInvalidIdn))
    {
      Status = EFI_INVALID_PARAMETER;
    } else {
      Status = EFI_DEVICE_ERROR;
    }

    goto Exit;
  }

  Status = UfsGetReturnDataFromQueryResponse (Packet, QueryResp);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Failed to get return data from query response\n"));
    goto Exit;
  }

Exit:
  Owner->CmdDescSize = CmdDescSize;
  Owner->CompletionStatus = Status;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Private->CleanupInProgress = TRUE;
  Status = UfsFinishOwnedRequest (Private, Owner);
  Private->CleanupInProgress = FALSE;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Sends Query Request to the device. Query is sent until device responds correctly or counter runs out.

  @param[in] Private Pointer to the UFS_PASS_THRU_PRIVATE_DATA.
  @param[in] Packet  Pointer to the UFS_DEVICE_MANAGEMENT_PACKET.

  @retval EFI_SUCCESS           The device responded correctly to the Query request.
  @retval EFI_INVALID_PARAMETER The DescId, Index and Selector fields in Packet are invalid
                                combination to point to a type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while waiting for the response from the device.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of the operation.

**/
EFI_STATUS
UfsSendDmRequest (
  IN UFS_PASS_THRU_PRIVATE_DATA            *Private,
  IN UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  *Packet
  )
{
  EFI_STATUS  Status;
  UINT8       Retry;

  Status = EFI_SUCCESS;

  for (Retry = 0; Retry < 5; Retry++) {
    Status = UfsSendDmRequestRetry (Private, Packet);
    if (!EFI_ERROR (Status)) {
      return EFI_SUCCESS;
    }
    if (Private->IoUnavailable || (Status == EFI_INVALID_PARAMETER) ||
        (Status == EFI_OUT_OF_RESOURCES) || (Status == EFI_NOT_READY)) {
      return Status;
    }
  }

  DEBUG ((DEBUG_ERROR, "Failed to get response from the device after %d retries\n", Retry));
  return Status;
}

/**
  Read or write specified device descriptor of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      DescId        The ID of device descriptor.
  @param[in]      Index         The Index of device descriptor.
  @param[in]      Selector      The Selector of device descriptor.
  @param[in, out] Descriptor    The buffer of device descriptor to be read or written.
  @param[in, out] DescSize      The size of device descriptor buffer. On input, the size, in bytes,
                                of the data buffer specified by Descriptor. On output, the number
                                of bytes that were actually transferred.

  @retval EFI_SUCCESS           The device descriptor was read/written successfully.
  @retval EFI_INVALID_PARAMETER DescId, Index and Selector are invalid combination to point to a
                                type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the device descriptor.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the device descriptor.

**/
EFI_STATUS
UfsRwDeviceDesc (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       DescId,
  IN     UINT8                       Index,
  IN     UINT8                       Selector,
  IN OUT VOID                        *Descriptor,
  IN OUT UINT32                      *DescSize
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;
  EFI_STATUS                            Status;

  if (DescSize == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdDesc;
  } else {
    Packet.DataDirection = UfsDataOut;
    Packet.Opcode        = UtpQueryFuncOpcodeWrDesc;
  }

  Packet.DataBuffer     = Descriptor;
  Packet.TransferLength = *DescSize;
  Packet.DescId         = DescId;
  Packet.Index          = Index;
  Packet.Selector       = Selector;
  Packet.Timeout        = UFS_TIMEOUT;

  Status = UfsSendDmRequest (Private, &Packet);
  if (EFI_ERROR (Status)) {
    *DescSize = 0;
  } else {
    *DescSize = Packet.TransferLength;
  }

  return Status;
}

/**
  Read or write specified attribute of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      AttrId        The ID of Attribute.
  @param[in]      Index         The Index of Attribute.
  @param[in]      Selector      The Selector of Attribute.
  @param[in, out] Attributes    The value of Attribute to be read or written.

  @retval EFI_SUCCESS           The Attribute was read/written successfully.
  @retval EFI_INVALID_PARAMETER AttrId, Index and Selector are invalid combination to point to a
                                type of UFS device descriptor.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the Attribute.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the Attribute.

**/
EFI_STATUS
UfsRwAttributes (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       AttrId,
  IN     UINT8                       Index,
  IN     UINT8                       Selector,
  IN OUT UINT32                      *Attributes
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdAttr;
  } else {
    Packet.DataDirection = UfsDataOut;
    Packet.Opcode        = UtpQueryFuncOpcodeWrAttr;
  }

  Packet.DataBuffer = Attributes;
  Packet.DescId     = AttrId;
  Packet.Index      = Index;
  Packet.Selector   = Selector;
  Packet.Timeout    = UFS_TIMEOUT;

  return UfsSendDmRequest (Private, &Packet);
}

/**
  Read or write specified flag of a UFS device.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Read          The boolean variable to show r/w direction.
  @param[in]      FlagId        The ID of flag to be read or written.
  @param[in, out] Value         The value to set or clear flag.

  @retval EFI_SUCCESS           The flag was read/written successfully.
  @retval EFI_INVALID_PARAMETER FlagId is an invalid UFS flag ID.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to r/w the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of r/w the flag.

**/
EFI_STATUS
UfsRwFlags (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     BOOLEAN                     Read,
  IN     UINT8                       FlagId,
  IN OUT UINT8                       *Value
  )
{
  UFS_DEVICE_MANAGEMENT_REQUEST_PACKET  Packet;

  if (Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ZeroMem (&Packet, sizeof (UFS_DEVICE_MANAGEMENT_REQUEST_PACKET));

  if (Read) {
    ASSERT (Value != NULL);
    Packet.DataDirection = UfsDataIn;
    Packet.Opcode        = UtpQueryFuncOpcodeRdFlag;
  } else {
    Packet.DataDirection = UfsDataOut;
    if (*Value == 1) {
      Packet.Opcode = UtpQueryFuncOpcodeSetFlag;
    } else if (*Value == 0) {
      Packet.Opcode = UtpQueryFuncOpcodeClrFlag;
    } else {
      return EFI_INVALID_PARAMETER;
    }
  }

  Packet.DataBuffer = Value;
  Packet.DescId     = FlagId;
  Packet.Index      = 0;
  Packet.Selector   = 0;
  Packet.Timeout    = UFS_TIMEOUT;

  return UfsSendDmRequest (Private, &Packet);
}

/**
  Set specified flag to 1 on a UFS device.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  FlagId            The ID of flag to be set.

  @retval EFI_SUCCESS           The flag was set successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to set the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of setting the flag.

**/
EFI_STATUS
UfsSetFlag (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN  UINT8                       FlagId
  )
{
  EFI_STATUS  Status;
  UINT8       Value;

  Value  = 1;
  Status = UfsRwFlags (Private, FALSE, FlagId, &Value);

  return Status;
}

/**
  Read specified flag from a UFS device.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  FlagId            The ID of flag to be read.
  @param[out] Value             The flag's value.

  @retval EFI_SUCCESS           The flag was read successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to read the flag.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the completion of reading the flag.

**/
EFI_STATUS
UfsReadFlag (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINT8                       FlagId,
  OUT UINT8                          *Value
  )
{
  EFI_STATUS  Status;

  Status = UfsRwFlags (Private, TRUE, FlagId, Value);

  return Status;
}

/**
  Sends NOP IN cmd to a UFS device for initialization process request.
  For more details, please refer to UFS 2.0 spec Figure 13.3.

  @param[in]  Private           The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS           The NOP IN command was sent by the host. The NOP OUT response was
                                received successfully.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to execute NOP IN command.
  @retval EFI_OUT_OF_RESOURCES  The resource for transfer is not available.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the NOP IN command to execute.

**/
EFI_STATUS
UfsExecNopCmds (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS                          Status;
  UINT8                               Slot;
  UTP_TRD                             *Trd;
  UTP_NOP_IN_UPIU                     *NopInUpiu;
  UINT32                              CmdDescSize;
  VOID                                *CmdDescHost;
  VOID                                *CmdDescMapping;
  UFS_PASS_THRU_TRANS_REQ             *Owner;
  EFI_TPL                            OldTpl;

  if (Private->IoUnavailable || Private->CleanupInProgress) return EFI_NOT_READY;
  Owner = AllocateZeroPool (sizeof (*Owner));
  if (Owner == NULL) return EFI_OUT_OF_RESOURCES;
  Owner->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  //
  // Find out which slot of transfer request list is available.
  //
  Status = UfsFindAvailableSlotInTrl (Private, &Slot);
  if (EFI_ERROR (Status)) {
    FreePool (Owner);
    return Status;
  }

  Trd    = ((UTP_TRD *)Private->UtpTrlBase) + Slot;
  Status = UfsCreateNopCommandDesc (Private, Trd, &CmdDescHost, &CmdDescMapping);
  if (EFI_ERROR (Status)) {
    UfsReleaseSlot (Private, Slot);
    FreePool (Owner);
    return Status;
  }

  //
  // Check the transfer request result.
  //
  Owner->OwnsSlot = TRUE;
  Owner->Slot = Slot;
  Owner->CmdDescHost = CmdDescHost;
  Owner->CmdDescMapping = CmdDescMapping;
  Owner->CmdDescFromPool = TRUE;
  NopInUpiu = (UTP_NOP_IN_UPIU *)((UINT8 *)CmdDescHost + Trd->RuO * sizeof (UINT32));
  ASSERT (NopInUpiu != NULL);
  CmdDescSize = Trd->RuO * sizeof (UINT32) + Trd->RuL * sizeof (UINT32);

  //
  // Start to execute the transfer request.
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Private->IoUnavailable || Private->CleanupInProgress) {
    gBS->RestoreTPL (OldTpl);
    Status = EFI_NOT_READY;
    goto Exit;
  }
  UfsTraceRequest (Private, Trd, CmdDescHost, CmdDescSize, Slot);
  Owner->Submitted = TRUE;
  Status = UfsStartExecCmd (Private, Slot);
  gBS->RestoreTPL (OldTpl);
  if (EFI_ERROR (Status)) goto Exit;

  //
  // Wait for the completion of the transfer request.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, 1U << Slot, 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    goto Exit;
  }
  Owner->DmaFenced = TRUE;

  // NOP readiness requires a NOP IN, not just a successful descriptor OCS.
  if (Trd->Ocs != 0 || NopInUpiu->TransCode != 0x20 || NopInUpiu->Resp != 0 ||
      NopInUpiu->TaskTag != ((UTP_NOP_OUT_UPIU *)CmdDescHost)->TaskTag) {
    Status = EFI_DEVICE_ERROR;
  } else {
    Status = EFI_SUCCESS;
  }

Exit:
  Owner->CmdDescSize = CmdDescSize;
  Owner->CompletionStatus = Status;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Private->CleanupInProgress = TRUE;
  Status = UfsFinishOwnedRequest (Private, Owner);
  Private->CleanupInProgress = FALSE;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Prepare data buffer for transfer.

  @param[in]      Private   Pointer to the UFS_PASS_THRU_PRIVATE_DATA
  @param[in, out] TransReq  Pointer to the transfer request

  @retval EFI_DEVICE_ERROR  Failed to prepare buffer for transfer
  @retval EFI_SUCCESS       Buffer ready for transfer
**/
EFI_STATUS
UfsPrepareDataTransferBuffer (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN OUT UFS_PASS_THRU_TRANS_REQ     *TransReq
  )
{
  EFI_STATUS Status;
  UINT32 Length, Aligned;
  UINTN MapLength;
  EFI_PHYSICAL_ADDRESS Device;
  VOID *Caller;
  BOOLEAN Read = TransReq->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ;
  UTP_TR_PRD *Prdt;
  Length = Read ? TransReq->Packet->InTransferLength : TransReq->Packet->OutTransferLength;
  Caller = Read ? TransReq->Packet->InDataBuffer : TransReq->Packet->OutDataBuffer;
  Status = UfsAlignedTransferLength (Length, &Aligned);
  if (EFI_ERROR (Status)) return Status;
  if (Length == 0) return EFI_SUCCESS;
  if (Caller == NULL) return EFI_INVALID_PARAMETER;
  // Always stage, including aligned caller buffers: deferred provider Unmap
  // may copy back later, after a synchronous caller has returned/freed memory.
  TransReq->AlignedDataBufSize = Aligned;
  Status = Private->UfsHostController->AllocateBuffer (Private->UfsHostController,
             AllocateAnyPages, EfiBootServicesData, EFI_SIZE_TO_PAGES (Aligned),
             &TransReq->AlignedDataBuf, 0);
  if (EFI_ERROR (Status)) return Status;
  ZeroMem (TransReq->AlignedDataBuf, Aligned);
  if (!Read) CopyMem (TransReq->AlignedDataBuf, Caller, Length);
  MapLength = Aligned;
  Status = Private->UfsHostController->Map (Private->UfsHostController,
             Read ? EdkiiUfsHcOperationBusMasterWrite : EdkiiUfsHcOperationBusMasterRead,
             TransReq->AlignedDataBuf, &MapLength, &Device, &TransReq->DataBufMapping);
  if (EFI_ERROR (Status)) return Status;
  if (MapLength != Aligned) return EFI_BAD_BUFFER_SIZE;
  Prdt = (UTP_TR_PRD *)((UINT8 *)TransReq->CmdDescHost +
          TransReq->Trd->PrdtO * sizeof (UINT32));
  UfsInitUtpPrdt (Prdt, (VOID *)(UINTN)Device, Aligned);
  return EFI_SUCCESS;
}

/**
  Sends a UFS-supported SCSI Request Packet to a UFS device that is attached to the UFS host controller.

  @param[in]      Private       The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]      Lun           The LUN of the UFS device to send the SCSI Request Packet.
  @param[in, out] Packet        A pointer to the SCSI Request Packet to send to a specified Lun of the
                                UFS device.
  @param[in]      Event         If nonblocking I/O is not supported then Event is ignored, and blocking
                                I/O is performed. If Event is NULL, then blocking I/O is performed. If
                                Event is not NULL and non blocking I/O is supported, then
                                nonblocking I/O is performed, and Event will be signaled when the
                                SCSI Request Packet completes.

  @retval EFI_SUCCESS           The SCSI Request Packet was sent by the host. For bi-directional
                                commands, InTransferLength bytes were transferred from
                                InDataBuffer. For write and bi-directional commands,
                                OutTransferLength bytes were transferred by
                                OutDataBuffer.
  @retval EFI_DEVICE_ERROR      A device error occurred while attempting to send the SCSI Request
                                Packet.
  @retval EFI_OUT_OF_RESOURCES  The resource for transfer is not available.
  @retval EFI_TIMEOUT           A timeout occurred while waiting for the SCSI Request Packet to execute.

**/
STATIC VOID UfsQueueOwnedRequest (UFS_PASS_THRU_PRIVATE_DATA *Private,
                                  UFS_PASS_THRU_TRANS_REQ *Request)
{
  if (!Request->OnQueue) {
    InsertTailList (&Private->Queue, &Request->TransferList);
    Request->OnQueue = TRUE;
  }
}

STATIC EFI_STATUS UfsFenceRequest (UFS_PASS_THRU_PRIVATE_DATA *Private,
                                   UFS_PASS_THRU_TRANS_REQ *Request)
{
  EFI_STATUS Status;
  LIST_ENTRY *Entry;
  if (!Request->Submitted || Request->DmaFenced) return EFI_SUCCESS;
  if (Private->FenceFailed) return EFI_NOT_READY;
  // Linux fatal recovery skips per-slot clears and resets the whole HCI.
  // A successful UTRLCLR cannot restore a controller with a sticky SBFE.
  if (Private->FatalError == 0) {
    Status = UfsStopExecCmd (Private, Request->Slot);
    if (!EFI_ERROR (Status)) { Request->DmaFenced = TRUE; return EFI_SUCCESS; }
  }
  Private->IoUnavailable = TRUE;
  Status = UfsControllerStop (Private);
  if (EFI_ERROR (Status)) {
    // A failed controller fence requires explicit Stop recovery, not a
    // multi-second retry for every request on the 1ms async timer.
    Private->FenceFailed = TRUE;
    return Status;
  }
  Request->DmaFenced = TRUE;
  BASE_LIST_FOR_EACH (Entry, &Private->Queue) {
    UFS_PASS_THRU_TRANS_REQ *Other = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
    Other->DmaFenced = TRUE;
    Other->CompletionStatus = EFI_DEVICE_ERROR;
    Other->ResponseValidated = FALSE;
  }
  return EFI_SUCCESS;
}

// Caller invokes with TPL_NOTIFY and CleanupInProgress set across callbacks.
STATIC EFI_STATUS UfsFinishOwnedRequest (UFS_PASS_THRU_PRIVATE_DATA *Private,
                                        UFS_PASS_THRU_TRANS_REQ *Request)
{
  EFI_STATUS Status, Result = Request->CompletionStatus;
  EFI_EVENT Event = Request->CallerEvent;
  Status = UfsFenceRequest (Private, Request);
  if (!EFI_ERROR (Status))
    Status = UfsReleaseRequestResources (Private, Request, !EFI_ERROR (Result));
  if (EFI_ERROR (Status) && !EFI_ERROR (Result)) Result = Status;
  if (Request->Packet != NULL && EFI_ERROR (Result)) {
    Request->Packet->HostAdapterStatus = Result == EFI_TIMEOUT ?
      EFI_EXT_SCSI_STATUS_HOST_ADAPTER_TIMEOUT_COMMAND : EFI_EXT_SCSI_STATUS_HOST_ADAPTER_PHASE_ERROR;
    if (Request->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_READ) {
      Request->Packet->InTransferLength = 0;
    } else if (Request->Packet->DataDirection == EFI_EXT_SCSI_DATA_DIRECTION_WRITE) {
      Request->Packet->OutTransferLength = 0;
    } else {
      Request->Packet->InTransferLength = 0;
      Request->Packet->OutTransferLength = 0;
    }
  }
  // From here on even a delayed SMMU copyback can touch only owned staging.
  Request->Packet = NULL;
  Request->CallerEvent = NULL;
  Request->CompletionStatus = Result;
  if (Event != NULL) {
    EFI_STATUS EventStatus = gBS->SignalEvent (Event);
    if (EFI_ERROR (EventStatus)) {
      Request->CallerEvent = Event;
      UfsQueueOwnedRequest (Private, Request);
      return EFI_ERROR (Result) ? Result : EventStatus;
    }
  }
  if (EFI_ERROR (Status)) {
    UfsQueueOwnedRequest (Private, Request);
  } else {
    if (Request->OnQueue) RemoveEntryList (&Request->TransferList);
    if (Request->OwnsSlot) UfsReleaseSlot (Private, Request->Slot);
    FreePool (Request);
  }
  return Result;
}

EFI_STATUS
UfsExecScsiCmds (
  IN     UFS_PASS_THRU_PRIVATE_DATA                  *Private,
  IN     UINT8                                       Lun,
  IN OUT EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET  *Packet,
  IN     EFI_EVENT                                   Event    OPTIONAL
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  UFS_PASS_THRU_TRANS_REQ *Request;
  if (Private->IoUnavailable || Private->CleanupInProgress) return EFI_NOT_READY;
  Request = AllocateZeroPool (sizeof (*Request));
  if (Request == NULL) return EFI_OUT_OF_RESOURCES;
  Request->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  Request->Packet = Packet;
  Request->TimeoutRemain = Packet->Timeout;
  Status = UfsFindAvailableSlotInTrl (Private, &Request->Slot);
  if (EFI_ERROR (Status)) { FreePool (Request); return Status; }
  Request->OwnsSlot = TRUE;
  Request->Trd = (UTP_TRD *)Private->UtpTrlBase + Request->Slot;
  Status = UfsCreateScsiCommandDesc (Private, Lun, Packet, Request->Trd,
                                    &Request->CmdDescHost, &Request->CmdDescMapping);
  if (EFI_ERROR (Status)) goto Finish;
  Request->CmdDescFromPool = TRUE;
  Request->CmdDescSize = Request->Trd->PrdtO * sizeof (UINT32) + Request->Trd->PrdtL * sizeof (UTP_TR_PRD);
  Status = UfsPrepareDataTransferBuffer (Private, Request);
  if (EFI_ERROR (Status)) goto Finish;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (Private->IoUnavailable || Private->CleanupInProgress) {
    gBS->RestoreTPL (OldTpl);
    Status = EFI_NOT_READY;
    goto Finish;
  }
  // A failed MMIO return does not prove the doorbell write never reached HW.
  UfsTraceRequest (Private, Request->Trd, Request->CmdDescHost, Request->CmdDescSize, Request->Slot);
  Request->Submitted = TRUE;
  Status = UfsStartExecCmd (Private, Request->Slot);
  if (!EFI_ERROR (Status) && Event != NULL) {
    Request->CallerEvent = Event;
    UfsQueueOwnedRequest (Private, Request);
  }
  gBS->RestoreTPL (OldTpl);
  if (EFI_ERROR (Status)) goto Finish;
  if (Event != NULL) return EFI_SUCCESS;
  Status = UfsWaitMemSet (Private, UFS_HC_UTRLDBR_OFFSET, 1U << Request->Slot, 0, Packet->Timeout);
  if (!EFI_ERROR (Status)) {
    Request->DmaFenced = TRUE;
    Status = UfsValidateScsiResponse (Private, Request);
    Request->ResponseValidated = !EFI_ERROR (Status);
  }
Finish:
  Request->CompletionStatus = Status;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Private->CleanupInProgress = TRUE;
  Status = UfsFinishOwnedRequest (Private, Request);
  Private->CleanupInProgress = FALSE;
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Send UIC command.

  @param[in]      Private     The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in, out] UicCommand  UIC command descriptor. On exit contains UIC command results.

  @return EFI_SUCCESS      Successfully execute this UIC command and detect attached UFS device.
  @return EFI_DEVICE_ERROR Fail to execute this UIC command and detect attached UFS device.

**/
EFI_STATUS
UfsExecUicCommands (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN OUT EDKII_UIC_COMMAND        *UicCommand
  )
{
  EFI_STATUS  Status;
  UINT32      Data;
  UINT32      ClearMask;
  BOOLEAN     PowerModeChange;

  // Linux ufshcd_uic_change_pwr_mode()/ufshcd_uic_pwr_ctrl(): a DME_SET
  // PA_PWRMODE command has a separate asynchronous UPMS completion. UCCS
  // alone only reports command acceptance, not a usable negotiated link.
  PowerModeChange = (UicCommand->Opcode == UfsUicDmeSet) &&
                    ((UicCommand->Arg1 >> 16) == 0x1571);

  Status = UfsMmioRead32 (Private, UFS_HC_IS_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ClearMask = Data & (UFS_HC_IS_UCCS | (PowerModeChange ? BIT4 : 0));
  if (ClearMask != 0) {
    //
    // Clear only stale events owned by this command; preserve other W1C bits.
    //
    Status = UfsMmioWrite32 (Private, UFS_HC_IS_OFFSET, ClearMask);
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  // Do not overwrite arguments while the controller is still processing a
  // prior UIC command. Linux ufshcd checks UCRDY before dispatch as well.
  Status = UfsWaitMemSet (Private, UFS_HC_STATUS_OFFSET, UFS_HC_HCS_UCRDY, UFS_HC_HCS_UCRDY, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // When programming UIC command registers, host software shall set the register UICCMD
  // only after all the UIC command argument registers (UICCMDARG1, UICCMDARG2 and UICCMDARG3)
  // are set.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG1_OFFSET, UicCommand->Arg1);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG2_OFFSET, UicCommand->Arg2);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UCMD_ARG3_OFFSET, UicCommand->Arg3);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UIC_CMD_OFFSET, UicCommand->Opcode);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // UFS 2.0 spec section 5.3.1 Offset:0x20 IS.Bit10 UIC Command Completion Status (UCCS)
  // This bit is set to '1' by the host controller upon completion of a UIC command.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_IS_OFFSET, UFS_HC_IS_UCCS, UFS_HC_IS_UCCS, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (UicCommand->Opcode != UfsUicDmeReset) {
    Status = UfsMmioRead32 (Private, UFS_HC_UCMD_ARG2_OFFSET, &UicCommand->Arg2);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = UfsMmioRead32 (Private, UFS_HC_UCMD_ARG3_OFFSET, &UicCommand->Arg3);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((UicCommand->Arg2 & 0xFF) != 0) {
      DEBUG_CODE_BEGIN ();
      DumpUicCmdExecResult ((UINT8)UicCommand->Opcode, (UINT8)(UicCommand->Arg2 & 0xFF));
      DEBUG_CODE_END ();
      return EFI_DEVICE_ERROR;
    }
  }

  if (PowerModeChange) {
    Status = UfsWaitMemSet (Private, UFS_HC_IS_OFFSET, BIT4, BIT4, UFS_TIMEOUT);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    Status = UfsMmioRead32 (Private, UFS_HC_STATUS_OFFSET, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // HCS.UPMCRS[10:8] must report PWR_LOCAL (1), not PWR_OK (0).
    Status = UfsMmioWrite32 (Private, UFS_HC_IS_OFFSET, BIT4);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (((Data >> 8) & 7) != 1) {
      return EFI_DEVICE_ERROR;
    }
  }

  return EFI_SUCCESS;
}

/**
  Allocate common buffer for host and UFS bus master access simultaneously.

  @param[in]  Private                The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.
  @param[in]  Size                   The length of buffer to be allocated.
  @param[out] CmdDescHost            A pointer to store the base system memory address of the allocated range.
  @param[out] CmdDescPhyAddr         The resulting map address for the UFS bus master to use to access the hosts CmdDescHost.
  @param[out] CmdDescMapping         A resulting value to pass to Unmap().

  @retval EFI_SUCCESS                The common buffer was allocated successfully.
  @retval EFI_DEVICE_ERROR           The allocation fails.
  @retval EFI_OUT_OF_RESOURCES       The memory resource is insufficient.

**/
EFI_STATUS
UfsAllocateAlignCommonBuffer (
  IN     UFS_PASS_THRU_PRIVATE_DATA  *Private,
  IN     UINTN                       Size,
  OUT VOID                           **CmdDescHost,
  OUT EFI_PHYSICAL_ADDRESS           *CmdDescPhyAddr,
  OUT VOID                           **CmdDescMapping
  )
{
  EFI_STATUS Status, Cleanup;
  EFI_TPL OldTpl;
  UINTN Bytes;
  UFS_PASS_THRU_TRANS_REQ *Owner;
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL *Host = Private->UfsHostController;
  *CmdDescHost = NULL; *CmdDescMapping = NULL; *CmdDescPhyAddr = 0;
  if (Size == 0 || Size > MAX_UINT32 || Size > MAX_UINTN - EFI_PAGE_MASK) return EFI_BAD_BUFFER_SIZE;
  Owner = AllocateZeroPool (sizeof (*Owner));
  if (Owner == NULL) return EFI_OUT_OF_RESOURCES;
  Owner->Signature = UFS_PASS_THRU_TRANS_REQ_SIG;
  Owner->CmdDescSize = (UINT32)Size;
  Status = Host->AllocateBuffer (Host, AllocateAnyPages, EfiBootServicesData,
                                 EFI_SIZE_TO_PAGES (Size), &Owner->CmdDescHost, 0);
  if (EFI_ERROR (Status)) { FreePool (Owner); return Status; }
  Bytes = EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (Size));
  Status = Host->Map (Host, EdkiiUfsHcOperationBusMasterCommonBuffer,
                      Owner->CmdDescHost, &Bytes, CmdDescPhyAddr, &Owner->CmdDescMapping);
  if (!EFI_ERROR (Status) && Bytes != EFI_PAGES_TO_SIZE (EFI_SIZE_TO_PAGES (Size)))
    Status = EFI_BAD_BUFFER_SIZE;
  if (!EFI_ERROR (Status) && !(Private->UfsHcInfo.Capabilities & UFS_HC_CAP_64ADDR) &&
      (*CmdDescPhyAddr > MAX_UINT32 || Bytes - 1 > MAX_UINT32 - *CmdDescPhyAddr))
    Status = EFI_DEVICE_ERROR;
  if (EFI_ERROR (Status)) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Cleanup = UfsReleaseRequestResources (Private, Owner, FALSE);
    if (EFI_ERROR (Cleanup)) UfsQueueOwnedRequest (Private, Owner);
    else FreePool (Owner);
    gBS->RestoreTPL (OldTpl);
    *CmdDescPhyAddr = 0;
    return Status;
  }
  ZeroMem (Owner->CmdDescHost, Bytes);
  *CmdDescHost = Owner->CmdDescHost;
  *CmdDescMapping = Owner->CmdDescMapping;
  FreePool (Owner);
  return EFI_SUCCESS;
}

/**
  Enable the UFS host controller for accessing.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS host controller enabling was executed successfully.
  @retval EFI_DEVICE_ERROR           A device error occurred while enabling the UFS host controller.

**/
EFI_STATUS
UfsEnableHostController (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINT32      Data;

  if ((mUfsHcPlatform == NULL) ||
      ((mUfsHcPlatform->Version >= 3) && !mUfsHcPlatform->SkipHceReenable))
  {
    //
    // UFS 2.0 spec section 7.1.1 - Host Controller Initialization
    //
    // Reinitialize the UFS host controller if HCE bit of HC register is set.
    //
    Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Data);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if ((Data & UFS_HC_HCE_EN) == UFS_HC_HCE_EN) {
      //
      // Write a 0 to the HCE register at first to disable the host controller.
      //
      Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, 0);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      //
      // Wait until HCE is read as '0' before continuing.
      //
      Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, 0, UFS_TIMEOUT);
      if (EFI_ERROR (Status)) {
        return EFI_DEVICE_ERROR;
      }
    }
  }

  // Linux ufshcd_hba_execute_hce stops the controller before PRE_CHANGE.
  // Platform callbacks may power down and recalibrate the PHY on retries.
  if ((mUfsHcPlatform != NULL) && (mUfsHcPlatform->Callback != NULL)) {
    Status = mUfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPreHce, &Private->UfsHcDriverInterface);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPreHce, Status = %r\n", Status));
      return Status;
    }
  }

  if ((mUfsHcPlatform == NULL) ||
      ((mUfsHcPlatform->Version >= 3) && !mUfsHcPlatform->SkipHceReenable))
  {
    //
    // Write a 1 to the HCE register to enable the UFS host controller.
    //
    Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    // HCE transitions 1 -> 0 -> 1 during initialization. As in Linux,
    // delay before polling so the initial write is not mistaken for ready.
    MicroSecondDelay (1000);
  }

  //
  // Wait until HCE is read as '1' before continuing.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, UFS_HC_HCE_EN, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  if ((mUfsHcPlatform != NULL) && (mUfsHcPlatform->Callback != NULL)) {
    Status = mUfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPostHce, &Private->UfsHcDriverInterface);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPostHce, Status = %r\n", Status));
      return Status;
    }
  }

  return EFI_SUCCESS;
}

/**
  Detect if a UFS device attached.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS device detection was executed successfully.
  @retval EFI_NOT_FOUND              Not found a UFS device attached.
  @retval EFI_DEVICE_ERROR           A device error occurred while detecting the UFS device.

**/
EFI_STATUS
UfsDeviceDetection (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINTN              Retry;
  EFI_STATUS         Status;
  UINT32             Data;
  EDKII_UIC_COMMAND  LinkStartupCommand;

  //
  // Retry link startup after resetting the local UniPro through HCE, as in
  // Linux ufshcd_link_startup(). Platform link setup must run after each reset.
  //
  for (Retry = 0; Retry < 3; Retry++) {
    if ((mUfsHcPlatform != NULL) && (mUfsHcPlatform->Callback != NULL)) {
      Status = mUfsHcPlatform->Callback (Private->Handle, EdkiiUfsHcPreLinkStartup, &Private->UfsHcDriverInterface);
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_ERROR, "Failure from platform driver during EdkiiUfsHcPreLinkStartup, Status = %r\n", Status));
        return Status;
      }
    }

    if ((mUfsHcPlatform != NULL) && (mUfsHcPlatform->Version >= 3) && mUfsHcPlatform->SkipLinkStartup) {
      return EFI_SUCCESS;
    }

    LinkStartupCommand.Opcode = UfsUicDmeLinkStartup;
    LinkStartupCommand.Arg1   = 0;
    LinkStartupCommand.Arg2   = 0;
    LinkStartupCommand.Arg3   = 0;
    Status                    = UfsExecUicCommands (Private, &LinkStartupCommand);
    if (EFI_ERROR (Status)) {
      if (Retry == 2) {
        return EFI_DEVICE_ERROR;
      }

      Status = UfsEnableHostController (Private);
      if (EFI_ERROR (Status)) {
        return Status;
      }

      continue;
    }

    Status = UfsMmioRead32 (Private, UFS_HC_STATUS_OFFSET, &Data);
    if (EFI_ERROR (Status)) {
      return EFI_DEVICE_ERROR;
    }

    // A successful LINKSTARTUP without HCS.DP means the device is absent.
    // ULSS reports link loss and need not arrive for an absent device.
    if ((Data & UFS_HC_HCS_DP) == 0) {
      return EFI_NOT_FOUND;
    }

    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

/**
  Initialize UFS task management request list related h/w context.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS task management list was initialzed successfully.
  @retval EFI_DEVICE_ERROR           The initialization fails.

**/
EFI_STATUS
UfsInitTaskManagementRequestList (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT8                 Nutmrs;
  VOID                  *CmdDescHost;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  VOID                  *CmdDescMapping;
  EFI_STATUS            Status;

  //
  // Initial h/w and s/w context for future operations.
  //
  CmdDescHost    = NULL;
  CmdDescMapping = NULL;
  CmdDescPhyAddr = 0;

  //
  // Allocate and initialize UTP Task Management Request List.
  //
  Nutmrs = (UINT8)(RShiftU64 ((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTMRS), 16) + 1);
  Status = UfsAllocateAlignCommonBuffer (Private, Nutmrs * sizeof (UTP_TMRD), &CmdDescHost, &CmdDescPhyAddr, &CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UtpTmrlBase = CmdDescHost;
  Private->Nutmrs      = Nutmrs;
  Private->TmrlMapping = CmdDescMapping;

  //
  // Program the UTP Task Management Request List Base Address and UTP Task Management
  // Request List Base Address with a 64-bit address allocated at step 6.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLBA_OFFSET, (UINT32)(UINTN)CmdDescPhyAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLBAU_OFFSET, (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32));
  if (EFI_ERROR (Status)) {
    return Status;
  }


  //
  // Enable the UTP Task Management Request List by setting the UTP Task Management
  // Request List RunStop Register (UTMRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLRSR_OFFSET, UFS_HC_UTMRLRSR);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Initialize UFS transfer request list related h/w context.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The UFS transfer list was initialzed successfully.
  @retval EFI_DEVICE_ERROR           The initialization fails.

**/
EFI_STATUS
UfsInitTransferRequestList (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT8                 Nutrs;
  VOID                  *CmdDescHost;
  EFI_PHYSICAL_ADDRESS  CmdDescPhyAddr;
  VOID                  *CmdDescMapping;
  EFI_STATUS            Status;
  EFI_PHYSICAL_ADDRESS  UcdAddress;
  UTP_TRD               *Trd;
  UINTN                 Index;

  //
  // Initial h/w and s/w context for future operations.
  //
  CmdDescHost    = NULL;
  CmdDescMapping = NULL;
  CmdDescPhyAddr = 0;

  //
  // Allocate and initialize UTP Transfer Request List.
  //
  if (Private->UtpUcdBase != NULL || Private->UtpTrlBase != NULL) return EFI_ALREADY_STARTED;
  Nutrs  = (UINT8)((Private->UfsHcInfo.Capabilities & UFS_HC_CAP_NUTRS) + 1);
  Private->Nutrs = Nutrs;
  // Match public Android ufshcd_memory_alloc/host_memory_configure: allocate
  // all UCD slots once and initialize every UTRD before setting RunStop.
  Status = UfsAllocateAlignCommonBuffer (Private, Nutrs * UFS_UCD_SLOT_SIZE,
                                       &Private->UtpUcdBase, &Private->UcdDeviceAddress,
                                       &Private->UcdMapping);
  if (EFI_ERROR (Status)) return Status;
  Private->UcdPoolSize = Nutrs * UFS_UCD_SLOT_SIZE;
  Status = UfsAllocateAlignCommonBuffer (Private, Nutrs * sizeof (UTP_TRD), &CmdDescHost, &CmdDescPhyAddr, &CmdDescMapping);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UtpTrlBase = CmdDescHost;
  Private->Nutrs      = Nutrs;
  Private->TrlMapping = CmdDescMapping;
  for (Index = 0; Index < Nutrs; Index++) {
    Trd = (UTP_TRD *)Private->UtpTrlBase + Index;
    UcdAddress = Private->UcdDeviceAddress + Index * UFS_UCD_SLOT_SIZE;
    Trd->UcdBa = (UINT32)RShiftU64 (UcdAddress, 7);
    Trd->UcdBaU = (UINT32)RShiftU64 (UcdAddress, 32);
    Trd->RuO = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
    Trd->RuL = UFS_UPIU_REGION_SIZE / sizeof (UINT32);
    Trd->PrdtO = 2 * UFS_UPIU_REGION_SIZE / sizeof (UINT32);
  }
  DEBUG ((DEBUG_WARN, "UFS: UCD pool host=%p IOVA=%Lx bytes=%u slots=%u stride=%u PRDs=%u\n",
          Private->UtpUcdBase, Private->UcdDeviceAddress, (UINT32)Private->UcdPoolSize,
          Nutrs, (UINT32)UFS_UCD_SLOT_SIZE, UFS_MAX_PRDT_ENTRIES));

  //
  // Program the UTP Transfer Request List Base Address and UTP Transfer Request List
  // Base Address with a 64-bit address allocated at step 8.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLBA_OFFSET, (UINT32)(UINTN)CmdDescPhyAddr);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLBAU_OFFSET, (UINT32)RShiftU64 ((UINT64)CmdDescPhyAddr, 32));
  if (EFI_ERROR (Status)) {
    return Status;
  }


  //
  // Enable the UTP Transfer Request List by setting the UTP Transfer Request List
  // RunStop Register (UTRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, UFS_HC_UTRLRSR);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}

/**
  Initialize the UFS host controller.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The Ufs Host Controller is initialized successfully.
  @retval Others                     A device error occurred while initializing the controller.

**/
EFI_STATUS
UfsControllerInit (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;

  Status = UfsEnableHostController (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Enable Host Controller Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsDeviceDetection (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Device Detection Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsInitTaskManagementRequestList (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Task management list initialization Fails, Status = %r\n", Status));
    return Status;
  }

  Status = UfsInitTransferRequestList (Private);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "UfsControllerInit: Transfer list initialization Fails, Status = %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "UfsControllerInit Finished\n"));
  return EFI_SUCCESS;
}

/**
  Stop the UFS host controller.

  @param[in] Private                 The pointer to the UFS_PASS_THRU_PRIVATE_DATA data structure.

  @retval EFI_SUCCESS                The Ufs Host Controller is stopped successfully.
  @retval Others                     A device error occurred while stopping the controller.

**/
EFI_STATUS
UfsControllerStop (
  IN  UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  EFI_STATUS  Status;
  UINT32      Data;

  //
  // Enable the UTP Task Management Request List by setting the UTP Task Management
  // Request List RunStop Register (UTMRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTMRLRSR_OFFSET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Enable the UTP Transfer Request List by setting the UTP Transfer Request List
  // RunStop Register (UTRLRSR) to '1'.
  //
  Status = UfsMmioWrite32 (Private, UFS_HC_UTRLRSR_OFFSET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Write a 0 to the HCE register in order to disable the host controller.
  //
  Status = UfsMmioRead32 (Private, UFS_HC_ENABLE_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Partial initialization and failed cleanup retries may already have HCE clear.

  Status = UfsMmioWrite32 (Private, UFS_HC_ENABLE_OFFSET, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Wait until HCE is read as '0' before continuing.
  //
  Status = UfsWaitMemSet (Private, UFS_HC_ENABLE_OFFSET, UFS_HC_HCE_EN, 0, UFS_TIMEOUT);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((DEBUG_INFO, "UfsController is stopped\n"));

  return EFI_SUCCESS;
}

/**
  Internal helper function which will signal the caller event and clean up
  resources.

  @param[in] Private   The pointer to the UFS_PASS_THRU_PRIVATE_DATA data
                       structure.
  @param[in] TransReq  The pointer to the UFS_PASS_THRU_TRANS_REQ data
                       structure.

**/
VOID EFIAPI SignalCallerEvent (UFS_PASS_THRU_PRIVATE_DATA *Private,
                               UFS_PASS_THRU_TRANS_REQ *TransReq)
{
  UfsFinishOwnedRequest (Private, TransReq);
}

/**
  Call back function when the timer event is signaled.

  @param[in]  Event     The Event this notify function registered to.
  @param[in]  Context   Pointer to the context data registered to the Event.

**/
VOID
EFIAPI
ProcessAsyncTaskList (EFI_EVENT Event, VOID *Context)
{
  UFS_PASS_THRU_PRIVATE_DATA *Private = Context;
  LIST_ENTRY *Entry, *Next;
  UFS_PASS_THRU_TRANS_REQ *Request;
  EFI_STATUS Status;
  UINT32 Doorbell;
  EFI_STATUS ControllerStatus = EFI_SUCCESS;
  EFI_TPL OldTpl;
  if (Private->CleanupInProgress) return;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Private->CleanupInProgress = TRUE;
  if (Private->Queue.ForwardLink != &Private->Queue && !Private->IoUnavailable)
    ControllerStatus = UfsCheckControllerErrors (Private);
  BASE_LIST_FOR_EACH_SAFE (Entry, Next, &Private->Queue) {
    Request = UFS_PASS_THRU_TRANS_REQ_FROM_THIS (Entry);
    if (Request->Packet == NULL || Private->IoUnavailable) {
      if (Request->Packet != NULL) Request->CompletionStatus = EFI_DEVICE_ERROR;
      UfsFinishOwnedRequest (Private, Request);
      continue;
    }
    Status = ControllerStatus;
    if (!EFI_ERROR (Status))
      Status = UfsMmioRead32 (Private, UFS_HC_UTRLDBR_OFFSET, &Doorbell);
    if (!EFI_ERROR (Status))
      Status = UfsCheckControllerErrors (Private);
    if (!EFI_ERROR (Status) && (Doorbell & (1U << Request->Slot)) != 0) {
      if (Request->TimeoutRemain == 0) continue; // Protocol: zero means no timeout.
      if (Request->TimeoutRemain > UFS_HC_ASYNC_TIMER) {
        Request->TimeoutRemain -= UFS_HC_ASYNC_TIMER;
        continue;
      }
      Status = EFI_TIMEOUT;
    } else if (!EFI_ERROR (Status)) {
      Request->DmaFenced = TRUE;
      Status = UfsValidateScsiResponse (Private, Request);
      Request->ResponseValidated = !EFI_ERROR (Status);
    }
    Request->CompletionStatus = Status;
    UfsFinishOwnedRequest (Private, Request);
  }
  Private->CleanupInProgress = FALSE;
  gBS->RestoreTPL (OldTpl);
}

/**
  Execute UIC command.

  @param[in]      This        Pointer to driver interface produced by the UFS controller.
  @param[in, out] UicCommand  Descriptor of the command that will be executed.

  @retval EFI_SUCCESS            Command executed successfully.
  @retval EFI_INVALID_PARAMETER  This or UicCommand is NULL.
  @retval Others                 Command failed to execute.
**/
EFI_STATUS
EFIAPI
UfsHcDriverInterfaceExecUicCommand (
  IN     EDKII_UFS_HC_DRIVER_INTERFACE  *This,
  IN OUT EDKII_UIC_COMMAND              *UicCommand
  )
{
  UFS_PASS_THRU_PRIVATE_DATA  *Private;

  if ((This == NULL) || (UicCommand == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Private = UFS_PASS_THRU_PRIVATE_DATA_FROM_DRIVER_INTF (This);

  return UfsExecUicCommands (Private, UicCommand);
}

/**
  Initializes UfsHcInfo field in private data.

  @param[in] Private  Pointer to host controller private data.

  @retval EFI_SUCCESS  UfsHcInfo initialized successfully.
  @retval Others       Failed to initalize UfsHcInfo.
**/
EFI_STATUS
GetUfsHcInfo (
  IN UFS_PASS_THRU_PRIVATE_DATA  *Private
  )
{
  UINT32      Data;
  EFI_STATUS  Status;

  Status = UfsMmioRead32 (Private, UFS_HC_VER_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UfsHcInfo.Version = Data;

  Status = UfsMmioRead32 (Private, UFS_HC_CAP_OFFSET, &Data);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Private->UfsHcInfo.Capabilities = Data;

  if ((mUfsHcPlatform != NULL) && (mUfsHcPlatform->OverrideHcInfo != NULL)) {
    Status = mUfsHcPlatform->OverrideHcInfo (Private->Handle, &Private->UfsHcInfo);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Failure from platform on OverrideHcInfo, Status = %r\n", Status));
      return Status;
    }
  }

  return EFI_SUCCESS;
}
