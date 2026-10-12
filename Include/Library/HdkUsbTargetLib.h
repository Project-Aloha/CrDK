/* Qualified HDK8245/8350 USB resources. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>

#define CR_HDK_USB_PORTS 2U
#define CR_HDK_USB_CLOCKS 8U
#define CR_HDK_USB_RAILS 5U
#define CR_HDK_USB_NO_GPIO 0xffffU
typedef struct { UINT32 Offset, Value, DelayUs; } CR_HDK_USB_WRITE;
typedef struct { CONST CHAR8 *Name; UINT64 RateHz; } CR_HDK_USB_CLOCK;
typedef struct { CONST CHAR8 *Name; UINT32 Millivolts; UINT8 Mode; } CR_HDK_USB_RAIL;
typedef struct {
  UINT32 Index;
  UINTN Controller, ControllerBytes, HsPhy, HsPhyBytes, Eud, QmpPhy, QmpPhyBytes;
  UINT16 StreamId;
  BOOLEAN FixedHost;
  UINT16 VbusGpio, HubGpio, OrientationGpio;
  UINT8 VbusValue, HubValue;
  CONST CHAR8 *IccProvider, *Gdsc, *CoreReset, *HsReset, *SsReset[2];
  UINT32 MemorySource, MemoryDestination, ConfigSource, ConfigDestination;
  CR_HDK_USB_CLOCK CoreClocks[CR_HDK_USB_CLOCKS];
  UINT32 CoreClockCount;
  CR_HDK_USB_CLOCK PhyClocks[CR_HDK_USB_CLOCKS];
  UINT32 PhyClockCount, HsPhyClockCount;
  CR_HDK_USB_RAIL Rails[CR_HDK_USB_RAILS];
  UINT32 RailCount, HsRailCount;
  /* HS overrides, public Android FEMTO sequence, offsets checked by consumer. */
  CONST CR_HDK_USB_WRITE *HsOverrides;
  UINT32 HsOverrideCount;
  /* Exact public board QMP sequence; no unqualified SoC substitutions. */
  CONST CR_HDK_USB_WRITE *SsSequence;
  UINT32 SsSequenceCount;
  UINT32 PcsReset, PcsStart, PcsPower, PcsStatus, PcsReadyMask;
} CR_HDK_USB_PORT;
typedef struct {
  UINT32 Soc, Board;
  CONST CR_HDK_USB_PORT *Ports;
  UINT32 PortCount;
} CR_HDK_USB_TARGET;
/* Static board-selected data; queries never touch hardware or create leases. */
CONST CR_HDK_USB_TARGET *CrHdkUsbGetTarget(VOID);
