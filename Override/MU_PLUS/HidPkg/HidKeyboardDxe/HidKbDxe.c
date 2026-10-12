/** @file HidKbDxe.c

  Copyright (C) Microsoft Corporation. All rights reserved.

  Portions derived from UsbKbDxe which is:
  Copyright (c) 2004 - 2018, Intel Corporation. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "HidKbDxe.h"
#include "HidKeyboard.h"

// Failed/partial startup and teardown keep their exact context discoverable.
// All registry and active-operation transitions are serialized at TPL_NOTIFY.
STATIC LIST_ENTRY mKeyboardDevices = INITIALIZE_LIST_HEAD_VARIABLE (mKeyboardDevices);

STATIC HID_KB_DEV *
FindKeyboardDevice (EFI_HANDLE Controller, EFI_HANDLE Binding)
{
  LIST_ENTRY *Link;
  HID_KB_DEV *Device;
  for (Link = GetFirstNode (&mKeyboardDevices); !IsNull (&mKeyboardDevices, Link);
       Link = GetNextNode (&mKeyboardDevices, Link)) {
    Device = CR (Link, HID_KB_DEV, DriverLink, HID_KB_DEV_SIGNATURE);
    if (Device->ControllerHandle == Controller && Device->DriverBindingHandle == Binding) {
      return Device;
    }
  }
  return NULL;
}

BOOLEAN
HidKeyboardBeginCallback (IN HID_KB_DEV *Device)
{
  EFI_TPL OldTpl;
  BOOLEAN Entered;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Entered = Device != NULL && Device->Signature == HID_KB_DEV_SIGNATURE && !Device->Stopping;
  if (Entered) {
    ++Device->ActiveCallbacks;
  }
  gBS->RestoreTPL (OldTpl);
  return Entered;
}

VOID
HidKeyboardEndCallback (IN HID_KB_DEV *Device)
{
  EFI_TPL OldTpl;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ASSERT (Device->ActiveCallbacks != 0);
  --Device->ActiveCallbacks;
  gBS->RestoreTPL (OldTpl);
}

STATIC EFI_STATUS
CloseKeyboardEvent (EFI_EVENT *Event)
{
  EFI_STATUS Status;
  if (*Event == NULL) {
    return EFI_SUCCESS;
  }
  Status = gBS->CloseEvent (*Event);
  if (!EFI_ERROR (Status)) {
    *Event = NULL;
  }
  return Status;
}

// Called at TPL_NOTIFY. A queued event is removed by CloseEvent; an already
// executing event/report is pinned by ActiveCallbacks and must return first.
STATIC EFI_STATUS
ReleaseKeyboardDevice (IN HID_KB_DEV *Device)
{
  EFI_STATUS Status;
  Device->Stopping = TRUE;
  if (Device->ActiveCallbacks != 0 || Device->Starting || Device->CleanupActive) {
    return EFI_NOT_READY;
  }
  // Uninstall can synchronously call a consuming driver's Stop. Retain an
  // exclusive cleanup claim through that callback and every partial teardown.
  Device->CleanupActive = TRUE;
  if (Device->ReportRegistered) {
    Status = Device->KeyboardProtocol->UnRegisterKeyboardHidReportCallback (Device->KeyboardProtocol);
    if (Status != EFI_SUCCESS) {
      if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
      goto Retained;
    }
    Device->ReportRegistered = FALSE;
  }
  if (Device->SimpleInputInstalled) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    Device->ControllerHandle,
                    &gEfiSimpleTextInProtocolGuid, &Device->SimpleInput,
                    &gEfiSimpleTextInputExProtocolGuid, &Device->SimpleInputEx, NULL);
    if (EFI_ERROR (Status)) {
      goto Retained;
    }
    Device->SimpleInputInstalled = FALSE;
  }
  // Close the notification event before destroying its queue, including a
  // signal queued behind the higher TPL used by this Stop invocation.
  Status = CloseKeyboardEvent (&Device->KeyNotifyProcessEvent);
  if (EFI_ERROR (Status)) { goto Retained; }
  Status = CloseKeyboardEvent (&Device->RepeatTimer);
  if (EFI_ERROR (Status)) { goto Retained; }
  Status = CloseKeyboardEvent (&Device->SimpleInput.WaitForKey);
  if (EFI_ERROR (Status)) { goto Retained; }
  Status = CloseKeyboardEvent (&Device->SimpleInputEx.WaitForKeyEx);
  if (EFI_ERROR (Status)) { goto Retained; }
  Status = CloseKeyboardEvent (&Device->KeyboardLayoutEvent);
  if (EFI_ERROR (Status)) { goto Retained; }
  if (Device->KeyboardProtocolOpened) {
    Status = gBS->CloseProtocol (Device->ControllerHandle, &gHidKeyboardProtocolGuid,
                               Device->DriverBindingHandle, Device->ControllerHandle);
    if (EFI_ERROR (Status)) { goto Retained; }
    Device->KeyboardProtocolOpened = FALSE;
  }
  Device->KeyboardProtocol = NULL;
  KbdFreeNotifyList (&Device->NotifyList);
  ReleaseKeyboardLayoutResources (Device);
  if (Device->ControllerNameTable != NULL) {
    FreeUnicodeStringTable (Device->ControllerNameTable);
  }
  DestroyQueue (&Device->HidKeyQueue);
  DestroyQueue (&Device->EfiKeyQueue);
  DestroyQueue (&Device->EfiKeyQueueForNotify);
  if (Device->LastReport != NULL) {
    FreePool (Device->LastReport);
  }
  RemoveEntryList (&Device->DriverLink);
  Device->Signature = 0;
  FreePool (Device);
  return EFI_SUCCESS;
Retained:
  Device->CleanupActive = FALSE;
  return Status;
}

//
// HID Keyboard Driver Global Variables
//
EFI_DRIVER_BINDING_PROTOCOL  mHidKeyboardDriverBinding = {
  HIDKeyboardDriverBindingSupported,
  HIDKeyboardDriverBindingStart,
  HIDKeyboardDriverBindingStop,
  HID_KEYBOARD_DRIVER_VERSION,
  NULL,
  NULL
};

/**
  Entrypoint of HID Keyboard Driver.

  This function is the entrypoint of HID Keyboard Driver. It
  installs Driver Binding Protocols.

  @param  ImageHandle       The firmware allocated handle for the EFI image.
  @param  SystemTable       A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.

**/
EFI_STATUS
EFIAPI
HIDKeyboardDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &mHidKeyboardDriverBinding,
             ImageHandle,
             &mHidKeyboardComponentName,
             &mHidKeyboardComponentName2
             );
  ASSERT_EFI_ERROR (Status);

  return EFI_SUCCESS;
}

/**
  Check whether HID keyboard driver supports this device.

  @param  This                   The HID keyboard driver binding
                                 protocol.
  @param  Controller             The controller handle to check.
  @param  RemainingDevicePath    The remaining device path.

  @retval EFI_SUCCESS            The driver supports this controller.
  @retval other                  This device isn't supported.

**/
EFI_STATUS
EFIAPI
HIDKeyboardDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS             Status = EFI_UNSUPPORTED;
  HID_KEYBOARD_PROTOCOL  *KeyboardProtocol;

  //
  // Try to bind to HID Keyboard Protocol.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gHidKeyboardProtocolGuid,
                  (VOID **)(HID_KEYBOARD_PROTOCOL **)&KeyboardProtocol,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  gBS->CloseProtocol (
         Controller,
         &gHidKeyboardProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return (Status);
}

/**
  Starts the HID Keyboard device with this driver.

  This function produces Simple Text Input Protocol and Simple Text Input Ex Protocol,
  initializes the keyboard device, and registers the Key Stroke
  Callback function on the HidKeyboardProtocol so that lower layers
  will call this callback when ever a key stroke event happens.

  @param  This                   The HID keyboard driver binding instance.
  @param  Controller             Handle of device to bind driver to.
  @param  RemainingDevicePath    Optional parameter use to pick a specific child
                                 device to start.

  @retval EFI_SUCCESS            HID key board driver started.
  @retval EFI_UNSUPPORTED        No interrupt endpoint can be found.
  @retval Other                  This controller cannot be started.

**/
EFI_STATUS
EFIAPI
HIDKeyboardDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS  Status;
  HID_KB_DEV  *HidKeyboardDevice = NULL;
  EFI_TPL     OldTpl;

  DEBUG ((DEBUG_VERBOSE, "[%a]\n", __FUNCTION__));

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  HidKeyboardDevice = FindKeyboardDevice (Controller, This->DriverBindingHandle);
  if (HidKeyboardDevice != NULL) {
    Status = HidKeyboardDevice->Stopping ? EFI_NOT_READY : EFI_ALREADY_STARTED;
    gBS->RestoreTPL (OldTpl);
    return Status;
  }

  // Allocate HidKeyboardDevice context.
  HidKeyboardDevice = AllocateZeroPool (sizeof (HID_KB_DEV));

  if (NULL == HidKeyboardDevice) {
    DEBUG ((DEBUG_ERROR, "[%a] - Failed to allocate HID Keyboard context.\n", __FUNCTION__));
    Status = EFI_OUT_OF_RESOURCES;
    goto ErrorExit;
  }

  HidKeyboardDevice->Signature        = HID_KB_DEV_SIGNATURE;
  HidKeyboardDevice->ControllerHandle = Controller;
  HidKeyboardDevice->DriverBindingHandle = This->DriverBindingHandle;
  HidKeyboardDevice->Starting = TRUE;
  InitializeListHead (&HidKeyboardDevice->NotifyList);
  InitializeListHead (&HidKeyboardDevice->NsKeyList);
  InsertTailList (&mKeyboardDevices, &HidKeyboardDevice->DriverLink);

  // Get the Device Path Protocol on Controller's handle
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&HidKeyboardDevice->DevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  // Bind to HID Keyboard Protocol.
  Status = gBS->OpenProtocol (
                  Controller,
                  &gHidKeyboardProtocolGuid,
                  (VOID **)(HID_KEYBOARD_PROTOCOL **)&HidKeyboardDevice->KeyboardProtocol,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    // There is no HID keyboard - this is unexpected, since DriverBindingSupport should guarantee
    // that this doesn't get called unless the keyboard exists, so ASSERT for debug.
    DEBUG ((DEBUG_ERROR, "[%a] - Failed to retrieve HID keyboard protocol: %r\n", __FUNCTION__, Status));
    goto ErrorExit;
  }

  HidKeyboardDevice->KeyboardProtocolOpened = TRUE;

  // Set up SimpleTextIn
  HidKeyboardDevice->SimpleInput.Reset         = HIDKeyboardReset;
  HidKeyboardDevice->SimpleInput.ReadKeyStroke = HIDKeyboardReadKeyStroke;

  HidKeyboardDevice->SimpleInputEx.Reset               = HIDKeyboardResetEx;
  HidKeyboardDevice->SimpleInputEx.ReadKeyStrokeEx     = HIDKeyboardReadKeyStrokeEx;
  HidKeyboardDevice->SimpleInputEx.SetState            = HIDKeyboardSetState;
  HidKeyboardDevice->SimpleInputEx.RegisterKeyNotify   = HIDKeyboardRegisterKeyNotify;
  HidKeyboardDevice->SimpleInputEx.UnregisterKeyNotify = HIDKeyboardUnregisterKeyNotify;

  InitializeListHead (&HidKeyboardDevice->NotifyList);

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  HIDKeyboardWaitForKey,
                  HidKeyboardDevice,
                  &(HidKeyboardDevice->SimpleInputEx.WaitForKeyEx)
                  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[%a] - Create WaitforKeyEx Event Failed!\n", __FUNCTION__));
    goto ErrorExit;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_WAIT,
                  TPL_NOTIFY,
                  HIDKeyboardWaitForKey,
                  HidKeyboardDevice,
                  &(HidKeyboardDevice->SimpleInput.WaitForKey)
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "[%a] - Create WaitforKey Event Failed!\n", __FUNCTION__));
    goto ErrorExit;
  }

  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  KeyNotifyProcessHandler,
                  HidKeyboardDevice,
                  &HidKeyboardDevice->KeyNotifyProcessEvent
                  );
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  // Initialize every queue and callback context before publishing interfaces.
  Status = InitKeyboardLayout (HidKeyboardDevice);
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }
  Status = HidKeyboardDevice->SimpleInputEx.Reset (&HidKeyboardDevice->SimpleInputEx, TRUE);
  if (EFI_ERROR (Status) || HidKeyboardDevice->Stopping) {
    if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
    goto ErrorExit;
  }

  //
  // Register the Keyboard Callback function in KeyboardProtocol. The lower layer HID keyboard driver will call this
  // callback whenever Keystroke events happen.
  Status = HidKeyboardDevice->KeyboardProtocol->RegisterKeyboardHidReportCallback (
                                         HidKeyboardDevice->KeyboardProtocol,
                                         HIDProcessKeyStrokesCallback,
                                         (VOID *)HidKeyboardDevice
                                         );

  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }
  HidKeyboardDevice->ReportRegistered = TRUE;
  if (HidKeyboardDevice->Stopping) { Status = EFI_NOT_READY; goto ErrorExit; }
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Controller,
                  &gEfiSimpleTextInProtocolGuid, &HidKeyboardDevice->SimpleInput,
                  &gEfiSimpleTextInputExProtocolGuid, &HidKeyboardDevice->SimpleInputEx,
                  NULL);
  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }
  HidKeyboardDevice->SimpleInputInstalled = TRUE;

  HidKeyboardDevice->ControllerNameTable = NULL;
  AddUnicodeString2 (
    "eng",
    mHidKeyboardComponentName.SupportedLanguages,
    &HidKeyboardDevice->ControllerNameTable,
    L"Generic HID Keyboard",
    TRUE
    );
  AddUnicodeString2 (
    "en",
    mHidKeyboardComponentName2.SupportedLanguages,
    &HidKeyboardDevice->ControllerNameTable,
    L"Generic HID Keyboard",
    FALSE
    );

  HidKeyboardDevice->Starting = FALSE;
  gBS->RestoreTPL (OldTpl);
  DEBUG ((DEBUG_VERBOSE, "[%a] - Completed successfully\n", __FUNCTION__));
  return EFI_SUCCESS;

  //
  // Error handler
  //
ErrorExit:
  if (HidKeyboardDevice != NULL) {
    EFI_STATUS CleanupStatus;
    HidKeyboardDevice->Starting = FALSE;
    CleanupStatus = ReleaseKeyboardDevice (HidKeyboardDevice);
    if (EFI_ERROR (CleanupStatus)) {
      DEBUG ((DEBUG_ERROR, "[%a] - startup cleanup retained: %r\n", __FUNCTION__, CleanupStatus));
    }
  }
  gBS->RestoreTPL (OldTpl);
  DEBUG ((DEBUG_ERROR, "[%a] - Failed: %r\n", __FUNCTION__, Status));
  return Status;
}

/**
  Stop the Hid keyboard device handled by this driver.

  @param  This                   The HID keyboard driver binding protocol.
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
HIDKeyboardDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN  EFI_HANDLE                   Controller,
  IN  UINTN                        NumberOfChildren,
  IN  EFI_HANDLE                   *ChildHandleBuffer
  )
{
  HID_KB_DEV *HidKeyboardDevice;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  HidKeyboardDevice = FindKeyboardDevice (Controller, This->DriverBindingHandle);
  if (HidKeyboardDevice == NULL) {
    gBS->RestoreTPL (OldTpl);
    return EFI_UNSUPPORTED;
  }
  Status = ReleaseKeyboardDevice (HidKeyboardDevice);
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Internal function to read the next keystroke from the keyboard buffer.

  @param  HidKeyboardDevice       HID keyboard's private structure.
  @param  KeyData                 A pointer to buffer to hold the keystroke
                                  data for the key that was pressed.

  @retval EFI_SUCCESS             The keystroke information was returned.
  @retval EFI_NOT_READY           There was no keystroke data availiable.
  @retval EFI_DEVICE_ERROR        The keystroke information was not returned due to
                                  hardware errors.
  @retval EFI_INVALID_PARAMETER   KeyData is NULL.
  @retval Others                  Fail to translate keycode into EFI_INPUT_KEY

**/
EFI_STATUS
HIDKeyboardReadKeyStrokeWorker (
  IN OUT HID_KB_DEV  *HidKeyboardDevice,
  OUT EFI_KEY_DATA   *KeyData
  )
{
  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  EFI_TPL OldTpl;
  EFI_STATUS Status;
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Status = HidKeyboardDevice->Stopping || IsQueueEmpty (&HidKeyboardDevice->EfiKeyQueue) ? EFI_NOT_READY :
    Dequeue (&HidKeyboardDevice->EfiKeyQueue, KeyData, sizeof (*KeyData));
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Reset the input device and optionally run diagnostics

  There are 2 types of reset for HID keyboard. For
  non-exhaustive reset, only keyboard buffer is cleared. For
  exhaustive reset, in addition to clearance of keyboard buffer,
  the hardware status is also re-initialized.

  @param  This                 Protocol instance pointer.
  @param  ExtendedVerification Driver may perform diagnostics on reset.

  @retval EFI_SUCCESS          The device was reset.
  @retval EFI_DEVICE_ERROR     The device is not functioning properly and could not be reset.

**/
EFI_STATUS
EFIAPI
HIDKeyboardReset (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  IN  BOOLEAN                         ExtendedVerification
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  HID_KB_DEV *HidKeyboardDevice;
  HidKeyboardDevice = HID_KB_DEV_FROM_THIS (This);
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (HidKeyboardDevice->Stopping || HidKeyboardDevice->ActiveCallbacks != 0) {
    gBS->RestoreTPL (OldTpl);
    return EFI_NOT_READY;
  }
  ++HidKeyboardDevice->ActiveCallbacks;
  if (!ExtendedVerification) {
    Status = InitQueue (&HidKeyboardDevice->HidKeyQueue, sizeof (HID_KEY));
    if (!EFI_ERROR (Status)) { Status = InitQueue (&HidKeyboardDevice->EfiKeyQueue, sizeof (EFI_KEY_DATA)); }
    if (!EFI_ERROR (Status)) { Status = InitQueue (&HidKeyboardDevice->EfiKeyQueueForNotify, sizeof (EFI_KEY_DATA)); }
  } else {
    Status = InitHidKeyboard (HidKeyboardDevice);
  }
  if (!EFI_ERROR (Status)) {
    HidKeyboardDevice->KeyState.KeyShiftState = EFI_SHIFT_STATE_VALID;
    HidKeyboardDevice->KeyState.KeyToggleState = EFI_TOGGLE_STATE_VALID;
  }
  --HidKeyboardDevice->ActiveCallbacks;
  if (HidKeyboardDevice->Stopping && !EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**
  Reads the next keystroke from the input device.

  @param  This                 The EFI_SIMPLE_TEXT_INPUT_PROTOCOL instance.
  @param  Key                  A pointer to a buffer that is filled in with the keystroke
                               information for the key that was pressed.

  @retval EFI_SUCCESS          The keystroke information was returned.
  @retval EFI_NOT_READY        There was no keystroke data availiable.
  @retval EFI_DEVICE_ERROR     The keystroke information was not returned due to
                               hardware errors.

**/
EFI_STATUS
EFIAPI
HIDKeyboardReadKeyStroke (
  IN  EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *This,
  OUT EFI_INPUT_KEY                   *Key
  )
{
  HID_KB_DEV    *HidKeyboardDevice;
  EFI_KEY_DATA  KeyData;
  EFI_STATUS    Status = EFI_SUCCESS;

  if (Key == NULL) { return EFI_INVALID_PARAMETER; }
  HidKeyboardDevice = HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  if (!HidKeyboardBeginCallback (HidKeyboardDevice)) { return EFI_NOT_READY; }

  //
  // Considering if the partial keystroke is enabled, there maybe a partial
  // keystroke in the queue, so here skip the partial keystroke and get the
  // next key from the queue
  //
  while (1) {
    Status = HIDKeyboardReadKeyStrokeWorker (HidKeyboardDevice, &KeyData);
    if (EFI_ERROR (Status)) {
      HidKeyboardEndCallback (HidKeyboardDevice);
      return Status;
    }

    //
    // SimpleTextIn Protocol doesn't support partial keystroke;
    //
    if ((KeyData.Key.ScanCode == CHAR_NULL) && (KeyData.Key.UnicodeChar == SCAN_NULL)) {
      continue;
    }

    //
    // Translate the CTRL-Alpha characters to their corresponding control value
    // (ctrl-a = 0x0001 through ctrl-Z = 0x001A)
    //
    if ((KeyData.KeyState.KeyShiftState & (EFI_LEFT_CONTROL_PRESSED | EFI_RIGHT_CONTROL_PRESSED)) != 0) {
      if ((KeyData.Key.UnicodeChar >= L'a') && (KeyData.Key.UnicodeChar <= L'z')) {
        KeyData.Key.UnicodeChar = (CHAR16)(KeyData.Key.UnicodeChar - L'a' + 1);
      } else if ((KeyData.Key.UnicodeChar >= L'A') && (KeyData.Key.UnicodeChar <= L'Z')) {
        KeyData.Key.UnicodeChar = (CHAR16)(KeyData.Key.UnicodeChar - L'A' + 1);
      }
    }

    CopyMem (Key, &KeyData.Key, sizeof (EFI_INPUT_KEY));
    DEBUG ((DEBUG_VERBOSE, "[%a] - ReadKeyStroke, ScanCode = %d\n", __FUNCTION__, Key->ScanCode));
    HidKeyboardEndCallback (HidKeyboardDevice);
    return EFI_SUCCESS;
  }
}

/**
  Event notification function registered for EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL.WaitForKeyEx
  and EFI_SIMPLE_TEXT_INPUT_PROTOCOL.WaitForKey.

  @param  Event        Event to be signaled when a key is pressed.
  @param  Context      Points to HID_KB_DEV instance.

**/
VOID
EFIAPI
HIDKeyboardWaitForKey (
  IN  EFI_EVENT  Event,
  IN  VOID       *Context
  )
{
  HID_KB_DEV    *HidKeyboardDevice;
  EFI_KEY_DATA  KeyData;
  EFI_TPL       OldTpl;

  HidKeyboardDevice = (HID_KB_DEV *)Context;
  ASSERT (NULL != HidKeyboardDevice);

  if (!HidKeyboardBeginCallback (HidKeyboardDevice)) {
    return;
  }

  //
  // Enter critical section
  //
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  //
  // WaitforKey doesn't support the partial key.
  // Considering if the partial keystroke is enabled, there maybe a partial
  // keystroke in the queue, so here skip the partial keystroke and get the
  // next key from the queue
  //
  if (IsQueueEmpty (&HidKeyboardDevice->EfiKeyQueue)) {
    DEBUG ((DEBUG_VERBOSE, "[%a] - WaitForKey Queue empty!\n", __FUNCTION__));
  }

  while (!IsQueueEmpty (&HidKeyboardDevice->EfiKeyQueue)) {
    //
    // If there is pending key, signal the event.
    //
    CopyMem (
      &KeyData,
      HidKeyboardDevice->EfiKeyQueue.Buffer[HidKeyboardDevice->EfiKeyQueue.Head],
      sizeof (EFI_KEY_DATA)
      );
    if ((KeyData.Key.ScanCode == SCAN_NULL) && (KeyData.Key.UnicodeChar == CHAR_NULL)) {
      DEBUG ((DEBUG_VERBOSE, "[%a] - WaitForKey, DeQueued, ScanCode = %d \n", __FUNCTION__, KeyData.Key.ScanCode));
      Dequeue (&HidKeyboardDevice->EfiKeyQueue, &KeyData, sizeof (EFI_KEY_DATA));
      continue;
    }

    DEBUG ((DEBUG_VERBOSE, "[%a] - WaitForKey, Signaling event!\n", __FUNCTION__));
    gBS->SignalEvent (Event);
    break;
  }

  //
  // Leave critical section and return
  //
  gBS->RestoreTPL (OldTpl);
  HidKeyboardEndCallback (HidKeyboardDevice);
}

/**
  Free keyboard notify list.

  @param  NotifyList              The keyboard notify list to free.

  @retval EFI_SUCCESS             Free the notify list successfully.
  @retval EFI_INVALID_PARAMETER   NotifyList is NULL.

**/
EFI_STATUS
KbdFreeNotifyList (
  IN OUT LIST_ENTRY  *NotifyList
  )
{
  KEYBOARD_CONSOLE_IN_EX_NOTIFY  *NotifyNode;
  LIST_ENTRY                     *Link;

  if (NotifyList == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  while (!IsListEmpty (NotifyList)) {
    Link       = GetFirstNode (NotifyList);
    NotifyNode = CR (Link, KEYBOARD_CONSOLE_IN_EX_NOTIFY, NotifyEntry, HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE);
    RemoveEntryList (Link);
    FreePool (NotifyNode);
  }

  return EFI_SUCCESS;
}

/**
  Check whether the pressed key matches a registered key or not.

  @param  RegisteredData    A pointer to keystroke data for the key that was registered.
  @param  InputData         A pointer to keystroke data for the key that was pressed.

  @retval TRUE              Key pressed matches a registered key.
  @retval FALSE             Key pressed does not matches a registered key.

**/
BOOLEAN
IsKeyRegistered (
  IN EFI_KEY_DATA  *RegisteredData,
  IN EFI_KEY_DATA  *InputData
  )
{
  ASSERT (RegisteredData != NULL && InputData != NULL);

  if ((RegisteredData->Key.ScanCode    != InputData->Key.ScanCode) ||
      (RegisteredData->Key.UnicodeChar != InputData->Key.UnicodeChar))
  {
    return FALSE;
  }

  //
  // Assume KeyShiftState/KeyToggleState = 0 in Registered key data means these state could be ignored.
  //
  if ((RegisteredData->KeyState.KeyShiftState != 0) &&
      (RegisteredData->KeyState.KeyShiftState != InputData->KeyState.KeyShiftState))
  {
    return FALSE;
  }

  if (((RegisteredData->KeyState.KeyToggleState) != 0) &&
      (RegisteredData->KeyState.KeyToggleState != InputData->KeyState.KeyToggleState))
  {
    return FALSE;
  }

  return TRUE;
}

//
// Simple Text Input Ex protocol functions
//

/**
  Resets the input device hardware.

  The Reset() function resets the input device hardware. As part
  of initialization process, the firmware/device will make a quick
  but reasonable attempt to verify that the device is functioning.
  If the ExtendedVerification flag is TRUE the firmware may take
  an extended amount of time to verify the device is operating on
  reset. Otherwise the reset operation is to occur as quickly as
  possible. The hardware verification process is not defined by
  this specification and is left up to the platform firmware or
  driver to implement.

  @param This                 A pointer to the EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL instance.

  @param ExtendedVerification Indicates that the driver may perform a more exhaustive
                              verification operation of the device during reset.

  @retval EFI_SUCCESS         The device was reset.
  @retval EFI_DEVICE_ERROR    The device is not functioning correctly and could not be reset.

**/
EFI_STATUS
EFIAPI
HIDKeyboardResetEx (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN BOOLEAN                            ExtendedVerification
  )
{
  EFI_STATUS  Status = EFI_SUCCESS;
  HID_KB_DEV  *HidKeyboardDevice;

  HidKeyboardDevice = TEXT_INPUT_EX_HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  Status = HidKeyboardDevice->SimpleInput.Reset (&HidKeyboardDevice->SimpleInput, ExtendedVerification);
  if (EFI_ERROR (Status)) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Reads the next keystroke from the input device.

  @param  This                   Protocol instance pointer.
  @param  KeyData                A pointer to a buffer that is filled in with the keystroke
                                 state data for the key that was pressed.

  @retval EFI_SUCCESS            The keystroke information was returned.
  @retval EFI_NOT_READY          There was no keystroke data available.
  @retval EFI_DEVICE_ERROR       The keystroke information was not returned due to
                                 hardware errors.
  @retval EFI_INVALID_PARAMETER  KeyData is NULL.

**/
EFI_STATUS
EFIAPI
HIDKeyboardReadKeyStrokeEx (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  OUT EFI_KEY_DATA                       *KeyData
  )
{
  HID_KB_DEV  *HidKeyboardDevice;
  EFI_STATUS Status;

  if (KeyData == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboardDevice = TEXT_INPUT_EX_HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  if (!HidKeyboardBeginCallback (HidKeyboardDevice)) { return EFI_NOT_READY; }
  Status = HIDKeyboardReadKeyStrokeWorker (HidKeyboardDevice, KeyData);
  HidKeyboardEndCallback (HidKeyboardDevice);
  return Status;
}

/**
  Set certain state for the input device.

  @param  This                    Protocol instance pointer.
  @param  KeyToggleState          A pointer to the EFI_KEY_TOGGLE_STATE to set the
                                  state for the input device.

  @retval EFI_SUCCESS             The device state was set appropriately.
  @retval EFI_DEVICE_ERROR        The device is not functioning correctly and could
                                  not have the setting adjusted.
  @retval EFI_UNSUPPORTED         The device does not support the ability to have its state set.
  @retval EFI_INVALID_PARAMETER   KeyToggleState is NULL.

**/
EFI_STATUS
EFIAPI
HIDKeyboardSetState (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN EFI_KEY_TOGGLE_STATE               *KeyToggleState
  )
{
  HID_KB_DEV  *HidKeyboardDevice;

  if (KeyToggleState == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboardDevice = TEXT_INPUT_EX_HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  if (((HidKeyboardDevice->KeyState.KeyToggleState & EFI_TOGGLE_STATE_VALID) != EFI_TOGGLE_STATE_VALID) ||
      ((*KeyToggleState & EFI_TOGGLE_STATE_VALID) != EFI_TOGGLE_STATE_VALID))
  {
    return EFI_UNSUPPORTED;
  }

  if (!HidKeyboardBeginCallback (HidKeyboardDevice)) {
    return EFI_NOT_READY;
  }

  //
  // Update the status light
  //
  HidKeyboardDevice->ScrollOn            = FALSE;
  HidKeyboardDevice->NumLockOn           = FALSE;
  HidKeyboardDevice->CapsOn              = FALSE;
  HidKeyboardDevice->IsSupportPartialKey = FALSE;

  if ((*KeyToggleState & EFI_SCROLL_LOCK_ACTIVE) == EFI_SCROLL_LOCK_ACTIVE) {
    HidKeyboardDevice->ScrollOn = TRUE;
  }

  if ((*KeyToggleState & EFI_NUM_LOCK_ACTIVE) == EFI_NUM_LOCK_ACTIVE) {
    HidKeyboardDevice->NumLockOn = TRUE;
  }

  if ((*KeyToggleState & EFI_CAPS_LOCK_ACTIVE) == EFI_CAPS_LOCK_ACTIVE) {
    HidKeyboardDevice->CapsOn = TRUE;
  }

  if ((*KeyToggleState & EFI_KEY_STATE_EXPOSED) == EFI_KEY_STATE_EXPOSED) {
    HidKeyboardDevice->IsSupportPartialKey = TRUE;
  }

  SetKeyLED (HidKeyboardDevice);

  HidKeyboardDevice->KeyState.KeyToggleState = *KeyToggleState;

  HidKeyboardEndCallback (HidKeyboardDevice);

  return EFI_SUCCESS;
}

/**
  Register a notification function for a particular keystroke for the input device.

  @param  This                        Protocol instance pointer.
  @param  KeyData                     A pointer to a buffer that is filled in with
                                      the keystroke information for the key that was
                                      pressed. If KeyData.Key, KeyData.KeyState.KeyToggleState
                                      and KeyData.KeyState.KeyShiftState are 0, then any incomplete
                                      keystroke will trigger a notification of the KeyNotificationFunction.
  @param  KeyNotificationFunction     Points to the function to be called when the key
                                      sequence is typed specified by KeyData. This notification function
                                      should be called at <=TPL_CALLBACK.
  @param  NotifyHandle                Points to the unique handle assigned to the registered notification.

  @retval EFI_SUCCESS                 The notification function was registered successfully.
  @retval EFI_OUT_OF_RESOURCES        Unable to allocate resources for necessary data structures.
  @retval EFI_INVALID_PARAMETER       KeyData or NotifyHandle or KeyNotificationFunction is NULL.

**/
EFI_STATUS
EFIAPI
HIDKeyboardRegisterKeyNotify (
  IN  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN  EFI_KEY_DATA                       *KeyData,
  IN  EFI_KEY_NOTIFY_FUNCTION            KeyNotificationFunction,
  OUT VOID                               **NotifyHandle
  )
{
  HID_KB_DEV                     *HidKeyboardDevice;
  KEYBOARD_CONSOLE_IN_EX_NOTIFY  *NewNotify;
  LIST_ENTRY                     *Link;
  LIST_ENTRY                     *NotifyList;
  KEYBOARD_CONSOLE_IN_EX_NOTIFY  *CurrentNotify;

  EFI_TPL OldTpl;

  if ((KeyData == NULL) || (NotifyHandle == NULL) || (KeyNotificationFunction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboardDevice = TEXT_INPUT_EX_HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  if (HidKeyboardDevice->Stopping) {
    return EFI_NOT_READY;
  }

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  //
  // Return EFI_SUCCESS if the (KeyData, NotificationFunction) is already registered.
  //
  NotifyList = &HidKeyboardDevice->NotifyList;

  for (Link = GetFirstNode (NotifyList);
       !IsNull (NotifyList, Link);
       Link = GetNextNode (NotifyList, Link))
  {
    CurrentNotify = CR (
                      Link,
                      KEYBOARD_CONSOLE_IN_EX_NOTIFY,
                      NotifyEntry,
                      HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE
                      );
    DEBUG ((DEBUG_VERBOSE, "[%a] - RegisterKeyNotify, NotifyScanCode = %d, KeyDataScanCode= %d\n", __FUNCTION__, CurrentNotify->KeyData.Key.ScanCode, KeyData->Key.ScanCode));
    if (!CurrentNotify->Removed && IsKeyRegistered (&CurrentNotify->KeyData, KeyData)) {
      if (CurrentNotify->KeyNotificationFn == KeyNotificationFunction) {
        *NotifyHandle = CurrentNotify;
        gBS->RestoreTPL (OldTpl);
        return EFI_SUCCESS;
      }
    }
  }

  //
  // Allocate resource to save the notification function
  //
  NewNotify = (KEYBOARD_CONSOLE_IN_EX_NOTIFY *)AllocateZeroPool (sizeof (KEYBOARD_CONSOLE_IN_EX_NOTIFY));
  if (NewNotify == NULL) {
    DEBUG ((DEBUG_ERROR, "[%a] - allocation failed!\n", __FUNCTION__));
    gBS->RestoreTPL (OldTpl);
    return EFI_OUT_OF_RESOURCES;
  }

  NewNotify->Signature         = HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE;
  NewNotify->KeyNotificationFn = KeyNotificationFunction;
  CopyMem (&NewNotify->KeyData, KeyData, sizeof (EFI_KEY_DATA));
  DEBUG ((DEBUG_VERBOSE, "[%a] - inserting new notify!\n", __FUNCTION__));
  InsertTailList (&HidKeyboardDevice->NotifyList, &NewNotify->NotifyEntry);

  *NotifyHandle = NewNotify;

  gBS->RestoreTPL (OldTpl);

  return EFI_SUCCESS;
}

/**
  Remove a registered notification function from a particular keystroke.

  @param  This                      Protocol instance pointer.
  @param  NotificationHandle        The handle of the notification function being unregistered.

  @retval EFI_SUCCESS              The notification function was unregistered successfully.
  @retval EFI_INVALID_PARAMETER    The NotificationHandle is invalid

**/
EFI_STATUS
EFIAPI
HIDKeyboardUnregisterKeyNotify (
  IN EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *This,
  IN VOID                               *NotificationHandle
  )
{
  HID_KB_DEV                     *HidKeyboardDevice;
  KEYBOARD_CONSOLE_IN_EX_NOTIFY  *CurrentNotify;
  LIST_ENTRY                     *Link;
  LIST_ENTRY                     *NotifyList;

  EFI_TPL OldTpl;

  if (NotificationHandle == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  HidKeyboardDevice = TEXT_INPUT_EX_HID_KB_DEV_FROM_THIS (This);
  ASSERT (NULL != HidKeyboardDevice);

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);

  //
  // Traverse notify list of HID keyboard and remove the entry of NotificationHandle.
  //
  NotifyList = &HidKeyboardDevice->NotifyList;
  for (Link = GetFirstNode (NotifyList);
       !IsNull (NotifyList, Link);
       Link = GetNextNode (NotifyList, Link))
  {
    CurrentNotify = CR (
                      Link,
                      KEYBOARD_CONSOLE_IN_EX_NOTIFY,
                      NotifyEntry,
                      HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE
                      );
    if (CurrentNotify == NotificationHandle && !CurrentNotify->Removed) {
      //
      // Remove the notification function from NotifyList and free resources
      //
      if (HidKeyboardDevice->NotifyDispatchDepth != 0) {
        CurrentNotify->Removed = TRUE;
      } else {
        RemoveEntryList (&CurrentNotify->NotifyEntry);
        FreePool (CurrentNotify);
      }
      gBS->RestoreTPL (OldTpl);
      return EFI_SUCCESS;
    }
  }

  //
  // Cannot find the matching entry in database.
  //
  gBS->RestoreTPL (OldTpl);
  return EFI_INVALID_PARAMETER;
}

/**
  Process key notify.

  @param  Event                 Indicates the event that invoke this function.
  @param  Context               Indicates the calling context.
**/
VOID
EFIAPI
KeyNotifyProcessHandler (
  IN  EFI_EVENT  Event,
  IN  VOID       *Context
  )
{
  EFI_STATUS Status;
  HID_KB_DEV *Device = Context;
  EFI_KEY_DATA KeyData;
  LIST_ENTRY *Link, *Next;
  KEYBOARD_CONSOLE_IN_EX_NOTIFY *Notify;
  EFI_KEY_NOTIFY_FUNCTION Function;
  EFI_TPL OldTpl;
  if (!HidKeyboardBeginCallback (Device)) { return; }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  ++Device->NotifyDispatchDepth;
  gBS->RestoreTPL (OldTpl);
  while (TRUE) {
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    Status = Device->Stopping ? EFI_NOT_READY : Dequeue (&Device->EfiKeyQueueForNotify, &KeyData, sizeof (KeyData));
    Link = GetFirstNode (&Device->NotifyList);
    gBS->RestoreTPL (OldTpl);
    if (EFI_ERROR (Status)) { break; }
    while (!IsNull (&Device->NotifyList, Link)) {
      OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
      Notify = CR (Link, KEYBOARD_CONSOLE_IN_EX_NOTIFY, NotifyEntry, HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE);
      Next = GetNextNode (&Device->NotifyList, Link);
      Function = !Device->Stopping && !Notify->Removed && IsKeyRegistered (&Notify->KeyData, &KeyData) ? Notify->KeyNotificationFn : NULL;
      gBS->RestoreTPL (OldTpl);
      if (Function != NULL) { Function (&KeyData); }
      Link = Next;
    }
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  --Device->NotifyDispatchDepth;
  if (Device->NotifyDispatchDepth == 0) {
    for (Link = GetFirstNode (&Device->NotifyList); !IsNull (&Device->NotifyList, Link); Link = Next) {
      Next = GetNextNode (&Device->NotifyList, Link);
      Notify = CR (Link, KEYBOARD_CONSOLE_IN_EX_NOTIFY, NotifyEntry, HID_KB_CONSOLE_IN_EX_NOTIFY_SIGNATURE);
      if (Notify->Removed) { RemoveEntryList (Link); FreePool (Notify); }
    }
  }
  gBS->RestoreTPL (OldTpl);
  HidKeyboardEndCallback (Device);
}
