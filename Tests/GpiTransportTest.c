/** @file Qualcomm GPI ring, completion and quiescence tests. SPDX-License-Identifier: MIT */
#include "GeniTestCommon.h"

static BOOLEAN DmaDebugEnabled;
static VOID TestGpiDebugPrint (UINTN Level, CONST CHAR8 *Format, ...);

#undef DEBUG
#define DEBUG(Expression) do { if (DmaDebugEnabled) { TestGpiDebugPrint Expression; } } while (FALSE)

#include "../Driver/GpiCrDxe/GpiCrDxe.c"

#define TEST_CONFIGURE_PINS GpiConfigurePins
#include "GeniPinmuxTest.h"
#include "GpiDmaLifetimeTest.h"

EFI_GUID gEfiGpiCrProtocolGuid;
EFI_GUID gEfiSmmuCrProtocolGuid;
EFI_GUID gEdkiiIoMmuProtocolGuid;

static UINT32 Registers[0x30000 / 4];
/* Include the GENI DMA interface register at offset 0x2004. */
static UINT32 SeRegisters[0x3000 / 4];
static UINT32 TxData[64], RxData[64], Events[256], DmaData[1024];
static UINT32 TxHardwareOffset, RxHardwareOffset, EventProducer;
static UINT32 Writes, Completions, Aborts, Releases;
static UINT32 ConfigTres, CurrentGo, CurrentLength;
static BOOLEAN ReadGoPending, BusHeld, InjectControllerError;
static BOOLEAN TransferTimeout, StopTimeout, AbortTimeout, WrongPointer, WrongLength;
static BOOLEAN EventResetTimeout;
static BOOLEAN CheckLease;
static UINT32 NotifyStatus, CompletionStatus;
static BOOLEAN VerifyReadCache;
static BOOLEAN ReadInvalidateReady;
static UINT32 ReadInvalidateCalls, ReadDataWriteBackCalls;
static UINT8 ReadDevicePattern[8];

static BOOLEAN DmaSnapshotActive;
static UINT32 DmaSnapshotReads, DmaSnapshotContexts, DmaSnapshotSeRegisters;
static UINT32 DmaSnapshotsCompleted, DoorbellsWritten, DoorbellsReturned;
static UINT32 EventWaitBeginCount, EventWaitReturnedCount;
static UINTN CorruptContextWrite;
static UINT32 ChannelStarts, DataDoorbells, EventDoorbells;

static VOID TestGpiDebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {
  va_list Arguments;
  (VOID)Level;
  va_start (Arguments, Format);
  if (!strcmp (Format, "GPI: DMA snapshot stage=%a ring=%u begin\n")) {
    CONST CHAR8 *Stage = va_arg (Arguments, CONST CHAR8 *);
    UINT32 Ring = va_arg (Arguments, UINT32);
    if (Ring == 0) {
      assert (!DmaSnapshotActive);
      if (!strcmp (Stage, "before-doorbell")) assert (DoorbellsWritten == 0);
      if (!strcmp (Stage, "after-doorbell"))
        assert ((DoorbellsReturned & BIT (GPI_TX_CH)) != 0);
      DmaSnapshotActive = TRUE;
    } else {
      assert (DmaSnapshotActive);
    }
  } else if (!strcmp (Format, "GPI: DMA snapshot stage=%a complete\n")) {
    assert (DmaSnapshotActive);
    DmaSnapshotActive = FALSE;
    ++DmaSnapshotsCompleted;
  } else if (!strcmp (Format, "GPI: RX doorbell write returned\n")) {
    assert (DmaSnapshotsCompleted == 1 && !DmaSnapshotActive);
    assert (DoorbellsWritten == BIT (GPI_RX_CH));
    DoorbellsReturned |= BIT (GPI_RX_CH);
  } else if (!strcmp (Format, "GPI: TX doorbell write returned\n")) {
    assert (DmaSnapshotsCompleted == 1 && !DmaSnapshotActive);
    assert ((DoorbellsWritten & BIT (GPI_TX_CH)) != 0);
    if ((DoorbellsWritten & BIT (GPI_RX_CH)) != 0)
      assert ((DoorbellsReturned & BIT (GPI_RX_CH)) != 0);
    DoorbellsReturned |= BIT (GPI_TX_CH);
  } else if (!strcmp (Format, "GPI: event wait begin\n")) {
    assert (DmaSnapshotsCompleted == 2 && !DmaSnapshotActive);
    assert (DoorbellsReturned == DoorbellsWritten);
    ++EventWaitBeginCount;
  } else if (!strcmp (Format, "GPI: event wait returned status=%r\n")) {
    assert (EventWaitBeginCount == 1 && !DmaSnapshotActive);
    ++EventWaitReturnedCount;
  }
  va_end (Arguments);
}

UINT32 EFIAPI MmioRead32 (UINTN Address) {
  if (Address >= mGpi.SeBase && Address < mGpi.SeBase + sizeof (SeRegisters)) {
    if (DmaSnapshotActive) {
      UINTN Offset = Address - mGpi.SeBase;
      if (Offset == SE_GENI_STATUS) DmaSnapshotSeRegisters |= BIT (0);
      else if (Offset == SE_DMA_MODE) DmaSnapshotSeRegisters |= BIT (1);
      else if (Offset == SE_DMA_IF_EN) DmaSnapshotSeRegisters |= BIT (2);
      else if (Offset == SE_GSI_EVENT_EN) DmaSnapshotSeRegisters |= BIT (3);
      else if (Offset == SE_M_IRQ_EN) DmaSnapshotSeRegisters |= BIT (4);
      else assert (FALSE);
      ++DmaSnapshotReads;
    }
    return SeRegisters[(Address - mGpi.SeBase) / 4];
  }
  assert (Address >= mGpi.GpiBase && Address < mGpi.GpiBase + sizeof (Registers));
  if (DmaSnapshotActive) {
    CONST UINTN Contexts[] = {
      GPI_CH_CTX (GPII_ID, GPI_TX_CH), GPI_CH_CTX (GPII_ID, GPI_RX_CH),
      GPI_EV_CTX (GPII_ID)
    };
    UINTN Offset = Address - mGpi.GpiBase;
    BOOLEAN ContextRead = FALSE;
    for (UINTN Index = 0; Index < ARRAY_SIZE (Contexts); ++Index) {
      if ((Offset >= Contexts[Index]) &&
          (Offset <= Contexts[Index] + GPI_RING_WP_H) &&
          (((Offset - Contexts[Index]) % sizeof (UINT32)) == 0)) {
        DmaSnapshotContexts |= BIT (Index);
        ContextRead = TRUE;
        break;
      }
    }
    assert (ContextRead);
    ++DmaSnapshotReads;
  }
  return Registers[(Address - mGpi.GpiBase) / 4];
}

static void CompleteDoorbell (UINT8 Channel, UINT32 Value) {
  GPI_RING *Ring = Channel == GPI_TX_CH ? &mGpi.TxRing : &mGpi.RxRing;
  UINT32 *HardwareOffset = Channel == GPI_TX_CH ? &TxHardwareOffset : &RxHardwareOffset;
  UINT32 End = Value - (UINT32)Ring->Device;
  assert (End < Ring->Length && End % GPI_EL_SIZE == 0);
  /* RX DMA cannot run until the TX channel supplies its linked READ GO. */
  if (Channel == GPI_RX_CH && !ReadGoPending) return;
  while (*HardwareOffset != End) {
    UINT32 *Tre = (UINT32 *)((UINT8 *)Ring->Host + *HardwareOffset);
    UINT32 Type = (Tre[3] >> 16) & 0xff;
    /* A skipped slot or stale element cannot be interpreted as a valid TRE. */
    assert (Type == GPI_TRE_CONFIG0 || Type == GPI_TRE_GO || Type == GPI_TRE_DMA);
    if (Type == GPI_TRE_CONFIG0) {
      /* A continued I2C message must not reconfigure a stretched bus. */
      assert (Channel == GPI_TX_CH && !BusHeld);
      assert (Tre[0] == (11U | (5U << 8) | (22U << 16) | BIT (24) | BIT (25)));
      assert (Tre[2] == 2 && (Tre[3] & GPI_FLAG_CHAIN));
      ++ConfigTres;
    }
    if (Type == GPI_TRE_GO) {
      assert (Channel == GPI_TX_CH && ConfigTres != 0);
      assert ((SeRegisters[SE_M_IRQ_EN / 4] & (BIT (10) | BIT (12) | BIT (13))) ==
              (BIT (10) | BIT (12) | BIT (13)));
      CurrentGo = Tre[0];
      CurrentLength = Tre[2];
      if ((CurrentGo & 0x1f) == 2) {
        assert ((Tre[3] & (GPI_FLAG_LINK | GPI_FLAG_CHAIN)) == GPI_FLAG_LINK);
        ReadGoPending = TRUE;
        CompleteDoorbell (GPI_RX_CH, Registers[GPI_CH_DB (GPII_ID, GPI_RX_CH) / 4]);
        ReadGoPending = FALSE;
      } else {
        assert ((CurrentGo & 0x1f) == 1 && (Tre[3] & GPI_FLAG_CHAIN));
      }
    }
    if (Type == GPI_TRE_DMA && !TransferTimeout) {
      assert ((Tre[3] & GPI_FLAG_IEOT) && Tre[2] == CurrentLength);
      BusHeld = (CurrentGo & BIT (26)) != 0;
      if (VerifyReadCache && Channel == GPI_RX_CH) {
        /* The device writes only after the CPU has invalidated its stale
           cache line.  A write-back before this point would publish stale
           bytes back to the device on a non-coherent system. */
        assert (ReadInvalidateReady);
        assert (CurrentLength <= sizeof (ReadDevicePattern));
        CopyMem (mGpi.Data.Host, ReadDevicePattern, CurrentLength);
      }
      if (NotifyStatus != 0) {
        UINT32 *Notify = (UINT32 *)((UINT8 *)mGpi.EventRing.Host + EventProducer);
        Notify[0] = NotifyStatus;
        Notify[1] = Notify[2] = 0;
        Notify[3] = (GPI_EVENT_QUP_NOTIFY << 16) | ((UINT32)Channel << 24);
        EventProducer = (EventProducer + GPI_EL_SIZE) % mGpi.EventRing.Length;
      }
      UINT32 *Event = (UINT32 *)((UINT8 *)mGpi.EventRing.Host + EventProducer);
      EFI_PHYSICAL_ADDRESS Pointer = Ring->Device + *HardwareOffset + (WrongPointer ? 16 : 0);
      Event[0] = (UINT32)Pointer;
      Event[1] = (UINT32)(Pointer >> 32);
      Event[2] = (GPI_EVENT_EOT << 24) | (Tre[2] - (WrongLength ? 1 : 0));
      Event[3] = (GPI_EVENT_XFER_COMPLETE << 16) | ((UINT32)Channel << 24) | CompletionStatus;
      EventProducer = (EventProducer + GPI_EL_SIZE) % mGpi.EventRing.Length;
      Registers[(GPI_EV_CTX (GPII_ID) + GPI_RING_RP_L) / 4] =
        (UINT32)(mGpi.EventRing.Device + EventProducer);
      Registers[(GPI_EV_CTX (GPII_ID) + GPI_RING_RP_H) / 4] =
        (UINT32)(mGpi.EventRing.Device >> 32);
      ++Completions;
    }
    /* Model hardware consuming the TRE.  Reusing it without writing a new
       element must fail, including at the ring wrap boundary. */
    ZeroMem (Tre, GPI_EL_SIZE);
    *HardwareOffset = (*HardwareOffset + GPI_EL_SIZE) % Ring->Length;
  }
}

UINT32 EFIAPI MmioWrite32 (UINTN Address, UINT32 Value) {
  assert (!DmaSnapshotActive);
  ++Writes;
  if (CheckLease) assert (mSeOwners[1][1].Busy != 0);
  if (Address >= mGpi.SeBase && Address < mGpi.SeBase + sizeof (SeRegisters)) {
    UINTN Offset = Address - mGpi.SeBase;
    if (Offset == SE_DMA_MODE && (Value & SE_DMA_MODE_EN) != 0)
      assert ((SeRegisters[SE_DMA_IF_EN / 4] & SE_DMA_IF_EN_BIT) != 0);
    if (Offset == SE_GSI_EVENT_EN && Value != 0) {
      assert ((SeRegisters[SE_DMA_IF_EN / 4] & SE_DMA_IF_EN_BIT) != 0);
      assert ((SeRegisters[SE_DMA_MODE / 4] & SE_DMA_MODE_EN) != 0);
    }
    if (Offset == SE_M_IRQ_CLEAR) SeRegisters[SE_M_IRQ_STATUS / 4] &= ~Value;
    else SeRegisters[Offset / 4] = Value;
    if (Offset == SE_M_CMD_CTRL && Value == BIT (1)) {
      ++Aborts;
      if (!AbortTimeout) {
        SeRegisters[SE_M_IRQ_STATUS / 4] |= SE_M_CMD_ABORT;
        SeRegisters[SE_GENI_STATUS / 4] = 0;
        BusHeld = FALSE;
      }
    }
    return Value;
  }
  UINTN Offset = Address - mGpi.GpiBase;
  assert (Offset < sizeof (Registers));
  if (Offset == CorruptContextWrite) Value ^= BIT (0);
  if (Offset == GPI_CH_DB (GPII_ID, GPI_TX_CH) ||
      Offset == GPI_CH_DB (GPII_ID, GPI_RX_CH)) ++DataDoorbells;
  if (Offset == GPI_EV_DB (GPII_ID)) ++EventDoorbells;
  if (DmaDebugEnabled) {
    if (Offset == GPI_CH_DB (GPII_ID, GPI_RX_CH)) DoorbellsWritten |= BIT (GPI_RX_CH);
    if (Offset == GPI_CH_DB (GPII_ID, GPI_TX_CH)) DoorbellsWritten |= BIT (GPI_TX_CH);
  }
  if (Offset == GPI_CH_CMD (GPII_ID) || Offset == GPI_EV_CMD (GPII_ID)) {
    BOOLEAN Event = Offset == GPI_EV_CMD (GPII_ID);
    UINT32 Channel = Value & 0xff;
    UINT32 Opcode = Value >> 24;
    if (!Event && Opcode == 1) ++ChannelStarts;
    UINTN Config = Event ? GPI_EV_CTX (GPII_ID) : GPI_CH_CTX (GPII_ID, Channel);
    assert (Channel < 2);
    /* Model disabled control-status sources at cold entry. Enabling the
       GPII source and IRQ routing is required even when the CPU polls it;
       the top-level mask must stay off without a registered ISR. */
    assert ((Registers[(0x23088U + 0x4000U * GPII_ID) / 4] & 0x7fU) == 0);
    /* Linux enables every global/general source gate before issuing channel
       commands. Polling suppresses only the top-level delivery mask. */
    assert ((Registers[(0x23108U + 0x4000U * GPII_ID) / 4] & 1U) == 1U);
    assert ((Registers[(0x23120U + 0x4000U * GPII_ID) / 4] & 0xfU) == 0xfU);
    if (!(Registers[(0x23180U + 0x4000U * GPII_ID) / 4] & 1U) ||
        !(Registers[((Event ? 0x2309cU : 0x23098U) + 0x4000U * GPII_ID) / 4] &
          (Event ? 1U : BIT (Channel)))) return Value;
    if (InjectControllerError) {
      Registers[(0x23100U + 0x4000U * GPII_ID) / 4] = 1;
      return Value;
    }
    if (Opcode == 2 && StopTimeout) return Value;
    if (Event && Opcode == 9 && EventResetTimeout) return Value;
    /* Linux: DEALLOC acknowledges but leaves the hardware state unchanged. */
    UINT32 State = Opcode == 10 ? GPI_STATE (Registers[Config / 4]) :
        Opcode == 0 || Opcode == 9 ? 1 : Opcode == 1 ? 2 : Opcode == 2 ? 3 : 0;
    Registers[Config / 4] = (Registers[Config / 4] & ~(0xfU << 20)) | (State << 20);
    Registers[(Event ? GPI_EV_CTRL_STATUS (GPII_ID) : GPI_CH_CTRL_STATUS (GPII_ID)) / 4] |=
        Event ? BIT (0) : BIT (Channel);
    if (Opcode == 0 || Opcode == 9) {
      if (Event) {
        EventProducer = 0;
        Registers[(Config + GPI_RING_RP_L) / 4] = (UINT32)mGpi.EventRing.Device;
      } else if (Channel == GPI_TX_CH) TxHardwareOffset = 0;
      else RxHardwareOffset = 0;
    }
  } else if (Offset == GPI_EV_CTRL_CLEAR (GPII_ID)) {
    Registers[GPI_EV_CTRL_STATUS (GPII_ID) / 4] &= ~Value;
  } else if (Offset == GPI_CH_CTRL_CLEAR (GPII_ID)) {
    Registers[GPI_CH_CTRL_STATUS (GPII_ID) / 4] &= ~Value;
  } else if (Offset == 0x23110U + 0x4000U * GPII_ID) {
    Registers[(0x23100U + 0x4000U * GPII_ID) / 4] &= ~Value;
  } else if (Offset == 0x23128U + 0x4000U * GPII_ID) {
    Registers[(0x23118U + 0x4000U * GPII_ID) / 4] &= ~Value;
  } else if (Offset == GPI_CH_CTX (GPII_ID, 0) ||
             Offset == GPI_CH_CTX (GPII_ID, 1) || Offset == GPI_EV_CTX (GPII_ID)) {
    Registers[Offset / 4] = (Value & ~(0xfU << 20)) | (Registers[Offset / 4] & (0xfU << 20));
  } else {
    Registers[Offset / 4] = Value;
    if (Offset == GPI_CH_DB (GPII_ID, GPI_TX_CH)) CompleteDoorbell (GPI_TX_CH, Value);
    if (Offset == GPI_CH_DB (GPII_ID, GPI_RX_CH)) CompleteDoorbell (GPI_RX_CH, Value);
  }
  return Value;
}
UINTN EFIAPI MicroSecondDelay (UINTN Delay) { return Delay; }

static void VerifyReadWriteBack (VOID *Start, UINTN Length) {
  (VOID)Length;
  if (Start == mGpi.Data.Host) ++ReadDataWriteBackCalls;
}

static void VerifyReadInvalidate (VOID *Start, UINTN Length) {
  if (Start == mGpi.Data.Host) {
    assert (Length == sizeof (ReadDevicePattern));
    ++ReadInvalidateCalls;
    ReadInvalidateReady = TRUE;
  }
}

static EFI_STATUS EFIAPI Locate (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  assert (Guid == &gEfiQupCrProtocolGuid);
  *Interface = &mQupProtocol;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Release (EFI_QUP_CR_PROTOCOL *This, UINT8 Wrapper,
                                  UINT8 Engine, VOID *Owner, BOOLEAN Quiesced) {
  ++Releases;
  if (Quiesced) {
    assert (!mGpi.ChannelTouched[0] && !mGpi.ChannelTouched[1] && !mGpi.EventTouched);
    assert (SeRegisters[SE_GSI_EVENT_EN / 4] == 0);
  }
  return QupReleaseSe (This, Wrapper, Engine, Owner, Quiesced);
}
static void Reset (void) {
  DmaDebugEnabled = DmaSnapshotActive = FALSE;
  DmaSnapshotReads = DmaSnapshotContexts = DmaSnapshotSeRegisters = 0;
  DmaSnapshotsCompleted = DoorbellsWritten = DoorbellsReturned = 0;
  EventWaitBeginCount = EventWaitReturnedCount = 0;
  CorruptContextWrite = MAX_UINTN;
  ChannelStarts = DataDoorbells = EventDoorbells = 0;
  TestIccReset ();
  ZeroMem (&mGpi, sizeof (mGpi));
  ZeroMem (mSeOwners, sizeof (mSeOwners));
  ZeroMem (Registers, sizeof (Registers));
  ZeroMem (SeRegisters, sizeof (SeRegisters));
  SeRegisters[SE_GENI_FW_REVISION_RO / 4] = 3U << 8;
  ZeroMem (TxData, sizeof (TxData));
  ZeroMem (RxData, sizeof (RxData));
  ZeroMem (Events, sizeof (Events));
  mQupContext = &TestContext;
  mQupProtocol.ReleaseSe = Release;
  BootServices.LocateProtocol = Locate;
  mGpi.Protocol = mGpiProtocol;
  mGpi.Qup = &mQupProtocol;
  mGpi.GpiBase = 0x9f0000;
  mGpi.SeBase = 0xa84000;
  mGpi.TxRing = (GPI_RING){.Host = TxData, .Device = 0x100000, .Length = sizeof (TxData), .ElSize = 16};
  mGpi.RxRing = (GPI_RING){.Host = RxData, .Device = 0x200000, .Length = sizeof (RxData), .ElSize = 16};
  mGpi.EventRing = (GPI_RING){.Host = Events, .Device = 0x300000, .Length = sizeof (Events), .ElSize = 16};
  mGpi.Data = (GPI_BUFFER){.Host = DmaData, .Device = 0x400000, .Pages = 1};
  mGpi.Initialized = TRUE;
  TxHardwareOffset = RxHardwareOffset = EventProducer = Writes = Completions = Aborts = Releases = 0;
  ConfigTres = CurrentGo = CurrentLength = 0;
  ReadGoPending = BusHeld = InjectControllerError = FALSE;
  TransferTimeout = StopTimeout = AbortTimeout = WrongPointer = WrongLength = FALSE;
  EventResetTimeout = FALSE;
  CheckLease = TRUE;
  NotifyStatus = CompletionStatus = 0;
  VerifyReadCache = ReadInvalidateReady = FALSE;
  ReadInvalidateCalls = ReadDataWriteBackCalls = 0;
  TestWriteBackHook = NULL;
  TestInvalidateHook = NULL;
}
static void TestProtocolErrors (void) {
  UINT8 Buffer = 0;
  const UINT32 NotifyBits[] = {BIT (10), BIT (12), BIT (13)};
  /* DM_I2C_CB_ERR: ADDR_NACK, BUS_PROTO, ARB_LOST. */
  const UINT32 CompletionBits[] = {BIT (6), BIT (8), BIT (9)};
  for (UINT32 Read = 0; Read < 2; ++Read) {
    EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1,
      EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
      (Read ? EFI_GPI_I2C_FLAG_READ : EFI_GPI_I2C_FLAG_WRITE)};
    for (UINT32 Index = 0; Index < 6; ++Index) {
      UINT32 Transferred = 1;
      Reset ();
      if (Index < 3) NotifyStatus = NotifyBits[Index];
      else CompletionStatus = CompletionBits[Index - 3];
      assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1,
          &Transferred) == EFI_DEVICE_ERROR);
      assert (Transferred == 0 && Aborts == 1 && mSeOwners[1][1].Busy == 0);
    }
    /* A notification without the documented I2C error bits is harmless. */
    Reset ();
    NotifyStatus = BIT (0);
    assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_SUCCESS);
    Reset ();
    NotifyStatus = BIT (10);
    CompletionStatus = BIT (6);
    SeRegisters[SE_M_GP_LENGTH / 4] = 1;
    assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) ==
        (Read ? EFI_SUCCESS : EFI_DEVICE_ERROR));
  }
}
static void TestTransfers (void) {
  UINT8 Buffer[8] = {0};
  EFI_GPI_I2C_DESCRIPTOR Descs[40];
  UINT32 Transferred;
  Reset ();
  for (UINT32 Index = 0; Index < 40; ++Index)
    Descs[Index] = (EFI_GPI_I2C_DESCRIPTOR){Buffer, sizeof (Buffer),
        EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
        (Index % 2 ? EFI_GPI_I2C_FLAG_READ : EFI_GPI_I2C_FLAG_WRITE)};
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 40, &Transferred) == EFI_SUCCESS);
  assert (Transferred == 320 && Completions == 40 && Releases == 1);
  assert (ConfigTres == 1);
  /* Recycled EREs remain device-owned: never dirty their shared cache line
     by clearing the consumed element from the CPU. */
  assert (Events[3] == (GPI_EVENT_XFER_COMPLETE << 16));
  assert (mSeOwners[1][1].Busy == 0);
  /* Cross-request reinitialization starts at element zero again. */
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 2, &Transferred) == EFI_SUCCESS);
  assert (Transferred == 16 && Completions == 42 && Releases == 2);
  assert (ConfigTres == 2);
  /* Register-address write followed by a linked read on the same bus.
     This must work with no FIFO transfer or OEM GPI setup preceding it. */
  Reset ();
  Descs[0].Flags &= ~EFI_GPI_I2C_FLAG_STOP;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 2, &Transferred) == EFI_SUCCESS);
  assert (Transferred == 16 && Completions == 2 && ConfigTres == 1 && !BusHeld);
}
static void TestReadCacheOrdering (void) {
  UINT8 Buffer[sizeof (ReadDevicePattern)];
  EFI_GPI_I2C_DESCRIPTOR Desc = {
    Buffer, sizeof (Buffer), EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
      EFI_GPI_I2C_FLAG_READ
  };

  Reset ();
  for (UINTN Index = 0; Index < sizeof (ReadDevicePattern); ++Index) {
    ReadDevicePattern[Index] = (UINT8)(0x80U + Index);
    Buffer[Index] = 0;
  }
  /* The CPU copy starts stale.  A correct read path invalidates it before
     handing ownership to GPI and invalidates again before copying it out. */
  memset (mGpi.Data.Host, 0xa5, sizeof (ReadDevicePattern));
  VerifyReadCache = TRUE;
  TestWriteBackHook = VerifyReadWriteBack;
  TestInvalidateHook = VerifyReadInvalidate;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_SUCCESS);
  TestWriteBackHook = NULL;
  TestInvalidateHook = NULL;
  assert (ReadInvalidateCalls == 2);
  assert (ReadDataWriteBackCalls == 0);
  assert (memcmp (Buffer, ReadDevicePattern, sizeof (Buffer)) == 0);
  puts ("GPI DMA: read invalidation precedes device write and avoids stale writeback passed");
}
static void TestSeDmaInterface (void) {
  UINT8 Buffer = 0;
  CONST UINT32 InitialValues[] = {0, BIT (7)};
  for (UINTN Index = 0; Index < ARRAY_SIZE (InitialValues); ++Index) {
    for (UINT32 Read = 0; Read < 2; ++Read) {
      EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1,
        EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
        (Read ? EFI_GPI_I2C_FLAG_READ : EFI_GPI_I2C_FLAG_WRITE)};
      Reset ();
      SeRegisters[SE_DMA_IF_EN / 4] = InitialValues[Index];
      assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_SUCCESS);
      assert (SeRegisters[SE_DMA_IF_EN / 4] == (InitialValues[Index] | SE_DMA_IF_EN_BIT));
      assert ((SeRegisters[SE_DMA_MODE / 4] & SE_DMA_MODE_EN) != 0);
      assert (SeRegisters[SE_GSI_EVENT_EN / 4] == 0);
      assert (mSeOwners[1][1].Busy == 0 && Releases == 1);
    }
  }
  puts ("GPI init: DMA interface ordering, preserved bits and real transfer leases passed");
}
static void TestDmaDiagnostics (void) {
  static UINT32 RegisterSnapshot[ARRAY_SIZE (Registers)];
  static UINT32 SeRegisterSnapshot[ARRAY_SIZE (SeRegisters)];
  UINT8 Buffer = 0;

  Reset ();
  DmaDebugEnabled = TRUE;
  Registers[GPI_CH_CTRL_STATUS (GPII_ID) / 4] = 3;
  Registers[GPI_EV_CTRL_STATUS (GPII_ID) / 4] = 1;
  Registers[GPI_GLOBAL_STATUS (GPII_ID) / 4] = 1;
  Registers[GPI_GENERAL_STATUS (GPII_ID) / 4] = 0xf;
  CopyMem (RegisterSnapshot, Registers, sizeof (Registers));
  CopyMem (SeRegisterSnapshot, SeRegisters, sizeof (SeRegisters));
  GpiLogDmaState ("standalone");
  assert (DmaSnapshotReads != 0 && DmaSnapshotContexts == 7);
  assert (DmaSnapshotSeRegisters == 0x1f && DmaSnapshotsCompleted == 1);
  assert (!DmaSnapshotActive && Writes == 0);
  assert (!memcmp (RegisterSnapshot, Registers, sizeof (Registers)));
  assert (!memcmp (SeRegisterSnapshot, SeRegisters, sizeof (SeRegisters)));

  for (UINT32 Read = 0; Read < 2; ++Read) {
    for (UINT32 Timeout = 0; Timeout < 2; ++Timeout) {
      EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1,
        EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
        (Read ? EFI_GPI_I2C_FLAG_READ : EFI_GPI_I2C_FLAG_WRITE)};
      Reset ();
      DmaDebugEnabled = TRUE;
      TransferTimeout = Timeout != 0;
      assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) ==
              (Timeout ? EFI_TIMEOUT : EFI_SUCCESS));
      assert (DmaSnapshotsCompleted == 2 && !DmaSnapshotActive);
      assert (DmaSnapshotContexts == 7 && DmaSnapshotSeRegisters == 0x1f);
      assert (DoorbellsReturned == (Read ? 3U : BIT (GPI_TX_CH)));
      assert (EventWaitBeginCount == 1 && EventWaitReturnedCount == 1);
      if (!Timeout) {
        UINT32 SnapshotReads = DmaSnapshotReads;
        assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_SUCCESS);
        assert (DmaSnapshotReads == SnapshotReads && DmaSnapshotsCompleted == 2);
        assert (EventWaitBeginCount == 1 && EventWaitReturnedCount == 1);
      }
    }
  }
  DmaDebugEnabled = FALSE;
  puts ("GPI DMA: debug snapshots preserve IRQ state and bracket first RX/TX doorbells and event waits passed");
}

static void TestRingContextReadback (void) {
  const UINTN Contexts[] = {GPI_EV_CTX (GPII_ID),
    GPI_CH_CTX (GPII_ID, GPI_TX_CH), GPI_CH_CTX (GPII_ID, GPI_RX_CH)};
  const UINTN Fields[] = {GPI_CONFIG, GPI_LENGTH, GPI_RING_BASE_L, GPI_RING_BASE_H};
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1, 7};
  for (UINTN Ring = 0; Ring < ARRAY_SIZE (Contexts); ++Ring) {
    for (UINTN Field = 0; Field < ARRAY_SIZE (Fields); ++Field) {
      Reset ();
      CorruptContextWrite = Contexts[Ring] + Fields[Field];
      assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_DEVICE_ERROR);
      assert (!ChannelStarts && !DataDoorbells && !Completions);
      if (Ring == 0) assert (!EventDoorbells);
      assert (Releases == 1 && !mSeOwners[1][1].Busy && !mSeOwners[1][1].Poisoned);
    }
  }
  Reset ();
  Registers[GPI_GLOBAL_ENABLE (GPII_ID) / 4] = 0xa5a5000aU;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_SUCCESS);
  assert (Registers[GPI_GLOBAL_ENABLE (GPII_ID) / 4] == 0xa5a5000bU);
  puts ("GPI: rejected ring contexts stop before START/DMA; global enable preserves unrelated fields passed");
}

static void TestValidation (void) {
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Descs[] = {{&Buffer, 1, 5}, {&Buffer, 1, 7}};
  Reset ();
  Descs[1].Length = EFI_PAGE_SIZE + 1;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 2, NULL) == EFI_BAD_BUFFER_SIZE);
  Descs[1].Length = 1; Descs[1].Flags = 5;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 2, NULL) == EFI_INVALID_PARAMETER);
  Descs[1].Flags = 0x87;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, Descs, 2, NULL) == EFI_INVALID_PARAMETER);
  assert (Writes == 0 && Releases == 0);
}
static void TestFailures (void) {
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1, 7};
  for (UINT32 Fault = 0; Fault < 6; ++Fault) {
    Reset ();
    TransferTimeout = Fault <= 1;
    AbortTimeout = Fault == 1;
    StopTimeout = Fault == 2;
    WrongPointer = Fault == 3;
    WrongLength = Fault == 4;
    EventResetTimeout = Fault == 5;
    assert (EFI_ERROR (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL)));
    assert (Releases == 1 && mGpi.Data.Host == DmaData && mGpi.TxRing.Host == TxData);
    if (AbortTimeout || StopTimeout || EventResetTimeout) {
      assert (mSeOwners[1][1].Poisoned);
      UINT32 Before = Writes;
      assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_DEVICE_ERROR);
      assert (Before == Writes);
    } else {
      assert (Aborts == 1 && mSeOwners[1][1].Busy == 0);
    }
  }
}
static void TestActiveSe (void) {
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1, 7};
  Reset ();
  SeRegisters[SE_GENI_STATUS / 4] = BIT (0);
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_ACCESS_DENIED);
  assert (Writes == 0 && Aborts == 0 && Releases == 1);
  assert (SeRegisters[SE_GENI_STATUS / 4] == BIT (0));
}
static void TestControllerError (void) {
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1, 7};
  Reset ();
  InjectControllerError = TRUE;
  assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1, NULL) == EFI_DEVICE_ERROR);
  assert (Completions == 0 && mSeOwners[1][1].Poisoned);
}
static void TestFirmware (void) {
  UINT8 Buffer = 0;
  EFI_GPI_I2C_DESCRIPTOR Desc = {&Buffer, 1, 7};
  const UINT32 Protocols[] = {0, 1, 2, 4, 255};
  for (UINTN Index = 0; Index < ARRAY_SIZE (Protocols); ++Index) {
    UINT32 Transferred = 1;
    Reset ();
    SeRegisters[SE_GENI_FW_REVISION_RO / 4] = Protocols[Index] << 8;
    assert (GpiI2cTransfer (&mGpi.Protocol, 10, 0x2b, &Desc, 1,
        &Transferred) == EFI_UNSUPPORTED);
    assert (Transferred == 0 && Writes == 0 && Aborts == 0 && Releases == 1);
    assert (mSeOwners[1][1].Busy == 0);
  }
}

/* Exercise initialization without the transfer tests' Initialized shortcut.
   An IOMMU protocol alone cannot attach the GPI SID to a native SMMU domain. */
static EFI_QUP_CR_PROTOCOL InitQup;
static EFI_CLOCK_CR_PROTOCOL InitClock;
static EFI_GPIO_CR_PROTOCOL InitGpio;
static EFI_SMMU_CR_PROTOCOL InitSmmu;
static EDKII_IOMMU_PROTOCOL InitIommu;
static CR_TARGET_GPI_CONTROLLER InitController;
static CR_TARGET_QUP_WRAPPER InitWrapper;
static CR_TARGET_QUP_CONTEXT InitContext;
static UINT32 InitAttachCalls, InitAllocateCalls;
static BOOLEAN InitMissingSmmu;
static EFI_STATUS InitAttachStatus;
static EFI_STATUS InitDataAttachStatus;
static UINT8 InitDmaMemory[4][EFI_PAGE_SIZE];
static UINT32 InitAttributes, InitUnmappedMask, InitFreedMask;
static BOOLEAN InitFailPayloadAttribute;
static UINT32 InitClockRefs[3], InitClockAcquires, InitClockReleases;
static UINT32 InitClockFailAcquire = MAX_UINT32, InitClockFailRelease = MAX_UINT32;
static BOOLEAN InitClockRetainFailure;
static UINT32 InitClockIndex(CONST CHAR8 *Name) {
  if (!strcmp(Name,"gcc_qupv3_wrap_1_m_ahb_clk")) return 0;
  if (!strcmp(Name,"gcc_qupv3_wrap_1_s_ahb_clk")) return 1;
  assert(!strcmp(Name,"gcc_qupv3_wrap1_s1_clk"));return 2;
}
static void ResetInitClocks(void) {
  for (UINTN I=0;I<3;++I) InitClockRefs[I]=1; /* Independent foreign owners. */
  InitClockAcquires=InitClockReleases=0;
  InitClockFailAcquire=InitClockFailRelease=MAX_UINT32;
  InitClockRetainFailure=FALSE;
}

static UINT32 InitDmaIndex (VOID *Host) {
  for (UINT32 Index = 0; Index < ARRAY_SIZE (InitDmaMemory); ++Index)
    if (Host == InitDmaMemory[Index]) return Index;
  assert (FALSE);
  return MAX_UINT32;
}

static EFI_STATUS EFIAPI InitAllocateComplete (EDKII_IOMMU_PROTOCOL *This,
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    VOID **Host, UINT64 Attributes) {
  assert (This == &InitIommu && InitAttachCalls == 2);
  assert (Type == AllocateAnyPages && MemoryType == EfiBootServicesData);
  assert (Pages == 1 && Attributes == 0 && InitAllocateCalls < 4);
  *Host = InitDmaMemory[InitAllocateCalls++];
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InitMapComplete (EDKII_IOMMU_PROTOCOL *This,
    EDKII_IOMMU_OPERATION Operation, VOID *Host, UINTN *Bytes,
    EFI_PHYSICAL_ADDRESS *Device, VOID **Mapping) {
  assert (This == &InitIommu && *Bytes == EFI_PAGE_SIZE);
  assert (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64);
  *Device = 0x100000 + InitDmaIndex (Host) * EFI_PAGE_SIZE;
  *Mapping = Host;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InitAttributeComplete (EDKII_IOMMU_PROTOCOL *This,
    UINT64 Base, UINT32 Sid, VOID *Mapping, UINT64 Access) {
  UINT32 Index = InitDmaIndex (Mapping);
  assert (This == &InitIommu && Base == 0x15000000 && Index == InitAttributes++);
  /* Independent hardware identities: three GPI rings, one QUP payload. */
  assert (Sid == (Index < 3 ? 0x56U : 0x43U));
  assert (Access == (EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE));
  return Index == 3 && InitFailPayloadAttribute ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InitUnmapComplete (EDKII_IOMMU_PROTOCOL *This, VOID *Mapping) {
  UINT32 Mask = BIT (InitDmaIndex (Mapping));
  assert (This == &InitIommu && (InitUnmappedMask & Mask) == 0);
  InitUnmappedMask |= Mask;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InitFreeComplete (EDKII_IOMMU_PROTOCOL *This,
    UINTN Pages, VOID *Host) {
  UINT32 Mask = BIT (InitDmaIndex (Host));
  assert (This == &InitIommu && Pages == 1 && (InitUnmappedMask & Mask) != 0);
  assert ((InitFreedMask & Mask) == 0);
  InitFreedMask |= Mask;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI InitGetContext (EFI_QUP_CR_PROTOCOL *This,
    CONST CR_TARGET_QUP_CONTEXT **Context) {
  assert (This == &InitQup);
  *Context = &InitContext;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InitSetClock (EFI_CLOCK_CR_PROTOCOL *This,
    CONST CHAR8 *Controller, CONST CHAR8 *Name, UINT64 Rate, BOOLEAN Enable) {
  assert (This == &InitClock && !strcmp (Controller, "gcc") && Name != NULL);
  assert (!Enable && Rate == 0);
  UINT32 I=InitClockIndex(Name);++InitClockReleases;
  assert(InitClockRefs[I]>1);
  if(I==InitClockFailRelease) return EFI_TIMEOUT;
  --InitClockRefs[I];
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InitAcquireClock(EFI_CLOCK_CR_PROTOCOL *This,
  CONST CHAR8 *Controller, CONST CHAR8 *Name, UINT64 Rate, BOOLEAN *Owned) {
  assert(This==&InitClock && !strcmp(Controller,"gcc") && Owned!=NULL);
  UINT32 I=InitClockIndex(Name);assert(Rate==(I==2?19200000U:0U));++InitClockAcquires;
  *Owned=I!=InitClockFailAcquire || InitClockRetainFailure;
  if(*Owned) ++InitClockRefs[I];
  return I==InitClockFailAcquire ? EFI_TIMEOUT : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InitPins (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  assert (This == &InitGpio && (Config->PinNumber == 32 || Config->PinNumber == 33));
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI InitAttach (EFI_SMMU_CR_PROTOCOL *This,
    UINT16 Segment, UINT16 Bdf, UINT16 Sid) {
  assert (This == &InitSmmu && Segment == 0 && Bdf == 0xffff);
  assert (Sid == (InitAttachCalls == 0 ? 0x56 : 0x43));
  ++InitAttachCalls;
  return Sid == 0x56 ? InitAttachStatus : InitDataAttachStatus;
}
static EFI_STATUS EFIAPI InitAllocate (EDKII_IOMMU_PROTOCOL *This,
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    VOID **Host, UINT64 Attributes) {
  (VOID)Type; (VOID)MemoryType; (VOID)Pages; (VOID)Host; (VOID)Attributes;
  assert (This == &InitIommu && InitAttachCalls == 2);
  ++InitAllocateCalls;
  return EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI InitLocate (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  if (Guid == &gEfiQupCrProtocolGuid) *Interface = &InitQup;
  else if (Guid == &gEfiClockCrProtocolGuid) *Interface = &InitClock;
  else if (Guid == &gEfiGpioCrProtocolGuid) *Interface = &InitGpio;
  else if (Guid == &gEdkiiIoMmuProtocolGuid) *Interface = &InitIommu;
  else if (Guid == &gEfiSmmuCrProtocolGuid && !InitMissingSmmu) *Interface = &InitSmmu;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static void TestInitContracts (void) {
  for (UINTN Fault = 0; Fault < 12; ++Fault) {
    ZeroMem (&mGpi, sizeof (mGpi));
    ResetInitClocks();
    InitQup = (EFI_QUP_CR_PROTOCOL){.Revision = EFI_QUP_CR_PROTOCOL_REVISION,
                                  .GetContext = InitGetContext};
    InitClock = (EFI_CLOCK_CR_PROTOCOL){.Revision = EFI_CLOCK_CR_PROTOCOL_REVISION,
                                      .SetClock = InitSetClock,.AcquireClock=InitAcquireClock};
    InitGpio = (EFI_GPIO_CR_PROTOCOL){.ConfigGpio = InitPins};
    InitSmmu = (EFI_SMMU_CR_PROTOCOL){.Revision = EFI_SMMU_CR_PROTOCOL_REVISION,
                                    .Attach = InitAttach};
    InitIommu = DmaIommu;
    InitIommu.AllocateBuffer = InitAllocate;
    InitController = (CR_TARGET_GPI_CONTROLLER){.Id = 1, .WrapperId = 1,
      .Base = 0xa00000, .Size = 0x60000, .ChannelMask = 0x7e,
      .IommuStreamId = 0x56, .InterruptCount = 12};
    InitWrapper = TestWrapper;
    InitContext = (CR_TARGET_QUP_CONTEXT){.Qup = &InitWrapper, .QupCount = 1,
                                         .Gpi = &InitController, .GpiCount = 1};
    InitMissingSmmu = FALSE;
    InitAttachStatus = EFI_SUCCESS;
    InitDataAttachStatus = EFI_SUCCESS;
    InitAttachCalls = InitAllocateCalls = 0;
    BootServices.LocateProtocol = InitLocate;
    switch (Fault) {
      case 0: InitQup.GetContext = NULL; break;
      case 1: InitContext.Gpi = NULL; break;
      case 2: InitClock.SetClock = NULL; break;
      case 3: InitIommu.Revision = 0x10000; break;
      case 4: InitMissingSmmu = TRUE; break;
      case 5: InitSmmu.Revision = 0; break;
      case 6: InitSmmu.Attach = NULL; break;
      case 7: InitAttachStatus = EFI_DEVICE_ERROR; break;
      case 8: InitAttachStatus = EFI_ALREADY_STARTED; break;
      case 10: InitDataAttachStatus = EFI_DEVICE_ERROR; break;
      case 11: InitWrapper.IommuStreamId = 0; break;
      default: break;
    }
    EFI_STATUS Status = GpiInitialize (10);
    if (Fault == 1) assert (Status == EFI_NOT_FOUND);
    else if (Fault == 7 || Fault == 10) assert (Status == EFI_DEVICE_ERROR);
    else if (Fault == 11) assert (Status == EFI_COMPROMISED_DATA);
    else if (Fault >= 8) assert (Status == EFI_OUT_OF_RESOURCES);
    else assert (Status == EFI_NOT_READY);
    assert (InitAttachCalls == (Fault == 7 ? 1U : (Fault >= 8 && Fault <= 10 ? 2U : 0U)));
    assert (InitAllocateCalls == (Fault == 8 || Fault == 9 ? 1U : 0U));
    assert (!mGpi.Initialized && mGpi.TxRing.Host == NULL);
    if(Fault==4) {
      UINT32 Acquires=InitClockAcquires;
      assert(GpiInitialize(10)==EFI_NOT_READY && InitClockAcquires==Acquires);
      assert(mGpi.ClockMask==7 && !mGpi.ClockCleanupPending);
    }
  }
  InitWrapper = TestWrapper;
  InitIommu.AllocateBuffer = InitAllocateComplete;
  InitIommu.Map = InitMapComplete;
  InitIommu.SetAttributeById = InitAttributeComplete;
  InitIommu.Unmap = InitUnmapComplete;
  InitIommu.FreeBuffer = InitFreeComplete;
  for (UINT32 Fail = 0; Fail < 2; ++Fail) {
    ZeroMem (&mGpi, sizeof (mGpi));
    ResetInitClocks();
    InitAttachCalls = InitAllocateCalls = InitAttributes = 0;
    InitUnmappedMask = InitFreedMask = 0;
    InitFailPayloadAttribute = Fail != 0;
    EFI_STATUS Status = GpiInitialize (10);
    assert (InitAttachCalls == 2 && InitAllocateCalls == 4 && InitAttributes == 4);
    if (Fail) {
      assert (Status == EFI_DEVICE_ERROR && !mGpi.Initialized);
      assert (mGpi.TxRing.Host == NULL && mGpi.RxRing.Host == NULL);
      assert (mGpi.EventRing.Host == NULL && mGpi.Data.Host == NULL);
    } else {
      assert (Status == EFI_SUCCESS && mGpi.Initialized);
      assert (mGpi.TxRing.Host == InitDmaMemory[0]);
      assert (mGpi.RxRing.Host == InitDmaMemory[1]);
      assert (mGpi.EventRing.Host == InitDmaMemory[2]);
      assert (mGpi.Data.Host == InitDmaMemory[3]);
      GpiDmaFree (&mGpi.Data);
      GpiRingFree (&mGpi.EventRing);
      GpiRingFree (&mGpi.RxRing);
      GpiRingFree (&mGpi.TxRing);
    }
    assert (InitUnmappedMask == 15 && InitFreedMask == 15 && !mGpi.DmaCleanupFailed);
  }
  puts ("GPI init: native contracts and both GPI ring/QUP payload streams attached before DMA passed");
  puts ("GPI DMA: distinct ring/payload authorization and payload-permission rollback passed");
}
static void TestInitClockOwnership(void) {
  ZeroMem(&mGpi,sizeof(mGpi));ResetInitClocks();
  InitClock=(EFI_CLOCK_CR_PROTOCOL){.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,
    .SetClock=InitSetClock,.AcquireClock=InitAcquireClock};
  BootServices.LocateProtocol=InitLocate;
  InitClockFailAcquire=InitClockFailRelease=2;InitClockRetainFailure=TRUE;
  assert(GpiPrepareClocks()==EFI_TIMEOUT && mGpi.ClockMask==7 && mGpi.ClockCleanupPending);
  assert(InitClockAcquires==3 && InitClockReleases==1);
  assert(GpiPrepareClocks()==EFI_TIMEOUT && InitClockAcquires==3);
  InitClockFailRelease=1;
  assert(GpiPrepareClocks()==EFI_TIMEOUT && mGpi.ClockMask==3 && InitClockAcquires==3);
  assert(InitClockRefs[2]==1 && InitClockRefs[1]==2 && InitClockRefs[0]==2);
  InitClockFailAcquire=InitClockFailRelease=MAX_UINT32;
  assert(GpiPrepareClocks()==EFI_SUCCESS && mGpi.ClockMask==7 && !mGpi.ClockCleanupPending);
  assert(InitClockAcquires==6);
  UINT32 Acquires=InitClockAcquires;
  assert(GpiPrepareClocks()==EFI_SUCCESS && InitClockAcquires==Acquires);
  for(UINTN I=0;I<3;++I) assert(InitClockRefs[I]==2);
  for(UINTN State=0;State<3;++State) {
    UINT32 ReleasesBefore=InitClockReleases;
    mGpi.Initialized=State==0;mGpi.ChannelTouched[0]=State==1;mGpi.DmaCleanupFailed=State==2;
    assert(GpiReleaseClocks()==EFI_NOT_READY && InitClockReleases==ReleasesBefore && mGpi.ClockMask==7);
  }
  mGpi.Initialized=mGpi.ChannelTouched[0]=mGpi.DmaCleanupFailed=FALSE;
  assert(GpiReleaseClocks()==EFI_SUCCESS && mGpi.ClockMask==0);
  for(UINTN I=0;I<3;++I) assert(InitClockRefs[I]==1);
  InitClockFailAcquire=1;InitClockRetainFailure=FALSE;
  assert(GpiPrepareClocks()==EFI_TIMEOUT && !mGpi.ClockMask && !mGpi.ClockCleanupPending);
  for(UINTN I=0;I<3;++I) assert(InitClockRefs[I]==1);
  ResetInitClocks();
  puts("GPI clocks: retained acquire, reverse cleanup retry, foreign votes and live DMA protection passed");
}
int main (void) {
  TestInitClockOwnership();
  TestDmaLifetime ();
  TestPinmux ();
  TestOwnership ();
  TestTransfers ();
  TestReadCacheOrdering ();
  TestSeDmaInterface ();
  TestDmaDiagnostics ();
  TestRingContextReadback ();
  TestValidation ();
  TestFailures ();
  TestActiveSe ();
  TestControllerError ();
  TestFirmware ();
  TestProtocolErrors ();
  TestInitContracts ();
  puts ("GPI: continuous rings/wrap, RX-only completion, event identity, validation and quiescence passed");
  return 0;
}
