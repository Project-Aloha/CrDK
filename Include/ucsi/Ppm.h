/* Raw PMIC UCSI transport for an OS-owned OPM. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#define CR_UCSI_V1_BLOCK_SIZE 48U
#define CR_UCSI_NOTIFY_CAPACITY 16U
typedef CR_STATUS (*CR_UCSI_PPM_SEND)(VOID *, CONST VOID *, UINTN);
typedef enum { CrUcsiPpmIdle, CrUcsiPpmReading, CrUcsiPpmWriting } CR_UCSI_PPM_OPERATION;
typedef struct {
  CR_UCSI_PPM_SEND Send;
  VOID *Context;
  CR_UCSI_PPM_OPERATION Operation;
  UINT64 DeadlineUs;
  UINT32 Generation;
  UINT16 Version;
  UINT8 Block[CR_UCSI_V1_BLOCK_SIZE];
  UINT32 Notifications[CR_UCSI_NOTIFY_CAPACITY];
  UINTN NotificationHead, NotificationCount;
  CR_STATUS Status;
  BOOLEAN Online, Faulted, Done, Cancelled;
} CR_UCSI_PPM;
/* All calls are serialized by the adapter at PASSIVE/work level. Send copies
 * its packet before returning; no pointer outlives the call. This engine owns
 * no RESET/ACK/notification policy: the OS class extension is the sole OPM.
 * The PMIC protocol has no transaction ID. Timeout or uncertain send poisons
 * this peer generation; caller must establish a new GLink/PD generation before
 * reusing it, so a late response cannot complete a newer operation. */
VOID CrUcsiPpmInitialize(CR_UCSI_PPM *, CR_UCSI_PPM_SEND, VOID *);
VOID CrUcsiPpmPeer(CR_UCSI_PPM *, BOOLEAN Online, UINT32 Generation);
CR_STATUS CrUcsiPpmRead(CR_UCSI_PPM *, UINT64 NowUs);
CR_STATUS CrUcsiPpmWrite(CR_UCSI_PPM *, CONST UINT8 Block[48], UINT64 NowUs);
VOID CrUcsiPpmReceive(CR_UCSI_PPM *, CONST UINT8 *, UINTN);
VOID CrUcsiPpmTick(CR_UCSI_PPM *, UINT64 NowUs);
VOID CrUcsiPpmCancel(CR_UCSI_PPM *);
CR_STATUS CrUcsiPpmTake(CR_UCSI_PPM *, UINT8 Block[48]);
BOOLEAN CrUcsiPpmNotification(CR_UCSI_PPM *, UINT32 *Cci);
