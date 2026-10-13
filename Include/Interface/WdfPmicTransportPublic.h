/* Permanent PEP GLink/QRTR transport; UcmUcsiCx owns UCSI policy. GPL-2.0-only */
#pragma once
#include <wdm.h>
#define WDF_PMIC_TRANSPORT_INTERFACE_REVISION 3U
#define CR_PMIC_TRANSPORT_MESSAGE_SIZE 1024U
DEFINE_GUID(GUID_PMIC_TRANSPORT_CR_INTERFACE,0x8cb9a9f4,0x5a52,0x4bf8,0x9d,0x71,0xd2,0x49,0x3f,0x48,0xce,0x86);
typedef enum { CrPmicTransportUcsi=1, CrPmicTransportBattery=2 } CR_PMIC_TRANSPORT_ROLE;
typedef struct {PVOID Provider;UINT64 Token;} CR_PMIC_TRANSPORT_LEASE;
typedef VOID (*CR_PMIC_TRANSPORT_WAKE)(PVOID);
typedef struct {
 UINT32 PeerGeneration,AdspGeneration;
 BOOLEAN Online,Faulted,More,Quarantined;
 NTSTATUS LastError;
} CR_PMIC_TRANSPORT_STATE;
typedef struct {
 INTERFACE Header;
 NTSTATUS (*Begin)(PVOID,PVOID Owner,CR_PMIC_TRANSPORT_WAKE,PVOID WakeContext,CR_PMIC_TRANSPORT_LEASE *);
 NTSTATUS (*End)(PVOID,PVOID Owner,CR_PMIC_TRANSPORT_LEASE *,BOOLEAN Quiesced);
 NTSTATUS (*Pump)(PVOID,PVOID Owner,CONST CR_PMIC_TRANSPORT_LEASE *,CR_PMIC_TRANSPORT_STATE *);
 NTSTATUS (*Send)(PVOID,PVOID Owner,CONST CR_PMIC_TRANSPORT_LEASE *,CONST VOID *,UINT32 Length);
 NTSTATUS (*Receive)(PVOID,PVOID Owner,CONST CR_PMIC_TRANSPORT_LEASE *,VOID *,UINT32 *Length);
 NTSTATUS (*GetState)(PVOID,PVOID Owner,CONST CR_PMIC_TRANSPORT_LEASE *,CR_PMIC_TRANSPORT_STATE *);
 NTSTATUS (*GetDependencies)(PVOID,PVOID Owner,CONST CR_PMIC_TRANSPORT_LEASE *,PDEVICE_OBJECT Out[2]);
 NTSTATUS (*BeginForOwner)(PVOID,PVOID Owner,CR_PMIC_TRANSPORT_ROLE,CR_PMIC_TRANSPORT_WAKE,PVOID WakeContext,CR_PMIC_TRANSPORT_LEASE *);
} WDF_PMIC_TRANSPORT_INTERFACE;
/* Discover the permanent BUS alias and query this referenced interface. Two
 * fixed policy subscriptions (UCSI32779/32780 and battery32778) share one raw
 * backend/IRQ lease. BeginForOwner selects the qualified subscription; the
 * legacy Begin slot is reserved and returns NOT_SUPPORTED. Each subscription
 * has an exact owner, immutable lease address, independent reply queue and
 * Wake rundown. A foreign owner cannot send/consume the other subscription. Methods
 * run PASSIVE; Wake runs <=DISPATCH and only schedules work (no interface calls
 * or waits). End synchronously drains Wake before caller frees its context.
 * A failed Begin may retain Lease.Provider: call exact End until it succeeds.
 * Quiesced means the consumer has drained its transaction with a matching
 * reply; cancellation/timeout/removal must use FALSE. Unquiesced End poisons
 * only that policy subscription for the actual ADSP generation until a genuinely
 * new ADSP instance is observed. Other qualified subscriptions remain usable.
 * Battery sends are source-qualified GET-only; charging controls are rejected.
 * This provider issues no UCSI RESET/ACK/policy commands. Pump services RX only
 * following IPCC work; timers may advance TX/PDR and bounded deadlines.
 * Interface refs and retained leases pin actual prerequisites. Quarantine
 * closes hardware access permanently, permits exact End/ref drain and retains
 * hardware/dependency storage until reboot. GetDependencies runs PASSIVE with
 * the exact live owner/lease; success returns independently referenced actual
 * ADSP Subsystem (ADS0) then SMEM PDOs, failure leaves Out unchanged. The child reports these
 * plus the permanent PEP PDO in PowerRelations before using the wire. It drops
 * the returned references after validation/relationship storage. These refs
 * describe dependency identity; the retained transport lease owns hardware
 * readiness. No PEP-to-ADS0 power edge is added (ADS0 depends on PIL, then PEP).
 * D0 ordering does not imply ADSP READY; Begin may require a paced retry.
 * No _CRS/metadata implies Online. */
