/* Portable consumer of the Kodiak LPMD resource-program schema.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

typedef struct { CONST CHAR8 *Data; UINTN Length; } CR_PEP_TEXT;
typedef enum { CrPepInteger, CrPepString, CrPepPackage, CrPepBuffer } CR_PEP_VALUE_TYPE;
typedef struct _CR_PEP_VALUE CR_PEP_VALUE;
struct _CR_PEP_VALUE {
  CR_PEP_VALUE_TYPE Type;
  union {
    UINT64 Integer;
    CR_PEP_TEXT String; /* Length excludes any NUL terminator. */
    struct { CONST UINT8 *Data; UINTN Length; } Buffer;
    struct { CONST CR_PEP_VALUE *Values; UINTN Count; } Package;
  };
};
typedef enum {
  CrPepClock, CrPepFootswitch, CrPepVreg, CrPepNpa, CrPepBusArb,
  CrPepTlmmGpio, CrPepTlmmGpioV2, CrPepPmicGpio, CrPepDelay
} CR_PEP_RESOURCE_KIND;
typedef struct {
  CR_PEP_RESOURCE_KIND Kind;
  CR_PEP_TEXT Name, SecondaryName;
  /* Optional Kodiak display PMIC package qualifiers; opaque and preserved.
     Hardware providers must explicitly qualify both, never discard them. */
  CR_PEP_TEXT DriverName, VoteType;
  UINT64 Arguments[8];
  UINT32 ArgumentCount;
} CR_PEP_RESOURCE_OP;
typedef struct { CONST CR_PEP_VALUE *Root; } CR_PEP_POLICY;
typedef enum { CrPepDState, CrPepFState, CrPepPState } CR_PEP_STATE_KIND;
typedef struct {
  CR_PEP_STATE_KIND Kind;
  UINT32 Component, State;
} CR_PEP_TRANSITION;
typedef struct {
  CR_PEP_TEXT Id;
  UINT32 DStateMask, ComponentMask, AbandonState;
  BOOLEAN HasAbandonState;
} CR_PEP_DEVICE_INFO;
typedef struct { UINT64 FStateMask, PStateMask; } CR_PEP_COMPONENT_INFO;

/* The caller owns a decoded, immutable tree for the whole policy lifetime,
   serializes transactions per resource owner, and supplies passive-context
   providers. Validate must perform no writes; failed Begin leaves the owner
   unchanged. Begin/Apply/Commit journal every
   successful change; Rollback restores that journal, including shared votes.
   Failed rollback retains the provider owner for retry; never free it merely
   because the first operation failed. Integer units/opcodes are preserved from
   AML and interpreted by the resource provider, not guessed by this parser. */
typedef struct {
  VOID *Context;
  CR_STATUS (*Validate)(VOID *, VOID *Owner, CONST CR_PEP_RESOURCE_OP *);
  CR_STATUS (*Begin)(VOID *, VOID *Owner);
  CR_STATUS (*Apply)(VOID *, VOID *Owner, CONST CR_PEP_RESOURCE_OP *);
  CR_STATUS (*Commit)(VOID *, VOID *Owner);
  CR_STATUS (*Rollback)(VOID *, VOID *Owner);
} CR_PEP_RESOURCE_IO;
typedef struct {
  CR_STATUS Status, RollbackStatus;
  UINT32 Applied;
} CR_PEP_RESULT;

CR_STATUS CrPepDecodeResource(CONST CR_PEP_VALUE *, CR_PEP_RESOURCE_OP *);
/* Decode Windows ACPI V1/V2 output into caller-owned nodes without allocating.
   Output is the list of method output arguments; a single returned LPCC package
   is Output->Package.Values[0]. Keep both buffer and nodes immutable/alive while
   their policy is used. Query with Nodes=NULL/Capacity=0 to obtain Required.
   Bounds, nesting, lengths and strings are validated before writing any nodes. */
CR_STATUS CrPepDecodeAcpiOutput(CONST VOID *Buffer, UINTN Length,
                               CR_PEP_VALUE *Nodes, UINTN Capacity,
                               UINTN *Required, CONST CR_PEP_VALUE **Output);
CR_STATUS CrPepPolicyInit(CR_PEP_POLICY *, CONST CR_PEP_VALUE *Root);
CR_STATUS CrPepFindDevice(CONST CR_PEP_POLICY *, CR_PEP_TEXT Id, UINT32 *Index);
CR_STATUS CrPepGetAbandonState(CONST CR_PEP_POLICY *, UINT32 Device, UINT32 *State);
/* Immutable descriptions contain policy-owned views. Missing states are not
   synthesized. ValidateDeviceResources preflights every declared D/F/P state
   without Begin/Apply, so prepare can decline unsupported future operations. */
CR_STATUS CrPepGetDeviceInfo(CONST CR_PEP_POLICY *, UINT32 Device, CR_PEP_DEVICE_INFO *);
CR_STATUS CrPepGetComponentInfo(CONST CR_PEP_POLICY *, UINT32 Device, UINT32 Component,
                                CR_PEP_COMPONENT_INFO *);
CR_STATUS CrPepValidateDeviceResources(CONST CR_PEP_POLICY *, UINT32 Device,
                                      VOID *Owner, CONST CR_PEP_RESOURCE_IO *);
/* Up to 65 transitions for one owner (D0 plus F0/P0 for 32 components).
   Resolve every state before provider
   preflight. Apply uses one transaction/rollback for the complete ordered
   batch, including D0 plus all component F0 operations during prepare. */
CR_STATUS CrPepValidateStates(CONST CR_PEP_POLICY *, UINT32 Device,
                             CONST CR_PEP_TRANSITION *, UINT32 Count,
                             VOID *Owner, CONST CR_PEP_RESOURCE_IO *);
CR_STATUS CrPepApplyStates(CONST CR_PEP_POLICY *, UINT32 Device,
                          CONST CR_PEP_TRANSITION *, UINT32 Count,
                          VOID *Owner, CONST CR_PEP_RESOURCE_IO *, CR_PEP_RESULT *);
CR_STATUS CrPepApplyState(CONST CR_PEP_POLICY *, UINT32 Device,
                         CR_PEP_STATE_KIND Kind, UINT32 Component, UINT32 State,
                         VOID *Owner, CONST CR_PEP_RESOURCE_IO *, CR_PEP_RESULT *);
