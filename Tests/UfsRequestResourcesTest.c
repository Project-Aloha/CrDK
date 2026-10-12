/** Actual release helper with delayed provider copyback and caller destruction. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include "UfsPassThru.h"

static VOID *Staging, *Descriptor;
static UINT8 DataToken, DescriptorToken;
static unsigned DataUnmaps, DescriptorUnmaps, DescriptorFrees, StagingFrees;
static unsigned FailStep;

VOID *EFIAPI CopyMem (VOID *To, CONST VOID *From, UINTN Size) { return memcpy (To, From, Size); }
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Size) { return memset (Buffer, 0, Size); }

static EFI_STATUS EFIAPI Unmap (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This, VOID *Mapping)
{
  if (Mapping == &DataToken) {
    if (FailStep == 1) return EFI_DEVICE_ERROR;
    assert (DataUnmaps++ == 0);
    // SmmuCr performs READ copyback only after successful revocation.
    // The host address must still be the retained driver-owned allocation.
    assert (Staging != NULL);
    memset (Staging, 0x5a, 8);
  } else {
    assert (Mapping == &DescriptorToken);
    if (FailStep == 2) return EFI_DEVICE_ERROR;
    assert (DescriptorUnmaps++ == 0);
  }
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI FreeBuffer (EDKII_UFS_HOST_CONTROLLER_PROTOCOL *This,
  UINTN Pages, VOID *Buffer)
{
  assert (Pages == 1);
  if (Buffer == Descriptor) {
    if (FailStep == 3) return EFI_DEVICE_ERROR;
    assert (DescriptorFrees++ == 0);
    free (Descriptor);
    Descriptor = NULL;
  } else {
    assert (Buffer == Staging && ((UINT8 *)Staging)[0] == 0);
    if (FailStep == 4) return EFI_DEVICE_ERROR;
    assert (StagingFrees++ == 0);
    free (Staging);
    Staging = NULL;
  }
  return EFI_SUCCESS;
}

#include "UfsRequestRelease.inc"

int main (void)
{
  EDKII_UFS_HOST_CONTROLLER_PROTOCOL Host = { .Unmap = Unmap, .FreeBuffer = FreeBuffer };
  UFS_PASS_THRU_PRIVATE_DATA Owner = { .UfsHostController = &Host };
  for (unsigned Case = 0; Case <= 4; Case++) {
    UINT8 *Caller = malloc (8);
    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet = {
      .InDataBuffer = Caller, .InTransferLength = 8,
      .DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ
    };
    Staging = calloc (1, EFI_PAGE_SIZE);
    Descriptor = calloc (1, EFI_PAGE_SIZE);
    assert (Caller && Staging && Descriptor);
    memset (Caller, 0xaa, 8);
    UFS_PASS_THRU_TRANS_REQ Request = {
      .Packet = &Packet, .Submitted = TRUE, .ResponseValidated = TRUE,
      .DataBufMapping = &DataToken, .CmdDescMapping = &DescriptorToken,
      .CmdDescHost = Descriptor, .CmdDescSize = 128,
      .AlignedDataBuf = Staging, .AlignedDataBufSize = 8
    };
    DataUnmaps = DescriptorUnmaps = DescriptorFrees = StagingFrees = 0;
    FailStep = Case;
    assert (UfsReleaseRequestResources (&Owner, &Request, TRUE) == EFI_NOT_READY);
    assert (DataUnmaps == 0 && Caller[0] == 0xaa);
    Request.DmaFenced = TRUE;
    EFI_STATUS Status = UfsReleaseRequestResources (&Owner, &Request, TRUE);
    if (Case == 0) {
      assert (Status == EFI_SUCCESS && Caller[0] == 0x5a);
      free (Caller);
    } else {
      assert (Status == EFI_DEVICE_ERROR);
      if (Case == 1) assert (Caller[0] == 0xaa && Request.DataBufMapping != NULL);
      // Synchronous failure returns ownership to the caller. Retry is allowed
      // only after detaching its packet; ASan catches accidental later access.
      Request.Packet = NULL;
      free (Caller);
      memset (&Packet, 0xcc, sizeof (Packet));
      FailStep = 0;
      assert (UfsReleaseRequestResources (&Owner, &Request, FALSE) == EFI_SUCCESS);
    }
    assert (!Request.DataBufMapping && !Request.CmdDescMapping);
    assert (!Request.CmdDescHost && !Request.AlignedDataBuf);
    assert (DataUnmaps == 1 && DescriptorUnmaps == 1 && DescriptorFrees == 1 && StagingFrees == 1);
  }
  // A request borrows one slot of the controller's permanent descriptor pool.
  // The pool must survive both a failed fence and failed data revocation.
  for (unsigned Case = 0; Case < 2; Case++) {
    UINT8 Caller[8] = {0};
    EFI_EXT_SCSI_PASS_THRU_SCSI_REQUEST_PACKET Packet = {
      .InDataBuffer = Caller, .InTransferLength = 8,
      .DataDirection = EFI_EXT_SCSI_DATA_DIRECTION_READ
    };
    Descriptor = calloc (1, 3072);
    Staging = calloc (1, EFI_PAGE_SIZE);
    assert (Descriptor && Staging);
    UFS_PASS_THRU_TRANS_REQ Request = {
      .Packet = &Packet, .Submitted = TRUE, .ResponseValidated = TRUE,
      .DataBufMapping = &DataToken, .CmdDescHost = Descriptor,
      .CmdDescSize = 1040, .CmdDescFromPool = TRUE,
      .AlignedDataBuf = Staging, .AlignedDataBufSize = 8
    };
    DataUnmaps = DescriptorUnmaps = DescriptorFrees = StagingFrees = 0;
    FailStep = Case;
    assert (UfsReleaseRequestResources (&Owner, &Request, TRUE) == EFI_NOT_READY);
    assert (Request.CmdDescHost == Descriptor && Request.CmdDescFromPool);
    Request.DmaFenced = TRUE;
    if (Case) {
      assert (UfsReleaseRequestResources (&Owner, &Request, TRUE) == EFI_DEVICE_ERROR);
      assert (Request.CmdDescHost == Descriptor && Request.CmdDescFromPool && !DataUnmaps);
      FailStep = 0;
    }
    assert (UfsReleaseRequestResources (&Owner, &Request, TRUE) == EFI_SUCCESS);
    assert (!Request.CmdDescHost && !Request.CmdDescFromPool && Caller[0] == 0x5a);
    assert (DataUnmaps == 1 && StagingFrees == 1 && !DescriptorUnmaps && !DescriptorFrees);
    assert (UfsReleaseRequestResources (&Owner, &Request, FALSE) == EFI_SUCCESS);
    memset (Descriptor, 0x6a, 3072); // ASan proves the shared backing is still alive.
    free (Descriptor);
    Descriptor = NULL;
  }
  puts ("UFS request release: fence gate, delayed copyback, failure retries and borrowed permanent UCD preservation PASS");
  return 0;
}
