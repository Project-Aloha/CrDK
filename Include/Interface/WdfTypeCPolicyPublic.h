/* Sole PEP PMIC Type-C observation, not a physical writer grant. GPL-2.0-only */
#pragma once
#include <ntddk.h>
#include <ucsi/TypeC.h>
#define WDF_TYPEC_POLICY_INTERFACE_REVISION 1U
DEFINE_GUID(GUID_TYPEC_POLICY_CR_INTERFACE,0x113b0142,0x944f,0x4fa9,0xb5,0xd7,0xbc,0xd4,0x1a,0x74,0x84,0x01);
typedef struct {
  INTERFACE Header;
  NTSTATUS (*GetState)(PVOID,CR_USB_TYPEC_STATE *,UINT64 *RouteGeneration);
} WDF_TYPEC_POLICY_INTERFACE;
/* Discover the permanent PEP BUS target and query this interface. Query commits
 * one reference. PASSIVE GetState rechecks actual wire/OS-client health and
 * leaves outputs unchanged on failure. Generation excludes HPD/battery-only
 * events. No reset/exclusive PHY/controller/DMA or independent D0 right follows
 * from a successful observation; those leases remain separately required. */
