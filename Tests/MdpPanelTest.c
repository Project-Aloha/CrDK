/** @file
  Exercise the firmware XML parser against packaged HDK8450 panel files.
**/
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#undef NULL
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "../Driver/MDSSCrDxe/MdpPanel.h"

UINTN EFIAPI AsciiStrLen (CONST CHAR8 *String) { return strlen (String); }
INTN EFIAPI CompareMem (CONST VOID *Left, CONST VOID *Right, UINTN Length) {
  return memcmp (Left, Right, Length);
}
VOID *EFIAPI ZeroMem (VOID *Buffer, UINTN Length) { return memset (Buffer, 0, Length); }

static int FailAllocation;
VOID *EFIAPI AllocateZeroPool (UINTN Size) {
  if (FailAllocation) return NULL;
  return calloc (1, Size);
}
VOID EFIAPI FreePool (VOID *Buffer) { free (Buffer); }

static CHAR8 *ReadFile (const char *Path, size_t *Size) {
  FILE *File = fopen (Path, "rb");
  CHAR8 *Text;
  long Length;
  assert (File != NULL);
  assert (fseek (File, 0, SEEK_END) == 0);
  Length = ftell (File);
  assert (Length > 0 && fseek (File, 0, SEEK_SET) == 0);
  Text = malloc ((size_t)Length + 1);
  assert (Text != NULL);
  assert (fread (Text, 1, (size_t)Length, File) == (size_t)Length);
  assert (fclose (File) == 0);
  Text[Length] = 0;
  *Size = (size_t)Length;
  return Text;
}

static void AppendCase (const CHAR8 *Text, size_t Size, const char *Extra,
                        EFI_STATUS ParseStatus, EFI_STATUS ValidateStatus) {
  MDP_PANEL_CONFIG Panel = {0};
  size_t Length = Size + strlen (Extra);
  /* Deliberately no terminating NUL: exercise the firmware Size contract. */
  CHAR8 *Buffer = malloc (Length);
  assert (Buffer != NULL);
  memcpy (Buffer, Text, Size);
  memcpy (Buffer + Size, Extra, strlen (Extra));
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Buffer, Length, &Panel) == ParseStatus);
  if (ParseStatus == EFI_SUCCESS)
    assert (MdpValidateVideoPanel (&Panel) == ValidateStatus);
  MdpFreePanelConfig (&Panel);
  free (Buffer);
}

static void CheckPolarity (const CHAR8 *Text, size_t Size, const char *Extra,
                           BOOLEAN Hsync, BOOLEAN Vsync) {
  MDP_PANEL_CONFIG Panel = {0};
  size_t Length = Size + strlen (Extra);
  CHAR8 *Buffer = malloc (Length);
  assert (Buffer != NULL);
  memcpy (Buffer, Text, Size);
  memcpy (Buffer + Size, Extra, strlen (Extra));
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Buffer, Length, &Panel) == EFI_SUCCESS);
  assert (Panel.InvertHsyncPolarity == Hsync);
  assert (Panel.InvertVsyncPolarity == Vsync);
  assert (MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
  MdpFreePanelConfig (&Panel);
  free (Buffer);
}

static void CheckSequences (void) {
  const char Valid[] = " 05 11 00\r\nff 78\n15 51 ff\n39 b0 01 02\nFF 14\n03\n13 ab\n23 01 02\n29 ab cd";
  const char *Bad[] = { "0", "05 1", "05 gg", "05 11 01", "05", "15 11",
                        "ff", "ff 01 02", "39", "05 11,00", "0511", "03 00",
                        "13", "23 00", "05 11\n39 0x12" };
  MDP_DSI_SEQUENCE Sequence;
  UINTN I;
  char *Copy = malloc (sizeof (Valid) - 1);
  assert (Copy != NULL);
  memcpy (Copy, Valid, sizeof (Valid) - 1);
  assert (MdpParseDsiSequence (Copy, sizeof (Valid) - 1, &Sequence) == EFI_SUCCESS);
  memset (Copy, 0, sizeof (Valid) - 1);
  free (Copy); /* Owned payload must survive XML destruction. */
  assert (Sequence.Count == 9 && !Sequence.Unsupported);
  assert (Sequence.Commands[0].Length == 1 && Sequence.Commands[0].Payload[0] == 0x11);
  assert (Sequence.Commands[1].DataType == 0xff && Sequence.Commands[1].DelayMs == 120);
  assert (Sequence.Commands[2].Payload[1] == 0xff);
  assert (Sequence.Commands[3].Length == 3 && Sequence.Commands[3].Payload[2] == 2);
  assert (Sequence.Commands[4].DelayMs == 20 && Sequence.Commands[4].Length == 0);
  assert (Sequence.Commands[5].Length == 0);
  MdpFreeDsiSequence (&Sequence);
  MdpFreeDsiSequence (&Sequence);
  for (I = 0; I < ARRAY_SIZE (Bad); I++) {
    assert (MdpParseDsiSequence (Bad[I], strlen (Bad[I]), &Sequence) == EFI_COMPROMISED_DATA);
    assert (Sequence.Commands == NULL && Sequence.Count == 0);
  }
  assert (MdpParseDsiSequence ("07 01 00\n0A 11 00\n99 ff", sizeof ("07 01 00\n0A 11 00\n99 ff") - 1, &Sequence) == EFI_SUCCESS);
  assert (Sequence.Unsupported && Sequence.Count == 3);
  MdpFreeDsiSequence (&Sequence);
  assert (MdpParseDsiSequence (NULL, 0, &Sequence) == EFI_SUCCESS);
  assert (Sequence.Count == 0);
  {
    UINTN Bytes;
    char *Long = malloc (3 + 3 * (MDP_DSI_MAX_WRITE_PAYLOAD + 1));
    assert (Long != NULL);
    memcpy (Long, "39 ", 3);
    for (Bytes = 0; Bytes < MDP_DSI_MAX_WRITE_PAYLOAD + 1; Bytes++)
      memcpy (Long + 3 + Bytes * 3, "aa ", 3);
    assert (MdpParseDsiSequence (Long, 3 + 3 * MDP_DSI_MAX_WRITE_PAYLOAD, &Sequence) == EFI_SUCCESS);
    assert (!Sequence.Unsupported && Sequence.Commands[0].Length == MDP_DSI_MAX_WRITE_PAYLOAD);
    MdpFreeDsiSequence (&Sequence);
    assert (MdpParseDsiSequence (Long, 3 + 3 * (MDP_DSI_MAX_WRITE_PAYLOAD + 1), &Sequence) == EFI_SUCCESS);
    assert (Sequence.Unsupported);
    MdpFreeDsiSequence (&Sequence);
    free (Long);
  }
  FailAllocation = 1;
  assert (MdpParseDsiSequence ("05 11", 5, &Sequence) == EFI_OUT_OF_RESOURCES);
  assert (Sequence.Commands == NULL && Sequence.Count == 0);
  FailAllocation = 0;
}

static void CheckResetBacklight (const CHAR8 *Text, size_t Size) {
  MDP_PANEL_CONFIG Panel = {0};
  const char *Start = strstr (Text, "<DisplayResetInfo>");
  const char *End;
  char *Base, *Owned;
  size_t Removed, BaseSize, Total;
  const char Extra[] = "<DisplayResetInfo units='raw'>1, 0x14 30,0X28, 0</DisplayResetInfo>"
                       "<BacklightType>3</BacklightType>"
                       "<BacklightPmicControlType>2</BacklightPmicControlType>"
                       "<BacklightBitWidth>12</BacklightBitWidth>";
  assert (Start != NULL);
  End = strstr (Start, "</DisplayResetInfo>");
  assert (End != NULL);
  End += strlen ("</DisplayResetInfo>");
  Removed = (size_t)(End - Start); BaseSize = Size - Removed;
  Base = malloc (BaseSize); assert (Base);
  memcpy (Base, Text, (size_t)(Start - Text));
  memcpy (Base + (Start - Text), End, Size - (size_t)(End - Text));
  assert (MdpParsePanelXml (Base, BaseSize, &Panel) == EFI_SUCCESS);
  assert (!Panel.ResetInfoPresent && !Panel.BacklightTypePresent &&
          !Panel.BacklightPmicControlTypePresent && !Panel.BacklightBitWidthPresent);
  MdpFreePanelConfig (&Panel);
  Total = BaseSize + sizeof (Extra) - 1;
  Owned = malloc (Total); assert (Owned);
  memcpy (Owned, Base, BaseSize); memcpy (Owned + BaseSize, Extra, sizeof (Extra) - 1);
  assert (MdpParsePanelXml (Owned, Total, &Panel) == EFI_SUCCESS);
  memset (Owned, 0xa5, Total); free (Owned);
  assert (Panel.ResetInfoPresent && Panel.ResetInfo[0] == 1 && Panel.ResetInfo[1] == 20 &&
          Panel.ResetInfo[2] == 30 && Panel.ResetInfo[3] == 40 && Panel.ResetInfo[4] == 0);
  assert (Panel.BacklightTypePresent && Panel.BacklightType == 3);
  assert (Panel.BacklightPmicControlTypePresent && Panel.BacklightPmicControlType == 2);
  assert (Panel.BacklightBitWidthPresent && Panel.BacklightBitWidth == 12);
  MdpFreePanelConfig (&Panel);
  AppendCase (Base, BaseSize, "<DisplayResetInfo>0 10 1000 10000 0</DisplayResetInfo>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Base, BaseSize, "<DisplayResetInfo>0 10 1000 10000</DisplayResetInfo>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<DisplayResetInfo>0 10 1000 10000 0 0</DisplayResetInfo>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<DisplayResetInfo/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<DisplayResetInfo>0 10 4294967296 10000 0</DisplayResetInfo>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<DisplayResetInfo>0 10 -1 10000 0</DisplayResetInfo>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<BacklightBitWidth>0</BacklightBitWidth>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<BacklightBitWidth>17</BacklightBitWidth>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<BacklightBitWidth>16</BacklightBitWidth>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Base, BaseSize, "<BacklightType/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<BacklightPmicControlType>False</BacklightPmicControlType>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Base, BaseSize, "<BacklightType>3</BacklightType><BacklightType>1</BacklightType>", EFI_COMPROMISED_DATA, 0);
  free (Base);
}

int main (int argc, char **argv) {
  MDP_PANEL_CONFIG Panel = {0};
  CHAR8 *Text, *Mapping, *Width;
  size_t Size;
  int Index;
  CheckSequences ();
  assert (argc >= 4);
  Text = ReadFile (argv[1], &Size);
  CheckResetBacklight (Text, Size);
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
  assert (Panel.ResetInfoPresent && Panel.ResetInfo[0] == 0 && Panel.ResetInfo[1] == 20000 &&
          Panel.ResetInfo[2] == 20000 && Panel.ResetInfo[3] == 100 && Panel.ResetInfo[4] == 0);
  assert (!Panel.BacklightTypePresent && !Panel.BacklightBitWidthPresent);
  assert (Panel.BorderColor == 0);
  {
    const char Extra[] = "<BorderColor>0x00123456</BorderColor>"
                         "<BorderColor>1193046</BorderColor>";
    CHAR8 *Color = malloc (Size + sizeof (Extra) - 1);
    assert (Color != NULL);
    memcpy (Color, Text, Size);
    memcpy (Color + Size, Extra, sizeof (Extra) - 1);
    MdpFreePanelConfig (&Panel);
    assert (MdpParsePanelXml (Color, Size + sizeof (Extra) - 1, &Panel) == EFI_SUCCESS);
    free (Color);
    assert (Panel.BorderColor == 0x00123456 && MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
  }
  assert (Panel.Timing.Width == 1920 && Panel.Timing.Height == 1080);
  assert (Panel.Timing.DsiChannel == 1 && Panel.Timing.DsiController == 0);
  assert (Panel.Timing.DsiLanes == 4);
  assert (Panel.Timing.DsiVideoFlags & MDP_DSI_VIDEO_BLLP_LP);
  assert (Panel.Timing.DsiVideoFlags & MDP_DSI_VIDEO_EOF_BLLP_LP);
  {
    const char *Tags[] = { "<DSILowPowerModeInBLLP>", "<DSILowPowerModeInBLLPEOF>" };
    UINT32 Bits[] = { MDP_DSI_VIDEO_BLLP_LP, MDP_DSI_VIDEO_EOF_BLLP_LP };
    size_t Tag, Offset;
    for (Tag = 0; Tag < ARRAY_SIZE (Tags); ++Tag) {
      CHAR8 *Copy = malloc (Size + 1); /* False adds one byte, still no NUL */
      assert (Copy != NULL);
      Width = strstr (Text, Tags[Tag]);
      assert (Width != NULL);
      Offset = (size_t)(Width - Text) + strlen (Tags[Tag]);
      assert (memcmp (Text + Offset, "True", 4) == 0);
      memcpy (Copy, Text, Offset);
      memcpy (Copy + Offset, "False", 5);
      memcpy (Copy + Offset + 5, Text + Offset + 4, Size - Offset - 4);
      MdpFreePanelConfig (&Panel);
      assert (MdpParsePanelXml (Copy, Size + 1, &Panel) == EFI_SUCCESS);
      assert ((Panel.Timing.DsiVideoFlags & Bits[Tag]) == 0);
      assert ((Panel.Timing.DsiVideoFlags & Bits[1 - Tag]) != 0);
      assert (MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
      {
        const CHAR8 *End = strchr (Text + Offset + 4, '>');
        size_t Start = (size_t)(Width - Text);
        size_t Removed;
        assert (End != NULL);
        Removed = (size_t)(End + 1 - Width);
        memcpy (Copy, Text, Start);
        memcpy (Copy + Start, End + 1, Size - Start - Removed);
        MdpFreePanelConfig (&Panel);
        assert (MdpParsePanelXml (Copy, Size - Removed, &Panel) == EFI_SUCCESS);
        assert ((Panel.Timing.DsiVideoFlags & Bits[Tag]) == 0);
        assert ((Panel.Timing.DsiVideoFlags & Bits[1 - Tag]) != 0);
        assert (MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
      }
      free (Copy);
    }
  }
  /* In-place whitespace keeps the bounded XML size identical to the fixture. */
  {
    const char *CommandTags[] = { "<DSICmdSwapInterface>", "<DSICmdUsingTrigger>" };
    size_t Tag;
    for (Tag = 0; Tag < ARRAY_SIZE (CommandTags); ++Tag) {
      Width = strstr (Text, CommandTags[Tag]);
      assert (Width != NULL);
      Width += strlen (CommandTags[Tag]);
      assert (memcmp (Width, "False", 5) == 0);
      memcpy (Width, "True ", 5);
      MdpFreePanelConfig (&Panel);
      assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
      assert (Panel.HasUnsupportedCommandMode && MdpValidateVideoPanel (&Panel) == EFI_UNSUPPORTED);
      memcpy (Width, "False", 5);
    }
  }
  AppendCase (Text, Size, "<DSIEnableAutoRefresh>True</DSIEnableAutoRefresh>",
              EFI_SUCCESS, EFI_UNSUPPORTED);
  AppendCase (Text, Size, "<DSITECheckEnable>True</DSITECheckEnable>",
              EFI_SUCCESS, EFI_UNSUPPORTED);
  AppendCase (Text, Size, "<DSITEUsingDedicatedTEPin>True</DSITEUsingDedicatedTEPin>",
              EFI_SUCCESS, EFI_UNSUPPORTED);
  AppendCase (Text, Size, "<DSIEnableAutoRefresh>False</DSIEnableAutoRefresh>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<DSICmdUsingTrigger/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<DSITermSequence>05 28</DSITermSequence>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<DSIInitSequence>05 11</DSIInitSequence>"
                         "<DSITermSequence>05 zz</DSITermSequence>",
              EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<DSIInitSequence>07 01 00</DSIInitSequence>",
              EFI_SUCCESS, EFI_UNSUPPORTED);
  AppendCase (Text, Size, "<DSIInitSequence/>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<DSICPhyMode/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<DSISplitLinkConfig/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<HorizontalActive>0x780</HorizontalActive>"
                         "<HorizontalActive>01920</HorizontalActive>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<DSIRefreshRate>3932160</DSIRefreshRate>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<HorizontalActive>0x781</HorizontalActive>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<HorizontalActive>4294967296</HorizontalActive>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<HorizontalActive>0x</HorizontalActive>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<HorizontalActiveExtra>1</HorizontalActiveExtra>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<BorderColor>0</BorderColor>", EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<BorderColor>0x100000000</BorderColor>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<BorderColor/>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<BorderColor>-1</BorderColor>", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<DSIRefreshRate>0x3C0000</DSIRefreshRate>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<DSIRefreshRate>0x780000</DSIRefreshRate>",
              EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<!-- <DSICPhyMode>True</DSICPhyMode> -->",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<!-- unterminated", EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<InvertHsyncPolarity>True</InvertHsyncPolarity>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<InvertDataPolarity>True</InvertDataPolarity>",
              EFI_SUCCESS, EFI_UNSUPPORTED);
  AppendCase (Text, Size, "<InvertVsyncPolairty>True</InvertVsyncPolairty>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<InvertVsyncPolarity>True</InvertVsyncPolarity>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<InvertVsyncPolairty>True</InvertVsyncPolairty>"
                         "<InvertVsyncPolarity>False</InvertVsyncPolarity>",
              EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<InvertVsyncPolairty>False</InvertVsyncPolairty>"
                         "<InvertVsyncPolarity>True</InvertVsyncPolarity>",
              EFI_COMPROMISED_DATA, 0);
  CheckPolarity (Text, Size,
                 "<InvertVsyncPolairty>True</InvertVsyncPolairty>"
                 "<InvertVsyncPolarity>True</InvertVsyncPolarity>", FALSE, TRUE);
  CheckPolarity (Text, Size,
                 "<InvertHsyncPolarity>True</InvertHsyncPolarity>"
                 "<InvertVsyncPolairty>True</InvertVsyncPolairty>",
                 TRUE, TRUE);
  CheckPolarity (Text, Size,
                 "<InvertVsyncPolarity>True</InvertVsyncPolarity>",
                 FALSE, TRUE);
  AppendCase (Text, Size, "<InvertHsyncPolarity units='Bool'>False</InvertHsyncPolarity>"
                         "<InvertVsyncPolairty>False</InvertVsyncPolairty>"
                         "<InvertDataPolarity>False</InvertDataPolarity>",
              EFI_SUCCESS, EFI_SUCCESS);
  AppendCase (Text, Size, "<InvertHsyncPolarity>1</InvertHsyncPolarity>",
              EFI_COMPROMISED_DATA, 0);
  AppendCase (Text, Size, "<InvertVsyncPolairty/>", EFI_COMPROMISED_DATA, 0);
  Width = strstr (Text, "<HorizontalSyncSkew>0");
  assert (Width != NULL);
  Width += strlen ("<HorizontalSyncSkew>");
  *Width = '1';
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (MdpValidateVideoPanel (&Panel) == EFI_UNSUPPORTED);
  *Width = '0';
  Mapping = strstr (Text, "<DSIControllerMapping>");
  assert (Mapping != NULL);
  Mapping = strstr (Mapping, "00");
  assert (Mapping != NULL);
  Mapping[1] = '1';
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (Panel.Timing.DsiController == 1);
  Mapping[1] = '2';
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_COMPROMISED_DATA);
  Mapping[1] = '0';
  Width = strstr (Text, "<HorizontalActive>1920");
  assert (Width != NULL);
  Width += strlen ("<HorizontalActive>");
  memcpy (Width, "0000", 4);
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (MdpValidateVideoPanel (&Panel) == EFI_INVALID_PARAMETER);
  free (Text);

  Text = ReadFile (argv[2], &Size);
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (MdpValidateVideoPanel (&Panel) == EFI_UNSUPPORTED);
  free (Text);
  Text = ReadFile (argv[3], &Size);
  MdpFreePanelConfig (&Panel);
  assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_SUCCESS);
  assert (MdpValidateVideoPanel (&Panel) == EFI_UNSUPPORTED);
  free (Text);
  for (Index = 4; Index < argc; Index++) {
    Text = ReadFile (argv[Index], &Size);
    MdpFreePanelConfig (&Panel);
    if (MdpParsePanelXml (Text, Size, &Panel) != EFI_SUCCESS) {
      fprintf (stderr, "Failed to parse %s\n", argv[Index]);
      abort ();
    }
    /* Some uncompressed video panels now have executable commands. */
    assert (MdpValidateVideoPanel (&Panel) == EFI_UNSUPPORTED ||
            MdpValidateVideoPanel (&Panel) == EFI_SUCCESS);
    if (Panel.ControllerCount == 2) {
      Mapping = strstr (Text, "<DSIControllerMapping>");
      assert (Mapping != NULL);
      Mapping = strstr (Mapping, "00 01");
      assert (Mapping != NULL);
      Mapping[4] = '0';
      MdpFreePanelConfig (&Panel);
      assert (MdpParsePanelXml (Text, Size, &Panel) == EFI_COMPROMISED_DATA);
    }
    free (Text);
  }
  MdpFreePanelConfig (&Panel);
  printf ("MDP panel parser: %d packaged XML files and malformed/capability cases passed\n",
          argc - 1);
  return 0;
}
