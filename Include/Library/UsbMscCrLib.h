/* Portable USB mass-storage BOT, at most two LUNs. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_USB_MSC_DISK_BYTES (16U * 1024U * 1024U)
#define CR_USB_MSC_BLOCK_BYTES 512U
#define CR_USB_MSC_TRANSFER_MAX 65536U
#define CR_USB_MSC_MAX_LUNS 2U

typedef enum {
  CrUsbMscNone = 0,
  CrUsbMscBulkOut,
  CrUsbMscBulkIn,
  CrUsbMscStallOut,
  CrUsbMscStallIn
} CR_USB_MSC_ACTION_TYPE;
typedef struct {
  CR_USB_MSC_ACTION_TYPE Type;
  VOID *Buffer;
  UINT32 Length;
} CR_USB_MSC_ACTION;

typedef CR_STATUS (*CR_USB_MSC_BLOCK_READ)(VOID *Context, UINT64 Lba,
                                          UINT32 Blocks, VOID *Buffer);
typedef CR_STATUS (*CR_USB_MSC_BLOCK_WRITE)(VOID *Context, UINT64 Lba,
                                           UINT32 Blocks, CONST VOID *Buffer);
typedef CR_STATUS (*CR_USB_MSC_BLOCK_FLUSH)(VOID *Context);
typedef CR_STATUS (*CR_USB_MSC_BLOCK_CHECK)(VOID *Context);
typedef struct {
  VOID *Context;
  UINT32 BlockSize;
  UINT64 LastBlock;
  BOOLEAN ReadOnly;
  CR_USB_MSC_BLOCK_READ Read;
  CR_USB_MSC_BLOCK_WRITE Write;
  CR_USB_MSC_BLOCK_FLUSH Flush;
  CR_USB_MSC_BLOCK_CHECK Check; /* Optional media identity/presence check. */
} CR_USB_MSC_BLOCK_BACKEND;

typedef struct {
  CR_USB_MSC_BLOCK_BACKEND Backend;
  CR_STATUS MediaError;
  UINT8 SenseKey, SenseAsc, SenseAscq;
  BOOLEAN Ejected, PreventRemoval;
} CR_USB_MSC_LUN;

/* Private state is exposed only for caller allocation; do not mutate fields.
 * All API calls require the same caller serialization. This core allocates no
 * memory. Block callbacks run synchronously in the same serialized caller.
 * The caller retains Context, Disk/backend context until every transfer is
 * retired. Device-to-host direction is BOOLEAN In == TRUE. */
typedef struct {
  UINT8 *Disk;
  UINT32 DiskBytes, State, AfterStall, HostBytes, DeviceBytes, DoneBytes;
  UINT32 Residue, DiskOffset, Remaining, IssuedLength, IssuedType;
  UINT16 MaxPacket;
  UINT8 Cbw[32], Csw[13], Reply[96];
  UINT8 Status, DeviceDirection, LunCount, ActiveLun;
  BOOLEAN Initialized, Issued, HaltIn, HaltOut, ResetRequired, ResetSeen;
  BOOLEAN ClearIn, ClearOut, NeedZlp, DataOnDisk;
  UINT8 Discard[CR_USB_MSC_TRANSFER_MAX];
  CR_USB_MSC_LUN Luns[CR_USB_MSC_MAX_LUNS];
  CR_USB_MSC_LUN UnsupportedLun; /* Sense only; never a backend. */
  UINT64 MediaLba;
  BOOLEAN BlockBackend, SyncRequested, CheckRequested;
} CR_USB_MSC;

/* Init formats a 16 MiB MBR/FAT16 disk with README.TXT; no physical storage is
 * referenced. Init requires zeroed context and disjoint caller-owned disk.
 * SetMaxPacket accepts 64/512/1024, before CBW work on each configured bus. */
CR_STATUS CrUsbMscInit(CR_USB_MSC *Context, VOID *Disk, UINTN DiskBytes);
/* Copies immutable single-media geometry/callbacks; never formats media.
 * BlockSize must be a power of two from 512 through 65536. Read is required;
 * writable media requires Write and Flush, and every successful write chunk
 * is flushed before acknowledgment (MODE SENSE WCE=0). Read-only media may
 * omit Write/Flush. Callbacks receive whole blocks, at most 64 KiB, using
 * internal staging; they must not retain Buffer. Exact CR_SUCCESS means all
 * blocks completed. Failures may have partially changed media; no rollback
 * or command-level atomicity is promised.
 * Status mapping: CR_NOT_FOUND=no media; CR_ABORT=media changed;
 * CR_BUSY=write protected; CR_NOT_READY=not ready; others=I/O failure.
 * No-media/changed is latched across USB/BOT resets: reinitialize a new core
 * to select a replacement medium. The adapter must pin media identity and
 * reject changed media on each callback. Context storage remains caller-owned.
 * Optional Check is called for admitted TEST UNIT READY / READ CAPACITY10/16
 * commands, after CDB and BOT direction/length validation. NULL means removal
 * is detected only when an I/O callback reports it. Check must not select a
 * replacement medium. A failed check follows the same status/sense mapping.
 * Requires zeroed core and a separate descriptor; performs no callback. */
CR_STATUS CrUsbMscInitBlock(CR_USB_MSC *Context,
                           CONST CR_USB_MSC_BLOCK_BACKEND *Backend);
/* Copies Count immutable descriptors into USB LUNs 0..Count-1. Count must be
 * 1 or 2. All descriptors are validated before the core is changed; no
 * callback runs during initialization. Descriptors must not overlap Context.
 * Each LUN's geometry, sense, media-error latch, eject and removal-prevention
 * state is independent. All LUNs share the sequential BOT transport/buffer.
 * A replacement medium requires a new core; no LUN can silently rebind. */
CR_STATUS CrUsbMscInitBlocks(CR_USB_MSC *Context,
                            CONST CR_USB_MSC_BLOCK_BACKEND *Backends,
                            UINT8 Count);
/* Available immediately after successful initialization. Does not issue I/O.
 * Output must not overlap core storage or the RAM disk. */
CR_STATUS CrUsbMscGetMaxLun(CONST CR_USB_MSC *Context, UINT8 *MaxLun);
CR_STATUS CrUsbMscSetMaxPacket(CR_USB_MSC *Context, UINT16 MaxPacket);
/* Next issues exactly one action; its Buffer remains valid until Complete.
 * Calling Next while Issued returns CR_BUSY. NONE has no corresponding
 * Complete. BULK_IN Length==0 is an explicit ZLP; disable automatic USB ZLP.
 * OUT data must be copied to Buffer before successful Complete. For stalls,
 * Complete(TRUE,0) follows successful hardware halt programming. */
CR_STATUS CrUsbMscNext(CR_USB_MSC *Context, CR_USB_MSC_ACTION *Action);
CR_STATUS CrUsbMscComplete(CR_USB_MSC *Context, BOOLEAN Success, UINT32 Bytes);
/* Adapter must abort/drain outstanding USB transfers before either reset.
 * BOT class reset preserves endpoint halts, and reset recovery additionally
 * requires successful clear-halt requests on BOTH bulk endpoints afterwards.
 * BusReset clears transport halts and preserves RAM contents. Both resets
 * clear every LUN's prevent-removal state. Class reset preserves sense/eject;
 * bus reset clears sense/eject. Both preserve latched no-media/changed errors
 * and all immutable backend identities. */
CR_STATUS CrUsbMscReset(CR_USB_MSC *Context);
CR_STATUS CrUsbMscBusReset(CR_USB_MSC *Context);
/* Call before clearing the physical endpoint. FALSE means retain hardware
 * halt (invalid CBW before class reset). If TRUE, physical clear must succeed
 * before any further Next call. A failed hardware clear requires stopping the
 * transport; do not continue from the acknowledged software transition. */
BOOLEAN CrUsbMscClearHalt(CR_USB_MSC *Context, BOOLEAN In);
