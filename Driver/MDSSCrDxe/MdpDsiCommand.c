/** @file
  MSM 6G command DMA, derived from Linux dsi_host.c/dsi.xml and Android
  dsi_ctrl_hw_cmn_soft_reset(). No firmware binary register sequences.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include "MdpDsiCommand.h"
#include "MdpDsiSequence.h"
#include "MdpResources.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>

/* Linux sm8450.dtsi uses the same MSM 6G host layout for both instances. */
#define HOST0 (0x0ae94000U + 4U)
#define HOST1 (0x0ae96000U + 4U)
#define CTRL 0x00
#define STATUS 0x04
#define DMA_CTRL 0x38
#define DMA_BASE 0x44
#define DMA_LEN 0x48
#define TRIG_CTRL 0x80
#define TRIG_DMA 0x8c
#define TRIG_DMA_TRIGGER_SW 4U
#define TRIG_STREAM(_v) (((UINT32)(_v) & 0x3U) << 8)
#define INTR 0x10c
#define RESET 0x114
#define CLK_CTRL 0x118
#define BUSY (BIT0 | BIT1 | BIT2 | BIT3 | BIT4)
#define IRQ_STATUS (BIT0 | BIT8 | BIT16 | BIT20 | BIT24)
#define IRQ_MASK (BIT1 | BIT9 | BIT17 | BIT21 | BIT25)
#define POLLS 20000
STATIC UINTN mHost;
STATIC UINT32 mController;
STATIC BOOLEAN mOwned;
STATIC BOOLEAN mSaved;
STATIC BOOLEAN mFrozen;
STATIC EFI_STATUS mFreezeStatus;
STATIC UINT32 mCtrl, mDma, mTrigger, mBase, mLength, mClock;
STATIC UINT32 Read (UINTN R) { return MmioRead32 (mHost + R); }
STATIC VOID Write (UINTN R, UINT32 V) { MmioWrite32 (mHost + R, V); }

/* W1C only error bits, never FIFO fullness/watermark or clock-active bits. */
STATIC BOOLEAN Errors (VOID)
{
  STATIC CONST UINT32 Reg[] = {0x64, 0xbc, 0xb0, 0x08, STATUS, 0x11c};
  STATIC CONST UINT32 Mask[] = {0xffffffff, 0xffffffff, 0x11111,
    0xcccc4489, BIT31, BIT16};
  UINTN I;
  UINT32 V;
  BOOLEAN Error = FALSE;
  for (I = 0; I < sizeof (Reg) / sizeof (Reg[0]); I++) {
    V = Read (Reg[I]) & Mask[I];
    if (V != 0) {
      Error = TRUE;
      Write (Reg[I], V);
      if (Reg[I] == 0x64) Write (Reg[I], 0);
    }
  }
  return Error;
}

STATIC BOOLEAN Idle (VOID)
{
  UINTN I;
  for (I = 0; I < POLLS; I++) {
    if ((Read (STATUS) & BUSY) == 0) return TRUE;
    MicroSecondDelay (10);
  }
  return FALSE;
}

STATIC BOOLEAN Fence (VOID)
{
  /* Android soft-reset ordering, restricted to non-video operation. Keep
     engines disabled until reset and documented STATUS busy bits are clear. */
  Write (INTR, IRQ_STATUS);
  Write (CTRL, Read (CTRL) & ~7U);
  MemoryFence ();
  Write (CLK_CTRL, mClock | 0x23fU | BIT8 | BIT9 | BIT11 | BIT21);
  MemoryFence ();
  Write (RESET, 1);
  MemoryFence ();
  MicroSecondDelay (1);
  Write (RESET, 0);
  MemoryFence ();
  return Idle ();
}

STATIC EFI_STATUS Finish (VOID)
{
  EFI_STATUS Status;
  Write (INTR, IRQ_STATUS); /* mask every source on all owned exit paths */
  if (mSaved) {
    Write (DMA_CTRL, mDma);
    Write (TRIG_CTRL, mTrigger);
    Write (DMA_BASE, mBase);
    Write (DMA_LEN, mLength);
    Write (CLK_CTRL, mClock);
    Write (CTRL, mCtrl);
    MemoryFence ();
    mSaved = FALSE;
  }
  Status = MdpReleaseCommandBuffer ();
  if (EFI_ERROR (Status)) return EFI_NOT_READY;
  mOwned = FALSE;
  return EFI_SUCCESS;
}

EFI_STATUS MdpDsiCommandQuiesce (UINT32 Controller)
{
  UINT32 Active;
  EFI_STATUS Status;
  if (mFrozen) return EFI_ACCESS_DENIED;
  if (Controller > 1) return EFI_UNSUPPORTED;
  if (!mOwned) return EFI_SUCCESS;
  if (Controller != mController) return EFI_NOT_READY;
  Status = MdpGetActiveController (&Active);
  if (EFI_ERROR (Status)) return Status;
  if (Active != Controller) return EFI_NOT_READY;
  if (mSaved && !Fence ()) return EFI_NOT_READY;
  return Finish ();
}

EFI_STATUS MdpDsiCommandFreeze (VOID)
{
  if (mFrozen) return mFreezeStatus;
  mFrozen = TRUE;
  /* A retained command cannot be certified as a live primary display, even
     if its DMA can be fenced. Retain all map/storage journals for SMMU. */
  mFreezeStatus = mOwned ? EFI_NOT_READY : EFI_SUCCESS;
  if (mOwned && mSaved && !Fence ()) mFreezeStatus = EFI_DEVICE_ERROR;
  return mFreezeStatus;
}

EFI_STATUS MdpDsiCommandTransfer (UINT32 Controller, UINT8 VirtualChannel,
  UINT8 DataType, CONST UINT8 *Payload, UINTN Length, BOOLEAN LowPower)
{
  UINTN Expected, Size, I;
  BOOLEAN Long, Failed;
  VOID *Cpu;
  UINT8 *Data;
  EFI_PHYSICAL_ADDRESS Device;
  UINTN Capacity;
  UINT32 Irq, Active, ClockTiming;
  EFI_STATUS Result, Status;
  if (mFrozen) return EFI_ACCESS_DENIED;
  if (Controller > 1) return EFI_UNSUPPORTED;
  if (VirtualChannel > 3 || (Length != 0 && Payload == NULL)) return EFI_INVALID_PARAMETER;
  Long = FALSE;
  switch (DataType) {
    case 0x03: Expected = 0; break;
    case 0x13: case 0x05: Expected = 1; break;
    case 0x23: case 0x15: Expected = 2; break;
    case 0x29: case 0x39: Long = TRUE; Expected = Length; break;
    default: return EFI_UNSUPPORTED; /* RX, ACK, compression, video excluded */
  }
  if (Length != Expected || Length > 65535 || (Long && Length == 0)) return EFI_INVALID_PARAMETER;
  if (Length > MDP_DSI_MAX_WRITE_PAYLOAD) return EFI_BAD_BUFFER_SIZE;
  if (mOwned) return EFI_NOT_READY;
  Status = MdpGetActiveController (&Active);
  if (EFI_ERROR (Status)) return Status;
  if (Active != Controller) return EFI_NOT_READY;
  /* Active ownership intentionally survives partial teardown so Quiesce can
     fence retained DMA. A new transfer additionally needs fully ready clocks
     and PHY; the timing accessor enforces ready/not-retained before MMIO. */
  Status = MdpGetHostClockTiming (&ClockTiming);
  if (EFI_ERROR (Status)) return Status;
  mHost = Controller == 0 ? HOST0 : HOST1;
  if ((Read (CTRL) & BIT1) || (Read (STATUS) & BUSY)) return EFI_NOT_READY;
  Status = MdpAcquireCommandBuffer (&Cpu, &Device, &Capacity);
  if (EFI_ERROR (Status)) return Status;
  mController = Controller;
  mOwned = TRUE;
  Size = Long ? ((Length + 7) & ~(UINTN)3) : 4;
  if (Size > Capacity || Device > MAX_UINT32 || (Device & 3) != 0 || Cpu == NULL) {
    Status = Finish ();
    return EFI_ERROR (Status) ? Status : EFI_BAD_BUFFER_SIZE;
  }
  Data = Cpu;
  SetMem (Data, Size, 0xff);
  Data[0] = Long ? (UINT8)Length : (Length > 0 ? Payload[0] : 0);
  Data[1] = Long ? (UINT8)(Length >> 8) : (Length > 1 ? Payload[1] : 0);
  Data[2] = DataType | (VirtualChannel << 6);
  Data[3] = BIT7 | (Long ? BIT6 : 0);
  if (Long) CopyMem (Data + 4, Payload, Length);
  WriteBackDataCacheRange (Data, Size);
  mCtrl = Read (CTRL); mDma = Read (DMA_CTRL); mTrigger = Read (TRIG_CTRL);
  mBase = Read (DMA_BASE); mLength = Read (DMA_LEN); mClock = Read (CLK_CTRL);
  mSaved = TRUE;
  Write (INTR, IRQ_STATUS);
  Errors ();
  Write (DMA_CTRL, BIT28 | (LowPower ? BIT26 : 0));
  /* Linux dsi_ctrl_enable() programs the stream selector from
     msm_host->channel.  The packet header VC and this selector must agree;
     keeping STREAM at zero would silently misroute non-zero VC panels. */
  Write (TRIG_CTRL, TRIG_DMA_TRIGGER_SW | TRIG_STREAM (VirtualChannel));
  Write (CTRL, mCtrl | BIT0 | BIT2);
  /* Android dsi_ctrl_hw_cmn_poll_dma_status reads raw CMD_DMA_DONE (bit0).
     BIT1/BIT25 only enable IRQ delivery; keep every source masked because
     this synchronous path does not register a DSI/MDSS interrupt handler. */
  Write (DMA_BASE, (UINT32)Device);
  Write (DMA_LEN, (UINT32)Size);
  MemoryFence ();
  Write (TRIG_DMA, 1);
  MemoryFence ();
  Result = EFI_TIMEOUT;
  for (I = 0; I < POLLS; I++) {
    Irq = Read (INTR);
    Failed = Errors ();
    if ((Irq & BIT24) || Failed) { Result = EFI_DEVICE_ERROR; break; }
    if (Irq & BIT0) { Result = EFI_SUCCESS; break; }
    MicroSecondDelay (10);
  }
  Write (INTR, IRQ_STATUS);
  if (EFI_ERROR (Result) || !Idle ()) {
    if (!EFI_ERROR (Result)) Result = EFI_TIMEOUT;
    if (!Fence ()) return EFI_NOT_READY;
  }
  Status = Finish ();
  return EFI_ERROR (Status) ? Status : Result;
}
