/* Validate complete ACPI tables before publishing GPIO/PDC configuration.
 * SPDX-License-Identifier: MIT */
#include <Library/GpioConfigLib.h>

enum { RegFields = 21, PinFields = 7, PdcFields = 3, ReservedFields = 2 };

STATIC BOOLEAN Package(CONST CR_PEP_VALUE *Value) {
  return Value != NULL && Value->Type == CrPepPackage &&
    (Value->Package.Count == 0 || Value->Package.Values != NULL);
}
STATIC CONST CR_PEP_VALUE *Table(CONST CR_PEP_VALUE *Value, BOOLEAN Rows) {
  if (!Package(Value)) return NULL;
  if (Value->Package.Count == 1 && Package(&Value->Package.Values[0])) {
    CONST CR_PEP_VALUE *Child = &Value->Package.Values[0];
    if (!Rows || Child->Package.Count == 0 ||
        Child->Package.Values[0].Type == CrPepPackage) return Child;
  }
  return Value;
}
STATIC BOOLEAN Bounds(CONST CR_PEP_VALUE *Value, UINTN Min, UINTN Max) {
  return Package(Value) && Value->Package.Count >= Min && Value->Package.Count <= Max;
}
STATIC BOOLEAN Row(CONST CR_PEP_VALUE *Value, UINTN Count, UINT32 *Fields) {
  if (!Bounds(Value, Count, Count)) return FALSE;
  for (UINTN Index = 0; Index < Count; ++Index) {
    CONST CR_PEP_VALUE *Field = &Value->Package.Values[Index];
    if (Field->Type != CrPepInteger || Field->Integer > CR_MAX_UINT32) return FALSE;
    Fields[Index] = (UINT32)Field->Integer;
  }
  return TRUE;
}
STATIC BOOLEAN Bit(UINT32 Start, UINT32 Width) {
  return Start == 0xff || (Width >= 1 && Width <= 32 && Start <= 32 - Width);
}
STATIC UINT32 Mask(UINT32 Start, UINT32 Width) {
  if (Start == 0xff) return 0;
  return (UINT32)(((1ULL << Width) - 1) << Start);
}
STATIC BOOLEAN RegisterRow(CONST CR_PEP_VALUE *Value, UINTN Index, UINT32 *Fields) {
  STATIC CONST UINT8 Width[16] = {4,2,3,1,1,1,1,1,1,1,3,0,1,1,2,0};
  if (!Row(Value, RegFields, Fields)) return FALSE;
  for (UINTN I = 0; I < 5; ++I) {
    if ((Fields[I] & 3) != 0) return FALSE;
    if (Index != 0 && I != 0 && Fields[I] > CR_MAX_UINT32 - Fields[0]) return FALSE;
  }
  if (Index == 0 && Fields[0] != 0) return FALSE; // Core's normal control bank.
  for (UINTN I = 5; I < RegFields; ++I) {
    if (I == 16) { // KPSS target value, 0xff only when target is absent.
      if ((Fields[15] == 0xff && Fields[I] != 0xff) ||
          (Fields[15] != 0xff && Fields[I] > 7)) return FALSE;
    } else if (I == 20) {
      if (Fields[19] == 0xff) {
        if (Fields[I] != 0 && Fields[I] != 0xff) return FALSE;
      } else if (Fields[I] != 1 && Fields[I] != 2) return FALSE;
    } else if (!Bit(Fields[I], I == 19 ? Fields[20] : Width[I - 5])) return FALSE;
  }
  return TRUE;
}
STATIC BOOLEAN Covers(CONST CR_PEP_VALUE *Ranges, UINT32 Pin) {
  for (UINTN I = 0; I < Ranges->Package.Count; ++I) {
    CONST CR_PEP_VALUE *R = Ranges->Package.Values[I].Package.Values;
    if (Pin >= R[0].Integer && Pin < R[0].Integer + R[2].Integer) return TRUE;
  }
  return FALSE;
}

CR_STATUS CrGpioConfigLoad(CONST CR_PEP_VALUE *Registers,
                          CONST CR_PEP_VALUE *Tiles,
                          CONST CR_PEP_VALUE *Pins,
                          CONST CR_PEP_VALUE *PdcRanges,
                          CONST CR_PEP_VALUE *ReservedRanges,
                          UINTN TlmmSize, GpioDeviceContext *Gpio,
                          PdcDeviceContext *Pdc) {
  UINT32 V[RegFields], R[RegFields];
  BOOLEAN Seen[GPIO_PINS_MAX] = {FALSE};
  BOOLEAN HaveReserved = ReservedRanges != NULL;
  Registers = Table(Registers, TRUE); Tiles = Table(Tiles, FALSE);
  Pins = Table(Pins, TRUE); PdcRanges = Table(PdcRanges, TRUE);
  if (ReservedRanges != NULL) ReservedRanges = Table(ReservedRanges, TRUE);
  if (Gpio == NULL || Pdc == NULL || TlmmSize < 4 ||
      !Bounds(Registers, 1, GPIO_MAX_REG_CFG) || !Bounds(Tiles, 1, GPIO_TLMM_TILE_MAX) ||
      !Bounds(Pins, 1, GPIO_PINS_MAX) || !Bounds(PdcRanges, 1, PDC_RANGE_COUNT_MAX) ||
      (HaveReserved && !Bounds(ReservedRanges, 0, GPIO_PINS_MAX)))
    return CR_INVALID_PARAMETER;
  for (UINTN I = 0; I < Registers->Package.Count; ++I)
    if (!RegisterRow(&Registers->Package.Values[I], I, V)) return CR_INVALID_PARAMETER;
  for (UINTN I = 0; I < Tiles->Package.Count; ++I) {
    CONST CR_PEP_VALUE *Tile = &Tiles->Package.Values[I];
    if (Tile->Type != CrPepInteger || Tile->Integer > CR_MAX_UINT32 ||
        (Tile->Integer & 3) != 0 || Tile->Integer > TlmmSize - 4) return CR_INVALID_PARAMETER;
  }
  for (UINTN I = 0; I < PdcRanges->Package.Count; ++I) {
    if (!Row(&PdcRanges->Package.Values[I], PdcFields, V) ||
        V[0] > 0xffff || V[1] >= 988 || V[2] == 0 || V[2] > 0xffff ||
        V[0] + V[2] > 0x10000 || V[1] + V[2] > 988) return CR_INVALID_PARAMETER;
    for (UINTN J = 0; J < I; ++J) {
      CONST CR_PEP_VALUE *Previous = PdcRanges->Package.Values[J].Package.Values;
      if ((V[0] < Previous[0].Integer + Previous[2].Integer &&
           Previous[0].Integer < (UINT64)V[0] + V[2]) ||
          (V[1] < Previous[1].Integer + Previous[2].Integer &&
           Previous[1].Integer < (UINT64)V[1] + V[2])) return CR_INVALID_PARAMETER;
    }
  }
  for (UINTN I = 0; I < Pins->Package.Count; ++I) {
    if (!Row(&Pins->Package.Values[I], PinFields, V) || V[0] >= Pins->Package.Count ||
        Seen[V[0]] || V[1] >= Tiles->Package.Count || V[2] >= Registers->Package.Count ||
        V[3] > 0xffff || (V[3] != GPIO_PDC_PIN_NUMBER_INVALID && !Covers(PdcRanges, V[3])) ||
        (V[4] != 0xff && (V[4] >= 32 || (V[5] & 3) != 0)) || V[6] > 0xffff)
      return CR_INVALID_PARAMETER;
    Seen[V[0]] = TRUE;
    (VOID)RegisterRow(&Registers->Package.Values[V[2]], V[2], R);
    UINT64 Base = V[2] == 0 ? Tiles->Package.Values[V[1]].Integer +
                                   (UINT64)V[0] * GPIO_TLMM_PIN_STRIDE : R[0];
    for (UINTN J = 0; J < 5; ++J) {
      UINT64 Offset = Base + (J == 0 ? 0 : R[J]);
      if (Offset > TlmmSize - 4) return CR_INVALID_PARAMETER;
    }
    if (V[4] != 0xff && V[5] > TlmmSize - 4) return CR_INVALID_PARAMETER;
    if (V[3] != GPIO_PDC_PIN_NUMBER_INVALID) {
      for (UINTN J = 0; J < I; ++J)
        if (Pins->Package.Values[J].Package.Values[3].Integer == V[3])
          return CR_INVALID_PARAMETER;
    }
  }
  if (ReservedRanges != NULL) {
    for (UINTN I = 0; I < ReservedRanges->Package.Count; ++I)
      if (!Row(&ReservedRanges->Package.Values[I], ReservedFields, V) ||
          V[0] >= Pins->Package.Count || V[1] == 0 ||
          V[1] > Pins->Package.Count - V[0]) return CR_INVALID_PARAMETER;
  }
  // No errors or hardware operations remain beyond this point. Input storage
  // remains immutable throughout this serialized two-pass transaction.
  for (UINTN I = 0; I < Registers->Package.Count; ++I) {
    GpioConfigRegInfo *C = &Gpio->RegCfg[I];
    (VOID)RegisterRow(&Registers->Package.Values[I], I, V);
    UINT32 Bank = I == 0 ? 0 : V[0];
    C->Index = (UINT8)I; C->ControlRegOffset = V[0];
    C->IoRegOffset = Bank + V[1]; C->IntCfgRegOffset = Bank + V[2];
    C->IntStatusRegOffset = Bank + V[3]; C->IntTargetRegOffset = Bank + V[4];
    C->MuxSelMsk = Mask(V[5], 4); C->PullMsk = Mask(V[6], 2);
    C->DriveStrengthMsk = Mask(V[7], 3); C->EgpioEnabledMsk = Mask(V[8], 1);
    C->EgpioPresentMsk = Mask(V[9], 1); C->OutputEnableMsk = Mask(V[10], 1);
    C->InputMsk = Mask(V[11], 1); C->OutputMsk = Mask(V[12], 1);
    C->InterruptEnMsk = Mask(V[13], 1); C->InterruptStatusMsk = Mask(V[14], 1);
    C->InterruptTargetMsk = Mask(V[15], 3); C->TargetKpssVal = (UINT8)V[16];
    C->InterruptRawStatusMsk = Mask(V[17], 1); C->InterruptPolarityMsk = Mask(V[18], 1);
    C->InterruptDetectionMsk = Mask(V[19], V[20]); C->InterruptDetectionWidth = (UINT8)V[20];
    C->WakeMsk = 0;
  }
  for (UINTN I = 0; I < Tiles->Package.Count; ++I)
    Gpio->Tiles[I].TileOffset = (UINT32)Tiles->Package.Values[I].Integer;
  Gpio->PinCount = (UINT16)Pins->Package.Count;
  for (UINTN I = 0; I < Pins->Package.Count; ++I) {
    (VOID)Row(&Pins->Package.Values[I], PinFields, V);
    GpioPinInfo *Pin = &Gpio->GpioPins[V[0]];
    Pin->pTileInfo = &Gpio->Tiles[V[1]]; Pin->pRegCfg = &Gpio->RegCfg[V[2]];
    Pin->PdcPinNumber = (UINT16)V[3]; Pin->WakeBit = (UINT8)V[4];
    Pin->WakeRegOffset = V[5]; Pin->FuncMuxMap = V[6];
    Pin->PinAddress = 0; Pin->Reserved = FALSE;
  }
  Pdc->PdcRangeCount = (UINT16)PdcRanges->Package.Count;
  for (UINTN I = 0; I < PdcRanges->Package.Count; ++I) {
    (VOID)Row(&PdcRanges->Package.Values[I], PdcFields, V);
    Pdc->PdcRegion[I].PdcPinBase = (UINT16)V[0];
    Pdc->PdcRegion[I].GicSpiBase = (UINT16)V[1]; Pdc->PdcRegion[I].PdcCount = (UINT16)V[2];
  }
  if (ReservedRanges != NULL) {
    for (UINTN I = 0; I < ReservedRanges->Package.Count; ++I) {
      (VOID)Row(&ReservedRanges->Package.Values[I], ReservedFields, V);
      for (UINT32 J = V[0]; J < V[0] + V[1]; ++J) Gpio->GpioPins[J].Reserved = TRUE;
    }
  }
  return CR_SUCCESS;
}

CR_STATUS CrGpioBankLayout(UINT16 PinCount, UINT16 PdcPinCount,
                          UINT32 *PhysicalBanks, UINT32 *TotalPins) {
  if (PinCount == 0 || PinCount > GPIO_PINS_MAX || PdcPinCount > GPIO_PINS_MAX ||
      PhysicalBanks == NULL || TotalPins == NULL) return CR_INVALID_PARAMETER;
  UINT32 Banks = ((UINT32)PinCount + 63) / 64;
  *PhysicalBanks = Banks;
  *TotalPins = PdcPinCount == 0 ? PinCount : (Banks + PdcPinCount - 1) * 64 + 1;
  return CR_SUCCESS;
}
CR_STATUS CrGpioBankMask(UINT16 PinCount, UINT16 PdcPinCount, UINT32 Bank,
                        UINT64 *ValidMask) {
  UINT32 Banks, Total;
  if (ValidMask == NULL || CR_ERROR(CrGpioBankLayout(PinCount,PdcPinCount,&Banks,&Total)) ||
      Bank >= Banks + PdcPinCount) return CR_INVALID_PARAMETER;
  if (Bank >= Banks) *ValidMask = 1;
  else {
    UINT32 Remaining = (UINT32)PinCount - Bank * 64;
    *ValidMask = Remaining >= 64 ? CR_MAX_UINT64 : (1ULL << Remaining) - 1;
  }
  return CR_SUCCESS;
}
CR_STATUS CrGpioResolveBankPin(UINT16 PinCount, CONST UINT16 *PdcPins,
                              UINT16 PdcPinCount, UINT32 Bank, UINT32 Pin,
                              UINT16 *PhysicalPin) {
  UINT64 Mask;
  UINT32 Banks, Total;
  if (PhysicalPin == NULL || (PdcPinCount != 0 && PdcPins == NULL) || Pin >= 64 ||
      CR_ERROR(CrGpioBankMask(PinCount,PdcPinCount,Bank,&Mask)) || (Mask & (1ULL << Pin)) == 0)
    return CR_INVALID_PARAMETER;
  (VOID)CrGpioBankLayout(PinCount,PdcPinCount,&Banks,&Total);
  UINT32 Index = Bank < Banks ? Bank * 64 + Pin : PdcPins[Bank - Banks];
  if (Index >= PinCount) return CR_INVALID_PARAMETER;
  *PhysicalPin = (UINT16)Index;
  return CR_SUCCESS;
}
CR_STATUS CrGpioResolveBankMask(UINT16 PinCount, CONST UINT16 *PdcPins,
                               UINT16 PdcPinCount, UINT32 Bank, UINT64 Mask,
                               UINT16 PhysicalPins[64]) {
  UINT64 Valid;
  UINT16 Pins[64];
  if (PhysicalPins == NULL ||
      CR_ERROR(CrGpioBankMask(PinCount,PdcPinCount,Bank,&Valid)) || (Mask & ~Valid) != 0)
    return CR_INVALID_PARAMETER;
  for (UINT32 I=0; I<64; ++I) {
    Pins[I]=0xffff;
    if ((Mask & (1ULL << I)) != 0 &&
        CR_ERROR(CrGpioResolveBankPin(PinCount,PdcPins,PdcPinCount,Bank,I,&Pins[I])))
      return CR_INVALID_PARAMETER;
  }
  for (UINT32 I=0; I<64; ++I) PhysicalPins[I]=Pins[I];
  return CR_SUCCESS;
}

STATIC UINT16 PhysicalForPdc(CONST GpioDeviceContext *Gpio, UINT16 PdcPin) {
  for (UINT16 I=0; I<Gpio->PinCount; ++I)
    if (Gpio->GpioPins[I].PdcPinNumber == PdcPin) return I;
  return 0xffff;
}

CR_STATUS CrGpioPdcRoutesLoad(CONST GpioDeviceContext *Gpio,
                            PdcDeviceContext *Pdc,
                            CONST CR_PEP_VALUE *DirectPins,
                            CONST UINT16 *GicIrqs, UINT16 Count,
                            CR_GPIO_BANK_ROUTE *Routes) {
  CR_GPIO_BANK_ROUTE Pending[GPIO_PINS_MAX];
  UINT16 DirectCount=0, FoundDirect=0, Unused;
  if (Gpio == NULL || Gpio->PinCount == 0 || Gpio->PinCount > GPIO_PINS_MAX ||
      Pdc == NULL || Count > GPIO_PINS_MAX ||
      (Count != 0 && (GicIrqs == NULL || Routes == NULL))) return CR_INVALID_PARAMETER;
  if (DirectPins != NULL) {
    DirectPins=Table(DirectPins,FALSE);
    if (!Bounds(DirectPins,0,GPIO_PINS_MAX)) return CR_INVALID_PARAMETER;
    DirectCount=(UINT16)DirectPins->Package.Count;
    for (UINT16 I=0; I<DirectCount; ++I) {
      CONST CR_PEP_VALUE *Pin=&DirectPins->Package.Values[I];
      if (Pin->Type != CrPepInteger || Pin->Integer >= 0xffff ||
          PhysicalForPdc(Gpio,(UINT16)Pin->Integer) != 0xffff ||
          CR_ERROR(PdcGetGicIrqFromPdcPin(Pdc,(UINT16)Pin->Integer,&Unused)))
        return CR_INVALID_PARAMETER;
      for (UINT16 J=0; J<I; ++J)
        if (DirectPins->Package.Values[J].Integer == Pin->Integer)
          return CR_INVALID_PARAMETER;
    }
  }
  for (UINT16 I=0; I<Count; ++I) {
    CR_GPIO_BANK_ROUTE *Route=&Pending[I];
    if (CR_ERROR(PdcGetPdcPinFromGicIrq(Pdc,GicIrqs[I],&Route->PdcPin)) ||
        Route->PdcPin == 0xffff) return CR_INVALID_PARAMETER;
    Route->PhysicalPin=PhysicalForPdc(Gpio,Route->PdcPin);
    if (Route->PhysicalPin == 0xffff) {
      BOOLEAN Allowed=FALSE;
      for (UINT16 J=0; J<DirectCount; ++J)
        if (DirectPins->Package.Values[J].Integer == Route->PdcPin) Allowed=TRUE;
      if (!Allowed) return CR_INVALID_PARAMETER;
      ++FoundDirect;
    } else {
      CONST GpioPinInfo *Pin=&Gpio->GpioPins[Route->PhysicalPin];
      if (Pin->Reserved || Pin->pRegCfg == NULL || Pin->pRegCfg->InterruptEnMsk == 0)
        return CR_INVALID_PARAMETER;
    }
    for (UINT16 J=0; J<I; ++J) {
      // Distinct banks cannot safely perform RMW of the same PDC/SPI word.
      if ((Pending[J].PdcPin >> 5) == (Route->PdcPin >> 5) ||
          ((GicIrqs[J]-32U) >> 5) == ((GicIrqs[I]-32U) >> 5))
        return CR_INVALID_PARAMETER;
    }
  }
  if (FoundDirect != DirectCount) return CR_INVALID_PARAMETER;
  for (UINT16 I=0; I<Count; ++I) Routes[I]=Pending[I];
  return CR_SUCCESS;
}

CR_STATUS CrGpioResolveBankRoute(UINT16 PinCount,
                               CONST CR_GPIO_BANK_ROUTE *PdcRoutes,
                               UINT16 PdcPinCount, UINT32 Bank, UINT32 Pin,
                               CR_GPIO_BANK_ROUTE *Route) {
  UINT64 Mask;
  UINT32 Banks, Total;
  if (Route == NULL || (PdcPinCount != 0 && PdcRoutes == NULL) || Pin >= 64 ||
      CR_ERROR(CrGpioBankMask(PinCount,PdcPinCount,Bank,&Mask)) ||
      (Mask & (1ULL << Pin)) == 0) return CR_INVALID_PARAMETER;
  (VOID)CrGpioBankLayout(PinCount,PdcPinCount,&Banks,&Total);
  CR_GPIO_BANK_ROUTE Resolved;
  if (Bank < Banks) Resolved=(CR_GPIO_BANK_ROUTE){(UINT16)(Bank*64+Pin),0xffff};
  else {
    Resolved=PdcRoutes[Bank-Banks];
    if (Resolved.PdcPin == 0xffff ||
        (Resolved.PhysicalPin != 0xffff && Resolved.PhysicalPin >= PinCount))
      return CR_INVALID_PARAMETER;
  }
  *Route=Resolved;
  return CR_SUCCESS;
}

CR_STATUS CrGpioResolveBankRouteMask(UINT16 PinCount,
                                   CONST CR_GPIO_BANK_ROUTE *PdcRoutes,
                                   UINT16 PdcPinCount, UINT32 Bank, UINT64 Mask,
                                   CR_GPIO_BANK_ROUTE Routes[64]) {
  UINT64 Valid;
  CR_GPIO_BANK_ROUTE Pending[64];
  if (Routes == NULL || (PdcPinCount != 0 && PdcRoutes == NULL) ||
      CR_ERROR(CrGpioBankMask(PinCount,PdcPinCount,Bank,&Valid)) || (Mask & ~Valid) != 0)
    return CR_INVALID_PARAMETER;
  for (UINT32 I=0; I<64; ++I) {
    Pending[I]=(CR_GPIO_BANK_ROUTE){0xffff,0xffff};
    if ((Mask & (1ULL << I)) != 0 &&
        CR_ERROR(CrGpioResolveBankRoute(PinCount,PdcRoutes,PdcPinCount,Bank,I,&Pending[I])))
      return CR_INVALID_PARAMETER;
  }
  for (UINT32 I=0; I<64; ++I) Routes[I]=Pending[I];
  return CR_SUCCESS;
}
