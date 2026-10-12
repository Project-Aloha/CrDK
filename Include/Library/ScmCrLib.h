/* Typed Qualcomm SCM ownership core. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/PilSmcccLib.h>
#include <oskal/cr_mutex.h>

#define CR_SCM_ROLE_PAS1 1U
#define CR_SCM_ROLE_GPU  2U
#define CR_SCM_ROLE_QTEE 3U
#define CR_SCM_ROLE_COUNT 3U
#define CR_SCM_RESUME_BUDGET 256U

typedef struct {
  VOID *Provider;
  VOID *Owner;
  UINT64 Generation;
  UINT32 Role;
} CR_SCM_LEASE;

/* Authority is supplied by the resource provider, never by a user request.
   Acquire must set Grant before its first retained effect. A failed acquisition
   can return a Grant and is released through the same exact lease. Validate
   must pin/check the current resources, including PAS metadata referred to by
   Frame, or GPU power/clock/domain identity and acknowledged CP retirement.
   GpuBank is the actual qualified domain bank, not a requested/default number.
   Callbacks run under the PASSIVE provider mutex and must not reenter this core
   or acquire outer PIL/PEP locks. Acquire any such admission before entering
   this core and pass an already pinned authority owner; Validate is a leaf.
   Release returns exact success only after all authority-owned resources are
   safely retired; failed/nonfinal returns keep the Grant and provider resident. */
typedef struct {
  VOID *Context;
  CR_STATUS (*Acquire)(VOID *, UINT32 Role, VOID *Owner, VOID **Grant, UINT32 *GpuBank);
  CR_STATUS (*Validate)(VOID *, UINT32 Role, VOID *Owner, VOID *Grant,
                        CONST CR_PIL_SMC_ARGS *Frame, UINT32 GpuBank);
  CR_STATUS (*Release)(VOID *, UINT32 Role, VOID *Owner, VOID *Grant);
} CR_SCM_AUTHORITY;

typedef struct {
  CR_SCM_LEASE Lease;
  CONST CR_SCM_LEASE *Identity;
  VOID *Grant;
  UINT32 GpuBank;
  BOOLEAN Acquired;
} CR_SCM_OWNER;

typedef struct {
  CR_PIL_SMC_ARGS Original;
  UINT64 Cookie;
  UINT32 Step, Base;
  BOOLEAN Pending, Ready;
} CR_SCM_GPU_STATE;

/* Trusted permanent-host wiring. Describe derives its extent from the exact
   retained allocation Grant; no caller-supplied physical-address admission. */
typedef struct { UINT64 Physical; UINT32 Bytes, Profile; } CR_SCM_QTEE_MEMORY;
typedef struct {
  VOID *Context;
  CR_STATUS (*Describe)(VOID *, VOID *Owner, VOID *Grant, CR_SCM_QTEE_MEMORY *);
} CR_SCM_QTEE_AUTHORITY;
typedef enum { CrScmQteeEnable, CrScmQteeCreate, CrScmQteeInvoke,
  CrScmQteeDelete } CR_SCM_QTEE_OPERATION;
typedef struct {
  CR_SCM_GPU_STATE Wire;
  CR_SCM_QTEE_MEMORY Memory;
  UINT64 BridgeHandle;
  BOOLEAN Enabled, MaybeOwned, Created, CapabilityRejected;
} CR_SCM_QTEE_STATE;

typedef struct {
  CR_MUTEX Lock;
  CR_SCM_AUTHORITY Authority;
  CR_SCM_OWNER Owners[CR_SCM_ROLE_COUNT];
  CR_PIL_TARGET PasTarget;
  CR_PIL_SMCCC Pas;
  CR_SCM_GPU_STATE Gpu;
  CR_SCM_QTEE_STATE Qtee;
  CR_SCM_QTEE_AUTHORITY QteeAuthority;
  UINT64 NextGeneration;
  CR_PIL_SMC_ARGS UncertainResult, UncertainOriginal;
  UINT32 UncertainRole;
  BOOLEAN Initialized, PasMaybeOwned, Quarantined;
} CR_SCM_CORE;

typedef struct {
  CR_PIL_SMC_ARGS Original;
  UINT64 ResumeCookie, PasSupportResult;
  UINT32 ProbeStep, SmcBase;
  BOOLEAN Interrupted, Qualified, PasSupportKnown, MaybeOwned, Quarantined;
  CR_PIL_SMC_ARGS UncertainResult, UncertainOriginal;
} CR_SCM_PAS_STATE;

/* Core storage is nonpaged, zero-initialized once, never copied, and remains
   alive with provider code/authority after any lease, partial acquire or pending
   operation. Initialize/Uninitialize require externally serialized lifecycle.
   Uninitialize preserves the generation counter. Do not memset/reuse the same
   address to reset generations. All other operations internally serialize.
   One instance may be initialized per image. This core is not published by
   any current device and issues no startup SMC. No second image may publish
   a conduit; deployment must migrate all existing direct callers atomically. */
CR_STATUS CrScmInitialize(CR_SCM_CORE *, CONST CR_SCM_AUTHORITY *);
CR_STATUS CrScmUninitialize(CR_SCM_CORE *);
/* Source-loss fence: closes admission and drains the same mutex used by both
   roles. Retains every grant/frame; there is no rearm after lost authority. */
CR_STATUS CrScmQuarantine(CR_SCM_CORE *);
CR_STATUS CrScmAcquire(CR_SCM_CORE *, UINT32 Role, VOID *Owner, CR_SCM_LEASE *);
CR_STATUS CrScmRelease(CR_SCM_CORE *, CR_SCM_LEASE *);
CR_STATUS CrScmPasProbe(CR_SCM_CORE *, CONST CR_SCM_LEASE *);
/* Compatibility boundary for existing Pil.c: exact same admitted PAS frames;
   no public IOCTL and no general SMC descriptor/owner/function passthrough. */
CR_STATUS CrScmPasCall(CR_SCM_CORE *, CONST CR_SCM_LEASE *, CR_PIL_SMC_ARGS *);
CR_STATUS CrScmPasSnapshot(CR_SCM_CORE *, CONST CR_SCM_LEASE *, CR_SCM_PAS_STATE *);
/* Performs/probes only CP aperture, never LPAC, PAS13 or secure-memory assign.
   Reinvoke with the exact retained lease to continue a bounded interrupted
   operation. CR_TIMEOUT retains the frame/Grant; another role is excluded.
   A returned busy code permits a later caller retry; it is never a success.
   Unexpected WAITQ/RPC positive replies quarantine the entire route because
   this first provider has no qualified SCM IRQ/waitqueue service. */
CR_STATUS CrScmGpuAperture(CR_SCM_CORE *, CONST CR_SCM_LEASE *);

/* Configure once per externally serialized host attach, before any QTEE lease.
   Describe runs under the core lock and may not reacquire provider locks. */
CR_STATUS CrScmConfigureQtee(CR_SCM_CORE *, CONST CR_SCM_QTEE_AUTHORITY *);
/* The lease owns the WHOLE chain, even between calls. All frames/addresses and
   returned bridge handles are constructed/stored in this sole provider. Out
   preserves the actual raw result; it is never an arbitrary input descriptor. */
CR_STATUS CrScmQteeCall(CR_SCM_CORE *, CONST CR_SCM_LEASE *,
                       CR_SCM_QTEE_OPERATION, CR_PIL_SMC_ARGS *Out);

/* Snapshot of terminal pre-operation capability refusal. It is not a release
   or memory grant. Only exact retained lease storage may obtain this state. */
typedef struct {BOOLEAN CapabilityRejected,Pending,MaybeOwned,Quarantined;} CR_SCM_QTEE_SNAPSHOT;
CR_STATUS CrScmQteeSnapshot(CR_SCM_CORE *,CONST CR_SCM_LEASE *,CR_SCM_QTEE_SNAPSHOT *);
