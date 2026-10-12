/** @file GENI UART input readiness and partial FIFO regression.
  SPDX-License-Identifier: MIT
**/
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

#define _PCD_VALUE_PcdUartSerialBase 0
#define _PCD_VALUE_PcdUartDefaultBaudRate 115200
#include "../../MUCommonCrPkg/Library/QcomGeniSerialPortLib/GeniSerial.h"

static UINT32 Registers[0x1000 / 4];
static UINT32 FifoWords[128];
static UINT32 FifoLength, FifoReadIndex, FifoPartialBytes;
static BOOLEAN FifoLast;
static UINT32 StatusReads;

VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size);

static UINT32 TestRead (UINT32 Group, UINT32 Offset, UINT32 Mask) {
  UINT32 Address = Group + Offset;
  UINT32 Value;
  assert (Address < sizeof (Registers));
  if (Address == GENI4_DATA + GENI_RX_FIFO_STATUS_ADDR) {
    ++StatusReads;
    Value = FifoLength - FifoReadIndex;
    /* The byte-valid bits may retain the preceding packet's value, even
       when RX_LAST is clear or WC has become zero. */
    Value |= FifoPartialBytes << RX_LAST_VALID_BYTES_SHIFT;
    if (FifoLast) Value |= RX_LAST;
  } else if (Address == GENI4_DATA + GENI_RX_FIFO_ADDR) {
    assert (FifoReadIndex < FifoLength);
    Value = FifoWords[FifoReadIndex++];
  } else {
    Value = Registers[Address / 4];
  }
  return Value & Mask;
}

static VOID TestWrite (UINT32 Group, UINT32 Offset, UINT32 Value) {
  UINT32 Address = Group + Offset;
  assert (Address < sizeof (Registers));
  if (Address == GENI4_DATA + GENI_S_IRQ_CLEAR_ADDR)
    Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / 4] &= ~Value;
  else if (Address == GENI4_DATA + GENI_M_IRQ_CLEAR_ADDR)
    Registers[(GENI4_DATA + GENI_M_IRQ_STATUS_ADDR) / 4] &= ~Value;
  else
    Registers[Address / 4] = Value;
}

#undef MemRead
#undef MemWrite
#define MemRead(_group, _offset, _mask) TestRead ((_group), (_offset), (_mask))
#define MemWrite(_group, _offset, _value) TestWrite ((_group), (_offset), (_value))
#include "../../MUCommonCrPkg/Library/QcomGeniSerialPortLib/QcomGeniSerialPortLib.c"

UINTN EFIAPI MicroSecondDelay (UINTN Delay) { return Delay; }

static VOID ResetInput (VOID) {
  ZeroMem (Registers, sizeof (Registers));
  mRxReadIndex = mRxWriteIndex = mRxCount = 0;
  FifoLength = FifoReadIndex = FifoPartialBytes = 0;
  FifoLast = FALSE;
  StatusReads = 0;
}

VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size) {
  return memset (Buffer, 0, Size);
}

int main (void) {
  UINT8 Output[512];
  UINT32 Index;

  ResetInput ();
  Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / 4] =
      RX_LAST_IRQ | RX_FIFO_WATERMARK_IRQ;
  Output[0] = 0xA5;
  assert (!SerialPortPoll ());
  assert (SerialPortRead (Output, 1) == 0 && Output[0] == 0xA5);
  assert (Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / 4] == 0);

  /* Stale byte-valid state without RX_LAST cannot truncate a complete word. */
  ResetInput ();
  FifoLength = 1;
  FifoPartialBytes = 1;
  FifoWords[0] = 0x44332211;
  assert (SerialPortPoll ());
  assert (FifoReadIndex == 1 && mRxCount == 4);
  Index = StatusReads;
  assert (SerialPortPoll () && StatusReads == Index);
  assert (SerialPortRead (Output, 4) == 4);
  assert (memcmp (Output, "\x11\x22\x33\x44", 4) == 0);
  assert (!SerialPortPoll ());

  /* One-to-three final bytes follow all preceding complete FIFO words. */
  for (UINT32 Partial = 1; Partial <= 3; ++Partial) {
    ResetInput ();
    FifoLength = 2;
    FifoPartialBytes = Partial;
    FifoLast = TRUE;
    FifoWords[0] = 0x44332211;
    FifoWords[1] = 0x88776655;
    Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / 4] = RX_LAST_IRQ;
    assert (SerialPortPoll ());
    assert (SerialPortRead (Output, sizeof (Output)) == 4 + Partial);
    assert (memcmp (Output, "\x11\x22\x33\x44\x55\x66\x77", 4 + Partial) == 0);
    assert (!SerialPortPoll ());
  }

  /* RX_LAST with zero means a full word; invalid byte counts stay bounded. */
  for (UINT32 Partial = 0; Partial <= 7; ++Partial) {
    if (Partial > 0 && Partial < 4) continue;
    ResetInput ();
    FifoLength = 1;
    FifoPartialBytes = Partial;
    FifoLast = TRUE;
    FifoWords[0] = 0x44332211;
    assert (SerialPortPoll ());
    assert (SerialPortRead (Output, sizeof (Output)) == 4);
    assert (!SerialPortPoll ());
  }

  /* A full software ring leaves a final partial hardware word unconsumed,
     then receives it intact after the caller has drained the ring. */
  ResetInput ();
  FifoLength = 65;
  FifoPartialBytes = 3;
  FifoLast = TRUE;
  for (Index = 0; Index < FifoLength; ++Index) FifoWords[Index] = 0x44332211;
  FifoWords[64] = 0x00CCBBAA;
  assert (SerialPortPoll ());
  assert (mRxCount == RING_SIZE && FifoReadIndex == 64);
  assert (SerialPortRead (Output, RING_SIZE) == RING_SIZE);
  assert (SerialPortPoll ());
  assert (SerialPortRead (Output, 3) == 3);
  assert (memcmp (Output, "\xAA\xBB\xCC", 3) == 0);
  assert (!SerialPortPoll ());
  puts ("GENI serial ConIn passed: sticky IRQ, RX_LAST byte count, partial word and ring bounds");
  return 0;
}
