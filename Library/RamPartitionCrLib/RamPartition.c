/** Allocation-free Qualcomm SMEM RAM partition decoder. SPDX-License-Identifier: MIT */
#include <Library/RamPartitionCrLib.h>
#include <oskal/cr_string.h>

/* Read wire values bytewise: SMEM item alignment is not an ABI guarantee. */
STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
STATIC UINT64 Le64(CONST UINT8 *P) {
  return (UINT64)Le32(P) | ((UINT64)Le32(P + 4) << 32);
}
STATIC BOOLEAN Range(CONST VOID *P, UINTN Size, UINTN *End) {
  UINTN Base = (UINTN)P;
  if (P == NULL || Size == 0 || Base > (UINTN)-1 - Size) return FALSE;
  *End = Base + Size;
  return TRUE;
}

CR_STATUS CrRamPartitionParse(CONST VOID *Bytes, UINTN Size,
                             CR_RAM_PARTITION_TABLE *Out) {
  UINTN InputEnd, OutputEnd;
  if (!Range(Bytes, Size, &InputEnd) ||
      !Range(Out, sizeof(*Out), &OutputEnd) || ((UINTN)Out & 7U) != 0 ||
      ((UINTN)Bytes < OutputEnd && (UINTN)Out < InputEnd))
    return CR_INVALID_PARAMETER;
  if (Size < 20) return CR_BAD_BUFFER_SIZE;
  CONST UINT8 *P = (CONST UINT8 *)Bytes;
  if (Le32(P) != CR_RAM_PARTITION_MAGIC1 ||
      Le32(P + 4) != CR_RAM_PARTITION_MAGIC2) return CR_COMPROMISED_DATA;
  UINT32 Version = Le32(P + 8), Count = Le32(P + 16);
  UINTN HeaderSize, EntrySize;
  switch (Version) {
  case 0: HeaderSize = 20; EntrySize = 56; break;
  case 1: HeaderSize = 24; EntrySize = 64; break;
  case 2: case 3: HeaderSize = 24; EntrySize = 72; break;
  default: return CR_UNSUPPORTED;
  }
  if (Count > CR_RAM_PARTITION_MAX_ENTRIES) return CR_COMPROMISED_DATA;
  if (Size < HeaderSize || Count > (Size - HeaderSize) / EntrySize)
    return CR_BAD_BUFFER_SIZE;

  CR_RAM_PARTITION_TABLE Table;
  cr_memset(&Table, 0, sizeof(Table));
  Table.Version = Version;
  Table.EntryCount = Count;
  for (UINT32 I = 0; I < Count; ++I) {
    CONST UINT8 *Wire = P + HeaderSize + I * EntrySize;
    UINTN Attributes = Version == 0 ? 24 : 32;
    UINT32 Category = Le32(Wire + Attributes + 4);
    UINT32 Domain = Le32(Wire + Attributes + 8);
    UINT32 Type = Le32(Wire + Attributes + 12);
    if (Category != CR_RAM_PARTITION_SDRAM ||
        Type != CR_RAM_PARTITION_SYSTEM_MEMORY) continue;
    /* Domain is a bitmask; accepting unknown bits as APPS would invent
       accessibility for a format we do not understand. */
    if ((Domain & ~3U) != 0) return CR_COMPROMISED_DATA;
    /* DEFAULT_DOMAIN is unspecified, not modem-only. Qualcomm's published
       U-Boot decoder and LK DDR predicate select system SDRAM without an
       APPS-only filter. Preserve that default while excluding explicit MODEM
       ownership; the caller still applies AvailableLength and reservations. */
    if (Domain == CR_RAM_PARTITION_MODEM_DOMAIN) continue;
    CR_RAM_PARTITION_ENTRY E;
    cr_memset(&E, 0, sizeof(E));
    cr_memcpy(E.Name, Wire, sizeof(E.Name));
    E.Base = Version == 0 ? Le32(Wire + 16) : Le64(Wire + 16);
    E.Length = Version == 0 ? Le32(Wire + 20) : Le64(Wire + 24);
    E.AvailableLength = Version >= 2 ? Le64(Wire + 64) : E.Length;
    E.Attribute = Le32(Wire + Attributes);
    E.Category = Category;
    E.Domain = Domain;
    E.Type = Type;
    E.NumPartitions = Le32(Wire + Attributes + 16);
    if (Version != 0) E.HardwareInfo = Le32(Wire + 52);
    if (Version == 3) {
      E.HighestBankBit = Wire[56];
      E.MinPasrSize = Le32(Wire + 60);
    }
    if (E.Base > CR_MAX_UINT64 - E.Length ||
        E.AvailableLength > E.Length ||
        ((E.Base | E.Length | E.AvailableLength) & 0xfffU) != 0)
      return CR_COMPROMISED_DATA;
    /* XBL can retain an empty system-DDR record (HDK8350 v3). It owns no
       bytes and must not enter the sorted inventory or overlap a real bank.
       The checks above still reject a nonzero available length or bad base. */
    if (E.Length == 0) continue;
    if (Table.PhysicalBytes > CR_MAX_UINT64 - E.Length ||
        Table.AvailableBytes > CR_MAX_UINT64 - E.AvailableLength)
      return CR_COMPROMISED_DATA;
    /* Sort without merging: even two adjacent banks can carry different
       attributes or independent available tails. */
    UINT32 Slot = Table.BankCount;
    while (Slot != 0 && Table.Banks[Slot - 1].Base > E.Base) {
      cr_memcpy(&Table.Banks[Slot], &Table.Banks[Slot - 1], sizeof(E));
      --Slot;
    }
    cr_memcpy(&Table.Banks[Slot], &E, sizeof(E));
    ++Table.BankCount;
    Table.PhysicalBytes += E.Length;
    Table.AvailableBytes += E.AvailableLength;
  }
  if (Table.BankCount == 0) return CR_NOT_FOUND;
  for (UINT32 I = 1; I < Table.BankCount; ++I)
    if (Table.Banks[I - 1].Base + Table.Banks[I - 1].Length > Table.Banks[I].Base)
      return CR_COMPROMISED_DATA;
  cr_memcpy(Out, &Table, sizeof(Table));
  return CR_SUCCESS;
}
