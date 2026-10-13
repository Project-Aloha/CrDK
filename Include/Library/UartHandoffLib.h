/* Data-only final UART evidence builder/reader. SPDX-License-Identifier: MIT */
#pragma once
#include <Interface/CrUartHandoff.h>
#include <Library/UartAdmissionLib.h>

typedef struct {
  UINT64 Generation, RecordPhysical;
  UINT64 DescriptorBase, DescriptorSize, DescriptorType, DescriptorAttributes;
  UINT64 NativeExitStatus; /* raw native EFI_STATUS, zero only exact success */
  CONST CR_UART_FIRMWARE_COVERAGE *Coverage;
  /* Kind resolved only after actual same-FV/PE validation; UNKNOWN records an
   * unidentifiable/replaced loaded runtime source, including an unknown GUID. */
  CONST CR_UART_FIRMWARE_IMAGE *Loaded;
  UINT32 LoadedCount;
  CONST CR_UART_RETIRE_OBSERVATION *Retirement;
} CR_UART_HANDOFF_INPUT;

CR_STATUS CrUartCoverageValidate(CONST CR_UART_FIRMWARE_COVERAGE *);
/* Verifies the actual source FV's full runtime-PE set against generated
 * immutable PE size/CRC. Bounded raw uncompressed ARM64 FFS only; no allocation,
 * decompression or authentication. Caller independently qualifies normal RAM
 * and exact loaded-image DeviceHandle/FilePath source identity. */
CR_STATUS CrUartCoverageVerifyFv(CONST CR_UART_FIRMWARE_COVERAGE *,
                               CONST VOID *Fv, UINTN Bytes);
CR_STATUS CrUartCoverageResolve(CONST CR_UART_FIRMWARE_COVERAGE *,
                               CONST UINT8 Guid[16], CR_UART_FIRMWARE_IMAGE *Out);
/* Copies caller-owned input into Out. Diagnostic output is committed even
 * when an unknown loaded image or incomplete retirement refuses claim.
 * Invalid pointers/counts/header framing preserve Out unchanged. No IO. */
CR_STATUS CrUartHandoffBuild(CR_UART_HANDOFF *Out, CONST CR_UART_HANDOFF_INPUT *);
CR_STATUS CrUartHandoffValidate(CONST CR_UART_HANDOFF *, UINT64 RecordPhysical,
                              UINT64 Generation);
CR_STATUS CrUartHandoffCanClaim(CONST CR_UART_HANDOFF *, UINT64 RecordPhysical,
                              UINT64 Generation);
/* Source must be naturally aligned, independently OS-qualified normal RAM.
 * Read fences bracket volatile copying. Unstable/corrupt data preserves Out.
 * Pure parser success may return an UNCERTAIN diagnostic; call CanClaim. */
CR_STATUS CrUartHandoffSnapshot(CR_UART_HANDOFF *Out,
    CONST volatile CR_UART_HANDOFF *Source, UINT64 Physical, UINT64 Generation);
UINT32 CrUartHandoffChecksum(CONST CR_UART_HANDOFF *);
