/** SM8150 HDK native providers. Resource facts: sm8150{,-hdk}.dts,
    pm8150.dtsi and Android pdc-sm8150.c; see HanaCrPkg references.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrTargetPmicGpioLib.h>
#include <Library/CrTargetButtonsLib.h>
#include <Library/CrDalLib.h>
#include <Library/spmi.h>

STATIC RpmhDeviceContext mRpmh = {
  .drv_base_address=0x18220000,.drv_size=0x10000,.drv_id=2,.tcs_offset=0xd00,
  .InterruptConfig={.InterruptNumber=GIC_SPI(5),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .tcs_config={.active_tcs={.tcs_count=2},.sleep_tcs={.tcs_count=3},
               .wake_tcs={.tcs_count=3},.control_tcs={.tcs_count=1}},
};
/* SM8150 PDC has no separately described SPI configuration aperture. The
   Android PDC driver also programs only PDC registers and the parent GIC. */
STATIC PdcDeviceContext mPdc = {
  .BaseAddress=0x0b220000,.Size=0x10000,.PdcRangeCount=3,
  .PdcRegion={{.PdcPinBase=0,.GicSpiBase=480,.PdcCount=94},
              {.PdcPinBase=94,.GicSpiBase=609,.PdcCount=31},
              {.PdcPinBase=125,.GicSpiBase=63,.PdcCount=1}},
};
STATIC CONST CR_DAL_CMD_DB_CONFIG mCmdDb = {.BaseAddress=0x85f20000,.Size=0x20000};
STATIC SpmiDeviceContext mSpmi = {
  .Regions={
    [SPMI_MEMORY_REGION_TYPE_CORE]={0x0c440000,0x1100},
    [SPMI_MEMORY_REGION_TYPE_CH_SLAVES]={0x0c600000,0x2000000},
    [SPMI_MEMORY_REGION_TYPE_OBSERVER]={0x0e600000,0x100000},
    [SPMI_MEMORY_REGION_TYPE_INTERRUPT]={0x0e700000,0xa0000},
    [SPMI_MEMORY_REGION_TYPE_CONFIG]={0x0c40a000,0x26000}},
  .PdcPinNumber=1,.ActiveEE=0,.Channel=0,.BusId=0,.HasInterrupt=TRUE,
  .InterruptConfig={.InterruptNumber=GIC_SPI(481),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
};
STATIC CONST PmicGpioTargetController mPmicControllers[] = {
  {"pm8150","qcom,pm8150-gpio",0,0xc000,10},
};
STATIC CONST PmicGpioTargetContext mPmicGpio = {mPmicControllers,ARRAY_SIZE(mPmicControllers)};
STATIC CONST ButtonTarget mButtons[] = {
  {"power",BUTTON_SOURCE_PON,BUTTON_CODE_POWER,"pm8150",0,0x08,0,0,FALSE,15625,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-up",BUTTON_SOURCE_PMIC_GPIO,BUTTON_CODE_VOLUME_UP,"pm8150",0,0xc5,6,0,TRUE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-down",BUTTON_SOURCE_PON,BUTTON_CODE_VOLUME_DOWN,"pm8150",0,0x08,0,1,FALSE,15625,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
};
STATIC CONST ButtonsTargetContext mButtonContext={mButtons,ARRAY_SIZE(mButtons)};
/* Names only: these inventory entries do not select a voltage or enable a
   rail. Consumer supply policy will be resolved from qualified boot FDT data. */
STATIC CONST CHAR8 *CONST mRailNames[]={
  "ldoa2","ldoa5","ldoa9","ldoa10","ldoa12","ldoa18","ldoc3"
};
STATIC CONST CR_DAL_RPMH_RAILS mRails={ARRAY_SIZE(mRailNames),mRailNames};
#define IMM (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define MUT (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define ENTRY(kind,obj,attrs) {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),kind,0,CR_DAL_DATA_REVISION_1,attrs,sizeof(obj),&obj}
STATIC CR_DAL_DEVICE_INFO mDevices[]={
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceDebugUart,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(CrDebugUartContext),NULL},
  ENTRY(CrDalDeviceRpmh,mRpmh,MUT),ENTRY(CrDalDevicePdc,mPdc,MUT),
  ENTRY(CrDalDeviceCmdDb,mCmdDb,IMM),ENTRY(CrDalDeviceSpmi,mSpmi,MUT),
  ENTRY(CrDalDevicePmicGpio,mPmicGpio,IMM),ENTRY(CrDalDeviceButtons,mButtonContext,IMM),
  ENTRY(CrDalDeviceRpmhRails,mRails,IMM),
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceGpio,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(GpioDeviceContext),NULL},
};
STATIC CONST CR_DAL_DEVICE_MANIFEST mManifest={CR_DAL_DEVICE_MANIFEST_REVISION,sizeof(CR_DAL_DEVICE_MANIFEST),ARRAY_SIZE(mDevices),mDevices};
EFI_STATUS CrTargetGetDeviceManifest(CONST CR_DAL_DEVICE_MANIFEST **Manifest) {
  if (Manifest==NULL) return EFI_INVALID_PARAMETER;
  *Manifest=NULL;
  mDevices[0].Data=CrTargetGetDebugUartContext();
  mDevices[8].Data=CrTargetGetGpioContext();
  if (mDevices[0].Data==NULL || mDevices[8].Data==NULL) return EFI_NOT_FOUND;
  *Manifest=&mManifest;return EFI_SUCCESS;
}
