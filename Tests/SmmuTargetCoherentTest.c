/** Actual target data oracle; no SMMU initialization or simulated DMA.
 * SPDX-License-Identifier: MIT
 */
#include <Library/CrTargetSmmuLib.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* Keep the pre-extension layout independent of the production declaration. */
typedef struct {
  UINT64 Base;
  UINT64 Size;
  UINT16 SegmentCount;
  UINT16 MaximumDomains;
  CONST UINT16 *DefaultStreamIds;
  UINT16 DefaultStreamCount;
  BOOLEAN ExternalIoMmuOwnsPcieStreams;
  CONST SmmuActlrSetting *ActlrSettings;
  UINTN ActlrSettingCount;
  UINT32 SctlrReadbackIgnoreMask;
  CONST CrTargetSmmuDmaDevice *DmaDevices;
  UINTN DmaDeviceCount;
} LEGACY_TARGET_CONTEXT;

#define SAME_OFFSET(Field) \
  _Static_assert(offsetof(CrTargetSmmuContext, Field) == \
                 offsetof(LEGACY_TARGET_CONTEXT, Field), #Field " moved")
SAME_OFFSET(Base);
SAME_OFFSET(Size);
SAME_OFFSET(SegmentCount);
SAME_OFFSET(MaximumDomains);
SAME_OFFSET(DefaultStreamIds);
SAME_OFFSET(DefaultStreamCount);
SAME_OFFSET(ExternalIoMmuOwnsPcieStreams);
SAME_OFFSET(ActlrSettings);
SAME_OFFSET(ActlrSettingCount);
SAME_OFFSET(SctlrReadbackIgnoreMask);
SAME_OFFSET(DmaDevices);
SAME_OFFSET(DmaDeviceCount);
_Static_assert(offsetof(CrTargetSmmuContext, CoherentStreamIds) ==
               sizeof(LEGACY_TARGET_CONTEXT), "coherent metadata is a tail");

static unsigned Checks;
#define CHECK(Condition) do { \
  ++Checks; \
  if (!(Condition)) { \
    fprintf(stderr, "line %u: %s\n", __LINE__, #Condition); \
    abort(); \
  } \
} while (0)

static BOOLEAN
Contains(CONST UINT16 *Ids, UINTN Count, UINT16 Id)
{
  UINTN Index;
  for (Index = 0; Index < Count; ++Index) {
    if (Ids[Index] == Id) {
      return TRUE;
    }
  }
  return FALSE;
}

int
main(void)
{
  CrTargetSmmuContext *Target = CrTargetGetSmmuContext();
  UINTN Index;
  UINTN Other;
#if TEST_SOC == 8450
  static CONST UINT16 Default[] = {
    0, 0x1c00, 0x1c01, 0x1c80, 0x1c81, 0x56, 0x43, 0xe0,
    0x2800, 0x2802, 0x2c00, 0x2c02
  };
  /* UFS pairs coherent translation with CPU WB common buffers. */
  static CONST UINT16 Coherent[] = {0x1c00, 0x1c01, 0x1c80, 0x1c81, 0xe0};
#elif TEST_SOC == 8250
  static CONST UINT16 Default[] = {0xe0, 0x4e0, 0x1c00, 0x1c01, 0x1c80, 0x1c81, 0x1d00, 0x1d01};
  static CONST UINT16 Coherent[] = {0x1c00, 0x1c01, 0x1c80, 0x1c81, 0x1d00, 0x1d01};
#elif TEST_SOC == 8350
  static CONST UINT16 Default[] = {0xe0, 0x1c00, 0x1c01, 0x1c80, 0x1c81};
  static CONST UINT16 Coherent[] = {0x1c00, 0x1c01, 0x1c80, 0x1c81};
#else
#error Unknown target oracle
#endif

  CHECK(Target != NULL);
  CHECK(Target == CrTargetGetSmmuContext());
  CHECK(Target->Base == 0x15000000ULL);
  CHECK(Target->Size == 0x100000ULL);
  CHECK(!Target->ExternalIoMmuOwnsPcieStreams);
  CHECK(Target->DefaultStreamCount == sizeof(Default) / sizeof(Default[0]));
  CHECK(Target->DefaultStreamIds != NULL);
  for (Index = 0; Index < Target->DefaultStreamCount; ++Index) {
    CHECK(Target->DefaultStreamIds[Index] == Default[Index]);
    for (Other = 0; Other < Index; ++Other) {
      CHECK(Target->DefaultStreamIds[Index] != Target->DefaultStreamIds[Other]);
    }
  }

#if TEST_SOC == 8450
  CHECK(Target->SegmentCount == 2);
  CHECK(Target->MaximumDomains == 16);
  CHECK(Target->ActlrSettingCount == 1);
  CHECK(Target->ActlrSettings != NULL);
  CHECK(Target->ActlrSettings[0].Sid == 0x2800);
  CHECK(Target->ActlrSettings[0].Mask == 0x402);
  CHECK(Target->ActlrSettings[0].Value == 1);
  CHECK(Target->SctlrReadbackIgnoreMask == (1U << 12));
  CHECK(Target->DmaDeviceCount == 1);
  CHECK(Target->DmaDevices != NULL);
  CHECK(Target->DmaDevices[0].Base == 0xa600000);
  CHECK(Target->DmaDevices[0].Size == 0x100000);
  CHECK(Target->DmaDevices[0].Sid == 0);
  CHECK(Target->CoherentStreamCount == sizeof(Coherent) / sizeof(Coherent[0]));
  CHECK(Target->CoherentStreamIds != NULL);
  for (Index = 0; Index < Target->CoherentStreamCount; ++Index) {
    CHECK(Target->CoherentStreamIds[Index] == Coherent[Index]);
    CHECK(Contains(Target->DefaultStreamIds, Target->DefaultStreamCount,
                   Target->CoherentStreamIds[Index]));
    for (Other = 0; Other < Index; ++Other) {
      CHECK(Target->CoherentStreamIds[Index] != Target->CoherentStreamIds[Other]);
    }
  }
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0));
  CHECK(Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0xe0));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x56));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x43));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x2800));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x2802));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x2c00));
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0x2c02));
#else
  CHECK(Target->SegmentCount == (TEST_SOC == 8250 ? 3 : 2));
  CHECK(Target->MaximumDomains == sizeof(Default) / sizeof(Default[0]));
  CHECK(Target->ActlrSettingCount == 0);
  CHECK(Target->ActlrSettings == NULL);
  CHECK(Target->SctlrReadbackIgnoreMask == (1U << 12));
  CHECK(Target->DmaDeviceCount == 0);
  CHECK(Target->DmaDevices == NULL);
  CHECK(Target->CoherentStreamCount == sizeof(Coherent) / sizeof(Coherent[0]));
  CHECK(Target->CoherentStreamIds != NULL);
  for (Index = 0; Index < Target->CoherentStreamCount; ++Index) {
    CHECK(Target->CoherentStreamIds[Index] == Coherent[Index]);
    CHECK(Contains(Target->DefaultStreamIds, Target->DefaultStreamCount, Coherent[Index]));
  }
  CHECK(!Contains(Target->CoherentStreamIds, Target->CoherentStreamCount, 0xe0));
  CHECK(Contains(Target->DefaultStreamIds, Target->DefaultStreamCount, 0xe0));
#endif
  printf("SM%u actual target coherent policy: %u checks PASS\n", TEST_SOC, Checks);
  return 0;
}
