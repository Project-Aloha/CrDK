/* SPDX-License-Identifier: MIT */
/* Actual ScsiDisk -> UefiScsiLib -> native UFS HCI wire composition. */
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "ScsiDisk.h"
#include "UfsPassThruHci.c"
#include "UfsRequestResources.inc"
#include "ScsiInquiry.inc"

EFI_BOOT_SERVICES *gBS;
EDKII_UFS_HC_PLATFORM_PROTOCOL *mUfsHcPlatform;
static UFS_PASS_THRU_PRIVATE_DATA Owner;
static UINT32 Doorbell;
static VOID *DmaHost;
static UINTN DmaBytes, LiveBuffers, LiveMappings, LivePools;
static unsigned StandardRequests, SupportedRequests, BlockRequests, Resets;
static UINT32 LastBlockExpected, LastBlockPrdBytes;
static UINT8 LastBlockAllocation, LastFlags;
static UINT32 LastResidual;
static BOOLEAN ForceOverflow;

VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) {}
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { return FALSE; }
BOOLEAN EFIAPI DebugCodeEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Text) { abort(); }
VOID *EFIAPI ZeroMem (VOID *P, UINTN N) { return memset (P, 0, N); }
VOID *EFIAPI CopyMem (VOID *P, CONST VOID *Q, UINTN N) { return memcpy (P, Q, N); }
VOID *EFIAPI AllocateZeroPool (UINTN N) { LivePools++; return calloc (1, N); }
VOID EFIAPI FreePool (VOID *P) { assert (LivePools); LivePools--; free (P); }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINT64 EFIAPI DivU64x32 (UINT64 A, UINT32 B) { return A / B; }
UINT64 EFIAPI RShiftU64 (UINT64 A, UINTN B) { return A >> B; }
UINTN EFIAPI MicroSecondDelay (UINTN N) { return N; }
VOID *EFIAPI AllocateAlignedPages (UINTN Pages, UINTN Alignment) {
  VOID *P = NULL;
  assert (Alignment == 1 && posix_memalign (&P, EFI_PAGE_SIZE, Pages * EFI_PAGE_SIZE) == 0);
  LiveBuffers++; return P;
}
VOID EFIAPI FreeAlignedPages (VOID *P, UINTN Pages) { assert (LiveBuffers); LiveBuffers--; free (P); }
LIST_ENTRY *EFIAPI InitializeListHead (LIST_ENTRY *L) { L->ForwardLink = L->BackLink = L; return L; }
LIST_ENTRY *EFIAPI InsertTailList (LIST_ENTRY *H, LIST_ENTRY *E) {
  E->BackLink = H->BackLink; E->ForwardLink = H; H->BackLink->ForwardLink = E; H->BackLink = E; return H;
}
LIST_ENTRY *EFIAPI RemoveEntryList (CONST LIST_ENTRY *E) {
  E->BackLink->ForwardLink = E->ForwardLink; E->ForwardLink->BackLink = E->BackLink; return E->ForwardLink;
}
static EFI_TPL EFIAPI Raise (EFI_TPL T) { return TPL_APPLICATION; }
static VOID EFIAPI Restore (EFI_TPL T) {}
static EFI_STATUS EFIAPI Allocate (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EFI_ALLOCATE_TYPE T,
  EFI_MEMORY_TYPE M, UINTN Pages, VOID **P, UINT64 Attributes) {
  *P = aligned_alloc (EFI_PAGE_SIZE, Pages * EFI_PAGE_SIZE); assert (*P);
  memset (*P, 0, Pages * EFI_PAGE_SIZE); LiveBuffers++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Free (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, UINTN Pages, VOID *P) {
  assert (LiveBuffers); LiveBuffers--; free (P); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Map (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, EDKII_UFS_HOST_CONTROLLER_OPERATION Op,
  VOID *P, UINTN *Bytes, EFI_PHYSICAL_ADDRESS *Device, VOID **Token) {
  assert (Op == EdkiiUfsHcOperationBusMasterWrite && !LiveMappings);
  DmaHost = P; DmaBytes = *Bytes; *Device = (UINTN)P; *Token = P; LiveMappings++; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H, VOID *Token) {
  assert (!Doorbell && LiveMappings && Token == DmaHost);
  LiveMappings--; DmaHost = NULL; return EFI_SUCCESS;
}
static UINT32 Be32 (CONST UINT8 *P) {
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) | ((UINT32)P[2] << 8) | P[3];
}
static EFI_STATUS EFIAPI Read (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Offset, UINTN Count, VOID *P) {
  *(UINT32 *)P = Offset == UFS_HC_UTRLDBR_OFFSET ? Doorbell :
    Offset == UFS_HC_UTRLRSR_OFFSET ? 1 : 0;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *H,
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL_WIDTH W, UINT64 Offset, UINTN Count, VOID *P) {
  if (Offset != UFS_HC_UTRLDBR_OFFSET) return EFI_SUCCESS;
  Doorbell = *(UINT32 *)P; assert (Doorbell == 1 && LiveMappings);
  UTP_TRD *T = Owner.UtpTrlBase;
  UINT8 *Cmd = Owner.UtpUcdBase, *Rsp = Cmd + T->RuO * 4;
  UINT32 Expected = Be32 (Cmd + 12), *Prd = (VOID *)(Cmd + T->PrdtO * 4);
  UINT64 Address = ((UINT64)Prd[1] << 32) | Prd[0];
  assert (Cmd[0] == 1 && Cmd[1] == 0x41 && Cmd[16] == 0x12 && T->Dd == 2);
  assert (T->PrdtL == 1 && Address == (UINTN)DmaHost && Prd[2] == 0 && Prd[3] + 1 == DmaBytes);
  assert (Expected <= DmaBytes && DmaBytes - Expected < 4 && Cmd[20] == (Expected > 255 ? 255 : Expected));
  UINT8 Data[96] = {0}; UINT32 Actual;
  if (Cmd[17] == 0) {
    assert (Expected == 96); Actual = 96; StandardRequests++;
  } else if (Cmd[18] == 0) {
    Data[1] = 0; Data[3] = 2; Data[4] = 0; Data[5] = 0xb0; Actual = 6; SupportedRequests++;
  } else {
    assert (Cmd[17] == 1 && Cmd[18] == 0xb0 && (Expected == 60 || Expected == 64));
    /* Hand-coded Linux/SPC wire bytes, independent of the MU packed type:
       B0 page length60, be64 WRITE SAME at36, atomic fields44..63. */
    Data[1] = 0xb0; Data[3] = 0x3c;
    Data[6] = 0x12; Data[7] = 0x34;
    Data[20] = 1; Data[21] = 2; Data[22] = 3; Data[23] = 4;
    Data[27] = 7; Data[31] = 8; Data[32] = 0x80; Data[35] = 9;
    for (UINTN I = 36; I < 64; I++) Data[I] = (UINT8)I;
    Actual = ForceOverflow ? 68 : 64; BlockRequests++;
    LastBlockExpected = Expected; LastBlockAllocation = Cmd[20]; LastBlockPrdBytes = Prd[3] + 1;
  }
  UINT32 Residual = Actual > Expected ? Actual - Expected : Expected - Actual;
  LastFlags = Actual > Expected ? 0x40 : Actual < Expected ? 0x20 : 0;
  LastResidual = Residual;
  memset (Rsp, 0, 52); Rsp[0] = 0x21; Rsp[1] = LastFlags; Rsp[2] = Cmd[2]; Rsp[3] = Cmd[3];
  Rsp[12] = (UINT8)(Residual >> 24); Rsp[13] = (UINT8)(Residual >> 16);
  Rsp[14] = (UINT8)(Residual >> 8); Rsp[15] = (UINT8)Residual;
  memcpy (DmaHost, Data, Actual > Expected ? Expected : Actual);
  T->Ocs = 0; Doorbell = 0; return EFI_SUCCESS;
}
/* SCSI IO and extended pass-through packet layouts have the same ABI here.
   The glue delegates every command to the actual native HCI and copies the
   output status/actual-length packet back; it performs no response validation. */
static EFI_STATUS EFIAPI Execute (EFI_SCSI_IO_PROTOCOL *Io,
  EFI_SCSI_IO_SCSI_REQUEST_PACKET *Packet, EFI_EVENT Event) {
  EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Ext;
  _Static_assert (sizeof (Ext) == sizeof (*Packet), "SCSI packet ABI");
  memcpy (&Ext, Packet, sizeof (Ext));
  EFI_STATUS Status = UfsExecScsiCmds (&Owner, 0, &Ext, Event);
  memcpy (Packet, &Ext, sizeof (Ext)); return Status;
}
static EFI_STATUS EFIAPI Reset (EFI_SCSI_IO_PROTOCOL *Io) { Resets++; return EFI_UNSUPPORTED; }
EFI_STATUS ScsiDiskRequestSenseKeys (SCSI_DISK_DEV *Disk, BOOLEAN *Retry,
  EFI_SCSI_SENSE_DATA **Sense, UINTN *Count, BOOLEAN AskReset) { assert (0); return EFI_DEVICE_ERROR; }

int main (void) {
  EFI_BOOT_SERVICES Bs = {.RaiseTPL = Raise, .RestoreTPL = Restore};
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL H = {.Read = Read, .Write = Write, .Map = Map,
    .Unmap = Unmap, .AllocateBuffer = Allocate, .FreeBuffer = Free};
  UTP_TRD Trd = {0}; EFI_SCSI_IO_PROTOCOL Io = {.ExecuteScsiCommand = Execute, .IoAlign = 1,
    .ResetBus = Reset, .ResetDevice = Reset};
  SCSI_DISK_DEV Disk = {.ScsiIo = &Io, .DeviceType = EFI_SCSI_TYPE_DISK};
  BOOLEAN Retry = FALSE; Disk.BlkIo.Media = &Disk.BlkIoMedia;
  gBS = &Bs; Owner.UtpUcdBase = aligned_alloc (128, 3072); assert (Owner.UtpUcdBase);
  Owner.UtpTrlBase = &Trd; Owner.UfsHostController = &H; Owner.Nutrs = 1;
  Owner.UcdPoolSize = 3072; Owner.UcdDeviceAddress = (UINTN)Owner.UtpUcdBase;
  InitializeListHead (&Owner.Queue);
  EFI_STATUS Status = ScsiDiskInquiryDevice (&Disk, &Retry);
  fprintf (stderr, "actual B0 caller: CDB=%u UPIU=%u PRD=%u flags=%02x residual=%u status=%llx\n",
    LastBlockAllocation, LastBlockExpected, LastBlockPrdBytes, LastFlags, LastResidual, (unsigned long long)Status);
  assert (Status == EFI_SUCCESS && Disk.BlockLimitsVpdSupported && Resets == 0);
  assert (StandardRequests == 1 && SupportedRequests == 1 && BlockRequests == 1);
  assert (LastBlockExpected == 64 && LastBlockAllocation == 64 && LastBlockPrdBytes == 64);
  assert (Disk.BlkIo.Media->OptimalTransferLengthGranularity == 0x1234);
  assert (Disk.UnmapInfo.MaxLbaCnt == 0x01020304 && Disk.UnmapInfo.MaxBlkDespCnt == 7);
  assert (Disk.EraseBlock.EraseLengthGranularity == 8 && Disk.UnmapInfo.GranularityAlignment == 9);
  assert (sizeof (CR_SCSI_BLOCK_LIMITS_VPD_PAGE) == 64);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumWriteSameLength4) == 40);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumWriteSameLength1) == 43);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumAtomicTransferLength4) == 44);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, AtomicAlignment4) == 48);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, AtomicTransferLengthGranularity4) == 52);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumAtomicTransferLengthWithAtomicBoundary4) == 56);
  assert (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumAtomicBoundarySize4) == 60);
  /* A genuinely overflowing device remains an error after the size fix. */
  ForceOverflow = TRUE; Retry = FALSE;
  memset (&Disk.InquiryData, 0, sizeof (Disk.InquiryData)); Disk.BlockLimitsVpdSupported = FALSE;
  assert (ScsiDiskInquiryDevice (&Disk, &Retry) == EFI_DEVICE_ERROR);
  assert (LastFlags == 0x40 && LastResidual == 4 && !Disk.BlockLimitsVpdSupported && Resets == 1);
  assert (!LiveBuffers && !LiveMappings && !LivePools && !Owner.ReservedSlots && !Doorbell);
  free (Owner.UtpUcdBase);
  puts ("SCSI B0 actual caller/library/UFS wire: legal64 layout and discovery, overflow rejection PASS");
  return 0;
}
