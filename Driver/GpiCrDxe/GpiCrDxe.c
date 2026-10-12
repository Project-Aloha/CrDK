/** @file
 *  Linux-compatible Qualcomm GPI-DMA I2C transport.
 *
 *  This driver owns GPI instance 1 and GENI SE9 (Linux i2c9).  The register
 *  layout and TRE encoding follow Linux qcom/gpi.c and qcom-geni-i2c.c.  No
 *  private MU protocol or prebuilt GPI implementation is used.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>
#include <Base.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/SynchronizationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/IoMmu.h>

#include <Library/CrDalLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>

#define GPI_CONTROLLER_ID              1U
#define GPII_ID                        1U
#define GPI_I2C_INSTANCE               10U
#define GPI_SE_INDEX                   1U
#define GPI_SEID                       1U
#define SE_GENI_FW_REVISION_RO         0x068U
#define GPI_I2C_PROTOCOL               3U /* QCOM_GPI_I2C */
#define GPI_SID                        0x56U
#define GPI_DATA_SID                   0x43U /* QUP1 serial-engine DMA */
#define GPI_EE_OFFSET                  0x10000U /* sm8450 match-data offset */
#define GPI_TX_CH                      0U
#define GPI_RX_CH                      1U
#define GPI_RING_ELEMENTS              64U
#define GPI_EVENT_ELEMENTS             256U /* Linux CHAN_TRES << EV_FACTOR */
#define GPI_EL_SIZE                    16U
#define GPI_CMD_TIMEOUT_US             250000U
#define GPI_XFER_TIMEOUT_US            500000U

#define GPI_TRE_DMA                    0x10U
#define GPI_TRE_GO                     0x20U
#define GPI_TRE_CONFIG0                0x22U
#define GPI_FLAG_CHAIN                 BIT (0)
#define GPI_FLAG_IEOB                  BIT (8)
#define GPI_FLAG_IEOT                  BIT (9)
#define GPI_FLAG_BEI                   BIT (10)
#define GPI_FLAG_LINK                  BIT (11)
#define GPI_TYPE(_Type)                ((_Type) << 16)
#define GPI_C0_CLK_DIV                 2U

#define GPI_CH_CTX(_N, _K)             (0x20000U + 0x4000U * (_N) + 0x80U * (_K))
#define GPI_EV_CTX(_N)                 (0x21000U + 0x4000U * (_N))
#define GPI_CH_DB(_N, _K)              (0x22000U + 0x4000U * (_N) + 0x8U * (_K))
#define GPI_EV_DB(_N)                  (0x22100U + 0x4000U * (_N))
#define GPI_CH_CMD(_N)                 (0x23008U + 0x4000U * (_N))
#define GPI_EV_CMD(_N)                 (0x23010U + 0x4000U * (_N))
#define GPI_IRQ_TYPE_MASK(_N)          (0x23088U + 0x4000U * (_N))
#define GPI_CH_CTRL_STATUS(_N)         (0x23090U + 0x4000U * (_N))
#define GPI_EV_CTRL_STATUS(_N)         (0x23094U + 0x4000U * (_N))
#define GPI_CH_CTRL_MASK(_N)           (0x23098U + 0x4000U * (_N))
#define GPI_EV_CTRL_MASK(_N)           (0x2309CU + 0x4000U * (_N))
#define GPI_CH_CTRL_CLEAR(_N)          (0x230A0U + 0x4000U * (_N))
#define GPI_EV_CTRL_CLEAR(_N)          (0x230A4U + 0x4000U * (_N))
#define GPI_IEOB_MASK(_N)              (0x230B8U + 0x4000U * (_N))
#define GPI_IEOB_CLEAR(_N)             (0x230C0U + 0x4000U * (_N))
#define GPI_GLOBAL_STATUS(_N)          (0x23100U + 0x4000U * (_N))
#define GPI_GLOBAL_ENABLE(_N)          (0x23108U + 0x4000U * (_N))
#define GPI_GLOBAL_CLEAR(_N)           (0x23110U + 0x4000U * (_N))
#define GPI_GENERAL_STATUS(_N)         (0x23118U + 0x4000U * (_N))
#define GPI_GENERAL_ENABLE(_N)         (0x23120U + 0x4000U * (_N))
#define GPI_GENERAL_CLEAR(_N)          (0x23128U + 0x4000U * (_N))
#define GPI_IRQ_TYPE(_N)               (0x23180U + 0x4000U * (_N))
#define GPI_MSI_BASE_L(_N)             (0x23188U + 0x4000U * (_N))
#define GPI_MSI_BASE_H(_N)             (0x2318CU + 0x4000U * (_N))
#define GPI_ERROR_LOG(_N)              (0x23200U + 0x4000U * (_N))
#define GPI_SCRATCH(_N, _W)            (0x23400U + 0x4000U * (_N) + 4U * (_W))
#define GPI_CH_SCRATCH(_N, _K, _W)     (0x20060U + 0x4000U * (_N) + 0x80U * (_K) + 4U * (_W))
#define GPI_CH_QOS(_N, _K)             (0x2005cU + 0x4000U * (_N) + 0x80U * (_K))

#define GPI_CONFIG                     0x00U
#define GPI_LENGTH                     0x04U
#define GPI_RING_BASE_L                0x08U
#define GPI_RING_BASE_H                0x0cU
#define GPI_RING_RP_L                  0x10U
#define GPI_RING_RP_H                  0x14U
#define GPI_RING_WP_L                  0x18U
#define GPI_RING_WP_H                  0x1cU
#define GPI_INT_MOD                    0x20U
#define GPI_MSI_L                      0x28U
#define GPI_MSI_H                      0x2cU
#define GPI_RP_UPDATE_L                0x30U
#define GPI_RP_UPDATE_H                0x34U
#define GPI_STATE(_V)                  (((_V) >> 20) & 0xfU)
#define GPI_CH_ALLOCATED               1U
#define GPI_CH_STARTED                 2U

#define SE_GENI_STATUS                 0x040U
#define SE_M_CMD_CTRL                  0x604U
#define SE_M_IRQ_STATUS                0x610U
#define SE_M_CMD_ABORT                 BIT (5)
#define SE_M_IRQ_EN                    0x614U
#define SE_M_IRQ_CLEAR                 0x618U
#define SE_M_GP_LENGTH                 0x910U
#define SE_S_IRQ_CLEAR                 0x648U
#define SE_DMA_MODE                    0x258U
#define SE_DMA_TX_IRQ_CLEAR            0xC44U
#define SE_DMA_RX_IRQ_CLEAR            0xD44U
#define SE_DMA_IF_EN                   0x2004U
#define SE_GSI_EVENT_EN                0xe18U
#define SE_IRQ_EN                      0xe1cU
#define SE_DMA_GENERAL_CFG             0xe30U
#define SE_CGC_CTRL                    0x28U
#define SE_OUTPUT_CTRL                 0x24U
#define SE_FORCE_DEFAULT               0x20U
#define SE_CLK_SEL                     0x7cU
#define SE_SER_M_CLK_CFG              0x48U
#define SE_I2C_SCL_COUNTERS           0x278U
#define SE_DMA_MODE_EN                 BIT (0)
#define SE_DMA_IF_EN_BIT               BIT (0)
#define SE_M_CMD_DONE_EN               BIT (0)
#define SE_M_TX_FIFO_WATERMARK_EN      BIT (30)
#define SE_M_RX_FIFO_WATERMARK_EN      BIT (26)
#define SE_M_RX_FIFO_LAST_EN           BIT (27)
#define SE_M_COMMON_IRQ_EN             (BIT (1) | BIT (2) | BIT (3) | BIT (4) | \
                                       BIT (5) | BIT (6) | BIT (22) | BIT (23) | \
                                       BIT (24) | BIT (25) | BIT (28) | BIT (29))
#define SE_M_I2C_IRQ_EN                (BIT (9) | BIT (10) | BIT (12) | BIT (13))
#define SE_DEFAULT_CGC_EN              0x7fU
#define SE_DMA_GENERAL_CLK_EN          0x0fU

#define GPI_EVENT_XFER_COMPLETE        0x22U
#define GPI_EVENT_QUP_NOTIFY           0x31U
#define GPI_NOTIFY_I2C_ERROR_MASK      (BIT (10) | BIT (12) | BIT (13))
/* Linux DM_I2C_CB_ERR maps GP_IRQ1/3/4 to TCE status bits 6, 8 and 9
   respectively after the five-bit TCE status offset. */
#define GPI_TCE_I2C_ERROR_MASK         (BIT (6) | BIT (8) | BIT (9))
#define GPI_EVENT_SUCCESS              1U
#define GPI_EVENT_EOT                  2U
#define GPI_EVENT_EOB                  4U
#define GPI_EVENT_ERROR                16U

typedef struct {
  VOID                 *Host;
  EFI_PHYSICAL_ADDRESS Device;
  VOID                 *Mapping;
  UINTN                Pages;
  UINT32               Length;
  UINT32               ElSize;
  UINT32               ReadOffset;
  UINT32               DoorbellOffset;
} GPI_RING;

typedef struct {
  VOID                 *Host;
  EFI_PHYSICAL_ADDRESS Device;
  VOID                 *Mapping;
  UINTN                Pages;
} GPI_BUFFER;

typedef struct {
  EFI_GPI_CR_PROTOCOL              Protocol;
  EFI_QUP_CR_PROTOCOL              *Qup;
  EFI_CLOCK_CR_PROTOCOL            *Clock;
  UINT32                           ClockMask;
  BOOLEAN                          ClockCleanupPending;
  EFI_SMMU_CR_PROTOCOL             *Smmu;
  EDKII_IOMMU_PROTOCOL             *Iommu;
  CONST CR_TARGET_GPI_CONTROLLER   *Controller;
  UINTN                            GpiBase;
  UINTN                            SeBase;
  GPI_RING                         TxRing;
  GPI_RING                         RxRing;
  GPI_RING                         EventRing;
  GPI_BUFFER                       Data;
  BOOLEAN                          SeReady;
  BOOLEAN                          CommandFailed;
  BOOLEAN                          EventTouched;
  BOOLEAN                          ChannelTouched[2];
  BOOLEAN                          Initialized;
  BOOLEAN                          HaveQuiesced;
  BOOLEAN                          DmaCleanupFailed;
  BOOLEAN                          DiagnosticLogged;
  BOOLEAN                          DmaDiagnosticLogged;
} GPI_STATE;

STATIC GPI_STATE mGpi;

STATIC VOID
GpiMemoryFence (VOID)
{
#ifdef CRANE_HOST_TEST
  __sync_synchronize ();
#else
  MemoryFence ();
#endif
}

/*
 * Linux's gpi_alloc_*() path ends with wmb() before the context command or
 * doorbell is issued.  MemoryFence() is sufficient to order normal-memory
 * stores, but an ARM DMB does not wait for an earlier Device-nGnRE write to
 * reach the QUP/GPI block.  Use a DSB at the hardware ownership hand-off so
 * the START/doorbell cannot overtake ring-base, scratch, or TRE writes.  The
 * host transport tests still get a full compiler/CPU barrier.
 */
STATIC VOID
GpiDeviceFence (VOID)
{
#ifdef CRANE_HOST_TEST
  __sync_synchronize ();
#else
  __asm__ volatile ("dsb sy" ::: "memory");
#endif
}

STATIC EFI_STATUS
GpiConfigurePins (VOID)
{
  EFI_GPIO_CR_PROTOCOL *Gpio;
  EFI_STATUS            Status;
  UINTN                 Index;

  Gpio = NULL;
  Status = gBS->LocateProtocol (&gEfiGpioCrProtocolGuid, NULL, (VOID **)&Gpio);
  if (EFI_ERROR (Status) || (Gpio == NULL) || (Gpio->ConfigGpio == NULL)) {
    DEBUG ((DEBUG_WARN, "GPI: GPIO pinmux protocol unavailable: %r\n", Status));
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  /* Linux sm8450 GPIO32/33 PINGROUP lists qup9 as function 1. */
  for (Index = 0; Index < 2; ++Index) {
    GpioConfigParams Config;
    ZeroMem (&Config, sizeof (Config));
    Config.PinNumber = (UINT16)(32U + Index);
    Config.OutputEnable = FALSE;
    Config.FunctionSel = GPIO_FUNC_1;
    Config.Pull = GPIO_PULL_UP;
    Config.DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE;
    Config.OutputValue = GPIO_VALUE_UNCHANGE;
    Status = Gpio->ConfigGpio (Gpio, &Config);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_WARN, "GPI: GPIO%u pinmux failed: %r\n", Config.PinNumber, Status));
      return Status;
    }
  }
  return EFI_SUCCESS;
}

STATIC UINT32 GpiRead (IN UINTN Offset) { return MmioRead32 (mGpi.GpiBase + Offset); }
STATIC VOID GpiWrite (IN UINTN Offset, IN UINT32 Value) { MmioWrite32 (mGpi.GpiBase + Offset, Value); }
STATIC UINT32 GpiSeRead (IN UINTN Offset) { return MmioRead32 (mGpi.SeBase + Offset); }
STATIC VOID GpiSeWrite (IN UINTN Offset, IN UINT32 Value) { MmioWrite32 (mGpi.SeBase + Offset, Value); }

STATIC VOID
GpiUpdate (IN UINTN Offset, IN UINT32 Mask, IN UINT32 Value)
{
  GpiWrite (Offset, (GpiRead (Offset) & ~Mask) | (Value & Mask));
}

/* Linux gpi_config_interrupts() establishes these sources before ALLOCATE.
   This synchronous transport polls their status: keep the top-level delivery
   mask clear because it has no GIC handler, but initialize source enables,
   IRQ (not MSI) selection, scratch and stale status ourselves. Call only
   after confirming this GPII is idle and acquiring its SE lease. */
STATIC VOID
GpiConfigurePolling (VOID)
{
  GpiUpdate (GPI_IRQ_TYPE_MASK (GPII_ID), 0x7fU, 0);
  GpiWrite (GPI_CH_CTRL_CLEAR (GPII_ID), 3U);
  GpiWrite (GPI_EV_CTRL_CLEAR (GPII_ID), 1U);
  GpiWrite (GPI_IEOB_CLEAR (GPII_ID), 1U);
  GpiWrite (GPI_GLOBAL_CLEAR (GPII_ID), 1U);
  GpiWrite (GPI_GENERAL_CLEAR (GPII_ID), 0xfU);
  GpiUpdate (GPI_CH_CTRL_MASK (GPII_ID), 3U, 3U);
  GpiUpdate (GPI_EV_CTRL_MASK (GPII_ID), 1U, 1U);
  GpiUpdate (GPI_IEOB_MASK (GPII_ID), 1U, 1U);
  /* Global ERROR_INT is bit 0; the separate general source has four bits.
     Preserve all fields outside the documented masks, as Linux does. */
  GpiUpdate (GPI_GLOBAL_ENABLE (GPII_ID), 1U, 1U);
  GpiUpdate (GPI_GENERAL_ENABLE (GPII_ID), 0xfU, 0xfU);
  GpiWrite (GPI_MSI_BASE_L (GPII_ID), 0);
  GpiWrite (GPI_MSI_BASE_H (GPII_ID), 0);
  GpiWrite (GPI_SCRATCH (GPII_ID, 0), 0);
  GpiWrite (GPI_SCRATCH (GPII_ID, 1), 0);
  GpiWrite (GPI_ERROR_LOG (GPII_ID), 0);
  GpiUpdate (GPI_IRQ_TYPE (GPII_ID), 1U, 1U);
  DEBUG ((DEBUG_ERROR, "GPI: polling init gpii=%u type-mask=0x%x CH-mask=0x%x EV-mask=0x%x\n",
          GPII_ID, GpiRead (GPI_IRQ_TYPE_MASK (GPII_ID)),
          GpiRead (GPI_CH_CTRL_MASK (GPII_ID)), GpiRead (GPI_EV_CTRL_MASK (GPII_ID))));
}

STATIC EFI_STATUS
GpiCheckErrors (VOID)
{
  UINT32 Global = GpiRead (GPI_GLOBAL_STATUS (GPII_ID)) & 1U;
  UINT32 General = GpiRead (GPI_GENERAL_STATUS (GPII_ID)) & 0xfU;
  if ((Global | General) == 0) return EFI_SUCCESS;
  DEBUG ((DEBUG_ERROR, "GPI: controller error global=0x%x general=0x%x log=0x%x\n",
          Global, General, GpiRead (GPI_ERROR_LOG (GPII_ID))));
  /* Leave the fault latched for diagnosis. Cleanup may release ownership
     only if fresh STOP/RESET acknowledgements establish quiescence. */
  return EFI_DEVICE_ERROR;
}

/* Do not return pages to the allocator while a failed Unmap may leave an
   SMMU translation live.  Retain the descriptor for diagnosis and fence
   initialization so a retry cannot overwrite the retained ownership. */
STATIC VOID
GpiDmaFree (IN OUT GPI_BUFFER *Buffer)
{
  EFI_STATUS Status;
  if ((Buffer == NULL) || (Buffer->Host == NULL) || (mGpi.Iommu == NULL)) return;
  if (Buffer->Mapping != NULL) {
    Status = mGpi.Iommu->Unmap (mGpi.Iommu, Buffer->Mapping);
    if (EFI_ERROR (Status)) {
      mGpi.DmaCleanupFailed = TRUE;
      return;
    }
    Buffer->Mapping = NULL;
  }
  Status = mGpi.Iommu->FreeBuffer (mGpi.Iommu, Buffer->Pages, Buffer->Host);
  if (EFI_ERROR (Status)) {
    mGpi.DmaCleanupFailed = TRUE;
    return;
  }
  ZeroMem (Buffer, sizeof (*Buffer));
}

STATIC EFI_STATUS
GpiDmaAllocate (IN UINTN Pages, IN UINT16 Sid, OUT GPI_BUFFER *Buffer)
{
  EFI_STATUS Status;
  UINTN      Bytes;

  if ((Buffer == NULL) || (Pages == 0) || (Pages > MAX_UINTN / EFI_PAGE_SIZE) ||
      (mGpi.Iommu == NULL) ||
      (mGpi.Iommu->Revision < EDKII_IOMMU_PROTOCOL_REVISION) ||
      (mGpi.Iommu->AllocateBuffer == NULL) || (mGpi.Iommu->Map == NULL) ||
      (mGpi.Iommu->Unmap == NULL) || (mGpi.Iommu->FreeBuffer == NULL) ||
      (mGpi.Iommu->SetAttributeById == NULL)) {
    return EFI_UNSUPPORTED;
  }
  if (mGpi.DmaCleanupFailed) return EFI_DEVICE_ERROR;
  ZeroMem (Buffer, sizeof (*Buffer));
  Status = mGpi.Iommu->AllocateBuffer (
      mGpi.Iommu, AllocateAnyPages, EfiBootServicesData, Pages, &Buffer->Host, 0);
  if (EFI_ERROR (Status)) return Status;
  Buffer->Pages = Pages;
  Bytes = Pages * EFI_PAGE_SIZE;
  Status = mGpi.Iommu->Map (mGpi.Iommu,
      EdkiiIoMmuOperationBusMasterCommonBuffer64, Buffer->Host, &Bytes,
      &Buffer->Device, &Buffer->Mapping);
  if (EFI_ERROR (Status)) {
    /* Map outputs are not valid on failure. */
    Buffer->Mapping = NULL;
    GpiDmaFree (Buffer);
    return Status;
  }
  if (Bytes != Pages * EFI_PAGE_SIZE) {
    GpiDmaFree (Buffer);
    return EFI_BAD_BUFFER_SIZE;
  }
  Status = mGpi.Iommu->SetAttributeById (
      mGpi.Iommu, 0x15000000ULL, Sid, Buffer->Mapping,
      EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE);
  if (EFI_ERROR (Status)) {
    GpiDmaFree (Buffer);
    return Status;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiRingAllocate (IN UINT32 Elements, OUT GPI_RING *Ring)
{
  EFI_STATUS Status;
  GPI_BUFFER Buffer = {0};
  UINTN Pages;

  if (Ring == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem (Ring, sizeof (*Ring));
  Ring->ElSize = GPI_EL_SIZE;
  Ring->Length = Elements * GPI_EL_SIZE;
  Pages = (Ring->Length + EFI_PAGE_SIZE - 1U) / EFI_PAGE_SIZE;
  Status = GpiDmaAllocate (Pages, GPI_SID, &Buffer);
  Ring->Host = Buffer.Host;
  Ring->Device = Buffer.Device;
  Ring->Mapping = Buffer.Mapping;
  Ring->Pages = Buffer.Pages;
  if (EFI_ERROR (Status)) return Status;
  /* Transfer ring WP points to the next free TRE, as in Linux. */
  Ring->DoorbellOffset = 0;
  ZeroMem (Ring->Host, Ring->Pages * EFI_PAGE_SIZE);
  /* Linux gpi_alloc_ring() uses dma_alloc_coherent().  Our IOMMU common
     buffer can be cached, so publish initialization before hardware owns
     the event ring.  Otherwise dirty zero-filled cache lines can later
     overwrite DMA-produced EREs.  Clean the entire zeroed allocation,
     including padding, before any ring address reaches GPI. */
  WriteBackDataCacheRange (Ring->Host, Ring->Pages * EFI_PAGE_SIZE);
  return EFI_SUCCESS;
}

STATIC VOID
GpiRingFree (IN OUT GPI_RING *Ring)
{
  GPI_BUFFER Buffer;
  if ((Ring == NULL) || (Ring->Host == NULL)) return;
  Buffer.Host = Ring->Host;
  Buffer.Device = Ring->Device;
  Buffer.Mapping = Ring->Mapping;
  Buffer.Pages = Ring->Pages;
  GpiDmaFree (&Buffer);
  Ring->Mapping = Buffer.Mapping;
  if (Buffer.Host == NULL) ZeroMem (Ring, sizeof (*Ring));
}

STATIC EFI_STATUS
GpiCommand (IN BOOLEAN Event, IN UINT8 Channel, IN UINT8 Opcode, IN UINT32 State)
{
  UINTN Command = Event ? GPI_EV_CMD (GPII_ID) : GPI_CH_CMD (GPII_ID);
  UINTN Context = Event ? GPI_EV_CTX (GPII_ID) : GPI_CH_CTX (GPII_ID, Channel);
  UINTN IrqStatus = Event ? GPI_EV_CTRL_STATUS (GPII_ID) : GPI_CH_CTRL_STATUS (GPII_ID);
  UINTN IrqClear = Event ? GPI_EV_CTRL_CLEAR (GPII_ID) : GPI_CH_CTRL_CLEAR (GPII_ID);
  UINT32 Mask = Event ? BIT (0) : BIT (Channel);
  UINT32 Elapsed;
  UINT32 ControlStatus;
  BOOLEAN Diagnostic;
  Diagnostic = !mGpi.DiagnosticLogged;
  if (Diagnostic) {
    /* The first command is the point at which the real GPI block first
       observes an opcode.  Keep this one-shot snapshot before any W1C or
       command write so a board stop can be separated from a rejected
       command without changing the command sequence. */
    DEBUG ((DEBUG_ERROR,
            "GPI: cmd begin event=%u ch=%u op=%u expect=%u base=0x%lx se=0x%lx\n",
            Event, Channel, Opcode, State, mGpi.GpiBase, mGpi.SeBase));
    DEBUG ((DEBUG_ERROR,
            "GPI: cmd pre ctx=0x%x ctrl=0x%x irq-type=0x%x irq-mask=0x%x ev-mask=0x%x ch-mask=0x%x global=0x%x general=0x%x err=0x%x se-status=0x%x\n",
            GpiRead (Context + GPI_CONFIG), GpiRead (IrqStatus),
            GpiRead (GPI_IRQ_TYPE (GPII_ID)),
            GpiRead (GPI_IRQ_TYPE_MASK (GPII_ID)),
            GpiRead (GPI_EV_CTRL_MASK (GPII_ID)),
            GpiRead (GPI_CH_CTRL_MASK (GPII_ID)),
            GpiRead (GPI_GLOBAL_STATUS (GPII_ID)),
            GpiRead (GPI_GENERAL_STATUS (GPII_ID)),
            GpiRead (GPI_ERROR_LOG (GPII_ID)), GpiSeRead (SE_GENI_STATUS)));
    mGpi.DiagnosticLogged = TRUE;
  }
  /* Linux waits for a fresh control acknowledgement, not merely the target
     state: RESET can leave an already-allocated event ring in that state.
     Both channel and event DEALLOC acknowledge without changing HW state. */
  GpiWrite (IrqClear, Mask);
  if (Diagnostic) {
    DEBUG ((DEBUG_ERROR, "GPI: cmd after-clear ctrl=0x%x\n", GpiRead (IrqStatus)));
  }
  GpiWrite (Command, ((UINT32)Opcode << 24) | (Event ? 0U : Channel));
  if (Diagnostic) {
    DEBUG ((DEBUG_ERROR, "GPI: cmd after-write\n"));
  }
  for (Elapsed = 0; Elapsed < GPI_CMD_TIMEOUT_US; Elapsed += 10U) {
    /* Read the source status exactly once per poll.  The Qualcomm GPI
       status registers are documented as W1C, but some revisions have
       observable clear-on-read behaviour when the aggregate IRQ mask is
       disabled for polling.  Do not consume an acknowledgement in the
       diagnostic print and then read zero in the state check. */
    ControlStatus = GpiRead (IrqStatus);
    if (Diagnostic && ((Elapsed == 0U) || (Elapsed == 100000U) ||
                       (Elapsed == 200000U))) {
      DEBUG ((DEBUG_ERROR,
              "GPI: cmd poll us=%u ctx=0x%x ctrl=0x%x irq-type=0x%x global=0x%x general=0x%x err=0x%x se-status=0x%x\n",
              Elapsed, GpiRead (Context + GPI_CONFIG), ControlStatus,
              GpiRead (GPI_IRQ_TYPE (GPII_ID)),
              GpiRead (GPI_GLOBAL_STATUS (GPII_ID)),
              GpiRead (GPI_GENERAL_STATUS (GPII_ID)),
              GpiRead (GPI_ERROR_LOG (GPII_ID)), GpiSeRead (SE_GENI_STATUS)));
    }
    if ((ControlStatus & Mask) != 0) {
      if (Diagnostic) {
        DEBUG ((DEBUG_ERROR, "GPI: cmd ack observed ctrl=0x%x\n", ControlStatus));
      }
      UINT32 ContextValue = GpiRead (Context + GPI_CONFIG);
      UINT32 Current = GPI_STATE (ContextValue);
      if (Diagnostic) {
        DEBUG ((DEBUG_ERROR, "GPI: cmd ack state=%u raw=0x%x\n", Current,
                ContextValue));
      }
      GpiWrite (IrqClear, Mask);
      if (Diagnostic) {
        DEBUG ((DEBUG_ERROR, "GPI: cmd ack clear issued ctrl=0x%x\n", ControlStatus & ~Mask));
      }
      /*
       * Linux completes a command from the channel/event control source and
       * handles global/general errors in a separate interrupt path.  Polling
       * must follow the same ordering: consume the command acknowledgement
       * and sample the context first, then inspect the error sources.  On
       * SM8450, reading an aggregate error status while a control source is
       * being delivered can stall the MMIO transaction even though the
       * command has already reached its target state.
       */
      if (EFI_ERROR (GpiCheckErrors ())) {
        mGpi.CommandFailed = TRUE;
        return EFI_DEVICE_ERROR;
      }
      if ((Opcode == 10U) || (Current == State)) {
        if (Diagnostic) DEBUG ((DEBUG_ERROR, "GPI: cmd success\n"));
        return EFI_SUCCESS;
      }
      /* Linux ignores STOP_IN_PROC and awaits the final STOP interrupt. */
      if (Current != 4U) {
        DEBUG ((DEBUG_ERROR, "GPI: command rejected event=%u ch=%u op=%u state=%u expected=%u\n",
                Event, Channel, Opcode, Current, State));
        mGpi.CommandFailed = TRUE;
        return EFI_DEVICE_ERROR;
      }
    }
    MicroSecondDelay (10);
  }
  /* A controller fault can suppress the control acknowledgement entirely.
     Sample the aggregate sources once after the bounded wait so the caller
     receives a device error instead of disguising a latched GPI fault as a
     generic command timeout. */
  if (EFI_ERROR (GpiCheckErrors ())) {
    mGpi.CommandFailed = TRUE;
    return EFI_DEVICE_ERROR;
  }
  DEBUG ((DEBUG_ERROR, "GPI: command timeout event=%u ch=%u op=%u state=%u control=0x%x\n",
          Event, Channel, Opcode, GPI_STATE (GpiRead (Context + GPI_CONFIG)),
          GpiRead (IrqStatus)));
  mGpi.CommandFailed = TRUE;
  return EFI_TIMEOUT;
}

STATIC VOID
GpiConfigureSe (VOID)
{
  UINT32 Value;

  /*
   * Keep the GENI engine in one well-defined mode.  This is the same mode
   * transition as geni_se_select_gpi_mode() in Linux: clear stale IRQs and
   * FIFO enables before asserting GENI_DMA_MODE_EN and the GSI events.
   */
  GpiSeWrite (SE_GSI_EVENT_EN, 0);
  GpiSeWrite (SE_M_IRQ_CLEAR, 0xffffffffU);
  GpiSeWrite (SE_S_IRQ_CLEAR, 0xffffffffU);
  /* geni_se_irq_clear() also clears both GENI DMA interrupt banks.  Leaving
     a stale completion/error latched here can make the first GPI transfer
     look complete before the event ring has received its ERE. */
  GpiSeWrite (SE_DMA_TX_IRQ_CLEAR, 0xffffffffU);
  GpiSeWrite (SE_DMA_RX_IRQ_CLEAR, 0xffffffffU);
  GpiSeWrite (SE_IRQ_EN, 0);
  Value = GpiSeRead (SE_M_IRQ_EN);
  Value &= ~(SE_M_CMD_DONE_EN | SE_M_TX_FIFO_WATERMARK_EN |
             SE_M_RX_FIFO_WATERMARK_EN | SE_M_RX_FIFO_LAST_EN);
  /* The GPI probe need not have been preceded by a FIFO transaction. */
  Value |= SE_M_COMMON_IRQ_EN | SE_M_I2C_IRQ_EN;
  GpiSeWrite (SE_M_IRQ_EN, Value);

  /* geni_se_io_init(): clocks and DMA-side AHB paths must be ungated. */
  Value = GpiSeRead (SE_CGC_CTRL);
  GpiSeWrite (SE_CGC_CTRL, Value | SE_DEFAULT_CGC_EN);
  Value = GpiSeRead (SE_DMA_GENERAL_CFG);
  GpiSeWrite (SE_DMA_GENERAL_CFG, Value | SE_DMA_GENERAL_CLK_EN);
  GpiSeWrite (SE_OUTPUT_CTRL, 0x7fU);
  GpiSeWrite (SE_FORCE_DEFAULT, BIT (0));

  /* qcom-geni-se.c enables the serial-engine DMA interface after loading
     firmware.  GPI uses the already-provisioned I2C firmware, so preserve
     the register's other bits and enable the interface before publishing
     DMA mode and GSI events. */
  Value = GpiSeRead (SE_DMA_IF_EN);
  GpiSeWrite (SE_DMA_IF_EN, Value | SE_DMA_IF_EN_BIT);

  /* Linux qcom-geni-i2c.c: 19.2 MHz / 400 kHz: div=2, high=5, low=11, cycle=22. */
  GpiSeWrite (SE_CLK_SEL, 0);
  GpiSeWrite (SE_SER_M_CLK_CFG, (2U << 4) | BIT (0));
  GpiSeWrite (SE_I2C_SCL_COUNTERS, 22U | (11U << 10) | (5U << 20));
  GpiSeWrite (SE_DMA_MODE, SE_DMA_MODE_EN);
  GpiSeWrite (SE_GSI_EVENT_EN, 0xfU);
}

STATIC EFI_STATUS
GpiValidateRingContext (IN UINTN Base, IN GPI_RING *Ring,
                        IN UINT32 Config, IN UINT32 ConfigMask)
{
  UINT32 ActualConfig = GpiRead (Base + GPI_CONFIG);
  UINT32 ActualLength = GpiRead (Base + GPI_LENGTH);
  EFI_PHYSICAL_ADDRESS ActualBase = GpiRead (Base + GPI_RING_BASE_L);
  ActualBase |= (EFI_PHYSICAL_ADDRESS)GpiRead (Base + GPI_RING_BASE_H) << 32;
  if (((ActualConfig & ConfigMask) != Config) ||
      (ActualLength != Ring->Length) || (ActualBase != Ring->Device)) {
    DEBUG ((DEBUG_ERROR,
            "GPI: ring context rejected ctx=0x%lx config=0x%x length=0x%x base=0x%lx expected=0x%lx\n",
            Base, ActualConfig, ActualLength, ActualBase, Ring->Device));
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiProgramChannel (IN UINT8 Channel, IN GPI_RING *Ring, IN BOOLEAN Output)
{
  UINTN Base = GPI_CH_CTX (GPII_ID, Channel);
  DEBUG ((DEBUG_ERROR,
          "GPI: channel program begin ch=%u output=%u ctx=0x%lx ring=0x%lx len=0x%x\n",
          Channel, Output, Base, Ring->Device, Ring->Length));
  GpiWrite (Base + GPI_CONFIG, (GPI_EL_SIZE << 24) | (Output ? BIT (3) : 0) | 2U);
  GpiWrite (Base + GPI_LENGTH, Ring->Length);
  GpiWrite (Base + GPI_RING_BASE_L, (UINT32)Ring->Device);
  GpiWrite (Base + GPI_RING_BASE_H, (UINT32)(Ring->Device >> 32));
  /* Linux programs the ring doorbell's high half during allocation.  The
     low half is then advanced for each queued TRE. */
  GpiWrite (GPI_CH_DB (GPII_ID, Channel) + sizeof (UINT32),
            (UINT32)(Ring->Device >> 32));
  GpiWrite (GPI_CH_SCRATCH (GPII_ID, Channel, 0),
            ((UINT32)(Channel ^ 1U) << 16) |
            (GPI_I2C_PROTOCOL << 4) | GPI_SEID);
  GpiWrite (GPI_CH_SCRATCH (GPII_ID, Channel, 1), 0);
  GpiWrite (GPI_CH_SCRATCH (GPII_ID, Channel, 2), 0);
  GpiWrite (GPI_CH_SCRATCH (GPII_ID, Channel, 3), 0);
  GpiWrite (GPI_CH_QOS (GPII_ID, Channel), 1);
  /* Linux gpi_alloc_chan() issues wmb() before the channel can be started. */
  GpiMemoryFence ();
  GpiDeviceFence ();
  DEBUG ((DEBUG_ERROR, "GPI: channel program complete ch=%u config=0x%x db=0x%lx\n",
          Channel, GpiRead (Base + GPI_CONFIG),
          GpiRead (GPI_CH_DB (GPII_ID, Channel))));
  /* CHSTATE is hardware-owned; check only EL_SIZE, ERIDX, DIR and PROTO. */
  return GpiValidateRingContext (
           Base, Ring, (GPI_EL_SIZE << 24) | (Output ? BIT (3) : 0) | 2U,
           0xff07c00fU);
}

STATIC EFI_STATUS
GpiProgramEvent (VOID)
{
  UINTN Base = GPI_EV_CTX (GPII_ID);
  EFI_STATUS Status;
  DEBUG ((DEBUG_ERROR,
          "GPI: event program begin ctx=0x%lx ring=0x%lx len=0x%x\n",
          Base, mGpi.EventRing.Device, mGpi.EventRing.Length));
  mGpi.EventRing.DoorbellOffset = mGpi.EventRing.Length - GPI_EL_SIZE;
  GpiWrite (Base + GPI_CONFIG, (GPI_EL_SIZE << 24) | BIT (16) | 2U);
  GpiWrite (Base + GPI_LENGTH, mGpi.EventRing.Length);
  GpiWrite (Base + GPI_RING_BASE_L, (UINT32)mGpi.EventRing.Device);
  GpiWrite (Base + GPI_RING_BASE_H, (UINT32)(mGpi.EventRing.Device >> 32));
  GpiWrite (Base + GPI_INT_MOD, 0);
  GpiWrite (Base + GPI_MSI_L, 0);
  GpiWrite (Base + GPI_MSI_H, 0);
  GpiWrite (Base + GPI_RP_UPDATE_L, 0);
  GpiWrite (Base + GPI_RP_UPDATE_H, 0);
  GpiWrite (GPI_EV_DB (GPII_ID) + sizeof (UINT32),
            (UINT32)(mGpi.EventRing.Device >> 32));
  /* The event ring must be fully configured before its initial WP is exposed. */
  GpiMemoryFence ();
  GpiDeviceFence ();
  Status = GpiValidateRingContext (
             Base, &mGpi.EventRing, (GPI_EL_SIZE << 24) | BIT (16) | 2U,
             0xff01000fU);
  if (EFI_ERROR (Status)) return Status;
  GpiWrite (GPI_EV_DB (GPII_ID),
            (UINT32)(mGpi.EventRing.Device + mGpi.EventRing.DoorbellOffset));
  DEBUG ((DEBUG_ERROR, "GPI: event program complete config=0x%x db=0x%x\n",
          GpiRead (Base + GPI_CONFIG), GpiRead (GPI_EV_DB (GPII_ID))));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiStart (VOID)
{
  EFI_STATUS Status;
  mGpi.HaveQuiesced = FALSE;
  DEBUG ((DEBUG_ERROR, "GPI: start begin event-ctx=0x%lx tx-ctx=0x%lx rx-ctx=0x%lx\n",
          GPI_EV_CTX (GPII_ID), GPI_CH_CTX (GPII_ID, GPI_TX_CH),
          GPI_CH_CTX (GPII_ID, GPI_RX_CH)));
  GpiConfigurePolling ();
  mGpi.EventTouched = TRUE;
  Status = GpiCommand (TRUE, 0, 0, GPI_CH_ALLOCATED);
  if (EFI_ERROR (Status)) return Status;
  DEBUG ((DEBUG_ERROR, "GPI: event allocate complete\n"));
  Status = GpiProgramEvent ();
  if (EFI_ERROR (Status)) return Status;
  DEBUG ((DEBUG_ERROR, "GPI: event context programmed\n"));
  mGpi.ChannelTouched[GPI_TX_CH] = TRUE;
  Status = GpiCommand (FALSE, GPI_TX_CH, 0, GPI_CH_ALLOCATED);
  if (EFI_ERROR (Status)) return Status;
  mGpi.ChannelTouched[GPI_RX_CH] = TRUE;
  Status = GpiCommand (FALSE, GPI_RX_CH, 0, GPI_CH_ALLOCATED);
  if (EFI_ERROR (Status)) return Status;
  Status = GpiProgramChannel (GPI_TX_CH, &mGpi.TxRing, TRUE);
  if (EFI_ERROR (Status)) return Status;
  Status = GpiProgramChannel (GPI_RX_CH, &mGpi.RxRing, FALSE);
  if (EFI_ERROR (Status)) return Status;
  /* Do not let either START command reach GPI before both contexts are live. */
  GpiMemoryFence ();
  GpiDeviceFence ();
  Status = GpiCommand (FALSE, GPI_TX_CH, 1, GPI_CH_STARTED);
  if (EFI_ERROR (Status)) return Status;
  Status = GpiCommand (FALSE, GPI_RX_CH, 1, GPI_CH_STARTED);
  DEBUG ((DEBUG_ERROR, "GPI: start complete status=%r event=0x%x tx=0x%x rx=0x%x\n",
          Status, GPI_STATE (GpiRead (GPI_EV_CTX (GPII_ID))),
          GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, GPI_TX_CH))),
          GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, GPI_RX_CH)))));
  return Status;
}

/* Linux gpi_terminate_all() stops both channels before resetting them.
   Retain every DMA allocation on failure; callers fence the shared SE so
   FIFO cannot take ownership while a channel may still access memory. */
STATIC EFI_STATUS
GpiQuiesce (IN BOOLEAN Failed)
{
  EFI_STATUS Result = EFI_SUCCESS;
  EFI_STATUS Status;
  UINT8 Channel;
  UINT32 State;
  UINT32 Elapsed;
  for (Channel = 0; Channel < 2; ++Channel) {
    if (!mGpi.ChannelTouched[Channel]) continue;
    State = GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, Channel)));
    if ((State == GPI_CH_STARTED) || (State == 4U)) {
      Status = GpiCommand (FALSE, Channel, 2, 3);
      if (EFI_ERROR (Status)) Result = Status;
    }
  }
  if (EFI_ERROR (Result)) return Result;
  for (Channel = 0; Channel < 2; ++Channel) {
    if (!mGpi.ChannelTouched[Channel]) continue;
    State = GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, Channel)));
    if (State != 0) {
      Status = GpiCommand (FALSE, Channel, 9, GPI_CH_ALLOCATED);
      if (EFI_ERROR (Status)) return Status;
      Status = GpiCommand (FALSE, Channel, 10, 0);
      if (EFI_ERROR (Status)) return Status;
    }
    mGpi.ChannelTouched[Channel] = FALSE;
  }
  if (mGpi.SeReady) {
    /* STOP_STRETCH may retain the bus after CMD_ACTIVE has cleared. */
    if (Failed || ((GpiSeRead (SE_GENI_STATUS) & BIT (0)) != 0)) {
      GpiSeWrite (SE_M_IRQ_CLEAR, SE_M_CMD_ABORT);
      GpiSeWrite (SE_M_CMD_CTRL, BIT (1));
      for (Elapsed = 0; Elapsed < GPI_CMD_TIMEOUT_US; Elapsed += 10) {
        if (((GpiSeRead (SE_M_IRQ_STATUS) & SE_M_CMD_ABORT) != 0) &&
            ((GpiSeRead (SE_GENI_STATUS) & BIT (0)) == 0)) break;
        MicroSecondDelay (10);
      }
      if (Elapsed == GPI_CMD_TIMEOUT_US) return EFI_TIMEOUT;
    }
    GpiSeWrite (SE_GSI_EVENT_EN, 0);
    GpiSeWrite (SE_M_IRQ_CLEAR, MAX_UINT32);
    mGpi.SeReady = FALSE;
  }
  if (mGpi.EventTouched) {
    if (GPI_STATE (GpiRead (GPI_EV_CTX (GPII_ID))) != 0) {
      Status = GpiCommand (TRUE, 0, 9, GPI_CH_ALLOCATED);
      if (EFI_ERROR (Status)) return Status;
      Status = GpiCommand (TRUE, 0, 10, 0);
      if (EFI_ERROR (Status)) return Status;
    }
    mGpi.EventTouched = FALSE;
    mGpi.HaveQuiesced = !mGpi.CommandFailed;
  }
  mGpi.TxRing.DoorbellOffset = mGpi.RxRing.DoorbellOffset = 0;
  mGpi.EventRing.ReadOffset = 0;
  /* A timed-out control command may take effect after a state read.  Keep
     the lease fenced even if later reads happened to look idle. */
  /* A failed IOMMU unmap/free leaves a DMA mapping live even when no GPI
     context was programmed in this call (for example, allocation failed
     before GpiStart).  Do not report the lease as quiesced in that case:
     QUP ReleaseSe uses this bit to decide whether it may hand the shared
     serial engine to FIFO or another owner. */
  return (mGpi.CommandFailed || mGpi.DmaCleanupFailed) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiReleaseClocks (VOID)
{
  STATIC CONST CHAR8 *CONST Names[] = {
    "gcc_qupv3_wrap_1_m_ahb_clk", "gcc_qupv3_wrap_1_s_ahb_clk",
    "gcc_qupv3_wrap1_s1_clk"
  };
  UINTN Index;
  EFI_STATUS Status;
  /* This unwind is only for first-acquire failures before any DMA engine or
     IRQ context is programmed. Live or uncertain DMA retains its clocks. */
  if (mGpi.Initialized || mGpi.EventTouched || mGpi.ChannelTouched[0] ||
      mGpi.ChannelTouched[1] || mGpi.DmaCleanupFailed) return EFI_NOT_READY;
  for (Index = ARRAY_SIZE(Names); Index != 0; --Index) {
    UINT32 Bit = 1U << (Index - 1U);
    if ((mGpi.ClockMask & Bit) == 0) continue;
    Status = mGpi.Clock->SetClock(mGpi.Clock, "gcc", Names[Index - 1U], 0, FALSE);
    if (EFI_ERROR(Status)) return Status;
    mGpi.ClockMask &= ~Bit;
  }
  mGpi.ClockCleanupPending = FALSE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiPrepareClocks (VOID)
{
  STATIC CONST CHAR8 *CONST Names[] = {
    "gcc_qupv3_wrap_1_m_ahb_clk", "gcc_qupv3_wrap_1_s_ahb_clk",
    "gcc_qupv3_wrap1_s1_clk"
  };
  EFI_STATUS Status;
  UINTN Index;
  BOOLEAN Owned;
  if (mGpi.ClockCleanupPending) {
    Status = GpiReleaseClocks();
    if (EFI_ERROR(Status)) return Status;
  }
  if (mGpi.Clock == NULL) {
    Status = gBS->LocateProtocol(&gEfiClockCrProtocolGuid, NULL, (VOID **)&mGpi.Clock);
    if (EFI_ERROR(Status)) return EFI_NOT_READY;
  }
  if (mGpi.Clock == NULL || mGpi.Clock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
      mGpi.Clock->SetClock == NULL || mGpi.Clock->AcquireClock == NULL) return EFI_NOT_READY;
  /* Later pre-hardware initialization failures retain the healthy prefix;
     retries reuse these exact votes instead of incrementing them again. */
  for (Index = 0; Index < ARRAY_SIZE(Names); ++Index) {
    if ((mGpi.ClockMask & (1U << Index)) != 0) continue;
    Status = CrAcquireClock(mGpi.Clock, "gcc", Names[Index],
                            Index == 2 ? 19200000 : 0, &Owned);
    if (Owned) mGpi.ClockMask |= 1U << Index;
    if (EFI_ERROR(Status)) {
      mGpi.ClockCleanupPending = TRUE;
      (VOID)GpiReleaseClocks();
      return Status;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GpiInitialize (IN UINT32 Instance)
{
  EFI_STATUS Status;
  CONST CR_TARGET_QUP_CONTEXT *Context;
  CONST CR_TARGET_QUP_WRAPPER *Wrapper;
  UINTN Index;

  if (mGpi.DmaCleanupFailed) return EFI_DEVICE_ERROR;
  if (mGpi.Initialized) return (Instance == GPI_I2C_INSTANCE) ? EFI_SUCCESS : EFI_UNSUPPORTED;
  if (Instance != GPI_I2C_INSTANCE) return EFI_UNSUPPORTED;
  Status = gBS->LocateProtocol (&gEfiQupCrProtocolGuid, NULL, (VOID **)&mGpi.Qup);
  if (EFI_ERROR (Status) || (mGpi.Qup == NULL) ||
      (mGpi.Qup->Revision < EFI_QUP_CR_PROTOCOL_REVISION) ||
      (mGpi.Qup->GetContext == NULL) ||
      EFI_ERROR (mGpi.Qup->GetContext (mGpi.Qup, &Context))) return EFI_NOT_READY;
  if ((Context == NULL) || (Context->Qup == NULL) || (Context->Gpi == NULL)) return EFI_NOT_FOUND;
  Wrapper = NULL;
  for (Index = 0; Index < Context->QupCount; ++Index) {
    if (Context->Qup[Index].Id == GPI_CONTROLLER_ID) {
      Wrapper = &Context->Qup[Index];
      break;
    }
  }
  if ((Wrapper == NULL) || (Wrapper->Id != GPI_CONTROLLER_ID) ||
      (Wrapper->SerialEngineCount <= GPI_SE_INDEX) ||
      (Wrapper->SerialEngineBase[GPI_SE_INDEX] == 0)) {
    return EFI_NOT_FOUND;
  }
  if (Wrapper->IommuStreamId != GPI_DATA_SID) return EFI_COMPROMISED_DATA;
  mGpi.Controller = NULL;
  for (Index = 0; Index < Context->GpiCount; ++Index) {
    if (Context->Gpi[Index].Id == GPI_CONTROLLER_ID) {
      mGpi.Controller = &Context->Gpi[Index];
      break;
    }
  }
  if (mGpi.Controller == NULL) return EFI_NOT_FOUND;
  if ((mGpi.Controller->Base != 0x00a00000ULL) ||
      (mGpi.Controller->Size < 0x60000U) ||
      (mGpi.Controller->WrapperId != GPI_CONTROLLER_ID) ||
      (mGpi.Controller->InterruptCount < 12U) ||
      (mGpi.Controller->IommuStreamId != GPI_SID) ||
      ((mGpi.Controller->ChannelMask & BIT (GPII_ID)) == 0)) {
    return EFI_COMPROMISED_DATA;
  }
  /* Linux sm8450-gpi-dma maps the EE register window at base - 0x10000. */
  if (mGpi.Controller->Base < GPI_EE_OFFSET) return EFI_COMPROMISED_DATA;
  mGpi.GpiBase = (UINTN)(mGpi.Controller->Base - GPI_EE_OFFSET);
  mGpi.SeBase = (UINTN)Wrapper->SerialEngineBase[GPI_SE_INDEX];
  Status = GpiConfigurePins ();
  if (EFI_ERROR (Status)) return Status;
  DEBUG ((DEBUG_INFO,
          "GPI: controller=%u gpii=%u seid=%u ee=0x%lx se=0x%lx sid=0x%x\n",
          GPI_CONTROLLER_ID, GPII_ID, GPI_SEID, mGpi.GpiBase,
          mGpi.SeBase, GPI_SID));

  Status = GpiPrepareClocks();
  if (EFI_ERROR (Status)) return Status;

  Status = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL, (VOID **)&mGpi.Iommu);
  if (EFI_ERROR (Status) || (mGpi.Iommu == NULL) ||
      (mGpi.Iommu->Revision < EDKII_IOMMU_PROTOCOL_REVISION)) return EFI_NOT_READY;
  Status = gBS->LocateProtocol (&gEfiSmmuCrProtocolGuid, NULL, (VOID **)&mGpi.Smmu);
  if (EFI_ERROR (Status) || (mGpi.Smmu == NULL) ||
      (mGpi.Smmu->Revision < EFI_SMMU_CR_PROTOCOL_REVISION) ||
      (mGpi.Smmu->Attach == NULL)) return EFI_NOT_READY;
  Status = mGpi.Smmu->Attach (mGpi.Smmu, 0, 0xffff, GPI_SID);
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) return Status;
  /* GPI fetches TREs and writes events through its own stream. GENI moves
     the payload through the QUP wrapper stream; Linux maps I2C GPI data
     against se.dev->parent, independently of gpi_alloc_ring(). */
  Status = mGpi.Smmu->Attach (mGpi.Smmu, 0, 0xffff, GPI_DATA_SID);
  if (EFI_ERROR (Status) && (Status != EFI_ALREADY_STARTED)) return Status;
  DEBUG ((DEBUG_INIT, "GPI: DMA streams attached ring-SID=0x%x data-SID=0x%x\n",
          GPI_SID, GPI_DATA_SID));
  Status = GpiRingAllocate (GPI_RING_ELEMENTS, &mGpi.TxRing);
  if (EFI_ERROR (Status)) return Status;
  Status = GpiRingAllocate (GPI_RING_ELEMENTS, &mGpi.RxRing);
  if (EFI_ERROR (Status)) goto Error;
  Status = GpiRingAllocate (GPI_EVENT_ELEMENTS, &mGpi.EventRing);
  if (EFI_ERROR (Status)) goto Error;
  Status = GpiDmaAllocate (1, GPI_DATA_SID, &mGpi.Data);
  if (EFI_ERROR (Status)) goto Error;
  /* Hardware receives ring addresses only in GpiStart(), after all
     allocations succeed.  This error path therefore cannot free live DMA. */
  mGpi.Initialized = TRUE;
  return EFI_SUCCESS;
Error:
  GpiDmaFree (&mGpi.Data);
  GpiRingFree (&mGpi.EventRing);
  GpiRingFree (&mGpi.RxRing);
  GpiRingFree (&mGpi.TxRing);
  return Status;
}

STATIC VOID
GpiAdvance (IN GPI_RING *Ring, IN OUT UINT32 *Offset)
{
  *Offset += Ring->ElSize;
  if (*Offset >= Ring->Length) *Offset = 0;
}

STATIC VOID
GpiTre (IN GPI_RING *Ring, IN OUT UINT32 *Offset,
        IN UINT32 D0, IN UINT32 D1, IN UINT32 D2, IN UINT32 D3)
{
  UINT32 *Tre;

  /* Keep the CPU-side address visible in the diagnostic stream.  A crash
     immediately after GpiStart() otherwise cannot distinguish a bad host
     pointer from a device fault after the doorbell. */
  if ((Ring == NULL) || (Ring->Host == NULL) || (Ring->Device == 0) ||
      (*Offset >= Ring->Length) || ((Ring->Length - *Offset) < GPI_EL_SIZE)) {
    DEBUG ((DEBUG_ERROR,
            "GPI: invalid TRE ring host=%p device=0x%lx offset=0x%x length=0x%x\n",
            (Ring == NULL) ? NULL : Ring->Host,
            (Ring == NULL) ? 0 : Ring->Device,
            (Ring == NULL) ? 0 : *Offset,
            (Ring == NULL) ? 0 : Ring->Length));
    return;
  }
  Tre = (UINT32 *)((UINT8 *)Ring->Host + *Offset);
  DEBUG ((DEBUG_ERROR, "GPI: TRE host=%p device=0x%lx offset=0x%x d=0x%x/0x%x/0x%x/0x%x\n",
          Tre, Ring->Device + *Offset, *Offset, D0, D1, D2, D3));
  Tre[0] = D0; Tre[1] = D1; Tre[2] = D2; Tre[3] = D3;
  WriteBackDataCacheRange (Tre, GPI_EL_SIZE);
  GpiMemoryFence ();
  GpiAdvance (Ring, Offset);
}

STATIC EFI_STATUS
GpiWaitEvents (IN UINT8 RequiredChannels, IN UINT32 Length,
               IN EFI_PHYSICAL_ADDRESS CompletionTre)
{
  UINT32 Elapsed;
  UINT8 CompletedChannels = 0;
  EFI_PHYSICAL_ADDRESS HardwareRp = 0;
  for (Elapsed = 0; Elapsed < GPI_XFER_TIMEOUT_US; Elapsed += 20U) {
    EFI_PHYSICAL_ADDRESS ExpectedRp =
      mGpi.EventRing.Device + mGpi.EventRing.ReadOffset;
    if (EFI_ERROR (GpiCheckErrors ())) return EFI_DEVICE_ERROR;
    HardwareRp = (EFI_PHYSICAL_ADDRESS)GpiRead (
      GPI_EV_CTX (GPII_ID) + GPI_RING_RP_L);
    HardwareRp |= (EFI_PHYSICAL_ADDRESS)GpiRead (
      GPI_EV_CTX (GPII_ID) + GPI_RING_RP_H) << 32;
    if ((HardwareRp < mGpi.EventRing.Device) ||
        (HardwareRp >= mGpi.EventRing.Device + mGpi.EventRing.Length) ||
        (((HardwareRp - mGpi.EventRing.Device) % GPI_EL_SIZE) != 0))
      return EFI_DEVICE_ERROR;
    if (HardwareRp != ExpectedRp) {
      UINT32 *Event = (UINT32 *)((UINT8 *)mGpi.EventRing.Host + mGpi.EventRing.ReadOffset);
      UINT32 Type;
      UINT32 Code;
      UINT32 EventLength;
      UINT32 Channel;
      UINT32 NotifyErrorMask = GPI_NOTIFY_I2C_ERROR_MASK;
      UINT32 CompletionErrorMask = GPI_TCE_I2C_ERROR_MASK;
      InvalidateDataCacheRange (Event, GPI_EL_SIZE);
      Type = (Event[3] >> 16) & 0xffU;
      Channel = Event[3] >> 24;
      Code = Event[2] >> 24;
      EventLength = Event[2] & 0x00ffffffU;
      /* Android distinguishes address NACK (GP_LENGTH zero) from data
         NACK; a data-phase NACK is an error only for writes. */
      if ((RequiredChannels == BIT (GPI_RX_CH)) &&
          (GpiSeRead (SE_M_GP_LENGTH) != 0)) {
        NotifyErrorMask &= ~BIT (10);
        CompletionErrorMask &= ~BIT (6);
      }
      /* Android waipio gi2c_ev_cb() receives the GENI GP IRQ status in
         QUP notifications; NACK, bus protocol and arbitration errors must
         not be discarded while waiting for a later DMA completion. */
      if ((Type == GPI_EVENT_QUP_NOTIFY) &&
          ((Event[0] & NotifyErrorMask) != 0)) {
        return EFI_DEVICE_ERROR;
      }
      if (Type == GPI_EVENT_XFER_COMPLETE) {
        /* gi2c_gsi_cb_err() checks the same GP errors in the TCE status
           field, encoded at bits 6/8/9 rather than GENI bits 10/12/13. */
        if (((Event[3] & CompletionErrorMask) != 0) ||
            (Code == GPI_EVENT_ERROR) ||
            ((Code != GPI_EVENT_SUCCESS) && (Code != GPI_EVENT_EOT) && (Code != GPI_EVENT_EOB)) ||
            (Channel >= 2U)) return EFI_DEVICE_ERROR;
        /* A read has a TX GO TRE (channel 0) linked to an RX DMA TRE
           (channel 1).  The TX GO does not carry a data DMA TRE, so Linux
           completes the transfer from the RX channel's EOT event.  Wait on
           the channel that owns the payload instead of assuming that both
           channels emit an event. */
        if ((RequiredChannels & BIT (Channel)) != 0) {
          EFI_PHYSICAL_ADDRESS Pointer = (EFI_PHYSICAL_ADDRESS)Event[0] |
              ((EFI_PHYSICAL_ADDRESS)Event[1] << 32);
          if ((Pointer != CompletionTre) ||
              ((Length != 0) && (EventLength != Length))) {
            return EFI_DEVICE_ERROR;
          }
          CompletedChannels |= (UINT8)BIT (Channel);
        }
      }
      /* Like Linux gpi_ring_recycle_ev_element(), recycle by advancing the
         consumer and doorbell only.  CPU stores here would dirty a cache line
         shared with events that the device can still be writing. */
      GpiAdvance (&mGpi.EventRing, &mGpi.EventRing.ReadOffset);
      GpiAdvance (&mGpi.EventRing, &mGpi.EventRing.DoorbellOffset);
      GpiWrite (GPI_EV_DB (GPII_ID),
                (UINT32)(mGpi.EventRing.Device + mGpi.EventRing.DoorbellOffset));
      GpiWrite (GPI_IEOB_CLEAR (GPII_ID), 1U);
      if ((CompletedChannels & RequiredChannels) == RequiredChannels) {
        return EFI_SUCCESS;
      }
    }
    MicroSecondDelay (20);
  }
  DEBUG ((DEBUG_ERROR, "GPI: timeout events=0x%x/0x%x RP=0x%lx\n",
          CompletedChannels, RequiredChannels, HardwareRp));
  return EFI_TIMEOUT;
}

STATIC VOID
GpiLogDmaState (IN CONST CHAR8 *Stage)
{
  CONST UINTN Contexts[] = {
    GPI_CH_CTX (GPII_ID, GPI_TX_CH),
    GPI_CH_CTX (GPII_ID, GPI_RX_CH),
    GPI_EV_CTX (GPII_ID)
  };
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (Contexts); ++Index) {
    DEBUG ((DEBUG_ERROR, "GPI: DMA snapshot stage=%a ring=%u begin\n",
            Stage, (UINT32)Index));
    DEBUG ((DEBUG_ERROR,
            "GPI: DMA context config=0x%x length=0x%x base=0x%08x:%08x\n",
            GpiRead (Contexts[Index] + GPI_CONFIG),
            GpiRead (Contexts[Index] + GPI_LENGTH),
            GpiRead (Contexts[Index] + GPI_RING_BASE_H),
            GpiRead (Contexts[Index] + GPI_RING_BASE_L)));
    DEBUG ((DEBUG_ERROR,
            "GPI: DMA pointers RP=0x%08x:%08x WP=0x%08x:%08x\n",
            GpiRead (Contexts[Index] + GPI_RING_RP_H),
            GpiRead (Contexts[Index] + GPI_RING_RP_L),
            GpiRead (Contexts[Index] + GPI_RING_WP_H),
            GpiRead (Contexts[Index] + GPI_RING_WP_L)));
  }
  DEBUG ((DEBUG_ERROR, "GPI: DMA snapshot stage=%a SE begin\n", Stage));
  DEBUG ((DEBUG_ERROR,
          "GPI: DMA SE status=0x%x mode=0x%x dma-if-en=0x%x events=0x%x irq-en=0x%x\n",
          GpiSeRead (SE_GENI_STATUS), GpiSeRead (SE_DMA_MODE),
          GpiSeRead (SE_DMA_IF_EN), GpiSeRead (SE_GSI_EVENT_EN),
          GpiSeRead (SE_M_IRQ_EN)));
  DEBUG ((DEBUG_ERROR, "GPI: DMA snapshot stage=%a complete\n", Stage));
}

STATIC EFI_STATUS
GpiTransferOne (IN UINT16 Address, IN BOOLEAN Read, IN BOOLEAN KeepBus,
                IN BOOLEAN Configure, IN UINT8 *Buffer, IN UINT32 Length)
{
  UINT32 TxOffset;
  UINT32 RxOffset;
  UINT32 Config0 = 11U | (5U << 8) | (22U << 16) | BIT (24) | BIT (25);
  UINT32 Go = ((UINT32)Address << 8) | (KeepBus ? BIT (26) : 0);
  EFI_STATUS Status;
  BOOLEAN Diagnostic;

  if ((Buffer == NULL) || (Length == 0) || (Length > 0x00ffffffU)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Length > (mGpi.Data.Pages * EFI_PAGE_SIZE)) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (!mGpi.Initialized || (mGpi.Data.Host == NULL) ||
      (mGpi.Data.Device == 0) || (mGpi.TxRing.Host == NULL) ||
      (mGpi.TxRing.Device == 0) || (mGpi.RxRing.Host == NULL) ||
      (mGpi.RxRing.Device == 0) || (mGpi.EventRing.Host == NULL) ||
      (mGpi.EventRing.Device == 0)) {
    DEBUG ((DEBUG_ERROR,
            "GPI: transfer buffers invalid init=%u data=%p/0x%lx tx=%p/0x%lx rx=%p/0x%lx ev=%p/0x%lx\n",
            mGpi.Initialized, mGpi.Data.Host, mGpi.Data.Device,
            mGpi.TxRing.Host, mGpi.TxRing.Device,
            mGpi.RxRing.Host, mGpi.RxRing.Device,
            mGpi.EventRing.Host, mGpi.EventRing.Device));
    return EFI_NOT_READY;
  }
  DEBUG ((DEBUG_ERROR,
          "GPI: transfer begin addr=0x%02x read=%u keep=%u config=%u buffer=%p length=0x%x data=%p/0x%lx tx-off=0x%x rx-off=0x%x\n",
          Address, Read, KeepBus, Configure, Buffer, Length, mGpi.Data.Host,
          mGpi.Data.Device, mGpi.TxRing.DoorbellOffset,
          mGpi.RxRing.DoorbellOffset));
  if (!Read) {
    CopyMem (mGpi.Data.Host, Buffer, Length);
    /* DMA_TO_DEVICE: publish CPU writes before ringing the TX doorbell. */
    WriteBackDataCacheRange (mGpi.Data.Host, Length);
  } else {
    /* DMA_FROM_DEVICE: discard dirty CPU lines before the device writes.  A
       write-back here can overwrite bytes in a partially covered cache line
       after the I2C engine has produced the read result on non-coherent HW. */
    InvalidateDataCacheRange (mGpi.Data.Host, Length);
  }
  Diagnostic = !mGpi.DmaDiagnosticLogged;
  TxOffset = mGpi.TxRing.DoorbellOffset;
  RxOffset = mGpi.RxRing.DoorbellOffset;
  /* Linux sends CONFIG0 once per request, before the first GO. Subsequent
     messages may hold SCL with STOP_STRETCH for a repeated START. */
  if (Configure) {
    GpiTre (&mGpi.TxRing, &TxOffset, Config0, 0, GPI_C0_CLK_DIV,
            GPI_TYPE (GPI_TRE_CONFIG0) | GPI_FLAG_CHAIN);
  }
  if (Read) {
    /* The TX GO starts the read; RX only owns the DMA data TRE.  Linux's
       geni_i2c_gpi() deliberately skips a second CONFIG0 on the RX channel. */
    GpiTre (&mGpi.RxRing, &RxOffset, (UINT32)mGpi.Data.Device,
            (UINT32)(mGpi.Data.Device >> 32), Length,
            GPI_TYPE (GPI_TRE_DMA) | GPI_FLAG_IEOT);
    Go |= 2U;
    GpiTre (&mGpi.TxRing, &TxOffset, Go, 0, Length,
            GPI_TYPE (GPI_TRE_GO) | GPI_FLAG_LINK);
    GpiMemoryFence ();
    GpiDeviceFence ();
    if (Diagnostic) GpiLogDmaState ("before-doorbell");
    DEBUG ((DEBUG_ERROR, "GPI: RX doorbell=0x%lx TX doorbell=0x%lx\n",
            mGpi.RxRing.Device + RxOffset,
            mGpi.TxRing.Device + TxOffset));
    GpiWrite (GPI_CH_DB (GPII_ID, GPI_RX_CH), (UINT32)(mGpi.RxRing.Device + RxOffset));
    if (Diagnostic) DEBUG ((DEBUG_ERROR, "GPI: RX doorbell write returned\n"));
    GpiWrite (GPI_CH_DB (GPII_ID, GPI_TX_CH), (UINT32)(mGpi.TxRing.Device + TxOffset));
    if (Diagnostic) {
      DEBUG ((DEBUG_ERROR, "GPI: TX doorbell write returned\n"));
      GpiLogDmaState ("after-doorbell");
      mGpi.DmaDiagnosticLogged = TRUE;
      DEBUG ((DEBUG_ERROR, "GPI: event wait begin\n"));
    }
    Status = GpiWaitEvents ((UINT8)BIT (GPI_RX_CH), Length,
        mGpi.RxRing.Device + (RxOffset + mGpi.RxRing.Length - GPI_EL_SIZE) % mGpi.RxRing.Length);
    if (!EFI_ERROR (Status)) {
      InvalidateDataCacheRange (mGpi.Data.Host, Length);
      CopyMem (Buffer, mGpi.Data.Host, Length);
    }
    mGpi.RxRing.DoorbellOffset = RxOffset;
  } else {
    Go |= 1U;
    GpiTre (&mGpi.TxRing, &TxOffset, Go, 0, Length,
            GPI_TYPE (GPI_TRE_GO) | GPI_FLAG_CHAIN);
    GpiTre (&mGpi.TxRing, &TxOffset, (UINT32)mGpi.Data.Device,
            (UINT32)(mGpi.Data.Device >> 32), Length,
            GPI_TYPE (GPI_TRE_DMA) | GPI_FLAG_IEOT);
    GpiMemoryFence ();
    GpiDeviceFence ();
    if (Diagnostic) GpiLogDmaState ("before-doorbell");
    DEBUG ((DEBUG_ERROR, "GPI: TX doorbell=0x%lx\n",
            mGpi.TxRing.Device + TxOffset));
    GpiWrite (GPI_CH_DB (GPII_ID, GPI_TX_CH), (UINT32)(mGpi.TxRing.Device + TxOffset));
    if (Diagnostic) {
      DEBUG ((DEBUG_ERROR, "GPI: TX doorbell write returned\n"));
      GpiLogDmaState ("after-doorbell");
      mGpi.DmaDiagnosticLogged = TRUE;
      DEBUG ((DEBUG_ERROR, "GPI: event wait begin\n"));
    }
    Status = GpiWaitEvents ((UINT8)BIT (GPI_TX_CH), Length,
        mGpi.TxRing.Device + (TxOffset + mGpi.TxRing.Length - GPI_EL_SIZE) % mGpi.TxRing.Length);
  }
  if (Diagnostic) DEBUG ((DEBUG_ERROR, "GPI: event wait returned status=%r\n", Status));
  mGpi.TxRing.DoorbellOffset = TxOffset;
  return Status;
}

STATIC EFI_STATUS
EFIAPI
GpiI2cTransfer (IN EFI_GPI_CR_PROTOCOL *This, IN UINT32 Instance,
                IN UINT16 SlaveAddress, IN EFI_GPI_I2C_DESCRIPTOR *Descriptors,
                IN UINT16 DescriptorCount, OUT UINT32 *Transferred OPTIONAL)
{
  EFI_STATUS Status;
  EFI_STATUS Cleanup;
  EFI_STATUS ReleaseStatus;
  UINTN Index;
  UINT32 Total = 0;
  UINT64 Requested = 0;
  UINT32 EventState;
  UINT32 TxState;
  UINT32 RxState;
  if (Transferred != NULL) *Transferred = 0;
  if ((This != &mGpi.Protocol) || (Descriptors == NULL) || (DescriptorCount == 0) ||
      (SlaveAddress > 0x7f)) return EFI_INVALID_PARAMETER;
  if (Instance != GPI_I2C_INSTANCE) return EFI_UNSUPPORTED;
  /* Validate the whole request before its first START.  A missing final STOP
     cannot carry a bus lease across calls to this synchronous protocol. */
  for (Index = 0; Index < DescriptorCount; ++Index) {
    UINT32 Flags = Descriptors[Index].Flags;
    if ((Descriptors[Index].Buffer == NULL) || (Descriptors[Index].Length == 0) ||
        ((Flags & ~(EFI_GPI_I2C_FLAG_START | EFI_GPI_I2C_FLAG_STOP |
                    EFI_GPI_I2C_FLAG_WRITE | EFI_GPI_I2C_FLAG_READ)) != 0) ||
        ((Flags & EFI_GPI_I2C_FLAG_START) == 0) ||
        (((Flags & EFI_GPI_I2C_FLAG_READ) != 0) ==
         ((Flags & EFI_GPI_I2C_FLAG_WRITE) != 0)) ||
        ((Index + 1 == DescriptorCount) && ((Flags & EFI_GPI_I2C_FLAG_STOP) == 0)))
      return EFI_INVALID_PARAMETER;
    Requested += Descriptors[Index].Length;
    if ((Descriptors[Index].Length > EFI_PAGE_SIZE) || (Requested > MAX_UINT32))
      return EFI_BAD_BUFFER_SIZE;
  }
  Status = gBS->LocateProtocol (&gEfiQupCrProtocolGuid, NULL, (VOID **)&mGpi.Qup);
  if (EFI_ERROR (Status) || (mGpi.Qup == NULL) ||
      (mGpi.Qup->Revision < EFI_QUP_CR_PROTOCOL_REVISION) ||
      (mGpi.Qup->AcquireSe == NULL) || (mGpi.Qup->ReleaseSe == NULL))
    return EFI_NOT_READY;
  Status = mGpi.Qup->AcquireSe (mGpi.Qup, GPI_CONTROLLER_ID, GPI_SE_INDEX,
                                &mGpi, QupCrSeGpi);
  if (EFI_ERROR (Status)) return Status;
  DEBUG ((DEBUG_ERROR, "GPI: transfer lease acquired instance=%u address=0x%02x descriptors=%u\n",
          Instance, SlaveAddress, DescriptorCount));
  Status = GpiInitialize (Instance);
  if (!EFI_ERROR (Status)) {
    EventState = GPI_STATE (GpiRead (GPI_EV_CTX (GPII_ID)));
    TxState = GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, GPI_TX_CH)));
    RxState = GPI_STATE (GpiRead (GPI_CH_CTX (GPII_ID, GPI_RX_CH)));
    /* A channel or active GENI command left by another owner must not be
       reset or reused.  Accept retained ALLOCATED state only after our
       own acknowledged RESET/DEALLOC sequence under the shared SE lease. */
    if ((EventState > GPI_CH_ALLOCATED) || (TxState > GPI_CH_ALLOCATED) ||
        (RxState > GPI_CH_ALLOCATED) ||
        (!mGpi.HaveQuiesced && ((EventState | TxState | RxState) != 0)) ||
        ((GpiSeRead (SE_GENI_STATUS) & BIT (0)) != 0)) {
      Status = EFI_ACCESS_DENIED;
    } else if (((GpiSeRead (SE_GENI_FW_REVISION_RO) >> 8) & 0xffU) != 3U) {
      /* GPI transports TREs; it does not install I2C sequencer firmware. */
      DEBUG ((DEBUG_ERROR, "GPI: SE1 has no I2C firmware (FW=0x%x)\n",
              GpiSeRead (SE_GENI_FW_REVISION_RO)));
      Status = EFI_UNSUPPORTED;
    } else {
      mGpi.SeReady = TRUE;
      GpiConfigureSe ();
      Status = GpiStart ();
    }
  }
  for (Index = 0; !EFI_ERROR (Status) && (Index < DescriptorCount); ++Index) {
    UINT32 Flags = Descriptors[Index].Flags;
    Status = GpiTransferOne (SlaveAddress, (Flags & EFI_GPI_I2C_FLAG_READ) != 0,
                             (Flags & EFI_GPI_I2C_FLAG_STOP) == 0,
                             Index == 0,
                             Descriptors[Index].Buffer, Descriptors[Index].Length);
    if (!EFI_ERROR (Status)) Total += Descriptors[Index].Length;
  }
  /* Even on success stop/reset both GPI channels before permitting FIFO
     mode.  On failed quiescence keep mappings and permanently fence the SE. */
  Cleanup = GpiQuiesce (EFI_ERROR (Status));
  if (EFI_ERROR (Cleanup)) {
    DEBUG ((DEBUG_ERROR, "GPI: quiesce failed %r; retaining DMA and SE ownership\n", Cleanup));
    if (!EFI_ERROR (Status)) Status = Cleanup;
  }
  ReleaseStatus = mGpi.Qup->ReleaseSe (mGpi.Qup, GPI_CONTROLLER_ID,
                                       GPI_SE_INDEX, &mGpi,
                                       !EFI_ERROR (Cleanup));
  if (EFI_ERROR (ReleaseStatus) && !EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "GPI: QUP lease release failed: %r\n", ReleaseStatus));
    Status = ReleaseStatus;
  }
  if (Transferred != NULL) *Transferred = Total;
  return Status;
}

STATIC EFI_STATUS
EFIAPI
GpiGetController (IN EFI_GPI_CR_PROTOCOL *This, IN UINT8 ControllerId,
                  OUT CONST CR_TARGET_GPI_CONTROLLER **Controller)
{
  CONST CR_TARGET_QUP_CONTEXT *Context;
  UINTN Index;
  (VOID)This;
  if (Controller == NULL) return EFI_INVALID_PARAMETER;
  *Controller = NULL;
  Context = CrDalGetQupContext ();
  if (Context == NULL) return EFI_NOT_READY;
  for (Index = 0; Index < Context->GpiCount; ++Index) {
    if (Context->Gpi[Index].Id == ControllerId) {
      *Controller = &Context->Gpi[Index];
      return EFI_SUCCESS;
    }
  }
  return EFI_NOT_FOUND;
}

STATIC EFI_STATUS
EFIAPI
GpiGetContext (IN EFI_GPI_CR_PROTOCOL *This, OUT CONST CR_TARGET_QUP_CONTEXT **Context)
{
  (VOID)This;
  if (Context == NULL) return EFI_INVALID_PARAMETER;
  *Context = CrDalGetQupContext ();
  return (*Context == NULL) ? EFI_NOT_FOUND : EFI_SUCCESS;
}

STATIC EFI_GPI_CR_PROTOCOL mGpiProtocol = {
  EFI_GPI_CR_PROTOCOL_REVISION, GpiGetController, GpiGetContext, GpiI2cTransfer
};

EFI_STATUS
EFIAPI
GpiCrEntryPoint (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  (VOID)SystemTable;
  ZeroMem (&mGpi, sizeof (mGpi));
  mGpi.Protocol = mGpiProtocol;
  return gBS->InstallMultipleProtocolInterfaces (
      &ImageHandle, &gEfiGpiCrProtocolGuid, &mGpi.Protocol, NULL);
}
