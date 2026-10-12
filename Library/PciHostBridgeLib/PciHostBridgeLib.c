/** @file
  Crane PCI host bridge description provider.

  This library cold-initializes the target controllers and gives the generic
  PciHostBridgeDxe driver owned root-bridge descriptions.  No private PCIe
  producer protocol is installed.

  SPDX-License-Identifier: MIT
**/

#include <PiDxe.h>

#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Library/CranePcieHostLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>

GLOBAL_REMOVE_IF_UNREFERENCED
CHAR16  *mPciHostBridgeLibAcpiAddressSpaceTypeStr[] = {
  L"Mem", L"I/O", L"Bus"
};

STATIC
VOID
CanonicalizeEmptyAperture (
  IN OUT PCI_ROOT_BRIDGE_APERTURE  *Aperture
  )
{
  if (Aperture->Base > Aperture->Limit) {
    Aperture->Base        = 1;
    Aperture->Limit       = 0;
    Aperture->Translation = 0;
  }
}

STATIC
VOID
CanonicalizeBridge (
  IN OUT PCI_ROOT_BRIDGE  *Bridge
  )
{
  CanonicalizeEmptyAperture (&Bridge->Bus);
  CanonicalizeEmptyAperture (&Bridge->Io);
  CanonicalizeEmptyAperture (&Bridge->Mem);
  CanonicalizeEmptyAperture (&Bridge->MemAbove4G);
  CanonicalizeEmptyAperture (&Bridge->PMem);
  CanonicalizeEmptyAperture (&Bridge->PMemAbove4G);
}

STATIC PcieDeviceContext       *mPcieContext;
STATIC CONST PcieTargetContext *mPcieTarget;
STATIC PcieIoOps                mPcieIo;
STATIC UINT32                   mPcieInitializedMask;
STATIC EFI_STATUS               mPcieInitStatus = EFI_NOT_READY;
STATIC BOOLEAN                  mPcieInitAttempted;
STATIC BOOLEAN                  mPcieAdaptersInitialized;
STATIC PCIE_DIAGNOSE_LINK        mPciePlatformDiagnoseLink;

#define CRANE_PCIE_RESOURCE_REFS  64U

typedef struct {
  CONST CHAR8  *Id;
  UINT16        Count;
  BOOLEAN       Ready;
  BOOLEAN       Owned;
  /** Requested rail operating point for this adapter's acquired RPMh vote. */
  UINT32        VoltageMv;
  UINT8         Mode;
} CRANE_PCIE_RESOURCE_REF;

typedef struct {
  UINT64   SupplyMask;
  BOOLEAN  WlanManaged;
  BOOLEAN  XoManaged;
  BOOLEAN  Ready;
  UINT64 StepMask;
} CRANE_PCIE_ENDPOINT_STATE;

typedef struct {
  EFI_RPMH_CR_PROTOCOL       *Rpmh;
  EFI_GPIO_CR_PROTOCOL       *Gpio;
  EFI_CLOCK_CR_PROTOCOL      *ClockProtocol;
  EFI_INTERCONNECT_CR_PROTOCOL *Interconnect;
  EFI_SMMU_CR_PROTOCOL       *Smmu;
  CRANE_PCIE_RESOURCE_REF     SupplyRefs[CRANE_PCIE_RESOURCE_REFS];
  CRANE_PCIE_RESOURCE_REF     ClockRefs[CRANE_PCIE_RESOURCE_REFS];
  INTERCONNECT_PATH_HANDLE    MemPaths[PCIE_MAX_CONTROLLERS];
  INTERCONNECT_PATH_HANDLE    CpuPaths[PCIE_MAX_CONTROLLERS];
  BOOLEAN                     InterconnectReady[PCIE_MAX_CONTROLLERS];
  BOOLEAN                     IommuAttached[PCIE_MAX_CONTROLLERS];
  BOOLEAN                     DomainOwned[PCIE_MAX_CONTROLLERS];
  CRANE_PCIE_ENDPOINT_STATE    Endpoints[PCIE_MAX_CONTROLLERS];
} CRANE_PCIE_RESOURCE_CONTEXT;

STATIC CRANE_PCIE_RESOURCE_CONTEXT  mPcieResources;

STATIC
CR_STATUS
ValidateClockProvider (
  IN CONST PcieTargetClock  *Clock
  )
{
  if ((Clock == NULL) || (Clock->Controller == NULL) || (Clock->Id == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  return CranePcieHostClockProviderValid (Clock) ?
         CR_SUCCESS : CR_UNSUPPORTED;
}

STATIC
CRANE_PCIE_RESOURCE_REF *
FindResourceRef (
  IN OUT CRANE_PCIE_RESOURCE_REF  *Refs,
  IN CONST CHAR8                  *Id,
  IN BOOLEAN                       Allocate
  )
{
  UINTN  Index;
  CRANE_PCIE_RESOURCE_REF  *Empty;

  Empty = NULL;
  for (Index = 0; Index < CRANE_PCIE_RESOURCE_REFS; Index++) {
    if ((Refs[Index].Id != NULL) && (AsciiStrCmp (Refs[Index].Id, Id) == 0)) {
      return &Refs[Index];
    }
    if ((Empty == NULL) && (Refs[Index].Id == NULL)) {
      Empty = &Refs[Index];
    }
  }
  if (Allocate && (Empty != NULL)) {
    Empty->Id = Id;
    return Empty;
  }
  return NULL;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetSupply (
  IN VOID                    *Context,
  IN CONST PcieTargetSupply  *Supply,
  IN BOOLEAN                  Enable
  )
{
  CRANE_PCIE_RESOURCE_REF  *Ref;
  EFI_STATUS                Status;

  (VOID)Context;
  if ((Supply == NULL) || (Supply->Id == NULL) ||
      (Supply->Controller == NULL) ||
      ((AsciiStrCmp (Supply->Controller, "rpmh") != 0) &&
       (AsciiStrCmp (Supply->Controller, "rpmh-enable") != 0)) ||
      (mPcieResources.Rpmh == NULL) ||
      (mPcieResources.Rpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION) ||
      (mPcieResources.Rpmh->AcquireVreg == NULL) ||
      (mPcieResources.Rpmh->RpmhEnableVreg == NULL)) {
    return CR_UNSUPPORTED;
  }
  if (Supply->LoadUa != 0) {
    return CR_UNSUPPORTED;
  }

  if (AsciiStrCmp (Supply->Controller, "rpmh-enable") == 0) {
    /* clk-rpmh VRM clocks share the regulator enable register at +4.
       Retain the provider-owned vote: never invent a voltage or mode and
       never withdraw an inherited/shared RF reference on link failure. */
    if (Supply->VoltageMv != 0 || Supply->Mode != 0 ||
        mPcieResources.Rpmh->EnsureRetainedVregEnabled == NULL)
      return CR_UNSUPPORTED;
    return Enable ? mPcieResources.Rpmh->EnsureRetainedVregEnabled (
        mPcieResources.Rpmh, Supply->Id) : CR_SUCCESS;
  }

  Ref = FindResourceRef (
          mPcieResources.SupplyRefs, Supply->Id, Enable);
  if (Ref == NULL) {
    return Enable ? CR_OUT_OF_RESOURCES : CR_SUCCESS;
  }
  if (Enable && (Ref->Count != 0)) {
    if (Ref->Count == MAX_UINT16) {
      return CR_OUT_OF_RESOURCES;
    }
    Ref->Count++;
    /* The caller records even a failed acquisition for rollback. Give it a
       reference, but never let it use another port's incomplete vote. */
    return Ref->Ready ? CR_SUCCESS : CR_BUSY;
  }
  if (!Enable) {
    if (Ref->Count == 0) {
      return CR_SUCCESS;
    }
    Ref->Count--;
    if (Ref->Count != 0) {
      return CR_SUCCESS;
    }
    Ref->Ready = FALSE;
    if (!Ref->Owned) {
      return CR_SUCCESS;
    }
  } else {
    Ref->Count = 1;
    Ref->Ready = FALSE;
    Ref->Owned = FALSE;
    Ref->VoltageMv = Supply->VoltageMv;
    Ref->Mode = Supply->Mode;
  }

  if (Enable) {
    Status = mPcieResources.Rpmh->AcquireVreg (
                                    mPcieResources.Rpmh, Supply->Id,
                                    Supply->VoltageMv, Supply->Mode,
                                    &Ref->Owned);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Ref->Ready = TRUE;
    return CR_SUCCESS;
  }
  Status = mPcieResources.Rpmh->RpmhEnableVreg (
                                mPcieResources.Rpmh,
                                Supply->Id,
                                FALSE
                                );
  if (EFI_ERROR (Status)) {
    Ref->Count = 1;
    return Status;
  }
  Ref->Owned = FALSE;
  return CR_SUCCESS;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetClock (
  IN VOID                   *Context,
  IN CONST PcieTargetClock  *Clock,
  IN BOOLEAN                 Enable
  )
{
  CRANE_PCIE_RESOURCE_REF  *Ref;
  CR_STATUS                 Status;

  (VOID)Context;
  Status = ValidateClockProvider (Clock);
  if (CR_ERROR (Status)) {
    return Status;
  }
  if ((Clock->Provider == PCIE_CLOCK_PROVIDER_GCC) &&
      ((mPcieResources.ClockProtocol == NULL) ||
       (mPcieResources.ClockProtocol->SetClock == NULL))) {
    return CR_UNSUPPORTED;
  }
  Ref = FindResourceRef (mPcieResources.ClockRefs, Clock->Id, Enable);
  if (Ref == NULL) {
    return Enable ? CR_OUT_OF_RESOURCES : CR_SUCCESS;
  }
  if (Enable && (Ref->Count != 0)) {
    if (Ref->Count == MAX_UINT16) {
      return CR_OUT_OF_RESOURCES;
    }
    if (!Ref->Ready) return CR_BUSY;
    Ref->Count++;
    return CR_SUCCESS;
  }
  if (!Enable) {
    if (Ref->Count == 0) {
      return CR_SUCCESS;
    }
    Ref->Count--;
    if (Ref->Count != 0) {
      return CR_SUCCESS;
    }
    Ref->Ready = FALSE;
  }

  Status = CR_SUCCESS;
  if (Enable) {
    Ref->Count = 1;
    Ref->Ready = FALSE;
  }
  if (Clock->Provider == PCIE_CLOCK_PROVIDER_GCC) {
    if (Enable) {
      Status = CrAcquireClock (mPcieResources.ClockProtocol, Clock->Controller,
                                Clock->Id, Clock->RateHz, &Ref->Owned);
      if (!Ref->Owned) Ref->Count = 0;
    } else {
      Status = mPcieResources.ClockProtocol->SetClock (
                 mPcieResources.ClockProtocol, Clock->Controller, Clock->Id, 0, FALSE);
      if (!CR_ERROR (Status)) Ref->Owned = FALSE;
    }
  }
  if (CR_ERROR (Status)) {
    if (!Enable) {
      Ref->Count = 1;
    }
    return Status;
  }
  if (Enable) {
    Ref->Count = 1;
    Ref->Ready = TRUE;
  }
  return CR_SUCCESS;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetPowerDomain (
  IN VOID                        *Context,
  IN CONST PcieTargetController  *Controller,
  IN BOOLEAN                      Enable
  )
{
  UINTN Index;
  EFI_STATUS Status;
  (VOID)Context;
  if ((Controller == NULL) || (Controller->PowerDomainController == NULL) ||
      (Controller->PowerDomainId == NULL) || mPcieTarget == NULL ||
      (mPcieResources.ClockProtocol == NULL) ||
      (mPcieResources.ClockProtocol->SetGdsc == NULL)) return CR_UNSUPPORTED;
  for (Index = 0; Index < mPcieTarget->ControllerCount && Index < PCIE_MAX_CONTROLLERS;
       ++Index) if (&mPcieTarget->Controllers[Index] == Controller) break;
  if (Index >= mPcieTarget->ControllerCount || Index >= PCIE_MAX_CONTROLLERS)
    return CR_INVALID_PARAMETER;
  if (Enable) {
    if (mPcieResources.DomainOwned[Index]) return CR_BUSY;
    return CrAcquireGdsc (mPcieResources.ClockProtocol,
        Controller->PowerDomainController, Controller->PowerDomainId,
        &mPcieResources.DomainOwned[Index]);
  }
  if (!mPcieResources.DomainOwned[Index]) return CR_SUCCESS;
  Status = mPcieResources.ClockProtocol->SetGdsc (mPcieResources.ClockProtocol,
        Controller->PowerDomainController, Controller->PowerDomainId, FALSE);
  if (!EFI_ERROR (Status)) mPcieResources.DomainOwned[Index] = FALSE;
  return Status;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetReset (
  IN VOID                   *Context,
  IN CONST PcieTargetReset  *Reset,
  IN BOOLEAN                 Assert
  )
{
  (VOID)Context;
  if ((Reset == NULL) || (Reset->Id == NULL) ||
      (Reset->Controller == NULL) ||
      (mPcieResources.ClockProtocol == NULL) ||
      (mPcieResources.ClockProtocol->SetReset == NULL)) {
    return CR_UNSUPPORTED;
  }
  return mPcieResources.ClockProtocol->SetReset (
             mPcieResources.ClockProtocol, Reset->Controller, Reset->Id,
             Assert);
}

STATIC CR_STATUS EFIAPI
CranePcieSetClockParent (
  IN VOID                *Context,
  IN CONST PcieTargetPhy *Phy,
  IN UINT32               Source
  )
{
  (VOID)Context;
  if ((Phy == NULL) || (Phy->AuxMuxController == NULL) ||
      (Phy->AuxMuxId == NULL) || (mPcieResources.ClockProtocol == NULL) ||
      (mPcieResources.ClockProtocol->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION) ||
      (mPcieResources.ClockProtocol->SetParent == NULL)) {
    return CR_UNSUPPORTED;
  }
  return mPcieResources.ClockProtocol->SetParent (
           mPcieResources.ClockProtocol, Phy->AuxMuxController,
           Phy->AuxMuxId, Source);
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetGpio (
  IN VOID                  *Context,
  IN CONST PcieTargetGpio  *Gpio,
  IN PCIE_GPIO_DIRECTION    Direction,
  IN BOOLEAN                Active
  )
{
  GpioConfigParams  Config;
  GpioConfigParams  ReadBack;
  EFI_STATUS        Status;
  BOOLEAN           Level;

  (VOID)Context;
  if ((Gpio == NULL) || (Gpio->Controller == NULL) ||
      (Gpio->Polarity == NULL) || (Gpio->Role == NULL) ||
      (AsciiStrCmp (Gpio->Controller, "tlmm") != 0) ||
      (mPcieResources.Gpio == NULL) ||
      (mPcieResources.Gpio->InitGpioConfigParams == NULL) ||
      (mPcieResources.Gpio->ConfigGpio == NULL) ||
      (mPcieResources.Gpio->GetGpioConfig == NULL)) {
    return CR_UNSUPPORTED;
  }
  if ((Direction != PCIE_GPIO_INPUT) && (Direction != PCIE_GPIO_OUTPUT) &&
      (Direction != PCIE_GPIO_PRESERVE)) {
    return CR_INVALID_PARAMETER;
  }

  ZeroMem (&Config, sizeof (Config));
  Config.PinNumber = Gpio->Pin;
  Status = mPcieResources.Gpio->InitGpioConfigParams (
                                  mPcieResources.Gpio, &Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (Direction == PCIE_GPIO_PRESERVE) {
    Status = mPcieResources.Gpio->GetGpioConfig (mPcieResources.Gpio, &Config);
    if (EFI_ERROR (Status)) return Status;
  }
  if ((Gpio->Function > GPIO_FUNC_MAX) ||
      (Gpio->DriveStrengthMa < 2) || (Gpio->DriveStrengthMa > 16) ||
      ((Gpio->DriveStrengthMa & 1U) != 0) ||
      (Gpio->Pull > PCIE_GPIO_PULL_UP)) {
    return CR_INVALID_PARAMETER;
  }
  Config.FunctionSel = (GpioFunctionType)Gpio->Function;
  Config.DriveStrength =
      (GpioDriveStrength)(Gpio->DriveStrengthMa / 2U - 1U);
  Config.Pull = Gpio->Pull == PCIE_GPIO_PULL_UP
                    ? GPIO_PULL_UP
                    : (Gpio->Pull == PCIE_GPIO_PULL_DOWN
                           ? GPIO_PULL_DOWN
                           : GPIO_PULL_NONE);
  if (Direction != PCIE_GPIO_PRESERVE)
    Config.OutputEnable = (Direction == PCIE_GPIO_OUTPUT);
  if (Config.OutputEnable && Direction != PCIE_GPIO_PRESERVE) {
    Level = Active;
    if (AsciiStrCmp (Gpio->Polarity, "GPIO_ACTIVE_LOW") == 0) {
      Level = !Level;
    } else if (AsciiStrCmp (Gpio->Polarity, "GPIO_ACTIVE_HIGH") != 0) {
      return CR_INVALID_PARAMETER;
    }
    Config.OutputValue = Level ? GPIO_VALUE_HIGH : GPIO_VALUE_LOW;
  }
  Status = mPcieResources.Gpio->ConfigGpio (mPcieResources.Gpio, &Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ZeroMem (&ReadBack, sizeof (ReadBack));
  ReadBack.PinNumber = Gpio->Pin;
  Status = mPcieResources.Gpio->GetGpioConfig (
                                  mPcieResources.Gpio, &ReadBack);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "PCIe: GPIO %u (%a) readback failed: %r\n",
            Gpio->Pin, Gpio->Role, Status));
    return Status;
  }

  DEBUG ((DEBUG_INIT,
          "PCIe: GPIO %u (%a) mux=%u OE=%u pull=%u drive=%u mA %a=%u\n",
          Gpio->Pin, Gpio->Role, ReadBack.FunctionSel, ReadBack.OutputEnable,
          ReadBack.Pull, 2U * ((UINT32)ReadBack.DriveStrength + 1U),
          ReadBack.OutputEnable ? "output-latch" : "input", ReadBack.OutputValue));
  // Check the output latch only on driven pins; input levels belong to the endpoint.
  if ((ReadBack.FunctionSel != Config.FunctionSel) ||
      (ReadBack.OutputEnable != Config.OutputEnable) ||
      (ReadBack.Pull != Config.Pull) ||
      (ReadBack.DriveStrength != Config.DriveStrength) ||
      (Config.OutputEnable && (ReadBack.OutputValue != Config.OutputValue))) {
    DEBUG ((DEBUG_ERROR, "PCIe: GPIO %u (%a) configuration did not latch\n",
            Gpio->Pin, Gpio->Role));
    return CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS
CranePcieSetBoardPower (
  CONST PcieIoOps *Io, CONST PcieTargetController *Controller, BOOLEAN Enable)
{
  CONST PcieTargetPowerSequence *Sequence = Controller->PowerSequence;
  CRANE_PCIE_ENDPOINT_STATE *State;
  UINT16 Cursor, Index;
  CR_STATUS Status;
  if (mPcieTarget == NULL || Controller->Domain >= PCIE_MAX_CONTROLLERS ||
      Sequence == NULL || Sequence->Steps == NULL || Sequence->Count == 0 || Sequence->Count > 64 ||
      Io->SetSupply == NULL || Io->SetGpio == NULL || Io->DelayUs == NULL)
    return CR_INVALID_PARAMETER;
  State = &mPcieResources.Endpoints[Controller->Domain];
  if (Enable && State->Ready) return CR_SUCCESS;
  if (Enable && State->StepMask != 0) return CR_BUSY;
  State->Ready = FALSE;
  for (Cursor = 0; Cursor < Sequence->Count; ++Cursor) {
    CONST PcieTargetPowerStep *Step;
    Index = Enable ? Cursor : Sequence->Count - 1 - Cursor;
    Step = &Sequence->Steps[Index];
    if (Step->Kind == PCIE_POWER_DELAY) {
      if (Enable) Io->DelayUs (Io->Context, Step->DelayUs);
      continue;
    }
    if (!Enable && (State->StepMask & LShiftU64 (1, Index)) == 0) continue;
    /* Failed writes/acquisitions may still have changed hardware. */
    if (Enable) State->StepMask |= LShiftU64 (1, Index);
    if (Step->Kind == PCIE_POWER_SUPPLY && Step->ResourceIndex < mPcieTarget->SupplyCount)
      Status = Io->SetSupply (Io->Context, &mPcieTarget->Supplies[Step->ResourceIndex], Enable);
    else if (Step->Kind == PCIE_POWER_GPIO && Step->ResourceIndex < mPcieTarget->GpioCount)
      Status = Io->SetGpio (Io->Context, &mPcieTarget->Gpios[Step->ResourceIndex], PCIE_GPIO_OUTPUT, Enable);
    else if (Step->Kind == PCIE_POWER_PINCTRL && Step->ResourceIndex < mPcieTarget->GpioCount)
      Status = Enable ? Io->SetGpio (Io->Context, &mPcieTarget->Gpios[Step->ResourceIndex], PCIE_GPIO_PRESERVE, FALSE) : CR_SUCCESS;
    else return CR_INVALID_PARAMETER;
    if (CR_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "PCIe: port %u board power step %u enable=%u failed: %r\n",
              Controller->Domain, Index, Enable, Status));
      return Status;
    }
    if (!Enable) State->StepMask &= ~LShiftU64 (1, Index);
    if (Enable && Step->DelayUs != 0) Io->DelayUs (Io->Context, Step->DelayUs);
  }
  State->Ready = Enable;
  DEBUG ((DEBUG_INFO, "PCIe: port %u board power enable=%u complete\n", Controller->Domain, Enable));
  return CR_SUCCESS;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetEndpointPower (
  IN CONST PcieIoOps             *Io,
  IN CONST PcieTargetController  *Controller,
  IN BOOLEAN                     Enable
  )
{
  CONST PcieTargetEndpointPower  *Endpoint;
  CRANE_PCIE_ENDPOINT_STATE      *State;
  GpioConfigParams               InitialWlan;
  CR_STATUS                      Status;
  UINT16                         Index;
  UINT8                          Bt, Sw;
  CONST CHAR8                   *Stage;

  if (Controller == NULL || Io == NULL) return CR_INVALID_PARAMETER;
  if (Controller->PowerSequence != NULL)
    return CranePcieSetBoardPower (Io, Controller, Enable);
  Endpoint = Controller->EndpointPower;
  if (Endpoint == NULL) return CR_SUCCESS;
  if (mPcieTarget == NULL || Controller->Domain >= PCIE_MAX_CONTROLLERS ||
      Endpoint->Compatible == NULL ||
      AsciiStrCmp (Endpoint->Compatible, "qcom,wcn6855-pmu") != 0 ||
      Endpoint->SupplyCount == 0 || Endpoint->SupplyCount > 64 ||
      Endpoint->SupplyOffset > mPcieTarget->SupplyCount ||
      Endpoint->SupplyCount > mPcieTarget->SupplyCount - Endpoint->SupplyOffset ||
      Io->SetSupply == NULL || Io->SetGpio == NULL || Io->DelayUs == NULL ||
      mPcieResources.Gpio == NULL || mPcieResources.Gpio->GetGpioConfig == NULL) {
    return CR_UNSUPPORTED;
  }
  State = &mPcieResources.Endpoints[Controller->Domain];
  if (!Enable) {
    State->Ready = FALSE;
    /* PcieLib has already cleared LTSSM, asserted PERST and blocked DMA.
       Stop only WLAN/XO owned by this sequencer. Never change BT_EN/SWCTRL.
       Retain each failed step and its rails for a later shutdown retry. */
    if (State->WlanManaged) {
      Status = Io->SetGpio (Io->Context, &Endpoint->EnableGpio,
                            PCIE_GPIO_OUTPUT, FALSE);
      if (CR_ERROR (Status)) return Status;
      State->WlanManaged = FALSE;
    }
    if (State->XoManaged) {
      Status = Io->SetGpio (Io->Context, &Endpoint->XoGpio,
                            PCIE_GPIO_OUTPUT, FALSE);
      if (CR_ERROR (Status)) return Status;
      State->XoManaged = FALSE;
    }
    for (Index = Endpoint->SupplyCount; Index > 0; --Index) {
      if ((State->SupplyMask & LShiftU64 (1, Index - 1)) == 0) continue;
      Status = Io->SetSupply (Io->Context,
          &mPcieTarget->Supplies[Endpoint->SupplyOffset + Index - 1], FALSE);
      if (CR_ERROR (Status)) return Status;
      State->SupplyMask &= ~LShiftU64 (1, Index - 1);
    }
    DEBUG ((DEBUG_INFO, "PCIe: port %u WCN6855 endpoint power released\n",
            Controller->Domain));
    return CR_SUCCESS;
  }
  if (State->Ready) return CR_SUCCESS;
  if (State->SupplyMask != 0 || State->WlanManaged || State->XoManaged)
    return CR_BUSY;

  /* Linux requests WLAN_EN ASIS so an inherited high level does not briefly
     reset the device. UEFI owns the WLAN lifetime once this callback runs. */
  ZeroMem (&InitialWlan, sizeof (InitialWlan));
  InitialWlan.PinNumber = Endpoint->EnableGpio.Pin;
  Status = mPcieResources.Gpio->GetGpioConfig (mPcieResources.Gpio, &InitialWlan);
  if (CR_ERROR (Status)) return Status;
  Bt = Sw = 0xFF;
  if (mPcieResources.Gpio->ReadIoValue != NULL) {
    (VOID)mPcieResources.Gpio->ReadIoValue (
        mPcieResources.Gpio, Endpoint->BtEnablePin, &Bt);
    (VOID)mPcieResources.Gpio->ReadIoValue (
        mPcieResources.Gpio, Endpoint->SwControlPin, &Sw);
  }
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u WCN6855 power-on WLAN%u=%u mux=%u OE=%u BT%u=%u SWCTRL%u=%u\n",
          Controller->Domain, Endpoint->EnableGpio.Pin, InitialWlan.OutputValue,
          InitialWlan.FunctionSel, InitialWlan.OutputEnable,
          Endpoint->BtEnablePin, Bt, Endpoint->SwControlPin, Sw));
  /* Claim the WLAN lifetime before any rail can change. Even an inherited
     high WLAN must be stopped before withdrawing a partially acquired PMU. */
  State->WlanManaged = TRUE;
  Stage = "supplies";
  for (Index = 0; Index < Endpoint->SupplyCount; ++Index) {
    /* AcquireVreg can own a partial transaction even when it fails. */
    State->SupplyMask |= LShiftU64 (1, Index);
    Status = Io->SetSupply (Io->Context,
        &mPcieTarget->Supplies[Endpoint->SupplyOffset + Index], TRUE);
    if (CR_ERROR (Status)) goto Error;
  }
  Stage = "wlan-as-is";
  Status = Io->SetGpio (Io->Context, &Endpoint->EnableGpio, PCIE_GPIO_OUTPUT,
                        InitialWlan.OutputValue == GPIO_VALUE_HIGH);
  if (CR_ERROR (Status)) goto Error;
  if (InitialWlan.OutputValue != GPIO_VALUE_HIGH) {
    State->XoManaged = TRUE;
    Stage = "xo-low";
    Status = Io->SetGpio (Io->Context, &Endpoint->XoGpio, PCIE_GPIO_OUTPUT, FALSE);
    if (CR_ERROR (Status)) goto Error;
    /* Android CNSS2 guards BT-off -> WLAN-on by 100ms. We cannot know an
       inherited BT edge's timestamp, so use that bounded quiet interval.
       It also covers mainline's 5ms GPIO guard and 1ms XO settling delay. */
    Io->DelayUs (Io->Context, 100000);
    Stage = "xo-assert";
    Status = Io->SetGpio (Io->Context, &Endpoint->XoGpio, PCIE_GPIO_OUTPUT, TRUE);
    if (CR_ERROR (Status)) goto Error;
    Io->DelayUs (Io->Context, 100);
    Stage = "wlan-enable";
    Status = Io->SetGpio (Io->Context, &Endpoint->EnableGpio, PCIE_GPIO_OUTPUT, TRUE);
    if (CR_ERROR (Status)) goto Error;
    Io->DelayUs (Io->Context, 2000);
    Stage = "xo-release";
    Status = Io->SetGpio (Io->Context, &Endpoint->XoGpio, PCIE_GPIO_OUTPUT, FALSE);
    if (CR_ERROR (Status)) goto Error;
  }
  /* Linux WCN6855 target post-enable delay before PCIe enumerates. */
  Io->DelayUs (Io->Context, 50000);
  Sw = 0xFF;
  if (mPcieResources.Gpio->ReadIoValue != NULL) {
    (VOID)mPcieResources.Gpio->ReadIoValue (
        mPcieResources.Gpio, Endpoint->SwControlPin, &Sw);
  }
  State->Ready = TRUE;
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u WCN6855 endpoint power ready: WLAN_EN=1 supplies=%u SWCTRL=%u\n",
          Controller->Domain, Endpoint->SupplyCount, Sw));
  return CR_SUCCESS;
Error:
  DEBUG ((DEBUG_ERROR, "PCIe: port %u WCN6855 stage=%a failed: %r\n",
          Controller->Domain, Stage, Status));
  return Status;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetIommu (
  IN VOID                        *Context,
  IN CONST PcieTargetController  *Controller,
  IN BOOLEAN                      Enable
  )
{
  CONST PcieTargetIommuMap  *Map;
  EFI_STATUS                 Status;
  EFI_STATUS                 AttachStatus;
  EFI_STATUS                 BlockStatus;
  UINT16                     Index;
  UINTN                      ControllerIndex;

  (VOID)Context;
  if ((Controller == NULL) || (mPcieTarget == NULL) ||
      (mPcieResources.Smmu == NULL) ||
      (mPcieResources.Smmu->BlockSegment == NULL)) {
    return CR_UNSUPPORTED;
  }

  ControllerIndex = 0;
  while (ControllerIndex < mPcieTarget->ControllerCount &&
         &mPcieTarget->Controllers[ControllerIndex] != Controller) {
    ControllerIndex++;
  }
  if (ControllerIndex >= PCIE_MAX_CONTROLLERS ||
      ControllerIndex >= mPcieTarget->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }

  if (!Enable) {
    /* A failed attach may have rolled itself back before PcieLib's common
     * error path calls us again.  Do not turn an unrelated NOT_FOUND into a
     * successful revoke; only skip a revoke when this adapter owns no stream
     * for the segment. */
    if (!mPcieResources.IommuAttached[ControllerIndex]) {
      return CR_SUCCESS;
    }
    Status = mPcieResources.Smmu->BlockSegment (
                                  mPcieResources.Smmu,
                                  Controller->Domain
                                  );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mPcieResources.IommuAttached[ControllerIndex] = FALSE;
    return EFI_SUCCESS;
  }
  if (mPcieResources.Smmu->Attach == NULL) {
    return CR_UNSUPPORTED;
  }

  if (mPcieResources.IommuAttached[ControllerIndex]) {
    return CR_SUCCESS;
  }

  /* Set ownership before the first Attach call.  SmmuCrDxe may have written
   * context-bank state before reporting a device error; retaining the bit
   * makes the common cleanup path retry BlockSegment instead of declaring the
   * segment clean prematurely. */
  mPcieResources.IommuAttached[ControllerIndex] = TRUE;

  for (Index = 0; Index < Controller->IommuMapCount; Index++) {
    Map = &mPcieTarget->IommuMaps[Controller->IommuMapOffset + Index];
    AttachStatus = mPcieResources.Smmu->Attach (
                                      mPcieResources.Smmu,
                                      Controller->Domain,
                                      Map->Bdf,
                                      (UINT16)Map->StreamId
                                      );
    if (EFI_ERROR (AttachStatus)) {
      BlockStatus = mPcieResources.Smmu->BlockSegment (
                                      mPcieResources.Smmu,
                                      Controller->Domain
                                      );
      /*
       * BlockSegment() returns EFI_NOT_FOUND when Attach() rejected the
       * stream before a domain was committed.  That is an expected cleanup
       * result, rather than the cause of the PCIe failure.  Clear the local
       * ownership bit in both that case and after a successful rollback, and
       * always preserve AttachStatus for the caller.  Returning BlockStatus
       * here used to turn a useful CR_BUSY/ACCESS_DENIED diagnosis into the
       * misleading EFI_NOT_FOUND (0xE) seen by PciHostBridgeDxe.
       */
      if (!EFI_ERROR (BlockStatus) || (BlockStatus == EFI_NOT_FOUND)) {
        mPcieResources.IommuAttached[ControllerIndex] = FALSE;
      }
      return AttachStatus;
    }
  }
  return CR_SUCCESS;
}

STATIC
CR_STATUS
EFIAPI
CranePcieSetInterconnect (
  IN VOID                        *Context,
  IN CONST PcieTargetController  *Controller,
  IN BOOLEAN                      Enable
  )
{
  EFI_INTERCONNECT_CR_PROTOCOL  *Interconnect;
  CONST PcieTargetInterconnect   *Target;
  INTERCONNECT_PATH_HANDLE        MemPath;
  INTERCONNECT_PATH_HANDLE        CpuPath;
  EFI_STATUS                      EfiStatus;
  EFI_STATUS                      ReleaseStatus;
  UINTN                           Index;

  (VOID)Context;
  if (Controller == NULL || mPcieTarget == NULL ||
      (mPcieResources.Interconnect == NULL) ||
      (mPcieResources.Interconnect->AcquirePath == NULL) ||
      (mPcieResources.Interconnect->SetBandwidth == NULL) ||
      (mPcieResources.Interconnect->ReleasePath == NULL)) {
    return CR_UNSUPPORTED;
  }

  Index = 0;
  while (Index < mPcieTarget->ControllerCount &&
         &mPcieTarget->Controllers[Index] != Controller) {
    Index++;
  }
  if (Index >= PCIE_MAX_CONTROLLERS ||
      Index >= mPcieTarget->ControllerCount) {
    return CR_INVALID_PARAMETER;
  }
  Target = &Controller->Interconnect;
  if (Target->Provider == NULL || Target->MemSource == 0 ||
      Target->MemDestination == 0 || Target->CpuSource == 0 ||
      Target->CpuDestination == 0) {
    return CR_UNSUPPORTED;
  }
  Interconnect = mPcieResources.Interconnect;

  if (!Enable) {
    CR_STATUS FirstError = CR_SUCCESS;
    mPcieResources.InterconnectReady[Index] = FALSE;
    if (mPcieResources.CpuPaths[Index] != 0) {
      EfiStatus = Interconnect->ReleasePath(
          Interconnect, mPcieResources.CpuPaths[Index]);
      if (EFI_ERROR(EfiStatus)) {
        FirstError = CR_DEVICE_ERROR;
      } else {
        mPcieResources.CpuPaths[Index] = 0;
      }
    }
    if (mPcieResources.MemPaths[Index] != 0) {
      EfiStatus = Interconnect->ReleasePath(
          Interconnect, mPcieResources.MemPaths[Index]);
      if (EFI_ERROR(EfiStatus)) {
        if (!CR_ERROR(FirstError)) {
          FirstError = CR_DEVICE_ERROR;
        }
      } else {
        mPcieResources.MemPaths[Index] = 0;
      }
    }
    return FirstError;
  }

  if (mPcieResources.MemPaths[Index] != 0 ||
      mPcieResources.CpuPaths[Index] != 0) {
    /* Only a complete pair is ready.  A failed rollback can retain one
     * handle; the caller must finish teardown before enabling again. */
    return mPcieResources.InterconnectReady[Index] ? CR_SUCCESS : CR_BUSY;
  }

  MemPath = 0;
  EfiStatus = Interconnect->AcquirePath(
      Interconnect, Target->Provider, Target->MemSource,
      Target->MemDestination, &MemPath);
  if (EFI_ERROR(EfiStatus) || MemPath == 0) {
    return CR_DEVICE_ERROR;
  }
  EfiStatus = Interconnect->SetBandwidth(
      Interconnect, MemPath, Target->MemAverage, Target->MemPeak);
  if (EFI_ERROR(EfiStatus)) {
    ReleaseStatus = Interconnect->ReleasePath(Interconnect, MemPath);
    if (EFI_ERROR(ReleaseStatus)) {
      mPcieResources.MemPaths[Index] = MemPath;
    }
    return CR_DEVICE_ERROR;
  }

  CpuPath = 0;
  EfiStatus = Interconnect->AcquirePath(
      Interconnect, Target->Provider, Target->CpuSource,
      Target->CpuDestination, &CpuPath);
  if (EFI_ERROR(EfiStatus) || CpuPath == 0) {
    ReleaseStatus = Interconnect->ReleasePath(Interconnect, MemPath);
    if (EFI_ERROR(ReleaseStatus)) {
      mPcieResources.MemPaths[Index] = MemPath;
    }
    return CR_DEVICE_ERROR;
  }
  EfiStatus = Interconnect->SetBandwidth(
      Interconnect, CpuPath, Target->CpuAverage, Target->CpuPeak);
  if (EFI_ERROR(EfiStatus)) {
    ReleaseStatus = Interconnect->ReleasePath(Interconnect, CpuPath);
    if (EFI_ERROR(ReleaseStatus)) {
      mPcieResources.CpuPaths[Index] = CpuPath;
    }
    ReleaseStatus = Interconnect->ReleasePath(Interconnect, MemPath);
    if (EFI_ERROR(ReleaseStatus)) {
      mPcieResources.MemPaths[Index] = MemPath;
    }
    return CR_DEVICE_ERROR;
  }

  mPcieResources.MemPaths[Index] = MemPath;
  mPcieResources.CpuPaths[Index] = CpuPath;
  mPcieResources.InterconnectReady[Index] = TRUE;
  return CR_SUCCESS;
}

STATIC BOOLEAN
CranePcieDiagnosticRangeValid (UINT16 Offset, UINT16 Count, UINT16 Limit)
{
  return Count <= PCIE_MAX_RESOURCES && Offset <= Limit && Count <= Limit - Offset;
}

#include "PciClockDiagnostics.h"

/** Cached acquisition state only: this is not a measurement of slot voltage. */
STATIC VOID
CranePcieDiagnoseSupply (CONST PcieTargetController *Controller,
                       CONST PcieTargetSupply *Supply, CONST CHAR8 *Source,
                       BOOLEAN StepHeld)
{
  CRANE_PCIE_RESOURCE_REF *Ref =
    FindResourceRef (mPcieResources.SupplyRefs, Supply->Id, FALSE);
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u supply %a source=%a controller=%a id=%a requested=%u mV/mode %u cached=%u mV/mode %u adapter-refs=%u rpmh-owned=%u cached-ready=%u step-held=%u policy=%a\n",
          Controller->Domain, Supply->Name, Source, Supply->Controller, Supply->Id,
          Supply->VoltageMv, Supply->Mode,
          Ref == NULL ? 0 : Ref->VoltageMv, Ref == NULL ? 0 : Ref->Mode,
          Ref == NULL ? 0 : Ref->Count, Ref != NULL && Ref->Owned,
          Ref != NULL && Ref->Ready, StepHeld,
          AsciiStrCmp (Supply->Controller, "rpmh-enable") == 0
            ? "retained-enable-no-voltage-readback" : "cached-acquired-vote"));
  (VOID)Ref;
}

STATIC VOID
CranePcieDiagnoseGpio (CONST PcieTargetController *Controller,
                     CONST PcieTargetGpio *Gpio)
{
  GpioConfigParams Config;
  EFI_STATUS Status;
  ZeroMem (&Config, sizeof (Config));
  Config.PinNumber = Gpio->Pin;
  Status = mPcieResources.Gpio == NULL ||
           mPcieResources.Gpio->GetGpioConfig == NULL
             ? EFI_UNSUPPORTED
             : mPcieResources.Gpio->GetGpioConfig (mPcieResources.Gpio, &Config);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u GPIO %u (%a) snapshot readback failed: %r\n",
            Controller->Domain, Gpio->Pin, Gpio->Role, Status));
    return;
  }
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u GPIO %u (%a) snapshot mux=%u OE=%u pull=%u drive=%u mA %a=%u\n",
          Controller->Domain, Gpio->Pin, Gpio->Role, Config.FunctionSel,
          Config.OutputEnable, Config.Pull, 2U * ((UINT32)Config.DriveStrength + 1U),
          Config.OutputEnable ? "output-latch" : "input", Config.OutputValue));
}

STATIC VOID
CranePcieDiagnoseClocks (CONST PcieTargetController *Controller,
                       UINT16 Offset, UINT16 Count, CONST CHAR8 *Source)
{
  if (!CranePcieDiagnosticRangeValid (Offset, Count, mPcieTarget->ClockCount)) return;
  for (UINT16 Index = 0; Index < Count; ++Index) {
    CONST PcieTargetClock *Clock = &mPcieTarget->Clocks[Offset + Index];
    CRANE_PCIE_RESOURCE_REF *Ref =
      FindResourceRef (mPcieResources.ClockRefs, Clock->Id, FALSE);
    DEBUG ((DEBUG_WARN,
            "PCIe: port %u clock %a source=%a controller=%a id=%a provider=%u requested-hz=%u cached-refs=%u cached-owned=%u cached-ready=%u (lease state only)\n",
            Controller->Domain, Clock->Name, Source, Clock->Controller, Clock->Id,
            Clock->Provider, Clock->RateHz, Ref == NULL ? 0 : Ref->Count,
            Ref != NULL && Ref->Owned, Ref != NULL && Ref->Ready));
    (VOID)Ref;
  }
}

STATIC CONST PcieTargetRegion *
CranePcieDiagnosticRegion (CONST PcieTargetController *Controller, CONST CHAR8 *Name)
{
  if (!CranePcieDiagnosticRangeValid (Controller->RegionOffset,
      Controller->RegionCount, mPcieTarget->RegionCount)) return NULL;
  for (UINT16 Index = 0; Index < Controller->RegionCount; ++Index) {
    CONST PcieTargetRegion *Region =
      &mPcieTarget->Regions[Controller->RegionOffset + Index];
    if (AsciiStrCmp (Region->Name, Name) == 0) return Region;
  }
  return NULL;
}

STATIC VOID
CranePcieDiagnoseAuxClockFrequency (
  CONST PcieIoOps *Io, CONST PcieTargetController *Controller,
  CONST PcieTargetRegion *Dbi)
{
  CONST UINT32 Offset = 0xB40;
  UINT32 RateHz = 0;

  /* Public Android SM8250 pci-msm.c (cad09e061ef6) defines the core's
   * AUX_CLK_FREQ_REG at DM_CORE+0xb40 and writes 0x14 for 19.2 MHz.  Current
   * Linux ops_1_9_0 has no such write.  Observe the inherited value before
   * PERST release and at timeout; do not infer a measured rate or change it.
   * Other compatibles need their own register qualification first. */
  if (Io == NULL || Io->Read32 == NULL || Controller == NULL ||
      Controller->Compatible == NULL ||
      AsciiStrCmp (Controller->Compatible, "qcom,pcie-sm8250") != 0)
    return;
  if (Dbi == NULL || (Dbi->Base & 3U) != 0 ||
      Dbi->Size < Offset + sizeof (UINT32) ||
      Dbi->Base > MAX_UINT64 - Offset - (sizeof (UINT32) - 1U)) {
    DEBUG ((DEBUG_WARN, "PCIe: port %u AUX_CLK_FREQ unavailable: invalid DBI region\n",
            Controller->Domain));
    return;
  }
  if (mPcieTarget != NULL && mPcieTarget->Clocks != NULL &&
      CranePcieDiagnosticRangeValid (Controller->ClockOffset,
        Controller->ClockCount, mPcieTarget->ClockCount)) {
    for (UINT16 Index = 0; Index < Controller->ClockCount; ++Index) {
      CONST PcieTargetClock *Clock =
        &mPcieTarget->Clocks[Controller->ClockOffset + Index];
      if (Clock->Name != NULL && AsciiStrCmp (Clock->Name, "aux") == 0) {
        RateHz = Clock->RateHz;
        break;
      }
    }
  }
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u DBI AUX_CLK_FREQ +0x%x raw=0x%x target-aux-hz=%u (configuration only)\n",
          Controller->Domain, Offset,
          Io->Read32 (Io->Context, Dbi->Base + Offset), RateHz));
}

STATIC VOID
CranePcieDiagnoseRegisters (CONST PcieIoOps *Io,
                          CONST PcieTargetController *Controller,
                          CONST PcieTargetPhy *Phy)
{
  CONST PcieTargetRegion *Parf = CranePcieDiagnosticRegion (Controller, "parf");
  CONST PcieTargetRegion *Dbi = CranePcieDiagnosticRegion (Controller, "dbi");
  UINT8 Capability;
  UINT16 Guard, LinkStatus;
  UINT64 Pcs;
  if (Io->Read32 == NULL) return;
  CranePcieDiagnoseAuxClockFrequency (Io, Controller, Dbi);
  if (Parf != NULL && Parf->Size >= 0x2B4) {
    DEBUG ((DEBUG_WARN,
            "PCIe: port %u PARF base=0x%Lx sys=0x%x phy=0x%x LTSSM=0x%x CLKREQ=0x%x\n",
            Controller->Domain, Parf->Base,
            Io->Read32 (Io->Context, Parf->Base),
            Io->Read32 (Io->Context, Parf->Base + 0x40),
            Io->Read32 (Io->Context, Parf->Base + 0x1B0),
            Io->Read32 (Io->Context, Parf->Base + 0x2B0)));
  }
  if (Phy != NULL && Phy->BlockOffsets.Pcs <= Phy->Size &&
      Phy->Status <= Phy->Size - Phy->BlockOffsets.Pcs &&
      sizeof (UINT32) <= Phy->Size - Phy->BlockOffsets.Pcs - Phy->Status) {
    Pcs = Phy->Base + Phy->BlockOffsets.Pcs;
    DEBUG ((DEBUG_WARN,
            "PCIe: port %u PHY base=0x%Lx PCS=0x%Lx type=%a lanes=%u status=0x%x ready-mask=0x%x\n",
            Controller->Domain, Phy->Base, Pcs, Phy->Compatible, Phy->Lanes,
            Io->Read32 (Io->Context, Pcs + Phy->Status), Phy->StatusMask));
  }
  if (Dbi == NULL || Dbi->Size < 0x38) return;
  Capability = (UINT8)Io->Read32 (Io->Context, Dbi->Base + 0x34) & 0xFC;
  for (Guard = 0; Capability >= 0x40 && Guard < 48; ++Guard) {
    UINT32 Header;
    if (Dbi->Size < (UINT32)Capability + 4) break;
    Header = Io->Read32 (Io->Context, Dbi->Base + Capability);
    if ((Header & 0xFF) == 0x10 && Dbi->Size >= (UINT32)Capability + 0x14) {
      LinkStatus = (UINT16)(Io->Read32 (Io->Context,
                          Dbi->Base + Capability + 0x10) >> 16);
      DEBUG ((DEBUG_WARN,
              "PCIe: port %u DBI base=0x%Lx cap=0x%x configured-speed=%u configured-lanes=%u link-status=0x%x DLL-active=%u negotiated-speed=%u negotiated-lanes=%u\n",
              Controller->Domain, Dbi->Base, Capability, Controller->MaxLinkSpeed,
              Controller->Lanes, LinkStatus, (LinkStatus >> 13) & 1U,
              LinkStatus & 15U, (LinkStatus >> 4) & 63U));
      break;
    }
    Capability = (UINT8)(Header >> 8) & 0xFC;
  }
}

/** Bounded DEBUG snapshots; GPIO/register reads and cached leases do not
    establish connector routing, physical reference clocks or slot voltage. */
STATIC
VOID
CranePcieDiagnoseLink (
  IN CONST PcieIoOps             *Io,
  IN CONST PcieTargetController  *Controller
  )
{
  UINT16                         Index;
  CONST PcieTargetPhy            *Phy;
  CONST PcieTargetPowerSequence *Sequence;
  CONST CRANE_PCIE_ENDPOINT_STATE *Endpoint;
  CONST PciePortRuntime          *Port = NULL;

  if ((Io == NULL) || (Controller == NULL) || (mPcieTarget == NULL) ||
      Controller->Domain >= PCIE_MAX_CONTROLLERS ||
      !DebugPrintLevelEnabled (DEBUG_WARN) ||
      !CranePcieDiagnosticRangeValid (Controller->SupplyOffset,
        Controller->SupplyCount, mPcieTarget->SupplyCount)) {
    return;
  }
  if (mPcieContext != NULL && mPcieContext->Target == mPcieTarget &&
      mPcieTarget->ControllerCount <= PCIE_MAX_CONTROLLERS) {
    for (Index = 0; Index < mPcieTarget->ControllerCount; ++Index) {
      if (mPcieContext->Ports[Index].Target == Controller)
        Port = &mPcieContext->Ports[Index];
    }
  }
  Endpoint = &mPcieResources.Endpoints[Controller->Domain];
  DEBUG ((DEBUG_WARN,
          "PCIe: port %u snapshot point=%a controller=%a endpoint-ready=%u step-mask=0x%Lx endpoint-supply-mask=0x%Lx\n",
          Controller->Domain,
          Port == NULL ? "snapshot" :
            (Port->State == PCIE_PORT_INITIALIZING ? "pre-perst-release" : "timeout"),
          Controller->Compatible, Endpoint->Ready, Endpoint->StepMask, Endpoint->SupplyMask));
  if (Port != NULL) {
    DEBUG ((DEBUG_WARN,
            "PCIe: port %u snapshot timing-valid=0x%x assert=%Lu release=%Lu training=%Lu now=%Lu us\n",
            Controller->Domain, Port->TimingValidMask, Port->PerstAssertTimeUs,
            Port->PerstReleaseTimeUs, Port->TrainingStartTimeUs,
            Io->GetTimeUs == NULL ? 0 : Io->GetTimeUs (Io->Context)));
  }
  for (Index = 0; Index < Controller->SupplyCount; ++Index) {
    CranePcieDiagnoseSupply (Controller,
      &mPcieTarget->Supplies[Controller->SupplyOffset + Index], "controller", FALSE);
  }
  if (CranePcieDiagnosticRangeValid (Controller->GpioOffset,
      Controller->GpioCount, mPcieTarget->GpioCount)) {
    for (Index = 0; Index < Controller->GpioCount; ++Index)
      CranePcieDiagnoseGpio (Controller, &mPcieTarget->Gpios[Controller->GpioOffset + Index]);
  }
  Sequence = Controller->PowerSequence;
  if (Sequence != NULL) {
    if (Sequence->Steps == NULL || Sequence->Count == 0 || Sequence->Count > 64) {
      DEBUG ((DEBUG_WARN, "PCIe: port %u invalid board sequence\n", Controller->Domain));
    } else for (Index = 0; Index < Sequence->Count; ++Index) {
      CONST PcieTargetPowerStep *Step = &Sequence->Steps[Index];
      BOOLEAN Held = (Endpoint->StepMask & (1ULL << Index)) != 0;
      DEBUG ((DEBUG_WARN, "PCIe: port %u board step %u kind=%u resource=%u held=%u delay=%u us\n",
              Controller->Domain, Index, Step->Kind, Step->ResourceIndex, Held, Step->DelayUs));
      if (Step->Kind == PCIE_POWER_SUPPLY && Step->ResourceIndex < mPcieTarget->SupplyCount)
        CranePcieDiagnoseSupply (Controller, &mPcieTarget->Supplies[Step->ResourceIndex], "board-step", Held);
      else if ((Step->Kind == PCIE_POWER_GPIO || Step->Kind == PCIE_POWER_PINCTRL) &&
               Step->ResourceIndex < mPcieTarget->GpioCount)
        CranePcieDiagnoseGpio (Controller, &mPcieTarget->Gpios[Step->ResourceIndex]);
      else if (Step->Kind != PCIE_POWER_DELAY)
        DEBUG ((DEBUG_WARN, "PCIe: port %u board step %u invalid resource\n", Controller->Domain, Index));
    }
  }
  if (Controller->EndpointPower != NULL) {
    CONST PcieTargetEndpointPower *Power = Controller->EndpointPower;
    if (CranePcieDiagnosticRangeValid (Power->SupplyOffset, Power->SupplyCount, mPcieTarget->SupplyCount)) {
      for (Index = 0; Index < Power->SupplyCount; ++Index)
        CranePcieDiagnoseSupply (Controller, &mPcieTarget->Supplies[Power->SupplyOffset + Index],
                                "endpoint", (Endpoint->SupplyMask & (1ULL << Index)) != 0);
    }
    CranePcieDiagnoseGpio (Controller, &Power->EnableGpio);
    CranePcieDiagnoseGpio (Controller, &Power->XoGpio);
  }
  Phy = Controller->PhyIndex < mPcieTarget->PhyCount
          ? &mPcieTarget->Phys[Controller->PhyIndex] : NULL;
  CranePcieDiagnoseClocks (Controller, Controller->ClockOffset, Controller->ClockCount, "controller");
  if (Phy != NULL) CranePcieDiagnoseClocks (Controller, Phy->ClockOffset, Phy->ClockCount, "PHY");
  CranePcieDiagnoseClockHardware (Io, Controller, Phy);
  CranePcieDiagnoseRegisters (Io, Controller, Phy);
  if (mPciePlatformDiagnoseLink != NULL) {
    mPciePlatformDiagnoseLink (Io, Controller);
  }
}

STATIC
EFI_STATUS
EnsurePcieBootFloor (VOID)
{
  EFI_RPMH_CR_PROTOCOL *Rpmh = mPcieResources.Rpmh;
  EFI_STATUS Status;
  if (Rpmh == NULL || Rpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION ||
      Rpmh->EnsureBootFloor == NULL) return EFI_UNSUPPORTED;
  Status = Rpmh->EnsureBootFloor (Rpmh);
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_NOT_READY;
}

STATIC
EFI_STATUS
InitializeResourceAdapters (
  VOID
  )
{
  EFI_STATUS  Status;

  /* Resource reference counts belong to the platform drivers, not to one
   * initialization attempt.  Do not clear them when a failed cold start is
   * retried after a transient protocol or hardware error. */
  if (!mPcieAdaptersInitialized) {
    ZeroMem (&mPcieResources, sizeof (mPcieResources));
    Status = gBS->LocateProtocol (
                    &gEfiRpmhCrProtocolGuid,
                    NULL,
                    (VOID **)&mPcieResources.Rpmh
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->LocateProtocol (
                    &gEfiGpioCrProtocolGuid,
                    NULL,
                    (VOID **)&mPcieResources.Gpio
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->LocateProtocol (
                    &gEfiClockCrProtocolGuid,
                    NULL,
                    (VOID **)&mPcieResources.ClockProtocol
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->LocateProtocol (
                    &gEfiInterconnectCrProtocolGuid,
                    NULL,
                    (VOID **)&mPcieResources.Interconnect
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    Status = gBS->LocateProtocol (
                    &gEfiSmmuCrProtocolGuid,
                    NULL,
                    (VOID **)&mPcieResources.Smmu
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mPcieAdaptersInitialized = TRUE;
  }
  /* Protocol publication can precede an acknowledged ARC baseline. Retry
     that retained floor before any PCIe clock, rail, GPIO or MMIO access. */
  Status = EnsurePcieBootFloor ();
  if (EFI_ERROR (Status)) return Status;
  mPcieIo.SetClock        = CranePcieSetClock;
  mPcieIo.SetClockParent  = CranePcieSetClockParent;
  mPcieIo.SetReset        = CranePcieSetReset;
  mPcieIo.SetPowerDomain  = CranePcieSetPowerDomain;
  mPcieIo.SetSupply       = CranePcieSetSupply;
  mPcieIo.SetInterconnect = CranePcieSetInterconnect;
  mPcieIo.SetIommu        = CranePcieSetIommu;
  mPcieIo.SetGpio         = CranePcieSetGpio;
  mPcieIo.SetEndpointPower = CranePcieSetEndpointPower;
  if (mPcieIo.DiagnoseLink != CranePcieDiagnoseLink) {
    mPciePlatformDiagnoseLink = mPcieIo.DiagnoseLink;
    mPcieIo.DiagnoseLink = CranePcieDiagnoseLink;
  }
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
ReleasePciePortMask (
  IN UINT32  PortMask
  )
{
  EFI_STATUS  FirstError;
  CR_STATUS   Status;
  UINT16      Index;

  if ((mPcieContext == NULL) || !mPcieContext->Initialized ||
      (mPcieTarget == NULL)) {
    return EFI_SUCCESS;
  }

  FirstError = EFI_SUCCESS;
  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if ((PortMask & (UINT32)BIT (Index)) == 0) {
      continue;
    }
    Status = PcieShutdownPort (mPcieContext, Index);
    if (CR_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }
  mPcieInitializedMask = mPcieContext->InitializedMask;
  return FirstError;
}

/**
  Preserve ownership for a port whose initialization rollback stopped early.

  PcieInitializePort() deliberately leaves its runtime ownership flags set
  when ReleasePortResources() cannot revoke a resource.  The library's
  normal success path records ResourceMask, but the failed initialization
  path predates that bookkeeping.  Recover the bit here through the public
  PcieGetPortInfo() contract so the host shutdown/retry path can make another
  cleanup attempt instead of silently dropping the live resource vote.
**/
STATIC
VOID
RecordRetainedPcieResources (
  IN UINT32  PortMask
  )
{
  UINT16                    Index;
  CONST PciePortRuntime    *Port;

  if ((mPcieContext == NULL) || !mPcieContext->Initialized ||
      (mPcieTarget == NULL)) {
    return;
  }

  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if ((PortMask & (UINT32)BIT (Index)) == 0) {
      continue;
    }
    Port = NULL;
    if (CR_ERROR (PcieGetPortInfo (mPcieContext, Index, &Port)) ||
        (Port == NULL)) {
      continue;
    }
    if ((Port->ClockOwnedMask != 0) || (Port->SupplyOwnedMask != 0) ||
        Port->GpiosConfigured || Port->SuppliesEnabled ||
        Port->PowerEnabled || Port->ControllerClocksEnabled ||
        Port->ControllerResetsManaged || Port->PhyClocksEnabled ||
        Port->PhyResetsManaged || Port->InterconnectEnabled ||
        Port->IommuAttached || Port->AuxClockParentManaged ||
        Port->EndpointPowerManaged) {
      mPcieContext->ResourceMask |= (UINT32)BIT (Index);
    }
  }
}

STATIC
EFI_STATUS
ReleaseRetainedPcieResources (
  VOID
  )
{
  if ((mPcieContext == NULL) || !mPcieContext->Initialized) {
    return EFI_SUCCESS;
  }
  return ReleasePciePortMask (
           mPcieContext->ResourceMask | mPcieContext->InitializedMask);
}

STATIC
VOID
SetEmptyAperture (
  OUT PCI_ROOT_BRIDGE_APERTURE  *Aperture
  )
{
  Aperture->Base        = 1;
  Aperture->Limit       = 0;
  Aperture->Translation = 0;
}

STATIC
BOOLEAN
RangeLimit (
  IN  CONST PcieTargetRange  *Range,
  OUT UINT64                 *Limit
  )
{
  if ((Range == NULL) || (Limit == NULL) || (Range->Size == 0) ||
      (Range->PciBase > MAX_UINT64 - (Range->Size - 1U)) ||
      (Range->CpuBase > MAX_UINT64 - (Range->Size - 1U))) {
    return FALSE;
  }

  *Limit = Range->PciBase + Range->Size - 1U;
  return TRUE;
}

STATIC
BOOLEAN
FillAperture (
  OUT PCI_ROOT_BRIDGE_APERTURE  *Aperture,
  IN  CONST PcieTargetRange      *Range
  )
{
  UINT64 Limit;

  if (!RangeLimit (Range, &Limit)) {
    return FALSE;
  }

  Aperture->Base        = Range->PciBase;
  Aperture->Limit       = Limit;
  Aperture->Translation = Range->PciBase - Range->CpuBase;
  return TRUE;
}

STATIC
EFI_DEVICE_PATH_PROTOCOL *
CreateRootBridgeDevicePath (
  IN UINT32  Uid
  )
{
  typedef struct {
    ACPI_HID_DEVICE_PATH      Acpi;
    EFI_DEVICE_PATH_PROTOCOL  End;
  } CRANE_ROOT_DEVICE_PATH;
  CRANE_ROOT_DEVICE_PATH  *Path;

  Path = AllocateZeroPool (sizeof (*Path));
  if (Path == NULL) {
    return NULL;
  }

  Path->Acpi.Header.Type    = ACPI_DEVICE_PATH;
  Path->Acpi.Header.SubType = ACPI_DP;
  SetDevicePathNodeLength (&Path->Acpi.Header, sizeof (Path->Acpi));
  Path->Acpi.HID = EISA_PNP_ID (0x0A03); // PNP0A03, PCI root bridge.
  Path->Acpi.UID = Uid;

  Path->End.Type    = END_DEVICE_PATH_TYPE;
  Path->End.SubType = END_ENTIRE_DEVICE_PATH_SUBTYPE;
  SetDevicePathNodeLength (&Path->End, sizeof (Path->End));
  return (EFI_DEVICE_PATH_PROTOCOL *)Path;
}

STATIC
EFI_STATUS
BuildRootBridge (
  IN  UINT16             ControllerIndex,
  OUT PCI_ROOT_BRIDGE   *Bridge
  )
{
  CONST PcieTargetController  *Controller;
  UINT16                      Index;
  BOOLEAN                     HasMem64;
  CONST PcieTargetRange       *Range;

  if ((mPcieTarget == NULL) || (Bridge == NULL) ||
      (ControllerIndex >= mPcieTarget->ControllerCount)) {
    return EFI_INVALID_PARAMETER;
  }

  Controller = &mPcieTarget->Controllers[ControllerIndex];
  ZeroMem (Bridge, sizeof (*Bridge));
  SetEmptyAperture (&Bridge->Bus);
  SetEmptyAperture (&Bridge->Io);
  SetEmptyAperture (&Bridge->Mem);
  SetEmptyAperture (&Bridge->MemAbove4G);
  SetEmptyAperture (&Bridge->PMem);
  SetEmptyAperture (&Bridge->PMemAbove4G);

  Bridge->Segment              = Controller->Domain;
  Bridge->DmaAbove4G           = Controller->DmaAbove4G;
  Bridge->NoExtendedConfigSpace = FALSE;
  /* Cold initialization configures RC windows, not endpoint BARs. MU must
     expose resource allocation so PciBusDxe can enumerate and assign them.
     The target has common outbound memory windows for both BAR classes. */
  Bridge->ResourceAssigned     = FALSE;
  Bridge->AllocationAttributes = EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM;
  Bridge->Supports = EFI_PCI_ATTRIBUTE_ISA_IO |
                     EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO |
                     EFI_PCI_ATTRIBUTE_VGA_MEMORY |
                     EFI_PCI_ATTRIBUTE_VGA_IO |
                     EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO |
                     EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO |
                     EFI_PCI_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                     EFI_PCI_ATTRIBUTE_MEMORY_CACHED |
                     (Controller->DmaAbove4G ?
                      EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE : 0);
  Bridge->Attributes = 0;
  Bridge->Bus.Base    = Controller->BusStart;
  Bridge->Bus.Limit   = Controller->BusEnd;

  HasMem64 = FALSE;
  for (Index = 0; Index < Controller->RangeCount; Index++) {
    Range = &mPcieTarget->Ranges[Controller->RangeOffset + Index];
    switch (Range->Type) {
      case PCIE_RANGE_IO:
        if (!FillAperture (&Bridge->Io, Range)) {
          return EFI_INVALID_PARAMETER;
        }
        break;
      case PCIE_RANGE_MEM32:
        if (!FillAperture (&Bridge->Mem, Range) ||
            (Bridge->Mem.Limit >= SIZE_4GB)) {
          return EFI_INVALID_PARAMETER;
        }
        break;
      case PCIE_RANGE_MEM64:
        if (!FillAperture (&Bridge->MemAbove4G, Range) ||
            (Bridge->MemAbove4G.Base < SIZE_4GB)) {
          return EFI_INVALID_PARAMETER;
        }
        HasMem64 = TRUE;
        break;
      default:
        return EFI_INVALID_PARAMETER;
    }
  }

  if (HasMem64) {
    Bridge->AllocationAttributes |= EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  }
  Bridge->DevicePath = CreateRootBridgeDevicePath (Controller->Domain);
  if (Bridge->DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  return EFI_SUCCESS;
}

/**
  Initialize the platform PCIe context once, without installing a private
  protocol.  The generic MU PciHostBridgeDxe remains the sole producer of
  EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL.
**/
EFI_STATUS
EFIAPI
CranePcieHostInitialize (
  VOID
  )
{
  CR_STATUS  Status;
  EFI_STATUS CleanupStatus;
  UINT32     PortMask;
  UINT32     InitializedMask;
  UINT32     FailedResourceMask;
  UINT16     Index;

  /* A successful host is stable for the rest of the boot.  Failed attempts
   * remain retryable because protocol dispatch and resource teardown can be
   * transient. */
  if (mPcieInitAttempted) {
    return mPcieInitStatus;
  }

  mPcieTarget = CrDalGetPcieContext ();
  if (mPcieTarget == NULL || mPcieTarget->Controllers == NULL ||
      mPcieTarget->ControllerCount == 0 ||
      mPcieTarget->ControllerCount > 32) {
    mPcieInitStatus = EFI_UNSUPPORTED;
    return mPcieInitStatus;
  }

  /* A previous attempt may have fenced a port but failed to revoke one of
   * its resources.  Retry that teardown before touching the controller again;
   * re-running cold init with a live old vote would double-enable hardware. */
  if (mPcieContext != NULL && mPcieContext->Initialized &&
      ((mPcieContext->ResourceMask | mPcieContext->InitializedMask) != 0)) {
    Status = ReleaseRetainedPcieResources ();
    if (CR_ERROR (Status) ||
        ((mPcieContext->ResourceMask | mPcieContext->InitializedMask) != 0)) {
      mPcieInitStatus = EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR;
      return mPcieInitStatus;
    }
  }

  ZeroMem (&mPcieIo, sizeof (mPcieIo));
  Status = CrDalGetPcieIo (&mPcieIo);
  if (EFI_ERROR (Status)) {
    mPcieInitStatus = Status;
    return mPcieInitStatus;
  }
  /* CrDAL supplies MMIO/delay primitives; the adapter then
   * replaces its resource callbacks with the owning EFI drivers. */
  Status = InitializeResourceAdapters ();
  if (EFI_ERROR (Status)) {
    mPcieInitStatus = Status;
    return mPcieInitStatus;
  }
  Status = PcieLibInit (&mPcieContext, mPcieTarget, &mPcieIo);
  if (CR_ERROR (Status)) {
    mPcieInitStatus = Status;
    return mPcieInitStatus;
  }

  PortMask = 0;
  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if (mPcieTarget->Controllers[Index].Enabled) {
      PortMask |= (UINT32)(1U << Index);
    }
  }
  if (PortMask == 0) {
    mPcieInitStatus = EFI_NOT_FOUND;
    return mPcieInitStatus;
  }

  InitializedMask = 0;
  Status = PcieInitializeMask (mPcieContext, PortMask, &InitializedMask);
  /* A failed port can retain ownership when its rollback hits a clock,
   * interconnect, or other resource timeout.  PcieInitializeMask reports
   * only successfully initialized ports, so recover retained ownership from
   * the per-port state before calculating the retry mask. */
  RecordRetainedPcieResources (PortMask & ~InitializedMask);
  mPcieInitializedMask = InitializedMask;

  /* PcieInitializeMask can return one ready port while another port retains
   * resources because its error-path revoke failed.  Retry only those failed
   * ports here; shutting down the ready ports would make a partial host
   * usable state disappear and would also turn a transient cleanup failure
   * into a duplicate cold-init sequence. */
  FailedResourceMask = mPcieContext->ResourceMask & ~InitializedMask;
  if (FailedResourceMask != 0) {
    CleanupStatus = ReleasePciePortMask (FailedResourceMask);
    if (EFI_ERROR (CleanupStatus)) {
      DEBUG ((
        DEBUG_ERROR,
        "Crane PCIe: failed-port resource cleanup failed: %r\n",
        CleanupStatus
        ));
      if (InitializedMask == 0 && !CR_ERROR (Status)) {
        Status = (CR_STATUS)CleanupStatus;
      }
    }
  }

  if (InitializedMask != 0) {
    mPcieInitStatus    = EFI_SUCCESS;
    mPcieInitAttempted = TRUE;
  } else {
    /* Keep this path reentrant so a caller can retry after a failed revoke or
     * after the resource protocols become available later in DXE. */
    mPcieInitStatus    = Status;
    mPcieInitAttempted = FALSE;
  }
  return mPcieInitStatus;
}

PcieDeviceContext *
EFIAPI
CranePcieHostGetContext (
  VOID
  )
{
  return mPcieContext;
}

STATIC
EFI_STATUS
FindConfigPort (
  IN  UINT64                 Address,
  IN  UINTN                  Width,
  OUT PciePortRuntime       **Port,
  OUT UINT16                *PortIndex
  )
{
  UINT16             Index;
  UINT16             Segment;
  UINT8              Bus;
  UINT8              Device;
  UINT8              Function;
  UINT16             Register;
  PciePortRuntime   *Candidate;

  Segment  = (UINT16)((Address >> 32) & 0xFFFFU);
  Bus      = (UINT8)((Address >> 20) & 0xFFU);
  Device  = (UINT8)((Address >> 15) & 0x1FU);
  Function = (UINT8)((Address >> 12) & 0x07U);
  Register = (UINT16)(Address & 0x0FFFU);
  if ((Port == NULL) || (PortIndex == NULL) || (mPcieContext == NULL) ||
      (mPcieTarget == NULL) || (Register >= 0x1000U) ||
      (Device > 31U) || (Function > 7U)) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if ((mPcieInitializedMask & (1U << Index)) == 0 ||
        mPcieTarget->Controllers[Index].Domain != Segment ||
        Bus < mPcieTarget->Controllers[Index].BusStart ||
        Bus > mPcieTarget->Controllers[Index].BusEnd) {
      continue;
    }
    Candidate = &mPcieContext->Ports[Index];
    /* PcieConfigAccess() performs the window-specific DBI/CFG validation and
     * dynamically programs iATU for non-root BDFs.  Do not reject a valid
     * downstream bus merely because the CPU-side CFG aperture is one MiB. */
    (VOID)Width;
    *Port      = Candidate;
    *PortIndex = Index;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
CranePcieHostConfigAccess (
  IN UINT64    Address,
  IN UINTN     Width,
  IN BOOLEAN   Write,
  IN OUT UINT32 *Value
  )
{
  EFI_STATUS        Status;
  PciePortRuntime  *Port;
  UINT16            PortIndex;
  UINT8             Bus;
  UINT8             Device;
  UINT8             Function;
  UINT16            Register;

  if (Value == NULL || ((Address & 0xFFFF0000F0000000ULL) != 0) ||
      ((Width != 1) && (Width != 2) && (Width != 4)) ||
      (Address & (Width - 1U)) != 0) {
    return EFI_INVALID_PARAMETER;
  }
  if ((Address & 0x0FFFU) > 0x1000U - Width) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CranePcieHostInitialize ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = FindConfigPort (Address, Width, &Port, &PortIndex);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (mPcieIo.Read32 == NULL || mPcieIo.Write32 == NULL || Port == NULL) {
    return EFI_UNSUPPORTED;
  }

  Bus      = (UINT8)((Address >> 20) & 0xFFU);
  Device   = (UINT8)((Address >> 15) & 0x1FU);
  Function = (UINT8)((Address >> 12) & 0x07U);
  Register = (UINT16)(Address & 0x0FFFU);
  return PcieConfigAccess (
      mPcieContext, PortIndex, Bus, Device, Function, Register, Width, Write,
      Value);
}

EFI_STATUS
EFIAPI
CranePcieHostShutdown (
  VOID
  )
{
  EFI_STATUS  FirstError;
  UINT16      Index;
  CR_STATUS   Status;

  if (mPcieContext == NULL || mPcieTarget == NULL) {
    return EFI_NOT_READY;
  }
  FirstError = EFI_SUCCESS;
  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if (((mPcieInitializedMask | mPcieContext->ResourceMask) &
         (1U << Index)) == 0) {
      continue;
    }
    Status = PcieShutdownPort (mPcieContext, Index);
    if (CR_ERROR (Status) && !EFI_ERROR (FirstError)) {
      FirstError = Status;
    }
  }
  /* PcieShutdownPort deliberately retains failed ports so SMMU/fabric
   * revocation can be retried.  Mirror only the ports it actually released. */
  mPcieInitializedMask = mPcieContext->InitializedMask;
  if (!EFI_ERROR (FirstError) &&
      (mPcieContext->ResourceMask == 0) &&
      (mPcieContext->InitializedMask == 0)) {
    /* Permit a later explicit cold-init cycle after a complete shutdown. */
    mPcieInitAttempted = FALSE;
    mPcieInitStatus    = EFI_NOT_READY;
  }
  return FirstError;
}

STATIC
VOID
FreeBridgeArray (
  IN PCI_ROOT_BRIDGE  *Bridges,
  IN UINTN              Count
  )
{
  UINTN  Index;

  if (Bridges == NULL) {
    return;
  }

  for (Index = 0; Index < Count; Index++) {
    if (Bridges[Index].DevicePath != NULL) {
      FreePool (Bridges[Index].DevicePath);
    }
  }

  FreePool (Bridges);
}

/**
  Return all root bridge instances exported by cold-initialized controllers.

  PciHostBridgeDxe is the only PCIe UEFI protocol producer.  A port is added
  here only after PcieLib has completed its resource callback sequence,
  including the SMMU hand-off.
**/
PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  OUT UINTN  *Count
  )
{
  EFI_STATUS        Status;
  PCI_ROOT_BRIDGE  *Bridges;
  UINTN             BridgeCount;
  UINTN             Index;
  UINTN             BridgeIndex;

  if (Count == NULL) {
    return NULL;
  }

  *Count = 0;
  Status = CranePcieHostInitialize ();
  if (EFI_ERROR (Status) || (mPcieTarget == NULL) ||
      (mPcieInitializedMask == 0)) {
    (VOID)CranePcieHostShutdown ();
    return NULL;
  }

  BridgeCount = 0;
  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if ((mPcieInitializedMask & (1U << Index)) != 0) {
      BridgeCount++;
    }
  }
  if (BridgeCount == 0 || BridgeCount > MAX_UINTN / sizeof (*Bridges)) {
    (VOID)CranePcieHostShutdown ();
    return NULL;
  }

  Bridges = AllocateZeroPool (BridgeCount * sizeof (*Bridges));
  if (Bridges == NULL) {
    (VOID)CranePcieHostShutdown ();
    return NULL;
  }

  BridgeIndex = 0;
  for (Index = 0; Index < mPcieTarget->ControllerCount; Index++) {
    if ((mPcieInitializedMask & (1U << Index)) == 0) {
      continue;
    }
    Status = BuildRootBridge ((UINT16)Index, &Bridges[BridgeIndex]);
    if (EFI_ERROR (Status)) {
      FreeBridgeArray (Bridges, BridgeCount);
      (VOID)CranePcieHostShutdown ();
      return NULL;
    }
    CanonicalizeBridge (&Bridges[BridgeIndex]);
    BridgeIndex++;
  }

  *Count = BridgeCount;
  return Bridges;
}

/**
  Free root bridge instances returned by PciHostBridgeGetRootBridges().
**/
VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  IN PCI_ROOT_BRIDGE  *Bridges,
  IN UINTN              Count
  )
{
  FreeBridgeArray (Bridges, Count);
}

/**
  Report a PCI resource conflict using the standard descriptor format.
**/
VOID
EFIAPI
PciHostBridgeResourceConflict (
  IN EFI_HANDLE  HostBridgeHandle,
  IN VOID       *Configuration
  )
{
  EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR  *Descriptor;
  UINTN                              RootBridgeIndex;

  (VOID)HostBridgeHandle;
  if (Configuration == NULL) {
    return;
  }

  DEBUG ((DEBUG_ERROR, "Crane PCIe: resource conflict\n"));
  RootBridgeIndex = 0;
  Descriptor      = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)Configuration;
  while (Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR) {
    DEBUG ((DEBUG_ERROR, "RootBridge[%u]:\n", RootBridgeIndex++));
    for ( ; Descriptor->Desc == ACPI_ADDRESS_SPACE_DESCRIPTOR; Descriptor++) {
      if (Descriptor->ResType <= ACPI_ADDRESS_SPACE_TYPE_BUS) {
        DEBUG ((
          DEBUG_ERROR,
          " %s: Length/Alignment = 0x%lx / 0x%lx\n",
          mPciHostBridgeLibAcpiAddressSpaceTypeStr[Descriptor->ResType],
          Descriptor->AddrLen,
          Descriptor->AddrRangeMax
          ));
      }
    }

    if (Descriptor->Desc != ACPI_END_TAG_DESCRIPTOR) {
      return;
    }

    Descriptor = (EFI_ACPI_ADDRESS_SPACE_DESCRIPTOR *)(
                    (EFI_ACPI_END_TAG_DESCRIPTOR *)Descriptor + 1
                    );
  }
}
