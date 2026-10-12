/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "pdc_internal.h"
#ifdef _KERNEL_MODE
#include "pdc.tmh"
#endif

STATIC CR_STATUS PdcValidateRanges(IN CONST PdcDeviceContext *Ctx) {
  if (Ctx == NULL || Ctx->PdcRangeCount == 0 ||
      Ctx->PdcRangeCount > PDC_RANGE_COUNT_MAX) return CR_INVALID_PARAMETER;
  for (UINT16 I=0; I<Ctx->PdcRangeCount; ++I) {
    CONST PdcRange *Range=&Ctx->PdcRegion[I];
    UINT32 PinEnd=(UINT32)Range->PdcPinBase+Range->PdcCount;
    UINT32 SpiEnd=(UINT32)Range->GicSpiBase+Range->PdcCount;
    if (Range->PdcCount == 0 || PinEnd > 0x10000U || SpiEnd > 988U)
      return CR_INVALID_PARAMETER;
    for (UINT16 J=0; J<I; ++J) {
      CONST PdcRange *Other=&Ctx->PdcRegion[J];
      if ((Range->PdcPinBase < (UINT32)Other->PdcPinBase+Other->PdcCount &&
           Other->PdcPinBase < PinEnd) ||
          (Range->GicSpiBase < (UINT32)Other->GicSpiBase+Other->PdcCount &&
           Other->GicSpiBase < SpiEnd)) return CR_INVALID_PARAMETER;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS PdcValidateWord(UINTN Base, UINTN Size, UINTN Offset) {
  if (Base == 0 || (Base & 3U) != 0 || (Offset & 3U) != 0 ||
      Base > (UINTN)-1-Offset || Base+Offset > (UINTN)-1-(sizeof(UINT32)-1))
    return CR_INVALID_PARAMETER;
  if (Size != 0) {
    if ((Size & 3U) != 0 || Base > (UINTN)-1-(Size-1)) return CR_INVALID_PARAMETER;
    if (Size < sizeof(UINT32) || Offset > Size-sizeof(UINT32)) return CR_BAD_BUFFER_SIZE;
  }
  return CR_SUCCESS;
}

// Get GIC Irq from PDC Pin Number
CR_STATUS PdcGetGicIrqFromPdcPin(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                                 OUT UINT16 *GicIrq) {
  if (GicIrq == NULL || CR_ERROR(PdcValidateRanges(Ctx))) {
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
  if (PdcPinNumber == NULL || CR_ERROR(PdcValidateRanges(Ctx))) {
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

STATIC CR_STATUS PdcValidateRegister(PdcDeviceContext *Ctx, UINT16 Pin, UINTN Offset) {
  UINT16 Gic;
  if (Ctx == NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status = PdcGetGicIrqFromPdcPin(Ctx, Pin, &Gic);
  if (CR_ERROR(Status)) return Status;
  return PdcValidateWord(Ctx->BaseAddress,Ctx->Size,Offset);
}

CR_STATUS PdcValidateInterrupt(PdcDeviceContext *Ctx,UINT16 PdcPinNumber,
                               CR_INTERRUPT_TRIGGER_TYPE Type,
                               PDC_INTERRUPT_REGISTERS *Registers) {
  UINT8 PdcIrqConfig = 0;
  CR_STATUS Status = CR_SUCCESS;
  CR_INTERRUPT_TRIGGER_TYPE GicType = Type;
  UINTN ConfigOffset = PDC_IRQ_I_CFG + (UINTN)PdcPinNumber * sizeof(UINT32);
  UINT32 PinIdx = 0, PinMask = 0;
  if (Registers == NULL) return CR_INVALID_PARAMETER;
  Status = PdcValidateRegister(Ctx, PdcPinNumber, ConfigOffset);
  if (CR_ERROR(Status)) return Status;
  UINTN EnableOffset=PDC_IRQ_ENABLE_BANK+(PdcPinNumber/32)*sizeof(UINT32);
  Status=PdcValidateRegister(Ctx,PdcPinNumber,EnableOffset);
  if (CR_ERROR(Status)) return Status;

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

  // Validate the shared SPI word before changing any PDC configuration.
  if (Ctx->SPIConfigBase != 0) {
    UINT16 GicSPI = 0;
    Status = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber, &GicSPI);
    if (CR_ERROR(Status)) {
      log_err("PdcGetGicIrqFromPdcPin failed");
      return Status;
    }
    GicSPI -= GIC_SPI(0); // GSI

    PinIdx = GicSPI >> 5;         // pin / 32
    PinMask = (UINT32)BIT(GicSPI & 0x1f); // bit(pin % 32)
    Status=PdcValidateWord(Ctx->SPIConfigBase,Ctx->SPIConfigSize,
                            PinIdx*sizeof(UINT32));
    if (Ctx->SPIConfigSize == 0 || CR_ERROR(Status)) {
      log_err("Out of SPI Config Range");
      return CR_ERROR(Status) ? Status : CR_BAD_BUFFER_SIZE;
    }
  }

  *Registers=(PDC_INTERRUPT_REGISTERS){
      .ConfigAddress=Ctx->BaseAddress+ConfigOffset,
      .EnableAddress=Ctx->BaseAddress+EnableOffset,
      .SpiAddress=PinMask!=0?Ctx->SPIConfigBase+PinIdx*sizeof(UINT32):0,
      .PdcType=PdcIrqConfig,.EnableMask=(UINT32)BIT(PdcPinNumber&31),
      .SpiMask=PinMask,.GicType=GicType};
  return CR_SUCCESS;
}

CR_STATUS PdcGicSetType(IN PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                        IN CR_INTERRUPT_TRIGGER_TYPE Type,
                        OUT CR_INTERRUPT_TRIGGER_TYPE *GicTypeOut) {
  PDC_INTERRUPT_REGISTERS Registers;
  CR_STATUS Status=PdcValidateInterrupt(Ctx,PdcPinNumber,Type,&Registers);
  if (CR_ERROR(Status)) return Status;
  // Linux qcom_pdc_gic_set_type preserves status/mask bits outside type[2:0].
  UINT32 Config = CrMmioRead32(Registers.ConfigAddress);
  CrMmioWrite32(Registers.ConfigAddress, (Config & ~7U) | Registers.PdcType);

  if (Registers.SpiMask != 0) {
    // Read spi config register
    UINT32 SpiCfgRegVal =
        CrMmioRead32(Registers.SpiAddress);
    // Clear previous config
    SpiCfgRegVal = CLR_BITS(SpiCfgRegVal, Registers.SpiMask);
    // Mask if level triggered
    // Android spi_configure_type clears this bit again for an edge route.
    if (Registers.GicType == CR_INTERRUPT_TRIGGER_LEVEL_HIGH)
      SpiCfgRegVal = SET_BITS(SpiCfgRegVal, Registers.SpiMask);
    // Write back
    CrMmioWrite32(Registers.SpiAddress, SpiCfgRegVal);
  }

  // Later, you should configure GIC type and register handler.
  if (GicTypeOut != NULL) {
    *GicTypeOut = Registers.GicType;
  }
  return CR_SUCCESS;
}

CR_STATUS
PdcCheckInterruptEnabled(IN PdcDeviceContext *Ctx, IN UINT16 PdcPinNumber,
                         BOOLEAN *Enabled) {
  UINT32 RegVal = 0;
  UINT32 Index = PdcPinNumber >> 5;  // pin / 32
  UINT32 Mask = PdcPinNumber & 0x1f; // pin % 32

  if (Enabled == NULL) {
    return CR_INVALID_PARAMETER;
  }
  CR_STATUS Status = PdcValidateRegister(Ctx, PdcPinNumber,
                                         PDC_IRQ_ENABLE_BANK + Index * sizeof(UINT32));
  if (CR_ERROR(Status)) return Status;

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

  if (Enable > TRUE) return CR_INVALID_PARAMETER;
  CR_STATUS Status = PdcValidateRegister(Ctx, PdcPinNumber,
                                         PDC_IRQ_ENABLE_BANK + Index * sizeof(UINT32));
  if (CR_ERROR(Status)) return Status;

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
  for (UINT16 Range = 0; Range < Ctx->PdcRangeCount; ++Range) {
    for (UINT16 i = 0; i < PdcRegion->PdcCount; i++) {
      RegIndex = (i + PdcRegion->PdcPinBase) >> 5;
      IrqIndex = (i + PdcRegion->PdcPinBase) & 0x1f;
      IrqEnBankRegVal = CrMmioRead32(Ctx->BaseAddress + PDC_IRQ_ENABLE_BANK +
                                     (RegIndex * sizeof(UINT32)));
      IrqEnBankRegVal = CLR_BITS(IrqEnBankRegVal, BIT(IrqIndex));
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
  if (*Ctx == NULL) {
    log_err("PdcLibInit requires a target context.");
    return CR_NOT_FOUND;
  }
  if ((*Ctx)->ActiveInterrupts != 0) return CR_BUSY;
  CR_STATUS Status=PdcValidateRanges(*Ctx);
  if (CR_ERROR(Status)) return Status;
  // Validate every range before disabling any interrupt. The array has a
  // declared count; a full array does not contain a spare sentinel entry.
  for (UINT16 I = 0; I < (*Ctx)->PdcRangeCount; ++I) {
    PdcRange *Range = &(*Ctx)->PdcRegion[I];
    UINT32 End = (UINT32)Range->PdcPinBase + Range->PdcCount;
    Status=PdcValidateWord((*Ctx)->BaseAddress,(*Ctx)->Size,
                            PDC_IRQ_ENABLE_BANK+((End-1)/32)*sizeof(UINT32));
    if (CR_ERROR(Status)) return Status;
    Status=PdcValidateWord((*Ctx)->BaseAddress,(*Ctx)->Size,
                            PDC_IRQ_I_CFG+(End-1)*sizeof(UINT32));
    if (CR_ERROR(Status)) return Status;
  }
  // Disable All IRQs in PDC
  PdcPinsMapping(*Ctx);
  return CR_SUCCESS;
}
