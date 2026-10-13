/* Fixed absolute GPIO owners with exclusive pin fencing. MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
typedef enum {CrGpioOwnerTlmm,CrGpioOwnerPmic} CR_GPIO_OWNER_KIND;
typedef struct {UINT32 Kind,Controller,Pin;} CR_GPIO_OWNER_KEY;
typedef struct {
  UINT8 Function,Direction,Pull,Drive,Output,PowerSource,BufferType,Atest,Dtest,Enabled;
} CR_GPIO_OWNER_CONFIG;
typedef struct {BOOLEAN Present;CR_GPIO_OWNER_CONFIG Config;} CR_GPIO_OWNER_VALUE;
typedef struct {
  CR_GPIO_OWNER_KEY Key;
  CR_GPIO_OWNER_CONFIG Baseline;
  BOOLEAN Claimed,ClaimPending,Pending;
} CR_GPIO_OWNER_RESOURCE;
typedef struct {VOID *Owner;UINT32 Resource;CR_GPIO_OWNER_VALUE Value;BOOLEAN RecoveryPending,Closing;} CR_GPIO_OWNER_ENTRY;
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_CONFIG *);
  /* Fixed metadata claim/unclaim. Exclude GPIO class/foreign connections to
   * this physical pin before success; no writes, allocation or implicit D0. */
  CR_STATUS (*Claim)(VOID *,CONST CR_GPIO_OWNER_KEY *);
  CR_STATUS (*Unclaim)(VOID *,CONST CR_GPIO_OWNER_KEY *);
  CR_STATUS (*Write)(VOID *,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_CONFIG *);
} CR_GPIO_OWNER_IO;
typedef struct {
  CR_GPIO_OWNER_IO Io;
  CR_GPIO_OWNER_RESOURCE *Resources;UINT32 ResourceCount;
  CR_GPIO_OWNER_ENTRY *Entries;UINT32 Capacity;
  BOOLEAN Initialized;
} CR_GPIO_OWNER_BANK;
/* PASSIVE externally serialized, caller-owned fixed nonpaged storage. Baseline
 * is independently acquired/qualified under provider/class handoff fencing,
 * never guessed zero state. Conflicting owners fail before MMIO; release or
 * restore affects only this owner and preserves every compatible foreign
 * demand. Failed write retains exact old logical aggregate plus recovery owner
 * and the physical claim until replay succeeds. Describe/Get never repair.
 * Provider loss remains unavailable, including final unclaim. */
CR_STATUS CrGpioOwnerInit(CR_GPIO_OWNER_BANK *,CONST CR_GPIO_OWNER_IO *,CR_GPIO_OWNER_RESOURCE *,UINT32,CR_GPIO_OWNER_ENTRY *,UINT32);
CR_STATUS CrGpioOwnerValidate(CR_GPIO_OWNER_BANK *,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_CONFIG *);
CR_STATUS CrGpioOwnerReserve(CR_GPIO_OWNER_BANK *,VOID *Owner,CONST CR_GPIO_OWNER_KEY *);
CR_STATUS CrGpioOwnerGet(CR_GPIO_OWNER_BANK *,VOID *Owner,CONST CR_GPIO_OWNER_KEY *,CR_GPIO_OWNER_VALUE *,BOOLEAN *Pending);
CR_STATUS CrGpioOwnerSet(CR_GPIO_OWNER_BANK *,VOID *Owner,CONST CR_GPIO_OWNER_KEY *,CONST CR_GPIO_OWNER_VALUE *);
CR_STATUS CrGpioOwnerRelease(CR_GPIO_OWNER_BANK *,VOID *Owner);
CR_STATUS CrGpioOwnerDrain(CR_GPIO_OWNER_BANK *);
CR_STATUS CrGpioOwnerUninit(CR_GPIO_OWNER_BANK *);
