/** SM8250 HDK provider; Linux sm8250{,-hdk}.dts and pm8150.dtsi.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Library/pcie.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetMdssLib.h>
#include <Library/CrTargetClockLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetInterconnectLib.h>
#include <Library/CrTargetQupLib.h>
#include <Library/CrTargetSdLib.h>
#include <Library/CrTargetSmmuLib.h>
#include <Library/CrTargetUfsLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetTrngLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrTargetPmicGpioLib.h>
#include <Library/CrTargetButtonsLib.h>
#include <Library/CrDalLib.h>
#include <Library/spmi.h>

STATIC CrDebugUartContext mUart = {
  .BaseAddress=0x00a90000,
  .InterruptConfig={.InterruptNumber=GIC_SPI(357),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .IsDebugUart=TRUE,.Type=DEBUG_UART_TYPE_GENI,.BaudRate=115200,
};
STATIC RpmhDeviceContext mRpmh = {
  .drv_base_address=0x18220000,.drv_size=0x10000,.drv_id=2,.tcs_offset=0xd00,
  .InterruptConfig={.InterruptNumber=GIC_SPI(5),.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .tcs_config={.active_tcs={.tcs_count=2},.sleep_tcs={.tcs_count=3},
               .wake_tcs={.tcs_count=3},.control_tcs={.tcs_count=1}},
};
STATIC PdcDeviceContext mPdc = {
  .BaseAddress=0x0b220000,.Size=0x10000,.SPIConfigBase=0x17c000f0,.SPIConfigSize=0x60,
  .PdcRangeCount=4,
  .PdcRegion={{.PdcPinBase=0,.GicSpiBase=480,.PdcCount=94},
              {.PdcPinBase=94,.GicSpiBase=609,.PdcCount=31},
              {.PdcPinBase=125,.GicSpiBase=63,.PdcCount=1},
              {.PdcPinBase=126,.GicSpiBase=716,.PdcCount=12}},
};
STATIC CONST CR_TRNG_CONFIG mTrng = {
  .BaseAddress=0x00793000,.MmioSize=0x1000,.DataOutOffset=0,.StatusOffset=4,
  .DataAvailableMask=BIT0,.MaxEntropyBits=32,.PollDelayUs=200,.TimeoutUs=10000,
};
STATIC CONST CR_DAL_CMD_DB_CONFIG mCmdDb = {.BaseAddress=0x80860000,.Size=0x20000};
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
/* PM8150 PON RT_STS has KPDPWR_N at bit0 and RESIN_N at bit1.
   Their rising state means pressed, per qcom-pwrkey.c. GPIO6 is active low. */
STATIC CONST ButtonTarget mButtons[] = {
  {"power",BUTTON_SOURCE_PON,BUTTON_CODE_POWER,"pm8150",0,0x08,0,0,FALSE,15625,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-up",BUTTON_SOURCE_PMIC_GPIO,BUTTON_CODE_VOLUME_UP,"pm8150",0,0xc5,6,0,TRUE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-down",BUTTON_SOURCE_PON,BUTTON_CODE_VOLUME_DOWN,"pm8150",0,0x08,0,1,FALSE,15625,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
};
STATIC CONST ButtonsTargetContext mButtonContext={mButtons,ARRAY_SIZE(mButtons)};
/* Supplies used by the HDK UFS/USB nodes and Android HDK SDCC2 plus inputs.
   S4A is an RPMh supply in Android; mainline describes a fixed supply. */
STATIC CONST CHAR8 *CONST mRailNames[]={
  "ldoa13","ldoc1","smpf2","ldoa15","ldoc5","rfclka1","ldoa2","ldoa5","ldoa6","ldoa9","ldoa12","ldoa17","ldoa18",
  "smpa4","smpa5","smpa6","smpc8","bobc1","ldoc6","ldoc9","ldoa14","ldoc11"
};
STATIC CONST CR_DAL_RPMH_RAILS mRails={ARRAY_SIZE(mRailNames),mRailNames};
/* PM8150 SMPS use hardware HPM mode 7; PM8150L BOB AUTO mode 6.
   Voltages are within HDK input supply ranges and their selector steps. */
STATIC CONST CR_DAL_RPMH_PARENT mParents[]={
  {"ldoa13","bobc1",3296,6},{"ldoc1","smpa4",1800,7},
  {"ldoa15","smpa5",1904,7},{"ldoc5","bobc1",3296,6},
  {"ldoa2","bobc1",3296,6},{"ldoa5","smpa6",952,7},
  {"ldoa6","smpc8",1352,7},{"ldoa9","smpc8",1352,7},
  {"ldoa12","smpa5",1904,7},{"ldoa17","bobc1",3296,6},
  {"ldoa18","smpa6",952,7},
  /* PM8150L HDK input wiring: both SD PLDOs are supplied from BOB. */
  {"ldoc6","bobc1",3296,6},{"ldoc9","bobc1",3296,6},
};
STATIC CONST CR_DAL_RPMH_TOPOLOGY mTopology={ARRAY_SIZE(mParents),mParents};
STATIC EFI_STATUS EFIAPI ClockInitialize(ClockDriverContext *Context) {
  return CrTargetClockInit(Context);
}
STATIC EFI_STATUS EFIAPI ClockReset(CONST CHAR8 *Controller,CONST CHAR8 *Id,BOOLEAN Assert) {
  return CrTargetClockReset(Controller,Id,Assert);
}
STATIC EFI_STATUS EFIAPI ClockResolve(CONST CHAR8 *Controller,CONST CHAR8 *Id,
    CR_DAL_CLOCK_RESOURCE_KIND Kind,CONST CHAR8 **NativeId,UINT32 *Flags) {
  return CrTargetClockResolveResource(Controller,Id,Kind,NativeId,Flags);
}
STATIC CONST CR_DAL_CLOCK_SERVICES mClockServices={
  .Revision=CR_DAL_CLOCK_SERVICES_REVISION,.Size=sizeof(CR_DAL_CLOCK_SERVICES),
  .Initialize=ClockInitialize,.SetReset=ClockReset,.ResolveResource=ClockResolve,
};
#define IMM (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define MUT (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE|CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE|CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define ENTRY(kind,obj,attrs) {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),kind,0,CR_DAL_DATA_REVISION_1,attrs,sizeof(obj),&obj}
STATIC PcieIoOps mPcieIo;
STATIC CR_DAL_DEVICE_INFO mDevices[]={
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceMdss,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_MDSS_TARGET),NULL},
  ENTRY(CrDalDeviceDebugUart,mUart,MUT),ENTRY(CrDalDeviceRpmh,mRpmh,MUT),
  ENTRY(CrDalDevicePdc,mPdc,MUT),ENTRY(CrDalDeviceTrng,mTrng,IMM),
  ENTRY(CrDalDeviceCmdDb,mCmdDb,IMM),ENTRY(CrDalDeviceSpmi,mSpmi,MUT),
  ENTRY(CrDalDevicePmicGpio,mPmicGpio,IMM),ENTRY(CrDalDeviceButtons,mButtonContext,IMM),
  ENTRY(CrDalDeviceRpmhRails,mRails,IMM),ENTRY(CrDalDeviceRpmhTopology,mTopology,IMM),
  ENTRY(CrDalDeviceClockServices,mClockServices,IMM),
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceClock,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(ClockDriverContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceGpio,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(GpioDeviceContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceInterconnect,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(InterconnectTargetContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceQup,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_QUP_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceSd,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_SD_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceSmmu,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CrTargetSmmuContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceUfs,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_UFS_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDevicePcie,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(PcieTargetContext),NULL},
  ENTRY(CrDalDevicePcieIo,mPcieIo,IMM),
};
STATIC CONST CR_DAL_DEVICE_MANIFEST mManifest={CR_DAL_DEVICE_MANIFEST_REVISION,sizeof(CR_DAL_DEVICE_MANIFEST),ARRAY_SIZE(mDevices),mDevices};
EFI_STATUS CrTargetGetDeviceManifest(CONST CR_DAL_DEVICE_MANIFEST **Manifest) {
  if (Manifest==NULL) return EFI_INVALID_PARAMETER;
  for (UINTN Index=0;Index<ARRAY_SIZE(mDevices);++Index) {
    switch (mDevices[Index].Type) {
      case CrDalDeviceMdss:mDevices[Index].Data=CrTargetGetMdssContext();break;
      case CrDalDevicePcie:mDevices[Index].Data=CrTargetGetPcieContext();break;
      case CrDalDevicePcieIo:{
        CR_STATUS Status=CrTargetGetPcieIo(&mPcieIo);
        if (CR_ERROR(Status)) return Status;
        break;
      }
      case CrDalDeviceClock:mDevices[Index].Data=CrTargetGetClockContext();break;
      case CrDalDeviceGpio:mDevices[Index].Data=CrTargetGetGpioContext();break;
      case CrDalDeviceInterconnect:mDevices[Index].Data=CrTargetGetInterconnectContext();break;
      case CrDalDeviceQup:mDevices[Index].Data=CrTargetGetQupContext();break;
      case CrDalDeviceSd:mDevices[Index].Data=CrTargetGetSdContext();break;
      case CrDalDeviceSmmu:mDevices[Index].Data=CrTargetGetSmmuContext();break;
      case CrDalDeviceUfs:mDevices[Index].Data=CrTargetGetUfsContext();break;
      default:break;
    }
  }
  *Manifest=&mManifest;return EFI_SUCCESS;
}
