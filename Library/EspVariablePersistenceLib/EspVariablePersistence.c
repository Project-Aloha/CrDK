/** @file
  Boot-time persistence of the complete authenticated emulated variable store.
  Files are checksummed snapshots, not an authentication or anti-rollback root.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include <Uefi.h>
#include <Guid/Gpt.h>
#include <Guid/VariableFormat.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeLib.h>
#include <Library/VariablePersistenceLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>
#include <Protocol/PartitionInfo.h>
#include <Protocol/MmcHost.h>
#include <Protocol/ScsiPassThru.h>
#include <Protocol/ScsiPassThruExt.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/UfsHostController.h>

#define ESP_NV_SIGNATURE  SIGNATURE_64 ('C','R','N','V','E','S','P','1')
#define ESP_NV_MAX_STORE  (16U * 1024U * 1024U)
#define ESP_NV_MAX_PATH   (64U * 1024U)
#pragma pack(1)
typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 StoreSize;
  UINT32 Flags;
  UINT64 Generation;
  UINT32 PayloadCrc;
  UINT32 HeaderCrc;
  UINT8  Reserved[24];
} ESP_NV_HEADER;
#pragma pack()
STATIC_ASSERT (sizeof (ESP_NV_HEADER) == 64, "ESP NV wire header size");

typedef struct {
  BOOLEAN Loaded;
  BOOLEAN Busy;
  BOOLEAN Selected;
  BOOLEAN FailedClose;
  BOOLEAN HaveSnapshot;
  EFI_HANDLE Handle;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_GUID Partition;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN PathSize;
  UINT32 MediaId;
  UINTN StoreSize;
  UINT64 Generation;
  UINTN Slot;
  UINT8 *LastStore;
  UINT8 *Work;
} ESP_NV_CONTEXT;
STATIC ESP_NV_CONTEXT mEspNv;
STATIC CHAR16 *mNames[2] = { L"NvVars0.bin", L"NvVars1.bin" };

STATIC UINT32
EspCrc (IN CONST VOID *Buffer, IN UINTN Size)
{
  CONST UINT8 *Bytes = Buffer;
  UINT32 Crc = MAX_UINT32;
  UINTN Index;
  UINTN Bit;
  for (Index = 0; Index < Size; ++Index) {
    Crc ^= Bytes[Index];
    for (Bit = 0; Bit < 8; ++Bit) {
      Crc = (Crc >> 1) ^ ((0U - (Crc & 1U)) & 0xEDB88320U);
    }
  }
  return ~Crc;
}

STATIC BOOLEAN
EspAll (IN CONST UINT8 *Bytes, IN UINTN Size, IN UINT8 Value)
{
  UINTN Index;
  for (Index = 0; Index < Size; ++Index) {
    if (Bytes[Index] != Value) {
      return FALSE;
    }
  }
  return TRUE;
}

// Every pointer/size later consumed by VariableParsing.c is bounded here.
// Authentication metadata (timestamp, count, key index and certdb payload) is
// copied verbatim and is interpreted only by the normal variable/auth services.
STATIC BOOLEAN
EspStoreValid (IN CONST VOID *Store, IN UINTN Size)
{
  VARIABLE_STORE_HEADER Header;
  AUTHENTICATED_VARIABLE_HEADER Variable;
  CONST UINT8 *Bytes = Store;
  UINTN Offset;
  UINTN Name;
  UINTN End;
  UINTN Index;
  UINT16 Character;
  if ((Store == NULL) || (Size < sizeof (Header)) ||
      (Size > ESP_NV_MAX_STORE) || ((Size & 3U) != 0)) {
    return FALSE;
  }
  CopyMem (&Header, Store, sizeof (Header));
  if (!CompareGuid (&Header.Signature, &gEfiAuthenticatedVariableGuid) ||
      (Header.Size != Size) || (Header.Format != VARIABLE_STORE_FORMATTED) ||
      (Header.State != VARIABLE_STORE_HEALTHY)) {
    return FALSE;
  }
  Offset = (sizeof (Header) + 3U) & ~(UINTN)3U;
  while (Offset < Size) {
    if (Bytes[Offset] == 0xFF) {
      return EspAll (Bytes + Offset, Size - Offset, 0xFF);
    }
    if (Size - Offset < sizeof (Variable)) {
      return FALSE;
    }
    CopyMem (&Variable, Bytes + Offset, sizeof (Variable));
    if ((Variable.StartId != VARIABLE_DATA) ||
        ((Variable.State != VAR_HEADER_VALID_ONLY) &&
         (Variable.State != VAR_ADDED) && (Variable.State != 0x3E) &&
         (Variable.State != 0x3D) && (Variable.State != 0x3C)) ||
        ((Variable.Attributes & ~0x3FU) != 0) ||
        ((Variable.Attributes & VARIABLE_ATTRIBUTE_NV_BS) != VARIABLE_ATTRIBUTE_NV_BS) ||
        (Variable.NameSize < 2U * sizeof (CHAR16)) || ((Variable.NameSize & 1U) != 0)) {
      return FALSE;
    }
    Name = Offset + sizeof (Variable);
    if ((Variable.NameSize > Size - Name) ||
        (Variable.DataSize > Size - Name - Variable.NameSize)) {
      return FALSE;
    }
    for (Index = 0; Index < Variable.NameSize; Index += sizeof (CHAR16)) {
      CopyMem (&Character, Bytes + Name + Index, sizeof (Character));
      if ((Character == 0) != (Index == Variable.NameSize - sizeof (CHAR16))) {
        return FALSE;
      }
    }
    End = Name + Variable.NameSize + Variable.DataSize;
    Offset = (End + 3U) & ~(UINTN)3U;
    if ((Offset > Size) || !EspAll (Bytes + End, Offset - End, 0xFF)) {
      return FALSE;
    }
  }
  return TRUE;
}

STATIC EFI_STATUS
EspEnter (VOID)
{
  EFI_TPL OldTpl;
  if (EfiGetCurrentTpl () > TPL_CALLBACK) {
    return EFI_UNSUPPORTED;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  if (mEspNv.Busy) {
    gBS->RestoreTPL (OldTpl);
    return EFI_ACCESS_DENIED;
  }
  mEspNv.Busy = TRUE;
  gBS->RestoreTPL (OldTpl);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EspClose (IN EFI_FILE_PROTOCOL *File, IN EFI_STATUS Status)
{
  EFI_STATUS CloseStatus;
  if (File == NULL) {
    return Status;
  }
  CloseStatus = File->Close (File);
  if (CloseStatus != EFI_SUCCESS) {
    // A failed Close has no specified reusable-handle lifetime. Never retry
    // through that possibly freed object or publish further durable generations.
    mEspNv.FailedClose = TRUE;
    if (Status == EFI_SUCCESS) {
      Status = EFI_ERROR (CloseStatus) ? CloseStatus : EFI_DEVICE_ERROR;
    }
  }
  return Status;
}

STATIC VOID
EspConnect (IN EFI_GUID *Protocol)
{
  EFI_HANDLE *Handles;
  UINTN Count;
  UINTN Index;
  EFI_STATUS Status;
  Handles = NULL;
  Status = gBS->LocateHandleBuffer (ByProtocol, Protocol, NULL, &Count, &Handles);
  if (Status == EFI_SUCCESS) {
    for (Index = 0; Index < Count; ++Index) {
      // ConnectController may legitimately report already connected or absent
      // media; only subsequent real partition/filesystem discovery qualifies it.
      gBS->ConnectController (Handles[Index], NULL, NULL, TRUE);
    }
  }
  if (Handles != NULL) {
    FreePool (Handles);
  }
}

STATIC BOOLEAN
EspCandidate (
  IN EFI_HANDLE Handle,
  OUT EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **Fs,
  OUT EFI_PARTITION_INFO_PROTOCOL **Partition,
  OUT EFI_BLOCK_IO_PROTOCOL **Block,
  OUT EFI_DEVICE_PATH_PROTOCOL **Path,
  OUT UINTN *PathSize
  )
{
  EFI_GUID Zero = { 0 };
  if ((gBS->HandleProtocol (Handle, &gEfiPartitionInfoProtocolGuid, (VOID **)Partition) != EFI_SUCCESS) ||
      ((*Partition)->Revision != EFI_PARTITION_INFO_PROTOCOL_REVISION) ||
      ((*Partition)->Type != PARTITION_TYPE_GPT) || ((*Partition)->System != 1) ||
      !CompareGuid (&(*Partition)->Info.Gpt.PartitionTypeGUID, &gEfiPartTypeSystemPartGuid) ||
      CompareGuid (&(*Partition)->Info.Gpt.UniquePartitionGUID, &Zero) ||
      (gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)Block) != EFI_SUCCESS) ||
      ((*Block)->Media == NULL) || !(*Block)->Media->MediaPresent || !(*Block)->Media->LogicalPartition ||
      (gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)Path) != EFI_SUCCESS) ||
      !IsDevicePathValid (*Path, ESP_NV_MAX_PATH) ||
      (gBS->HandleProtocol (Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)Fs) != EFI_SUCCESS)) {
    return FALSE;
  }
  *PathSize = GetDevicePathSize (*Path);
  return (BOOLEAN)((*PathSize != 0) && (*PathSize <= ESP_NV_MAX_PATH));
}

STATIC EFI_STATUS
EspSelect (VOID)
{
  EFI_HANDLE *Handles = NULL;
  UINTN Count;
  UINTN Index;
  EFI_STATUS Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_PARTITION_INFO_PROTOCOL *Partition;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN PathSize;
  BOOLEAN Removable = TRUE;
  INTN Compare;
  EFI_DEVICE_PATH_PROTOCOL *NewPath;
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleFileSystemProtocolGuid, NULL, &Count, &Handles);
  if (Status != EFI_SUCCESS) {
    if (Handles != NULL) {
      FreePool (Handles);
    }
    return (Status == EFI_NOT_FOUND) ? EFI_SUCCESS : (EFI_ERROR (Status) ? Status : EFI_DEVICE_ERROR);
  }
  for (Index = 0; Index < Count; ++Index) {
    if (!EspCandidate (Handles[Index], &Fs, &Partition, &Block, &Path, &PathSize)) {
      continue;
    }
    Compare = mEspNv.Selected ? CompareMem (Path, mEspNv.Path, MIN (PathSize, mEspNv.PathSize)) : -1;
    if ((Compare == 0) && mEspNv.Selected) {
      Compare = (PathSize < mEspNv.PathSize) ? -1 : ((PathSize > mEspNv.PathSize) ? 1 : 0);
      if (Compare == 0) {
        Compare = CompareMem (&Partition->Info.Gpt.UniquePartitionGUID, &mEspNv.Partition, sizeof (EFI_GUID));
      }
    }
    if (mEspNv.Selected &&
        ((Block->Media->RemovableMedia > Removable) ||
         ((Block->Media->RemovableMedia == Removable) && (Compare >= 0)))) {
      continue;
    }
    NewPath = AllocateCopyPool (PathSize, Path);
    if (NewPath == NULL) {
      FreePool (Handles);
      return EFI_OUT_OF_RESOURCES;
    }
    if (mEspNv.Path != NULL) {
      FreePool (mEspNv.Path);
    }
    mEspNv.Path = NewPath;
    mEspNv.Selected = TRUE;
    mEspNv.Handle = Handles[Index];
    mEspNv.Fs = Fs;
    CopyMem (&mEspNv.Partition, &Partition->Info.Gpt.UniquePartitionGUID, sizeof (EFI_GUID));
    mEspNv.PathSize = PathSize;
    mEspNv.MediaId = Block->Media->MediaId;
    Removable = Block->Media->RemovableMedia;
  }
  FreePool (Handles);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
EspOpenDirectory (IN BOOLEAN Create, OUT EFI_FILE_PROTOCOL **Directory)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs;
  EFI_PARTITION_INFO_PROTOCOL *Partition;
  EFI_BLOCK_IO_PROTOCOL *Block;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  UINTN PathSize;
  EFI_FILE_PROTOCOL *Root = NULL;
  EFI_FILE_PROTOCOL *Efi = NULL;
  EFI_STATUS Status;
  UINT64 Mode = EFI_FILE_MODE_READ;
  *Directory = NULL;
  if (mEspNv.FailedClose ||
      !EspCandidate (mEspNv.Handle, &Fs, &Partition, &Block, &Path, &PathSize) ||
      (Fs != mEspNv.Fs) || (Block->Media->MediaId != mEspNv.MediaId) ||
      !CompareGuid (&Partition->Info.Gpt.UniquePartitionGUID, &mEspNv.Partition) ||
      (PathSize != mEspNv.PathSize) || (CompareMem (Path, mEspNv.Path, PathSize) != 0)) {
    return EFI_NO_MEDIA;
  }
  if (Create) {
    if (Block->Media->ReadOnly) {
      return EFI_WRITE_PROTECTED;
    }
    Mode |= EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE;
  }
  Status = Fs->OpenVolume (Fs, &Root);
  if (Status == EFI_SUCCESS) {
    Status = Root->Open (Root, &Efi, L"EFI", Mode, Create ? EFI_FILE_DIRECTORY : 0);
  }
  if (Status == EFI_SUCCESS) {
    Status = Efi->Open (Efi, Directory, L"Crane", Mode, Create ? EFI_FILE_DIRECTORY : 0);
  }
  Status = EspClose (Efi, Status);
  Status = EspClose (Root, Status);
  if ((Status != EFI_SUCCESS) && (*Directory != NULL)) {
    Status = EspClose (*Directory, Status);
    *Directory = NULL;
  }
  return Status;
}

STATIC EFI_STATUS
EspReadSlot (IN UINTN Slot, OUT ESP_NV_HEADER *Header)
{
  EFI_FILE_PROTOCOL *Directory = NULL;
  EFI_FILE_PROTOCOL *File = NULL;
  EFI_STATUS Status;
  ESP_NV_HEADER Checked;
  UINTN Count;
  UINT8 Tail;
  Status = EspOpenDirectory (FALSE, &Directory);
  if (Status == EFI_SUCCESS) {
    Status = Directory->Open (Directory, &File, mNames[Slot], EFI_FILE_MODE_READ, 0);
  }
  if (Status == EFI_SUCCESS) {
    Count = sizeof (Checked);
    Status = File->Read (File, &Count, &Checked);
    if ((Status == EFI_SUCCESS) && (Count != sizeof (Checked))) {
      Status = EFI_VOLUME_CORRUPTED;
    }
  }
  if (Status == EFI_SUCCESS) {
    CopyMem (Header, &Checked, sizeof (Checked));
    Checked.HeaderCrc = 0;
    if ((Header->Signature != ESP_NV_SIGNATURE) || (Header->Revision != 1) ||
        (Header->HeaderSize != sizeof (*Header)) || (Header->StoreSize != mEspNv.StoreSize) ||
        (Header->Flags != 0) || (Header->Generation == 0) ||
        !EspAll (Header->Reserved, sizeof (Header->Reserved), 0) ||
        (EspCrc (&Checked, sizeof (Checked)) != Header->HeaderCrc)) {
      Status = EFI_VOLUME_CORRUPTED;
    }
  }
  if (Status == EFI_SUCCESS) {
    Count = mEspNv.StoreSize;
    Status = File->Read (File, &Count, mEspNv.Work + sizeof (*Header));
    if ((Status == EFI_SUCCESS) && (Count != mEspNv.StoreSize)) {
      Status = EFI_VOLUME_CORRUPTED;
    }
  }
  if (Status == EFI_SUCCESS) {
    Count = 1;
    Status = File->Read (File, &Count, &Tail);
    if ((Status == EFI_SUCCESS) && ((Count != 0) ||
        (EspCrc (mEspNv.Work + sizeof (*Header), mEspNv.StoreSize) != Header->PayloadCrc) ||
        !EspStoreValid (mEspNv.Work + sizeof (*Header), mEspNv.StoreSize))) {
      Status = EFI_VOLUME_CORRUPTED;
    }
  }
  Status = EspClose (File, Status);
  return EspClose (Directory, Status);
}

EFI_STATUS EFIAPI
VariablePersistenceLoad (IN OUT VOID *Store, IN UINTN Size)
{
  EFI_STATUS Status;
  ESP_NV_HEADER Header;
  UINTN Slot;
  if (EfiAtRuntime ()) {
    return EFI_UNSUPPORTED;
  }
  if (!EspStoreValid (Store, Size)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = EspEnter ();
  if (Status != EFI_SUCCESS) {
    return Status;
  }
  if (mEspNv.Loaded) {
    VariablePersistenceEnd ();
    return EFI_ALREADY_STARTED;
  }
  mEspNv.Work = AllocatePool (Size + sizeof (Header));
  mEspNv.LastStore = AllocatePool (Size);
  if ((mEspNv.Work == NULL) || (mEspNv.LastStore == NULL)) {
    if (mEspNv.Work != NULL) { FreePool (mEspNv.Work); mEspNv.Work = NULL; }
    if (mEspNv.LastStore != NULL) { FreePool (mEspNv.LastStore); mEspNv.LastStore = NULL; }
    VariablePersistenceEnd ();
    return EFI_OUT_OF_RESOURCES;
  }
  mEspNv.StoreSize = Size;
  EspConnect (&gEdkiiUfsHostControllerProtocolGuid);
  EspConnect (&gEfiExtScsiPassThruProtocolGuid);
  EspConnect (&gEfiScsiPassThruProtocolGuid);
  EspConnect (&gEmbeddedMmcHostProtocolGuid);
  EspConnect (&gEfiBlockIoProtocolGuid);
  Status = EspSelect ();
  if (Status != EFI_SUCCESS) {
    FreePool (mEspNv.Work); mEspNv.Work = NULL;
    FreePool (mEspNv.LastStore); mEspNv.LastStore = NULL;
    if (mEspNv.Path != NULL) { FreePool (mEspNv.Path); mEspNv.Path = NULL; }
    mEspNv.Selected = FALSE;
    VariablePersistenceEnd ();
    return Status;
  }
  if (mEspNv.Selected) {
    for (Slot = 0; Slot < 2; ++Slot) {
      Status = EspReadSlot (Slot, &Header);
      if ((Status == EFI_SUCCESS) &&
          (!mEspNv.HaveSnapshot || (Header.Generation > mEspNv.Generation))) {
        CopyMem (mEspNv.LastStore, mEspNv.Work + sizeof (Header), Size);
        mEspNv.Generation = Header.Generation;
        mEspNv.Slot = Slot;
        mEspNv.HaveSnapshot = TRUE;
      }
    }
  }
  if (mEspNv.HaveSnapshot) {
    CopyMem (Store, mEspNv.LastStore, Size);
    DEBUG ((DEBUG_INFO, "ESP variables: restored generation %Lu slot %u\n", mEspNv.Generation, (UINT32)mEspNv.Slot));
  } else {
    DEBUG ((DEBUG_WARN, "ESP variables: %a; using formatted defaults\n", mEspNv.Selected ? "no valid snapshot" : "no GPT ESP"));
  }
  mEspNv.Loaded = TRUE;
  VariablePersistenceEnd ();
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI
VariablePersistenceBegin (VOID)
{
  if (EfiAtRuntime ()) {
    return EFI_SUCCESS;
  }
  if (!mEspNv.Loaded) {
    return EFI_NOT_READY;
  }
  return EspEnter ();
}

STATIC EFI_STATUS
EspWriteExact (IN EFI_FILE_PROTOCOL *File, IN CONST VOID *Buffer, IN UINTN Size)
{
  UINTN Count = Size;
  EFI_STATUS Status = File->Write (File, &Count, (VOID *)Buffer);
  if ((Status == EFI_SUCCESS) && (Count != Size)) {
    Status = EFI_DEVICE_ERROR;
  }
  return Status;
}

EFI_STATUS EFIAPI
VariablePersistenceCommit (IN CONST VOID *Store, IN UINTN Size)
{
  EFI_FILE_PROTOCOL *Directory = NULL;
  EFI_FILE_PROTOCOL *File = NULL;
  ESP_NV_HEADER Header;
  ESP_NV_HEADER Empty;
  EFI_STATUS Status;
  UINTN Slot;
  if (EfiAtRuntime ()) {
    return EFI_SUCCESS;
  }
  if (EfiGetCurrentTpl () > TPL_CALLBACK) {
    return EFI_UNSUPPORTED;
  }
  if (!mEspNv.Loaded || !mEspNv.Busy) {
    return EFI_NOT_READY;
  }
  if ((Size != mEspNv.StoreSize) || !EspStoreValid (Store, Size)) {
    return EFI_INVALID_PARAMETER;
  }
  if (!mEspNv.Selected) {
    return EFI_SUCCESS;
  }
  if (mEspNv.FailedClose) {
    return EFI_DEVICE_ERROR;
  }
  if (mEspNv.HaveSnapshot && (CompareMem (Store, mEspNv.LastStore, Size) == 0)) {
    return EFI_SUCCESS;
  }
  if (mEspNv.Generation == MAX_UINT64) {
    return EFI_OUT_OF_RESOURCES;
  }
  Slot = mEspNv.HaveSnapshot ? 1U - mEspNv.Slot : 0;
  ZeroMem (&Header, sizeof (Header));
  ZeroMem (&Empty, sizeof (Empty));
  Header.Signature = ESP_NV_SIGNATURE;
  Header.Revision = 1;
  Header.HeaderSize = sizeof (Header);
  Header.StoreSize = (UINT32)Size;
  Header.Generation = mEspNv.Generation + 1U;
  CopyMem (mEspNv.Work + sizeof (Header), Store, Size);
  Header.PayloadCrc = EspCrc (mEspNv.Work + sizeof (Header), Size);
  Header.HeaderCrc = EspCrc (&Header, sizeof (Header));
  Status = EspOpenDirectory (TRUE, &Directory);
  if (Status == EFI_SUCCESS) {
    Status = Directory->Open (Directory, &File, mNames[Slot], EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (Status == EFI_SUCCESS) {
      Status = File->Delete (File);
      File = NULL; // Delete closes the handle even on WARN_DELETE_FAILURE.
      if ((Status != EFI_SUCCESS) && !EFI_ERROR (Status)) {
        Status = EFI_DEVICE_ERROR;
      }
    } else if (Status == EFI_NOT_FOUND) {
      Status = EFI_SUCCESS;
    }
  }
  if (Status == EFI_SUCCESS) {
    Status = Directory->Open (Directory, &File, mNames[Slot], EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  }
  // An incomplete payload always has an invalid header. The valid header is
  // published only after payload Flush has acknowledged the complete bytes.
  if (Status == EFI_SUCCESS) { Status = EspWriteExact (File, &Empty, sizeof (Empty)); }
  if (Status == EFI_SUCCESS) { Status = EspWriteExact (File, mEspNv.Work + sizeof (Header), Size); }
  if (Status == EFI_SUCCESS) { Status = File->Flush (File); }
  if (Status == EFI_SUCCESS) { Status = File->SetPosition (File, 0); }
  if (Status == EFI_SUCCESS) { Status = EspWriteExact (File, &Header, sizeof (Header)); }
  if (Status == EFI_SUCCESS) { Status = File->Flush (File); }
  Status = EspClose (File, Status);
  if (Status == EFI_SUCCESS) { Status = Directory->Flush (Directory); }
  Status = EspClose (Directory, Status);
  if (Status == EFI_SUCCESS) {
    CopyMem (mEspNv.LastStore, mEspNv.Work + sizeof (Header), Size);
    mEspNv.Generation = Header.Generation;
    mEspNv.Slot = Slot;
    mEspNv.HaveSnapshot = TRUE;
  }
  return (Status == EFI_SUCCESS || EFI_ERROR (Status)) ? Status : EFI_DEVICE_ERROR;
}

VOID EFIAPI
VariablePersistenceEnd (VOID)
{
  EFI_TPL OldTpl;
  if (EfiAtRuntime ()) {
    return;
  }
  OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
  mEspNv.Busy = FALSE;
  gBS->RestoreTPL (OldTpl);
}
