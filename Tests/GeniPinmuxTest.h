/** @file Pinmux failures must prevent starting an I2C transaction. SPDX-License-Identifier: MIT */
static EFI_STATUS PinLocateStatus;
static UINTN PinCalls;
static UINTN PinFailAt;
static EFI_STATUS EFIAPI TestConfigPin (EFI_GPIO_CR_PROTOCOL *This,
                                      GpioConfigParams *Config) {
  (VOID)This;
  assert (Config->PinNumber == 32 + PinCalls);
  /* pinctrl-sm8450.c: PINGROUP(32/33, qup9, ibi_i3c, ...), f1. */
  assert (Config->FunctionSel == GPIO_FUNC_1);
  ++PinCalls;
  return PinCalls == PinFailAt ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_GPIO_CR_PROTOCOL PinProtocol;
static EFI_STATUS EFIAPI LocatePins (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  assert (Guid == &gEfiGpioCrProtocolGuid);
  *Interface = EFI_ERROR (PinLocateStatus) ? NULL : &PinProtocol;
  return PinLocateStatus;
}
static void TestPinmux (void) {
  EFI_LOCATE_PROTOCOL Saved = BootServices.LocateProtocol;
  BootServices.LocateProtocol = LocatePins;
  PinLocateStatus = EFI_NOT_FOUND;
  assert (TEST_CONFIGURE_PINS () == EFI_NOT_FOUND);
  PinLocateStatus = EFI_SUCCESS;
  assert (TEST_CONFIGURE_PINS () == EFI_NOT_READY);
  PinProtocol.ConfigGpio = TestConfigPin;
  for (PinFailAt = 1; PinFailAt <= 2; ++PinFailAt) {
    PinCalls = 0;
    assert (TEST_CONFIGURE_PINS () == EFI_DEVICE_ERROR);
    assert (PinCalls == PinFailAt);
  }
  PinFailAt = PinCalls = 0;
  assert (TEST_CONFIGURE_PINS () == EFI_SUCCESS);
  assert (PinCalls == 2);
  BootServices.LocateProtocol = Saved;
}
