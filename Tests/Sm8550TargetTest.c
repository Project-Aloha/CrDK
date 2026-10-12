/** SM8550 public resources through the real provider and portable cores.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrDalRegistryLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/PcdLib.h>
#include "../Library/GpioLib/gpio_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UINT32 Tlmm[0x300000 / 4], Pdc[0x10000 / 4], Spi[0x64 / 4];
static UINT32 Rsc[0x10000 / 4];
static UINT32 Writes, Registered, Unregistered;
static UINT32 *Word(UINTN Address) {
  assert((Address & 3U) == 0);
  if (Address >= 0x0f100000 && Address < 0x0f400000)
    return &Tlmm[(Address - 0x0f100000) / 4];
  if (Address >= 0x0b220000 && Address < 0x0b230000)
    return &Pdc[(Address - 0x0b220000) / 4];
  if (Address >= 0x174000f0 && Address < 0x17400154)
    return &Spi[(Address - 0x174000f0) / 4];
  if (Address >= 0x17a20000 && Address < 0x17a30000)
    return &Rsc[(Address - 0x17a20000) / 4];
  assert(!"Unexpected MMIO address"); return NULL;
}
UINT32 CrMmioRead32(UINTN Address) { return *Word(Address); }
UINT32 CrMmioWrite32(UINTN Address, UINT32 Value) {
  ++Writes; *Word(Address) = Value; return Value;
}
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {
  (VOID)Level; (VOID)Format;
}
VOID EFIAPI DebugAssert(CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description) {
  fprintf(stderr, "%s:%lu: %s\n", File, (unsigned long)Line, Description); assert(0);
}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *Value, UINT32 Compare, UINT32 Exchange) {
  __atomic_compare_exchange_n(Value, &Compare, Exchange, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return Compare;
}
SPIN_LOCK *EFIAPI InitializeSpinLock(SPIN_LOCK *Lock) { *Lock=1; return Lock; }
SPIN_LOCK *EFIAPI AcquireSpinLock(SPIN_LOCK *Lock) { assert(*Lock==1); *Lock=2; return Lock; }
SPIN_LOCK *EFIAPI ReleaseSpinLock(SPIN_LOCK *Lock) { assert(*Lock==2); *Lock=1; return Lock; }
VOID CrTestSleep(UINT64 Microseconds) { (VOID)Microseconds; }
CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  assert(Config->InterruptNumber == GIC_SPI(5) && Config->Handler != NULL);
  ++Registered; return CR_SUCCESS;
}
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *Config) {
  assert(Config->InterruptNumber == GIC_SPI(5)); ++Unregistered; return CR_SUCCESS;
}
static VOID TestManifest(VOID) {
  CONST CR_DAL_DEVICE_MANIFEST *Manifest = NULL, *Second = NULL;
  CONST CR_DAL_DEVICE_INFO *Device;
  assert(CrTargetGetDeviceManifest(NULL) == EFI_INVALID_PARAMETER);
  if (FixedPcdGet32(PcdUartSerialBase) == 0x0098c000) {
    assert(CrTargetGetDeviceManifest(&Manifest) == EFI_UNSUPPORTED && Manifest == NULL);
    assert(CrTargetGetDebugUartContext() == NULL && Writes == 0);
    return;
  }
  assert(CrTargetGetDeviceManifest(&Manifest) == EFI_SUCCESS);
  assert(CrDalRegistryValidate(Manifest) == EFI_SUCCESS && Writes == 0);
  assert(Manifest->DeviceCount == (FixedPcdGet32(PcdUartSerialBase) == 0 ? 4U : 5U));
  CR_DAL_DEVICE_INFO Saved[5];
  memcpy(Saved, Manifest->Devices, Manifest->DeviceCount*sizeof(*Saved));
  assert(CrTargetGetDeviceManifest(&Second) == EFI_SUCCESS && Second == Manifest);
  assert(memcmp(Saved, Manifest->Devices, Manifest->DeviceCount*sizeof(*Saved)) == 0);
  assert(CrDalRegistryGetDeviceInfo(Manifest, CrDalDeviceSocInfo, 0,
      CR_DAL_SOC_INFO_REVISION, sizeof(CR_DAL_SOC_INFO), &Device) == EFI_SUCCESS);
  CONST CR_DAL_SOC_INFO *Identity = Device->Data;
  assert(Identity->Revision == CR_DAL_SOC_INFO_REVISION && Identity->Size == sizeof(*Identity));
  assert(Identity->QualcommSocId == 519 && strcmp(Identity->Compatible, "qcom,sm8550") == 0);
  assert((Device->Attributes & CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE) != 0);
  for (UINT32 Type=CrDalDeviceRpmh; Type<CrDalDeviceMax; ++Type) {
    EFI_STATUS Status=CrDalRegistryGetDeviceInfo(Manifest,Type,0,0,0,&Device);
    if(Type==CrDalDeviceRpmh || Type==CrDalDeviceGpio || Type==CrDalDevicePdc || Type==CrDalDeviceSocInfo)
      assert(Status==EFI_SUCCESS);
    else if(Type==CrDalDeviceDebugUart && FixedPcdGet32(PcdUartSerialBase)!=0) {
      assert(Status==EFI_SUCCESS && Device->Data==CrTargetGetDebugUartContext());
      CrDebugUartContext *Uart=(VOID *)Device->Data;
      assert(Uart->BaseAddress==0x00a9c000 && Uart->InterruptConfig.InterruptNumber==GIC_SPI(579));
      assert(Uart->IsDebugUart && Uart->Type==DEBUG_UART_TYPE_GENI);
    } else assert(Status==EFI_NOT_FOUND);
  }
}
static VOID TestGpioPdc(VOID) {
  GpioDeviceContext *Gpio=CrTargetGetGpioContext();
  PdcDeviceContext *PdcContext=GetPdcDevContext();
  assert(GpioLibInit(&Gpio)==CR_SUCCESS && Gpio->PinCount==214 && Writes==0);
  assert(Gpio->InterruptConfig.InterruptNumber==GIC_SPI(208));
  UINT16 Routes=0;
  for(UINT16 Pin=0;Pin<214;++Pin) {
    GpioPinInfo *Info=&Gpio->GpioPins[Pin]; assert(!Info->Reserved);
    assert(Info->WakeRegOffset==MAX_UINT32 && Info->WakeBit==MAX_UINT8);
    if(Pin<210) {
      assert(GpioGetCfgRegByIndex(Gpio,GPIO_CFG_REG_TYPE_CTL_REG,Pin)==0x0f100000+(UINTN)Pin*0x1000);
      assert(Info->FuncMuxMap & 1U);
    }
    if(Info->PdcPinNumber!=GPIO_PDC_PIN_NUMBER_INVALID) {
      UINT16 Gic=0, Back=0;
      assert(PdcGetGicIrqFromPdcPin(PdcContext,Info->PdcPinNumber,&Gic)==CR_SUCCESS);
      assert(PdcGetPdcPinFromGicIrq(PdcContext,Gic,&Back)==CR_SUCCESS && Back==Info->PdcPinNumber);
      ++Routes;
    }
  }
  assert(Routes==93 && Gpio->EgpioFunction==GPIO_FUNC_9);
  assert(Gpio->GpioPins[0].PdcPinNumber==118 && Gpio->GpioPins[209].PdcPinNumber==122);
  assert(GpioGetCfgRegByIndex(Gpio,GPIO_CFG_REG_TYPE_CTL_REG,210)==0x0f1de000);
  assert(GpioGetCfgRegByIndex(Gpio,GPIO_CFG_REG_TYPE_IO_REG,210)==0x0f1de004);
  for(UINT16 Pin=211;Pin<214;++Pin)
    assert(GpioGetCfgRegByIndex(Gpio,GPIO_CFG_REG_TYPE_CTL_REG,Pin)==0x0f1d6000);
  GpioConfigParams Config;
  GpioInitConfigParams(&Config,0); Config.FunctionSel=GPIO_FUNC_1;
  Config.Pull=GPIO_PULL_NONE; Config.DriveStrength=GPIO_DRIVE_STRENGTH_2MA;
  Config.OutputValue=GPIO_VALUE_LOW;
  *Word(0x0f100000)=0x80002800; /* reserved bit31, strong-I2C bit13, EGPIO capable */
  assert(GpioApplyConfigChecked(Gpio,&Config)==CR_SUCCESS);
  assert((*Word(0x0f100000)&0x8000383c)==0x80003804);
  Config.FunctionSel=GPIO_FUNC_9;
  assert(GpioApplyConfigChecked(Gpio,&Config)==CR_SUCCESS);
  assert((*Word(0x0f100000)&0x1000)==0 && (*Word(0x0f100000)&0x3c)==4);
  GpioReadConfig(Gpio,&Config); assert(Config.FunctionSel==GPIO_FUNC_9);
  Config.FunctionSel=GPIO_FUNC_8; UINT32 Before=Writes;
  assert(GpioApplyConfigChecked(Gpio,&Config)==CR_INVALID_PARAMETER && Writes==Before);
  GpioInitConfigParams(&Config,210); Config.OutputEnable=TRUE; Config.OutputValue=GPIO_VALUE_HIGH;
  *Word(0x0f1de000)=0xa5a5001d;
  assert(GpioApplyConfigChecked(Gpio,&Config)==CR_UNSUPPORTED);
  GpioSetConfig(Gpio,&Config);
  assert(*Word(0x0f1de004)==1);
  assert(*Word(0x0f1de000)==0xa5a5001d);
  Before=Writes;
  for(UINT16 Pin=210;Pin<214;++Pin)
    assert(GpioSetInterruptCfg(Gpio,Pin,CR_INTERRUPT_TRIGGER_LEVEL_HIGH)==CR_UNSUPPORTED);
  assert(Writes==Before);
  for(UINT16 Pin=0;Pin<143;++Pin) {
    UINT16 Irq, Back;
    assert(PdcGetGicIrqFromPdcPin(PdcContext,Pin,&Irq)==CR_SUCCESS);
    assert(PdcGetPdcPinFromGicIrq(PdcContext,Irq,&Back)==CR_SUCCESS && Back==Pin);
  }
  UINT16 Result=0xffff;
  assert(PdcGetGicIrqFromPdcPin(PdcContext,138,&Result)==CR_SUCCESS && Result==GIC_SPI(251));
  assert(PdcGetGicIrqFromPdcPin(PdcContext,142,&Result)==CR_SUCCESS && Result==GIC_SPI(255));
  assert(PdcGetGicIrqFromPdcPin(PdcContext,143,&Result)==CR_NOT_FOUND);
  /* Exercise the inherited legacy enable layout through the real core.
     Actual hardware PDC version selection remains a separate milestone. */
  for(UINTN Bank=0;Bank<5;++Bank) *Word(0x0b220010+Bank*4)=MAX_UINT32;
  assert(PdcLibInit(&PdcContext)==CR_SUCCESS);
  for(UINTN Bank=0;Bank<4;++Bank) assert(*Word(0x0b220010+Bank*4)==0);
  assert(*Word(0x0b220020)==0xffff8000);
  BOOLEAN Enabled=FALSE;
  assert(PdcEnableInterrupt(PdcContext,142,TRUE)==CR_SUCCESS);
  assert(PdcCheckInterruptEnabled(PdcContext,142,&Enabled)==CR_SUCCESS && Enabled);
  assert(*Word(0x0b220020)==0xffffc000);
  assert(PdcEnableInterrupt(PdcContext,142,FALSE)==CR_SUCCESS);
  assert(PdcCheckInterruptEnabled(PdcContext,142,&Enabled)==CR_SUCCESS && !Enabled);
  Before=Writes;
  assert(PdcEnableInterrupt(PdcContext,143,TRUE)==CR_NOT_FOUND && Writes==Before);
  CR_INTERRUPT_TRIGGER_TYPE Type;
  *Word(0x0b220348)=0xa5000000; *Word(0x1740010c)=MAX_UINT32;
  assert(PdcGicSetType(PdcContext,142,CR_INTERRUPT_TRIGGER_EDGE_BOTH,&Type)==CR_SUCCESS);
  assert(Type==CR_INTERRUPT_TRIGGER_EDGE_RISING && *Word(0x0b220348)==0xa5000007);
  assert(*Word(0x1740010c)==0x7fffffff);
  assert(PdcGicSetType(PdcContext,142,CR_INTERRUPT_TRIGGER_LEVEL_LOW,&Type)==CR_SUCCESS);
  assert(Type==CR_INTERRUPT_TRIGGER_LEVEL_HIGH && *Word(0x1740010c)==MAX_UINT32);
  Before=Writes;
  assert(PdcGicSetType(PdcContext,143,CR_INTERRUPT_TRIGGER_LEVEL_HIGH,&Type)==CR_NOT_FOUND && Writes==Before);
}
static VOID TestRpmh(VOID) {
  RpmhDeviceContext *Context=CrTargetGetRpmhContext();
  assert(Context->drv_base_address==0x17a20000 && Context->drv_size==0x10000 && Context->drv_id==2);
  assert(Context->tcs_offset==0xd00 && Context->InterruptConfig.InterruptNumber==GIC_SPI(5));
  *Word(0x17a20000)=3U<<16; *Word(0x17a2000c)=(16U<<27)|(7U<<12);
  assert(RpmhLibInit(&Context)==CR_SUCCESS && Registered==1);
  assert(Context->tcs_config.active_tcs.mask==7 && Context->tcs_config.sleep_tcs.mask==0x18);
  assert(Context->tcs_config.wake_tcs.mask==0x60 && *Word(0x17a20d00)==7);
  /* Init touches the active IRQ registers only, never sleep/wake commands. */
  for(UINT32 Offset=0xd20;Offset<0x10000;Offset+=4) assert(Rsc[Offset/4]==0);
  assert(RpmhLibInit(&Context)==CR_SUCCESS && Registered==1);
  assert(RpmhLibDeinit(Context)==CR_SUCCESS && Unregistered==1 && *Word(0x17a20d00)==0);
}
int main(void) {
  TestManifest();
  if(FixedPcdGet32(PcdUartSerialBase)==0x0098c000) {
    puts("SM8550: unsupported inherited UART base rejected before publication or MMIO"); return 0;
  }
  TestGpioPdc(); TestRpmh();
  puts("SM8550: identity/manifest, 214 pads, 93 wake routes, PDC143 bounds and RPMh DRV2 shared cores passed");
  return 0;
}
