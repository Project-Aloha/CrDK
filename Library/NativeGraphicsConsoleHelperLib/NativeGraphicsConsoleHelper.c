/** @file
  Console policy for fixed-mode native HDK displays.
  SPDX-License-Identifier: MIT
**/
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/GraphicsConsoleHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_STATUS
Exact (EFI_STATUS Status) {
  return Status == EFI_SUCCESS || EFI_ERROR (Status) ? Status : EFI_NOT_READY;
}

EFI_STATUS
EFIAPI
SetGraphicsConsoleMode (GRAPHICS_CONSOLE_MODE Requested)
{
  EFI_STATUS Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop = NULL;
  EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *Text = NULL;
  UINTN Selected = MAX_UINTN, Columns = 0, Rows = 0;
  UINT64 SelectedCells = 0;

  if (Requested != GCM_LOW_RES && Requested != GCM_NATIVE_RES)
    return EFI_INVALID_PARAMETER;
  /* Do not mask an unconnected console by locating an unrelated physical
     GOP. The boot manager must connect GraphicsConsole and ConSplitter. */
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle,
             &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (Status != EFI_SUCCESS) return Exact (Status);
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle,
             &gEfiSimpleTextOutProtocolGuid, (VOID **)&Text);
  if (Status != EFI_SUCCESS) return Exact (Status);
  if (Gop == NULL || Gop->Mode == NULL || Gop->Mode->Info == NULL ||
      Gop->Mode->SizeOfInfo < sizeof (*Gop->Mode->Info) ||
      Gop->Mode->Info->HorizontalResolution == 0 || Gop->Mode->Info->VerticalResolution == 0 ||
      Gop->Mode->MaxMode == 0 || Gop->Mode->Mode >= Gop->Mode->MaxMode ||
      Text == NULL || Text->Mode == NULL || Text->QueryMode == NULL ||
      Text->SetMode == NULL || Text->Mode->MaxMode <= 0) return EFI_NOT_READY;

  /* These panels expose a fixed native timing, not VGA. Keep that timing
     and select a real negotiated text mode. Serial+graphics intersections
     need not contain width/glyph-width by height/glyph-height. */
  for (UINTN Index = 0; Index < (UINTN)Text->Mode->MaxMode; ++Index) {
    UINTN C, R;
    Status = Text->QueryMode (Text, Index, &C, &R);
    if (Status != EFI_SUCCESS || C == 0 || R == 0 || C > MAX_UINT32 || R > MAX_UINT32)
      continue;
    UINT64 Cells = (UINT64)C * R;
    if (Selected == MAX_UINTN ||
        (Requested == GCM_LOW_RES ? Cells < SelectedCells : Cells > SelectedCells)) {
      Selected = Index; Columns = C; Rows = R; SelectedCells = Cells;
    }
  }
  if (Selected == MAX_UINTN) return EFI_UNSUPPORTED;
  Status = Text->SetMode (Text, Selected);
  if (Status != EFI_SUCCESS) return Exact (Status);

  /* Do not overwrite a failed SetMode with PCD success, cache guessed text
     geometry, or disconnect every serial/graphical output to change modes. */
  Status = PcdSet32S (PcdVideoHorizontalResolution, Gop->Mode->Info->HorizontalResolution);
  if (Status != EFI_SUCCESS) return Exact (Status);
  Status = PcdSet32S (PcdVideoVerticalResolution, Gop->Mode->Info->VerticalResolution);
  if (Status != EFI_SUCCESS) return Exact (Status);
  Status = PcdSet32S (PcdConOutColumn, (UINT32)Columns);
  if (Status != EFI_SUCCESS) return Exact (Status);
  return Exact (PcdSet32S (PcdConOutRow, (UINT32)Rows));
}
