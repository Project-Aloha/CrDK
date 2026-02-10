/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "pdc_internal.h"
#include <Library/CrTargetPdcLib.h>
#ifdef _KERNEL_MODE
#include "pdc.tmh"
#endif

// Get GIC Irq from PDC Pin Number
CR_STATUS PdcGetGicIrqFromPdcPin(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                                 OUT UINT16 *GicIrq) {
  if (Ctx == NULL || Ctx->PdcRegion == NULL) {
    return CR_INVALID_PARAMETER;
  }

  PdcRange *PdcRegion = Ctx->PdcRegion;

  for (UINT32 i = 0; i < Ctx->PdcRangeCount; i++) {
    if ((PdcPinNumber >= PdcRegion->PdcPinBase) &&
        (PdcPinNumber < (PdcRegion->PdcPinBase + PdcRegion->PdcCount))) {
      *GicIrq = GIC_SPI(PdcRegion->GicSpiBase +
                        (PdcPinNumber - PdcRegion->PdcPinBase));
      return CR_SUCCESS;
    }
    PdcRegion++;
  }
  return CR_NOT_FOUND; // Not found
}

CR_STATUS PdcGetPdcPinFromGicIrq(PdcDeviceContext *Ctx, UINT16 GicIrq,
                                 OUT UINT16 *PdcPinNumber) {
  if (Ctx == NULL || Ctx->PdcRegion == NULL) {
    return CR_INVALID_PARAMETER;
  }

  PdcRange *PdcRegion = Ctx->PdcRegion;
  for (UINT32 i = 0; i < Ctx->PdcRangeCount; i++) {
    UINT32 StartIrq = GIC_SPI(PdcRegion->GicSpiBase);
    UINT32 EndIrq = GIC_SPI(PdcRegion->GicSpiBase + PdcRegion->PdcCount);
    if (GicIrq >= StartIrq && GicIrq < EndIrq) {
      *PdcPinNumber = (UINT16)(PdcRegion->PdcPinBase + (GicIrq - StartIrq));
      return CR_SUCCESS;
    }
    PdcRegion++;
  }

  return CR_NOT_FOUND; // Not found
}

CR_STATUS PdcGicSetType(IN PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                        IN CR_INTERRUPT_TRIGGER_TYPE Type,
                        OUT CR_INTERRUPT_TRIGGER_TYPE *GicTypeOut) {
  UINT8 PdcIrqConfig = 0;
  CR_STATUS Status = CR_SUCCESS;
  CR_INTERRUPT_TRIGGER_TYPE GicType = Type;

  // Map CR_INTERRUPT_TRIGGER_TYPE to PdcIrqConfig
  switch ((UINT32)Type) {
  case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
    PdcIrqConfig = PDC_IRQ_EDGE_FALLING;
    // Gic does not support falling edge, pdc will invert the signal
    GicType = CR_INTERRUPT_TRIGGER_EDGE_RISING;
    break;
  case CR_INTERRUPT_TRIGGER_EDGE_RISING:
    PdcIrqConfig = PDC_IRQ_EDGE_RISING;
    break;
  case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
    PdcIrqConfig = PDC_IRQ_EDGE_DUAL;
    GicType = CR_INTERRUPT_TRIGGER_EDGE_RISING;
    break;
  case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
    PdcIrqConfig = PDC_IRQ_LEVEL_LOW;
    GicType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH;
    break;
  case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
    PdcIrqConfig = PDC_IRQ_LEVEL_HIGH;
    break;
  default:
    log_err("Invalid interrupt trigger type");
    return CR_INVALID_PARAMETER;
  }

  // Linux implemented this, it seems not required here.
  // Read and check later, write a different may cause spurious interrupts
  // UINT32 OriPdcIrqCfgRegVal = CrMmioRead32(
  //     Ctx->BaseAddress + PDC_IRQ_I_CFG + PdcPinNumber * sizeof(UINT32));
  CrMmioWrite32(Ctx->BaseAddress + PDC_IRQ_I_CFG +
                    PdcPinNumber * sizeof(UINT32),
                PdcIrqConfig);

  // For gpio irq configuration with level triggered, set spi
  if ((Ctx->SPIConfigBase != 0) && (Type == CR_INTERRUPT_TRIGGER_LEVEL_HIGH ||
                                    Type == CR_INTERRUPT_TRIGGER_LEVEL_LOW)) {
    UINT16 GicSPI = 0;
    Status = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber, &GicSPI);
    if (CR_ERROR(Status)) {
      log_err("PdcGetGicIrqFromPdcPin failed");
      return Status;
    }
    GicSPI -= GIC_SPI(0); // GSI

    UINT32 PinIdx = GicSPI >> 5;         // pin / 32
    UINT32 PinMask = BIT(GicSPI & 0x1f); // bit(pin % 32)
    if (PinIdx * sizeof(UINT32) > Ctx->SPIConfigSize) {
      log_err("Out of SPI Config Range");
      return CR_INVALID_PARAMETER;
    }

    // Read spi config register
    UINT32 SpiCfgRegVal =
        CrMmioRead32(Ctx->SPIConfigBase + PinIdx * sizeof(UINT32));
    // Clear previous config
    SpiCfgRegVal = CLR_BITS(SpiCfgRegVal, PinMask);
    // Mask if level triggered
    SpiCfgRegVal = SET_BITS(SpiCfgRegVal, PinMask);
    // Write back
    CrMmioWrite32(Ctx->SPIConfigBase + PinIdx * sizeof(UINT32), SpiCfgRegVal);
  }

  // Later, you should configure GIC type and register handler.
  if (GicTypeOut != NULL) {
    *GicTypeOut = GicType;
  }
  return CR_SUCCESS;
}

CR_STATUS
PdcCheckInterruptEnabled(IN PdcDeviceContext *Ctx, IN UINT16 PdcPinNumber,
                         BOOLEAN *Enabled) {
  UINT32 RegVal = 0;
  UINT32 Index = PdcPinNumber >> 5;  // pin / 32
  UINT32 Mask = PdcPinNumber & 0x1f; // pin % 32

  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }

  RegVal = CrMmioRead32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                        Index * sizeof(UINT32));

  *Enabled = (GET_FIELD(RegVal, BIT(Mask)) != 0);
  return CR_SUCCESS;
}

CR_STATUS PdcEnableInterrupt(IN PdcDeviceContext *Ctx, IN UINT16 PdcPinNumber,
                             BOOLEAN Enable) {
  UINT32 RegVal = 0;
  UINT32 Index = PdcPinNumber >> 5;  // pin / 32
  UINT32 Mask = PdcPinNumber & 0x1f; // pin % 32

  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }

  log_info(CR_LOG_CHAR8_STR_FMT ": PDC Pin idx: %d msk: %d", __FUNCTION__,
           Index, Mask);
  RegVal = CrMmioRead32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                        Index * sizeof(UINT32));
  RegVal = Enable ? SET_BITS(RegVal, BIT(Mask)) : CLR_BITS(RegVal, BIT(Mask));
  // Write back
  CrMmioWrite32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK + Index * sizeof(UINT32),
                RegVal);

  log_info(CR_LOG_CHAR8_STR_FMT ": PDC Pin %d Interrupt " CR_LOG_CHAR8_STR_FMT,
           __FUNCTION__, PdcPinNumber, Enable ? "Enabled" : "Disabled");
  log_info(CR_LOG_CHAR8_STR_FMT ": RegValue 0x%x", __FUNCTION__,
           CrMmioRead32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                        Index * sizeof(UINT32)));
  return CR_SUCCESS;
}

STATIC
VOID PdcPinsMapping(PdcDeviceContext *Ctx) {
  PdcRange *PdcRegion = Ctx->PdcRegion;
  UINT32 IrqEnBankRegVal = 0;
  UINT16 RegIndex = 0;
  UINT16 IrqIndex = 0;

  // Clean all interrupts
  while (PdcRegion->PdcCount != 0) {
    for (UINT16 i = 0; i < PdcRegion->PdcCount; i++) {
      RegIndex = (i + PdcRegion->PdcPinBase) >> 5;
      IrqIndex = (i + PdcRegion->PdcPinBase) & 0x1f;
      IrqEnBankRegVal = CrMmioRead32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                                     (RegIndex * sizeof(UINT32)));
      IrqEnBankRegVal = CLR_BITS(IrqEnBankRegVal, IrqIndex);
      // Write back
      CrMmioWrite32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                        (RegIndex * sizeof(UINT32)),
                    IrqEnBankRegVal);
    }
    PdcRegion++;
  }
}

CR_STATUS
PdcLibInit(IN OUT PdcDeviceContext **Ctx) {
  if (Ctx == NULL) {
    return CR_INVALID_PARAMETER;
  }
  // Get PDC Context only if not provided
  if (*Ctx == NULL) {
    *Ctx = GetPdcDevContext();
    if (*Ctx == NULL) {
      log_err("GetPdcDevContext failed.");
      return CR_NOT_FOUND;
    }
  }
  // Disable All IRQs in PDC
  PdcPinsMapping(*Ctx);
  return CR_SUCCESS;
}