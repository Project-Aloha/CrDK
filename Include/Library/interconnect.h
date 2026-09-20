/** @file
 *  OS-independent Qualcomm RPMh interconnect voter.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetInterconnectLib.h>
#include <Library/rpmh.h>
#include <oskal/cr_lock.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define INTERCONNECT_MAX_NODES          256
#define INTERCONNECT_MAX_BCMS           64
#define INTERCONNECT_MAX_PATHS          32
#define INTERCONNECT_MAX_RPMH_COMMANDS  16
#define INTERCONNECT_INVALID_PATH       0

typedef UINT32 INTERCONNECT_PATH_HANDLE;

typedef CR_STATUS (*INTERCONNECT_CMD_DB_GET_ADDRESS)(
    IN VOID *Context, IN CONST CHAR8 *Name, OUT UINT32 *Address);

typedef CR_STATUS (*INTERCONNECT_CMD_DB_GET_AUX_DATA)(
    IN VOID *Context, IN CONST CHAR8 *Name, OUT UINT8 *Data,
    IN OUT UINT32 *Length);

typedef CR_STATUS (*INTERCONNECT_RPMH_WRITE)(
    IN VOID *Context, IN RpmhTcsCmd *Commands, IN UINT32 CommandCount);

typedef struct {
  INTERCONNECT_CMD_DB_GET_ADDRESS  GetAddress;
  INTERCONNECT_CMD_DB_GET_AUX_DATA GetAuxData;
  INTERCONNECT_RPMH_WRITE           WriteRpmh;
  VOID                            *CmdDbContext;
  VOID                            *RpmhContext;
  UINT16                           MaxRpmhCommands;
} InterconnectIoOps;

typedef struct {
  UINT32  Address;
  UINT32  Unit;
  UINT16  Width;
  UINT8   Vcd;
  UINT64  VoteX;
  UINT64  VoteY;
  UINT64  ProgrammedX;
  UINT64  ProgrammedY;
  BOOLEAN Programmed;
  BOOLEAN Dirty;
} InterconnectBcmRuntime;

typedef struct {
  UINT64  NodeMask[INTERCONNECT_MAX_NODES / 64];
  UINT64  AverageBandwidth;
  UINT64  PeakBandwidth;
  UINT16  ProviderIndex;
  UINT32  SourceId;
  UINT32  DestinationId;
  BOOLEAN InUse;
} InterconnectPathRuntime;

typedef struct {
  CONST InterconnectTargetContext *Target;
  InterconnectIoOps                Io;
  UINT64                           NodeAverage[INTERCONNECT_MAX_NODES];
  UINT64                           NodePeak[INTERCONNECT_MAX_NODES];
  InterconnectBcmRuntime           Bcm[INTERCONNECT_MAX_BCMS];
  InterconnectPathRuntime          Paths[INTERCONNECT_MAX_PATHS];
  CR_LOCK                          Lock;
  BOOLEAN                          Initialized;
  BOOLEAN                          DeinitPending;
} InterconnectDeviceContext;

/** Initialize target topology, CmdDB metadata, and keepalive votes. */
CR_STATUS
InterconnectLibInit(
    IN OUT InterconnectDeviceContext **Context,
    IN CONST InterconnectIoOps *Io);

/** Release all path votes and make the context available for reinitialization. */
CR_STATUS
InterconnectLibDeinit(IN OUT InterconnectDeviceContext *Context);

/** Acquire a reusable path between two provider-local endpoint IDs. */
CR_STATUS
InterconnectAcquirePath(
    IN InterconnectDeviceContext *Context, IN CONST CHAR8 *ProviderCompatible,
    IN UINT32 SourceId, IN UINT32 DestinationId,
    OUT INTERCONNECT_PATH_HANDLE *Path);

/** Set bytes/second average and peak requirements for a path. */
CR_STATUS
InterconnectSetBandwidth(
    IN InterconnectDeviceContext *Context,
    IN INTERCONNECT_PATH_HANDLE Path, IN UINT64 AverageBandwidth,
    IN UINT64 PeakBandwidth);

/** Remove a path vote and release its handle. */
CR_STATUS
InterconnectReleasePath(
    IN InterconnectDeviceContext *Context,
    IN INTERCONNECT_PATH_HANDLE Path);

/** Return the number of generated providers available to consumers. */
UINT16
InterconnectGetProviderCount(IN InterconnectDeviceContext *Context);
