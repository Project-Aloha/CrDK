/* Qualified ARM64 PAS transport, separated from PAS lifecycle ownership. MIT */
#pragma once
#include <Library/PILCrLib.h>
typedef struct {
  CONST CR_PIL_TARGET *Target;
  CR_PIL_SMC_ARGS Original;
  UINT64 ResumeCookie,PasSupportResult;
  UINT32 ProbeStep,SmcBase;
  BOOLEAN Initialized, Interrupted, Qualified, PasSupportKnown;
} CR_PIL_SMCCC;
/* Windows ARM64 only: this executes SMC #0 from EL1, without an invented HAL
   export. Caller prequalifies exclusive PAS1 firmware ownership and the board's
   SMC conduit, then probes the actual caller EL and SCM/PAS services at runtime.
   Caller serializes the COMPLETE core
   start/stop under a PASSIVE mutex. Target/context are nonpaged and retained
   with metadata/proxy votes until secure cleanup succeeds. Unsupported SIP
   replies remain raw wire errors for Pil.c; instruction execution is never
   evidence of PAS success. Native UEFI continues using ArmSmcLib.
   Initialize requires a zeroed context and refuses a live instance.
   Only the exact PIL information/PAS frames and matching opaque interrupted
   continuation cookies are accepted. After a bounded core resume timeout,
   a later shutdown first drains the retained continuation (bounded256) before
   issuing a new PAS shutdown, preserving all prerequisites on failure. */
CR_STATUS CrPilSmcccInitialize(CR_PIL_SMCCC *,CONST CR_PIL_TARGET *);
CR_STATUS CrPilSmcccCall(VOID *Context,CR_PIL_SMC_ARGS *);
/* Nonmutating runtime qualification, following Linux qcom_scm convention and
   PAS-supported discovery. Checks actual CurrentEL==1, tries ARM64 before32,
   verifies init/memory/auth/shutdown/supported service availability and records
   the completed PAS1-support diagnostic. PAS_SUPPORTED boolean0 is not a
   readiness veto: Linux remoteproc startup does not require that boolean and
   HDK8450's native signed PAS startup succeeds despite0. Actual signed PAS
   startup still determines readiness. Failed transport never qualifies a route.
   Interrupted probes retain exact Original/cookie/step; retry Probe with the
   same context and all leases held. Bounded256 calls per pending frame. The
   caller must not start PAS or discard the context while a probe is pending.
   This cannot certify future firmware calls; their results remain mandatory.
   SMC exception handling or a future hypervisor-policy promise is not claimed. */
CR_STATUS CrPilSmcccProbe(CR_PIL_SMCCC *);
CR_STATUS CrPilSmcccUninitialize(CR_PIL_SMCCC *);
/* AAPCS/Windows ARM64 ABI: x0 points to64 bytes; returns exact x0..x7 in that
   frame, zeroes unused x8..x17 on entry to firmware, preserves x19..x30/SP
   and leaves x18 untouched. No exported IOCTL. */
VOID CrPilArm64Smccc(CR_PIL_SMC_ARGS *);
UINT32 CrPilArm64CurrentEl(VOID);
