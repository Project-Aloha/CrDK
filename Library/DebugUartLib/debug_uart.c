/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "debug_uart_internal.h"
#include <Library/CrTargetDebugUartLib.h>

VOID MsmGeniSerialIsr(VOID *Params)
{
  CrDebugUartContext *DebugUartContext = (CrDebugUartContext *)Params;
  UINT32              StatusMIrq       = 0;
  UINT32              StatusSIrq       = 0;

  StatusMIrq = CrMmioRead32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_STATUS);
  StatusSIrq = CrMmioRead32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_S_IRQ_STATUS);

//  log_info(
//      "MsmGeniSerialIsr called, StatusMIrq=0x%X, StatusSIrq=0x%X", StatusMIrq,
//      StatusSIrq);

  // Clear the IRQ
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_CLR, StatusMIrq);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_S_IRQ_CLR, StatusSIrq);

  if (StatusMIrq & DEBUG_UART_GENI_M_ILLEGEAL_CMD_EN) {
    // log_warn(
    //     "Debug UART Illegal Command. StatusMIrq=0x%X, StatusSIrq=0x%X",
    //     StatusMIrq, StatusSIrq);
  }

  if (StatusSIrq & DEBUG_UART_GENI_S_RX_FIFO_WR_ERR_EN) {
//    log_warn(
//        "Debug UART Buffer Overrun. StatusMIrq=0x%X, StatusSIrq=0x%X",
//        StatusMIrq, StatusSIrq);
  }

  if (StatusMIrq & DEBUG_UART_GENI_M_CMD_DONE_EN) {
    // log_info("Debug UART Tx Command Done.");
    DebugUartContext->TxComplete = TRUE;
  }

  // Check if dma is enabled
  if (CrMmioRead32(
          DebugUartContext->BaseAddress + DEBUG_UART_GENI_DMA_MODE_EN)) {
    // log_warn("Dma mode is not supported");
    return;
  }

  // Check word count in fifo
  UINT8 WordCount =
      CrMmioRead32(
          DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_FIFO_STATUS) &
      DEBUG_UART_GENI_RX_FIFO_STATUS_FIFO_WC_MSK;

  // Read all words
  while (WordCount--) {
    UINT32 Data =
        CrMmioRead32(DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_FIFOn);
        // Fill ring buffer with up to 4 bytes
        for (UINT8 i = 0; i < 4; i++) {
            if (DebugUartContext->InputBuffer == NULL ||
                    DebugUartContext->InputBufferCapacity == 0) {
                break;
            }
            UINT8 byte = (Data >> (i * 8)) & 0xFF;
            if (DebugUartContext->InputBufferSize < DebugUartContext->InputBufferCapacity) {
                // space available: write at tail
                ((UINT8 *)DebugUartContext->InputBuffer)[DebugUartContext->InputBufferTail] = byte;
                DebugUartContext->InputBufferTail++;
                if (DebugUartContext->InputBufferTail >= DebugUartContext->InputBufferCapacity)
                    DebugUartContext->InputBufferTail = 0;
                DebugUartContext->InputBufferSize++;
            } else {
                // buffer full: overwrite oldest by advancing head, then write at tail
                DebugUartContext->InputBufferHead++;
                if (DebugUartContext->InputBufferHead >= DebugUartContext->InputBufferCapacity)
                    DebugUartContext->InputBufferHead = 0;
                ((UINT8 *)DebugUartContext->InputBuffer)[DebugUartContext->InputBufferTail] = byte;
                DebugUartContext->InputBufferTail++;
                if (DebugUartContext->InputBufferTail >= DebugUartContext->InputBufferCapacity)
                    DebugUartContext->InputBufferTail = 0;
                // size remains at capacity
            }
        }
  }
  // log_info("Count: %d", DebugUartContext->InputBufferSize);
}

VOID MsmGeniWriteTxFifo(
    CrDebugUartContext *DebugUartContext, VOID *Buffer, UINTN Length)
{
  if (Length == 0 || Buffer == NULL)
    return;

  // Check available space in TX FIFO
  UINT32 TxFifoStatus = CrMmioRead32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_FIFO_STATUS);
  UINT32 AvailableTxFifoBytes =
      DebugUartContext->TxFifoDepth -
      (TxFifoStatus & DEBUG_UART_GENI_TX_FIFO_STATUS_FIFO_WC_MSK);
  UINT32 BufToWrite = 0;

  CrMmioUpdateBits32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_EN,
      DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_WATERMARK_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_DONE_EN,
      DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_WATERMARK_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_DONE_EN);

  while (Length > 0) {
    DebugUartContext->TxComplete = FALSE;
    BufToWrite                   = 0;
    // Merge bytes to write into 32bit words
    UINT8 BytesThisRound = (Length > sizeof(UINT32)) ? sizeof(UINT32) : Length;
    for (UINT8 i = 0; i < BytesThisRound; i++) {
      BufToWrite |= ((UINT8 *)Buffer)[i] << (i * 8);
    }

    // Wait until there is space in TX FIFO
    while (AvailableTxFifoBytes == 0) {
      TxFifoStatus = CrMmioRead32(
          DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_FIFO_STATUS);
      AvailableTxFifoBytes =
          (DebugUartContext->TxFifoDepth -
           (TxFifoStatus & DEBUG_UART_GENI_TX_FIFO_STATUS_FIFO_WC_MSK)) *
          4;
    }

    // Setup TX buffer
    CrMmioUpdateBits32(
        DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_CMD0,
        DEBUG_UART_GENI_M_CMD0_M_OPCODE_MSK,
        (OPCODE_SE_M_UART_START_TX) << DEBUG_UART_GENI_M_CMD0_M_OPCODE_SFT);

    CrMmioWrite32(
        DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_TRANS_LEN,
        BytesThisRound);

    // Write to TX FIFO
    CrMmioWrite32(
        DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_FIFOn, BufToWrite);

    // Wait for transfer to complete
    for (UINT32 WaitTime = 0; WaitTime < DEBUG_UART_TX_TIMEOUT_US; WaitTime++) {
      if (DebugUartContext->TxComplete) {
        break;
      }
      cr_sleep(1);
    }

    Buffer += BytesThisRound;
    Length -= BytesThisRound;
  }

  // Stop Transfer
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_CLR,
      DEBUG_UART_GENI_M_IRQ_CLR_M_TX_FIFO_WATERMARK_EN);

  CrMmioUpdateBits32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_EN,
      DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_WATERMARK_EN, 0);
}

STATIC
UINT32
DebugUartGetTxFifoDepth(CrDebugUartContext *DebugUartContext)
{
  return GET_FIELD(
      CrMmioRead32(
          DebugUartContext->BaseAddress + DEBUG_UART_GENI_SE_HW_PARAM_0),
      DEBUG_UART_GENI_SE_HW_PARAM_0_TX_FIFO_DEPTH_MSK);
}

STATIC
VOID DebugUartSetRxFifoWatermarkRfr(
    CrDebugUartContext *DebugUartContext, UINT32 Watermark, UINT32 Rfr)
{
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_WATERMARK, Watermark);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_RFR_WATERMARK, Rfr);
}

STATIC
VOID DebugUartSetTxFifoWatermark(
    CrDebugUartContext *DebugUartContext, UINT32 Watermark)
{
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_WATERMARK, Watermark);
}

STATIC
UINT32
DebugUartGetRxFifoDepth(CrDebugUartContext *DebugUartContext)
{
  return GET_FIELD(
      CrMmioRead32(
          DebugUartContext->BaseAddress + DEBUG_UART_GENI_SE_HW_PARAM_1),
      DEBUG_UART_GENI_SE_HW_PARAM_1_RX_FIFO_DEPTH_MSK);
}

typedef enum {
  QUPV3_NONE = 0,
  QUPV3_SPI,
  QUPV3_UART,
  QUPV3_I2C,
  QUPV3_I3C,
  QUPv3_SLAVE_SPI,
} QUPV3_SE_TYPE;

#define DEBUG_UART_GENI_FW_REVISION_RO_FW_REV_PROTOCOL_MSK GEN_MSK(15, 8)
#define DEBUG_UART_GENI_FW_REVISION_RO 0x68

CR_STATUS
CrDebugUartLibInit(OUT CrDebugUartContext **OutDebugUartContext)
{
  CR_STATUS           Status           = CR_SUCCESS;
  CrDebugUartContext *DebugUartContext = CrTargetGetDebugUartContext();
  if (DebugUartContext == NULL) {
    log_err("No Debug Uart Context provided!");
    return CR_DEVICE_ERROR;
  }

  if (OutDebugUartContext != NULL)
    *OutDebugUartContext = DebugUartContext;

  // Verify se proto
  if (GET_FIELD(
          CrMmioRead32(
              DebugUartContext->BaseAddress + DEBUG_UART_GENI_FW_REVISION_RO),
          DEBUG_UART_GENI_FW_REVISION_RO_FW_REV_PROTOCOL_MSK) != QUPV3_UART) {
    log_err("Debug Uart proto is not UART!");
  }

  // Clear all IRQ
  // CrMmioWrite32(
  //     DebugUartContext->BaseAddress + DEBUG_UART_GENI_GSI_EVENT_EN, 0);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_CLR, 0xffffffff);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_DMA_TX_IRQ_CLR,
      0xffffffff);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_DMA_RX_IRQ_CLR,
      0xffffffff);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_SE_IRQ_EN, 0xffffffff);

  // Read RX FIFO depth
  DebugUartContext->RxFifoDepth = DebugUartGetRxFifoDepth(DebugUartContext);
  DebugUartContext->TxFifoDepth = DebugUartGetTxFifoDepth(DebugUartContext);
  log_info(
      "Debug UART Rx Fifo depth: %d, Tx FIFO Depth: %d",
      DebugUartContext->RxFifoDepth, DebugUartContext->TxFifoDepth);

  // Set watermark to 2 in console driver
  DebugUartSetRxFifoWatermarkRfr(
      DebugUartContext, 2, DebugUartContext->RxFifoDepth - 2);
  DebugUartSetTxFifoWatermark(DebugUartContext, 2);

  // Enable M/S IRQs
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_M_IRQ_EN,
      DEBUG_UART_GENI_M_IRQ_EN_M_CMD_OVERRUN_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_ILLEGAL_CMD_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_FAILURE_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_CANCEL_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_ABORT_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_TIMESTAMP_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_IO_DATA_DEASSERT_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_IO_DATA_ASSERT_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_RX_FIFO_RD_ERR_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_RX_FIFO_WR_ERR_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_RD_ERR_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_WR_ERR_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_RX_FIFO_WATERMARK_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_RX_FIFO_LAST_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_TX_FIFO_WATERMARK_EN |
          DEBUG_UART_GENI_M_IRQ_EN_M_CMD_DONE_EN);

  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_S_IRQ_EN,
      DEBUG_UART_GENI_S_IRQ_EN_S_CMD_OVERRUN_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_ILLEGAL_CMD_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_CMD_FAILURE_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_CMD_CANCEL_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_CMD_ABORT_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_GP_IRQ_0_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_GP_IRQ_1_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_GP_IRQ_2_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_GP_IRQ_3_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_GP_IRQ_4_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_RX_FIFO_RD_ERR_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_RX_FIFO_WR_ERR_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_RX_FIFO_WATERMARK_EN |
          DEBUG_UART_GENI_S_IRQ_EN_S_RX_FIFO_LAST_EN);

#define DEBUG_UART_GENI_TX_TRANS_CFG 0x25c
#define DEBUG_UART_GENI_TX_WORD_LEN 0x268
#define DEBUG_UART_GENI_TX_STOP_BIT_LEN 0x26c
#define DEBUG_UART_GENI_RX_TRANS_CFG 0x280
#define DEBUG_UART_GENI_TX_PARITY_CFG 0x2a4
#define DEBUG_UART_GENI_RX_PARITY_CFG 0x2a8
#define DEBUG_UART_GENI_RX_WORD_LEN 0x28c

  /* Setup UART parameters */
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_TRANS_CFG, 2);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_PARITY_CFG, 0);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_TRANS_CFG, 0);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_PARITY_CFG, 0);
  CrMmioWrite32(DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_WORD_LEN, 8);
  CrMmioWrite32(DebugUartContext->BaseAddress + DEBUG_UART_GENI_RX_WORD_LEN, 8);
  CrMmioWrite32(
      DebugUartContext->BaseAddress + DEBUG_UART_GENI_TX_STOP_BIT_LEN, 0);

  // Register interrupt handler
  Status = CrRegisterInterrupt(
      DebugUartContext->InterruptNo,    // Irq Number
      MsmGeniSerialIsr,                 // Handler
      DebugUartContext,                 // Param
      CR_INTERRUPT_TRIGGER_LEVEL_HIGH); // Level trigger
  if (CR_ERROR(Status)) {
    log_err("Failed to register DebugUart interrupt, Status=0x%X", Status);
    return Status;
  }

  MsmGeniWriteTxFifo(
      DebugUartContext, "Debug Uart Initialized.\n",
      sizeof("Debug Uart Initialized.\n") - 1);

  return Status;
}
