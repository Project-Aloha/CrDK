/** @file
  SM8450 target-data manifest consumed by CrDALDxe.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrTargetClockLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetInterconnectLib.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetMdssLib.h>
#include <Library/CrTargetPcieLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetQupLib.h>
#include <Library/CrTargetSdLib.h>
#include <Library/CrTargetUfsLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrTargetSmmuLib.h>
#include <Library/CrTargetTrngLib.h>
#include <Library/CrTargetButtonsLib.h>
#include <Library/CrTargetPmicGpioLib.h>
#include <Library/spmi.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/pcie.h>
#include <Library/PILCrLib.h>

#define CR_DAL_STATIC_IMMUTABLE \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define CR_DAL_STATIC_MUTABLE \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)

enum {
  Sm8450DalRpmh,
  Sm8450DalClock,
  Sm8450DalClockServices,
  Sm8450DalDebugClock,
  Sm8450DalDebugUart,
  Sm8450DalGpio,
  Sm8450DalPdc,
  Sm8450DalTrng,
  Sm8450DalPcie,
  Sm8450DalPcieIo,
  Sm8450DalInterconnect,
  Sm8450DalSmmu,
  Sm8450DalQup,
  Sm8450DalCmdDb,
  Sm8450DalRpmhRails,
  Sm8450DalRpmhTopology,
  Sm8450DalPil,
  Sm8450DalSpmi,
  Sm8450DalPmicGpio,
  Sm8450DalButtons,
  Sm8450DalSd,
  Sm8450DalUfs,
  Sm8450DalMdss,
  // Optional board device must remain last in the manifest.
  Sm8450DalLt9611,
  Sm8450DalCount
};

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockInitialize (
  IN ClockDriverContext *ClockContext
  )
{
  return CrTargetClockInit (ClockContext);
}

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockSetReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  return CrTargetClockReset (Controller, Id, Assert);
}

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  return CrTargetClockResolveResource (
           Controller,
           Id,
           Kind,
           NativeId,
           Flags
           );
}

STATIC CONST CR_DAL_CLOCK_SERVICES mSm8450ClockServices = {
  .Revision        = CR_DAL_CLOCK_SERVICES_REVISION,
  .Size            = sizeof (CR_DAL_CLOCK_SERVICES),
  .Initialize      = Sm8450ClockInitialize,
  .SetReset        = Sm8450ClockSetReset,
  .ResolveResource = Sm8450ClockResolveResource,
};

STATIC PcieIoOps mSm8450PcieIo;
STATIC CR_DAL_CMD_DB_CONFIG mSm8450CmdDb;
STATIC CR_DAL_LT9611_CONFIG mSm8450Lt9611;

/* Linux sm8450.dtsi / Android waipio.dtsi: arbiter bus 0, AP EE 0.
   PDC pin 1 maps to GIC SPI 481 (interrupt source 513). */
STATIC SpmiDeviceContext mSm8450Spmi = {
  .Regions = {
    [SPMI_MEMORY_REGION_TYPE_CORE] = {0x0c400000, 0x3000},
    [SPMI_MEMORY_REGION_TYPE_CH_SLAVES] = {0x0c500000, 0x400000},
    [SPMI_MEMORY_REGION_TYPE_OBSERVER] = {0x0c440000, 0x80000},
    [SPMI_MEMORY_REGION_TYPE_INTERRUPT] = {0x0c4c0000, 0x10000},
    [SPMI_MEMORY_REGION_TYPE_CONFIG] = {0x0c42d000, 0x10000},
  },
  .PdcPinNumber = 1,
  .ActiveEE = 0, .Channel = 0, .BusId = 0,
  .HasInterrupt = TRUE,
  .InterruptConfig = {.InterruptNumber = 513,
                      .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
};
STATIC CONST PmicGpioTargetController mSm8450PmicGpios[] = {
  {"pm8350", "qcom,pm8350-gpio", 1, 0x8800, 10},
};
STATIC CONST PmicGpioTargetContext mSm8450PmicGpio = {
  mSm8450PmicGpios, ARRAY_SIZE(mSm8450PmicGpios)
};
/* HDK8450 schematic sheet 56 overrides Android's RESIN key-code label.
   PON RT_STS bits are asserted on press; GPIO6 is electrically active low. */
STATIC CONST ButtonTarget mSm8450Buttons[] = {
  {"power", BUTTON_SOURCE_PON, BUTTON_CODE_POWER, "pmk8350",
   0, 0x13, 0, 7, FALSE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-up", BUTTON_SOURCE_PMIC_GPIO, BUTTON_CODE_VOLUME_UP, "pm8350",
   1, 0x8d, 6, 0, TRUE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-down", BUTTON_SOURCE_PON, BUTTON_CODE_VOLUME_DOWN, "pmk8350",
   0, 0x13, 0, 6, FALSE, 15000, CR_INTERRUPT_TRIGGER_EDGE_BOTH},
};
STATIC CONST ButtonsTargetContext mSm8450ButtonContext = {
  mSm8450Buttons, ARRAY_SIZE(mSm8450Buttons)
};

#include "CrRpmhRails.h"

/* OnePlus Android waipio.dtsi / waipio-smp2p.dtsi and Linux sm8450.dtsi.
   Firmware path and carveout match HDK8450's uefipil.cfg FULL_ADSP. */
STATIC CONST CR_PIL_TARGET mSm8450Pil = {
  .PasId = 1, .MemoryBase = 0x85e00000, .MemorySize = 0x02100000,
  .SmemHost = 2, .SmemIn = 443, .SmemOut = 429,
  .IpccSend = 0x0ed1800c, .IpccClient = 3,
  .QmpBase = 0x0c300000, .QmpSize = 0x400,
  .ImagePath = L"\\image\\adsp",
  .InterconnectProvider = "qcom,sm8450-pil",
  .CryptoSource = 0x7000, .CryptoDestination = 0x7001,
};

#define CR_DAL_ENTRY(DeviceType, PayloadType, DeviceAttributes) \
  { \
    .Revision     = CR_DAL_DEVICE_INFO_REVISION, \
    .Size         = sizeof (CR_DAL_DEVICE_INFO), \
    .Type         = (DeviceType), \
    .Instance     = 0, \
    .DataRevision = CR_DAL_DATA_REVISION_1, \
    .Attributes   = (DeviceAttributes), \
    .DataSize     = sizeof (PayloadType), \
    .Data         = NULL, \
  }

STATIC CR_DAL_DEVICE_INFO mSm8450Devices[Sm8450DalCount] = {
  [Sm8450DalSd] = CR_DAL_ENTRY (CrDalDeviceSd, CR_TARGET_SD_CONTEXT, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalUfs] = CR_DAL_ENTRY (CrDalDeviceUfs, CR_TARGET_UFS_CONTEXT, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalRpmh] =
    CR_DAL_ENTRY (CrDalDeviceRpmh, RpmhDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalClock] =
    CR_DAL_ENTRY (CrDalDeviceClock, ClockDriverContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalClockServices] =
    CR_DAL_ENTRY (
      CrDalDeviceClockServices,
      CR_DAL_CLOCK_SERVICES,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalDebugClock] =
    CR_DAL_ENTRY (
      CrDalDeviceDebugClock,
      DebugccDriverContext,
      CR_DAL_STATIC_MUTABLE
      ),
  [Sm8450DalDebugUart] =
    CR_DAL_ENTRY (
      CrDalDeviceDebugUart,
      CrDebugUartContext,
      CR_DAL_STATIC_MUTABLE
      ),
  [Sm8450DalGpio] =
    CR_DAL_ENTRY (CrDalDeviceGpio, GpioDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalPdc] =
    CR_DAL_ENTRY (CrDalDevicePdc, PdcDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalTrng] =
    CR_DAL_ENTRY (CrDalDeviceTrng, CR_TRNG_CONFIG, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalPcie] =
    CR_DAL_ENTRY (CrDalDevicePcie, PcieTargetContext, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalPcieIo] =
    CR_DAL_ENTRY (CrDalDevicePcieIo, PcieIoOps, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalInterconnect] =
    CR_DAL_ENTRY (
      CrDalDeviceInterconnect,
      InterconnectTargetContext,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalSmmu] =
    CR_DAL_ENTRY (
      CrDalDeviceSmmu,
      CrTargetSmmuContext,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalQup] =
    CR_DAL_ENTRY (
      CrDalDeviceQup,
      CR_TARGET_QUP_CONTEXT,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalCmdDb] =
    CR_DAL_ENTRY (
      CrDalDeviceCmdDb, CR_DAL_CMD_DB_CONFIG, CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalMdss] =
    CR_DAL_ENTRY (CrDalDeviceMdss, CR_MDSS_TARGET, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalLt9611] =
    CR_DAL_ENTRY (
      CrDalDeviceLt9611, CR_DAL_LT9611_CONFIG, CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalRpmhRails] =
    CR_DAL_ENTRY (
      CrDalDeviceRpmhRails, CR_DAL_RPMH_RAILS, CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalPil] =
    CR_DAL_ENTRY (CrDalDevicePil, CR_PIL_TARGET, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalRpmhTopology] =
    CR_DAL_ENTRY (CrDalDeviceRpmhTopology, CR_DAL_RPMH_TOPOLOGY,
                  CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalSpmi] =
    CR_DAL_ENTRY (CrDalDeviceSpmi, SpmiDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalPmicGpio] =
    CR_DAL_ENTRY (CrDalDevicePmicGpio, PmicGpioTargetContext, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalButtons] =
    CR_DAL_ENTRY (CrDalDeviceButtons, ButtonsTargetContext, CR_DAL_STATIC_IMMUTABLE),
};

STATIC CR_DAL_DEVICE_MANIFEST mSm8450Manifest = {
  .Revision    = CR_DAL_DEVICE_MANIFEST_REVISION,
  .Size        = sizeof (CR_DAL_DEVICE_MANIFEST),
  .DeviceCount = Sm8450DalCount,
  .Devices     = mSm8450Devices,
};

EFI_STATUS
CrTargetGetDeviceManifest (
  OUT CONST CR_DAL_DEVICE_MANIFEST **Manifest
  )
{
  STATIC BOOLEAN Initialized;
  EFI_STATUS     Status;
  UINTN          Index;
  ARM_MEMORY_REGION_DESCRIPTOR_EX CmdDbRegion;
  UINTN          PowerPinsSize;

  if (Manifest == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Manifest = NULL;

  if (!Initialized) {
    Status = LocateMemoryMapAreaByName ("AOP CMD DB", &CmdDbRegion);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mSm8450CmdDb.BaseAddress = CmdDbRegion.Address;
    mSm8450CmdDb.Size = CmdDbRegion.Length;
    mSm8450Devices[Sm8450DalCmdDb].Data = &mSm8450CmdDb;

    PowerPinsSize = FixedPcdGetSize (PcdLt9611GpioHigh);
    mSm8450Manifest.DeviceCount = Sm8450DalLt9611;
    if ((PowerPinsSize != 0) && ((PowerPinsSize % sizeof (UINT16)) == 0)) {
      mSm8450Lt9611.InterruptPin = FixedPcdGet16 (PcdLt9611GpioInt);
      mSm8450Lt9611.PowerPinCount = PowerPinsSize / sizeof (UINT16);
      mSm8450Lt9611.PowerPins = FixedPcdGetPtr (PcdLt9611GpioHigh);
      // Linux i2c9 is Qualcomm's one-based I2C_INSTANCE_010.
      mSm8450Lt9611.I2cInstance = 10;
      mSm8450Lt9611.I2cAddress = 0x2b;
      mSm8450Devices[Sm8450DalLt9611].Data = &mSm8450Lt9611;
      mSm8450Manifest.DeviceCount++;
    }
    mSm8450Devices[Sm8450DalRpmh].Data = CrTargetGetRpmhContext ();
    mSm8450Devices[Sm8450DalRpmhRails].Data = &mSm8450Rails;
    mSm8450Devices[Sm8450DalRpmhTopology].Data = &mSm8450RailTopology;
    mSm8450Devices[Sm8450DalPil].Data = &mSm8450Pil;
    mSm8450Devices[Sm8450DalSpmi].Data = &mSm8450Spmi;
    mSm8450Devices[Sm8450DalPmicGpio].Data = &mSm8450PmicGpio;
    mSm8450Devices[Sm8450DalButtons].Data = &mSm8450ButtonContext;
    mSm8450Devices[Sm8450DalClock].Data = CrTargetGetClockContext ();
    mSm8450Devices[Sm8450DalClockServices].Data = &mSm8450ClockServices;
    mSm8450Devices[Sm8450DalDebugClock].Data = CrTargetGetDebugccContext ();
    mSm8450Devices[Sm8450DalDebugUart].Data = CrTargetGetDebugUartContext ();
    mSm8450Devices[Sm8450DalGpio].Data = CrTargetGetGpioContext ();
    mSm8450Devices[Sm8450DalPdc].Data = GetPdcDevContext ();
    mSm8450Devices[Sm8450DalTrng].Data = CrTargetGetTrngConfig ();
    mSm8450Devices[Sm8450DalPcie].Data = CrTargetGetPcieContext ();
    Status = CrTargetGetPcieIo (&mSm8450PcieIo);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mSm8450Devices[Sm8450DalPcieIo].Data = &mSm8450PcieIo;
    mSm8450Devices[Sm8450DalInterconnect].Data =
      CrTargetGetInterconnectContext ();
    mSm8450Devices[Sm8450DalMdss].Data = CrTargetGetMdssContext ();
    mSm8450Devices[Sm8450DalSmmu].Data = CrTargetGetSmmuContext ();
    mSm8450Devices[Sm8450DalQup].Data = CrTargetGetQupContext ();
    mSm8450Devices[Sm8450DalSd].Data = CrTargetGetSdContext ();
    mSm8450Devices[Sm8450DalUfs].Data = CrTargetGetUfsContext ();

    for (Index = 0; Index < mSm8450Manifest.DeviceCount; Index++) {
      if (mSm8450Devices[Index].Data == NULL) {
        return EFI_NOT_FOUND;
      }
    }
    Initialized = TRUE;
  }

  *Manifest = &mSm8450Manifest;
  return EFI_SUCCESS;
}
