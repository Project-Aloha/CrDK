/* SM8450 SDHCI-v5, bounded PIO core. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_SDHC_HOST_END 0x220U
#define CR_SDHC_POWER_BEGIN 0x240U
#define CR_SDHC_POWER_END 0x254U
#define CR_SDHC_WINDOW_HOST 1U
#define CR_SDHC_WINDOW_POWER 2U
#define CR_SDHC_WINDOW_ALL 3U
#define CR_SDHC_MIN_MAPPING CR_SDHC_POWER_END
#define CR_SDHC_EVENT_COMMAND 1U
#define CR_SDHC_EVENT_TRANSFER 2U
#define CR_SDHC_EVENT_READ_READY 0x20U
#define CR_SDHC_EVENT_WRITE_READY 0x10U
#define CR_SDHC_POWER_OFF 1U
#define CR_SDHC_POWER_ON 2U
#define CR_SDHC_IO_LOW 4U
#define CR_SDHC_IO_HIGH 8U
typedef struct {
  VOID *Context;
  CR_STATUS (*Ready)(VOID *);
  CR_STATUS (*Read)(VOID *,UINT32 Offset,UINT8 Width,UINT32 *);
  CR_STATUS (*Write)(VOID *,UINT32 Offset,UINT8 Width,UINT32);
  CR_STATUS (*StallUs)(VOID *,UINT32 Microseconds);
  CR_STATUS (*Barrier)(VOID *);
  UINT32 MappingSize;
  /* Explicit assigned windows, not a contiguous fictitious mapping. Host
   * covers [0,0x220); aux power covers [0x240,0x254). Read/Write must enforce
   * their actual translated slices too. Independent PDOs may own each window.
   * Register 0x250 (vendor3 DLL savings) belongs to the aux power owner. */
  UINT32 Windows;
} CR_SDHC_IO;
typedef enum {CrSdResponseNone,CrSdResponse48,CrSdResponse48Busy,CrSdResponse136,CrSdResponse48NoCrc} CR_SDHC_RESPONSE;
typedef struct {
  UINT8 Index,Type;
  CR_SDHC_RESPONSE Response;
  UINT32 Argument;
  BOOLEAN Data,Read,AutoCmd12;
  UINT16 BlockSize,BlockCount;
  UINT8 *Buffer;
  UINT32 Length;
} CR_SDHC_COMMAND;
typedef struct CR_SDHC {
  CR_SDHC_IO Io;
  UINT8 *Buffer;
  UINT32 Length,Transferred,BlockProgress,PowerPending;
  /* Last atomic normal/error IRQ snapshot, including masked causes. */
  UINT32 InterruptStatus;
  UINT16 BlockSize,Remaining;
  /* Last issued command, including an uncertain write. Qualcomm needs a
   * CRC-engine settling interval after a response without CRC. */
  UINT16 LastCommand;
  BOOLEAN Initialized,DataActive,Read,NeedsReset;
  /* Optional owner-provided synchronous power boundary. Begin snapshots the
   * acknowledged state before a triggering write; End always runs after a
   * successful Begin, even when the write/barrier fails. It must perform or
   * wait for the actual IRQ effects at a sleepable level, with exact success.
   * NULL leaves synchronization to an independently serialized OS adapter. */
  CR_STATUS (*SyncPower)(struct CR_SDHC *,UINT32 Request,BOOLEAN Begin,CR_STATUS WriteStatus);
} CR_SDHC;
/* No MMIO in Init. Ready must prove actual native retirement/assigned mappings
 * and live access leases. Every sync callback requires EXACT CR_SUCCESS;
 * pending/warnings never qualify IO. Caller serializes register operations
 * across host ISR, power ISR/worker, bus operations and request DPC.
 * PIO-only: no DMA addresses, tuning, CQE, SDIO or MMC/UHS support is asserted. */
CR_STATUS CrSdhcInit(CR_SDHC *,CONST CR_SDHC_IO *);
/* After an observed RESET_ALL with card clock/power off, restore the v5
 * vendor POR and enable pad-voltage selection for initial 3V SDR operation.
 * This does not vote supplies or acknowledge a hardware power request.
 * An uncertain write requires another reset before commands are admitted. */
CR_STATUS CrSdhcColdInit(CR_SDHC *);
CR_STATUS CrSdhcRead(CR_SDHC *,UINT32,UINT8,UINT32 *);
CR_STATUS CrSdhcWrite(CR_SDHC *,UINT32,UINT8,UINT32);
CR_STATUS CrSdhcReset(CR_SDHC *,UINT8 Mask);
/* MSM bypasses internal SDHCI divider. Caller has set and qualified actual
 * external GCC apps clock to the requested Hz before enabling card clock. */
CR_STATUS CrSdhcClock(CR_SDHC *,BOOLEAN Enable);
CR_STATUS CrSdhcBusWidth(CR_SDHC *,UINT8 Width);
CR_STATUS CrSdhcSdrSpeed(CR_SDHC *,BOOLEAN HighSpeed);
/* Call aux PrepareSdr before this host-only MCLK selection and before changing
 * actual GCC rate. Leaves unrelated pad-power and power-save fields intact. */
CR_STATUS CrSdhcDefaultMclk(CR_SDHC *);
/* Aux-only v5 vendor3 PWRSAVE_DLL clear for the source-backed SDR clock path. */
/* Inactive v5 PIO: clears/checks DLL savings + alternate FIFO in vendor3.
 * Reset alone is not a witness of standard FIFO selection. */
CR_STATUS CrSdhcPrepareSdr(CR_SDHC *);
CR_STATUS CrSdhcIssue(CR_SDHC *,CONST CR_SDHC_COMMAND *,UINT32 *RequiredEvents);
/* Move one bounded block, journaling every irreversible FIFO byte. A partial
 * IO failure retains DataActive/NeedsReset; never replay the entire block. */
CR_STATUS CrSdhcPioBlock(CR_SDHC *,UINT8 *Buffer,UINT32 Length,UINT16 *Remaining);
CR_STATUS CrSdhcResponse(CR_SDHC *,CR_SDHC_RESPONSE,UINT8 Response[16]);
CR_STATUS CrSdhcInterrupt(CR_SDHC *,UINT32 *Events,UINT32 *Errors);
CR_STATUS CrSdhcEvents(CR_SDHC *,UINT32 Events,BOOLEAN Enable);
CR_STATUS CrSdhcClearEvents(CR_SDHC *,UINT32 Events);
/* Capture/mask/W1C at ISR; fallible regulator/pad work occurs PASSIVE. Pending
 * bits commit before W1C, and failed ACK retains exact demand for cleanup.
 * Capture includes bounded ten 10us clear retries for SDCC5 lost-clear erratum.
 * Complete writes only source-backed BUS/IO success/fail ACK bits, then enables
 * all four power causes; caller wakes waiter only after EXACT success. */
CR_STATUS CrSdhcPowerCapture(CR_SDHC *,UINT32 *Observed);
CR_STATUS CrSdhcPowerComplete(CR_SDHC *,UINT32 Observed,BOOLEAN BusSuccess,BOOLEAN IoSuccess);
CR_STATUS CrSdhcPowerMask(CR_SDHC *,BOOLEAN Enable);
CR_STATUS CrSdhcPowerRequest(CR_SDHC *,BOOLEAN On);
CR_STATUS CrSdhcIoPadHigh(CR_SDHC *,BOOLEAN High);
