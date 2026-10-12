/** SM8450 GPIO address regression using production GPIO library and target. */
#include <assert.h>
#include <stdio.h>
#include <Uefi.h>
#include <Library/CrTargetGpioLib.h>
#include "../Library/GpioLib/gpio_internal.h"

/* Linux sm8450.dtsi TLMM 0xf100000 + pinctrl-sm8450.c offsets. */
#define UFS_CTL 0x0F1DE000U
#define UFS_IO  0x0F1DE004U
#define GPIO8_CTL 0x0F108000U
#define GPIO8_IO  0x0F108004U
#define SDC_CTL 0x0F1D6000U
static UINT32 UfsCtl, UfsIo, NormalCtl, NormalIo, SdcCtl;
static UINT32 EgpioCtl[3], EgpioIo[3];
static UINTN Writes;
UINT32 EFIAPI MmioRead32(UINTN Address) {
  if (Address >= 0x0F1CC000U && Address <= 0x0F1CF004U) {
    UINTN Pin = (Address - 0x0F100000U) / 0x1000U;
    assert(Pin == 204 || Pin == 206 || Pin == 207);
    UINTN Index = Pin == 204 ? 0 : Pin - 205;
    return (Address & 4U) ? EgpioIo[Index] : EgpioCtl[Index];
  }
  switch (Address) {
  case UFS_CTL: return UfsCtl;
  case UFS_IO: return UfsIo;
  case GPIO8_CTL: return NormalCtl;
  case GPIO8_IO: return NormalIo;
  case SDC_CTL: return SdcCtl;
  default: fprintf(stderr, "unexpected GPIO MMIO read: 0x%lx\n", (unsigned long)Address); assert(0); return 0;
  }
}
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value) {
  if (Address >= 0x0F1CC000U && Address <= 0x0F1CF004U) {
    UINTN Pin = (Address - 0x0F100000U) / 0x1000U;
    assert(Pin == 204 || Pin == 206 || Pin == 207);
    UINTN Index = Pin == 204 ? 0 : Pin - 205;
    if (Address & 4U) EgpioIo[Index] = Value;
    else EgpioCtl[Index] = Value;
    ++Writes;
    return Value;
  }
  switch (Address) {
  case UFS_CTL: UfsCtl = Value; break;
  case UFS_IO: UfsIo = Value; break;
  case GPIO8_CTL: NormalCtl = Value; break;
  case GPIO8_IO: NormalIo = Value; break;
  case SDC_CTL: SdcCtl = Value; break;
  default: fprintf(stderr, "unexpected GPIO MMIO write: 0x%lx\n", (unsigned long)Address); assert(0);
  }
  Writes++;
  return Value;
}
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (void)Level; (void)Format; }
int main(void) {
  GpioDeviceContext *Context = CrTargetGetGpioContext();
  GpioConfigParams Config;
  UINTN Address;
  assert(GpioLibInit(&Context) == CR_SUCCESS);
  Address = GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, 210);
  if (Address != UFS_CTL) fprintf(stderr, "UFS reset control: got 0x%lx expected 0x%lx\n", (unsigned long)Address, (unsigned long)UFS_CTL);
  assert(Address == UFS_CTL);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_IO_REG, 210) == UFS_IO);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, 8) == GPIO8_CTL);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_IO_REG, 8) == GPIO8_IO);
  for (UINT16 Pin = 211; Pin <= 213; Pin++)
    assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, Pin) == SDC_CTL);
  UfsCtl = 0xA5A5001D; UfsIo = 0x5A5A00F1;
  GpioInitConfigParams(&Config, 210);
  Config.OutputEnable = TRUE;
  Config.OutputValue = GPIO_VALUE_LOW;
  GpioSetConfig(Context, &Config);
  assert(UfsCtl == 0xA5A5001D); /* preserve unspecified pull/drive and reserved bits */
  assert(UfsIo == 0x5A5A00F0);
  assert(GpioReadOutputVal(Context, 210) == GPIO_VALUE_LOW);
  GpioReadConfig(Context, &Config);
  assert(Config.OutputEnable && Config.OutputValue == GPIO_VALUE_LOW);
  assert(Config.DriveStrength == 5 && Config.Pull == GPIO_PULL_UP);
  Config.OutputValue = GPIO_VALUE_HIGH;
  GpioSetConfig(Context, &Config);
  assert(UfsCtl == 0xA5A5001D && UfsIo == 0x5A5A00F1);
  assert(GpioReadOutputVal(Context, 210) == GPIO_VALUE_HIGH);
  GpioReadConfig(Context, &Config);
  assert(Config.OutputEnable && Config.OutputValue == GPIO_VALUE_HIGH);
  assert(Writes == 4);
  NormalCtl = 0x800001C3; NormalIo = 0xA4;
  GpioInitConfigParams(&Config, 8);
  Config.OutputEnable = TRUE; Config.OutputValue = GPIO_VALUE_HIGH;
  GpioSetConfig(Context, &Config);
  assert(NormalCtl == 0x800003C3 && NormalIo == 0xA6);
  assert(GpioReadOutputVal(Context, 8) == GPIO_VALUE_HIGH);
  assert(Writes == 6);
  Config.OutputEnable = FALSE;
  Config.DriveStrength = GPIO_DRIVE_STRENGTH_2MA;
  Config.Pull = GPIO_PULL_DOWN;
  NormalIo = 0xA5;
  GpioSetConfig(Context, &Config);
  assert(NormalCtl == 0x80000001 && NormalIo == 0xA5);
  GpioReadConfig(Context, &Config);
  assert(!Config.OutputEnable && Config.OutputValue == GPIO_VALUE_HIGH);
  assert(Config.Pull == GPIO_PULL_DOWN && Config.DriveStrength == GPIO_DRIVE_STRENGTH_2MA);
  assert(GpioReadInputVal(Context, 8) == GPIO_VALUE_HIGH);
  NormalIo &= ~1U;
  assert(GpioReadInputVal(Context, 8) == GPIO_VALUE_LOW);
  assert(Writes == 7);
  assert(GpioLibInit(&Context) == CR_SUCCESS);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, 210) == UFS_CTL);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_IO_REG, 210) == UFS_IO);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, 8) == GPIO8_CTL);
  assert(GpioGetCfgRegByIndex(Context, GPIO_CFG_REG_TYPE_CTL_REG, 211) == SDC_CTL);
  /* SDC2 clock has a shared control word and no IO register. */
  SdcCtl = 0xA5A50000;
  GpioInitConfigParams(&Config, 211);
  Config.Pull = GPIO_PULL_UP;
  Config.DriveStrength = 3;
  GpioSetConfig(Context, &Config);
  assert(SdcCtl == ((0xA5A50000U & ~0xC1C0U) | 0xC0C0U));
  GpioReadConfig(Context, &Config);
  assert(Config.Pull == GPIO_PULL_UP && Config.DriveStrength == 3);
  assert(!Config.OutputEnable);
  assert(GpioReadInputVal(Context, 211) == GPIO_VALUE_LOW);
  assert(GpioReadOutputVal(Context, 211) == GPIO_VALUE_LOW);
  assert(Writes == 8);
  /* Linux claims bit12 when bit11 advertises EGPIO capability. A mux
     readback alone does not prove AP ownership of the physical pin. */
  for (UINTN Index = 0; Index < 3; ++Index) {
    UINT16 Pin = Index == 0 ? 204 : (UINT16)(205 + Index);
    EgpioCtl[Index] = 0x80000800U | (2U << 2);
    GpioInitConfigParams(&Config, Pin);
    Config.FunctionSel = Index == 0 ? GPIO_FUNC_NORMAL : GPIO_FUNC_1;
    Config.OutputEnable = Index == 0;
    Config.OutputValue = Index == 0 ? GPIO_VALUE_HIGH : GPIO_VALUE_UNCHANGE;
    GpioSetConfig(Context, &Config);
    assert((EgpioCtl[Index] & 0x80001800U) == 0x80001800U);
    GpioReadConfig(Context, &Config);
    assert(Config.FunctionSel == (Index == 0 ? GPIO_FUNC_NORMAL : GPIO_FUNC_1));
    UINT32 Mux = EgpioCtl[Index] & 0x3CU;
    Config.FunctionSel=GPIO_FUNC_9;
    GpioSetConfig(Context,&Config);
    assert((EgpioCtl[Index]&0x1000U)==0 && (EgpioCtl[Index]&0x3CU)==Mux);
    GpioReadConfig(Context,&Config); assert(Config.FunctionSel==GPIO_FUNC_9);
    Config.FunctionSel=GPIO_FUNC_UNCHANGE;
    GpioSetConfig(Context,&Config);
    assert((EgpioCtl[Index]&0x1000U)==0); /* pad-only edits preserve ownership */
    Config.FunctionSel=GPIO_FUNC_1;
    GpioSetConfig(Context,&Config);
    assert((EgpioCtl[Index]&0x1000U)!=0);
    EgpioCtl[Index]=0x80000000U; /* non-EGPIO pin must preserve reserved bit12 */
    GpioSetConfig(Context,&Config);
    assert((EgpioCtl[Index]&0x1800U)==0 && (EgpioCtl[Index]&0x80000000U)!=0);
  }
  UINTN Before = Writes;
  BOOLEAN Enabled = TRUE;
  for (UINT16 Pin = 210; Pin <= 213; ++Pin) {
    assert(GpioEnableInterrupt(Context, Pin, TRUE) == CR_UNSUPPORTED);
    assert(GpioEnableInterrupt(Context, Pin, FALSE) == CR_UNSUPPORTED);
    assert(GpioMaskInterrupt(Context, Pin, TRUE) == CR_UNSUPPORTED);
    assert(GpioCheckInterruptEnabled(Context, Pin, &Enabled) == CR_UNSUPPORTED && !Enabled);
    assert(GpioSetInterruptCfg(Context, Pin, CR_INTERRUPT_TRIGGER_EDGE_RISING) == CR_UNSUPPORTED);
    assert(!GpioReadIrqStatus(Context, Pin));
    GpioCleanIrqStatus(Context, Pin);
  }
  assert(Writes == Before);
  assert(GpioCheckInterruptEnabled(Context, 8, NULL) == CR_INVALID_PARAMETER);
  assert(GpioSetInterruptCfg(NULL, 0, CR_INTERRUPT_TRIGGER_EDGE_RISING) == CR_INVALID_PARAMETER);
  assert(GpioSetInterruptCfg(Context, GPIO_PINS_MAX, CR_INTERRUPT_TRIGGER_EDGE_RISING) == CR_INVALID_PARAMETER);
  GpioInitConfigParams(NULL, 8);
  GpioInitConfigParams(&Config, 8); GpioConfigParams SavedConfig=Config;
  GpioConfigRegInfo *SavedRegister=Context->GpioPins[8].pRegCfg;
  Context->GpioPins[8].pRegCfg=NULL;
  GpioReadConfig(Context,&Config); GpioSetConfig(Context,&Config);
  assert(Config.PinNumber==SavedConfig.PinNumber && Config.OutputValue==SavedConfig.OutputValue);
  assert(GpioReadInputVal(Context,8)==GPIO_VALUE_LOW && GpioReadOutputVal(Context,8)==GPIO_VALUE_LOW);
  assert(Writes==Before);
  Context->GpioPins[8].pRegCfg=SavedRegister;
  UINT16 SavedCount=Context->PinCount; Context->PinCount=GPIO_PINS_MAX+1;
  GpioReadConfig(Context,&Config); GpioSetConfig(Context,&Config);
  assert(GpioReadInputVal(Context,8)==GPIO_VALUE_LOW && Writes==Before);
  Context->PinCount=SavedCount;
  puts("GPIO: UFS/SDC, EGPIO AP/remote handoff, unchanged ownership and reserved bits passed");
  return 0;
}
