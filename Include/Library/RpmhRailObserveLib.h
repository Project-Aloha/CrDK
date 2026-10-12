/* Retained own-DRV request evidence; no regulator programming. MIT */
#pragma once
#include <Library/rpmh.h>
#include <Library/RpmhRailLib.h>

#define CR_RPMH_RAIL_OBSERVATION_REVISION 1U
#define CR_RPMH_RAIL_CAPTURE_VOLTAGE 1U
#define CR_RPMH_RAIL_CAPTURE_ENABLE  2U
#define CR_RPMH_RAIL_CAPTURE_MODE    4U
#define CR_RPMH_RAIL_CAPTURE_ALL     7U
#define CR_RPMH_RAIL_OWN_DRV_REQUEST 1U
typedef struct _CR_RPMH_RAIL_OBSERVATION_SOURCE CR_RPMH_RAIL_OBSERVATION_SOURCE;
typedef struct _CR_RPMH_RAIL_OBSERVER CR_RPMH_RAIL_OBSERVER;
typedef struct {
  UINT32 Size,Revision;
  CONST CR_RPMH_RAIL_OBSERVER *Owner;
  CONST CR_RPMH_RAIL_OBSERVATION_SOURCE *Source;
  UINT64 Generation,Sequence;
  UINT32 Address,DrvId,CapturedMask,Provenance;
  UINT32 RawVoltage,RawEnable,RawMode,VoltageMv;
  UINT8 RpmhMode;
  BOOLEAN Enabled;
} CR_RPMH_RAIL_OBSERVATION;
struct _CR_RPMH_RAIL_OBSERVATION_SOURCE {
  CR_RPMH_RAIL_OBSERVATION_SOURCE *Self;
  RpmhDeviceContext *Core;
  UINT64 Generation;
  UINTN Mapping,MappingSize;
  CONST RpmhDrvRegisters *Registers;
  UINT32 DrvId,TcsOffset,ObserverCount;
  BOOLEAN Held,Open,Closed;
};
struct _CR_RPMH_RAIL_OBSERVER {
  CR_RPMH_RAIL_OBSERVER *Self;
  CR_RPMH_RAIL_OBSERVATION_SOURCE *Source;
  CONST CR_RPMH_RAIL *Rail;
  CONST CHAR8 *Name;
  UINT64 Generation,Sequence;
  UINT32 Address,DrvId;
  BOOLEAN Held,HasSnapshot,Closed;
  CR_STATUS LastError;
  CR_RPMH_RAIL_OBSERVATION Snapshot;
};

/* PASSIVE (or native provider's existing polling context). Serialize ALL
 * source/observer/raw/rail operations with the actual provider's outer lock.
 * The provider retains mapping/IRQ/transport access; counters here guard core
 * Deinit, not the surrounding OS provider's query reference/rundown. Source,
 * observers, core and canonical rail/name storage are stable and zero initially.
 * Generation comes from the real provider incarnation, never a copied sample.
 * Closing source stops admission immediately, retaining storage on BUSY until
 * observers close and core work drains. Closed objects cannot reopen. */
CR_STATUS CrRpmhRailObserveSourceOpen(CR_RPMH_RAIL_OBSERVATION_SOURCE *,RpmhDeviceContext *,UINT64 Generation);
CR_STATUS CrRpmhRailObserveSourceClose(CR_RPMH_RAIL_OBSERVATION_SOURCE *);
CR_STATUS CrRpmhRailObserveOpen(CR_RPMH_RAIL_OBSERVATION_SOURCE *,CR_RPMH_RAIL_OBSERVER *,CONST CR_RPMH_RAIL *);
/* Capture is actual remote IO, never a cached fallback. Output is unchanged
 * unless every field completes exactly; a failed refresh invalidates Check.
 * The triple excludes local writers only, not other firmware. Zero/unknown
 * mode remains evidence. It does not grant a BOB floor or PMIC/analog readiness. */
CR_STATUS CrRpmhRailObserveCapture(CR_RPMH_RAIL_OBSERVER *,CR_RPMH_RAIL_OBSERVATION *Out);
/* Pure latest-evidence identity check, not a fresh hardware observation. */
CR_STATUS CrRpmhRailObserveCheck(CR_RPMH_RAIL_OBSERVER *,CONST CR_RPMH_RAIL_OBSERVATION *);
CR_STATUS CrRpmhRailObserveClose(CR_RPMH_RAIL_OBSERVER *);
