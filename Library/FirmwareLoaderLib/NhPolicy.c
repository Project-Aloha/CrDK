/* SPDX-License-Identifier: MIT. One selection policy for Windows and UEFI. */
#include <Library/FirmwareLoaderLib/NhPolicy.h>
#include <Library/FirmwareLoaderLib/NhBootSlot.h>
#include "NhString.h"

NH_STATUS NhBootSlotDecode(const void *record, size_t bytes,
                          uint32_t attributes, uint32_t *slot) {
  const uint8_t *raw = record;
  uint32_t state, selected, sources;
  if (!slot) return NH_INVALID;
  *slot = NH_BOOT_SLOT_UNKNOWN;
  if (!record) return NH_INVALID;
  if (bytes != NH_BOOT_SLOT_RECORD_BYTES ||
      attributes != NH_BOOT_SLOT_VARIABLE_ATTRIBUTES) return NH_CORRUPT;
  if (NhU32(raw) != NH_BOOT_SLOT_SIGNATURE ||
      NhU32(raw+4) != NH_BOOT_SLOT_VERSION ||
      NhU32(raw+8) != NH_BOOT_SLOT_RECORD_BYTES ||
      NhU32(raw+24) != NH_BOOT_SLOT_INITIALIZED || NhU32(raw+28))
    return NH_CORRUPT;
  state = NhU32(raw+12); selected = NhU32(raw+16); sources = NhU32(raw+20);
  if (state > NH_BOOT_SLOT_CONFLICT || (sources & ~NH_BOOT_SLOT_SOURCES))
    return NH_CORRUPT;
  if (state == NH_BOOT_SLOT_VALID) {
    if (selected > 1 || !sources) return NH_CORRUPT;
    *slot = selected;
    return NH_OK;
  }
  if (selected != NH_BOOT_SLOT_UNKNOWN) return NH_CORRUPT;
  return (state == NH_BOOT_SLOT_UNAVAILABLE || state == NH_BOOT_SLOT_ABSENT)
    ? NH_NOT_READY : NH_CORRUPT;
}

static NH_STATUS partition_name(char *name, uint32_t slot, uint32_t *needs) {
  size_t n = strlen(name);
  char *mark = strchr(name, '{');
  if (mark) {
    if (strcmp(mark, "{slot}") || mark == name) return NH_INVALID;
    *needs = 1;
    if (slot != NH_SLOT_UNKNOWN) {
      mark[0] = '_'; mark[1] = (char)('a' + slot); mark[2] = 0;
      n = (size_t)(mark - name) + 2;
    }
  }
  if (strchr(name, '}') && !mark) return NH_INVALID;
  for (size_t i = 0; i < n; ++i) {
    char c = name[i];
    if (mark && slot == NH_SLOT_UNKNOWN && name + i >= mark) break;
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return NH_INVALID;
  }
  if (!n || NhPathCheck(name, 0) != NH_OK) return NH_INVALID;
  if (n >= 2 && name[n-2] == '_' && (name[n-1] == 'a' || name[n-1] == 'b')) {
    *needs = 1;
    if (slot != NH_SLOT_UNKNOWN && name[n-1] != (char)('a' + slot)) return NH_INVALID;
  }
  return NH_OK;
}

NH_STATUS NhCandidateParse(const char *text, uint32_t slot, NH_CANDIDATE *out) {
  size_t length = 0, first = NH_MAX_PATH, second = NH_MAX_PATH;
  if (!text || !out || (slot != NH_SLOT_UNKNOWN && slot > 1)) return NH_INVALID;
  memset(out, 0, sizeof(*out));
  while (length < NH_MAX_PATH && text[length]) {
    unsigned char c = (unsigned char)text[length];
    if (c < 32 || c >= 127) return NH_INVALID;
    if (c == '|') {
      if (first == NH_MAX_PATH) first = length;
      else if (second == NH_MAX_PATH) second = length;
      else return NH_INVALID;
    }
    ++length;
  }
  if (length == NH_MAX_PATH || !first || first >= sizeof(out->Partition) ||
      second == NH_MAX_PATH || second-first-1 >= sizeof(out->Logical) ||
      second+1 == length) return NH_INVALID;
  memcpy(out->Partition, text, first);
  memcpy(out->Logical, text+first+1, second-first-1);
  memcpy(out->Path, text+second+1, length-second);
  NH_TRY(partition_name(out->Partition, slot, &out->NeedsSlot));
  if (out->Logical[0]) {
    if (strcmp(out->Partition, "super")) return NH_INVALID;
    out->NeedsSlot = 1;
    NH_TRY(partition_name(out->Logical, slot, &out->NeedsSlot));
  }
  return NhPathCheck(out->Path, 0);
}

NH_STATUS NhBootControlSlot(const uint8_t control[32], uint32_t *slot) {
  if (!control || !slot) return NH_INVALID;
  if (NhU32(control+4) != 0x42414342U || control[8] != 1 ||
      (control[9]&7) != 2 || control[0] != '_' ||
      (control[1] != 'a' && control[1] != 'b') || control[2] || control[3])
    return NH_NOT_READY;
  if ((NhCrc32(~0U, control, 28, 0xedb88320U)^~0U) != NhU32(control+28))
    return NH_CORRUPT;
  uint32_t active = (uint32_t)(control[1]-'a');
  uint8_t state = control[12+active*2];
  if (!(state&15) || (!(state&0x80) && !(state&0x70)) ||
      (control[13+active*2]&1)) return NH_NOT_READY;
  *slot = active;
  return NH_OK;
}

static int arg_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
NH_STATUS NhBootArgsSlot(const char *args, size_t bytes, uint32_t *slot) {
  const char suffix_key[] = "androidboot.slot_suffix=";
  const char slot_key[] = "androidboot.slot=";
  uint32_t found = NH_SLOT_UNKNOWN;
  size_t i = 0;
  if (!args || !slot || bytes > 65536) return NH_INVALID;
  while (i < bytes && args[i]) {
    while (i < bytes && arg_space(args[i])) ++i;
    size_t start = i;
    while (i < bytes && args[i] && !arg_space(args[i])) ++i;
    size_t end = i, key = 0;
    if (end-start >= sizeof(suffix_key)-1 &&
        !memcmp(args+start, suffix_key, sizeof(suffix_key)-1)) key = sizeof(suffix_key)-1;
    else if (end-start >= sizeof(slot_key)-1 &&
             !memcmp(args+start, slot_key, sizeof(slot_key)-1)) key = sizeof(slot_key)-1;
    if (!key) continue;
    start += key;
    if (start < end && args[start] == '_') ++start;
    if (end-start != 1 || (args[start] != 'a' && args[start] != 'b')) return NH_CORRUPT;
    uint32_t value = (uint32_t)(args[start]-'a');
    if (found != NH_SLOT_UNKNOWN && found != value) return NH_CORRUPT;
    found = value;
  }
  if (found == NH_SLOT_UNKNOWN) return NH_NOT_FOUND;
  *slot = found;
  return NH_OK;
}

NH_STATUS NhSnapshotCheck(const uint8_t message[64]) {
  if (!message) return NH_INVALID;
  /* AOSP misc_virtual_ab_message v2: only NONE permits reading base extents. */
  if (message[0] != 2 || NhU32(message+1) != 0x56740ab0U ||
      message[5] != 0 || message[6] > 1) return NH_NOT_READY;
  for (unsigned i = 7; i < 64; ++i) if (message[i]) return NH_UNSUPPORTED;
  return NH_OK;
}

typedef struct { const NH_POLICY_IO *Io; NH_SOURCE Source; } POLICY_SOURCE;
static void *policy_alloc(void *ctx, size_t n) {
  POLICY_SOURCE *s = ctx;
  return s->Io->Alloc(s->Io->Context, n);
}
static void policy_free(void *ctx, void *p) {
  POLICY_SOURCE *s = ctx;
  s->Io->Free(s->Io->Context, p);
}
static NH_STATUS policy_load(void *ctx, const char *path, void **data, uint32_t *bytes) {
  POLICY_SOURCE *s = ctx;
  return s->Source.Load(s->Source.Context, path, data, bytes);
}
static NH_STATUS read_source(POLICY_SOURCE *source, const char *path,
                             uint32_t kind, NH_BUNDLE **out) {
  if (!source->Source.Load || !source->Source.Close) return NH_INVALID;
  return NhBundleRead(source, policy_load, policy_alloc, policy_free, path, kind, out);
}

NH_STATUS NhPolicyRead(const NH_POLICY *policy, const NH_POLICY_IO *io,
                       NH_BUNDLE **out, uint32_t *index) {
  NH_CANDIDATE candidate;
  uint32_t needs = 0, slot = NH_SLOT_UNKNOWN, selected = UINT32_MAX;
  NH_STATUS status = NH_NOT_FOUND;
  int begun = 0;
  POLICY_SOURCE source;
  if (!out) return NH_INVALID;
  *out = NULL;
  if (index) *index = UINT32_MAX;
  if (!policy || !io || !io->Alloc || !io->Free || policy->Kind > NH_BUNDLE_PIL ||
      policy->AndroidCount > NH_MAX_CANDIDATES || policy->PackageCount > NH_MAX_CANDIDATES ||
      (policy->AndroidCount && (!policy->AndroidPaths || !io->Begin || !io->OpenAndroid || !io->Stable || !io->End)) ||
      (policy->PackageCount && (!policy->PackagePaths || !io->OpenPackage))) return NH_INVALID;
  /* Validate the complete policy before any read: a malformed later entry is
   * not hidden by a successful earlier candidate. */
  for (uint32_t i = 0; i < policy->AndroidCount; ++i) {
    NH_TRY(NhCandidateParse(policy->AndroidPaths[i], NH_SLOT_UNKNOWN, &candidate));
    needs |= candidate.NeedsSlot;
  }
  for (uint32_t i = 0; i < policy->PackageCount; ++i)
    NH_TRY(NhPathCheck(policy->PackagePaths[i], 0));
  source.Io = io;
  memset(&source.Source, 0, sizeof(source.Source));
  if (policy->AndroidCount) {
    begun = 1;
    status = io->Begin(io->Context, needs, &slot);
    if (status == NH_NOT_FOUND) status = NH_NOT_READY;
    if (status != NH_OK) goto done;
    if (needs && slot > 1) { status = NH_NOT_READY; goto done; }
    for (uint32_t i = 0; i < policy->AndroidCount; ++i) {
      status = NhCandidateParse(policy->AndroidPaths[i], slot, &candidate);
      if (status != NH_OK) goto done;
    }
    status = NH_NOT_FOUND;
    for (uint32_t i = 0; i < policy->AndroidCount; ++i) {
      status = NhCandidateParse(policy->AndroidPaths[i], slot, &candidate);
      if (status != NH_OK) goto done;
      memset(&source.Source, 0, sizeof(source.Source));
      status = io->OpenAndroid(io->Context, &candidate, &source.Source);
      if (status == NH_OK) {
        status = read_source(&source, candidate.Path, policy->Kind, out);
        if (source.Source.Close) source.Source.Close(source.Source.Context);
      }
      if (status == NH_OK) { (*out)->Origin = NH_ORIGIN_ANDROID; selected = i; break; }
      if (status != NH_NOT_FOUND) goto done;
    }
    /* Revalidate confirmed absence too; a slot/media change cannot authorize
     * falling back to a packaged image from a different software revision. */
    NH_STATUS stable = io->Stable(io->Context);
    if (stable == NH_NOT_FOUND) stable = NH_NOT_READY;
    if (stable != NH_OK) { status = stable; goto done; }
    if (status == NH_OK) goto done;
  }
  if (!policy->PackageCount) goto done;
  memset(&source.Source, 0, sizeof(source.Source));
  status = io->OpenPackage(io->Context, &source.Source);
  if (status != NH_OK) goto done;
  for (uint32_t i = 0; i < policy->PackageCount; ++i) {
    status = read_source(&source, policy->PackagePaths[i], policy->Kind, out);
    if (status == NH_OK) { (*out)->Origin = NH_ORIGIN_PACKAGE; selected = i; break; }
    if (status != NH_NOT_FOUND) break;
  }
  if (source.Source.Close) source.Source.Close(source.Source.Context);
done:
  if (begun) io->End(io->Context);
  if (status != NH_OK) { NhBundleFree(io->Context, io->Free, *out); *out = NULL; }
  else if (index) *index = selected;
  return status;
}
