/* Exact AML GPIO tuple qualification; no implicit field/alias conversion. MIT */
#pragma once
#include <Library/PepJournalLib.h>
#include <Library/GpioOwnerLib.h>
typedef struct {
  CR_PEP_RESOURCE_KIND Kind;
  CR_PEP_TEXT Command; /* Exact PMIC ioctl name; empty for TLMM. */
  UINT64 Arguments[8];UINT32 ArgumentCount;
  CR_GPIO_OWNER_CONFIG Config;
} CR_PEP_GPIO_PROFILE;
typedef struct {CR_GPIO_OWNER_KEY Key;CONST CR_PEP_GPIO_PROFILE *Profiles;UINT32 ProfileCount;} CR_PEP_GPIO_BINDING;
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_CONFIG *);
  CR_STATUS (*Get)(VOID *,VOID *,CONST CR_GPIO_OWNER_KEY *,CR_GPIO_OWNER_VALUE *,BOOLEAN *);
  CR_STATUS (*Set)(VOID *,VOID *,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_VALUE *);
} CR_PEP_GPIO_IO;
typedef struct {CR_PEP_GPIO_IO Io;CONST CR_PEP_GPIO_BINDING *Bindings;UINT32 Count;BOOLEAN Initialized;} CR_PEP_GPIO_PROVIDER;
/* Immutable board-qualified full tuples, including every V2/extra field. The
 * table explicitly maps AML pin/controller identifiers and enum units to real
 * typed GPIO configuration; unsupported tuple/name cannot silently succeed.
 * No allocation/claim/write during Init/Describe/Resolve. */
CR_STATUS CrPepGpioProviderInit(CR_PEP_GPIO_PROVIDER *,CONST CR_PEP_GPIO_IO *,CONST CR_PEP_GPIO_BINDING *,UINT32);
CR_STATUS CrPepGpioProviderDescribe(CR_PEP_GPIO_PROVIDER *,CONST CR_PEP_RESOURCE_OP *,CONST CR_PEP_GPIO_BINDING **,CR_GPIO_OWNER_CONFIG *);
CR_STATUS CrPepGpioProviderGetJournal(CR_PEP_GPIO_PROVIDER *,CR_PEP_JOURNAL_PROVIDER *);
CR_STATUS CrPepGpioProviderUninit(CR_PEP_GPIO_PROVIDER *);
