/** @file
 *  Target-data ABI for portable RPMh graphs and native Qualcomm ICB routes.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

typedef struct {
  CONST CHAR8 *Name;
  UINT16       Channels;
  UINT16       BusWidth;
  UINT16       LinkOffset;
  UINT16       LinkCount;
} InterconnectTargetNode;

typedef struct {
  CONST CHAR8 *Name;
  UINT32       EnableMask;
  BOOLEAN      KeepAlive;
  UINT16       NodeOffset;
  UINT16       NodeCount;
} InterconnectTargetBcm;

typedef struct {
  UINT32 Id;
  UINT16 NodeIndex;
} InterconnectTargetEndpoint;

typedef struct {
  CONST CHAR8 *Compatible;
  UINT16       EndpointOffset;
  UINT16       EndpointCount;
  UINT16       BcmOffset;
  UINT16       BcmCount;
} InterconnectTargetProvider;

/** Map a Crane endpoint pair to Qualcomm's native /icb/arbiter IDs. */
typedef struct {
  CONST CHAR8 *ProviderCompatible;
  UINT32       SourceId;
  UINT32       DestinationId;
  UINT32       MasterId;
  UINT32       SlaveId;
  CONST CHAR8 *ClientName;
} InterconnectTargetNativeRoute;

typedef struct {
  CONST InterconnectTargetNode     *Nodes;
  UINT16                            NodeCount;
  CONST UINT16                     *Links;
  UINT16                            LinkCount;
  CONST InterconnectTargetBcm      *Bcms;
  UINT16                            BcmCount;
  CONST UINT16                     *BcmNodes;
  UINT16                            BcmNodeCount;
  CONST InterconnectTargetEndpoint *Endpoints;
  UINT16                            EndpointCount;
  CONST InterconnectTargetProvider *Providers;
  UINT16                            ProviderCount;
  CONST UINT16                     *ProviderBcms;
  UINT16                            ProviderBcmCount;
  CONST InterconnectTargetNativeRoute *NativeRoutes;
  UINT16                            NativeRouteCount;
} InterconnectTargetContext;

InterconnectTargetContext *
CrTargetGetInterconnectContext(VOID);
