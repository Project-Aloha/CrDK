/** Kaanapali QRD source-qualified providers; see KaanapaliCrPkg/README.md.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrDalLib.h>
STATIC RpmhDeviceContext mRpmh={
 .drv_base_address=0x18920000,.drv_size=0x10000,.drv_id=2,.tcs_offset=0xd00,
 .InterruptConfig={.InterruptNumber=GIC_SPI(5),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
 .tcs_config={.active_tcs={.tcs_count=3},.sleep_tcs={.tcs_count=2},
 .wake_tcs={.tcs_count=2},.control_tcs={.tcs_count=0}},
};
STATIC PdcDeviceContext mPdc={
 .BaseAddress=0x0b220000,.Size=0x10000,
 .SPIConfigBase=0x179600f0,.SPIConfigSize=0xf4,.PdcRangeCount=24,
 .PdcRegion={
  {.PdcPinBase=0,.GicSpiBase=745,.PdcCount=38},
  {.PdcPinBase=40,.GicSpiBase=785,.PdcCount=11},
  {.PdcPinBase=51,.GicSpiBase=527,.PdcCount=4},
  {.PdcPinBase=58,.GicSpiBase=534,.PdcCount=2},
  {.PdcPinBase=61,.GicSpiBase=537,.PdcCount=20},
  {.PdcPinBase=84,.GicSpiBase=559,.PdcCount=14},
  {.PdcPinBase=98,.GicSpiBase=609,.PdcCount=32},
  {.PdcPinBase=130,.GicSpiBase=717,.PdcCount=12},
  {.PdcPinBase=142,.GicSpiBase=251,.PdcCount=5},
  {.PdcPinBase=147,.GicSpiBase=796,.PdcCount=16},
  {.PdcPinBase=163,.GicSpiBase=783,.PdcCount=2},
  {.PdcPinBase=165,.GicSpiBase=531,.PdcCount=2},
  {.PdcPinBase=167,.GicSpiBase=536,.PdcCount=1},
  {.PdcPinBase=168,.GicSpiBase=557,.PdcCount=2},
  {.PdcPinBase=170,.GicSpiBase=415,.PdcCount=1},
  {.PdcPinBase=171,.GicSpiBase=438,.PdcCount=1},
  {.PdcPinBase=172,.GicSpiBase=579,.PdcCount=1},
  {.PdcPinBase=173,.GicSpiBase=703,.PdcCount=1},
  {.PdcPinBase=174,.GicSpiBase=708,.PdcCount=1},
  {.PdcPinBase=175,.GicSpiBase=714,.PdcCount=1},
  {.PdcPinBase=176,.GicSpiBase=68,.PdcCount=1},
  {.PdcPinBase=177,.GicSpiBase=86,.PdcCount=1},
  {.PdcPinBase=178,.GicSpiBase=96,.PdcCount=1},
  {.PdcPinBase=179,.GicSpiBase=249,.PdcCount=1},
 },
};
STATIC CONST CR_DAL_CMD_DB_CONFIG mCmdDb={.BaseAddress=0x81c60000,.Size=0x20000};
#define IMM (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define MUT (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define E(type,obj,flags) {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),type,0,CR_DAL_DATA_REVISION_1,flags,sizeof(obj),&obj}
STATIC CR_DAL_DEVICE_INFO mDevices[]={
 {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceDebugUart,0,CR_DAL_DATA_REVISION_1,MUT,sizeof(CrDebugUartContext),NULL},
 E(CrDalDeviceRpmh,mRpmh,MUT),E(CrDalDevicePdc,mPdc,MUT),E(CrDalDeviceCmdDb,mCmdDb,IMM),
 {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceGpio,0,CR_DAL_DATA_REVISION_1,MUT,sizeof(GpioDeviceContext),NULL},
};
STATIC CONST CR_DAL_DEVICE_MANIFEST mManifest={CR_DAL_DEVICE_MANIFEST_REVISION,sizeof(CR_DAL_DEVICE_MANIFEST),ARRAY_SIZE(mDevices),mDevices};
EFI_STATUS CrTargetGetDeviceManifest(CONST CR_DAL_DEVICE_MANIFEST **Manifest){
 if(Manifest==NULL)return EFI_INVALID_PARAMETER;
 *Manifest=NULL;mDevices[0].Data=CrTargetGetDebugUartContext();mDevices[4].Data=CrTargetGetGpioContext();
 if(mDevices[0].Data==NULL || mDevices[4].Data==NULL)return EFI_NOT_FOUND;
 *Manifest=&mManifest;return EFI_SUCCESS;
}
