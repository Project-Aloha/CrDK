/**
  HDK8450 ADSP PIL filesystem consumer. Firmware is read from existing UFS
  FAT partitions, authenticated through PAS, and stopped at ExitBootServices.
  DriverBinding.Start runs at TPL_APPLICATION, where synchronous filesystem
  and UFS I/O may wait; firmware loading never runs in a timer callback.
  SPDX-License-Identifier: MIT
**/
#include <Uefi.h>
#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PILCrUefiLib.h>
#include <Library/PILCrHandoffUefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/EFIPilCrProtocol.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/SimpleFileSystem.h>

STATIC CR_PIL_CONTEXT mPil;
STATIC CR_PIL_UEFI_HANDOFF mHandoff;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC CR_PIL_CLIENTS mClients;
STATIC EFI_HANDLE mController;
STATIC EFI_EVENT mExit, mReadyBoot;
STATIC BOOLEAN mReady, mStarting, mExiting, mInstalled;
STATIC EFI_STATUS mLastError = EFI_NOT_READY;
STATIC EFI_PHYSICAL_ADDRESS mMetadata;
STATIC UINTN mMetadataPages;
STATIC UINT64 mMetadataAttributes;
STATIC BOOLEAN mMetadataChanged;
STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding;
STATIC CONST EFI_GUID mBasicData =
  {0xebd0a0a2,0xb9e5,0x4433,{0x87,0xc0,0x68,0xb6,0xb7,0x26,0x99,0xc7}};

STATIC EFI_STATUS ReleaseMetadata(VOID)
{
  EFI_STATUS Status;
  if (mMetadataPages == 0) return EFI_SUCCESS;
  if (mPil.SmcInterrupted || (mPil.PasOwned && !mReady)) return EFI_ACCESS_DENIED;
  if (mMetadataChanged) {
    Status = gDS->SetMemorySpaceAttributes(mMetadata,
               EFI_PAGES_TO_SIZE(mMetadataPages), mMetadataAttributes);
    if (EFI_ERROR(Status)) return Status;
    mMetadataChanged = FALSE;
  }
  Status = gBS->FreePages(mMetadata, mMetadataPages);
  if (EFI_ERROR(Status)) return Status;
  mMetadata = 0; mMetadataPages = 0;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS AllocateMetadata(UINTN Size)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS Status;
  UINT64 Bytes, Attributes;
  if (mMetadataPages != 0 || Size == 0 || Size > CR_PIL_MAX_METADATA)
    return EFI_INVALID_PARAMETER;
  mMetadata = MAX_UINT32;
  mMetadataPages = EFI_SIZE_TO_PAGES(Size);
  Status = gBS->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType,
                              mMetadataPages, &mMetadata);
  if (EFI_ERROR(Status)) { mMetadataPages = 0; mMetadata = 0; return Status; }
  Bytes = EFI_PAGES_TO_SIZE(mMetadataPages);
  Status = gDS->GetMemorySpaceDescriptor(mMetadata, &Descriptor);
  if (EFI_ERROR(Status)) goto Fail;
  if (mMetadata < Descriptor.BaseAddress || Bytes > Descriptor.Length ||
      mMetadata - Descriptor.BaseAddress > Descriptor.Length - Bytes ||
      (Descriptor.Capabilities & EFI_MEMORY_WC) == 0) {
    Status = EFI_UNSUPPORTED; goto Fail;
  }
  mMetadataAttributes = Descriptor.Attributes;
  /* On Arm WC is normal noncacheable memory. UC would become Device memory
     and could fault during ordinary metadata copying. Match dma_alloc_coherent. */
  Attributes = (Descriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_WC;
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)mMetadata, (UINTN)Bytes);
  /* Restore even after a failed attribute call: some CPU providers may have
     already changed part of a range before reporting a failure. */
  mMetadataChanged = TRUE;
  Status = gDS->SetMemorySpaceAttributes(mMetadata, Bytes, Attributes);
  if (EFI_ERROR(Status)) goto Fail;
  ZeroMem((VOID *)(UINTN)mMetadata, (UINTN)Bytes);
  return EFI_SUCCESS;
Fail:
  (VOID)ReleaseMetadata();
  return Status;
}

STATIC EFI_STATUS ValidateCarveout(VOID)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS Status;
  UINT64 Base = mPil.Target->MemoryBase, Size = mPil.Target->MemorySize;
  Status = gDS->GetMemorySpaceDescriptor(Base, &Descriptor);
  if (EFI_ERROR(Status)) return Status;
  if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeReserved ||
      Base < Descriptor.BaseAddress || Size > Descriptor.Length ||
      Base - Descriptor.BaseAddress > Descriptor.Length - Size ||
      (Descriptor.Attributes & EFI_MEMORY_RP) != 0)
    return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS FileSize(EFI_FILE_PROTOCOL *File, UINT64 *Size)
{
  UINT64 Buffer[128];
  UINTN Length = sizeof(Buffer);
  EFI_FILE_INFO *Info = (EFI_FILE_INFO *)Buffer;
  EFI_STATUS Status = File->GetInfo(File, &gEfiFileInfoGuid, &Length, Buffer);
  if (EFI_ERROR(Status)) return Status;
  if (Length < SIZE_OF_EFI_FILE_INFO || Length > sizeof(Buffer) ||
      Info->Size < SIZE_OF_EFI_FILE_INFO || Info->Size > Length ||
      (Info->Attribute & EFI_FILE_DIRECTORY) != 0) return EFI_COMPROMISED_DATA;
  *Size = Info->FileSize;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadBytes(EFI_FILE_PROTOCOL *File, VOID *Buffer, UINTN Size)
{
  EFI_STATUS Status;
  UINTN Length, Remaining = Size;
  UINT8 *Destination = Buffer;
  while (Remaining != 0) {
    Length = MIN(Remaining, 0x10000U);
    Status = File->Read(File, &Length, Destination);
    if (EFI_ERROR(Status)) return Status;
    if (Length == 0 || Length > MIN(Remaining, 0x10000U)) return EFI_BAD_BUFFER_SIZE;
    Destination += Length; Remaining -= Length;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ImageFile(EFI_FILE_PROTOCOL *Root, CONST CHAR16 *Suffix,
                            EFI_FILE_PROTOCOL **File)
{
  CHAR16 Path[128];
  EFI_STATUS Status;
  *File = NULL;
  Status = StrCpyS(Path, ARRAY_SIZE(Path), mPil.Target->ImagePath);
  if (EFI_ERROR(Status)) return Status;
  Status = StrCatS(Path, ARRAY_SIZE(Path), Suffix);
  if (EFI_ERROR(Status)) return Status;
  return Root->Open(Root, File, Path, EFI_FILE_MODE_READ, 0);
}

STATIC EFI_STATUS ReadSegment(VOID *Context, UINT32 Index, VOID *Buffer, UINT32 Size)
{
  EFI_FILE_PROTOCOL *File = NULL;
  CHAR16 Suffix[] = L".b00";
  UINT64 Actual;
  EFI_STATUS Status;
  if (Index >= 100) return EFI_INVALID_PARAMETER;
  Suffix[2] = (CHAR16)(L'0' + Index / 10); Suffix[3] = (CHAR16)(L'0' + Index % 10);
  Status = ImageFile(Context, Suffix, &File);
  if (EFI_ERROR(Status)) return Status;
  Status = FileSize(File, &Actual);
  if (!EFI_ERROR(Status) && Actual != Size) Status = EFI_BAD_BUFFER_SIZE;
  if (!EFI_ERROR(Status)) Status = ReadBytes(File, Buffer, Size);
  File->Close(File);
  return Status;
}

STATIC EFI_STATUS LoadImage(EFI_FILE_PROTOCOL *Root)
{
  EFI_FILE_PROTOCOL *File = NULL;
  VOID *Header = NULL;
  UINT64 Size;
  UINTN MetadataSize;
  CR_PIL_IMAGE Image;
  EFI_STATUS Status, Cleanup;
  CONST CHAR8 *Stage;
  mPil.Stage = "firmware discovery";
  Status = ImageFile(Root, L".mdt", &File);
  if (Status == EFI_NOT_FOUND) Status = ImageFile(Root, L".mbn", &File);
  if (EFI_ERROR(Status)) return Status;
  mPil.Stage = "firmware header read";
  Status = FileSize(File, &Size);
  if (EFI_ERROR(Status)) goto Done;
  if (Size < 52 || Size > (UINT64)mPil.Target->MemorySize + CR_PIL_MAX_METADATA) {
    Status = EFI_BAD_BUFFER_SIZE; goto Done;
  }
  Header = AllocatePool((UINTN)Size);
  if (Header == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  Status = ReadBytes(File, Header, (UINTN)Size);
  if (EFI_ERROR(Status)) goto Done;
  mPil.Stage = "ELF validation";
  Status = CrPilInspect(Header, (UINTN)Size, mPil.Target, &Image);
  if (EFI_ERROR(Status)) goto Done;
  mPil.Stage = "reserved carveout";
  Status = ValidateCarveout(); if (EFI_ERROR(Status)) goto Done;
  mPil.Stage = "metadata allocation";
  MetadataSize = (UINTN)Image.HeaderSize + Image.HashSize;
  Status = AllocateMetadata(MetadataSize); if (EFI_ERROR(Status)) goto Done;
  mPil.Stage = "signed metadata";
  Status = CrPilBuildMetadata(Header, (UINTN)Size, &Image, ReadSegment, Root,
                               (VOID *)(UINTN)mMetadata, MetadataSize);
  if (EFI_ERROR(Status)) goto Done;
  DEBUG((DEBUG_INIT, "PILCr: ADSP header=%Lu phdrs=%u split=%u reloc=%u span=0x%x\n",
          Size, Image.Count, Image.Split, Image.Relocatable, Image.Span));
  Status = CrPilStart(&mPil, Header, (UINTN)Size, &Image,
                      (VOID *)(UINTN)mMetadata, (VOID *)(UINTN)mPil.Target->MemoryBase,
                      ReadSegment, Root);
  if (Status == EFI_ALREADY_STARTED && !mPil.PasOwned) {
    mReady = TRUE; mPil.Stage = "existing ADSP; borrowed";
    DEBUG((DEBUG_INIT, "PILCr: ADSP already ready; retaining bootloader ownership\n"));
    Status = EFI_SUCCESS;
  } else if (!EFI_ERROR(Status)) mReady = TRUE;
Done:
  if (File != NULL) File->Close(File);
  if (Header != NULL) FreePool(Header);
  Stage = mPil.Stage;
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "PILCr: stage=%a failed %r\n", Stage, Status));
    Cleanup = CrPilStop(&mPil);
    if (EFI_ERROR(Cleanup)) DEBUG((DEBUG_ERROR, "PILCr: startup rollback failed %r\n", Cleanup));
  }
  Cleanup = ReleaseMetadata();
  if (EFI_ERROR(Cleanup))
    DEBUG((DEBUG_ERROR, "PILCr: metadata retained (reserved pages): %r\n", Cleanup));
  mPil.Stage = Stage;
  return Status;
}

STATIC EFI_STATUS EFIAPI GetState(EFI_PIL_CR_PROTOCOL *This, EFI_PIL_CR_STATE *State)
{
  EFI_TPL Previous;
  (VOID)This;
  if (State == NULL) return EFI_INVALID_PARAMETER;
  Previous = gBS->RaiseTPL(TPL_NOTIFY);
  State->Ready = mReady && !mExiting; State->Owned = mPil.PasOwned;
  State->Exiting = mExiting; State->LastError = mLastError; State->Stage = mPil.Stage;
  gBS->RestoreTPL(Previous);
  return State->Ready ? EFI_SUCCESS : EFI_NOT_READY;
}

STATIC EFI_STATUS EFIAPI RegisterClient(EFI_PIL_CR_PROTOCOL *This, VOID *Owner,
                                        CR_PIL_CLIENT_QUIESCE Quiesce, VOID *Context)
{
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS Status;
  (VOID)This;
  Status = mExiting ? EFI_ACCESS_DENIED : CrPilClientRegister(&mClients, Owner, Quiesce, Context);
  gBS->RestoreTPL(Tpl);
  return Status;
}

STATIC EFI_STATUS EFIAPI UnregisterClient(EFI_PIL_CR_PROTOCOL *This, VOID *Owner)
{
  EFI_TPL Tpl = gBS->RaiseTPL(TPL_NOTIFY);
  EFI_STATUS Status;
  (VOID)This;
  Status = CrPilClientUnregister(&mClients, Owner);
  gBS->RestoreTPL(Tpl);
  return Status;
}

STATIC EFI_PIL_CR_PROTOCOL mProtocol = {
  EFI_PIL_CR_PROTOCOL_REVISION, GetState, RegisterClient, UnregisterClient
};

STATIC EFI_STATUS EFIAPI Supported(EFI_DRIVER_BINDING_PROTOCOL *This,
                                   EFI_HANDLE Controller, EFI_DEVICE_PATH_PROTOCOL *Remaining)
{
  EFI_PARTITION_INFO_PROTOCOL *Partition;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_DEVICE_PATH_PROTOCOL *Node;
  EFI_STATUS Status;
  UINTN Count;
  BOOLEAN Ufs = FALSE;
  (VOID)Remaining;
  if (mExiting || mStarting || mReady || mController != NULL) return EFI_UNSUPPORTED;
  Status = gBS->HandleProtocol(Controller, &gEfiPartitionInfoProtocolGuid, (VOID **)&Partition);
  if (EFI_ERROR(Status) || Partition->Type != PARTITION_TYPE_GPT ||
      !CompareGuid(&Partition->Info.Gpt.PartitionTypeGUID, &mBasicData)) return EFI_UNSUPPORTED;
  Status = gBS->HandleProtocol(Controller, &gEfiBlockIoProtocolGuid, (VOID **)&Block);
  if (EFI_ERROR(Status) || Block->Media == NULL || Block->Media->RemovableMedia ||
      !Block->Media->MediaPresent) return EFI_UNSUPPORTED;
  Status = gBS->HandleProtocol(Controller, &gEfiDevicePathProtocolGuid, (VOID **)&Node);
  if (EFI_ERROR(Status)) return EFI_UNSUPPORTED;
  for (Count = 0; Count < 256; ++Count) {
    UINTN Length = Node->Length[0] | ((UINTN)Node->Length[1] << 8);
    if (Length < sizeof(*Node) || Length > 4096) return EFI_UNSUPPORTED;
    if (Node->Type == MESSAGING_DEVICE_PATH && Node->SubType == MSG_UFS_DP) Ufs = TRUE;
    if (Node->Type == END_DEVICE_PATH_TYPE) break;
    Node = (VOID *)((UINTN)Node + Length);
  }
  if (!Ufs || Count == 256) return EFI_UNSUPPORTED;
  Status = gBS->OpenProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs,
                             This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR(Status)) return Status;
  gBS->CloseProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid,
                      This->DriverBindingHandle, Controller);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI Start(EFI_DRIVER_BINDING_PROTOCOL *This,
                               EFI_HANDLE Controller, EFI_DEVICE_PATH_PROTOCOL *Remaining)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_FILE_PROTOCOL *Root = NULL;
  EFI_PARTITION_INFO_PROTOCOL *Partition;
  EFI_STATUS Status;
  (VOID)Remaining;
  if (mExiting || mStarting || mController != NULL || mReady || mPil.SmcInterrupted)
    return EFI_UNSUPPORTED;
  mStarting = TRUE;
  Status = gBS->OpenProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs,
                             This->DriverBindingHandle, Controller, EFI_OPEN_PROTOCOL_BY_DRIVER);
  if (EFI_ERROR(Status)) goto Done;
  Status = Fs->OpenVolume(Fs, &Root);
  if (!EFI_ERROR(Status)) {
    if (!EFI_ERROR(gBS->HandleProtocol(Controller, &gEfiPartitionInfoProtocolGuid, (VOID **)&Partition))) {
      CHAR16 Name[37];
      CopyMem(Name, Partition->Info.Gpt.PartitionName, sizeof(Name) - sizeof(CHAR16)); Name[36] = 0;
      DEBUG((DEBUG_INIT, "PILCr: searching UFS partition %s for %s.mdt/.mbn\n", Name, mPil.Target->ImagePath));
    }
    Status = LoadImage(Root);
    Root->Close(Root);
  }
  mLastError = Status;
  /* A failed secure shutdown or attribute restoration retains the driver and
     reserved metadata pages; never unload code backing unfinished cleanup. */
  if (!EFI_ERROR(Status) || mPil.PasOwned || mPil.Votes != 0 || mPil.LoadState ||
      mPil.CryptoPath != 0 || mPil.SmcInterrupted || mPil.QmpOpen || mPil.QmpClosing || mMetadataPages != 0) {
    mController = Controller; Status = EFI_SUCCESS;
  } else {
    gBS->CloseProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid,
                        This->DriverBindingHandle, Controller);
  }
Done:
  mStarting = FALSE;
  return Status;
}

STATIC EFI_STATUS EFIAPI Stop(EFI_DRIVER_BINDING_PROTOCOL *This,
                              EFI_HANDLE Controller, UINTN Children, EFI_HANDLE *ChildBuffer)
{
  EFI_STATUS Status;
  (VOID)ChildBuffer;
  if (mExiting || mStarting || Controller != mController || Children != 0)
    return EFI_UNSUPPORTED;
  if (mClients.Count != 0) return EFI_ACCESS_DENIED;
  mReady = FALSE;
  Status = CrPilStop(&mPil); if (EFI_ERROR(Status)) return Status;
  Status = ReleaseMetadata(); if (EFI_ERROR(Status)) return Status;
  Status = gBS->CloseProtocol(Controller, &gEfiSimpleFileSystemProtocolGuid,
                              This->DriverBindingHandle, Controller);
  if (EFI_ERROR(Status)) return Status;
  mController = NULL; mPil.Stage = "stopped"; mLastError = EFI_NOT_READY;
  return EFI_SUCCESS;
}

STATIC EFI_DRIVER_BINDING_PROTOCOL mBinding = {Supported, Start, Stop, 0x10, NULL, NULL};

STATIC VOID EFIAPI ReadyBoot(EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Event; (VOID)Context;
  if (!mExiting) {
    Status = CrPilHandoffPrepare(&mHandoff);
    if (EFI_ERROR(Status)) DEBUG((DEBUG_WARN, "PILCr: final handoff preparation unavailable: %r\n", Status));
    else DEBUG((DEBUG_INIT, "PILCr: reserved final ownership record=%Lx generation=%Lu; awaiting EBS retirement\n",
                mHandoff.Record, mHandoff.Generation));
  }
  if (!mReady && !mExiting)
    DEBUG((DEBUG_WARN, "PILCr: ADSP unavailable at ReadyToBoot: stage=%a status=%r; USB awaits PMIC GLink\n",
            mPil.Stage, mLastError));
}

STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Event; (VOID)Context;
  if (mHandoff.Published) return;
  /* Receipt precedes both client drainage and the secure stop. A reset before
     this line must not be diagnosed as an ADSP shutdown failure. */
  DEBUG((DEBUG_WARN, "PILCr: ExitBootServices entered clients=%u owned=%u running=%u starting=%u generation=%Lu\n",
          mClients.Count, mPil.PasOwned, mPil.Running, mStarting, mPil.StartGeneration));
  mExiting = TRUE; mReady = FALSE;
  if (mStarting) {
    mLastError = EFI_NOT_READY;
    DEBUG((DEBUG_ERROR, "PILCr: active startup retained; ExitBootServices shutdown deferred\n"));
    return;
  }
  Status = CrPilClientsQuiesce(&mClients);
  if (EFI_ERROR(Status)) {
    mLastError = Status;
    DEBUG((DEBUG_ERROR, "PILCr: consumer retirement failed: %r; ADSP shutdown deferred\n", Status));
    return;
  }
  DEBUG((DEBUG_WARN, "PILCr: ExitBootServices clients drained; ADSP stop begin owned=%u interrupted=%u QMP=%u/%u votes=0x%x crypto=%u\n",
          mPil.PasOwned, mPil.SmcInterrupted, mPil.QmpOpen, mPil.QmpClosing,
          mPil.Votes, mPil.CryptoPath));
  Status = CrPilStop(&mPil); mLastError = Status;
  DEBUG((EFI_ERROR(Status) ? DEBUG_ERROR : DEBUG_INIT,
          "PILCr: ExitBootServices ADSP shutdown: %r owned=%u QMP=%u/%u observed=%u generation=%Lu load=%u votes=0x%x crypto=%Lu\n",
          Status, mPil.PasOwned, mPil.QmpOpen, mPil.QmpClosing,
          mPil.Shutdown.Complete, mPil.StartGeneration, mPil.LoadState,
          mPil.Votes, mPil.CryptoPath));
  if (!EFI_ERROR(Status)) {
    EFI_STATUS HandoffStatus = CrPilHandoffPublish(&mHandoff, &mPil, &mClients, mRpmh);
    DEBUG((EFI_ERROR(HandoffStatus) ? DEBUG_WARN : DEBUG_INIT,
           "PILCr: final ownership record: %r generation=%Lu published=%u\n",
           HandoffStatus, mHandoff.Generation, mHandoff.Published));
  }
  /* No filesystem, memory-map changes, allocations or frees here. Startup
     failures that could not release metadata keep EfiReservedMemoryType pages. */
}

EFI_STATUS EFIAPI PILCrDxeUnload(EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  if (mExiting || mStarting || mClients.Count != 0) return EFI_ACCESS_DENIED;
  if (mController != NULL) {
    Status = Stop(&mBinding, mController, 0, NULL);
    if (EFI_ERROR(Status)) return Status;
  } else if (mPil.SmcInterrupted || mPil.PasOwned || mPil.Votes != 0 ||
             mPil.LoadState || mPil.CryptoPath != 0 || mPil.QmpOpen || mPil.QmpClosing || mMetadataPages != 0) {
    Status = CrPilStop(&mPil);
    if (EFI_ERROR(Status)) return Status;
    Status = ReleaseMetadata();
    if (EFI_ERROR(Status)) return Status;
  }
  Status = CrPilHandoffRelease(&mHandoff);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
              &gEfiDriverBindingProtocolGuid, &mBinding, &gEfiPilCrProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR(Status)) return Status;
  mInstalled = FALSE;
  gBS->CloseEvent(mReadyBoot); gBS->CloseEvent(mExit); mReadyBoot = mExit = NULL;
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI PILCrDxeEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  CONST CR_DAL_DEVICE_INFO *Info;
  EFI_SMEM_CR_PROTOCOL *Smem;
  EFI_CMD_DB_PROTOCOL *CmdDb;
  EFI_RPMH_CR_PROTOCOL *Rpmh;
  EFI_INTERCONNECT_CR_PROTOCOL *Interconnect;
  EFI_STATUS Status;
  (VOID)SystemTable;
  if (mInstalled) return EFI_ALREADY_STARTED;
  Status = CrDalGetDeviceInfo(CrDalDevicePil, 0, CR_DAL_DATA_REVISION_1,
                              sizeof(CR_PIL_TARGET), &Info);
  if (EFI_ERROR(Status)) return Status;
  if (Info == NULL || Info->Data == NULL) return EFI_NOT_FOUND;
  Status = gBS->LocateProtocol(&gEfiSmemCrProtocolGuid, NULL, (VOID **)&Smem);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiCmdDBCrProtocolGuid, NULL, (VOID **)&CmdDb);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid, NULL, (VOID **)&Rpmh);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiInterconnectCrProtocolGuid, NULL, (VOID **)&Interconnect);
  if (EFI_ERROR(Status)) return Status;
  Status = CrPilInitialize(&mPil, Info->Data, Smem, CmdDb, Rpmh, Interconnect);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, ExitBoot, NULL,
                              &gEfiEventExitBootServicesGuid, &mExit);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ReadyBoot, NULL,
                              &gEfiEventReadyToBootGuid, &mReadyBoot);
  if (EFI_ERROR(Status)) { gBS->CloseEvent(mExit); mExit = NULL; return Status; }
  mBinding.ImageHandle = mBinding.DriverBindingHandle = ImageHandle;
  Status = gBS->InstallMultipleProtocolInterfaces(&ImageHandle,
              &gEfiDriverBindingProtocolGuid, &mBinding, &gEfiPilCrProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR(Status)) {
    gBS->CloseEvent(mReadyBoot); gBS->CloseEvent(mExit); mReadyBoot = mExit = NULL;
    return Status;
  }
  mInstalled = TRUE;
  mRpmh = Rpmh;
  Status = CrPilHandoffPrepare(&mHandoff);
  if (EFI_ERROR(Status)) DEBUG((DEBUG_WARN, "PILCr: handoff preparation deferred: %r\n", Status));
  DEBUG((DEBUG_INIT, "PILCr: native ADSP loader registered; awaiting UFS firmware filesystem\n"));
  return EFI_SUCCESS;
}
