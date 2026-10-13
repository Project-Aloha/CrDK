/* Qualcomm PAN policy and passive observation of OS-owned UCSI commands.
 * Copyright (c) 2020-2021 The Linux Foundation.
 * Copyright (c) 2022-2023, Linaro Ltd.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <ucsi/TypeC.h>
typedef CR_STATUS (*CR_PMIC_TYPEC_SEND)(VOID *,CONST UINT8 *,UINTN);
typedef struct {
  CR_PMIC_TYPEC_SEND Send;VOID *Context;
  CR_USB_TYPEC_STATE State,Status,Alt;
  UINT64 Generation,Control,DeadlineUs,RetryUs,UsbcDeadlineUs;
  UINT32 AdspGeneration,PeerGeneration,PanCommand,Acks;
  BOOLEAN Online,StatusValid,AltValid,WritePending,WriteAcked,ReadPending;
  BOOLEAN PanPending,PanUncertain,PanEnabled,PanFaulted,UsbcFaulted,Closed;
} CR_PMIC_TYPEC;
/* External serialization; no allocation or hardware access. Send is used
 * exclusively for owner32780 PAN_ENABLE/PAN_ACK, never UCSI policy commands.
 * USBC observations are the exact packets accepted by the sole OS transport.
 * Generation changes with route/data-role/availability, excluding HPD-only
 * events. PAN wire has no transaction ID: timeout poisons its slot until a new
 * ADSP generation, so a late ACK cannot retire a subsequent request. */
VOID CrPmicTypeCInit(CR_PMIC_TYPEC *,CR_PMIC_TYPEC_SEND,VOID *);
VOID CrPmicTypeCPeer(CR_PMIC_TYPEC *,BOOLEAN Online,UINT32 Peer,UINT32 Adsp);
VOID CrPmicTypeCObserveSend(CR_PMIC_TYPEC *,CONST UINT8 *,UINTN,CR_STATUS,UINT64 NowUs);
/* Pure expiration; no Send callback. Used before returning cached policy. */
VOID CrPmicTypeCExpire(CR_PMIC_TYPEC *,UINT64 NowUs);
VOID CrPmicTypeCReceive(CR_PMIC_TYPEC *,CONST UINT8 *,UINTN,UINT64 NowUs);
CR_STATUS CrPmicTypeCTick(CR_PMIC_TYPEC *,UINT64 NowUs);
VOID CrPmicTypeCInvalidateUsbc(CR_PMIC_TYPEC *);
VOID CrPmicTypeCClose(CR_PMIC_TYPEC *);
/* No outstanding PAN request/ACK. Timeout is not remote retirement; an exact
 * late original response or a verified new ADSP generation can retire it. */
BOOLEAN CrPmicTypeCDrained(CONST CR_PMIC_TYPEC *);
/* Pure snapshot, unchanged outputs on failure; not a reset/DMA/PHY grant. */
CR_STATUS CrPmicTypeCGet(CONST CR_PMIC_TYPEC *,CR_USB_TYPEC_STATE *,UINT64 *);
