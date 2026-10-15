/* SPDX-License-Identifier: MIT. Shared read-only firmware selection policy. */
#ifndef NH_POLICY_H
#define NH_POLICY_H
#include <Library/FirmwareLoaderLib/NhBundle.h>

#define NH_SLOT_UNKNOWN UINT32_MAX
#define NH_MAX_CANDIDATES 64U
#define NH_ORIGIN_ANDROID 1U
#define NH_ORIGIN_PACKAGE 2U

typedef struct {
  char Partition[36];
  char Logical[36];
  char Path[NH_MAX_PATH];
  uint32_t NeedsSlot;
} NH_CANDIDATE;

/* With Slot=NH_SLOT_UNKNOWN, validate syntax and report NeedsSlot without
 * replacing {slot} or approving an explicit slot. Otherwise reject a name
 * selecting the other slot. Only super may contain a logical partition. */
NH_STATUS NhCandidateParse(const char *Text, uint32_t Slot, NH_CANDIDATE *Out);
NH_STATUS NhBootControlSlot(const uint8_t Control[32], uint32_t *Slot);
NH_STATUS NhBootArgsSlot(const char *Args, size_t Bytes, uint32_t *Slot);
NH_STATUS NhSnapshotCheck(const uint8_t Message[64]);

typedef struct {
  const char *const *AndroidPaths;
  uint32_t AndroidCount;
  const char *const *PackagePaths;
  uint32_t PackageCount;
  uint32_t Kind;
} NH_POLICY;

/* A source owns one opened filesystem or consumer package. Load must never
 * select another source. A successful Load returns memory from PolicyIo.Alloc
 * with PolicyIo.Context; any unsuccessful Load returns NULL and zero length.
 * Close is invoked after each successful Open, including a failed group read. */
typedef struct {
  void *Context;
  NH_LOAD_FILE Load;
  void (*Close)(void *Context);
} NH_SOURCE;

typedef struct {
  void *Context;
  NH_ALLOC Alloc;
  NH_FREE Free;
  /* Begin binds the current physical device, and obtains a stable active slot
   * when requested. No arbitrary disk scan or caller-controlled raw offset. */
  NH_STATUS (*Begin)(void *Context, uint32_t NeedsSlot, uint32_t *Slot);
  NH_STATUS (*OpenAndroid)(void *Context, const NH_CANDIDATE *, NH_SOURCE *);
  /* Recheck device/slot/media and any Virtual A/B state observed by Open.
   * Called after success or total absence, before returning/fallback. */
  NH_STATUS (*Stable)(void *Context);
  NH_STATUS (*OpenPackage)(void *Context, NH_SOURCE *);
  /* End is also called when Begin failed, allowing partial cleanup. */
  void (*End)(void *Context);
} NH_POLICY_IO;

/* Empty AndroidCount explicitly selects package-only. Any error other than
 * NH_NOT_FOUND stops the request. Missing secondary PIL parts are corruption,
 * never fallback. Returned private bundle is released with Io.Free/Context.
 * CandidateIndex is optional, and refers to the winning origin's path list. */
NH_STATUS NhPolicyRead(const NH_POLICY *, const NH_POLICY_IO *, NH_BUNDLE **,
                       uint32_t *CandidateIndex);
#endif
