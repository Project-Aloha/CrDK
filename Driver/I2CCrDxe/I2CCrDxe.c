/** @file
 *  Native GENI FIFO transport for Crane consumers.
 *
 *  Board target data selects native FIFO buses with shared QUP SE leases.
 *  No packaged Qualcomm or generic PI backend is consulted.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFII2cCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>

#ifndef BIT
#define BIT(_N)  (1U << (_N))
#endif

#ifdef CRANE_HOST_TEST
STATIC EFI_GUID mHostClockCrProtocolGuid =
  { 0xa355879b, 0x66c2, 0x4901, { 0xb9, 0xa5, 0xc3, 0x0f, 0x31, 0x91, 0x81, 0xc8 } };
STATIC EFI_GUID mHostGpioCrProtocolGuid =
  { 0x05726725, 0x2ef4, 0x4f4c, { 0x99, 0xa5, 0xb9, 0x5d, 0x58, 0x19, 0x92, 0xa1 } };
#define gEfiClockCrProtocolGuid mHostClockCrProtocolGuid
#define gEfiGpioCrProtocolGuid  mHostGpioCrProtocolGuid
#ifndef CRANE_MMIO_TEST
#define MmioRead32(_Address)             ((VOID)(_Address), 0U)
#define MmioWrite32(_Address, _Value)    ((VOID)(_Address), (VOID)(_Value))
#define MicroSecondDelay(_Usec)           ((VOID)(_Usec), (UINTN)0)
#endif
#undef DEBUG
#define DEBUG(_Args)                      do { } while (0)
#endif

#define I2C_CR_DEFAULT_STRETCH_US     500U
/* GENI's TX/RX TRANS_LEN fields are 24 bits.  Cap the complete request too. */
#define I2C_CR_MAX_TRANSFER_BYTES     0x00FFFFFFU

/* Linux sm8450 GENI I2C register contract. */
#define SE_GENI_STATUS                0x040U
#define SE_GENI_IF_DISABLE_RO         0x064U
#define SE_GENI_FW_REVISION_RO        0x068U
#define SE_HW_PARAM_0                 0xE24U
#define SE_CLK_SEL                    0x07CU
#define SE_DMA_MODE_EN                0x258U
#define SE_TX_PACKING_CFG0            0x260U
#define SE_TX_PACKING_CFG1            0x264U
#define SE_BYTE_GRAN                  0x254U
#define SE_RX_PACKING_CFG0            0x284U
#define SE_RX_PACKING_CFG1            0x288U
#define SE_M_CMD0                     0x600U
#define SE_M_CMD_CTRL                 0x604U
#define SE_M_IRQ_STATUS               0x610U
#define SE_M_IRQ_EN                   0x614U
#define SE_M_IRQ_CLEAR                0x618U
#define SE_M_GP_LENGTH                0x910U
#define SE_DMA_TX_IRQ_CLEAR           0xC44U
#define SE_DMA_RX_IRQ_CLEAR           0xD44U
#define SE_TX_FIFO                    0x700U
#define SE_RX_FIFO                    0x780U
#define SE_TX_FIFO_STATUS             0x800U
#define SE_RX_FIFO_STATUS             0x804U
#define SE_TX_WATERMARK               0x80CU
#define SE_RX_WATERMARK               0x810U
#define SE_RX_RFR_WATERMARK           0x814U
#define SE_I2C_TX_LEN                 0x26CU
#define SE_I2C_RX_LEN                 0x270U
#define SE_GSI_EVENT_EN               0xE18U
#define SE_IRQ_EN                     0xE1CU
#define SE_DMA_GENERAL_CFG            0xE30U
#define SE_CGC_CTRL                   0x028U
#define SE_OUTPUT_CTRL                0x024U
#define SE_FORCE_DEFAULT              0x020U
#define SE_S_IRQ_CLEAR                0x648U
#define SE_M_CMD_DONE                 BIT (0)
#define SE_M_CMD_OVERRUN              BIT (1)
#define SE_M_ILLEGAL_CMD              BIT (2)
#define SE_M_CMD_FAILURE              BIT (3)
#define SE_M_CMD_ABORT                BIT (5)
#define SE_M_GP_IRQ_0                 BIT (9)
#define SE_M_GP_IRQ_1                 BIT (10)
#define SE_M_GP_IRQ_3                 BIT (12)
#define SE_M_GP_IRQ_4                 BIT (13)
#define SE_M_GENI_CMD_ABORT           BIT (1)
#define SE_M_RX_FIFO_WATERMARK        BIT (26)
#define SE_M_RX_FIFO_LAST             BIT (27)
#define SE_M_TX_FIFO_WATERMARK        BIT (30)
#define SE_RX_FIFO_WC_MASK            0x01FFFFFFU
#define SE_TX_FIFO_WC_MASK            0x0FFFFFFFU
#define SE_M_I2C_ERROR_MASK           (SE_M_CMD_OVERRUN | SE_M_ILLEGAL_CMD | \
                                       SE_M_CMD_FAILURE | SE_M_CMD_ABORT | \
                                       SE_M_GP_IRQ_0 | SE_M_GP_IRQ_1 | SE_M_GP_IRQ_3 | \
                                       SE_M_GP_IRQ_4)
#define I2C_CMD_WRITE                 0x1U
#define I2C_CMD_READ                  0x2U
#define I2C_CMD_WRITE_READ            0x3U
#define I2C_STOP_STRETCH              BIT (2)
#define I2C_SLAVE_ADDR_SHIFT          9U
#define I2C_FIFO_TIMEOUT_US           500000U

typedef struct {
  CONST CR_TARGET_QUP_I2C_ENDPOINT *Target;
  EFI_HANDLE             AliasHandle;
  EFI_I2C_CR_PROTOCOL     Protocol;
  volatile UINT32        Busy;
  UINT32                 FifoDepth;
  UINT32                 ClockMask;
  EFI_CLOCK_CR_PROTOCOL *Clock;
  BOOLEAN                ClockCleanupPending;
} I2C_CR_INSTANCE;

typedef CR_TARGET_QUP_I2C_ENDPOINT I2C_CR_TARGET;
STATIC CONST I2C_CR_TARGET *
I2cCrTarget(IN I2C_CR_INSTANCE *Instance)
{
  if (Instance == NULL || Instance->Target == NULL ||
      Instance->Target->Instance != Instance->Protocol.Instance) return NULL;
  return Instance->Target;
}
STATIC EFI_QUP_CR_PROTOCOL *mFifoQup;

STATIC EFI_STATUS
I2cCrConfigurePins (IN CONST I2C_CR_TARGET *Target)
{
  EFI_GPIO_CR_PROTOCOL *Gpio;
  EFI_STATUS            Status;
  UINTN                 Index;

  Gpio = NULL;
  Status = gBS->LocateProtocol (&gEfiGpioCrProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status) || (Gpio == NULL) || (Gpio->ConfigGpio == NULL)) {
    DEBUG ((DEBUG_WARN, "I2CCr: GPIO pinmux protocol unavailable: %r\n", Status));
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  /* Pin routing and electrical policy belong to this board endpoint. */
  for (Index = 0; Index < 2; ++Index) {
    GpioConfigParams Config;
    ZeroMem (&Config, sizeof (Config));
    Config.PinNumber = Target->Pins[Index].Pin;
    Config.OutputEnable = FALSE;
    Config.FunctionSel = Target->Pins[Index].Function;
    Config.Pull = Target->Pins[Index].Pull;
    Config.DriveStrength = Target->Pins[Index].DriveStrength;
    Config.OutputValue = GPIO_VALUE_UNCHANGE;
    Status = Gpio->ConfigGpio (Gpio, &Config);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "I2CCr: GPIO%u pinmux failed: %r\n", Config.PinNumber, Status));
      return Status;
    }
    if (Gpio->GetGpioConfig != NULL) {
      GpioConfigParams Actual;
      ZeroMem (&Actual, sizeof (Actual));
      Actual.PinNumber = Config.PinNumber;
      Status = Gpio->GetGpioConfig (Gpio, &Actual);
      if (EFI_ERROR (Status)) return Status;
      /* GetConfig reports the EGPIO remote function if AP ownership did
         not latch; checking just the mux would miss a disconnected bus. */
      if (Actual.FunctionSel != Config.FunctionSel || Actual.OutputEnable ||
          (Config.Pull != GPIO_PULL_UNCHANGE && Actual.Pull != Config.Pull) ||
          (Config.DriveStrength != GPIO_DRIVE_STRENGTH_UNCHANGE &&
           Actual.DriveStrength != Config.DriveStrength)) {
        DEBUG ((DEBUG_ERROR, "I2CCr: GPIO%u route readback mux=%u OE=%u\n",
                Actual.PinNumber, Actual.FunctionSel, Actual.OutputEnable));
        return EFI_DEVICE_ERROR;
      }
    }
  }
  return EFI_SUCCESS;
}

STATIC I2C_CR_INSTANCE *
I2cCrInstanceFromProtocol (
  IN EFI_I2C_CR_PROTOCOL *This
  );

STATIC I2C_CR_INSTANCE *mInstances;

STATIC EFI_STATUS
I2cCrReleaseClocks (I2C_CR_INSTANCE *Instance, CONST I2C_CR_TARGET *Target)
{
  UINTN Index;
  EFI_STATUS Status;
  /* Only our successful or explicitly retained failed acquisitions are in
     this mask. Stop on failure so the exact cleanup owner remains retryable. */
  for (Index = ARRAY_SIZE(Target->Clocks); Index != 0; --Index) {
    UINT32 Bit = 1U << (Index - 1U);
    if ((Instance->ClockMask & Bit) == 0) continue;
    Status = Instance->Clock->SetClock(Instance->Clock, Target->ClockController,
                                      Target->Clocks[Index - 1U], 0, FALSE);
    if (EFI_ERROR(Status)) return Status;
    Instance->ClockMask &= ~Bit;
  }
  Instance->ClockCleanupPending = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cCrFifoPrepare (
  IN I2C_CR_INSTANCE *Instance,
  IN UINT32 FrequencyKHz
  )
{
  EFI_STATUS             Status;
  EFI_CLOCK_CR_PROTOCOL *Clock;
  UINTN                  Base;
  UINT32 Divider;
  UINT32 Low;
  UINT32 High;
  UINT32 Cycle;
  UINTN Index;
  BOOLEAN Owned;
  CONST I2C_CR_TARGET *Target = I2cCrTarget(Instance);

  if (Target == NULL) {
    return EFI_UNSUPPORTED;
  }
  if (Instance->ClockCleanupPending) {
    Status = I2cCrReleaseClocks(Instance, Target);
    if (EFI_ERROR(Status)) return Status;
  }
  /* Clocks are shared with the GPI backend.  Enable them once, but select
     FIFO mode on every transaction so a preceding GPI transfer cannot leave
     GENI in DMA mode. */
  if (Instance->ClockMask != 7U) {
    Clock = Instance->Clock;
    if (Clock == NULL) {
      Status = gBS->LocateProtocol (&gEfiClockCrProtocolGuid, NULL, (VOID **)&Clock);
      if (EFI_ERROR(Status)) return EFI_NOT_READY;
      Instance->Clock = Clock;
    }
    if (Clock == NULL || Clock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
        Clock->SetClock == NULL || Clock->AcquireClock == NULL) return EFI_NOT_READY;
    for (Index = 0; Index < 3; ++Index) {
      if ((Instance->ClockMask & (1U << Index)) != 0) continue;
      Status = CrAcquireClock(Clock, Target->ClockController, Target->Clocks[Index],
                              Index == 2 ? Target->SourceClockHz : 0, &Owned);
      if (Owned) Instance->ClockMask |= 1U << Index;
      if (EFI_ERROR(Status)) {
        Instance->ClockCleanupPending = TRUE;
        (VOID)I2cCrReleaseClocks(Instance, Target);
        return Status;
      }
    }
  }

  Base = (UINTN)Target->Base;
  if ((MmioRead32 (Base + SE_GENI_STATUS) & BIT (0)) != 0) {
    return EFI_NOT_READY;
  }
  /* Linux geni_i2c_init() requires I2C sequencer firmware and rejects
     FIFO mode when the hardware disables that interface.  Clock/pinmux
     setup cannot turn a SPI/UART or unloaded sequencer into I2C. */
  if (((MmioRead32 (Base + SE_GENI_FW_REVISION_RO) >> 8) & 0xFFU) != 3U) {
    DEBUG ((DEBUG_ERROR, "I2CCr: instance %u has no I2C firmware (FW=0x%x)\n",
            Target->Instance,
            MmioRead32 (Base + SE_GENI_FW_REVISION_RO)));
    return EFI_UNSUPPORTED;
  }
  if ((MmioRead32 (Base + SE_GENI_IF_DISABLE_RO) & BIT (0)) != 0) {
    DEBUG ((DEBUG_ERROR, "I2CCr: instance %u FIFO interface disabled; GPI required\n",
            Target->Instance));
    return EFI_UNSUPPORTED;
  }
  Instance->FifoDepth = (MmioRead32 (Base + SE_HW_PARAM_0) >> 16) & 0x3FU;
  if (Instance->FifoDepth < 2) return EFI_UNSUPPORTED;
  Status = I2cCrConfigurePins (Target);
  if (EFI_ERROR (Status)) return Status;
  /* Linux geni_se_init()/geni_se_select_fifo_mode(): initialize common
     GENI and DMA gates before selecting FIFO mode. */
  MmioWrite32 (Base + SE_CGC_CTRL,
      MmioRead32 (Base + SE_CGC_CTRL) | 0x7FU);
  MmioWrite32 (Base + SE_DMA_GENERAL_CFG,
      MmioRead32 (Base + SE_DMA_GENERAL_CFG) | 0x0FU);
  MmioWrite32 (Base + SE_OUTPUT_CTRL, 0x7FU);
  MmioWrite32 (Base + SE_FORCE_DEFAULT, BIT (0));
  /* Linux geni_se_select_fifo_mode() quiesces GPI and clears stale IRQs. */
  MmioWrite32 (Base + SE_GSI_EVENT_EN, 0);
  MmioWrite32 (Base + SE_IRQ_EN, BIT (0) | BIT (1) | BIT (2) | BIT (3));
  MmioWrite32 (Base + SE_M_IRQ_CLEAR, MAX_UINT32);
  MmioWrite32 (Base + SE_S_IRQ_CLEAR, MAX_UINT32);
  MmioWrite32 (Base + SE_DMA_TX_IRQ_CLEAR, MAX_UINT32);
  MmioWrite32 (Base + SE_DMA_RX_IRQ_CLEAR, MAX_UINT32);
  MmioWrite32 (Base + SE_DMA_MODE_EN, 0);
  MmioWrite32 (Base + SE_TX_WATERMARK, 0);
  MmioWrite32 (Base + SE_RX_WATERMARK, Instance->FifoDepth - 1U);
  MmioWrite32 (Base + SE_RX_RFR_WATERMARK, Instance->FifoDepth);

  /* geni_se_config_packing(8, 4, true, true, true). */
  MmioWrite32 (Base + SE_TX_PACKING_CFG0, 0x0007F8FEU);
  MmioWrite32 (Base + SE_TX_PACKING_CFG1, 0x000FFEFEU);
  MmioWrite32 (Base + SE_RX_PACKING_CFG0, 0x0007F8FEU);
  MmioWrite32 (Base + SE_RX_PACKING_CFG1, 0x000FFEFEU);
  MmioWrite32 (Base + SE_BYTE_GRAN, 0);
  /* Linux geni_i2c_clk_map_19p2mhz. */
  if (FrequencyKHz == 100) {
    Divider = 7; Cycle = 26; Low = 12; High = 10;
  } else if (FrequencyKHz == 400) {
    Divider = 2; Cycle = 22; Low = 11; High = 5;
  } else {
    Divider = 1; Cycle = 18; Low = 8; High = 2;
  }
  MmioWrite32 (Base + SE_CLK_SEL, 0);
  MmioWrite32 (Base + 0x048U, (Divider << 4) | BIT (0));
  MmioWrite32 (Base + 0x278U, Cycle | (Low << 10) | (High << 20));
  /* Keep the common GENI error/flow bits enabled as geni_se_init() does;
     qcom-geni-i2c adds the protocol-specific GP IRQs and FIFO service bits. */
  MmioWrite32 (Base + SE_M_IRQ_EN,
      BIT (1) | BIT (2) | BIT (3) | BIT (4) | BIT (5) | BIT (6) |
      BIT (22) | BIT (23) | BIT (24) | BIT (25) | BIT (28) | BIT (29) |
      SE_M_CMD_DONE | SE_M_GP_IRQ_0 | SE_M_GP_IRQ_1 | SE_M_GP_IRQ_3 |
      SE_M_GP_IRQ_4 | SE_M_RX_FIFO_WATERMARK | SE_M_RX_FIFO_LAST |
      SE_M_TX_FIFO_WATERMARK);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
I2cCrFifoTransaction (
  IN I2C_CR_INSTANCE *Instance,
  IN  UINT32       SlaveAddress,
  IN  UINT32       Command,
  IN  UINT32       Parameters,
  IN  CONST UINT8 *TxBuffer OPTIONAL,
  IN  UINT32       TxLength,
  OUT UINT8       *RxBuffer OPTIONAL,
  IN  UINT32       RxLength
  )
{
  UINTN  Base;
  UINT32 TxWords;
  UINT32 TxWord;
  UINT32 TxIndex;
  UINT32 RxIndex;
  UINT32 Elapsed;
  UINT32 Status;
  UINT32 ErrorStatus;
  CONST I2C_CR_TARGET *Target = I2cCrTarget (Instance);

  if ((Target == NULL) || (SlaveAddress > 0x7FU) || (TxLength > I2C_CR_MAX_TRANSFER_BYTES) ||
      (RxLength > I2C_CR_MAX_TRANSFER_BYTES) ||
      ((TxLength != 0) && (TxBuffer == NULL)) ||
      ((RxLength != 0) && (RxBuffer == NULL))) {
    return EFI_INVALID_PARAMETER;
  }
  Base = (UINTN)Target->Base;
  TxWords = (TxLength + 3U) / 4U;
  TxIndex = 0;
  RxIndex = 0;

  MmioWrite32 (Base + SE_M_IRQ_CLEAR, MAX_UINT32);
  MmioWrite32 (Base + SE_I2C_TX_LEN, TxLength);
  MmioWrite32 (Base + SE_I2C_RX_LEN, RxLength);

  MmioWrite32 (Base + SE_M_CMD0,
      (Command << 27) | Parameters |
      ((SlaveAddress & 0x7FU) << I2C_SLAVE_ADDR_SHIFT));

  for (Elapsed = 0; Elapsed < I2C_FIFO_TIMEOUT_US; Elapsed += 10U) {
    Status = MmioRead32 (Base + SE_M_IRQ_STATUS);
    ErrorStatus = Status & SE_M_I2C_ERROR_MASK;
    /* Linux geni_i2c_check_addr_data_nack(): GP_LENGTH zero is an address
       NACK; nonzero is a data NACK, which is an error only for writes.
       CMD_FAILURE is the summary for that GP cause, so discard it together
       with GP_IRQ1 for a read data NACK, retaining every independent error. */
    if ((Command == I2C_CMD_READ) && ((Status & SE_M_GP_IRQ_1) != 0) &&
        (MmioRead32 (Base + SE_M_GP_LENGTH) != 0)) {
      ErrorStatus &= ~(SE_M_GP_IRQ_1 | SE_M_CMD_FAILURE);
    }
    if (ErrorStatus != 0) {
      DEBUG ((DEBUG_ERROR, "I2CCr FIFO: instance=%u addr=0x%x command=0x%x IRQ=0x%x GENI=0x%x GP_LENGTH=%u\n",
              Target->Instance, SlaveAddress, Command, Status,
              MmioRead32 (Base + SE_GENI_STATUS), MmioRead32 (Base + SE_M_GP_LENGTH)));
      return EFI_DEVICE_ERROR;
    }
    if (TxIndex < TxWords) {
      UINT32 Used = MmioRead32 (Base + SE_TX_FIFO_STATUS) & SE_TX_FIFO_WC_MASK;
      if (Used > Instance->FifoDepth) return EFI_DEVICE_ERROR;
      while ((Used < Instance->FifoDepth) && (TxIndex < TxWords)) {
        UINT32 Byte;
        TxWord = 0;
        for (Byte = 0; (Byte < 4) && (TxIndex * 4U + Byte < TxLength); ++Byte)
          TxWord |= (UINT32)TxBuffer[TxIndex * 4U + Byte] << (Byte * 8U);
        MmioWrite32 (Base + SE_TX_FIFO, TxWord);
        ++TxIndex;
        ++Used;
      }
    }
    if (RxIndex < RxLength) {
      UINT32 RxWords = MmioRead32 (Base + SE_RX_FIFO_STATUS) & SE_RX_FIFO_WC_MASK;
      while ((RxWords != 0) && (RxIndex < RxLength)) {
        UINT32 Word = MmioRead32 (Base + SE_RX_FIFO);
        UINT32 Byte;
        for (Byte = 0; (Byte < 4U) && (RxIndex < RxLength); ++Byte) {
          RxBuffer[RxIndex++] = (UINT8)(Word >> (Byte * 8U));
        }
        RxWords--;
      }
    }
    if ((Status & SE_M_CMD_DONE) != 0) {
      MmioWrite32 (Base + SE_M_IRQ_CLEAR, Status);
      if ((RxIndex != RxLength) || (TxIndex != TxWords)) {
        DEBUG ((DEBUG_ERROR, "I2CCr FIFO: short command 0x%x TX=%u/%u RX=%u/%u\n",
                Command, TxIndex, TxWords, RxIndex, RxLength));
      }
      return ((RxIndex == RxLength) && (TxIndex == TxWords)) ?
          EFI_SUCCESS : EFI_DEVICE_ERROR;
    }
    MicroSecondDelay (10);
  }
  DEBUG ((DEBUG_ERROR, "I2CCr FIFO: command 0x%x timed out IRQ=0x%x GENI=0x%x\n",
          Command, Status, MmioRead32 (Base + SE_GENI_STATUS)));
  return EFI_TIMEOUT;
}

STATIC EFI_STATUS
I2cCrFifoQuiesce (IN I2C_CR_INSTANCE *Instance, IN BOOLEAN Failed)
{
  CONST I2C_CR_TARGET *Target = I2cCrTarget (Instance);
  UINTN Base;
  UINT32 Elapsed;
  if (Target == NULL) return EFI_INVALID_PARAMETER;
  Base = (UINTN)Target->Base;
  /* An error after STOP_STRETCH may hold SCL even when CMD_ACTIVE cleared.
     Always issue abort on an error, and wait for its acknowledgement. */
  if (Failed || ((MmioRead32 (Base + SE_GENI_STATUS) & BIT (0)) != 0)) {
    MmioWrite32 (Base + SE_M_IRQ_CLEAR, SE_M_CMD_ABORT);
    MmioWrite32 (Base + SE_M_CMD_CTRL, SE_M_GENI_CMD_ABORT);
    for (Elapsed = 0; Elapsed < I2C_FIFO_TIMEOUT_US; Elapsed += 10) {
      if (((MmioRead32 (Base + SE_M_IRQ_STATUS) & SE_M_CMD_ABORT) != 0) &&
          ((MmioRead32 (Base + SE_GENI_STATUS) & BIT (0)) == 0)) break;
      MicroSecondDelay (10);
    }
    if (Elapsed == I2C_FIFO_TIMEOUT_US) return EFI_TIMEOUT;
  }
  MmioWrite32 (Base + SE_M_IRQ_CLEAR, MAX_UINT32);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
I2cCrFifoMuTransfer (
  IN EFI_I2C_CR_PROTOCOL *This, IN MU_I2C_SLAVE_CONFIG *Config,
  IN MU_I2C_DESCRIPTOR *Descriptors, IN UINT16 DescriptorCount,
  IN MU_I2C_CALLBACK Callback OPTIONAL, IN VOID *Context OPTIONAL,
  IN UINT32 Delay, OUT UINT32 *Transferred OPTIONAL)
{
  I2C_CR_INSTANCE *Instance = I2cCrInstanceFromProtocol (This);
  EFI_STATUS Status;
  EFI_STATUS Cleanup;
  EFI_STATUS ReleaseStatus;
  UINT64 Requested = 0;
  UINT32 Total = 0;
  UINT16 Index;
  BOOLEAN Prepared = FALSE;
  CONST I2C_CR_TARGET *Target = I2cCrTarget(Instance);
  if (Transferred != NULL) *Transferred = 0;
  if ((Instance == NULL) || (Config == NULL) || (Descriptors == NULL) ||
      (DescriptorCount == 0) || (Config->SlaveAddress > 0x7F))
    return EFI_INVALID_PARAMETER;
  if ((Callback != NULL) || (Context != NULL) || (Delay != 0) ||
      (Config->Mode != MU_I2C_MODE_I2C) ||
      ((Config->BusFrequencyKHz != 100) && (Config->BusFrequencyKHz != 400) &&
       (Config->BusFrequencyKHz != 1000))) return EFI_UNSUPPORTED;
  if (Target == NULL) return EFI_UNSUPPORTED;
  for (Index = 0; Index < DescriptorCount; ++Index) {
    UINT32 Flags = Descriptors[Index].Flags;
    if ((Descriptors[Index].Buffer == NULL) || (Descriptors[Index].Length == 0) ||
        ((Flags & ~(MU_I2C_FLAG_START | MU_I2C_FLAG_STOP |
                    MU_I2C_FLAG_WRITE | MU_I2C_FLAG_READ)) != 0) ||
        ((Flags & MU_I2C_FLAG_START) == 0) ||
        (((Flags & MU_I2C_FLAG_READ) != 0) == ((Flags & MU_I2C_FLAG_WRITE) != 0)) ||
        ((Index + 1 == DescriptorCount) && ((Flags & MU_I2C_FLAG_STOP) == 0)))
      return EFI_INVALID_PARAMETER;
    Requested += Descriptors[Index].Length;
    if (Requested > I2C_CR_MAX_TRANSFER_BYTES) return EFI_BAD_BUFFER_SIZE;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0)
    return EFI_ALREADY_STARTED;
  Status = gBS->LocateProtocol (&gEfiQupCrProtocolGuid, NULL, (VOID **)&mFifoQup);
  if (EFI_ERROR (Status) || (mFifoQup == NULL) ||
      (mFifoQup->Revision < EFI_QUP_CR_PROTOCOL_REVISION) ||
      (mFifoQup->AcquireSe == NULL) || (mFifoQup->ReleaseSe == NULL)) {
    Status = EFI_NOT_READY;
    goto Done;
  }
  Status = mFifoQup->AcquireSe (mFifoQup, Target->Wrapper, Target->Engine, Instance, QupCrSeFifo);
  if (EFI_ERROR (Status)) goto Done;
  Status = I2cCrFifoPrepare (Instance, Config->BusFrequencyKHz);
  Prepared = !EFI_ERROR (Status);
  for (Index = 0; !EFI_ERROR (Status) && (Index < DescriptorCount); ++Index) {
    BOOLEAN Read = (Descriptors[Index].Flags & MU_I2C_FLAG_READ) != 0;
    Status = I2cCrFifoTransaction (
        Instance,
        Config->SlaveAddress, Read ? I2C_CMD_READ : I2C_CMD_WRITE,
        (Descriptors[Index].Flags & MU_I2C_FLAG_STOP) == 0 ? I2C_STOP_STRETCH : 0,
        Read ? NULL : Descriptors[Index].Buffer,
        Read ? 0 : Descriptors[Index].Length,
        Read ? Descriptors[Index].Buffer : NULL,
        Read ? Descriptors[Index].Length : 0);
    if (!EFI_ERROR (Status)) Total += Descriptors[Index].Length;
  }
  Cleanup = Prepared ? I2cCrFifoQuiesce (Instance, EFI_ERROR (Status)) : EFI_SUCCESS;
  ReleaseStatus = mFifoQup->ReleaseSe (mFifoQup, Target->Wrapper, Target->Engine, Instance,
                                       !EFI_ERROR (Cleanup));
  if (EFI_ERROR (ReleaseStatus) && !EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "I2CCr: QUP lease release failed: %r\n", ReleaseStatus));
    Status = ReleaseStatus;
  }
  if (EFI_ERROR (Cleanup)) {
    DEBUG ((DEBUG_ERROR, "I2CCr: quiesce failed %r; SE fenced\n", Cleanup));
    if (!EFI_ERROR (Status)) Status = Cleanup;
  }
Done:
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  if (Transferred != NULL) *Transferred = Total;
  return Status;
}

STATIC I2C_CR_INSTANCE *
I2cCrInstanceFromProtocol (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  return (This == NULL) ? NULL : BASE_CR (This, I2C_CR_INSTANCE, Protocol);
}

STATIC EFI_STATUS
EFIAPI
I2cCrMuTransfer (
  IN EFI_I2C_CR_PROTOCOL *This,
  IN MU_I2C_SLAVE_CONFIG *Config,
  IN MU_I2C_DESCRIPTOR   *Descriptors,
  IN UINT16               DescriptorCount,
  IN MU_I2C_CALLBACK      Callback OPTIONAL,
  IN VOID                *Context OPTIONAL,
  IN UINT32               Delay,
  OUT UINT32             *Transferred OPTIONAL
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  /* Switch the selected controller into GENI FIFO mode for each request. */
  if ((Instance != NULL) && (Instance->Protocol.FifoTransfer != NULL) &&
      (I2cCrTarget(Instance) != NULL)) {
    return Instance->Protocol.FifoTransfer (
        This, Config, Descriptors, DescriptorCount,
        Callback, Context, Delay, Transferred);
  }
  /* Every Crane I2C transaction is owned by a native GENI transport.  A
     non-native instance gets a deterministic unsupported result. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
I2cCrSetBusFrequency (
  IN EFI_I2C_CR_PROTOCOL *This,
  IN OUT UINTN           *BusClockHertz
  )
{
  I2C_CR_INSTANCE *Instance;
  UINTN            ActualHz;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (BusClockHertz == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (*BusClockHertz >= 1000000U) {
    ActualHz = 1000000U;
  } else if (*BusClockHertz >= 400000U) {
    ActualHz = 400000U;
  } else if (*BusClockHertz >= 100000U) {
    ActualHz = 100000U;
  } else {
    return EFI_UNSUPPORTED;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }
  Instance->Protocol.BusFrequencyKHz = (UINT32)(ActualHz / 1000U);
  *BusClockHertz = ActualHz;
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
I2cCrReset (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  if (Instance == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (InterlockedCompareExchange32 (&Instance->Busy, 0, 1) != 0) {
    return EFI_ALREADY_STARTED;
  }
  InterlockedCompareExchange32 (&Instance->Busy, 1, 0);
  /* A native GENI reset is performed by the FIFO quiesce path after every
     transaction.  This protocol has no separate controller-reset operation. */
  return EFI_UNSUPPORTED;
}

STATIC EFI_STATUS
EFIAPI
I2cCrStartRequest (
  IN EFI_I2C_CR_PROTOCOL    *This,
  IN UINTN                   SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET *RequestPacket,
  IN EFI_EVENT               Event OPTIONAL,
  OUT EFI_STATUS            *I2cStatus OPTIONAL
  )
{
  I2C_CR_INSTANCE      *Instance;
  MU_I2C_SLAVE_CONFIG   Config;
  MU_I2C_DESCRIPTOR    *Descriptors;
  EFI_STATUS            Status;
  UINTN                 Index;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (RequestPacket == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((SlaveAddress & I2C_ADDRESSING_10_BIT) != 0) {
    return ((SlaveAddress & ~((UINTN)I2C_ADDRESSING_10_BIT | 0x3FFU)) != 0)
             ? EFI_NOT_FOUND : EFI_UNSUPPORTED;
  }
  if ((SlaveAddress & ~(UINTN)0x7FU) != 0) {
    return EFI_NOT_FOUND;
  }
  if (RequestPacket->OperationCount == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (RequestPacket->OperationCount > MAX_UINT16) {
    return EFI_BAD_BUFFER_SIZE;
  }

  ZeroMem (&Config, sizeof (Config));
  Config.BusFrequencyKHz = Instance->Protocol.BusFrequencyKHz;
  Config.SlaveAddress = (UINT32)SlaveAddress;
  Config.Mode = MU_I2C_MODE_I2C;
  Config.SlaveMaxClockStretchUs = I2C_CR_DEFAULT_STRETCH_US;
  Descriptors = AllocateZeroPool (
      RequestPacket->OperationCount * sizeof (*Descriptors));
  if (Descriptors == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = EFI_SUCCESS;
  for (Index = 0; Index < RequestPacket->OperationCount; ++Index) {
    EFI_I2C_OPERATION *Operation;

    Operation = &RequestPacket->Operation[Index];
    if ((Operation->Flags & ~I2C_FLAG_READ) != 0) {
      Status = EFI_UNSUPPORTED;
      break;
    }
    if (Operation->LengthInBytes == 0) {
      /* The native descriptor path does not support quick probes. */
      Status = EFI_UNSUPPORTED;
      break;
    }
    if (Operation->Buffer == NULL) {
      Status = EFI_INVALID_PARAMETER;
      break;
    }
    Descriptors[Index].Buffer = Operation->Buffer;
    Descriptors[Index].Length = Operation->LengthInBytes;
    /* Each operation carries its own repeated START and slave address. */
    Descriptors[Index].Flags = MU_I2C_FLAG_START |
        (((Operation->Flags & I2C_FLAG_READ) != 0)
          ? MU_I2C_FLAG_READ : MU_I2C_FLAG_WRITE);
    if (Index + 1 == RequestPacket->OperationCount) {
      Descriptors[Index].Flags |= MU_I2C_FLAG_STOP;
    }
  }
  if (!EFI_ERROR (Status)) {
    if ((Instance->Protocol.FifoTransfer != NULL) &&
        (I2cCrTarget(Instance) != NULL)) {
      UINT32 Transferred;
      Transferred = 0;
      Status = (Event != NULL) ? EFI_UNSUPPORTED : Instance->Protocol.FifoTransfer (
          &Instance->Protocol, &Config, Descriptors,
          (UINT16)RequestPacket->OperationCount,
          NULL, NULL, 0, &Transferred);
      if (I2cStatus != NULL) *I2cStatus = Status;
    } else {
      Status = EFI_UNSUPPORTED;
      if (I2cStatus != NULL) {
        *I2cStatus = Status;
      }
    }
  }
  FreePool (Descriptors);
  return Status;
}

STATIC VOID
I2cCrInitializeProtocol (
  OUT EFI_I2C_CR_PROTOCOL *Protocol
  )
{
  Protocol->Revision        = EFI_I2C_CR_PROTOCOL_REVISION;
  Protocol->SetBusFrequency = I2cCrSetBusFrequency;
  Protocol->Reset           = I2cCrReset;
  Protocol->StartRequest    = I2cCrStartRequest;
  Protocol->MuTransfer      = I2cCrMuTransfer;
  Protocol->FifoTransfer    = I2cCrFifoMuTransfer;
}

/* Target storage is immutable and protected by the provider's boot-services
   lifetime. Validate the entire publication set before allocation or hardware
   operations so a malformed later bus cannot leave an earlier bus exposed. */
STATIC BOOLEAN
I2cCrClockName (CONST CHAR8 *Name)
{
  if (Name == NULL || Name[0] == 0) return FALSE;
  for (UINTN I=0; I<64; ++I) {
    if (Name[I] == 0) return TRUE;
    if ((UINT8)Name[I] < 33 || (UINT8)Name[I] > 126) return FALSE;
  }
  return FALSE;
}

STATIC EFI_STATUS
I2cCrValidateTargets (EFI_QUP_CR_PROTOCOL *Qup,
                     CONST CR_TARGET_QUP_CONTEXT *Context)
{
  if (Context == NULL || Context->Qup == NULL || !Context->QupCount ||
      Context->I2cCount > CR_TARGET_QUP_MAX_I2C_ENDPOINTS ||
      (Context->I2cCount && Context->I2c == NULL)) return EFI_COMPROMISED_DATA;
  for (UINTN I=0; I<Context->I2cCount; ++I) {
    CONST I2C_CR_TARGET *T=&Context->I2c[I];
    CONST CR_TARGET_QUP_WRAPPER *W=NULL;
    if (!T->Instance || T->Engine >= CR_TARGET_QUP_MAX_SERIAL_ENGINES ||
        !T->Base || (T->Base & 3U) || T->Size < SE_DMA_GENERAL_CFG+4 ||
        T->Base > MAX_UINTN || T->Size > MAX_UINTN-(UINTN)T->Base ||
        T->SourceClockHz != 19200000 ||
        (T->DefaultFrequencyKHz != 100 && T->DefaultFrequencyKHz != 400 &&
         T->DefaultFrequencyKHz != 1000) || !I2cCrClockName(T->ClockController))
      return EFI_COMPROMISED_DATA;
    for (UINTN C=0; C<ARRAY_SIZE(T->Clocks); ++C)
      if (!I2cCrClockName(T->Clocks[C])) return EFI_COMPROMISED_DATA;
    EFI_STATUS S=Qup->GetWrapper(Qup,T->Wrapper,&W);
    if (EFI_ERROR(S) || W == NULL || W->Id != T->Wrapper ||
        W->SerialEngineCount > CR_TARGET_QUP_MAX_SERIAL_ENGINES ||
        T->Engine >= W->SerialEngineCount ||
        T->Base != W->SerialEngineBase[T->Engine] ||
        T->Size != W->SerialEngineSize ||
        W->SerialEngineInterconnect[T->Engine] == NULL)
      return EFI_COMPROMISED_DATA;
    for (UINTN P=0; P<ARRAY_SIZE(T->Pins); ++P) {
      CONST CR_TARGET_QUP_I2C_PIN *Pin=&T->Pins[P];
      if (Pin->Pin >= GPIO_PINS_MAX || Pin->Function == GPIO_FUNC_NORMAL ||
          (UINT32)Pin->Function > GPIO_FUNC_MAX ||
          ((UINT32)Pin->Pull > GPIO_PULL_MAX && Pin->Pull != GPIO_PULL_UNCHANGE) ||
          ((UINT32)Pin->DriveStrength > GPIO_DRIVE_STRENGTH_MAX &&
           Pin->DriveStrength != GPIO_DRIVE_STRENGTH_UNCHANGE))
        return EFI_COMPROMISED_DATA;
      for (UINTN J=0; J<=I; ++J) {
        CONST I2C_CR_TARGET *Other=&Context->I2c[J];
        for (UINTN K=0; K<(J==I?P:ARRAY_SIZE(Other->Pins)); ++K)
          if (Pin->Pin == Other->Pins[K].Pin) return EFI_COMPROMISED_DATA;
      }
    }
    for (UINTN J=0; J<I; ++J)
      if (T->Instance == Context->I2c[J].Instance ||
          (T->Wrapper == Context->I2c[J].Wrapper && T->Engine == Context->I2c[J].Engine))
        return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
I2CCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS       Status;
  EFI_STATUS       FailureStatus;
  EFI_QUP_CR_PROTOCOL *Qup = NULL;
  CONST CR_TARGET_QUP_CONTEXT *Context=NULL;
  UINTN Index;

  (VOID)ImageHandle;
  (VOID)SystemTable;
  if (mInstances != NULL) {
    return EFI_ALREADY_STARTED;
  }
  Status = gBS->LocateProtocol (&gEfiQupCrProtocolGuid, NULL, (VOID **)&Qup);
  if (EFI_ERROR (Status)) return Status;
  if ((Qup == NULL) || (Qup->Revision < EFI_QUP_CR_PROTOCOL_REVISION) ||
      (Qup->Revision >> 16) != (EFI_QUP_CR_PROTOCOL_REVISION >> 16) ||
      Qup->GetContext == NULL || Qup->GetWrapper == NULL ||
      Qup->AcquireSe == NULL || Qup->ReleaseSe == NULL) return EFI_UNSUPPORTED;
  Status=Qup->GetContext(Qup,&Context);
  if (EFI_ERROR(Status)) return Status;
  Status=I2cCrValidateTargets(Qup,Context);
  if (EFI_ERROR(Status)) return Status;
  if (!Context->I2cCount) return EFI_NOT_FOUND;
  mInstances = AllocateZeroPool (Context->I2cCount * sizeof (*mInstances));
  if (mInstances == NULL) return EFI_OUT_OF_RESOURCES;
  for (Index = 0; Index < Context->I2cCount; ++Index) {
    mInstances[Index].Target=&Context->I2c[Index];
    I2cCrInitializeProtocol (&mInstances[Index].Protocol);
    mInstances[Index].Protocol.Instance = Context->I2c[Index].Instance;
    mInstances[Index].Protocol.BusFrequencyKHz = Context->I2c[Index].DefaultFrequencyKHz;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &mInstances[Index].AliasHandle, &gEfiI2cCrProtocolGuid, &mInstances[Index].Protocol, NULL);
    if (EFI_ERROR (Status)) break;
  }
  if (!EFI_ERROR (Status)) return EFI_SUCCESS;
  FailureStatus = Status;
  while (Index != 0) {
    --Index;
    Status = gBS->UninstallMultipleProtocolInterfaces (
        mInstances[Index].AliasHandle, &gEfiI2cCrProtocolGuid, &mInstances[Index].Protocol, NULL);
    if (EFI_ERROR (Status)) {
      /* A published callback must retain both its storage and loaded image. */
      DEBUG ((DEBUG_ERROR, "I2CCr: publication rollback failed: %r (initial %r)\n",
              Status, FailureStatus));
      return EFI_SUCCESS;
    }
    mInstances[Index].AliasHandle = NULL;
  }
  FreePool (mInstances);
  mInstances = NULL;
  return FailureStatus;
}
