/** SM8350 HDK core data, Linux sm8350{,-hdk}.dts and Android lahaina HDK.
 *  SPDX-License-Identifier: MIT
 */
#include <Uefi.h>
#include <Library/pcie.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetMdssLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrTargetPmicGpioLib.h>
#include <Library/CrTargetButtonsLib.h>
#include <Library/CrTargetClockLib.h>
#include <Library/CrTargetInterconnectLib.h>
#include <Library/CrTargetSdLib.h>
#include <Library/CrTargetUfsLib.h>
#include <Library/CrTargetSmmuLib.h>
#include <Library/CrTargetPilLib.h>
#include <Library/CrTargetQupLib.h>
#include <Library/spmi.h>
#include "CrStorageTarget.h"

STATIC CrDebugUartContext mUart = {
  .BaseAddress = 0x0098c000,
  .InterruptConfig = {.InterruptNumber = GIC_SPI(604), .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .IsDebugUart = TRUE, .Type = DEBUG_UART_TYPE_GENI, .BaudRate = 115200,
};
/* SM8350 R11 Android HDK and mainline agree on DRV2 with 2/3/3 TCS.
   Only the two active TCS are written. The later SM8450 BSP Lahaina copy
   changed this to 3/2/2 plus fast-path; it is not this board reference. */
STATIC RpmhDeviceContext mRpmh = {
  .drv_base_address = 0x18220000, .drv_size = 0x10000, .drv_id = 2, .tcs_offset = 0xd00,
  .InterruptConfig = {.InterruptNumber = GIC_SPI(5), .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
  .tcs_config = {.active_tcs = {.tcs_count = 2}, .sleep_tcs = {.tcs_count = 3},
                .wake_tcs = {.tcs_count = 3}, .control_tcs = {.tcs_count = 0}},
};
STATIC PdcDeviceContext mPdc = {
  .BaseAddress = 0x0b220000, .Size = 0x10000, .SPIConfigBase = 0x17c000f0, .SPIConfigSize = 0x60,
  .PdcRangeCount = 13,
  .PdcRegion = {{0,480,40}, {40,140,14}, {54,263,1}, {55,306,4},
                {59,312,3}, {62,374,2}, {64,434,2}, {66,438,3},
                {69,86,1}, {70,520,54}, {124,609,31}, {155,63,1}, {156,716,12}},
};
STATIC CONST UINT16 mBridgePins[]={47,49,48};
STATIC CONST CR_DAL_LT9611_CONFIG mBridge={.I2cInstance=15,.I2cAddress=0x2b,
  .InterruptPin=50,.PowerPinCount=ARRAY_SIZE(mBridgePins),.PowerPins=mBridgePins};
STATIC CONST CR_DAL_CMD_DB_CONFIG mCmdDb = {.BaseAddress = 0x80860000, .Size = 0x20000};
STATIC SpmiDeviceContext mSpmi = {
  .Regions = {
    [SPMI_MEMORY_REGION_TYPE_CORE] = {0x0c440000,0x1100},
    [SPMI_MEMORY_REGION_TYPE_CH_SLAVES] = {0x0c600000,0x2000000},
    [SPMI_MEMORY_REGION_TYPE_OBSERVER] = {0x0e600000,0x100000},
    [SPMI_MEMORY_REGION_TYPE_INTERRUPT] = {0x0e700000,0xa0000},
    [SPMI_MEMORY_REGION_TYPE_CONFIG] = {0x0c40a000,0x26000}},
  .PdcPinNumber = 1, .ActiveEE = 0, .Channel = 0, .BusId = 0, .HasInterrupt = TRUE,
  .InterruptConfig = {.InterruptNumber = GIC_SPI(481), .TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH},
};
STATIC CONST PmicGpioTargetController mPmicControllers[] = {
  {"pm8350", "qcom,pm8350-gpio", 1, 0x8800, 10},
};
STATIC CONST PmicGpioTargetContext mPmicGpio = {mPmicControllers, ARRAY_SIZE(mPmicControllers)};
/* lahaina-hdk.dtsi explicitly includes lahaina-qrd-common.dtsi, which wires
   volume-up to PM8350 GPIO6. pmk8350.dtsi wires power/resin to IRQ7/6. */
STATIC CONST ButtonTarget mButtons[] = {
  {"power", BUTTON_SOURCE_PON, BUTTON_CODE_POWER, "pmk8350", 0,0x13,0,7,FALSE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-up", BUTTON_SOURCE_PMIC_GPIO, BUTTON_CODE_VOLUME_UP, "pm8350", 1,0x8d,6,0,TRUE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
  {"volume-down", BUTTON_SOURCE_PON, BUTTON_CODE_VOLUME_DOWN, "pmk8350", 0,0x13,0,6,FALSE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
};
STATIC CONST ButtonsTargetContext mButtonContext = {mButtons, ARRAY_SIZE(mButtons)};
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
STATIC CR_DAL_DEVICE_INFO mDevices[] = {
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceQup,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_QUP_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceMdss,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_MDSS_TARGET),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDevicePil,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_PIL_TARGET),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceUfs,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_UFS_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceSmmu,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CrTargetSmmuContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceRpmhArcInventory,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_DAL_RPMH_ARC_INVENTORY),NULL},
  ENTRY(CrDalDeviceDebugUart,mUart,MUT), ENTRY(CrDalDeviceRpmh,mRpmh,MUT),
  ENTRY(CrDalDevicePdc,mPdc,MUT), ENTRY(CrDalDeviceCmdDb,mCmdDb,IMM),
  ENTRY(CrDalDeviceSpmi,mSpmi,MUT), ENTRY(CrDalDevicePmicGpio,mPmicGpio,IMM),
  ENTRY(CrDalDeviceButtons,mButtonContext,IMM),
  ENTRY(CrDalDeviceLt9611,mBridge,IMM),
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceGpio,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(GpioDeviceContext),NULL},
  ENTRY(CrDalDeviceClockServices,mClockServices,IMM),
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceClock,0,
   CR_DAL_DATA_REVISION_1,MUT,sizeof(ClockDriverContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceInterconnect,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(InterconnectTargetContext),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceSd,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_TARGET_SD_CONTEXT),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceRpmhRails,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_DAL_RPMH_RAILS),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDeviceRpmhTopology,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(CR_DAL_RPMH_TOPOLOGY),NULL},
  {CR_DAL_DEVICE_INFO_REVISION,sizeof(CR_DAL_DEVICE_INFO),CrDalDevicePcie,0,
   CR_DAL_DATA_REVISION_1,IMM,sizeof(PcieTargetContext),NULL},
  ENTRY(CrDalDevicePcieIo,mPcieIo,IMM),
};
STATIC CONST CR_DAL_DEVICE_MANIFEST mManifest = {
  CR_DAL_DEVICE_MANIFEST_REVISION,sizeof(CR_DAL_DEVICE_MANIFEST),ARRAY_SIZE(mDevices),mDevices
};
EFI_STATUS CrTargetGetDeviceManifest(CONST CR_DAL_DEVICE_MANIFEST **Manifest)
{
  if (Manifest == NULL) return EFI_INVALID_PARAMETER;
  for (UINTN I=0;I<ARRAY_SIZE(mDevices);++I) {
    switch (mDevices[I].Type) {
      case CrDalDeviceQup:mDevices[I].Data=CrTargetGetQupContext();break;
      case CrDalDeviceMdss:mDevices[I].Data=CrTargetGetMdssContext();break;
      case CrDalDevicePcie:mDevices[I].Data=CrTargetGetPcieContext();break;
      case CrDalDevicePcieIo:{
        CR_STATUS Status=CrTargetGetPcieIo(&mPcieIo);
        if (CR_ERROR(Status)) return Status;
        break;
      }
      case CrDalDeviceUfs:mDevices[I].Data=CrTargetGetUfsContext();break;
      case CrDalDevicePil:mDevices[I].Data=CrSm8350GetPilTarget();break;
      case CrDalDeviceSmmu:mDevices[I].Data=CrTargetGetSmmuContext();break;
      case CrDalDeviceRpmhArcInventory:mDevices[I].Data=CrSm8350GetArcInventory();break;
      case CrDalDeviceGpio:mDevices[I].Data=CrTargetGetGpioContext();break;
      case CrDalDeviceClock:mDevices[I].Data=CrTargetGetClockContext();break;
      case CrDalDeviceInterconnect:mDevices[I].Data=CrTargetGetInterconnectContext();break;
      case CrDalDeviceSd:mDevices[I].Data=CrTargetGetSdContext();break;
      case CrDalDeviceRpmhRails:mDevices[I].Data=CrSm8350GetStorageRails();break;
      case CrDalDeviceRpmhTopology:mDevices[I].Data=CrSm8350GetStorageTopology();break;
      default:break;
    }
  }
  *Manifest = &mManifest;
  return EFI_SUCCESS;
}
