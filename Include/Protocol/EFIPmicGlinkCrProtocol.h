/** One native PMIC GLINK transport owner; fixed owner subscriptions.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>
#define EFI_PMIC_GLINK_CR_PROTOCOL_REVISION 0x00010000ULL
#define EFI_PMIC_GLINK_CR_PROTOCOL_GUID \
  { 0x10c14b35, 0x1667, 0x44f3, { 0x86, 0x3e, 0x76, 0x68, 0xa5, 0x32, 0xae, 0x41 } }
extern EFI_GUID gEfiPmicGlinkCrProtocolGuid;
typedef struct _EFI_PMIC_GLINK_CR_PROTOCOL EFI_PMIC_GLINK_CR_PROTOCOL;
typedef struct { BOOLEAN Online; UINT32 AdspGeneration, PeerGeneration; EFI_STATUS LastError; } EFI_PMIC_GLINK_CR_STATE;
/* Receive/state callbacks are pure, synchronous, at TPL_NOTIFY; no waiting,
 * BootServices, events, allocation, transport reentry or subscriber retirement.
 * Registration pins callback storage/code until exact successful Unregister.
 * Methods are callable <=TPL_NOTIFY. Send accepts a packet only on exact
 * EFI_SUCCESS; any other return leaves the client request unaccepted. */
typedef VOID (EFIAPI *EFI_PMIC_GLINK_RECEIVE)(VOID *Context, CONST UINT8 *Data, UINTN Length);
typedef VOID (EFIAPI *EFI_PMIC_GLINK_STATE_CHANGE)(VOID *Context, CONST EFI_PMIC_GLINK_CR_STATE *);
typedef EFI_STATUS (EFIAPI *EFI_PMIC_GLINK_REGISTER)(EFI_PMIC_GLINK_CR_PROTOCOL *,
  UINT32 Owner, VOID *StableIdentity, EFI_PMIC_GLINK_RECEIVE,
  EFI_PMIC_GLINK_STATE_CHANGE, VOID *Context);
typedef EFI_STATUS (EFIAPI *EFI_PMIC_GLINK_UNREGISTER)(EFI_PMIC_GLINK_CR_PROTOCOL *,
  UINT32 Owner, VOID *StableIdentity);
typedef EFI_STATUS (EFIAPI *EFI_PMIC_GLINK_SEND)(EFI_PMIC_GLINK_CR_PROTOCOL *,
  UINT32 Owner, VOID *StableIdentity, CONST VOID *, UINTN Length);
typedef EFI_STATUS (EFIAPI *EFI_PMIC_GLINK_GET_STATE)(EFI_PMIC_GLINK_CR_PROTOCOL *,
  EFI_PMIC_GLINK_CR_STATE *);
struct _EFI_PMIC_GLINK_CR_PROTOCOL {
  UINT64 Revision;
  EFI_PMIC_GLINK_REGISTER Register;
  EFI_PMIC_GLINK_UNREGISTER Unregister;
  EFI_PMIC_GLINK_SEND Send;
  EFI_PMIC_GLINK_GET_STATE GetState;
};
