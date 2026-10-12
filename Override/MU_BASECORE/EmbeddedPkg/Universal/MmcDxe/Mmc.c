/** @file
  Main file of the MMC Dxe driver. The driver entrypoint is defined into this file.

  Copyright (c) 2011-2013, ARM Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Protocol/DevicePath.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>

#include "Mmc.h"

EFI_BLOCK_IO_MEDIA  mMmcMediaTemplate = {
  SIGNATURE_32 ('m', 'm', 'c', 'o'), // MediaId
  TRUE,                              // RemovableMedia
  FALSE,                             // MediaPresent
  FALSE,                             // LogicalPartition
  FALSE,                             // ReadOnly
  FALSE,                             // WriteCaching
  512,                               // BlockSize
  4,                                 // IoAlign
  0,                                 // Pad
  0                                  // LastBlock
};

//
// This device structure is serviced as a header.
// Its next field points to the first root bridge device node.
//
LIST_ENTRY  mMmcHostPool;

/**
  Event triggered by the timer to check if any cards have been removed
  or if new ones have been plugged in
**/

EFI_EVENT  gCheckCardsEvent;
STATIC BOOLEAN mCheckingCards;

STATIC
MMC_HOST_INSTANCE *
FindMmcHost (
  IN EFI_HANDLE Controller
  )
{
  LIST_ENTRY *Link;
  MMC_HOST_INSTANCE *Instance;
  for (Link = mMmcHostPool.ForwardLink; Link != &mMmcHostPool; Link = Link->ForwardLink) {
    Instance = MMC_HOST_INSTANCE_FROM_LINK (Link);
    if (Instance->ControllerHandle == Controller) {
      return Instance;
    }
  }
  return NULL;
}

/**
  Initialize the MMC Host Pool to support multiple MMC devices
**/
VOID
InitializeMmcHostPool (
  VOID
  )
{
  InitializeListHead (&mMmcHostPool);
}

/**
  Insert a new Mmc Host controller to the pool
**/
VOID
InsertMmcHost (
  IN MMC_HOST_INSTANCE  *MmcHostInstance
  )
{
  InsertTailList (&mMmcHostPool, &(MmcHostInstance->Link));
}

/*
  Remove a new Mmc Host controller to the pool
*/
VOID
RemoveMmcHost (
  IN MMC_HOST_INSTANCE  *MmcHostInstance
  )
{
  RemoveEntryList (&(MmcHostInstance->Link));
}

EFI_STATUS
CreateMmcHostInstance (
  IN MMC_HOST_INSTANCE *MmcHostInstance
  )
{
  EFI_STATUS Status;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_DEVICE_PATH_PROTOCOL End;
  EFI_MMC_HOST_PROTOCOL *MmcHost;

  MmcHost = MmcHostInstance->MmcHost;
  MmcHostInstance->State = MmcHwInitializationState;
  MmcHostInstance->BlockIo.Media = AllocateCopyPool (sizeof (EFI_BLOCK_IO_MEDIA), &mMmcMediaTemplate);
  if (MmcHostInstance->BlockIo.Media == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  MmcHostInstance->BlockIo.Revision = EFI_BLOCK_IO_INTERFACE_REVISION;
  MmcHostInstance->BlockIo.Reset = MmcReset;
  MmcHostInstance->BlockIo.ReadBlocks = MmcReadBlocks;
  MmcHostInstance->BlockIo.WriteBlocks = MmcWriteBlocks;
  MmcHostInstance->BlockIo.FlushBlocks = MmcFlushBlocks;
  Node = NULL;
  Status = MmcHost->BuildDevicePath (MmcHost, &Node);
  if (Status != EFI_SUCCESS) {
    if (Node != NULL) { FreePool (Node); }
    return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
  }
  if (Node == NULL) {
    return EFI_DEVICE_ERROR;
  }
  SetDevicePathEndNode (&End);
  MmcHostInstance->DevicePath = AppendDevicePathNode (&End, Node);
  FreePool (Node);
  if (MmcHostInstance->DevicePath == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &MmcHostInstance->MmcHandle,
                  &gEfiBlockIoProtocolGuid, &MmcHostInstance->BlockIo,
                  &gEfiDevicePathProtocolGuid, MmcHostInstance->DevicePath,
                  NULL);
  if (Status == EFI_SUCCESS) {
    MmcHostInstance->Published = TRUE;
  }
  return EFI_ERROR (Status) ? Status : (Status == EFI_SUCCESS ? EFI_SUCCESS : EFI_NOT_READY);
}

EFI_STATUS
DestroyMmcHostInstance (
  IN MMC_HOST_INSTANCE *MmcHostInstance
  )
{
  EFI_STATUS Status;

  /* Caller holds TPL_NOTIFY. Retain the exact instance on every failed
     uninstall/close, including synchronous nested DisconnectController. */
  if (MmcHostInstance->Busy || MmcHostInstance->CleanupActive || mCheckingCards) {
    return EFI_NOT_READY;
  }
  MmcHostInstance->Stopping = TRUE;
  MmcHostInstance->CleanupActive = TRUE;
  if (MmcHostInstance->Published) {
    Status = gBS->UninstallMultipleProtocolInterfaces (
                    MmcHostInstance->MmcHandle,
                    &gEfiBlockIoProtocolGuid, &MmcHostInstance->BlockIo,
                    &gEfiDevicePathProtocolGuid, MmcHostInstance->DevicePath,
                    NULL);
    if (Status != EFI_SUCCESS) { goto Retain; }
    MmcHostInstance->Published = FALSE;
  }
  if (MmcHostInstance->HostOpened) {
    Status = gBS->CloseProtocol (MmcHostInstance->ControllerHandle,
                                &gEmbeddedMmcHostProtocolGuid,
                                MmcHostInstance->DriverBindingHandle,
                                MmcHostInstance->ControllerHandle);
    if (Status != EFI_SUCCESS) { goto Retain; }
    MmcHostInstance->HostOpened = FALSE;
  }
  if (MmcHostInstance->DevicePath != NULL) { FreePool (MmcHostInstance->DevicePath); }
  if (MmcHostInstance->BlockIo.Media != NULL) { FreePool (MmcHostInstance->BlockIo.Media); }
  if (MmcHostInstance->CardInfo.ECSDData != NULL) {
    FreePages (MmcHostInstance->CardInfo.ECSDData, EFI_SIZE_TO_PAGES (sizeof (ECSD)));
  }
  RemoveMmcHost (MmcHostInstance);
  FreePool (MmcHostInstance);
  return EFI_SUCCESS;
Retain:
  MmcHostInstance->CleanupActive = FALSE;
  return EFI_ERROR (Status) ? Status : EFI_NOT_READY;
}

/**
  This function checks if the controller implement the Mmc Host and the Device Path Protocols
**/
EFI_STATUS
EFIAPI
MmcDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   Controller,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath
  )
{
  EFI_STATUS  Status;
  // EFI_DEVICE_PATH_PROTOCOL        *ParentDevicePath;
  EFI_MMC_HOST_PROTOCOL  *MmcHost;
  EFI_DEV_PATH_PTR       Node;

  //
  // Check RemainingDevicePath validation
  //
  if (RemainingDevicePath != NULL) {
    //
    // Check if RemainingDevicePath is the End of Device Path Node,
    // if yes, go on checking other conditions
    //
    if (!IsDevicePathEnd (RemainingDevicePath)) {
      //
      // If RemainingDevicePath isn't the End of Device Path Node,
      // check its validation
      //
      Node.DevPath = RemainingDevicePath;
      if ((Node.DevPath->Type != HARDWARE_DEVICE_PATH) ||
          (Node.DevPath->SubType != HW_VENDOR_DP) ||
          (DevicePathNodeLength (Node.DevPath) != sizeof (VENDOR_DEVICE_PATH)))
      {
        return EFI_UNSUPPORTED;
      }
    }
  }

  //
  // Check if Mmc Host protocol is installed by platform
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEmbeddedMmcHostProtocolGuid,
                  (VOID **)&MmcHost,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (Status == EFI_ALREADY_STARTED) {
    return EFI_SUCCESS;
  }

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Close the Mmc Host used to perform the supported test
  //
  gBS->CloseProtocol (
         Controller,
         &gEmbeddedMmcHostProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return EFI_SUCCESS;
}

/**

**/
EFI_STATUS
EFIAPI
MmcDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE Controller,
  IN EFI_DEVICE_PATH_PROTOCOL *RemainingDevicePath
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  MMC_HOST_INSTANCE *Instance;

  if ((RemainingDevicePath != NULL) && IsDevicePathEnd (RemainingDevicePath)) {
    return EFI_SUCCESS;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mCheckingCards) { Status = EFI_NOT_READY; goto Exit; }
  Instance = FindMmcHost (Controller);
  if (Instance != NULL) {
    if (!Instance->Stopping) { Status = EFI_ALREADY_STARTED; goto Exit; }
    Status = DestroyMmcHostInstance (Instance);
    if (Status != EFI_SUCCESS) { goto Exit; }
  }
  /* Reserve tracking before acquiring BY_DRIVER ownership. A failed close
     during partial startup must remain reachable for a later Stop retry. */
  Instance = AllocateZeroPool (sizeof (*Instance));
  if (Instance == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Exit; }
  Instance->Signature = MMC_HOST_INSTANCE_SIGNATURE;
  Instance->ControllerHandle = Controller;
  Instance->DriverBindingHandle = This->DriverBindingHandle;
  Instance->Busy = TRUE;
  InsertMmcHost (Instance);
  Status = gBS->OpenProtocol (Controller, &gEmbeddedMmcHostProtocolGuid,
                             (VOID **)&Instance->MmcHost, This->DriverBindingHandle,
                             Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (Status == EFI_SUCCESS) {
    Instance->HostOpened = TRUE;
    Status = CreateMmcHostInstance (Instance);
  }
  Instance->Busy = FALSE;
  if (Status != EFI_SUCCESS) {
    Instance->Stopping = TRUE;
    DestroyMmcHostInstance (Instance);
    if (!EFI_ERROR (Status)) { Status = EFI_NOT_READY; }
    goto Exit;
  }
  gBS->RestoreTPL (OldTpl);
  CheckCardsCallback (NULL, NULL);
  return EFI_SUCCESS;
Exit:
  gBS->RestoreTPL (OldTpl);
  return Status;
}

/**

**/
EFI_STATUS
EFIAPI
MmcDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE Controller,
  IN UINTN NumberOfChildren,
  IN EFI_HANDLE *ChildHandleBuffer
  )
{
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  MMC_HOST_INSTANCE *Instance;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  Instance = FindMmcHost (Controller);
  Status = Instance == NULL ? EFI_NOT_FOUND : DestroyMmcHostInstance (Instance);
  gBS->RestoreTPL (OldTpl);
  return Status;
}

VOID
EFIAPI
CheckCardsCallback (
  IN EFI_EVENT Event,
  IN VOID *Context
  )
{
  LIST_ENTRY *Link;
  MMC_HOST_INSTANCE *Instance;
  EFI_STATUS Status;
  EFI_TPL OldTpl;
  BOOLEAN Present;

  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mCheckingCards) { gBS->RestoreTPL (OldTpl); return; }
  mCheckingCards = TRUE;
  gBS->RestoreTPL (OldTpl);
  for (Link = mMmcHostPool.ForwardLink; Link != &mMmcHostPool; Link = Link->ForwardLink) {
    Instance = MMC_HOST_INSTANCE_FROM_LINK (Link);
    if (Instance->Stopping || Instance->Busy || !Instance->Published) { continue; }
    Instance->Busy = TRUE;
    Present = Instance->MmcHost->IsCardPresent (Instance->MmcHost);
    if (!Present || !Instance->CardPresent) {
      Instance->InitializationAttempts = 0;
    }
    Instance->CardPresent = Present;
    if (!Present && Instance->Initialized) {
      Instance->Initialized = FALSE;
      Instance->BlockIo.Media->MediaPresent = FALSE;
      Instance->BlockIo.Media->LastBlock = 0;
      Instance->BlockIo.Media->BlockSize = 512;
      Instance->BlockIo.Media->ReadOnly = FALSE;
      Instance->BlockIo.Media->MediaId++;
      Instance->State = MmcHwInitializationState;
      Instance->ChangePending = TRUE;
      DEBUG ((DEBUG_INIT, "MMC card removed: media=%u\n", Instance->BlockIo.Media->MediaId));
    } else if (Present && !Instance->Initialized && (Instance->InitializationAttempts < 3)) {
      Instance->InitializationAttempts++;
      if (Instance->CardInfo.ECSDData != NULL) {
        FreePages (Instance->CardInfo.ECSDData, EFI_SIZE_TO_PAGES (sizeof (ECSD)));
      }
      ZeroMem (&Instance->CardInfo, sizeof (Instance->CardInfo));
      Instance->State = MmcHwInitializationState;
      Instance->BlockIo.Media->MediaPresent = FALSE;
      Status = InitializeMmcDevice (Instance);
      if (Status == EFI_SUCCESS) {
        Instance->Initialized = TRUE;
        Instance->BlockIo.Media->MediaPresent = TRUE;
        Instance->ChangePending = TRUE;
        DEBUG ((DEBUG_INIT, "MMC card ready: media=%u last-block=%Lu block-size=%u\n",
          Instance->BlockIo.Media->MediaId, Instance->BlockIo.Media->LastBlock,
          Instance->BlockIo.Media->BlockSize));
      } else {
        Instance->BlockIo.Media->MediaPresent = FALSE;
        Instance->BlockIo.Media->LastBlock = 0;
        Instance->BlockIo.Media->BlockSize = 512;
        Instance->State = MmcHwInitializationState;
        DEBUG ((DEBUG_ERROR, "MMC card initialization: %r\n", Status));
      }
    }
    Instance->Busy = FALSE;
    if (Instance->ChangePending) {
      /* Reinstall synchronously reconnects partition/filesystem drivers.
         Release I/O exclusion so those consumers can read the new media.
         mCheckingCards still prevents nested scans and instance destruction.
         Clear this notification before calling out: nested I/O may discover
         another media change, which must survive a successful reinstall. */
      Instance->ChangePending = FALSE;
      Status = gBS->ReinstallProtocolInterface (Instance->MmcHandle,
                    &gEfiBlockIoProtocolGuid, &Instance->BlockIo, &Instance->BlockIo);
      if (Status != EFI_SUCCESS) { Instance->ChangePending = TRUE; }
    }
  }
  gBS->RaiseTPL (TPL_NOTIFY);
  mCheckingCards = FALSE;
  gBS->RestoreTPL (OldTpl);
}

EFI_DRIVER_BINDING_PROTOCOL  gMmcDriverBinding = {
  MmcDriverBindingSupported,
  MmcDriverBindingStart,
  MmcDriverBindingStop,
  0xa,
  NULL,
  NULL
};

/**

**/
EFI_STATUS
EFIAPI
MmcDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;

  //
  // Initializes MMC Host pool
  //
  InitializeMmcHostPool ();

  //
  // Install driver model protocol(s).
  //
  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gMmcDriverBinding,
             ImageHandle,
             &gMmcComponentName,
             &gMmcComponentName2
             );
  ASSERT_EFI_ERROR (Status);

  // Install driver diagnostics
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDriverDiagnostics2ProtocolGuid,
                  &gMmcDriverDiagnostics2,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  // Use a timer to detect if a card has been plugged in or removed
  Status = gBS->CreateEvent (
                  EVT_NOTIFY_SIGNAL | EVT_TIMER,
                  TPL_CALLBACK,
                  CheckCardsCallback,
                  NULL,
                  &gCheckCardsEvent
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->SetTimer (
                  gCheckCardsEvent,
                  TimerPeriodic,
                  (UINT64)(10*1000*200)
                  );                    // 200 ms
  ASSERT_EFI_ERROR (Status);

  return Status;
}
