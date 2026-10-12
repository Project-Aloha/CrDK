/* Fixed LPASS PRM policy on the existing audio subscription. GPL-2.0-only */
#pragma once
#include <Interface/WdfAudioGprPublic.h>
#include <Library/AudioPrmCrLib.h>
#define WDF_AUDIO_POWER_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_AUDIO_POWER_CR_INTERFACE,0x96ad02e8,0x8a9e,0x4da9,0xa5,0x97,0x3d,0x5b,0x25,0x6a,0x80,0xc2);
typedef struct {
 UINT32 Desired;
 CR_AUDIO_PRM_VOTE Votes[CR_AUDIO_PRM_VOTES];
 BOOLEAN Bound,SourceLost,Retained;
} CR_AUDIO_POWER_INFORMATION;
typedef struct { PVOID Provider;UINT64 Token; } CR_AUDIO_HARDWARE_POWER_LEASE;
typedef struct { PVOID Self,Provider;UINT64 Token;volatile LONG Active; } CR_AUDIO_HARDWARE_POWER_GUARD;
typedef struct {
 INTERFACE Header;
 NTSTATUS (*SetDesired)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,UINT32 Mask);
 NTSTATUS (*GetState)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CR_AUDIO_POWER_INFORMATION *);
 NTSTATUS (*BeginHardware)(PVOID,PVOID Owner,CONST CR_AUDIO_GPR_LEASE *,CR_AUDIO_HARDWARE_POWER_LEASE *);
 BOOLEAN (*EnterHardware)(PVOID,PVOID Owner,CONST CR_AUDIO_HARDWARE_POWER_LEASE *,CR_AUDIO_HARDWARE_POWER_GUARD *);
 VOID (*LeaveHardware)(PVOID,CR_AUDIO_HARDWARE_POWER_GUARD *);
 NTSTATUS (*EndHardware)(PVOID,PVOID Owner,CR_AUDIO_HARDWARE_POWER_LEASE *);
} WDF_AUDIO_POWER_INTERFACE;
/* PASSIVE, externally serialized. This companion interface uses the SAME
 * exact-address Begin lease as AudioGpr, never a second named GLINK owner.
 * The permanent provider owns all PRM tokens, packets and uncertain votes.
 * SetDesired never means granted: AudioGpr.Pump advances bounded async work.
 * GetState is diagnostic and not a transferable MMIO grant. Only the actual
 * permanent hardware owner may consume the internal qualified PRM journal.
 * On End/source loss local bytes may drain while unknown remote votes remain.
 * No user raw GPR operation, imported READY flag or local StopTicket can retire
 * these effects. Retained effects prevent a fresh power session until actual
 * old-instance retirement is separately proven. Existing SPF v1 stays intact. */

/* Hardware power lease pins all five actual acknowledged PRM votes. Begin/End
 * are PASSIVE; Enter/Leave are bounded nonpaged through DIRQL. Every register access must be bracketed by
 * its own zero-initialized stable guard; no copy or mismatched Leave is legal.
 * Provider source loss atomically closes new Enter; local End cannot finish
 * until admitted accesses leave. Before EndHardware, drain every actual IRQ,
 * worker and future Enter caller. Failure retains exact lease/native storage.
 * This grant is necessary but not sufficient: actual translated MMIO, rail,
 * reset and PEP hardware lifetime owners remain independently required. */
