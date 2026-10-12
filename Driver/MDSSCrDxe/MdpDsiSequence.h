/** Owned, bounded panel XML command rows (not Android packet headers). */
#ifndef MDP_DSI_SEQUENCE_H_
#define MDP_DSI_SEQUENCE_H_
#include <Uefi.h>
/* One 4096-byte DMA page, less the four-byte packet header. */
#define MDP_DSI_MAX_WRITE_PAYLOAD 4092U
typedef struct {
  UINT8 DataType;
  CONST UINT8 *Payload;
  UINTN Length;
  UINT32 DelayMs;
} MDP_DSI_COMMAND;
typedef struct {
  MDP_DSI_COMMAND *Commands;
  UINTN Count;
  BOOLEAN Unsupported;
} MDP_DSI_SEQUENCE;
EFI_STATUS MdpParseDsiSequence (CONST CHAR8 *Text, UINTN Size, MDP_DSI_SEQUENCE *Sequence);
VOID MdpFreeDsiSequence (MDP_DSI_SEQUENCE *Sequence);
#endif
