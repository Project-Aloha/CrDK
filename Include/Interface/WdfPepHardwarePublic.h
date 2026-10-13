/* Actual permanent PEP hardware lifetime, independent of PoFx policy. MIT */
#pragma once
#include <wdm.h>
DEFINE_GUID(GUID_PEP_HARDWARE_CR_INTERFACE,0x59b045a9,0x7c2c,0x4831,0x99,0x8d,0x45,0xc1,0xa7,0x04,0x94,0xaa);
#define WDF_PEP_HARDWARE_INTERFACE_REVISION 1U
typedef struct {
  PVOID Provider;
  UINT64 Incarnation;
  UINT32 Slot,Serial;
} CR_PEP_HARDWARE_LEASE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID StableOwner,CR_PEP_HARDWARE_LEASE *);
  BOOLEAN (*Check)(PVOID,PVOID StableOwner,CONST CR_PEP_HARDWARE_LEASE *);
  NTSTATUS (*End)(PVOID,PVOID StableOwner,CR_PEP_HARDWARE_LEASE *);
} WDF_PEP_HARDWARE_INTERFACE;
/* Query on the unique referenced GUID_DEVINTERFACE_BUS_CR permanent PEP.
 * Begin/End and query PASSIVE; Header reference callbacks <=DISPATCH. Retain
 * that interface and exact nonpaged Owner/token through failed cleanup. Begin
 * pins the actual qualified base hardware D0/mappings, including Clock/RPMh/
 * ICB/CmdDB, until End. It does not reserve votes or prove consumer-specific
 * native handoff, pads, reset, IRQ assignment, analogue outputs or policy.
 * Check is pure, bounded, nonpaged and usable at DIRQL. Owner/token are stable
 * and unmodified until all consumer IRQ/work/IO/Check callers drain before
 * End. Copies/foreign tokens fail. This instantaneous observation does not
 * bracket an external consumer's MMIO or drain its in-flight IRQ accesses.
 * A rejected reassignment/surprise quarantine closes Check/new Begin forever
 * while retaining exact leases/maps; End remains usable after consumer drain.
 * Ordinary D0 exit/resource release refuses every live lease. No success or
 * elapsed-time inference may replace the consumer's own power/grant proof. */
