/* SM8450 processor thermal locations, not PPM performance counters. MIT */
#pragma once
#include <Interface/WdfTsensPublic.h>
#define WDF_TSENS_CPU_CR_INTERFACE_REVISION 1U
#define CR_TSENS_CPU_MAX_SENSORS 3U
DEFINE_GUID(GUID_TSENS_CPU_CR_INTERFACE,0xa02e2696,0xe3bf,0x4549,0xb5,0x21,0x13,0x47,0x68,0xd3,0x01,0x45);
typedef struct {
  UINT64 Affinity;
  UINT32 Bank,Count;
  UINT32 Sensor[CR_TSENS_CPU_MAX_SENSORS];
  CR_TSENS_SAMPLE Sample[CR_TSENS_CPU_MAX_SENSORS];
  INT32 MaximumMilliCelsius;
} CR_TSENS_CPU_SAMPLE;
typedef struct {
  INTERFACE Header;
  NTSTATUS (*Begin)(PVOID,PVOID StableOwner,CR_TSENS_LEASE *);
  NTSTATUS (*End)(PVOID,PVOID StableOwner,CR_TSENS_LEASE *);
  NTSTATUS (*ReadCpu)(PVOID,PVOID StableOwner,CONST CR_TSENS_LEASE *,
                      UINT64 ProcessorAffinity,CR_TSENS_CPU_SAMPLE *);
} WDF_TSENS_CPU_CR_INTERFACE;
/* Discover GUID_DEVINTERFACE_TSENS_CR then query this exact referenced ABI.
 * Begin/End PASSIVE; ReadCpu <=DISPATCH, bounded MMIO under the same lease.
 * ProcessorAffinity is the exact CPU DT reg/MADT affinity value 0,0x100,...,
 * 0x700, not a Windows logical processor number or an unmasked MPIDR_EL1.
 * CPU0..3 use bank1 sensors1..4; CPU4/5/6 use bank0 sensor pairs5/6,7/8,9/10;
 * CPU7 uses bank0 sensors11/12/13. Every source-qualified sample must succeed
 * before publishing the group and its signed maximum. Reads occur in order;
 * hardware does not provide a simultaneous multi-sensor latch. Failure leaves
 * output unchanged. No temperature-to-frequency, trip/IRQ, _TMP, PPM feedback
 * counter or scheduling contract is established by this readable interface. */
