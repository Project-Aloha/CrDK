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

#define CRANE_PCIE_RESOURCE_REFS  64U

typedef struct {
  CONST CHAR8  *Id;
  UINT16        Count;
} CRANE_PCIE_RESOURCE_REF;

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
  BOOLEAN                     IommuAttached[PCIE_MAX_CONTROLLERS];
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
      (AsciiStrCmp (Supply->Controller, "rpmh") != 0) ||
      (mPcieResources.Rpmh == NULL) ||
      (mPcieResources.Rpmh->RpmhEnableVreg == NULL)) {
    return CR_UNSUPPORTED;
  }
  if (Supply->LoadUa != 0) {
    return CR_UNSUPPORTED;
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
  }

  if (Enable && (Supply->VoltageMv != 0)) {
    if (mPcieResources.Rpmh->RpmhSetVregVoltage == NULL) {
      return CR_UNSUPPORTED;
    }
    Status = mPcieResources.Rpmh->RpmhSetVregVoltage (
                                  mPcieResources.Rpmh,
                                  Supply->Id,
                                  Supply->VoltageMv
                                  );
    if (EFI_ERROR (Status)) {
      return Status;
    }
  }
  Status = mPcieResources.Rpmh->RpmhEnableVreg (
                                mPcieResources.Rpmh,
                                Supply->Id,
                                Enable
                                );
  if (EFI_ERROR (Status)) {
    if (!Enable) {
      Ref->Count = 1;
    }
    return Status;
  }
  if (Enable) {
    Ref->Count = 1;
  }
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
  }

  Status = CR_SUCCESS;
  if (Clock->Provider == PCIE_CLOCK_PROVIDER_GCC) {
    Status = mPcieResources.ClockProtocol->SetClock (
               mPcieResources.ClockProtocol, Clock->Controller, Clock->Id,
               Clock->RateHz, Enable);
  }
  if (CR_ERROR (Status)) {
    if (!Enable) {
      Ref->Count = 1;
    }
    return Status;
  }
  if (Enable) {
    Ref->Count = 1;
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
  (VOID)Context;
  if ((Controller == NULL) || (Controller->PowerDomainController == NULL) ||
      (Controller->PowerDomainId == NULL) ||
      (mPcieResources.ClockProtocol == NULL) ||
      (mPcieResources.ClockProtocol->SetGdsc == NULL)) {
    return CR_UNSUPPORTED;
  }
  return mPcieResources.ClockProtocol->SetGdsc (
             mPcieResources.ClockProtocol, Controller->PowerDomainController,
             Controller->PowerDomainId, Enable);
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
  EFI_STATUS        Status;
  BOOLEAN           Level;

  (VOID)Context;
  if ((Gpio == NULL) || (Gpio->Controller == NULL) ||
      (Gpio->Polarity == NULL) ||
      (AsciiStrCmp (Gpio->Controller, "tlmm") != 0) ||
      (mPcieResources.Gpio == NULL) ||
      (mPcieResources.Gpio->InitGpioConfigParams == NULL) ||
      (mPcieResources.Gpio->ConfigGpio == NULL)) {
    return CR_UNSUPPORTED;
  }

  ZeroMem (&Config, sizeof (Config));
  Config.PinNumber = Gpio->Pin;
  Status = mPcieResources.Gpio->InitGpioConfigParams (
                                  mPcieResources.Gpio, &Config);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Config.FunctionSel = GPIO_FUNC_NORMAL;
  Config.OutputEnable = (Direction == PCIE_GPIO_OUTPUT);
  if (Config.OutputEnable) {
    Level = Active;
    if (AsciiStrCmp (Gpio->Polarity, "GPIO_ACTIVE_LOW") == 0) {
      Level = !Level;
    } else if (AsciiStrCmp (Gpio->Polarity, "GPIO_ACTIVE_HIGH") != 0) {
      return CR_INVALID_PARAMETER;
    }
    Config.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
    Config.OutputValue = Level ? GPIO_VALUE_HIGH : GPIO_VALUE_LOW;
  } else {
    Config.Pull = GPIO_PULL_NONE;
  }
  return mPcieResources.Gpio->ConfigGpio (mPcieResources.Gpio, &Config);
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
      if (!EFI_ERROR (BlockStatus)) {
        mPcieResources.IommuAttached[ControllerIndex] = FALSE;
      }
      return EFI_ERROR (BlockStatus) ? BlockStatus : AttachStatus;
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
    return CR_SUCCESS;
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
    (VOID)Interconnect->ReleasePath(Interconnect, MemPath);
    return CR_DEVICE_ERROR;
  }

  CpuPath = 0;
  EfiStatus = Interconnect->AcquirePath(
      Interconnect, Target->Provider, Target->CpuSource,
      Target->CpuDestination, &CpuPath);
  if (EFI_ERROR(EfiStatus) || CpuPath == 0) {
    (VOID)Interconnect->ReleasePath(Interconnect, MemPath);
    return CR_DEVICE_ERROR;
  }
  EfiStatus = Interconnect->SetBandwidth(
      Interconnect, CpuPath, Target->CpuAverage, Target->CpuPeak);
  if (EFI_ERROR(EfiStatus)) {
    (VOID)Interconnect->ReleasePath(Interconnect, CpuPath);
    (VOID)Interconnect->ReleasePath(Interconnect, MemPath);
    return CR_DEVICE_ERROR;
  }

  mPcieResources.MemPaths[Index] = MemPath;
  mPcieResources.CpuPaths[Index] = CpuPath;
  return CR_SUCCESS;
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
  mPcieIo.SetClock        = CranePcieSetClock;
  mPcieIo.SetReset        = CranePcieSetReset;
  mPcieIo.SetPowerDomain  = CranePcieSetPowerDomain;
  mPcieIo.SetSupply       = CranePcieSetSupply;
  mPcieIo.SetInterconnect = CranePcieSetInterconnect;
  mPcieIo.SetIommu        = CranePcieSetIommu;
  mPcieIo.SetGpio         = CranePcieSetGpio;
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
  Bridge->DmaAbove4G           = FALSE;
  Bridge->NoExtendedConfigSpace = FALSE;
  Bridge->ResourceAssigned     = TRUE;
  Bridge->AllocationAttributes = 0;
  Bridge->Supports = EFI_PCI_ATTRIBUTE_ISA_IO |
                     EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO |
                     EFI_PCI_ATTRIBUTE_VGA_MEMORY |
                     EFI_PCI_ATTRIBUTE_VGA_IO |
                     EFI_PCI_ATTRIBUTE_IDE_PRIMARY_IO |
                     EFI_PCI_ATTRIBUTE_IDE_SECONDARY_IO |
                     EFI_PCI_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                     EFI_PCI_ATTRIBUTE_MEMORY_CACHED |
                     EFI_PCI_ATTRIBUTE_DUAL_ADDRESS_CYCLE;
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
    Bridge->DmaAbove4G           = TRUE;
    Bridge->AllocationAttributes = EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
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

  mPcieTarget = CrTargetGetPcieContext ();
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
  Status = CrTargetGetPcieIo (&mPcieIo);
  if (CR_ERROR (Status)) {
    mPcieInitStatus = Status;
    return mPcieInitStatus;
  }
  /* CrTargetGetPcieIo supplies MMIO/delay primitives; the adapter then
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
