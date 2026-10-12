/* Inactive UEFI per-image gate lifecycle, no runtime library dependency. MIT */
#pragma once
#include <Uefi.h>
#include <Library/UartAdmissionLib.h>

#define CR_UART_ADMISSION_TABLE_GUID \
  {0x0a89f30b,0xf3ee,0x434b,{0x9c,0x2b,0x3a,0x3b,0x72,0x88,0xaf,0x19}}

/* One zeroed instance per linked image. Gate storage is permanently retained
 * RuntimeServicesData, shared through the configuration table. The EBS event
 * is closed before image unload under TPL_NOTIFY. Native integration relies
 * on actual CoreCloseEvent's valid-owned-event invariant: queued callbacks
 * are removed synchronously before its event pool is freed. Generated EDK
 * library destructors discard return statuses; they are not an image pin.
 * Caller must never unload an image executing one of its entrypoints or
 * externally close our private Event. Failed-close model state is diagnostic
 * and cannot be claimed as a recoverable native module-lifetime mechanism.
 * AtRuntime/Closed live in the image's runtime data. Enter checks Closed first
 * and never touches boot pointers or the physical Gate after successful EBS;
 * virtual-address events need no callback, conversion, or new allocation. */
typedef struct {
  EFI_BOOT_SERVICES *Boot;
  CR_UART_ADMISSION *Gate;
  EFI_EVENT Event;
  volatile UINT32 Closed, Calls;
  BOOLEAN Constructing, Bound, AtRuntime;
} CR_UART_UEFI_CLIENT;

EFI_STATUS CrUartUefiConstruct(CR_UART_UEFI_CLIENT *, EFI_SYSTEM_TABLE *, BOOLEAN Runtime);
BOOLEAN CrUartUefiEnter(CR_UART_UEFI_CLIENT *);
EFI_STATUS CrUartUefiLeave(CR_UART_UEFI_CLIENT *);
EFI_STATUS CrUartUefiDestruct(CR_UART_UEFI_CLIENT *);
/* Diagnostic only; not an EDK unload barrier. */
BOOLEAN CrUartUefiCanUnload(CONST CR_UART_UEFI_CLIENT *);
