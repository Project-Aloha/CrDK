/* @file UsbKbHid.c
 * USB HID Keyboard Driver that manages USB keyboard and produces HID Keyboard Protocol.
 *
 * USB Keyboard Driver consumes USB I/O Protocol and Device Path Protocol, and produces
 * HID Keyboard Protocol on USB keyboard devices.
 * This module refers to following specifications:
 * 1. Universal Serial Bus HID Firmware Specification, ver 1.11
 * 2. Universal Serial Bus HID Usage Tables, ver 1.12
 * 3. UEFI Specification, v2.1
 *
 * Copyright (C) Microsoft Corporation. All rights reserved.
 * Portions derived from MdeModulePkg\Bus\Usb\UsbKbDxe which is:
 * Copyright (c) 2006 - 2018, Intel Corporation. All rights reserved.<BR>
 *
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "UsbKbHidDxe.h"

//
// USB Keyboard Driver Global Variables
//
EFI_DRIVER_BINDING_PROTOCOL  gUsbKbHidDriverBinding = {
  UsbKbHidDriverBindingSupported,
  UsbKbHidDriverBindingStart,
  UsbKbHidDriverBindingStop,
  USB_HID_KEYBOARD_DRIVER_VERSION,
  NULL,
  NULL
};

// Also retains partially started/stopped providers until cleanup can be retried.
STATIC LIST_ENTRY mUsbKbHidDevices = INITIALIZE_LIST_HEAD_VARIABLE (mUsbKbHidDevices);

STATIC
USB_KB_HID_DEV *
UsbKbHidFind (
  IN EFI_HANDLE Controller
  )
{
  LIST_ENTRY *Link;
  USB_KB_HID_DEV *Device;
  for (Link = GetFirstNode (&mUsbKbHidDevices);
       !IsNull (&mUsbKbHidDevices, Link);
       Link = GetNextNode (&mUsbKbHidDevices, Link)) {
    Device = CR (Link, USB_KB_HID_DEV, Link, USB_HID_KB_DEV_SIGNATURE);
    if (Device->ControllerHandle == Controller) return Device;
  }
  return NULL;
}

// USB control failures may synchronously disconnect this very provider.
// Pin it and the registered consumer until every enclosing call returns.
STATIC
EFI_STATUS
UsbKbHidEnter (
  IN USB_KB_HID_DEV *Device
  )
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  EFI_STATUS Status = EFI_NOT_READY;
  if (!Device->Stopping) {
    Device->ActiveCalls++;
    Status = EFI_SUCCESS;
  }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

STATIC
VOID
UsbKbHidLeave (
  IN USB_KB_HID_DEV *Device
  )
{
  EFI_TPL OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ASSERT (Device->ActiveCalls != 0);
  Device->ActiveCalls--;
  gBS->RestoreTPL (OldTpl);
}

STATIC
EFI_STATUS
UsbKbHidCleanup (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN USB_KB_HID_DEV *Device
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  ASSERT (Device->Stopping && Device->ActiveCalls == 0 && Device->CleanupActive);
  if (Device->DelayedRecoveryEvent != NULL) {
    Status = gBS->SetTimer (Device->DelayedRecoveryEvent, TimerCancel, 0);
    if (EFI_ERROR (Status)) goto Retained;
  }
  if (Device->AsyncActive) {
    Status = Device->UsbIo->UsbAsyncInterruptTransfer (
               Device->UsbIo, Device->IntEndpointDescriptor.EndpointAddress,
               FALSE, Device->IntEndpointDescriptor.Interval, 0, NULL, NULL);
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) goto Retained;
    Device->AsyncActive = FALSE;
  }
  if (Device->ProtocolInstalled) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
               Device->ControllerHandle, &gHidKeyboardProtocolGuid,
               &Device->HidKeyboard, NULL);
    if (EFI_ERROR (Status)) goto Retained;
    Device->ProtocolInstalled = FALSE;
  }
  if (Device->DelayedRecoveryEvent != NULL) {
    Status = gBS->CloseEvent (Device->DelayedRecoveryEvent);
    if (EFI_ERROR (Status)) goto Retained;
    Device->DelayedRecoveryEvent = NULL;
  }
  if (Device->UsbIoOpened) {
    Status = gBS->CloseProtocol (Device->ControllerHandle, &gEfiUsbIoProtocolGuid,
                                This->DriverBindingHandle, Device->ControllerHandle);
    if (EFI_ERROR (Status)) goto Retained;
    Device->UsbIoOpened = FALSE;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  RemoveEntryList (&Device->Link);
  gBS->RestoreTPL (OldTpl);
  if (Device->ControllerNameTable != NULL) FreeUnicodeStringTable (Device->ControllerNameTable);
  FreePool (Device);
  return EFI_SUCCESS;

Retained:
  Device->CleanupActive = FALSE;
  return Status;
}

/**
  Entrypoint of USB HID Keyboard Driver.

  This function is the entrypoint of the USB HID Keyboard Driver. It installs Driver Binding
  Protocols together with Component Name Protocols.

  @param  ImageHandle       The firmware allocated handle for the EFI image.
  @param  SystemTable       A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.

**/
EFI_STATUS
EFIAPI
UsbKbHidDriverBindingEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gUsbKbHidDriverBinding,
             ImageHandle,
             &gUsbKbHidComponentName,
             &gUsbKbHidComponentName2
             );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}

/**
  Check whether USB HID keyboard driver supports this device.

  @param  This                   The USB keyboard driver binding protocol.
  @param  Controller             The controller handle to check.
  @param  RemainingDevicePath    The remaining device path.

  @retval EFI_SUCCESS            The driver supports this controller.
  @retval other                  This device isn't supported.

**/
EFI_STATUS
EFIAPI
UsbKbHidDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS           Status;
  EFI_USB_IO_PROTOCOL  *UsbIo;

  //
  // Check if USB I/O Protocol is attached on the controller handle.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Use the USB I/O Protocol interface to check whether Controller is
  // a keyboard device that can be managed by this driver.
  //
  Status = EFI_SUCCESS;

  if (!IsUSBKeyboard (UsbIo)) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (
         Controller,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

/**
  Starts the keyboard device with this driver.

  This function produces Simple Text Input Protocol and Simple Text Input Ex Protocol,
  initializes the keyboard device, and submit Asynchronous Interrupt Transfer to manage
  this keyboard device.

  @param  This                   The USB keyboard driver binding instance.
  @param  Controller             Handle of device to bind driver to.
  @param  RemainingDevicePath    Optional parameter use to pick a specific child
                                 device to start.

  @retval EFI_SUCCESS            The controller is controlled by the usb keyboard driver.
  @retval EFI_UNSUPPORTED        No interrupt endpoint can be found.
  @retval Other                  This controller cannot be started.

**/
EFI_STATUS
EFIAPI
UsbKbHidDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS                   Status;
  EFI_USB_IO_PROTOCOL          *UsbIo;
  USB_KB_HID_DEV               *UsbKeyboardDevice;
  UINT8                        EndpointNumber;
  EFI_USB_ENDPOINT_DESCRIPTOR  EndpointDescriptor;
  UINT8                        Index;
  UINT8                        EndpointAddr;
  UINT8                        PollingInterval;
  UINTN                        PacketSize;
  BOOLEAN                      Found;
  EFI_TPL                      OldTpl;
  EFI_TPL                      RegistryTpl;
  EFI_STATUS                   CleanupStatus;

  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);
  // Allocate the retry context before acquiring any protocol ownership.
  UsbKeyboardDevice = AllocateZeroPool (sizeof (USB_KB_HID_DEV));
  if (UsbKeyboardDevice == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExitNoUsbIo;
  }
  //
  // Open USB I/O Protocol
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **)&UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    FreePool (UsbKeyboardDevice);
    goto ErrorExitNoUsbIo;
  }

  UsbKeyboardDevice->Signature = USB_HID_KB_DEV_SIGNATURE;
  UsbKeyboardDevice->ControllerHandle = Controller;
  UsbKeyboardDevice->UsbIo = UsbIo;
  UsbKeyboardDevice->UsbIoOpened = TRUE;
  UsbKeyboardDevice->ActiveCalls = 1;
  RegistryTpl = gBS->RaiseTPL (TPL_NOTIFY);
  InsertTailList (&mUsbKbHidDevices, &UsbKeyboardDevice->Link);
  gBS->RestoreTPL (RegistryTpl);

  //
  // Get the Device Path Protocol on Controller's handle
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&UsbKeyboardDevice->DevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  //
  // Report that the USB keyboard is being enabled
  //
  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE,
    (EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_ENABLE),
    UsbKeyboardDevice->DevicePath
    );

  //
  // This is pretty close to keyboard detection, so log progress
  //
  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE,
    (EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_PRESENCE_DETECT),
    UsbKeyboardDevice->DevicePath
    );

  UsbKeyboardDevice->UsbIo = UsbIo;

  //
  // Get interface & endpoint descriptor
  //
  Status = UsbIo->UsbGetInterfaceDescriptor (
           UsbIo,
           &UsbKeyboardDevice->InterfaceDescriptor
           );

  if (EFI_ERROR (Status)) goto ErrorExit;

  EndpointNumber = UsbKeyboardDevice->InterfaceDescriptor.NumEndpoints;

  //
  // Traverse endpoints to find interrupt endpoint IN
  //
  Found = FALSE;
  for (Index = 0; Index < EndpointNumber; Index++) {
    Status = UsbIo->UsbGetEndpointDescriptor (
             UsbIo,
             Index,
             &EndpointDescriptor
             );

    if (EFI_ERROR (Status)) goto ErrorExit;

    if (((EndpointDescriptor.Attributes & (BIT0 | BIT1)) == USB_ENDPOINT_INTERRUPT) &&
        ((EndpointDescriptor.EndpointAddress & USB_ENDPOINT_DIR_IN) != 0))
    {
      //
      // We only care interrupt endpoint here
      //
      CopyMem (&UsbKeyboardDevice->IntEndpointDescriptor, &EndpointDescriptor, sizeof (EndpointDescriptor));
      Found = TRUE;
      break;
    }
  }

  if (!Found) {
    //
    // Report Status Code to indicate that there is no USB keyboard
    //
    REPORT_STATUS_CODE (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      (EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_NOT_DETECTED)
      );
    //
    // No interrupt endpoint found, then return unsupported.
    //
    Status = EFI_UNSUPPORTED;
    DEBUG ((DEBUG_ERROR, "[%a] - failed to locate keyboard interrupt endpoint: %r.\n", __FUNCTION__, Status));
    goto ErrorExit;
  }

  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE,
    (EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_DETECTED),
    UsbKeyboardDevice->DevicePath
    );

  UsbKeyboardDevice->Signature                                       = USB_HID_KB_DEV_SIGNATURE;
  UsbKeyboardDevice->HidKeyboard.RegisterKeyboardHidReportCallback   = RegisterKeyboardHidReportCallback;
  UsbKeyboardDevice->HidKeyboard.UnRegisterKeyboardHidReportCallback = UnRegisterKeyboardHidReportCallback;
  UsbKeyboardDevice->HidKeyboard.SetOutputReport                     = SetOutputReport;

  UsbKeyboardDevice->ControllerHandle = Controller;

  Status = InitUsbKeyboard (UsbKeyboardDevice);
  if (EFI_ERROR (Status)) {
    //
    // Report Status Code to indicate keyboard init failure
    //
    REPORT_STATUS_CODE (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      (EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_NOT_CONFIGURED)
      );
    //
    DEBUG ((DEBUG_ERROR, "[%a] - failed to initialize keyboard: %r.\n", __FUNCTION__, Status));
    goto ErrorExit;
  }

  //
  // Submit Asynchronous Interrupt Transfer to manage this device.
  //
  EndpointAddr    = UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress;
  PollingInterval = UsbKeyboardDevice->IntEndpointDescriptor.Interval;
  PacketSize      = UsbKeyboardDevice->IntEndpointDescriptor.MaxPacketSize;

  UsbKeyboardDevice->AsyncActive = TRUE;
  Status = UsbIo->UsbAsyncInterruptTransfer (
                    UsbIo,
                    EndpointAddr,
                    TRUE,
                    PollingInterval,
                    PacketSize,
                    KeyboardHandler,
                    UsbKeyboardDevice
                    );
  if (EFI_ERROR (Status)) {
    //
    // Report Status Code to indicate keyboard init failure
    //
    REPORT_STATUS_CODE (
      EFI_ERROR_CODE | EFI_ERROR_MINOR,
      (EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_CONTROLLER_ERROR)
      );
    DEBUG ((DEBUG_ERROR, "[%a] - failed to initialize keyboard interrupt handler: %r.\n", __FUNCTION__, Status));
    goto ErrorExit;
  }

  //
  // Install HID USB keyboard device.
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Controller,
                  &gHidKeyboardProtocolGuid,
                  &UsbKeyboardDevice->HidKeyboard,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  UsbKeyboardDevice->ProtocolInstalled = TRUE;
  UsbKeyboardDevice->ControllerNameTable = NULL;
  AddUnicodeString2 (
    "eng",
    gUsbKbHidComponentName.SupportedLanguages,
    &UsbKeyboardDevice->ControllerNameTable,
    L"Generic USB HID Keyboard",
    TRUE
    );
  AddUnicodeString2 (
    "en",
    gUsbKbHidComponentName2.SupportedLanguages,
    &UsbKeyboardDevice->ControllerNameTable,
    L"Generic USB HID Keyboard",
    FALSE
    );

  UsbKbHidLeave (UsbKeyboardDevice);
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;

  //
  // Error handler
  //
ErrorExit:
  if (UsbKeyboardDevice != NULL) {
    UsbKeyboardDevice->Stopping = TRUE;
    UsbKeyboardDevice->CleanupActive = TRUE;
    UsbKbHidLeave (UsbKeyboardDevice);
    CleanupStatus = UsbKbHidCleanup (This, UsbKeyboardDevice);
    if (EFI_ERROR (CleanupStatus)) {
      DEBUG ((DEBUG_ERROR, "USB keyboard: start cleanup retained: %r\n", CleanupStatus));
    }
  }

ErrorExitNoUsbIo:
  gBS->RestoreTPL (OldTpl);

  return Status;
}

/**
  Stop the USB keyboard device handled by this driver.

  @param  This                   The USB keyboard driver binding protocol.
  @param  Controller             The controller to release.
  @param  NumberOfChildren       The number of handles in ChildHandleBuffer.
  @param  ChildHandleBuffer      The array of child handle.

  @retval EFI_SUCCESS            The device was stopped.
  @retval EFI_UNSUPPORTED        Simple Text In Protocol or Simple Text In Ex Protocol
                                 is not installed on Controller.
  @retval EFI_DEVICE_ERROR       The device could not be stopped due to a device error.
  @retval Others                 Fail to uninstall protocols attached on the device.

**/
EFI_STATUS
EFIAPI
UsbKbHidDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  )
{
  EFI_TPL OldTpl;
  USB_KB_HID_DEV *Device;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Device = UsbKbHidFind (Controller);
  if (Device == NULL) {
    gBS->RestoreTPL (OldTpl);
    return EFI_UNSUPPORTED;
  }
  if (Device->ActiveCalls != 0 || Device->CleanupActive) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  Device->Stopping = TRUE;
  Device->CleanupActive = TRUE;
  gBS->RestoreTPL (OldTpl);
  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE, EFI_PERIPHERAL_KEYBOARD | EFI_P_PC_DISABLE, Device->DevicePath);
  return UsbKbHidCleanup (This, Device);
}

//
// HID Keyboard Protocol functions
//

/**
  This function registers a callback function to occur whenever there is a HID Keyboard Report packet available.
  Note: Only one callback registration is permitted.

  @param  This                    - pointer to the current driver instance that this callback is being registered with.
  @param  KeyboardReportCallback  - Keyboard HID Report Callback function (see: KEYBOARD_HID_REPORT_CALLBACK)
  @param  Context                 - pointer to context (if any) that should be included when the callback is invoked.

  @retval EFI_SUCCESS         - Successfully registered the callback.
  @retval EFI_ALREADY_STARTED - Another callback is already registered.
  @retval other               - there was an implementation specific failure registering the callback.
**/
EFI_STATUS
EFIAPI
RegisterKeyboardHidReportCallback (
  IN HID_KEYBOARD_PROTOCOL         *This,
  IN KEYBOARD_HID_REPORT_CALLBACK  KeyboardReportCallback,
  IN VOID                          *Context
  )
{
  USB_KB_HID_DEV  *HidKeyboard;
  EFI_TPL OldTpl;

  if ((This == NULL) || (KeyboardReportCallback == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboard = USB_KB_HID_DEV_FROM_THIS (This);

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (HidKeyboard->Stopping) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  if (HidKeyboard->KeyReportCallback != NULL) {
    gBS->RestoreTPL (OldTpl);
    return EFI_ALREADY_STARTED;
  }

  HidKeyboard->KeyReportCallback        = KeyboardReportCallback;
  HidKeyboard->KeyReportCallbackContext = Context;

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

/**
  This function unregisters a previously registered keyboard HID report callback function.
  Note: Only one callback registration is permitted.

  @param  This  - pointer to the current driver instance that this callback is being unregistered from.

  @retval EFI_SUCCESS     - Successfully unregistered the callback.
  @retval EFI_NOT_FOUND   - No previously registered callback.
  @retval other           - there was an implementation specific failure unregistering the callback.
**/
EFI_STATUS
EFIAPI
UnRegisterKeyboardHidReportCallback (
  IN HID_KEYBOARD_PROTOCOL  *This
  )
{
  USB_KB_HID_DEV  *HidKeyboard;
  EFI_TPL OldTpl;

  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboard = USB_KB_HID_DEV_FROM_THIS (This);

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (HidKeyboard->ActiveCalls != 0) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  if (HidKeyboard->KeyReportCallback == NULL) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_FOUND;
  }

  HidKeyboard->KeyReportCallback        = NULL;
  HidKeyboard->KeyReportCallbackContext = NULL;

  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

/**
  This function sends an Ouput Report HID packet to the hardware layer.

  @param  This                      - pointer to the current driver instance.
  @param  Interface                 - defines the format of the HID report.
  @param  HidOuputReportBuffer      - pointer to the HID ouput report buffer.
  @param  HidOuputReportBufferSize  - Size of the HidOutputReportBuffer

  @retval EFI_SUCCESS     - Output Report successfully transmitted to the hardware.
  @retval other           - there was an implementation specific failure transmitting the output report.
**/
EFI_STATUS
EFIAPI
SetOutputReport (
  IN HID_KEYBOARD_PROTOCOL   *This,
  IN KEYBOARD_HID_INTERFACE  Interface,
  IN UINT8                   *HidOutputReportBuffer,
  IN UINTN                   HidOutputReportBufferSize
  )
{
  EFI_STATUS      Status;
  UINT8           ReportId;
  USB_KB_HID_DEV  *HidKeyboard;

  if (Interface != BootKeyboard) {
    DEBUG ((DEBUG_ERROR, "[%a] - Unsupported HID report interface %d\n", __FUNCTION__, Interface));
    return EFI_UNSUPPORTED;
  }

  if ((This == NULL) || (HidOutputReportBuffer == NULL)) return EFI_INVALID_PARAMETER;

  if (HidOutputReportBufferSize != 1) {
    return EFI_UNSUPPORTED;
  }

  HidKeyboard = USB_KB_HID_DEV_FROM_THIS (This);

  Status = UsbKbHidEnter (HidKeyboard);
  if (EFI_ERROR (Status)) return Status;
  // HID 1.11 Appendix B boot keyboard reports do not have report IDs.
  ReportId = 0;

  //
  // Call Set_Report Request to lighten the LED.
  //
  Status = UsbSetReportRequest (
             HidKeyboard->UsbIo,
             HidKeyboard->InterfaceDescriptor.InterfaceNumber,
             ReportId,
             HID_OUTPUT_REPORT,
             1,
             HidOutputReportBuffer
             );

  UsbKbHidLeave (HidKeyboard);
  return Status;
}

//
// Module-global utility functions
//

/**
  Uses USB I/O to check whether the device is a USB keyboard device.

  @param  UsbIo    Pointer to a USB I/O protocol instance.

  @retval TRUE     Device is a USB keyboard device.
  @retval FALSE    Device is a not USB keyboard device.

**/
BOOLEAN
IsUSBKeyboard (
  IN  EFI_USB_IO_PROTOCOL  *UsbIo
  )
{
  EFI_STATUS                    Status;
  EFI_USB_INTERFACE_DESCRIPTOR  InterfaceDescriptor;

  //
  // Get the default interface descriptor
  //
  Status = UsbIo->UsbGetInterfaceDescriptor (
                    UsbIo,
                    &InterfaceDescriptor
                    );

  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if ((InterfaceDescriptor.InterfaceClass == CLASS_HID) &&
      (InterfaceDescriptor.InterfaceSubClass == SUBCLASS_BOOT) &&
      (InterfaceDescriptor.InterfaceProtocol == PROTOCOL_KEYBOARD)
      )
  {
    return TRUE;
  }

  return FALSE;
}

/**
  Initialize USB keyboard device and all private data structures.

  @param  UsbKeyboardDevice  The USB_KB_DEV instance.

  @retval EFI_SUCCESS        Initialization is successful.
  @retval EFI_DEVICE_ERROR   Keyboard initialization failed.

**/
EFI_STATUS
InitUsbKeyboard (
  IN OUT USB_KB_HID_DEV  *UsbKeyboardDevice
  )
{
  UINT16      ConfigValue;
  EFI_STATUS  Status;
  UINT32      TransferResult;

  REPORT_STATUS_CODE_WITH_DEVICE_PATH (
    EFI_PROGRESS_CODE,
    (EFI_PERIPHERAL_KEYBOARD | EFI_P_KEYBOARD_PC_SELF_TEST),
    UsbKeyboardDevice->DevicePath
    );

  //
  // Use the config out of the descriptor
  // Assumed the first config is the correct one and this is not always the case
  //
  Status = UsbGetConfiguration (
             UsbKeyboardDevice->UsbIo,
             &ConfigValue,
             &TransferResult
             );
  if (EFI_ERROR (Status)) {
    ConfigValue = 0x01;
    //
    // Uses default configuration to configure the USB Keyboard device.
    //
    Status = UsbSetConfiguration (
               UsbKeyboardDevice->UsbIo,
               ConfigValue,
               &TransferResult
               );
    if (EFI_ERROR (Status)) {
      //
      // If configuration could not be set here, it means
      // the keyboard interface has some errors and could
      // not be initialized
      //
      REPORT_STATUS_CODE_WITH_DEVICE_PATH (
        EFI_ERROR_CODE | EFI_ERROR_MINOR,
        (EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_INTERFACE_ERROR),
        UsbKeyboardDevice->DevicePath
        );

      return EFI_DEVICE_ERROR;
    }
  }

  // HID 1.11 7.2.6: explicitly establish the protocol at initialization.
  // This provider only decodes boot reports.
  Status = UsbSetProtocolRequest (
             UsbKeyboardDevice->UsbIo,
             UsbKeyboardDevice->InterfaceDescriptor.InterfaceNumber,
             BOOT_PROTOCOL
             );
  if (EFI_ERROR (Status)) return Status;

  // HID 1.11 7.2.4 and F.3: duration zero requests reports on change for
  // all input reports. Linux usbhid establishes this before polling too.
  // Keep the request best effort, as Linux does, for nonconforming devices.
  Status = UsbSetIdleRequest (
             UsbKeyboardDevice->UsbIo,
             UsbKeyboardDevice->InterfaceDescriptor.InterfaceNumber,
             0,
             0
             );
  DEBUG ((DEBUG_WARN,
          "UsbKbHid: boot interface=%u SET_IDLE(0,0)=%r ep=%02x packet=%u interval=%u\n",
          UsbKeyboardDevice->InterfaceDescriptor.InterfaceNumber,
          Status,
          UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress,
          UsbKeyboardDevice->IntEndpointDescriptor.MaxPacketSize,
          UsbKeyboardDevice->IntEndpointDescriptor.Interval));

  //
  // Create event for delayed recovery, which deals with device error.
  //
  if (UsbKeyboardDevice->DelayedRecoveryEvent != NULL) {
    Status = gBS->CloseEvent (UsbKeyboardDevice->DelayedRecoveryEvent);
    if (EFI_ERROR (Status)) return Status;
    UsbKeyboardDevice->DelayedRecoveryEvent = NULL;
  }

  Status = gBS->CreateEvent (
         EVT_TIMER | EVT_NOTIFY_SIGNAL,
         TPL_NOTIFY,
         UsbKbHidRecoveryHandler,
         UsbKeyboardDevice,
         &UsbKeyboardDevice->DelayedRecoveryEvent
         );

  return Status;
}

/**
  Handler function for USB keyboard's asynchronous interrupt transfer.

  This function is the handler function for USB keyboard's asynchronous interrupt transfer
  to manage the keyboard. It parses the USB keyboard input report, and inserts data to
  keyboard buffer according to state of modifier keys and normal keys. Timer for repeat key
  is also set accordingly.

  @param  Data             A pointer to a buffer that is filled with key data which is
                           retrieved via asynchronous interrupt transfer.
  @param  DataLength       Indicates the size of the data buffer.
  @param  Context          Pointing to USB_KB_DEV instance.
  @param  Result           Indicates the result of the asynchronous interrupt transfer.

  @retval EFI_SUCCESS      Asynchronous interrupt transfer is handled successfully.
  @retval EFI_DEVICE_ERROR Hardware error occurs.

**/
EFI_STATUS
EFIAPI
KeyboardHandler (
  IN  VOID    *Data,
  IN  UINTN   DataLength,
  IN  VOID    *Context,
  IN  UINT32  Result
  )
{
  USB_KB_HID_DEV       *UsbKeyboardDevice;
  EFI_USB_IO_PROTOCOL  *UsbIo;
  UINT32               UsbStatus;
  EFI_STATUS           Status;
  UINT8                EmptyKeyPacket[8];

  ASSERT (Context != NULL);

  UsbKeyboardDevice = (USB_KB_HID_DEV *)Context;
  if (EFI_ERROR (UsbKbHidEnter (UsbKeyboardDevice))) return EFI_NOT_READY;
  UsbIo             = UsbKeyboardDevice->UsbIo;

  //
  // Analyzes Result and performs corresponding action.
  //
  if (Result != EFI_USB_NOERROR) {
    // Report and release held keys once per error episode. A failed endpoint
    // must not start a new unbounded series of host-controller retries each
    // time the class recovery timer submits a fresh interrupt transfer.
    if (UsbKeyboardDevice->ConsecutiveErrors == 0) {
      REPORT_STATUS_CODE_WITH_DEVICE_PATH (
        EFI_ERROR_CODE | EFI_ERROR_MINOR,
        (EFI_PERIPHERAL_KEYBOARD | EFI_P_EC_INPUT_ERROR),
        UsbKeyboardDevice->DevicePath
        );
      ZeroMem (EmptyKeyPacket, sizeof (EmptyKeyPacket));
      if (UsbKeyboardDevice->KeyReportCallback != NULL) {
        UsbKeyboardDevice->KeyReportCallback (
                             BootKeyboard,
                             EmptyKeyPacket,
                             sizeof (EmptyKeyPacket),
                             UsbKeyboardDevice->KeyReportCallbackContext
                             );
      }
    }
    if (UsbKeyboardDevice->ConsecutiveErrors < USB_HID_KB_RECOVERY_LIMIT) {
      UsbKeyboardDevice->ConsecutiveErrors++;
    }

    if ((Result & EFI_USB_ERR_STALL) == EFI_USB_ERR_STALL) {
      UsbClearEndpointHalt (
        UsbIo,
        UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress,
        &UsbStatus
        );
    }

    //
    // Delete & Submit this interrupt again
    // Handler of DelayedRecoveryEvent triggered by timer will re-submit the interrupt.
    //
    Status = UsbIo->UsbAsyncInterruptTransfer (
             UsbIo,
             UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress,
             FALSE,
             0,
             0,
             NULL,
             NULL
             );
    if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
      UsbKbHidLeave (UsbKeyboardDevice);
      return Status;
    }
    UsbKeyboardDevice->AsyncActive = FALSE;
    if (UsbKeyboardDevice->ConsecutiveErrors >= USB_HID_KB_RECOVERY_LIMIT) {
      // UsbPortReset removes this interface's configuration and may reenter
      // Stop, so it cannot run from this pinned callback. Leave the endpoint
      // cancelled; bus disconnect/re-enumeration creates a fresh provider.
      DEBUG ((DEBUG_ERROR, "USB keyboard: interrupt recovery exhausted after %u errors; endpoint 0x%02x stopped pending re-enumeration\n",
              UsbKeyboardDevice->ConsecutiveErrors,
              UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress));
      UsbKbHidLeave (UsbKeyboardDevice);
      return EFI_DEVICE_ERROR;
    }
    //
    // EFI_USB_INTERRUPT_DELAY is defined in USB standard for error handling.
    //
    Status = gBS->SetTimer (
                   UsbKeyboardDevice->DelayedRecoveryEvent,
                   TimerRelative,
                   EFI_USB_INTERRUPT_DELAY
                   );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "USB keyboard: cannot schedule interrupt recovery: %r\n", Status));
    }

    UsbKbHidLeave (UsbKeyboardDevice);
    return EFI_DEVICE_ERROR;
  }

  UsbKeyboardDevice->ConsecutiveErrors = 0;

  //
  // If no error and no data, just return EFI_SUCCESS.
  //
  if ((DataLength == 0) || (Data == NULL)) {
    UsbKbHidLeave (UsbKeyboardDevice);
    return EFI_SUCCESS;
  }

  //
  // Send the data up to the HID layer via means of the KeyReportCallback.
  //
  if (UsbKeyboardDevice->KeyReportCallback != NULL) {
    UsbKeyboardDevice->KeyReportCallback (
                         BootKeyboard,
                         (UINT8 *)Data,
                         DataLength,
                         UsbKeyboardDevice->KeyReportCallbackContext
                         );
  }

  UsbKbHidLeave (UsbKeyboardDevice);
  return EFI_SUCCESS;
}

/**
  Handler for Delayed Recovery event.

  This function is the handler for Delayed Recovery event triggered
  by timer.
  After a device error occurs, the event would be triggered
  with interval of EFI_USB_INTERRUPT_DELAY. EFI_USB_INTERRUPT_DELAY
  is defined in USB standard for error handling.

  @param  Event              The Delayed Recovery event.
  @param  Context            Points to the USB_KB_HID_DEV instance.

**/
VOID
EFIAPI
UsbKbHidRecoveryHandler (
  IN    EFI_EVENT  Event,
  IN    VOID       *Context
  )
{
  USB_KB_HID_DEV       *UsbKeyboardDevice;
  EFI_USB_IO_PROTOCOL  *UsbIo;
  UINTN                PacketSize;
  EFI_STATUS           Status;

  UsbKeyboardDevice = (USB_KB_HID_DEV *)Context;

  ASSERT (Context != NULL);

  if (EFI_ERROR (UsbKbHidEnter (UsbKeyboardDevice))) return;
  if (UsbKeyboardDevice->AsyncActive ||
      UsbKeyboardDevice->ConsecutiveErrors >= USB_HID_KB_RECOVERY_LIMIT) {
    UsbKbHidLeave (UsbKeyboardDevice);
    return;
  }
  UsbIo = UsbKeyboardDevice->UsbIo;

  PacketSize = UsbKeyboardDevice->IntEndpointDescriptor.MaxPacketSize;

  //
  // Re-submit Asynchronous Interrupt Transfer for recovery.
  //
  UsbKeyboardDevice->AsyncActive = TRUE;
  Status = UsbIo->UsbAsyncInterruptTransfer (
           UsbIo,
           UsbKeyboardDevice->IntEndpointDescriptor.EndpointAddress,
           TRUE,
           UsbKeyboardDevice->IntEndpointDescriptor.Interval,
           PacketSize,
           KeyboardHandler,
           UsbKeyboardDevice
           );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "USB keyboard: recovery submission failed: %r\n", Status));
  }
  UsbKbHidLeave (UsbKeyboardDevice);
}
