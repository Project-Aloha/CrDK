/**
  Parse the HDK8450 panel XML names without depending on NUL termination.

  Firmware XML files can contain a forest of PanelName/Group elements and
  optional units attributes. Keep that vocabulary intact. Missing required
  timings and malformed present values are errors, not alternate panel modes.
**/
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include "MdpPanel.h"

STATIC BOOLEAN Space (CHAR8 C)
{
  return C == ' ' || C == '\t' || C == '\r' || C == '\n';
}

STATIC BOOLEAN Match (CONST CHAR8 *Xml, UINTN Size, UINTN At, CONST CHAR8 *Text)
{
  UINTN Length = AsciiStrLen (Text);
  return At <= Size && Length <= Size - At &&
         CompareMem (Xml + At, Text, Length) == 0;
}

STATIC EFI_STATUS Number (CONST CHAR8 *Value, UINTN Length, UINT32 *Result);

/* Find one scalar element; ignore declarations, comments and other tags.
   Identical duplicates occur in packaged XML; numeric aliases may also use
   equivalent decimal/hex spellings. Conflicting definitions must
   never silently change the selected panel. */
STATIC EFI_STATUS ValueSpanInternal (
  CONST CHAR8 *Xml, UINTN Size, CONST CHAR8 *Name,
  CONST CHAR8 **Value, UINTN *Length, BOOLEAN Numeric
  )
{
  UINTN I, Start, End, NameLength;
  UINT32 OldNumber, NewNumber;
  CHAR8 Quote;
  BOOLEAN Found, Selected;

  NameLength = AsciiStrLen (Name);
  Found = FALSE;
  *Value = NULL;
  *Length = 0;
  for (I = 0; I < Size;) {
    if (Xml[I++] != '<') continue;
    if (Match (Xml, Size, I, "!--")) {
      I += 3;
      while (I < Size && !Match (Xml, Size, I, "-->")) I++;
      if (I == Size) return EFI_COMPROMISED_DATA;
      I += 3;
      continue;
    }
    Selected = Match (Xml, Size, I, Name) && I + NameLength < Size &&
               (Xml[I + NameLength] == '>' || Xml[I + NameLength] == '/' ||
                Space (Xml[I + NameLength]));
    Quote = 0;
    for (; I < Size; I++) {
      if (Quote != 0) {
        if (Xml[I] == Quote) Quote = 0;
      } else if (Xml[I] == '\'' || Xml[I] == '"') {
        Quote = Xml[I];
      } else if (Xml[I] == '>') {
        break;
      }
    }
    if (I == Size) return EFI_COMPROMISED_DATA;
    I++;
    if (!Selected) continue;
    if (I >= 2 && Xml[I - 2] == '/') {
      if (Found && *Length != 0) return EFI_COMPROMISED_DATA;
      *Value = Xml + I;
      *Length = 0;
      Found = TRUE;
      continue;
    }
    Start = I;
    while (I < Size && Xml[I] != '<') I++;
    End = I;
    if (!Match (Xml, Size, I, "</") || !Match (Xml, Size, I + 2, Name))
      return EFI_COMPROMISED_DATA;
    I += 2 + NameLength;
    while (I < Size && Space (Xml[I])) I++;
    if (I == Size || Xml[I] != '>') return EFI_COMPROMISED_DATA;
    I++;
    while (Start < End && Space (Xml[Start])) Start++;
    while (End > Start && Space (Xml[End - 1])) End--;
    if (Found && (*Length != End - Start ||
                  CompareMem (*Value, Xml + Start, End - Start) != 0)) {
      // A numeric duplicate may use a different valid decimal/hex spelling.
      // Preserve conflict detection by comparing values, never picking a winner.
      if (!Numeric || EFI_ERROR (Number (*Value, *Length, &OldNumber)) ||
          EFI_ERROR (Number (Xml + Start, End - Start, &NewNumber)) ||
          OldNumber != NewNumber)
        return EFI_COMPROMISED_DATA;
    }
    *Value = Xml + Start;
    *Length = End - Start;
    Found = TRUE;
  }
  return Found ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_STATUS ValueSpan (
  CONST CHAR8 *Xml, UINTN Size, CONST CHAR8 *Name,
  CONST CHAR8 **Value, UINTN *Length
  )
{
  return ValueSpanInternal (Xml, Size, Name, Value, Length, FALSE);
}

STATIC EFI_STATUS Number (CONST CHAR8 *Value, UINTN Length, UINT32 *Result)
{
  UINTN I = 0;
  UINT32 Base = 10, N = 0, Digit;
  if (Length >= 2 && Value[0] == '0' && (Value[1] == 'x' || Value[1] == 'X')) {
    I = 2;
    Base = 16;
  }
  if (I == Length) return EFI_COMPROMISED_DATA;
  for (; I < Length; I++) {
    if (Value[I] >= '0' && Value[I] <= '9') Digit = (UINT32)(Value[I] - '0');
    else if (Value[I] >= 'a' && Value[I] <= 'f') Digit = (UINT32)(Value[I] - 'a' + 10);
    else if (Value[I] >= 'A' && Value[I] <= 'F') Digit = (UINT32)(Value[I] - 'A' + 10);
    else return EFI_COMPROMISED_DATA;
    if (Digit >= Base || N > (MAX_UINT32 - Digit) / Base) return EFI_COMPROMISED_DATA;
    N = N * Base + Digit;
  }
  *Result = N;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadNumber (
  CONST CHAR8 *Xml, UINTN Size, CONST CHAR8 *Name,
  BOOLEAN Required, UINT32 Default, UINT32 *Result
  )
{
  CONST CHAR8 *Value;
  UINTN Length;
  EFI_STATUS Status = ValueSpanInternal (Xml, Size, Name, &Value, &Length, TRUE);
  if (Status == EFI_NOT_FOUND && !Required) {
    *Result = Default;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status)) return Status;
  return Number (Value, Length, Result);
}

STATIC EFI_STATUS ReadOptionalNumber (
  CONST CHAR8 *Xml, UINTN Size, CONST CHAR8 *Name,
  BOOLEAN *Present, UINT32 *Result
  )
{
  CONST CHAR8 *Value;
  UINTN Length;
  EFI_STATUS Status = ValueSpanInternal (Xml, Size, Name, &Value, &Length, TRUE);
  *Present = FALSE;
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (EFI_ERROR (Status)) return Status;
  Status = Number (Value, Length, Result);
  if (!EFI_ERROR (Status)) *Present = TRUE;
  return Status;
}

STATIC EFI_STATUS ReadResetInfo (CONST CHAR8 *Xml, UINTN Size, MDP_PANEL_CONFIG *Panel)
{
  CONST CHAR8 *Value;
  UINTN Length, I = 0, Start, Cell;
  EFI_STATUS Status = ValueSpan (Xml, Size, "DisplayResetInfo", &Value, &Length);
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (EFI_ERROR (Status)) return Status;
  for (Cell = 0; Cell < ARRAY_SIZE (Panel->ResetInfo); Cell++) {
    while (I < Length && (Space (Value[I]) || Value[I] == ',')) I++;
    Start = I;
    while (I < Length && !Space (Value[I]) && Value[I] != ',') I++;
    Status = Number (Value + Start, I - Start, &Panel->ResetInfo[Cell]);
    if (EFI_ERROR (Status)) return Status;
  }
  while (I < Length && (Space (Value[I]) || Value[I] == ',')) I++;
  if (I != Length) return EFI_COMPROMISED_DATA;
  Panel->ResetInfoPresent = TRUE;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS ReadBool (
  CONST CHAR8 *Xml, UINTN Size, CONST CHAR8 *Name, BOOLEAN *Result
  )
{
  CONST CHAR8 *Value;
  UINTN Length;
  EFI_STATUS Status = ValueSpan (Xml, Size, Name, &Value, &Length);
  *Result = FALSE;
  if (Status == EFI_NOT_FOUND) return EFI_SUCCESS;
  if (EFI_ERROR (Status)) return Status;
  if (Length == 4 && CompareMem (Value, "True", 4) == 0) *Result = TRUE;
  else if (Length != 5 || CompareMem (Value, "False", 5) != 0) return EFI_COMPROMISED_DATA;
  return EFI_SUCCESS;
}

EFI_STATUS MdpParsePanelXml (CONST CHAR8 *Xml, UINTN Size, MDP_PANEL_CONFIG *Panel)
{
  EFI_STATUS Status;
  CONST CHAR8 *Value;
  UINTN Length, I;
  UINT32 N, ControllerMask = 0;
  BOOLEAN Flag, LegacyVsyncPresent, CorrectedVsyncPresent;
  STATIC CONST CHAR8 *Geometry[] = {
    "HorizontalSyncSkew", "VerticalSyncSkew",
    "HorizontalLeftBorder", "HorizontalRightBorder",
    "VerticalTopBorder", "VerticalBottomBorder"
  };
  STATIC CONST CHAR8 *PhyFlags[] = {
    "DSIPhyDCDCMode", "DSITimingHSZeroOverride", "DSITimingHSTrailOverride"
  };
  STATIC CONST CHAR8 *CommandFlags[] = {
    "DSICmdSwapInterface", "DSICmdUsingTrigger", "DSIEnableAutoRefresh",
    "DSITECheckEnable", "DSITEUsingDedicatedTEPin"
  };
  STATIC CONST struct { CONST CHAR8 *Name; UINT32 Bit; } VideoFlags[] = {
    { "DSILowPowerModeInBLLP", MDP_DSI_VIDEO_BLLP_LP },
    { "DSILowPowerModeInBLLPEOF", MDP_DSI_VIDEO_EOF_BLLP_LP },
    { "DSILowPowerModeInHSA", MDP_DSI_VIDEO_HSA_LP },
    { "DSILowPowerModeInHBP", MDP_DSI_VIDEO_HBP_LP },
    { "DSILowPowerModeInHFP", MDP_DSI_VIDEO_HFP_LP },
  };

  if (Xml == NULL || Size == 0 || Panel == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem (Panel, sizeof (*Panel));
#define READ(Field, Tag, Required, Default) \
  do { Status = ReadNumber (Xml, Size, Tag, Required, Default, &Panel->Field); \
       if (EFI_ERROR (Status)) return Status; } while (0)
  READ (Timing.Width, "HorizontalActive", TRUE, 0);
  READ (Timing.Height, "VerticalActive", TRUE, 0);
  READ (Timing.HFrontPorch, "HorizontalFrontPorch", TRUE, 0);
  READ (Timing.HBackPorch, "HorizontalBackPorch", TRUE, 0);
  READ (Timing.HSync, "HorizontalSyncPulse", TRUE, 0);
  READ (Timing.VFrontPorch, "VerticalFrontPorch", TRUE, 0);
  READ (Timing.VBackPorch, "VerticalBackPorch", TRUE, 0);
  READ (Timing.VSync, "VerticalSyncPulse", TRUE, 0);
  READ (Timing.DsiChannel, "DSIChannelId", TRUE, 0);
  READ (Timing.DsiVirtualChannel, "DSIVirtualId", FALSE, 0);
  READ (Timing.DsiColorFormat, "DSIColorFormat", TRUE, 0);
  READ (Timing.DsiTrafficMode, "DSITrafficMode", FALSE, 1);
  READ (Timing.DsiLanes, "DSILanes", TRUE, 0);
  READ (Timing.DsiRefreshRate, "DSIRefreshRate", TRUE, 0);
  READ (Timing.DisplayResetGpio, "DisplayResetGpio", FALSE, MAX_UINT32);
  READ (InterfaceType, "InterfaceType", TRUE, 0);
  READ (InterfaceColorFormat, "InterfaceColorFormat", TRUE, 0);
  READ (BorderColor, "BorderColor", FALSE, 0);
#undef READ
  Status = ReadResetInfo (Xml, Size, Panel);
  if (EFI_ERROR (Status)) return Status;
#define BACKLIGHT(Field) \
  do { Status = ReadOptionalNumber (Xml, Size, #Field, &Panel->Field##Present, &Panel->Field); \
       if (EFI_ERROR (Status)) return Status; } while (0)
  BACKLIGHT (BacklightType);
  BACKLIGHT (BacklightPmicControlType);
  BACKLIGHT (BacklightBitWidth);
#undef BACKLIGHT
  /* Android dsi_panel_update_backlight accepts a 16-bit DCS brightness value.
     Preserve absence: it must not become an invented brightness default. */
  if (Panel->BacklightBitWidthPresent &&
      (Panel->BacklightBitWidth == 0 || Panel->BacklightBitWidth > 16))
    return EFI_COMPROMISED_DATA;
  /* These alter active/total timing; the current scanout has no programming
     for them. Preserve their presence rather than silently changing the mode. */
  for (I = 0; I < ARRAY_SIZE (Geometry); I++) {
    Status = ReadNumber (Xml, Size, Geometry[I], FALSE, 0, &N);
    if (EFI_ERROR (Status)) return Status;
    if (N != 0) Panel->HasUnsupportedGeometry = TRUE;
  }
  Status = ReadBool (Xml, Size, "InvertHsyncPolarity", &Panel->InvertHsyncPolarity);
  if (EFI_ERROR (Status)) return Status;
  /* Packaged XML uses Polairty; accept the corrected spelling as an alias,
     but never silently OR contradictory definitions of the same polarity. */
  Status = ValueSpan (Xml, Size, "InvertVsyncPolairty", &Value, &Length);
  LegacyVsyncPresent = (Status == EFI_SUCCESS);
  Status = ReadBool (Xml, Size, "InvertVsyncPolairty", &Panel->InvertVsyncPolarity);
  if (EFI_ERROR (Status)) return Status;
  Status = ValueSpan (Xml, Size, "InvertVsyncPolarity", &Value, &Length);
  CorrectedVsyncPresent = (Status == EFI_SUCCESS);
  Status = ReadBool (Xml, Size, "InvertVsyncPolarity", &Flag);
  if (EFI_ERROR (Status)) return Status;
  if (LegacyVsyncPresent && CorrectedVsyncPresent && Flag != Panel->InvertVsyncPolarity)
    return EFI_COMPROMISED_DATA;
  if (CorrectedVsyncPresent) Panel->InvertVsyncPolarity = Flag;
  Status = ReadBool (Xml, Size, "InvertDataPolarity", &Flag);
  if (EFI_ERROR (Status)) return Status;
  if (Flag) Panel->HasUnsupportedGeometry = TRUE;
  Status = ReadBool (Xml, Size, "DSIDSCEnable", &Panel->Dsc);
  if (EFI_ERROR (Status)) return Status;
  STATIC CONST CHAR8 *DscNames[] = {"DSIDSCMajor","DSIDSCMinor","DSIDSCScr","DSIDSCProfileID",
                                   "DSIDSCSliceWidth","DSIDSCSliceHeight","DSIDSCSlicePerPacket"};
  UINT32 *DscValues[] = {&Panel->DscMajor,&Panel->DscMinor,&Panel->DscScr,&Panel->DscProfileId,
                        &Panel->DscSliceWidth,&Panel->DscSliceHeight,&Panel->DscSlicesPerPacket};
  for (I=0; I<ARRAY_SIZE(DscNames); ++I) {
    Status=ReadOptionalNumber(Xml,Size,DscNames[I],&Panel->DscFieldsPresent[I],DscValues[I]);
    if(EFI_ERROR(Status))return Status;
  }
  Status = ReadBool (Xml, Size, "DSICPhyMode", &Panel->Cphy);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadBool (Xml, Size, "DSIHsaHseAfterVsVe", &Panel->HsaHseAfterVsVe);
  if (EFI_ERROR (Status)) return Status;
  Status = ReadBool (Xml, Size, "DSIDisableEoTAfterHSXfer", &Panel->DisableEot);
  if (EFI_ERROR (Status)) return Status;
  for (I = 0; I < ARRAY_SIZE (PhyFlags); I++) {
    Status = ReadBool (Xml, Size, PhyFlags[I], &Flag);
    if (EFI_ERROR (Status)) return Status;
    if (Flag) Panel->HasUnsupportedPhy = TRUE;
  }
  Status = ReadNumber (Xml, Size, "DSITimingMargin", FALSE, 0, &N);
  if (EFI_ERROR (Status)) return Status;
  if (N != 0) Panel->HasUnsupportedPhy = TRUE;
  Status = ReadNumber (Xml, Size, "DSISplitLinkConfig", FALSE, 0, &N);
  if (EFI_ERROR (Status)) return Status;
  Panel->SplitLink = (N != 0);
  for (I = 0; I < ARRAY_SIZE (VideoFlags); I++) {
    Status = ReadBool (Xml, Size, VideoFlags[I].Name, &Flag);
    if (EFI_ERROR (Status)) return Status;
    if (Flag) Panel->Timing.DsiVideoFlags |= VideoFlags[I].Bit;
  }
  Status = ValueSpan (Xml, Size, "DSIControllerMapping", &Value, &Length);
  if (EFI_ERROR (Status) || Length == 0) return EFI_COMPROMISED_DATA;
  /* OEM mapping is an array of hex bytes: 00, 01 or 00 01. */
  for (I = 0; I < Length;) {
    if (Length - I < 2 || Value[I] != '0' || (Value[I + 1] != '0' && Value[I + 1] != '1'))
      return EFI_COMPROMISED_DATA;
    N = (UINT32)(Value[I + 1] - '0');
    if ((ControllerMask & (1U << N)) != 0) return EFI_COMPROMISED_DATA;
    ControllerMask |= 1U << N;
    if (Panel->ControllerCount++ == 0) Panel->Timing.DsiController = (UINT32)(Value[I + 1] - '0');
    I += 2;
    if (I < Length && !Space (Value[I])) return EFI_COMPROMISED_DATA;
    while (I < Length && Space (Value[I])) I++;
  }
  /* The video-only host does not program command-interface swapping,
     trigger/TE handling or command auto-refresh. Android's DSI panel
     parser treats video and command engine configuration separately.
     Preserve these explicit requests even when command byte lists are
     empty, rather than accepting them as ordinary video configuration. */
  for (I = 0; I < ARRAY_SIZE (CommandFlags); I++) {
    Status = ReadBool (Xml, Size, CommandFlags[I], &Flag);
    if (EFI_ERROR (Status)) return Status;
    if (Flag) Panel->HasUnsupportedCommandMode = TRUE;
  }
  Status = ValueSpan (Xml, Size, "DSIInitSequence", &Value, &Length);
  if (Status != EFI_NOT_FOUND && EFI_ERROR (Status)) return Status;
  Status = MdpParseDsiSequence (Value, Length, &Panel->InitSequence);
  if (EFI_ERROR (Status)) return Status;
  Status = ValueSpan (Xml, Size, "DSITermSequence", &Value, &Length);
  if (Status != EFI_NOT_FOUND && EFI_ERROR (Status)) goto Failed;
  Status = MdpParseDsiSequence (Value, Length, &Panel->TermSequence);
  if (EFI_ERROR (Status)) goto Failed;
  Panel->HasCommands = Panel->InitSequence.Count != 0 || Panel->TermSequence.Count != 0;
  return EFI_SUCCESS;
Failed:
  MdpFreePanelConfig (Panel);
  return Status;
}

VOID MdpFreePanelConfig (MDP_PANEL_CONFIG *Panel) {
  if (Panel == NULL) return;
  MdpFreeDsiSequence (&Panel->InitSequence);
  MdpFreeDsiSequence (&Panel->TermSequence);
  Panel->HasCommands = FALSE;
}

EFI_STATUS MdpValidateVideoPanel (CONST MDP_PANEL_CONFIG *Panel)
{
  CONST EFI_MDP_DSI_TIMING *P;
  UINT64 HTotal, VTotal;
  if (Panel == NULL) return EFI_INVALID_PARAMETER;
  P = &Panel->Timing;
  /* Capability checking is separate from XML syntax compatibility. Never
     drive a DSC/command/CPHY panel as uncompressed DPHY video. */
  if (Panel->InterfaceType != 8 || Panel->InterfaceColorFormat != 3 ||
      Panel->Dsc || Panel->Cphy || Panel->SplitLink || Panel->HasUnsupportedCommandMode ||
      Panel->InitSequence.Unsupported || Panel->TermSequence.Unsupported ||
      Panel->HasUnsupportedGeometry || Panel->HasUnsupportedPhy ||
      Panel->ControllerCount != 1 || P->DsiColorFormat != 36 ||
      P->DsiController > 1 || P->DsiTrafficMode > 2)
    return EFI_UNSUPPORTED;
  HTotal = (UINT64)P->Width + P->HSync + P->HFrontPorch + P->HBackPorch;
  VTotal = (UINT64)P->Height + P->VSync + P->VFrontPorch + P->VBackPorch;
  if (P->Width == 0 || P->Height == 0 || P->HSync == 0 || P->VSync == 0 ||
      P->Width > 0xffff || P->Height > 0xffff ||
      P->DsiLanes == 0 || P->DsiLanes > 4 || P->DsiVirtualChannel > 3 ||
      P->DsiRefreshRate == 0 || HTotal > 0xffff || VTotal > 0xffff ||
      HTotal * VTotal > MAX_UINT32)
    return EFI_INVALID_PARAMETER;
  return EFI_SUCCESS;
}
