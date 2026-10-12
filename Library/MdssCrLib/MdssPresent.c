/** @file SPDX-License-Identifier: MIT **/
#include <Library/MdssCrLib.h>

static BOOLEAN ValidRect(CONST CR_MDSS_RECT *R, UINT32 W, UINT32 H)
{
  return R->Left >= 0 && R->Top >= 0 && R->Right >= R->Left &&
      R->Bottom >= R->Top && (UINT32)R->Right <= W && (UINT32)R->Bottom <= H;
}

static VOID CopyBytes(volatile UINT8 *Dst, CONST volatile UINT8 *Src,
                      UINTN Count, BOOLEAN Backward)
{
  if (Backward) while (Count) { --Count; Dst[Count] = Src[Count]; }
  else { UINTN I; for (I = 0; I < Count; ++I) Dst[I] = Src[I]; }
}

CR_STATUS CrMdssPresentValidate(CONST CR_MDSS *D, CONST VOID *Source, UINTN Bytes,
                       UINT32 Pitch, CONST CR_MDSS_MOVE *Moves, UINTN Mc,
                       CONST CR_MDSS_RECT *Dirty, UINTN Dc)
{
  UINTN I;
  if (!D || !D->Initialized || Mc > CR_MDSS_MAX_RECTS ||
      Dc > CR_MDSS_MAX_RECTS || (Mc && !Moves) || (Dc && !Dirty))
    return CR_INVALID_PARAMETER;
  if (!D->Active || D->CleanupPending) return CR_NOT_READY;
  if (Dc && (!Source || (UINT64)Pitch < (UINT64)D->Mode.Width * 4 ||
      (UINT64)Pitch * D->Mode.Height > Bytes ||
      Bytes > (UINTN)-1 - (UINTN)Source)) return CR_BAD_BUFFER_SIZE;
  /* Qualify every operation before touching the destination. */
  for (I = 0; I < Mc; ++I) {
    CONST CR_MDSS_MOVE *M = &Moves[I];
    if (!ValidRect(&M->Destination, D->Mode.Width, D->Mode.Height) ||
        M->SourceX < 0 || M->SourceY < 0 ||
        (UINT64)(UINT32)M->SourceX + (UINT32)(M->Destination.Right - M->Destination.Left) > D->Mode.Width ||
        (UINT64)(UINT32)M->SourceY + (UINT32)(M->Destination.Bottom - M->Destination.Top) > D->Mode.Height)
      return CR_INVALID_PARAMETER;
  }
  for (I = 0; I < Dc; ++I)
    if (!ValidRect(&Dirty[I], D->Mode.Width, D->Mode.Height))
      return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
static VOID ApplyPresent(volatile UINT8 *Fb, UINT32 Stride, CONST VOID *Source,
                         UINT32 Pitch, CONST CR_MDSS_MOVE *Moves, UINTN Mc,
                         CONST CR_MDSS_RECT *Dirty, UINTN Dc)
{
  UINTN I,Row;
  for (I = 0; I < Mc; ++I) {
    CONST CR_MDSS_MOVE *M = &Moves[I];
    UINTN Rows = (UINT32)(M->Destination.Bottom - M->Destination.Top);
    UINTN Width = (UINT32)(M->Destination.Right - M->Destination.Left) * 4U;
    BOOLEAN BackRows = M->Destination.Top > M->SourceY;
    for (Row = 0; Row < Rows; ++Row) {
      UINTN Y = BackRows ? Rows - Row - 1 : Row;
      UINTN To = ((UINTN)(UINT32)M->Destination.Top + Y) * Stride + (UINT32)M->Destination.Left * 4U;
      UINTN From = ((UINTN)(UINT32)M->SourceY + Y) * Stride + (UINT32)M->SourceX * 4U;
      CopyBytes(Fb + To, Fb + From, Width, To > From);
    }
  }
  for (I = 0; I < Dc; ++I) {
    CONST CR_MDSS_RECT *R = &Dirty[I];
    for (Row = (UINT32)R->Top; Row < (UINT32)R->Bottom; ++Row) {
      UINTN From = Row * Pitch + (UINT32)R->Left * 4U;
      UINTN To = Row * Stride + (UINT32)R->Left * 4U;
      CopyBytes(Fb + To, (CONST UINT8 *)Source + From,
                 (UINT32)(R->Right - R->Left) * 4U, FALSE);
    }
  }
}

CR_STATUS CrMdssPresent(CR_MDSS *D, CONST VOID *Source, UINTN Bytes,
                       UINT32 Pitch, CONST CR_MDSS_MOVE *Moves, UINTN Mc,
                       CONST CR_MDSS_RECT *Dirty, UINTN Dc)
{
  CR_STATUS S=CrMdssPresentValidate(D,Source,Bytes,Pitch,Moves,Mc,Dirty,Dc);
  if(S!=CR_SUCCESS)return S;
  ApplyPresent(D->FrameBuffer,D->Mode.Stride,Source,Pitch,Moves,Mc,Dirty,Dc);
  D->Io.PublishCpuWrites(D->Io.Context, D->FrameBuffer,
                         (UINTN)D->Mode.Stride * D->Mode.Height);
  return CR_SUCCESS;
}

static BOOLEAN ValidRange(CONST VOID *P, UINTN Bytes)
{
  return P && Bytes && Bytes <= (UINTN)-1 - (UINTN)P;
}

/* Call only for validated ranges; touching endpoints do not overlap. */
static BOOLEAN Overlap(CONST VOID *A, UINTN Ab, CONST VOID *B, UINTN Bb)
{
  return (UINTN)A < (UINTN)B + Bb && (UINTN)B < (UINTN)A + Ab;
}

CR_STATUS CrMdssPresentStage(CONST CR_MDSS *D, CONST VOID *Source, UINTN Bytes,
                       UINT32 Pitch, CONST CR_MDSS_MOVE *Moves, UINTN Mc,
                       CONST CR_MDSS_RECT *Dirty, UINTN Dc,
                       CONST CR_MDSS_PRESENT_STAGE *Stage)
{
  if (!ValidRange(D,sizeof(*D)) || !ValidRange(Stage,sizeof(*Stage)) ||
      Mc > CR_MDSS_MAX_RECTS || Dc > CR_MDSS_MAX_RECTS ||
      (Mc && !ValidRange(Moves,Mc*sizeof(*Moves))) ||
      (Dc && !ValidRange(Dirty,Dc*sizeof(*Dirty)))) return CR_INVALID_PARAMETER;
  if ((Stage->PixelsTransform != 0 && Stage->PixelsTransform != 3) ||
      (Stage->NextPixelsTransform != 0 && Stage->NextPixelsTransform != 3))
    return CR_UNSUPPORTED;
  CR_STATUS S=CrMdssPresentValidate(D,Source,Bytes,Pitch,Moves,Mc,Dirty,Dc);
  if (S != CR_SUCCESS) return S;
  S=CrMdssValidateMode(&D->Mode,D->BufferBytes);
  if (S != CR_SUCCESS) return S;
  UINTN Size=(UINTN)D->Mode.Stride*D->Mode.Height;
  if (!ValidRange(D->FrameBuffer,D->BufferBytes) ||
      !ValidRange(Stage->Pixels,Stage->PixelBytes) || Stage->PixelBytes < Size ||
      !ValidRange(Stage->Before,Stage->BeforeBytes) || Stage->BeforeBytes < Size)
    return CR_BAD_BUFFER_SIZE;
  CONST VOID *Inputs[]={D,Stage,D->FrameBuffer,Source,Moves,Dirty};
  UINTN Lengths[]={sizeof(*D),sizeof(*Stage),D->BufferBytes,Dc ? Bytes : 0,
                   Mc*sizeof(*Moves),Dc*sizeof(*Dirty)};
  if (Overlap(Stage->Pixels,Stage->PixelBytes,Stage->Before,Stage->BeforeBytes))
    return CR_INVALID_PARAMETER;
  for (UINTN I=0;I<sizeof(Inputs)/sizeof(Inputs[0]);++I)
    if (Lengths[I] &&
        (Overlap(Stage->Pixels,Stage->PixelBytes,Inputs[I],Lengths[I]) ||
         Overlap(Stage->Before,Stage->BeforeBytes,Inputs[I],Lengths[I])))
      return CR_INVALID_PARAMETER;
  CopyBytes(Stage->Before,D->FrameBuffer,Size,FALSE);
  CopyBytes(Stage->Pixels,Stage->Before,Size,FALSE);
  if (Stage->PixelsTransform != Stage->NextPixelsTransform) {
    /* Reverse the visible pixel sequence, keeping each row's padding in
       place. Odd dimensions retain their one center pixel. */
    UINTN Count=(UINTN)D->Mode.Width*D->Mode.Height;
    volatile UINT8 *Pixels=Stage->Pixels;
    for (UINTN I=0;I<Count/2;++I) {
      UINTN J=Count-1-I;
      UINTN A=(I/D->Mode.Width)*D->Mode.Stride+(I%D->Mode.Width)*4;
      UINTN B=(J/D->Mode.Width)*D->Mode.Stride+(J%D->Mode.Width)*4;
      for (UINTN K=0;K<4;++K) {
        UINT8 Byte=Pixels[A+K];Pixels[A+K]=Pixels[B+K];Pixels[B+K]=Byte;
      }
    }
  }
  ApplyPresent(Stage->Pixels,D->Mode.Stride,Source,Pitch,Moves,Mc,Dirty,Dc);
  return CR_SUCCESS;
}

CR_STATUS CrMdssBlack(CR_MDSS *D)
{
  UINTN I, Size;
  volatile UINT8 *Fb;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (D->CleanupPending) return CR_NOT_READY;
  Size = (UINTN)D->Mode.Stride * D->Mode.Height; Fb = D->FrameBuffer;
  for (I = 0; I < Size; ++I) Fb[I] = 0;
  D->Io.PublishCpuWrites(D->Io.Context, D->FrameBuffer, Size);
  return CR_SUCCESS;
}
