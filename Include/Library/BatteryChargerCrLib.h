/** Source-qualified SM8450 PMIC GLINK battery properties.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_BATTERY_OWNER 32778U
#define CR_BATTERY_SNAPSHOT_REVISION 1U
#define CR_BATTERY_VALID_PRESENT (1U << 0)
#define CR_BATTERY_VALID_STATUS (1U << 1)
#define CR_BATTERY_VALID_SOC (1U << 2)
#define CR_BATTERY_VALID_VOLTAGE (1U << 3)
#define CR_BATTERY_VALID_CURRENT (1U << 4)
#define CR_BATTERY_VALID_TEMPERATURE (1U << 5)
#define CR_BATTERY_VALID_USB_ONLINE (1U << 6)
#define CR_BATTERY_PROPERTY_COUNT 7U
#define CR_BATTERY_VALID_ALL ((1U << CR_BATTERY_PROPERTY_COUNT) - 1U)

typedef enum {
  CrBatteryUnknown = 0, CrBatteryCharging = 1, CrBatteryDischarging = 2,
  CrBatteryNotCharging = 3, CrBatteryFull = 4
} CR_BATTERY_STATUS;

/* SOC is hundredths of a percent (0..10000), never an assumed scale.
 * Android BATT_TEMP is signed hundredths C; normalized here to milli C.
 * SampleUs order matches the validity bits. Invalid/stale fields are not UI
 * measurements even when the diagnostic last value remains in this copy. */
typedef struct {
  UINT32 Size, Revision, Generation, Sequence, ValidMask;
  BOOLEAN PeerReady, Pending, Faulted, Present, UsbOnline;
  UINT8 Status;
  UINT16 SocHundredthsPercent;
  UINT32 VoltageUv;
  INT32 CurrentUa, TemperatureMilliC;
  UINT64 SampleUs[CR_BATTERY_PROPERTY_COUNT];
  CR_STATUS LastError;
  UINT32 Timeouts, Malformed;
} CR_BATTERY_CHARGER_SNAPSHOT;

typedef CR_STATUS (*CR_BATTERY_SEND)(VOID *Context, CONST VOID *Data, UINTN Length);

#define CR_BATTERY_STAT_CYCLE_COUNT (1U << 0)
#define CR_BATTERY_STAT_TIME_TO_EMPTY (1U << 1)
/* Optional legacy properties BATT_CYCLE_COUNT15 and BATT_TTE_AVG20. The
 * latter is the remote's estimate at its measured load, in whole seconds. */
typedef struct {
  UINT32 ValidMask, CycleCount, TimeToEmptySeconds;
} CR_BATTERY_STATISTICS;

typedef struct {
  CR_BATTERY_SEND Send;
  VOID *Context;
  CR_BATTERY_CHARGER_SNAPSHOT State;
  UINT64 DeadlineUs, NextCycleUs;
  UINT32 PendingIndex, NextIndex, AwaitLateMask, RefreshMask;
  UINT32 AdspGeneration, PeerGeneration;
  BOOLEAN Initialized, Sending;
  /* Optional Windows metadata request shares the exact untagged scheduler.
   * Native charger default remains seven fields; Snapshot ABI is unchanged. */
  BOOLEAN MetadataEnabled, TechnologyValid;
  UINT32 Technology; UINT64 TechnologySampleUs;
  BOOLEAN StatisticsEnabled;
  CR_BATTERY_STATISTICS Statistics;
  UINT64 StatisticsSampleUs[2];
} CR_BATTERY_CHARGER;

/* Caller serializes all entry points; receive never sends or allocates.
 * A timed-out property is fenced until its exact late reply or generation
 * boot; there is no on-wire transaction ID with which to match a new retry.
 * Caller supplies the provider's per-instance monotonically increasing ADSP
 * identity. Zero/lower identities are unavailable and retain the old fence.
 * Offline/PDR invalidates samples but retains accepted property identities;
 * only their original replies or a qualified newer ADSP boot retire them. */
CR_STATUS CrBatteryChargerInit(CR_BATTERY_CHARGER *, CR_BATTERY_SEND, VOID *);
VOID CrBatteryChargerOnline(CR_BATTERY_CHARGER *, BOOLEAN Online,
                           UINT32 AdspGeneration, UINT32 PeerGeneration);
CR_STATUS CrBatteryChargerTick(CR_BATTERY_CHARGER *, UINT64 NowUs);
VOID CrBatteryChargerReceive(CR_BATTERY_CHARGER *, CONST VOID *, UINTN Length, UINT64 NowUs);
CR_STATUS CrBatteryChargerSnapshot(CONST CR_BATTERY_CHARGER *, UINT64 NowUs,
                                   CR_BATTERY_CHARGER_SNAPSHOT *);
VOID CrBatteryChargerRefresh(CR_BATTERY_CHARGER *);

/* Android BATT_TECHNOLOGY13 returns Linux power_supply technology0..6.
 * Unknown0 is retained as unknown; callers must not infer rechargeability. */
VOID CrBatteryChargerEnableMetadata(CR_BATTERY_CHARGER *);
CR_STATUS CrBatteryChargerTechnology(CONST CR_BATTERY_CHARGER *, UINT64 NowUs, UINT32 *Technology);

/* Opt-in also enables technology. Unsupported/error replies invalidate only
 * their optional field; mandatory sampling continues. The snapshot ABI and
 * native charger's default seven-property selection are unchanged. */
VOID CrBatteryChargerEnableStatistics(CR_BATTERY_CHARGER *);
CR_STATUS CrBatteryChargerStatistics(CONST CR_BATTERY_CHARGER *, UINT64 NowUs,
                                     CR_BATTERY_STATISTICS *);
