/* Private GPUH ABI: immutable early stopped handoff, never a DMA grant. MIT */
#pragma once
#include <Library/AdrenoRouteCrLib.h>
#include <Library/AdrenoStopCrLib.h>
#define CR_ADRENO_GPUH_SIGNATURE 0x48555047U
#define CR_ADRENO_RECORD_SIGNATURE 0x48525047U
#define CR_ADRENO_HANDOFF_VERSION 1U
#define CR_ADRENO_HANDOFF_FLAGS 3U /* stopped before table publication + native permanently closed */
#pragma pack(push,1)
typedef struct {
 UINT32 Signature,Length;UINT8 Revision,Checksum,OemId[6],OemTableId[8];
 UINT32 OemRevision,CreatorId,CreatorRevision,WireVersion;
 UINT64 RecordPhysical;UINT32 RecordBytes,Reserved;UINT64 Incarnation;
} CR_ADRENO_HANDOFF_LOCATOR;
#pragma pack(pop)
CR_STATIC_ASSERT(sizeof(CR_ADRENO_HANDOFF_LOCATOR)==64,"GPUH locator layout");
typedef struct {
 UINT32 Signature,Version,Size,Crc32,Flags,ChipId,Reserved[2];
 UINT64 Incarnation,RecordPhysical;
 CR_ADRENO_ROUTE_FACTS Route;
 CR_ADRENO_STOP_OBSERVATION Stop;
 UINT32 SmmuId[3],SmmuControl,StreamCount,ContextCount,RetiredStreams,RetiredContexts;
 UINT32 BeforeInventoryCrc,AfterInventoryCrc,ReservedTail[2];
} CR_ADRENO_HANDOFF_RECORD;
CR_STATIC_ASSERT(sizeof(CR_ADRENO_HANDOFF_RECORD)<=4096,"one reserved record page");
/* The native producer alone allocates/reserves this page, zeroes it, fills all
 * fields, computes Crc32 with that field zero, then publishes GPUH and IORT.
 * It can never write it again after publication. No seqlock/late EBS completion.
 * Consumer copies the whole record twice with barriers and requires exact
 * equality; validates installed IORT and locator. Windows additionally proves
 * whole page exclusion from usable RAM before mapping read-only/noncached.
 * Neither a CRC nor an ACPI record substitutes for trusted boot firmware. */
CR_STATUS CrAdrenoHandoffValidateLocator(CONST CR_ADRENO_HANDOFF_LOCATOR *);
CR_STATUS CrAdrenoHandoffValidate(CONST CR_ADRENO_HANDOFF_LOCATOR *,
 CONST CR_ADRENO_HANDOFF_RECORD *,CONST CR_ADRENO_ROUTE_FACTS *Installed);
