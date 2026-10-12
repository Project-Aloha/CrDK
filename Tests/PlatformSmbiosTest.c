/** @file SMBIOS publication with optional OEM services missing.
  SPDX-License-Identifier: MIT
**/
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

#define _PCD_VALUE_PcdCoreCount 8
#define _PCD_GET_MODE_32_PcdCoreCount 8
#define _PCD_VALUE_PcdSystemMemoryBase 0x80000000ULL
#define _PCD_VALUE_PcdSystemMemorySize 0x300000000ULL
#define _PCD_VALUE_PcdSmbiosSystemBrand "Qualcomm"
#define _PCD_VALUE_PcdSmbiosSystemModel "HDK8450"
#define _PCD_VALUE_PcdSmbiosBoardModel "HDK8450"
#define _PCD_VALUE_PcdSmbiosSystemRetailModel ""
#define _PCD_VALUE_PcdSmbiosSystemRetailSku "6"
#define _PCD_VALUE_PcdSmbiosProcessorModel "Snapdragon 8 Gen 1"
#define _PCD_VALUE_PcdSmbiosProcessorRetailModel "SM8450"
#define _PCD_VALUE_PcdSmbiosLegacyHardwareInfo FALSE
#define FixedPcdGet32(Token) _PCD_VALUE_##Token
#define FixedPcdGet64(Token) _PCD_VALUE_##Token
#define FixedPcdGetBool(Token) _PCD_VALUE_##Token
#define PcdGet32(Token) _PCD_VALUE_##Token
#define FixedPcdGetPtr(Token) _PCD_VALUE_##Token

#include "../../MUCommonCrPkg/Driver/SmBiosTableDxe/SmBiosTableDxe.c"
#include "../../MUCommonCrPkg/Driver/SmBiosTableDxe/PlatformUtils.c"
#include "../../MUCommonCrPkg/Driver/SmBiosTableDxe/MemoryUtils.c"

EFI_GUID gEfiSmbiosProtocolGuid = EFI_SMBIOS_PROTOCOL_GUID;
EFI_GUID gEfiChipInfoProtocolGuid = {1};
EFI_GUID gEfiPlatformInfoProtocolGuid = {2};
EFI_GUID gEfiHash2ProtocolGuid = {3};
EFI_GUID gEfiHashAlgorithmSha1Guid = {4};
EFI_GUID gEfiRamPartitionProtocolGuid = {5};
EFI_HANDLE gImageHandle;
static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;
static EFI_SMBIOS_PROTOCOL Smbios;
static EFI_CHIPINFO_PROTOCOL ChipInfo;
static EFI_HASH2_PROTOCOL Hash2;
static EFI_PLATFORMINFO_PROTOCOL PlatformInfoProtocol;
static BOOLEAN ChipAvailable, HashAvailable, PlatformAvailable;
static UINTN HashSize = 20;
static UINT8 Records[32][512];
static UINTN RecordCount;
static INTN FailType = -1;

VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst, Src, Size); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN Size) { return memset(Dst, 0, Size); }
VOID *EFIAPI AllocateZeroPool(UINTN Size) { return calloc(1, Size); }
VOID EFIAPI FreePool(VOID *Buffer) { free(Buffer); }
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *String) { return strlen(String); }
UINTN EFIAPI AsciiStrSize(CONST CHAR8 *String) { return strlen(String) + 1; }
RETURN_STATUS EFIAPI AsciiStrnCpyS(CHAR8 *Dst, UINTN Size, CONST CHAR8 *Src, UINTN Count) {
  assert(Size > Count);
  memcpy(Dst, Src, Count);
  Dst[Count] = 0;
  return RETURN_SUCCESS;
}
UINTN EFIAPI AsciiSPrint(CHAR8 *Dst, UINTN Size, CONST CHAR8 *Format, ...) {
  va_list Args;
  int Result;
  va_start(Args, Format);
  if (strcmp(Format, "%Lu") == 0) {
    Result = snprintf(Dst, Size, "%llu", (unsigned long long)va_arg(Args, UINT64));
  } else {
    assert(strcmp(Format, "%d.%d") == 0);
    unsigned Major = va_arg(Args, int), Minor = va_arg(Args, int);
    Result = snprintf(Dst, Size, "%u.%u", Major, Minor);
  }
  va_end(Args);
  return (UINTN)Result;
}
EFI_STATUS EFIAPI GetUefiVersionStringAscii(CHAR8 *Buffer, UINTN *Length) {
  assert(*Length >= sizeof("001.2026.40.00"));
  memcpy(Buffer, "001.2026.40.00", sizeof("001.2026.40.00"));
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI GetBuildDateStringAscii(CHAR8 *Buffer, UINTN *Length) {
  assert(*Length >= sizeof("10/05/2026"));
  memcpy(Buffer, "10/05/2026", sizeof("10/05/2026"));
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestLocate(EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  if (Guid == &gEfiSmbiosProtocolGuid) *Interface = &Smbios;
  else if (Guid == &gEfiChipInfoProtocolGuid && ChipAvailable) *Interface = &ChipInfo;
  else if (Guid == &gEfiHash2ProtocolGuid && HashAvailable) *Interface = &Hash2;
  else if (Guid == &gEfiPlatformInfoProtocolGuid && PlatformAvailable) *Interface = &PlatformInfoProtocol;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestAdd(CONST EFI_SMBIOS_PROTOCOL *This, EFI_HANDLE Producer,
  EFI_SMBIOS_HANDLE *Handle, EFI_SMBIOS_TABLE_HEADER *Record) {
  if (Record->Type == FailType) return EFI_DEVICE_ERROR;
  UINT8 *End = (UINT8 *)Record + Record->Length;
  while (End[0] != 0 || End[1] != 0) ++End;
  UINTN Size = End + 2 - (UINT8 *)Record;
  assert(RecordCount < ARRAY_SIZE(Records) && Size <= sizeof(Records[0]));
  memcpy(Records[RecordCount], Record, Size);
  *Handle = (EFI_SMBIOS_HANDLE)(0x100 + RecordCount++);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestSerial(EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoSerialNumType *Serial) {
  *Serial = 1234567890U;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestFamily(EFI_CHIPINFO_PROTOCOL *This, EFIChipInfoFamilyType *Family) {
  *Family = (EFIChipInfoFamilyType)127;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestPlatform(EFI_PLATFORMINFO_PROTOCOL *This,
  EFI_PLATFORMINFO_PLATFORM_INFO_TYPE *Info) {
  ZeroMem(Info, sizeof(*Info));
  Info->platform = EFI_PLATFORMINFO_TYPE_HDK;
  Info->subtype = 1;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestHashSize(CONST EFI_HASH2_PROTOCOL *This, CONST EFI_GUID *Algorithm,
  UINTN *Size) { *Size = HashSize; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI TestHashInit(CONST EFI_HASH2_PROTOCOL *This, CONST EFI_GUID *Algorithm) {
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestHashUpdate(CONST EFI_HASH2_PROTOCOL *This, CONST UINT8 *Data, UINTN Size) {
  UINT8 Expected[16] = {0};
  UINT32 Serial = 1234567890U;
  memcpy(Expected, &Serial, sizeof(Serial));
  assert(Size == sizeof(Expected) && memcmp(Data, Expected, Size) == 0);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestHashFinal(CONST EFI_HASH2_PROTOCOL *This, EFI_HASH2_OUTPUT *Hash) {
  for (UINTN i = 0; i < sizeof(Hash->Sha1Hash); ++i) Hash->Sha1Hash[i] = (UINT8)i;
  return EFI_SUCCESS;
}
static EFI_SMBIOS_TABLE_HEADER *FindRecord(UINT8 Type) {
  for (UINTN i = 0; i < RecordCount; ++i)
    if (((EFI_SMBIOS_TABLE_HEADER *)Records[i])->Type == Type) return (VOID *)Records[i];
  return NULL;
}
static CHAR8 *RecordString(EFI_SMBIOS_TABLE_HEADER *Header, UINT8 Index) {
  assert(Header != NULL && Index != 0);
  CHAR8 *String = (CHAR8 *)Header + Header->Length;
  while (--Index != 0) { assert(*String != 0); String += strlen(String) + 1; }
  return String;
}
int main(void) {
  BootServices.LocateProtocol = TestLocate;
  Smbios.Add = TestAdd;
  ChipInfo.GetSerialNumber = TestSerial;
  ChipInfo.GetChipFamily = TestFamily;
  Hash2.GetHashSize = TestHashSize;
  Hash2.HashInit = TestHashInit;
  Hash2.HashUpdate = TestHashUpdate;
  Hash2.HashFinal = TestHashFinal;
  PlatformInfoProtocol.GetPlatformInfo = TestPlatform;

  /* Basic identity publishes with every optional OEM service absent. */
  assert(SmBiosTableDxeInitialize(NULL, NULL) == EFI_SUCCESS);
  assert(RecordCount == 7 && FindRecord(7) == NULL && FindRecord(19) == NULL);
  SMBIOS_TABLE_TYPE1 *System = (VOID *)FindRecord(1);
  assert(strcmp(RecordString(&System->Hdr, System->Manufacturer), "Qualcomm") == 0);
  assert(strcmp(RecordString(&System->Hdr, System->ProductName), "HDK8450") == 0);
  assert(strcmp(RecordString(&System->Hdr, System->Family), "SM8450") == 0);
  assert(strcmp(RecordString(&System->Hdr, System->SerialNumber), UNKNOWN_STRING_NAME) == 0);
  EFI_GUID Unavailable = {0};
  assert(memcmp(&System->Uuid, &Unavailable, sizeof(Unavailable)) == 0);
  SMBIOS_TABLE_TYPE4 *Processor = (VOID *)FindRecord(4);
  assert(Processor->CoreCount == 8 && Processor->MaxSpeed == 0 && Processor->CurrentSpeed == 0);
  assert(Processor->L1CacheHandle == 0xFFFF && Processor->L2CacheHandle == 0xFFFF && Processor->L3CacheHandle == 0xFFFF);
  SMBIOS_TABLE_TYPE16 *Array = (VOID *)FindRecord(16);
  SMBIOS_TABLE_TYPE17 *Memory = (VOID *)FindRecord(17);
  assert(Array->MaximumCapacity == 0x80000000 && Array->ExtendedMaximumCapacity == 0x300000000ULL);
  assert(Memory->MemoryArrayHandle == 0x105 && Memory->Size == 12288);
  assert(Memory->MemoryType == MemoryTypeUnknown && Memory->Speed == 0);
  assert(strcmp(RecordString(&Memory->Hdr, Memory->Manufacturer), UNKNOWN_STRING_NAME) == 0);
  assert(strcmp(RecordString(&Memory->Hdr, Memory->PartNumber), UNKNOWN_STRING_NAME) == 0);

  RecordCount = 0;
  FailType = 1;
  assert(SmBiosTableDxeInitialize(NULL, NULL) == EFI_DEVICE_ERROR && RecordCount == 1);
  RecordCount = 0;
  FailType = 16;
  assert(SmBiosTableDxeInitialize(NULL, NULL) == EFI_DEVICE_ERROR && FindRecord(17) == NULL);
  FailType = -1;
  assert(MemDevInfoUpdateSmbiosType17(64ULL << 30) == EFI_SUCCESS);
  assert(mMemDevInfoType17.Size == 0x7FFF && mMemDevInfoType17.ExtendedSize == 65536);
  assert(LogSmbiosData((VOID *)&mPhyMemArrayInfoType16, NULL, NULL) == EFI_SUCCESS);

  ChipAvailable = TRUE;
  CHAR8 Serial[PLATFORM_TYPE_STRING_MAX_SIZE];
  assert(RetrieveSerialNumber(Serial) == EFI_SUCCESS && strcmp(Serial, "1234567890") == 0);
  UINT8 Uuid[16];
  memset(Uuid, 0xA5, sizeof(Uuid));
  assert(GetUUIDFromEFIChipInfoSerialNumType(Uuid, sizeof(Uuid)) == EFI_NOT_FOUND);
  HashAvailable = TRUE;
  HashSize = 32;
  assert(GetUUIDFromEFIChipInfoSerialNumType(Uuid, sizeof(Uuid)) == EFI_UNSUPPORTED && Uuid[0] == 0xA5);
  HashSize = 20;
  assert(GetUUIDFromEFIChipInfoSerialNumType(Uuid, sizeof(Uuid)) == EFI_SUCCESS);
  for (UINTN i = 0; i < sizeof(Uuid); ++i) assert(Uuid[i] == i + 4);
  assert(GetUUIDFromEFIChipInfoSerialNumType(Uuid, 0) == EFI_INVALID_PARAMETER);
  PlatformAvailable = TRUE;
  CHAR8 Platform[PLATFORM_TYPE_STRING_MAX_SIZE];
  assert(RetrievePlatformName(Platform) == EFI_SUCCESS && strcmp(Platform, "HHG") == 0);
  puts("SMBIOS passed: optional services, HDK identity, unknown hardware, failures, serial and UUID bounds");
  return 0;
}
