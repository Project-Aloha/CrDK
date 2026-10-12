/* Absolute clock-owner interface for PEP journals and kernel consumers.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <wdm.h>
#include <Library/ClockVoteLib.h>
#define WDF_CLOCK_OWNER_INTERFACE_REVISION 2U
DEFINE_GUID(GUID_CLOCK_OWNER_INTERFACE, 0xafb5be7a, 0xf96d, 0x499c,
            0x9c, 0x51, 0x2c, 0x9e, 0x26, 0xad, 0xaa, 0x3f);

/* PASSIVE, kernel only; retain the query interface reference while using it.
   Owner is a stable nonpaged identity, never dereferenced by the provider.
   Release that owner's votes successfully before its identity/interface dies.
   Names are NUL terminated; controller IDs are ClockControllerType. Gdsc
   selects the domain inventory rather than the clock inventory. Validate is
   pure; rates use the exact Hz contract, without implicit ceiling/aggregation.
   Get copies the owner's logical vote; pending means hardware is uncertain.
   Reserve the entire owner/name policy before publication; Get/Set require
   that reservation, including after a journal Close restores absence. Slots
   survive D0 transitions and are retired by ReleaseOwner or hardware teardown.
   Set is absolute/idempotent and retains failed cleanup for retry. Requests
   fail while the provider is outside D0/removing; permanent brokers must retain
   their desired votes independently and requalify a new provider incarnation. */
typedef NTSTATUS (*CLOCK_OWNER_VALIDATE)(VOID *Context, UINT32 Controller,
    CONST CHAR8 *Name, BOOLEAN Gdsc, UINT64 RateHz);
typedef NTSTATUS (*CLOCK_OWNER_GET)(VOID *Context, VOID *Owner, UINT32 Controller,
    CONST CHAR8 *Name, BOOLEAN Gdsc, CR_CLOCK_VOTE_VALUE *, BOOLEAN *RecoveryPending);
typedef NTSTATUS (*CLOCK_OWNER_SET)(VOID *Context, VOID *Owner, UINT32 Controller,
    CONST CHAR8 *Name, BOOLEAN Gdsc, CONST CR_CLOCK_VOTE_VALUE *);
typedef NTSTATUS (*CLOCK_OWNER_RELEASE)(VOID *Context, VOID *Owner);
typedef NTSTATUS (*CLOCK_OWNER_RESERVE)(VOID *Context, VOID *Owner, UINT32 Controller,
    CONST CHAR8 *Name, BOOLEAN Gdsc, UINT64 RateHz);
typedef struct {
  INTERFACE Header;
  CLOCK_OWNER_VALIDATE Validate;
  CLOCK_OWNER_GET Get;
  CLOCK_OWNER_SET Set;
  CLOCK_OWNER_RELEASE ReleaseOwner;
  CLOCK_OWNER_RESERVE Reserve;
} WDF_CLOCK_OWNER_INTERFACE;
