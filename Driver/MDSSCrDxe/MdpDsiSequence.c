/**
  XML rows contain a packet type followed by hexadecimal payload bytes.
  Delay evidence: Panel_truly_1080p_vid.xml ends in 05 11 00 / ff 78 /
  05 29 00 / ff 78. Android dsi-panel-nt35695b-truly-fhd-video.dtsi
  has matching 11/29 packets with header wait byte 78; dsi_panel.c sets
  post_wait_ms=data[4] and sleeps in milliseconds. XML ff is a separate
  delay row, never an Android seven-byte packet header.
  Generic/DCS short-write lengths follow Linux drm_mipi_dsi.c and
  include/video/mipi_display.h; unknown types (including DSC/PPS) are
  retained for diagnostics but cannot execute.
**/
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include "MdpDsiSequence.h"

STATIC INTN Hex (CHAR8 C) {
  if (C >= '0' && C <= '9') return C - '0';
  if (C >= 'a' && C <= 'f') return C - 'a' + 10;
  if (C >= 'A' && C <= 'F') return C - 'A' + 10;
  return -1;
}
STATIC BOOLEAN Blank (CHAR8 C) { return C == ' ' || C == '\t' || C == '\r'; }
VOID MdpFreeDsiSequence (MDP_DSI_SEQUENCE *Sequence) {
  if (Sequence == NULL) return;
  if (Sequence->Commands != NULL) FreePool (Sequence->Commands);
  ZeroMem (Sequence, sizeof (*Sequence));
}
EFI_STATUS MdpParseDsiSequence (CONST CHAR8 *Text, UINTN Size, MDP_DSI_SEQUENCE *Sequence) {
  UINTN I, Rows = 1, At = 0, Start, Bytes, Total;
  UINT8 *Payload;
  MDP_DSI_COMMAND *Command;
  INTN High, Low;
  if (Sequence == NULL || (Text == NULL && Size != 0)) return EFI_INVALID_PARAMETER;
  ZeroMem (Sequence, sizeof (*Sequence));
  if (Size == 0) return EFI_SUCCESS;
  /* Bound allocation arithmetic and untrusted configuration memory usage. */
  if (Size > 1024 * 1024) return EFI_BAD_BUFFER_SIZE;
  for (I = 0; I < Size; I++) if (Text[I] == '\n') Rows++;
  Total = Rows * sizeof (*Command) + Size;
  Sequence->Commands = AllocateZeroPool (Total);
  if (Sequence->Commands == NULL) return EFI_OUT_OF_RESOURCES;
  Payload = (UINT8 *)(Sequence->Commands + Rows);
  for (I = 0; I < Size;) {
    while (I < Size && (Blank (Text[I]) || Text[I] == '\n')) I++;
    if (I == Size) break;
    Start = At;
    while (I < Size && Text[I] != '\n') {
      if (Blank (Text[I])) { I++; continue; }
      if (Size - I < 2) goto Malformed;
      High = Hex (Text[I]); Low = Hex (Text[I + 1]);
      if (High < 0 || Low < 0) goto Malformed;
      Payload[At++] = (UINT8)((High << 4) | Low);
      I += 2;
      if (I < Size && !Blank (Text[I]) && Text[I] != '\n') goto Malformed;
    }
    Bytes = At - Start;
    Command = &Sequence->Commands[Sequence->Count++];
    Command->DataType = Payload[Start];
    Command->Payload = Payload + Start + 1;
    Command->Length = Bytes - 1;
    switch (Command->DataType) {
    case 0xff:
      if (Command->Length != 1) goto Malformed;
      Command->DelayMs = Command->Payload[0];
      Command->Length = 0;
      break;
    case 0x05:
      /* OEM files pad DCS short writes with a zero second data byte. */
      if (Command->Length == 2 && Command->Payload[1] == 0) Command->Length = 1;
      if (Command->Length != 1) goto Malformed;
      break;
    case 0x03: if (Command->Length != 0) goto Malformed; break;
    case 0x13: if (Command->Length != 1) goto Malformed; break;
    case 0x15:
    case 0x23: if (Command->Length != 2) goto Malformed; break;
    case 0x29:
    case 0x39:
      if (Command->Length == 0 || Command->Length > 65535) goto Malformed;
      if (Command->Length > MDP_DSI_MAX_WRITE_PAYLOAD) Sequence->Unsupported = TRUE;
      break;
    default: Sequence->Unsupported = TRUE; break;
    }
  }
  return EFI_SUCCESS;
Malformed:
  MdpFreeDsiSequence (Sequence);
  return EFI_COMPROMISED_DATA;
}
