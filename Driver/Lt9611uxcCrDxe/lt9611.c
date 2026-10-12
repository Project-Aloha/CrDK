#include <Library/DebugLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/MdssCrLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Uefi.h>
#include <Guid/EventGroup.h>

#include <Protocol/EFII2cCrProtocol.h>
#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFILt9611CrProtocol.h>
#include <Protocol/EFIMdssHandoffProtocol.h>
#include "Lt9611PanelOwner.h"

#define LT9611_I2C_FREQUENCY_KHZ  400U
#define LT9611_I2C_STRETCH_US     500U
#define LT9611_IRQ_POLL_100NS     100000U

STATIC EFI_LT9611_CR_PROTOCOL mLt9611Bridge;
STATIC EFI_EVENT mIrqTimer;
STATIC EFI_EVENT mExitBootEvent;
STATIC EFI_EVENT mI2cTestEvent;
STATIC volatile BOOLEAN mIrqPending;
STATIC volatile BOOLEAN mBootServicesLive;
STATIC BOOLEAN mMcuBusy;
STATIC UINT8 mLastHpdStatus;
STATIC volatile BOOLEAN mHandoffFrozen;
STATIC BOOLEAN mHandoffRegistered;
STATIC BOOLEAN mStartupBusy;
STATIC BOOLEAN mResidentRetained;
STATIC BOOLEAN Lt9611SelectedPanel(VOID){return PcdGetBool(PcdMdssHdkExtendedProfileEnable)||PcdGet32(PcdMdssPanelProfile)==2;}
STATIC BOOLEAN mVideoTouched;
STATIC CR_MDSS_NATIVE_BRIDGE_STATE mNativeBridge;
STATIC LT9611_PANEL_OWNER mPanelOwner;

STATIC VOID Lt9611RetainFailure (EFI_STATUS Status)
{
  if (mHandoffRegistered && Status != EFI_SUCCESS) {
    mNativeBridge.State = CR_MDSS_NATIVE_UNCERTAIN;
    mNativeBridge.Flags |= CR_MDSS_NATIVE_RETAINED_ERROR;
  }
}

/* Registration precedes GPIO changes. Its permanent callback is also the
   partial-start journal, so an entry error must not unload this image after
   the coordinator retains it. No bridge protocol is published on failure. */
STATIC EFI_STATUS Lt9611StartupResult (EFI_STATUS Status)
{
  mStartupBusy = FALSE;
  if (mHandoffFrozen && Status == EFI_SUCCESS) Status = EFI_NOT_READY;
  if (Status == EFI_SUCCESS) return Status;
  if (!EFI_ERROR (Status)) Status = EFI_NOT_READY;
  Lt9611RetainFailure (Status);
  if(mResidentRetained)DEBUG((DEBUG_ERROR,"LT9611: selected bridge startup %r; retaining owner\n",Status));
  return (mHandoffRegistered||mResidentRetained) ? EFI_SUCCESS : Status;
}

STATIC EFI_STATUS EFIAPI Lt9611HandoffFreeze (VOID *Context)
{
  BOOLEAN InterruptState;
  BOOLEAN Busy;
  if (Context != &mLt9611Bridge) return EFI_INVALID_PARAMETER;
  /* GPIO delivers on the boot CPU. Make the ISR flag gate and pending
     retirement atomic with respect to that raw interrupt; no GPIO/MCU,
     boot-service event or allocation lifetime is changed here. */
  InterruptState = SaveAndDisableInterrupts ();
  mHandoffFrozen = TRUE;
  mBootServicesLive = FALSE;
  mIrqPending = FALSE;
  MemoryFence ();
  Busy = mMcuBusy || mStartupBusy;
  SetInterruptState (InterruptState);
  return Busy ? EFI_NOT_READY : EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Lt9611HandoffSnapshot (VOID *Context,
  CR_MDSS_HANDOFF_PARTICIPANT *Snapshot, VOID **Map)
{
  if (Context != &mLt9611Bridge || Snapshot == NULL || Map == NULL)
    return EFI_INVALID_PARAMETER;
  if (!mHandoffRegistered || !mHandoffFrozen || mMcuBusy || mStartupBusy)
    return EFI_NOT_READY;
  ZeroMem (Snapshot, sizeof (*Snapshot));
  Snapshot->Owner = CR_MDSS_NATIVE_BRIDGE;
  Snapshot->Bridge = mNativeBridge;
  *Map = NULL;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Lt9611RegisterHandoff (CONST CR_DAL_LT9611_CONFIG *Config)
{
  EFI_MDSS_HANDOFF_PROTOCOL *Handoff = NULL;
  EFI_STATUS Status;
  if (!PcdGetBool (PcdMdssHandoffEnable)) return EFI_SUCCESS;
  if (mHandoffRegistered || mHandoffFrozen) return EFI_ALREADY_STARTED;
  /* The target supplies the exact physical routing. Preserve every wire
     identity in the journal rather than substituting a platform default. */
  if (Config->I2cInstance == 0 || Config->I2cAddress != 0x2b ||
      Config->PowerPinCount != 3 || Config->PowerPins == NULL)
    return EFI_UNSUPPORTED;
  Status = gBS->LocateProtocol (&gEfiMdssHandoffProtocolGuid, NULL,
                                (VOID **)&Handoff);
  if (Status != EFI_SUCCESS) return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  if (Handoff == NULL || Handoff->Revision < EFI_MDSS_HANDOFF_PROTOCOL_REVISION ||
      Handoff->SetParticipant == NULL) return EFI_UNSUPPORTED;
  ZeroMem (&mNativeBridge, sizeof (mNativeBridge));
  mNativeBridge.State = CR_MDSS_NATIVE_STOPPED;
  mNativeBridge.I2cInstance = Config->I2cInstance;
  mNativeBridge.I2cAddress = Config->I2cAddress;
  mNativeBridge.I2cRateHz = LT9611_I2C_FREQUENCY_KHZ * 1000U;
  mNativeBridge.StretchUs = LT9611_I2C_STRETCH_US;
  mNativeBridge.PowerGpio0 = Config->PowerPins[0];
  mNativeBridge.PowerGpio1 = Config->PowerPins[1];
  mNativeBridge.ResetGpio = Config->PowerPins[2];
  mNativeBridge.IrqGpio = Config->InterruptPin;
  Status = Handoff->SetParticipant (Handoff, CR_MDSS_NATIVE_BRIDGE,
                  &mLt9611Bridge, Lt9611HandoffFreeze, Lt9611HandoffSnapshot);
  if (Status != EFI_SUCCESS) return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  mHandoffRegistered = TRUE;
  return EFI_SUCCESS;
}

/* GPIO callbacks run in the raw GIC ISR, without a raised UEFI TPL. Never
   SignalEvent there: it could dispatch its callback before the ISR returns. */
STATIC BOOLEAN Lt9611AcquireMcu (VOID)
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  BOOLEAN Acquired = !mMcuBusy && !mHandoffFrozen;
  if (Acquired) mMcuBusy = TRUE;
  gBS->RestoreTPL (OldTpl);
  return Acquired;
}
STATIC VOID Lt9611ReleaseMcu (VOID)
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mMcuBusy = FALSE;
  gBS->RestoreTPL (OldTpl);
}

typedef struct {
  UINT8 Register;
  UINT8 Value;
} LT9611_REG_VALUE;

STATIC EFI_STATUS
Lt9611FindI2c (
  IN  UINT32                Instance,
  OUT EFI_I2C_CR_PROTOCOL **I2c
  )
{
  EFI_HANDLE *Handles;
  EFI_STATUS  Status;
  UINTN       HandleCount;
  UINTN       Index;

  if (I2c == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *I2c = NULL;
  Handles = NULL;
  HandleCount = 0;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol, &gEfiI2cCrProtocolGuid, NULL,
                  &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if ((Handles == NULL) || (HandleCount == 0)) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    EFI_I2C_CR_PROTOCOL *Candidate;

    Candidate = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index], &gEfiI2cCrProtocolGuid,
                    (VOID **)&Candidate);
    if (!EFI_ERROR (Status) && (Candidate != NULL) &&
        (Candidate->Revision >= EFI_I2C_CR_PROTOCOL_REVISION) &&
        (Candidate->MuTransfer != NULL) &&
        (Candidate->Instance == Instance)) {
      *I2c = Candidate;
      break;
    }
  }
  FreePool (Handles);
  return (*I2c == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC EFI_STATUS
Lt9611WriteRegister (
  IN EFI_I2C_CR_PROTOCOL *I2c,
  IN MU_I2C_SLAVE_CONFIG *Config,
  IN UINT8                Register,
  IN UINT8                Value
  )
{
  UINT8             Buffer[2] = { Register, Value };
  MU_I2C_DESCRIPTOR Descriptor = {
    Buffer, sizeof (Buffer),
    MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE | MU_I2C_FLAG_STOP
  };

  if (mHandoffFrozen) return EFI_NOT_READY;
  return I2c->MuTransfer (I2c, Config, &Descriptor, 1, NULL, NULL, 0, NULL);
}

STATIC EFI_STATUS
Lt9611ReadRegister (
  IN  EFI_I2C_CR_PROTOCOL *I2c,
  IN  MU_I2C_SLAVE_CONFIG *Config,
  IN  UINT8                Register,
  OUT UINT8               *Value
  )
{
  MU_I2C_DESCRIPTOR Descriptors[2] = {
    { &Register, 1, MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE },
    { Value, 1, MU_I2C_FLAG_START | MU_I2C_FLAG_READ | MU_I2C_FLAG_STOP }
  };

  if (mHandoffFrozen) return EFI_NOT_READY;
  return I2c->MuTransfer (I2c, Config, Descriptors, 2, NULL, NULL, 0, NULL);
}

STATIC EFI_STATUS
Lt9611ReadChipId (
  IN  EFI_I2C_CR_PROTOCOL *I2c,
  IN  UINT8                Address,
  OUT UINT8                ChipId[3]
  )
{
  MU_I2C_SLAVE_CONFIG Config = {
    .BusFrequencyKHz = LT9611_I2C_FREQUENCY_KHZ,
    .SlaveAddress = Address,
    .Mode = MU_I2C_MODE_I2C,
    .SlaveMaxClockStretchUs = LT9611_I2C_STRETCH_US,
  };
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  UINTN      Index;

  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Lt9611WriteRegister (I2c, &Config, 0xee, 0x01);
  if (EFI_ERROR (Status)) {
    /* MU may report an error after the device has accepted the lock write. */
    goto UnlockMcu;
  }

  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0x81);
  for (Index = 0; !EFI_ERROR (Status) && (Index < 3); Index++) {
    Status = Lt9611ReadRegister (I2c, &Config, (UINT8)Index, &ChipId[Index]);
  }

UnlockMcu:
  CleanupStatus = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (!EFI_ERROR (CleanupStatus)) {
    /* Do not write 0xee in an unknown bank after a failed page select. */
    CleanupStatus = Lt9611WriteRegister (I2c, &Config, 0xee, 0x00);
  }
  if (EFI_ERROR (CleanupStatus)) {
    DEBUG ((DEBUG_ERROR, "LT9611: MCU unlock failed: %r\n", CleanupStatus));
  } else {
    MicroSecondDelay (50000);
  }
  return EFI_ERROR (Status) ? Status : CleanupStatus;
}

STATIC EFI_STATUS
Lt9611GpiWriteRegister (
  IN EFI_GPI_CR_PROTOCOL *Gpi,
  IN UINT32               Instance,
  IN UINT8                Address,
  IN UINT8                Register,
  IN UINT8                Value
  )
{
  UINT8 Buffer[2] = { Register, Value };
  EFI_GPI_I2C_DESCRIPTOR Descriptor = {
    Buffer, sizeof (Buffer), EFI_GPI_I2C_FLAG_START |
      EFI_GPI_I2C_FLAG_WRITE | EFI_GPI_I2C_FLAG_STOP
  };

  if (mHandoffFrozen) return EFI_NOT_READY;
  return Gpi->I2cTransfer (Gpi, Instance, Address, &Descriptor, 1, NULL);
}

STATIC EFI_STATUS
Lt9611GpiReadRegister (
  IN  EFI_GPI_CR_PROTOCOL *Gpi,
  IN  UINT32               Instance,
  IN  UINT8                Address,
  IN  UINT8                Register,
  OUT UINT8               *Value
  )
{
  EFI_GPI_I2C_DESCRIPTOR Descriptors[2];

  Descriptors[0].Buffer = &Register;
  Descriptors[0].Length = 1;
  Descriptors[0].Flags  = EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_WRITE;
  Descriptors[1].Buffer = Value;
  Descriptors[1].Length = 1;
  Descriptors[1].Flags  = EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_READ |
                          EFI_GPI_I2C_FLAG_STOP;
  if (mHandoffFrozen) return EFI_NOT_READY;
  return Gpi->I2cTransfer (
      Gpi, Instance, Address, Descriptors, ARRAY_SIZE (Descriptors), NULL);
}

STATIC EFI_STATUS
Lt9611ReadChipIdGpi (
  IN  CONST CR_DAL_LT9611_CONFIG *Config,
  OUT UINT8                       ChipId[3]
  )
{
  EFI_GPI_CR_PROTOCOL *Gpi;
  EFI_STATUS Status;
  UINTN Index;

  Gpi = NULL;
  Status = gBS->LocateProtocol (&gEfiGpiCrProtocolGuid, NULL, (VOID **)&Gpi);
  if (EFI_ERROR (Status) || (Gpi == NULL) ||
      (Gpi->Revision < EFI_GPI_CR_PROTOCOL_REVISION) ||
      (Gpi->I2cTransfer == NULL)) {
    return EFI_NOT_READY;
  }

  Status = Lt9611GpiWriteRegister (
      Gpi, Config->I2cInstance, Config->I2cAddress, 0xff, 0x80);
  if (EFI_ERROR (Status)) return Status;
  Status = Lt9611GpiWriteRegister (
      Gpi, Config->I2cInstance, Config->I2cAddress, 0xee, 0x01);
  if (EFI_ERROR (Status)) goto Unlock;
  Status = Lt9611GpiWriteRegister (
      Gpi, Config->I2cInstance, Config->I2cAddress, 0xff, 0x81);
  for (Index = 0; !EFI_ERROR (Status) && (Index < 3); ++Index) {
    Status = Lt9611GpiReadRegister (
        Gpi, Config->I2cInstance, Config->I2cAddress,
        (UINT8)Index, &ChipId[Index]);
  }

Unlock:
  {
    EFI_STATUS CleanupStatus;
    CleanupStatus = Lt9611GpiWriteRegister (
        Gpi, Config->I2cInstance, Config->I2cAddress, 0xff, 0x80);
    if (!EFI_ERROR (CleanupStatus)) {
      CleanupStatus = Lt9611GpiWriteRegister (
          Gpi, Config->I2cInstance, Config->I2cAddress, 0xee, 0x00);
    }
    if (EFI_ERROR (CleanupStatus)) {
      DEBUG ((DEBUG_ERROR, "LT9611: GPI MCU unlock failed: %r\n", CleanupStatus));
    } else {
      /* Linux lt9611uxc_unlock() lets the on-chip MCU resume for 50 ms. */
      MicroSecondDelay (50000);
    }
    if (!EFI_ERROR (Status)) Status = CleanupStatus;
  }
  return Status;
}

STATIC EFI_STATUS
Lt9611TestFifo (
  IN CONST CR_DAL_LT9611_CONFIG *Config,
  OUT UINT8                    ChipId[3]
  )
{
  EFI_I2C_CR_PROTOCOL *I2c;
  EFI_STATUS           Status;

  Status = Lt9611FindI2c (Config->I2cInstance, &I2c);
  if (!EFI_ERROR (Status)) {
    Status = Lt9611ReadChipId (I2c, Config->I2cAddress, ChipId);
  }
  /* Linux lontium-lt9611uxc.c reads/logs these three revision bytes without
     a silicon whitelist. The board configuration selects this device;
     matching revisions below establish transport consistency, not identity. */
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
          "LT9611 I2C FIFO test: %r (instance %u, address 0x%02x, ID %02x.%02x.%02x)\n",
          Status, Config->I2cInstance, Config->I2cAddress,
          ChipId[0], ChipId[1], ChipId[2]));
  return Status;
}

STATIC EFI_STATUS
Lt9611TestI2c (
  IN CONST CR_DAL_LT9611_CONFIG *Config
  )
{
  EFI_I2C_CR_PROTOCOL *I2c;
  EFI_STATUS           Status;
  EFI_STATUS           GpiStatus;
  UINT8                ChipId[3] = { 0 };
  UINT8                GpiChipId[3] = { 0 };
  UINT8                RestoredChipId[3] = { 0 };

  Status = Lt9611TestFifo (Config, ChipId);
  if (EFI_ERROR (Status)) return Status;

  CONST CR_TARGET_QUP_CONTEXT *Qup=CrDalGetQupContext();
  /* A FIFO-only target advertises no native GPI owner. Do not invent a DMA
     endpoint or wait on an unrelated GPI protocol to use its physical bus. */
  if(Qup&&Qup->GpiCount==0){
    DEBUG((DEBUG_INIT,"LT9611: target FIFO self-test complete\n"));
    return EFI_SUCCESS;
  }
  GpiStatus = Lt9611ReadChipIdGpi (Config, GpiChipId);
  if (!EFI_ERROR (GpiStatus) &&
      ((GpiChipId[0] != ChipId[0]) || (GpiChipId[1] != ChipId[1]) ||
       (GpiChipId[2] != ChipId[2]))) {
    GpiStatus = EFI_DEVICE_ERROR;
  }
  DEBUG ((EFI_ERROR (GpiStatus) ? DEBUG_ERROR : DEBUG_INIT,
          "LT9611 I2C GPI test: %r (instance %u, address 0x%02x, ID %02x.%02x.%02x)\n",
          GpiStatus, Config->I2cInstance, Config->I2cAddress,
          GpiChipId[0], GpiChipId[1], GpiChipId[2]));
  if (EFI_ERROR (GpiStatus)) return GpiStatus;

  /* Confirm that releasing GPI ownership restored a usable FIFO path. Do not
     touch FIFO after a failed GPI transaction: the SE may remain fenced. */
  Status = Lt9611FindI2c (Config->I2cInstance, &I2c);
  if (EFI_ERROR (Status)) return Status;
  Status = Lt9611ReadChipId (I2c, Config->I2cAddress, RestoredChipId);
  if (!EFI_ERROR (Status) &&
      ((RestoredChipId[0] != ChipId[0]) ||
       (RestoredChipId[1] != ChipId[1]) ||
       (RestoredChipId[2] != ChipId[2]))) {
    Status = EFI_DEVICE_ERROR;
  }
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
          "LT9611 I2C FIFO restore test: %r (instance %u, address 0x%02x, ID %02x.%02x.%02x)\n",
          Status, Config->I2cInstance, Config->I2cAddress,
          RestoredChipId[0], RestoredChipId[1], RestoredChipId[2]));
  return Status;
}

STATIC VOID EFIAPI
Lt9611TestI2cReadyToBoot (EFI_EVENT Event, VOID *Context)
{
  CONST CR_DAL_LT9611_CONFIG *Config;
  EFI_STATUS Status;
  (VOID)Context;

  /* ReadyToBoot may be signalled for several boot options. Consume it once,
     and close it before touching hardware or releasing the MCU lock. */
  if (!mBootServicesLive || mHandoffFrozen || Event != mI2cTestEvent || Event == NULL) return;
  gBS->CloseEvent (mI2cTestEvent);
  mI2cTestEvent = NULL;
  /* MDSS may retain reset LOW after a failed bring-up. This optional
     diagnostic must obey the same power/reset fence as the IRQ worker;
     consuming the event does not grant permission to access the MCU. */
  if (mPanelOwner.Initialized &&
      (!mPanelOwner.Powered || mPanelOwner.ResetHeld)) {
    DEBUG ((DEBUG_INIT, "LT9611: skipping self-test while panel reset/power is fenced\n"));
    return;
  }
  DEBUG ((DEBUG_INIT, "LT9611: ReadyToBoot FIFO/GPI/FIFO self-test begin\n"));
  Config = CrDalGetLt9611Config ();
  if (Config == NULL) {
    Status = EFI_NOT_FOUND;
  } else if (!Lt9611AcquireMcu ()) {
    Status = EFI_NOT_READY;
  } else {
    /* Same TPL_CALLBACK as the IRQ worker: no banked I2C access can nest in
       this sequence. Native QUP still arbitrates exclusive FIFO/GPI mode. */
    Status = Lt9611TestI2c (Config);
    Lt9611RetainFailure (Status);
    Lt9611ReleaseMcu ();
  }
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
          "LT9611: ReadyToBoot FIFO/GPI/FIFO self-test: %r\n", Status));
}

/* Linux lt9611uxc_read_version(): this bridge is programmed by its on-chip
   MCU. Consistent revision reads do not establish that usable firmware is running.
   Firmware updating is deliberately outside this native driver's contract. */
STATIC EFI_STATUS
Lt9611CheckFirmware (IN CONST CR_DAL_LT9611_CONFIG *Bridge)
{
  EFI_I2C_CR_PROTOCOL *I2c;
  MU_I2C_SLAVE_CONFIG Config = {
    .BusFrequencyKHz = LT9611_I2C_FREQUENCY_KHZ,
    .SlaveAddress = Bridge->I2cAddress,
    .Mode = MU_I2C_MODE_I2C,
    .SlaveMaxClockStretchUs = LT9611_I2C_STRETCH_US,
  };
  EFI_STATUS Status, Cleanup;
  UINT8 Version = 0;
  DEBUG ((DEBUG_INIT, "LT9611: MCU firmware check begin\n"));
  Status = Lt9611FindI2c (Bridge->I2cInstance, &I2c);
  if (EFI_ERROR (Status)) return Status;
  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (EFI_ERROR (Status)) return Status;
  Status = Lt9611WriteRegister (I2c, &Config, 0xee, 1);
  if (!EFI_ERROR (Status)) Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0xb0);
  if (!EFI_ERROR (Status)) Status = Lt9611ReadRegister (I2c, &Config, 0x21, &Version);
  Cleanup = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (!EFI_ERROR (Cleanup)) Cleanup = Lt9611WriteRegister (I2c, &Config, 0xee, 0);
  if (!EFI_ERROR (Cleanup)) MicroSecondDelay (50000);
  if (EFI_ERROR (Status)) return Status;
  if (EFI_ERROR (Cleanup)) return Cleanup;
  DEBUG ((DEBUG_INIT, "LT9611: MCU firmware version 0x%02x\n", Version));
  if (Version == 0) {
    DEBUG ((DEBUG_ERROR, "LT9611: firmware version zero; MCU video setup unavailable\n"));
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
Lt9611WriteLowNibble (
  IN EFI_I2C_CR_PROTOCOL *I2c,
  IN MU_I2C_SLAVE_CONFIG *Config,
  IN UINT8 Register,
  IN UINT8 Value
  )
{
  EFI_STATUS Status;
  UINT8 Current;

  Status = Lt9611ReadRegister (I2c, Config, Register, &Current);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  return Lt9611WriteRegister (I2c, Config, Register,
                              (Current & 0xf0U) | (Value & 0x0fU));
}

STATIC EFI_STATUS EFIAPI
Lt9611ConfigureVideoInternal (
  IN EFI_LT9611_CR_PROTOCOL *This,
  IN CONST EFI_MDP_DSI_TIMING *Timing
  )
{
  EFI_I2C_CR_PROTOCOL *I2c;
  CONST CR_DAL_LT9611_CONFIG *Bridge;
  MU_I2C_SLAVE_CONFIG Config;
  EFI_STATUS Status;
  EFI_STATUS CleanupStatus;
  UINT64 HTotal;
  UINT64 VTotal;
  UINTN Index;
  LT9611_REG_VALUE Registers[9];

  if (This != &mLt9611Bridge || Timing == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  /* Linux's LT9611UXC mode table includes this board's fixed XML mode. */
  if (Timing->Width != 1920 || Timing->Height != 1080 ||
      Timing->DsiRefreshRate != (60U << 16) || Timing->DsiLanes != 4 ||
      Timing->DsiController != 0 || Timing->HSync > 0xfff ||
      Timing->VSync > 0xff || Timing->HFrontPorch > 0xfff ||
      Timing->VFrontPorch > 0xfff) {
    return EFI_UNSUPPORTED;
  }
  HTotal = (UINT64)Timing->Width + Timing->HSync + Timing->HBackPorch +
           Timing->HFrontPorch;
  VTotal = (UINT64)Timing->Height + Timing->VSync + Timing->VBackPorch +
           Timing->VFrontPorch;
  if (HTotal > 0xffff || VTotal > 0xffff) {
    return EFI_UNSUPPORTED;
  }

  Bridge = CrDalGetLt9611Config ();
  if (Bridge == NULL || Bridge->I2cAddress > 0x7f) {
    return EFI_NOT_FOUND;
  }
  Status = Lt9611FindI2c (Bridge->I2cInstance, &I2c);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Config = (MU_I2C_SLAVE_CONFIG) {
    .BusFrequencyKHz = LT9611_I2C_FREQUENCY_KHZ,
    .SlaveAddress = Bridge->I2cAddress,
    .Mode = MU_I2C_MODE_I2C,
    .SlaveMaxClockStretchUs = LT9611_I2C_STRETCH_US,
  };
  /* Linux lt9611uxc_video_setup() uses page D0 and preserves the upper
     nibbles of the three packed porch/sync registers. */
  Registers[0] = (LT9611_REG_VALUE) { 0x0d, (UINT8)(VTotal >> 8) };
  Registers[1] = (LT9611_REG_VALUE) { 0x0e, (UINT8)VTotal };
  Registers[2] = (LT9611_REG_VALUE) { 0x0f, (UINT8)(Timing->Height >> 8) };
  Registers[3] = (LT9611_REG_VALUE) { 0x10, (UINT8)Timing->Height };
  Registers[4] = (LT9611_REG_VALUE) { 0x11, (UINT8)(HTotal >> 8) };
  Registers[5] = (LT9611_REG_VALUE) { 0x12, (UINT8)HTotal };
  Registers[6] = (LT9611_REG_VALUE) { 0x13, (UINT8)(Timing->Width >> 8) };
  Registers[7] = (LT9611_REG_VALUE) { 0x14, (UINT8)Timing->Width };
  Registers[8] = (LT9611_REG_VALUE) { 0x15, (UINT8)Timing->VSync };

  mVideoTouched = TRUE;
  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = Lt9611WriteRegister (I2c, &Config, 0xee, 0x01);
  if (EFI_ERROR (Status)) {
    goto Unlock;
  }
  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0xd0);
  for (Index = 0; !EFI_ERROR (Status) && Index < ARRAY_SIZE (Registers); Index++) {
    Status = Lt9611WriteRegister (I2c, &Config,
                                  Registers[Index].Register, Registers[Index].Value);
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteLowNibble (I2c, &Config, 0x16, (UINT8)(Timing->HSync >> 8));
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteRegister (I2c, &Config, 0x17, (UINT8)Timing->HSync);
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteLowNibble (I2c, &Config, 0x18, (UINT8)(Timing->VFrontPorch >> 8));
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteRegister (I2c, &Config, 0x19, (UINT8)Timing->VFrontPorch);
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteLowNibble (I2c, &Config, 0x1a, (UINT8)(Timing->HFrontPorch >> 8));
  }
  if (!EFI_ERROR (Status)) {
    Status = Lt9611WriteRegister (I2c, &Config, 0x1b, (UINT8)Timing->HFrontPorch);
  }

Unlock:
  CleanupStatus = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (!EFI_ERROR (CleanupStatus)) {
    CleanupStatus = Lt9611WriteRegister (I2c, &Config, 0xee, 0x00);
  }
  if (!EFI_ERROR (CleanupStatus)) {
    MicroSecondDelay (50000);
  }
  return EFI_ERROR (Status) ? Status : CleanupStatus;
}

STATIC EFI_STATUS EFIAPI
Lt9611ConfigureVideo (
  IN EFI_LT9611_CR_PROTOCOL *This,
  IN CONST EFI_MDP_DSI_TIMING *Timing
  )
{
  EFI_STATUS Status;
  if (!mBootServicesLive || mHandoffFrozen) return EFI_NOT_READY;
  if (mPanelOwner.Initialized && mPanelOwner.Owner != NULL) return EFI_ACCESS_DENIED;
  /* Release clears Owner but intentionally leaves reset asserted. The
     legacy interface must not bypass that physical-owner state. */
  if (mPanelOwner.Initialized &&
      (!mPanelOwner.Powered || mPanelOwner.ResetHeld)) return EFI_NOT_READY;
  if (!Lt9611AcquireMcu ()) return EFI_NOT_READY;
  mVideoTouched = FALSE;
  Status = Lt9611ConfigureVideoInternal (This, Timing);
  if (mHandoffFrozen && Status == EFI_SUCCESS) Status = EFI_NOT_READY;
  if (mHandoffRegistered) {
    if (Status == EFI_SUCCESS && mNativeBridge.HighPins == 7 &&
        !(mNativeBridge.Flags & CR_MDSS_NATIVE_RETAINED_ERROR))
      mNativeBridge.State = CR_MDSS_NATIVE_LIVE;
    else if (mVideoTouched) Lt9611RetainFailure (Status);
  }
  Lt9611ReleaseMcu ();
  return Status;
}

/* The opt-in profile reuses this exact resident bridge's GPIO/FIFO/MCU
   owner, rather than creating a second writer to its board power pins. */
STATIC EFI_STATUS LtPanelReady(VOID *Context)
{
  if (Context != &mLt9611Bridge || !mBootServicesLive || mHandoffFrozen)
    return EFI_NOT_READY;
  return EFI_SUCCESS;
}
STATIC BOOLEAN LtPanelLock(VOID *Context)
{
  return Context == &mLt9611Bridge && Lt9611AcquireMcu();
}
STATIC VOID LtPanelUnlock(VOID *Context)
{
  if (Context == &mLt9611Bridge) Lt9611ReleaseMcu();
}
STATIC VOID LtPanelObserve(VOID *Context,UINT32 Pin,BOOLEAN High,EFI_STATUS Status)
{
  if (Context != &mLt9611Bridge) return;
  if (mBootServicesLive && !mHandoffFrozen && Pin < 3 && mPanelOwner.Gpio) {
    UINT8 Input = 0xff;
    EFI_STATUS Sample = mPanelOwner.Gpio->ReadIoValue ?
      mPanelOwner.Gpio->ReadIoValue(mPanelOwner.Gpio,mPanelOwner.Pins[Pin],&Input) : EFI_UNSUPPORTED;
    DEBUG((DEBUG_WARN,"LT9611: GPIO%u requested=%u output-check=%r input=%u input-status=%r\n",
      mPanelOwner.Pins[Pin],High,Status,Input,Sample));
  }
  if (!mHandoffRegistered) return;
  if (Status != EFI_SUCCESS) { Lt9611RetainFailure(Status); return; }
  if (High) mNativeBridge.HighPins |= 1U << Pin;
  else mNativeBridge.HighPins &= ~(1U << Pin);
  mNativeBridge.State = mNativeBridge.HighPins ? CR_MDSS_NATIVE_UNCERTAIN : CR_MDSS_NATIVE_STOPPED;
}
STATIC EFI_STATUS LtPanelPrepare(VOID *Context,CONST EFI_MDP_DSI_TIMING *Timing)
{
  UINT8 ChipId[3];
  CONST CR_DAL_LT9611_CONFIG *Config = CrDalGetLt9611Config();
  if (LtPanelReady(Context) != EFI_SUCCESS || !mMcuBusy || Config == NULL)
    return EFI_NOT_READY;
  EFI_STATUS Status = Lt9611TestFifo(Config,ChipId);
  if (Status == EFI_SUCCESS) Status = Lt9611CheckFirmware(Config);
  if (Status == EFI_SUCCESS && !mHandoffFrozen)
    Status = Lt9611ConfigureVideoInternal(&mLt9611Bridge,Timing);
  if (Status != EFI_SUCCESS || mHandoffFrozen) {
    if (Status == EFI_SUCCESS || !EFI_ERROR(Status)) Status = EFI_NOT_READY;
    Lt9611RetainFailure(Status);
  } else if (mHandoffRegistered && mNativeBridge.HighPins == 7 &&
             !(mNativeBridge.Flags & CR_MDSS_NATIVE_RETAINED_ERROR)) {
    mNativeBridge.State = CR_MDSS_NATIVE_LIVE;
  }
  return Status;
}

VOID Lt9611IrqHandler (VOID *Param)
{
  (VOID)Param;
  /* No boot services, logging, bank access, or I2C in interrupt context. */
  if (mBootServicesLive && !mHandoffFrozen) mIrqPending = TRUE;
}

STATIC EFI_STATUS Lt9611ServiceIrq (VOID)
{
  CONST CR_DAL_LT9611_CONFIG *Bridge = CrDalGetLt9611Config ();
  EFI_I2C_CR_PROTOCOL *I2c;
  MU_I2C_SLAVE_CONFIG Config;
  EFI_STATUS Status, Cleanup;
  UINT8 Irq = 0, Hpd = 0;
  if (Bridge == NULL) return EFI_NOT_FOUND;
  Status = Lt9611FindI2c (Bridge->I2cInstance, &I2c);
  if (EFI_ERROR (Status)) return Status;
  Config = (MU_I2C_SLAVE_CONFIG) {
    .BusFrequencyKHz = LT9611_I2C_FREQUENCY_KHZ,
    .SlaveAddress = Bridge->I2cAddress,
    .Mode = MU_I2C_MODE_I2C,
    .SlaveMaxClockStretchUs = LT9611_I2C_STRETCH_US,
  };
  /* Linux lt9611uxc_irq_thread_handler: lock MCU, read B022/B023, clear
     B022 with zero, unlock. Bits0/1 indicate EDID-ready/HDMI-connected. */
  Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (EFI_ERROR (Status)) return Status;
  Status = Lt9611WriteRegister (I2c, &Config, 0xee, 1);
  if (!EFI_ERROR (Status)) Status = Lt9611WriteRegister (I2c, &Config, 0xff, 0xb0);
  if (!EFI_ERROR (Status)) Status = Lt9611ReadRegister (I2c, &Config, 0x22, &Irq);
  if (!EFI_ERROR (Status)) Status = Lt9611ReadRegister (I2c, &Config, 0x23, &Hpd);
  if (!EFI_ERROR (Status) && Irq != 0) Status = Lt9611WriteRegister (I2c, &Config, 0x22, 0);
  Cleanup = Lt9611WriteRegister (I2c, &Config, 0xff, 0x80);
  if (!EFI_ERROR (Cleanup)) Cleanup = Lt9611WriteRegister (I2c, &Config, 0xee, 0);
  if (!EFI_ERROR (Cleanup)) MicroSecondDelay (50000);
  if (EFI_ERROR (Status)) return Status;
  if (EFI_ERROR (Cleanup)) return Cleanup;
  mLastHpdStatus = Hpd;
  DEBUG ((DEBUG_INFO, "LT9611: IRQ 0x%02x EDID-ready=%u HDMI-connected=%u\n",
          Irq, (Hpd & BIT0) != 0, (Hpd & BIT1) != 0));
  return EFI_SUCCESS;
}

STATIC VOID EFIAPI Lt9611IrqWorker (EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Context;
  if (!mBootServicesLive || mHandoffFrozen || Event == NULL || Event != mIrqTimer) return;
  if (mIrqPending && (!mPanelOwner.Initialized ||
      (mPanelOwner.Powered && !mPanelOwner.ResetHeld)) && Lt9611AcquireMcu ()) {
    /* Clear before I2C so an edge arriving during work remains pending. */
    mIrqPending = FALSE;
    Status = Lt9611ServiceIrq ();
    if (Status != EFI_SUCCESS) {
      if (mBootServicesLive && !mHandoffFrozen) mIrqPending = TRUE;
      Lt9611RetainFailure (Status);
      DEBUG ((DEBUG_ERROR, "LT9611: deferred IRQ service failed: %r\n", Status));
    }
    Lt9611ReleaseMcu ();
  }
  /* MCU unlock alone takes 50 ms. A 10 ms periodic timer would queue this
     callback again while it is running and can starve DXE on repeated IRQs
     or bus errors. Rearm only after completing this service attempt. */
  if (mBootServicesLive && !mHandoffFrozen) {
    Status = gBS->SetTimer (mIrqTimer, TimerRelative, LT9611_IRQ_POLL_100NS);
    if (Status != EFI_SUCCESS) {
      Lt9611RetainFailure (Status);
      DEBUG ((DEBUG_ERROR, "LT9611: IRQ timer rearm failed: %r\n", Status));
    }
  }
}

STATIC VOID Lt9611CloseEvents (VOID)
{
  (VOID)Lt9611HandoffFreeze (&mLt9611Bridge);
  if (mI2cTestEvent != NULL) {
    gBS->CloseEvent (mI2cTestEvent);
    mI2cTestEvent = NULL;
  }
  if (mIrqTimer != NULL) {
    gBS->SetTimer (mIrqTimer, TimerCancel, 0);
    gBS->CloseEvent (mIrqTimer);
    mIrqTimer = NULL;
  }
  if (mExitBootEvent != NULL) {
    gBS->CloseEvent (mExitBootEvent);
    mExitBootEvent = NULL;
  }
}

STATIC VOID EFIAPI Lt9611ExitBootServices (EFI_EVENT Event, VOID *Context)
{
  (VOID)Event; (VOID)Context;
  Lt9611CloseEvents ();
  /* GPIO has no unregister method. The resident raw ISR is fenced by the
     same one-way software gate; it cannot change even its pending flag. */
}

EFI_STATUS
Lt9611EntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  // Setup gpio
  EFI_STATUS            Status       = EFI_SUCCESS;
  UINT8                 ChipId[3]     = { 0 };
  EFI_GPIO_CR_PROTOCOL *GpioProtocol = NULL;
  CONST CR_DAL_LT9611_CONFIG *Config = CrDalGetLt9611Config ();

  (VOID)ImageHandle;
  (VOID)SystemTable;
  if (Config == NULL) {
    return EFI_NOT_FOUND;
  }
  if ((Config->PowerPinCount != 3) || (Config->PowerPins == NULL) ||
      (Config->I2cInstance == 0) || (Config->I2cAddress > 0x7f)) {
    return EFI_COMPROMISED_DATA;
  }

  /* Disabled non-HDK builds keep their original startup contract. */
  if (!PcdGetBool (PcdMdssHandoffEnable)) mHandoffFrozen = FALSE;
  Status = Lt9611RegisterHandoff (Config);
  if (Status != EFI_SUCCESS) return Status;
  mStartupBusy = TRUE;
  if(PcdGet32(PcdMdssPanelProfile)==2)mResidentRetained=TRUE;

  Status = gBS->LocateProtocol(
      &gEfiGpioCrProtocolGuid, NULL, (VOID **)&GpioProtocol);
  if (Status != EFI_SUCCESS) {
    DEBUG((DEBUG_ERROR, "Failed to locate Gpio CR Protocol: %r\n", Status));
    return Lt9611StartupResult (Status);
  }
  if ((GpioProtocol == NULL) ||
      (GpioProtocol->Revision < EFI_GPIO_CR_PROTOCOL_REVISION) ||
      (GpioProtocol->ConfigGpio == NULL) ||
      (GpioProtocol->SetIoValue == NULL) ||
      (GpioProtocol->RegisterGpioInterrupt == NULL)) {
    return Lt9611StartupResult (EFI_UNSUPPORTED);
  }

  // Configure GPIOs for LT9611Uxc
  // Setup Interrupt
  UINT16 GpioIntPin = Config->InterruptPin;
  GpioConfigParams GpioIntConfig = {0};
  GpioIntConfig.PinNumber        = GpioIntPin;
  GpioIntConfig.OutputEnable     = FALSE; // Input
  GpioIntConfig.FunctionSel      = GPIO_FUNC_NORMAL;
  GpioIntConfig.Pull             = GPIO_PULL_NONE;
  GpioIntConfig.DriveStrength    = GPIO_DRIVE_STRENGTH_UNCHANGE;
  GpioIntConfig.OutputValue      = GPIO_VALUE_UNCHANGE;
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  if (mHandoffRegistered) {
    mNativeBridge.State = CR_MDSS_NATIVE_UNCERTAIN;
    mNativeBridge.Flags |= CR_MDSS_NATIVE_OWNED;
  }
  Status = GpioProtocol->ConfigGpio(GpioProtocol, &GpioIntConfig);
  if (Status != EFI_SUCCESS) {
    DEBUG(
        (DEBUG_ERROR,
         "Failed to configure Interrupt GPIO %d for LT9611Uxc: %r\n",
         GpioIntPin, Status));
    return Lt9611StartupResult (Status);
  }

  // The target lists two supplies followed by the active-high reset pin.
  for (UINTN i = 0; i < Config->PowerPinCount - 1; i++) {
    if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
    UINT16 GpioPin = Config->PowerPins[i];
    GpioConfigParams PowerConfig = {
      .PinNumber = GpioPin,
      .OutputEnable = TRUE,
      .FunctionSel = GPIO_FUNC_NORMAL,
      .Pull = GPIO_PULL_NONE,
      .DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE,
      .OutputValue = GPIO_VALUE_HIGH,
    };

    Status = GpioProtocol->ConfigGpio (GpioProtocol, &PowerConfig);
    if (Status != EFI_SUCCESS) {
      DEBUG(
          (DEBUG_ERROR, "Failed to set GPIO %d High for LT9611Uxc: %r\n",
           GpioPin, Status));
      return Lt9611StartupResult (Status);
    }
    if (mHandoffRegistered) mNativeBridge.HighPins |= 1U << i;
    if (i == 0) {
      MicroSecondDelay (1000);
    }
  }

  UINT16 ResetPin = Config->PowerPins[Config->PowerPinCount - 1];
  GpioConfigParams ResetConfig = {
    .PinNumber = ResetPin,
    .OutputEnable = TRUE,
    .FunctionSel = GPIO_FUNC_NORMAL,
    .Pull = GPIO_PULL_NONE,
    .DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE,
    .OutputValue = GPIO_VALUE_HIGH,
  };
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  Status = GpioProtocol->ConfigGpio (GpioProtocol, &ResetConfig);
  if (Status != EFI_SUCCESS) {
    return Lt9611StartupResult (Status);
  }
  if (mHandoffRegistered) mNativeBridge.HighPins |= 4U;
  MicroSecondDelay (20000);
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  Status = GpioProtocol->SetIoValue (GpioProtocol, ResetPin, GPIO_VALUE_LOW);
  if (Status != EFI_SUCCESS) {
    return Lt9611StartupResult (Status);
  }
  if (mHandoffRegistered) mNativeBridge.HighPins &= ~4U;
  MicroSecondDelay (20000);
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  Status = GpioProtocol->SetIoValue (GpioProtocol, ResetPin, GPIO_VALUE_HIGH);
  if (Status != EFI_SUCCESS) {
    return Lt9611StartupResult (Status);
  }
  if (mHandoffRegistered) mNativeBridge.HighPins |= 4U;
  MicroSecondDelay (300000);
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  Status = Lt9611TestFifo (Config, ChipId);
  if (Status != EFI_SUCCESS) {
    return Lt9611StartupResult (Status);
  }

  Status = Lt9611CheckFirmware (Config);
  if (Status != EFI_SUCCESS) return Lt9611StartupResult (Status);
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);

  /* Display and the bridge use FIFO. Defer the independent GPI transport
     test until ReadyToBoot so a DMA diagnostic cannot block DXE dispatch
     before MDSS initializes and publishes GOP. */
  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                            Lt9611IrqWorker, NULL, &mIrqTimer);
  if (Status != EFI_SUCCESS) return Lt9611StartupResult (Status);
  Status = gBS->CreateEvent (EVT_SIGNAL_EXIT_BOOT_SERVICES, TPL_NOTIFY,
                            Lt9611ExitBootServices, NULL, &mExitBootEvent);
  if (Status != EFI_SUCCESS) { Lt9611CloseEvents (); return Lt9611StartupResult (Status); }
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Lt9611TestI2cReadyToBoot,
                  NULL, &gEfiEventReadyToBootGuid, &mI2cTestEvent);
  if (Status != EFI_SUCCESS) { Lt9611CloseEvents (); return Lt9611StartupResult (Status); }
  if (mHandoffFrozen) { Lt9611CloseEvents (); return Lt9611StartupResult (EFI_NOT_READY); }
  mBootServicesLive = TRUE;
  mIrqPending = TRUE; /* Drain any status latched before IRQ registration. */
  /* Arm only after the worker is live; a one-shot expiry during event setup
     would otherwise return without rearming and lose all later IRQ work. */
  Status = gBS->SetTimer (mIrqTimer, TimerRelative, LT9611_IRQ_POLL_100NS);
  if (Status != EFI_SUCCESS) { Lt9611CloseEvents (); return Lt9611StartupResult (Status); }
  if (mHandoffFrozen) { Lt9611CloseEvents (); return Lt9611StartupResult (EFI_NOT_READY); }
  mLt9611Bridge.ConfigureVideo = Lt9611ConfigureVideo;
  EFI_HANDLE BridgeHandle = NULL;
  EFI_MDP_DSI_PROTOCOL *Mdp = NULL;
  if (Lt9611SelectedPanel()) {
    CONST LT9611_PANEL_OWNER_IO Io={&mLt9611Bridge,LtPanelReady,LtPanelLock,LtPanelUnlock,LtPanelPrepare,LtPanelObserve};
    CONST CR_MDSS_TARGET *MdssTarget=CrDalGetMdssContext();
    Status = Lt9611PanelOwnerInitRoute(&mPanelOwner,GpioProtocol,&Io,Config->PowerPins,
      MdssTarget!=NULL&&MdssTarget->Soc==CR_MDSS_SOC_SM8350);
    if (Status != EFI_SUCCESS) return Lt9611StartupResult(Status);
    Status = gBS->InstallMultipleProtocolInterfaces (
                  &BridgeHandle, &gEfiLt9611CrProtocolGuid, &mLt9611Bridge,
                  &gEfiLt9611PanelCrProtocolGuid,&mPanelOwner.Protocol,NULL);
  } else {
    Status = gBS->InstallMultipleProtocolInterfaces (
                  &BridgeHandle, &gEfiLt9611CrProtocolGuid, &mLt9611Bridge, NULL);
  }
  if (Status != EFI_SUCCESS) { Lt9611CloseEvents (); return Lt9611StartupResult (Status); }
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  DEBUG ((DEBUG_INIT, "LT9611: FIFO bridge ready; GPI self-test scheduled for ReadyToBoot\n"));
  Status = gBS->LocateProtocol (
                  &gEfiMdpDsiProtocolGuid, NULL, (VOID **)&Mdp);
  if (!EFI_ERROR (Status) && Mdp != NULL && Mdp->BridgeReady != NULL) {
    Status = Mdp->BridgeReady (Mdp);
    if (Status != EFI_SUCCESS) {
      DEBUG ((DEBUG_ERROR, "LT9611: MDSS bridge notification failed: %r\n", Status));
      if (Lt9611SelectedPanel()) {
        /* The new caller may retain a failed power/reset/DMA journal. Keep
           this actual provider callable for its exact cleanup. */
        Lt9611RetainFailure(Status);
        return Lt9611StartupResult(Status);
      }
      /* MdpBridgeFailure has already revoked prepared MDSS state when its
         fence succeeded.  Remove this bridge before returning an entry error;
         otherwise DXE could unload the image while the protocol still points
         at its static callback.  A failed uninstall keeps the image resident
         and avoids creating a dangling GPIO ISR. */
      {
        EFI_STATUS UninstallStatus;
        UninstallStatus = gBS->UninstallMultipleProtocolInterfaces (
                            BridgeHandle, &gEfiLt9611CrProtocolGuid,
                            &mLt9611Bridge, NULL);
        if (EFI_ERROR (UninstallStatus)) {
          DEBUG ((DEBUG_ERROR,
                  "LT9611: bridge handoff cleanup failed: %r\n",
                  UninstallStatus));
          return Lt9611StartupResult (EFI_DEVICE_ERROR);
        }
      }
      Lt9611CloseEvents ();
      return Lt9611StartupResult (Status);
    }
  } else {
    DEBUG ((DEBUG_ERROR, "LT9611: prepared MDSS protocol unavailable: %r\n", Status));
  }
  if (mHandoffFrozen) return Lt9611StartupResult (EFI_NOT_READY);
  Status = GpioProtocol->RegisterGpioInterrupt (
      GpioProtocol, GpioIntPin, Lt9611IrqHandler, NULL,
      CR_INTERRUPT_TRIGGER_EDGE_FALLING);
  if (Status != EFI_SUCCESS) {
    Lt9611RetainFailure (Status);
    /* GPIO may already have stored Handler before failing to enable it.
       Returning an entry error would unload that callback's code. */
    DEBUG ((DEBUG_ERROR, "LT9611: IRQ registration failed: %r; retaining resident bridge callback\n", Status));
  }
  DEBUG ((DEBUG_INIT, "LT9611: entry complete; IRQ registration: %r\n", Status));
  return Lt9611StartupResult (EFI_SUCCESS);
}
