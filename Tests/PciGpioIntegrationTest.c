/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#define NULL ((VOID *)0)
#include "../Library/PciHostBridgeLib/PciHostBridgeLib.c"
#include "../Driver/GpioCrDxe/GpioCrDxe.c"
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPcieLib.h>

STATIC UINT32 mCtl[211], mIo[211];
STATIC UINTN mExpectedPin;
STATIC UINT32 mExpectedLatch;
STATIC BOOLEAN mDropOutput;
STATIC BOOLEAN mDropOwner;

UINT32 EFIAPI MmioRead32 (UINTN Address)
{
  UINTN Pin = (Address - 0x0F100000U) / 0x1000U;
  assert (Address >= 0x0F100000U &&
          ((Pin >= 94 && Pin <= 99) || Pin == 80 || Pin == 204));
  assert ((Address & 0xFFFU) == 0 || (Address & 0xFFFU) == 4);
  return (Address & 4U) ? mIo[Pin] : mCtl[Pin];
}

UINT32 EFIAPI MmioWrite32 (UINTN Address, UINT32 Value)
{
  UINTN Pin = (Address - 0x0F100000U) / 0x1000U;
  assert (Pin == mExpectedPin);
  if ((Address & 0xFFFU) == 4) {
    if (!mDropOutput) {
      mIo[Pin] = Value;
    }
  } else {
    assert ((Address & 0xFFFU) == 0);
    if ((Value & BIT9) != 0 && !mDropOutput) {
      assert (((mIo[Pin] >> 1) & 1U) == mExpectedLatch);
    }
    mCtl[Pin] = mDropOwner ? Value & ~BIT12 : Value;
  }
  return Value;
}

BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (UINTN Level) { (VOID)Level; return TRUE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }
INTN EFIAPI AsciiStrCmp (CONST CHAR8 *Left, CONST CHAR8 *Right) { return strcmp (Left, Right); }
CR_STATUS GpioRegisterGpioIrq (GpioDeviceContext *Context, PdcDeviceContext *Pdc,
                             UINT16 Pin, CR_INTERRUPT_HANDLER Handler, VOID *Param,
                             CR_INTERRUPT_TRIGGER_TYPE Trigger)
{
  // The global protocol template retains this unused callback under ASan.
  assert (FALSE);
  return CR_UNSUPPORTED;
}
/* The expanded global protocol retains SD-only callbacks under ASan. PCI
 * never touches those four SD pins; fail rather than model their exclusion. */
CR_STATUS CrInterruptGuardAcquire (CR_INTERRUPT_GUARD *Guard)
{
  (VOID)Guard; assert (FALSE); return CR_UNSUPPORTED;
}
VOID CrInterruptGuardRelease (CR_INTERRUPT_GUARD *Guard)
{
  (VOID)Guard; assert (FALSE);
}

int main (void)
{
  EFI_GPIO_CR_PROTOCOL Gpio = {
    .Revision = EFI_GPIO_CR_PROTOCOL_REVISION,
    .InitGpioConfigParams = ProtocolGpioInitConfigParams,
    .ConfigGpio = ProtocolGpioConfig,
    .GetGpioConfig = ProtocolGpioGetConfig
  };
  PcieTargetContext *Pcie = CrTargetGetPcieContext ();
  mGpioContext = CrTargetGetGpioContext ();
  assert (GpioLibInit (&mGpioContext) == CR_SUCCESS);
  mPcieResources.Gpio = &Gpio;

  for (UINTN Index = 0; Index < Pcie->GpioCount; Index++) {
    CONST PcieTargetGpio *Pin = &Pcie->Gpios[Index];
    BOOLEAN Output = strcmp (Pin->Role, "perst") == 0;
    UINTN Slot = Pin->Pin;
    assert (Slot < ARRAY_SIZE (mCtl));
    mExpectedPin = Pin->Pin;
    mExpectedLatch = 0;
    mCtl[Slot] = 0x800009FFU; // inherited mux/drive/pull, remote EGPIO owner
    mIo[Slot] = 0xA6U; // stale high output latch
    assert (CranePcieSetGpio (NULL, Pin, Output ? PCIE_GPIO_OUTPUT : PCIE_GPIO_INPUT,
                             Output) == CR_SUCCESS);
    assert ((mCtl[Slot] & BIT31) != 0);
    assert ((mCtl[Slot] & (BIT11 | BIT12)) == (BIT11 | BIT12));
    assert (((mCtl[Slot] >> 6) & 7U) == 0); // 2 mA, including CLKREQ/WAKE
    assert (((mCtl[Slot] >> 2) & 15U) == Pin->Function);
    assert ((mCtl[Slot] & 3U) == (Output ? GPIO_PULL_DOWN : GPIO_PULL_UP));
    if (Output) {
      assert ((mIo[Slot] & 2U) == 0 && (mCtl[Slot] & BIT9) != 0);
      mExpectedLatch = 1;
      assert (CranePcieSetGpio (NULL, Pin, PCIE_GPIO_OUTPUT, FALSE) == CR_SUCCESS);
      assert ((mIo[Slot] & 2U) != 0 && (mCtl[Slot] & 3U) == GPIO_PULL_DOWN);
      mDropOutput = TRUE;
      assert (CranePcieSetGpio (NULL, Pin, PCIE_GPIO_OUTPUT, TRUE) == CR_DEVICE_ERROR);
      mDropOutput = FALSE;
    } else {
      assert (mIo[Slot] == 0xA6U && (mCtl[Slot] & BIT9) == 0);
      mIo[Slot] |= 1U;
      assert (CranePcieSetGpio (NULL, Pin, PCIE_GPIO_INPUT, FALSE) == CR_SUCCESS);
    }
  }
  CONST PcieTargetEndpointPower *Endpoint = Pcie->Controllers[0].EndpointPower;
  CONST PcieTargetGpio *WlanPins[] = { &Endpoint->EnableGpio, &Endpoint->XoGpio };
  for (UINTN Index=0; Index<ARRAY_SIZE(WlanPins); ++Index) {
    CONST PcieTargetGpio *Pin=WlanPins[Index];
    mExpectedPin=Pin->Pin;
    mCtl[Pin->Pin]=0x800009FFU;
    for (UINTN Active=0; Active<2; ++Active) {
      mExpectedLatch=(UINT32)Active;
      assert (CranePcieSetGpio(NULL,Pin,PCIE_GPIO_OUTPUT,Active)==CR_SUCCESS);
      assert ((mCtl[Pin->Pin]&BIT9)!=0 && (mCtl[Pin->Pin]&3U)==GPIO_PULL_DOWN);
      assert ((mCtl[Pin->Pin]&(BIT11|BIT12))==(BIT11|BIT12));
      assert (((mCtl[Pin->Pin]>>6)&7U)==7); // WCN WLAN/XO use 16mA
      assert (((mCtl[Pin->Pin]>>2)&15U)==0 && ((mIo[Pin->Pin]>>1)&1U)==Active);
    }
    mDropOutput=TRUE;
    assert (CranePcieSetGpio(NULL,Pin,PCIE_GPIO_OUTPUT,FALSE)==CR_DEVICE_ERROR);
    mDropOutput=FALSE;
    mExpectedLatch=0;
    mDropOwner=TRUE;
    assert (CranePcieSetGpio(NULL,Pin,PCIE_GPIO_OUTPUT,FALSE)==CR_DEVICE_ERROR);
    mDropOwner=FALSE;
    assert (CranePcieSetGpio(NULL,Pin,PCIE_GPIO_OUTPUT,FALSE)==CR_SUCCESS);
  }
  puts ("PCI GPIO integration passed: both ports, WLAN80/XO204 16mA, native protocol/registers, latch ordering and dropped writes");
  return 0;
}
