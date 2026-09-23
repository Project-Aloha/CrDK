/** @file
 *  Qualcomm Node Power Architecture protocol ABI used by OEM DXE drivers.
 *
 *  This is the public protocol prefix needed by Crane adapters.  The field
 *  order follows Qualcomm's EFI_NPA_PROTOCOL through the batch-request ABI so
 *  consumers can safely call the OEM NpaDxe installed by platform firmware.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Uefi.h>

#define EFI_NPA_PROTOCOL_VER_LEGACY                      0x0000000000010001ULL
#define EFI_NPA_PROTOCOL_VER_WITH_DEINIT_SUPPORT         0x0000000000010002ULL
#define EFI_NPA_PROTOCOL_VER_WITH_BATCH_REQUEST_SUPPORT  0x0000000000010003ULL

#define EFI_NPA_PROTOCOL_GUID                                                \
  { 0x79d6c870, 0x725e, 0x489e, { 0xa0, 0xa1, 0x27, 0xe7, 0xa5, 0xd0, 0xcb, 0x35 } }

extern EFI_GUID  gEfiNpaProtocolGuid;

typedef struct npa_client  *npa_client_handle;
typedef UINT32              npa_resource_state;

typedef enum {
  NPA_NO_CLIENT                   = 0x7fffffff,
  NPA_CLIENT_RESERVED1            = (1U << 0),
  NPA_CLIENT_RESERVED2            = (1U << 1),
  NPA_CLIENT_CUSTOM1              = (1U << 2),
  NPA_CLIENT_CUSTOM2              = (1U << 3),
  NPA_CLIENT_CUSTOM3              = (1U << 4),
  NPA_CLIENT_CUSTOM4              = (1U << 5),
  NPA_CLIENT_REQUIRED             = (1U << 6),
  NPA_CLIENT_ISOCHRONOUS          = (1U << 7),
  NPA_CLIENT_IMPULSE              = (1U << 8),
  NPA_CLIENT_LIMIT_MAX            = (1U << 9),
  NPA_CLIENT_VECTOR               = (1U << 10),
  NPA_CLIENT_SUPPRESSIBLE         = (1U << 11),
  NPA_CLIENT_SUPPRESSIBLE_VECTOR  = ((1U << 12) | NPA_CLIENT_VECTOR),
  NPA_CLIENT_CUSTOM5              = (1U << 13),
  NPA_CLIENT_CUSTOM6              = (1U << 14),
  NPA_CLIENT_SUPPRESSIBLE2        = (1U << 15),
  NPA_CLIENT_SUPPRESSIBLE2_VECTOR = (1U << 16),
  NPA_CLIENT_RESERVED3            = (1U << 17)
} npa_client_type;

typedef enum {
  NPA_QUERY_SUCCESS = 0,
  NPA_QUERY_UNSUPPORTED_QUERY_ID,
  NPA_QUERY_UNKNOWN_RESOURCE,
  NPA_QUERY_NULL_POINTER,
  NPA_QUERY_NO_VALUE
} npa_query_status;

typedef VOID (*npa_callback)(
  IN VOID         *Context,
  IN UINT32        EventType,
  IN VOID         *Data,
  IN UINT32        DataSize
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_INIT)(VOID);

typedef EFI_STATUS (EFIAPI *EFI_NPA_CREATE_SYNC_CLIENT_EX)(
  IN CONST CHAR8       *ResourceName,
  IN CONST CHAR8       *ClientName,
  IN npa_client_type    ClientType,
  IN UINT32             ClientValue,
  IN VOID              *ClientReference,
  OUT npa_client_handle *ClientHandle
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_COMPLETE_REQUEST)(
  IN npa_client_handle  Client
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_SCALAR_REQUEST)(
  IN npa_client_handle  Client,
  IN npa_resource_state State
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_RESOURCE_AVAILABLE_CB)(
  IN CONST CHAR8  *ResourceName,
  IN npa_callback  Callback,
  IN VOID         *Context
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_QUERY_RESOURCE_AVAILABLE)(
  IN CONST CHAR8      *ResourceName,
  OUT npa_query_status *Status
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_DESTROY_CLIENT)(
  IN npa_client_handle  Client
  );

typedef EFI_STATUS (EFIAPI *EFI_NPA_ISSUE_VECTOR_REQUEST)(
  IN npa_client_handle  Client,
  IN UINT32             ElementCount,
  IN npa_resource_state *Vector
  );

/* Unused protocol methods are pointer-sized ABI placeholders. */
typedef EFI_STATUS (EFIAPI *EFI_NPA_OPAQUE_METHOD)(VOID);

typedef struct _EFI_NPA_PROTOCOL {
  UINT64                           Revision;
  EFI_NPA_INIT                     NpaInit;
  EFI_NPA_CREATE_SYNC_CLIENT_EX    CreateSyncClientEx;
  EFI_NPA_COMPLETE_REQUEST         CompleteRequest;
  EFI_NPA_OPAQUE_METHOD            DefineNodeCb;
  EFI_NPA_SCALAR_REQUEST           ScalarRequest;
  EFI_NPA_RESOURCE_AVAILABLE_CB    ResourceAvailableCb;
  EFI_NPA_OPAQUE_METHOD            RemoteDefineResourceCb;
  EFI_NPA_OPAQUE_METHOD            RemoteResourceLocalAggregationFcn;
  EFI_NPA_OPAQUE_METHOD            RemoteResourceLocalAggregationNoInitialRequestDriverFcn;
  EFI_NPA_OPAQUE_METHOD            RemoteResourceRemoteAggregationDriverFcn;
  EFI_NPA_OPAQUE_METHOD            RemoteResourceAvailable;
  EFI_NPA_OPAQUE_METHOD            DalEventCallback;
  EFI_NPA_OPAQUE_METHOD            ResourcesAvailableCb;
  EFI_NPA_QUERY_RESOURCE_AVAILABLE QueryResourceAvailable;
  EFI_NPA_DESTROY_CLIENT           DestroyClient;
  EFI_NPA_OPAQUE_METHOD            ModifyRequiredRequest;
  EFI_NPA_OPAQUE_METHOD            IssueImpulseRequest;
  EFI_NPA_ISSUE_VECTOR_REQUEST     IssueVectorRequest;
  EFI_NPA_OPAQUE_METHOD            CreateQueryHandle;
  EFI_NPA_OPAQUE_METHOD            DestroyQueryHandle;
  EFI_NPA_OPAQUE_METHOD            Query;
  EFI_NPA_OPAQUE_METHOD            QueryByName;
  EFI_NPA_OPAQUE_METHOD            QueryByClient;
  EFI_NPA_OPAQUE_METHOD            QueryByEvent;
  EFI_NPA_OPAQUE_METHOD            CreateEventCb;
  EFI_NPA_OPAQUE_METHOD            SetEventWatermarks;
  EFI_NPA_OPAQUE_METHOD            SetEventThresholds;
  EFI_NPA_OPAQUE_METHOD            DestroyEventHandle;
  EFI_NPA_OPAQUE_METHOD            CancelRequest;
  EFI_NPA_OPAQUE_METHOD            SetClientForkPref;
  EFI_NPA_OPAQUE_METHOD            GetClientForkPref;
  EFI_NPA_OPAQUE_METHOD            JoinRequest;
  EFI_NPA_OPAQUE_METHOD            SetRequestAttribute;
  EFI_NPA_OPAQUE_METHOD            CreateAsyncClientCbEx;
  EFI_NPA_OPAQUE_METHOD            IssueIsocRequest;
  EFI_NPA_OPAQUE_METHOD            IssueLimitMaxRequest;
  EFI_NPA_OPAQUE_METHOD            AliasResourceCb;
  EFI_NPA_OPAQUE_METHOD            DefineMarker;
  EFI_NPA_OPAQUE_METHOD            DefineMarkerWithAttributes;
  EFI_NPA_OPAQUE_METHOD            AssignResourceState;
  EFI_NPA_OPAQUE_METHOD            QueryGetResource;
  EFI_NPA_OPAQUE_METHOD            EnableNode;
  EFI_NPA_OPAQUE_METHOD            DisableNode;
  EFI_NPA_OPAQUE_METHOD            ForkResource;
  EFI_NPA_OPAQUE_METHOD            ResourceLock;
  EFI_NPA_OPAQUE_METHOD            ResourceUnlock;
  EFI_NPA_OPAQUE_METHOD            RequestHasAttribute;
  EFI_NPA_OPAQUE_METHOD            GetRequestAttributes;
  EFI_NPA_OPAQUE_METHOD            PassRequestAttributes;
  EFI_NPA_OPAQUE_METHOD            IssueInternalRequest;
  EFI_NPA_OPAQUE_METHOD            ResourceAddSystemEventCallback;
  EFI_NPA_OPAQUE_METHOD            ResourceRemoveSystemEventCallback;
  EFI_NPA_OPAQUE_METHOD            NpaDeinit;
  EFI_NPA_OPAQUE_METHOD            IssueBatchRequest;
  EFI_NPA_OPAQUE_METHOD            BatchScalarRequest;
  EFI_NPA_OPAQUE_METHOD            BatchVectorRequest;
} EFI_NPA_PROTOCOL;
