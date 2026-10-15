/* SPDX-License-Identifier: MIT. UEFI GPT format, read-only bounded lookup. */
#include "NhInternal.h"

#define GPT_TABLE_MAX (1024U * 1024U)
typedef struct {
  uint64_t First, Last, Table;
  uint32_t Count, Stride, Crc;
  uint8_t Guid[16];
} GPT_HEADER;

static int zero(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) if (p[i]) return 0;
  return 1;
}

static NH_STATUS header(const NH_READER *r, uint32_t sector, uint64_t lba,
                        uint64_t other, uint8_t *buffer, GPT_HEADER *out) {
  NH_TRY(NhRead(r, lba*sector, buffer, sector));
  if (memcmp(buffer, "EFI PART", 8)) return NH_UNSUPPORTED;
  uint32_t length = NhU32(buffer+12), crc = NhU32(buffer+16);
  if (NhU32(buffer+8) != 0x00010000 || length < 92 || length > sector ||
      NhU32(buffer+20) || NhU64(buffer+24) != lba || NhU64(buffer+32) != other)
    return NH_CORRUPT;
  memset(buffer+16, 0, 4);
  if ((NhCrc32(~0U, buffer, length, 0xedb88320U)^~0U) != crc) return NH_CORRUPT;
  if (length > 92 && !zero(buffer+92, length-92)) return NH_CORRUPT;
  out->First = NhU64(buffer+40); out->Last = NhU64(buffer+48);
  out->Table = NhU64(buffer+72); out->Count = NhU32(buffer+80);
  out->Stride = NhU32(buffer+84); out->Crc = NhU32(buffer+88);
  memcpy(out->Guid, buffer+56, 16);
  if (!out->Count || out->Count > 4096 || out->Stride < 128 || out->Stride > 1024 ||
      (out->Stride&(out->Stride-1)) || out->Count > GPT_TABLE_MAX/out->Stride ||
      out->First < 2 || out->First > out->Last || out->Last >= r->Bytes/sector-1 ||
      zero(out->Guid, 16)) return NH_CORRUPT;
  uint64_t blocks = ((uint64_t)out->Count*out->Stride+sector-1)/sector;
  if (out->Table < 2 || out->Table >= r->Bytes/sector-1 ||
      blocks > r->Bytes/sector-1-out->Table) return NH_CORRUPT;
  /* Both arrays live outside the usable partition range and their own header. */
  if (lba == 1) {
    if (out->Table >= out->First || blocks > out->First-out->Table) return NH_CORRUPT;
  } else if (out->Table <= out->Last) return NH_CORRUPT;
  return NH_OK;
}

static int name_equal(const uint8_t *entry, const char *name) {
  size_t n = strlen(name);
  for (size_t i = 0; i < 36; ++i) {
    uint16_t c = NhU16(entry+56+i*2);
    if (i < n) { if (c != (unsigned char)name[i]) return 0; }
    else if (c) return 0;
  }
  return 1;
}

NH_STATUS NhGptFind(const NH_READER *r, uint32_t sector, const char *name,
                    uint64_t *offset, uint64_t *bytes) {
  GPT_HEADER a, b;
  uint8_t *block = NULL, *table = NULL;
  NH_STATUS status, sa, sb;
  if (!r || !r->Read || !r->Alloc || !r->Free || !offset || !bytes ||
      NhPathCheck(name, 0) != NH_OK || strlen(name) >= 36 || strchr(name, '/') ||
      (sector != 512 && sector != 4096) || r->Bytes%sector || r->Bytes/sector < 6)
    return NH_INVALID;
  *offset = *bytes = 0;
  block = r->Alloc(r->Context, sector);
  if (!block) return NH_MEMORY;
  uint64_t last = r->Bytes/sector-1;
  sa = header(r, sector, 1, last, block, &a);
  sb = header(r, sector, last, 1, block, &b);
  if (sa != NH_OK && sa != NH_CORRUPT && sa != NH_UNSUPPORTED) { status = sa; goto done; }
  if (sb != NH_OK && sb != NH_CORRUPT && sb != NH_UNSUPPORTED) { status = sb; goto done; }
  if (sa == NH_UNSUPPORTED && sb == NH_UNSUPPORTED) {
    /* A protective MBR is evidence of a GPT disk even when both signatures
     * were lost. Do not misclassify that damaged disk as an unpartitioned LUN. */
    status = NhRead(r, 0, block, 512);
    if (status != NH_OK) goto done;
    status = NH_UNSUPPORTED;
    if (NhU16(block+510) == 0xaa55) {
      for (unsigned i = 0; i < 4; ++i)
        if (block[446+i*16+4] == 0xee) status = NH_CORRUPT;
    }
    goto done;
  }
  if (sa != NH_OK || sb != NH_OK) { status = NH_CORRUPT; goto done; }
  if (a.First != b.First || a.Last != b.Last || a.Count != b.Count ||
      a.Stride != b.Stride || a.Crc != b.Crc || memcmp(a.Guid,b.Guid,16)) {
    status = NH_CORRUPT; goto done;
  }
  size_t length = (size_t)a.Count*a.Stride;
  table = r->Alloc(r->Context, length);
  if (!table) { status = NH_MEMORY; goto done; }
  status = NhRead(r, a.Table*sector, table, length);
  if (status != NH_OK) goto done;
  if ((NhCrc32(~0U,table,length,0xedb88320U)^~0U) != a.Crc) { status = NH_CORRUPT; goto done; }
  for (size_t at = 0; at < length; at += sector) {
    size_t take = NH_MIN(length-at, sector);
    status = NhRead(r, b.Table*sector+at, block, take);
    if (status != NH_OK) goto done;
    if (memcmp(table+at,block,take)) { status = NH_CORRUPT; goto done; }
  }
  int found = 0;
  for (uint32_t i = 0; i < a.Count; ++i) {
    const uint8_t *entry = table+(size_t)i*a.Stride;
    if (zero(entry,16)) continue;
    uint64_t first = NhU64(entry+32), end = NhU64(entry+40);
    if (first < a.First || first > end || end > a.Last || zero(entry+16,16)) {
      status = NH_CORRUPT; goto done;
    }
    for (uint32_t j = 0; j < i; ++j) {
      const uint8_t *prev = table+(size_t)j*a.Stride;
      if (zero(prev,16)) continue;
      if ((first <= NhU64(prev+40) && NhU64(prev+32) <= end) ||
          !memcmp(entry+16,prev+16,16)) { status = NH_CORRUPT; goto done; }
    }
    if (name_equal(entry,name)) {
      if (found) { status = NH_CORRUPT; goto done; }
      found = 1; *offset = first*sector; *bytes = (end-first+1)*sector;
    }
  }
  status = found ? NH_OK : NH_NOT_FOUND;
done:
  r->Free(r->Context,table); r->Free(r->Context,block);
  if (status != NH_OK) *offset = *bytes = 0;
  return status;
}
