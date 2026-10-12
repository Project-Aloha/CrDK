/** Portable authenticated peripheral image loader. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#ifdef _KERNEL_MODE
#define CR_PIL_SECURITY_VIOLATION ((CR_STATUS)0xc000a000L)
#define CR_PIL_INCOMPATIBLE_VERSION ((CR_STATUS)0xc0000059L)
#else
#define CR_PIL_SECURITY_VIOLATION EFI_SECURITY_VIOLATION
#define CR_PIL_INCOMPATIBLE_VERSION EFI_INCOMPATIBLE_VERSION
#endif

#define CR_PIL_MAX_SEGMENTS 100U
#define CR_PIL_MAX_METADATA 0x100000U

typedef struct {
  UINT32 PasId;
  UINT32 MemoryBase, MemorySize;
  UINT32 SmemHost, SmemIn, SmemOut;
  UINT32 IpccSend, IpccClient;
  UINT32 QmpBase, QmpSize;
  CONST UINT16 *ImagePath;
  CONST CHAR8 *InterconnectProvider;
  UINT32 CryptoSource, CryptoDestination;
  /* Board's public ARC level floor for both LMX/LCX; zero retains the older
   * target policy. CmdDB corner indices are never literal voltage levels. */
  UINT32 ProxyMinimumLevel;
} CR_PIL_TARGET;

typedef struct {
  UINT64 Offset, Address;
  UINT32 FileSize, MemorySize, Flags, Destination;
  BOOLEAN Load;
} CR_PIL_SEGMENT;

typedef struct {
  UINT32 Count, HeaderSize, HashIndex, HashSize, Span;
  BOOLEAN Relocatable, Split;
  CR_PIL_SEGMENT Segments[CR_PIL_MAX_SEGMENTS];
} CR_PIL_IMAGE;

/* Read exactly Size bytes from adsp.bNN, rejecting both short and oversized
   files. The library never owns or closes the caller's filesystem. */
typedef CR_STATUS (*CR_PIL_READ_SEGMENT)(VOID *Context, UINT32 Index,
                                         VOID *Buffer, UINT32 Size);
/* Callers serialize every operation at PASSIVE level. IO storage, target and
   mappings outlive retained failed cleanup. SMEM pointers remain valid for the
   whole binding; the Windows adapter must own a referenced provider lease.
   ProxyWrite and Crypto callbacks own this PIL instance's votes and must not
   withdraw another consumer's demand. SMC accepts the exact x0..x7 register
   frame; transport failure may have committed and must preserve ownership. */
typedef struct { UINT64 X[8]; } CR_PIL_SMC_ARGS;
typedef struct {
  VOID *Context;
  CR_STATUS (*Smc)(VOID *, CR_PIL_SMC_ARGS *);
  UINT32 (*Read32)(VOID *, UINT32 PhysicalAddress);
  VOID (*Write32)(VOID *, UINT32 PhysicalAddress, UINT32 Value);
  VOID (*Fence)(VOID *);
  VOID (*DelayUs)(VOID *, UINT32 Microseconds);
  CR_STATUS (*SmemGet)(VOID *, UINT32 Host, UINT32 Item, UINT32 *Size, VOID **Address);
  CR_STATUS (*SmemAllocate)(VOID *, UINT32 Host, UINT32 Item, UINT32 Size, VOID **Address);
  CR_STATUS (*CmdAddress)(VOID *, CONST CHAR8 *Name, UINT32 *Address);
  CR_STATUS (*CmdAux)(VOID *, CONST CHAR8 *Name, VOID *Data, UINT32 *Size);
  CR_STATUS (*ProxyWrite)(VOID *, UINT32 Address, UINT32 Value);
  CR_STATUS (*CryptoAcquire)(VOID *, CONST CHAR8 *Provider, UINT32 Source, UINT32 Destination, UINT64 *Handle);
  CR_STATUS (*CryptoSet)(VOID *, UINT64 Handle, UINT64 Average, UINT64 Peak);
  CR_STATUS (*CryptoRelease)(VOID *, UINT64 Handle);
} CR_PIL_IO;
/* Observations of this instance's owned PAS shutdown, not permission for a
   future OS to execute SMC. Complete requires an actual returned x0=x1=0
   from shutdown or its exact continuation. Other resources may still need
   cleanup after Complete becomes true. Snapshot under the lifecycle lock. */
typedef struct {
  UINT64 Generation, FunctionId, X0, X1;
  CR_STATUS CallStatus;
  BOOLEAN Attempted, Returned, Complete;
} CR_PIL_SHUTDOWN_EVIDENCE;
typedef struct {
  CONST CR_PIL_TARGET *Target;
  CR_PIL_IO Io;
  /* Native initializer privately stores referenced EFI adapters here. The
     opaque slots keep the public context identical across operating systems. */
  VOID *NativeAdapter[4];
  UINT64 CryptoPath;
  volatile UINT32 *SmpOut;
  UINT32 QmpOffset;
  UINT32 VoteAddress[3], VoteValue[3];
  UINT32 Votes;
  UINT32 SmcBase;
  /* Interrupted standard calls retain the original arguments and opaque
     cookie. Stop must retire this continuation before shutdown/resource
     release; failed retirement keeps the complete context and IO binding. */
  CR_PIL_SMC_ARGS SmcOriginal;
  UINT64 SmcCookie;
  BOOLEAN SmcInterrupted;
  BOOLEAN QmpOpen, QmpChanged, LoadState, PasOwned, Running;
  CONST CHAR8 *Stage;
  BOOLEAN QmpClosing;
  /* The complete AOSS handshake returned before this producer sent a load
     request. Stop retires the producer after consumed off + coherent UP;
     it does not remove the shared AOSS transport service. Partial setup
     keeps its separate changed/closing journal until cleanup is observed. */
  BOOLEAN QmpEstablished;
  UINT64 StartGeneration;
  CR_PIL_SHUTDOWN_EVIDENCE Shutdown;
  /* Native adapter observation of the actual successful owned shutdown SMC.
     Not inferred from the boot CPU or usable as Windows call permission. */
  UINT64 NativeShutdownGeneration;
  UINT32 NativeShutdownEl;
} CR_PIL_CONTEXT;

CR_STATUS CrPilInspect(CONST VOID *Header, UINTN Size,
                        CONST CR_PIL_TARGET *Target, CR_PIL_IMAGE *Image);
CR_STATUS CrPilBuildMetadata(CONST VOID *Header, UINTN Size,
                              CONST CR_PIL_IMAGE *Image,
                              CR_PIL_READ_SEGMENT Read, VOID *FileContext,
                              VOID *Metadata, UINTN MetadataSize);
CR_STATUS CrPilCoreInitialize(CR_PIL_CONTEXT *Pil, CONST CR_PIL_TARGET *Target,
                              CONST CR_PIL_IO *Io);
/* Metadata must be page-aligned, normal noncacheable memory below 4 GiB,
   retained until successful authentication or confirmed PAS shutdown. The
   target carveout must be reserved and mapped normal noncacheable. */
CR_STATUS CrPilStart(CR_PIL_CONTEXT *Pil, CONST VOID *Header, UINTN Size,
                      CONST CR_PIL_IMAGE *Image, VOID *Metadata,
                      VOID *Carveout, CR_PIL_READ_SEGMENT Read,
                      VOID *FileContext);
/* No allocation or filesystem access; usable during ExitBootServices.
   Failed shutdown keeps ownership and prerequisites for a later retry.
   Success retires this PAS/load-state producer and its votes, not the shared
   AOSS transport: established QMP stays coherently UP for the next owner. */
CR_STATUS CrPilStop(CR_PIL_CONTEXT *Pil);
/* Pure copy of recorded evidence, including failed/incomplete observations.
   CR_SUCCESS means the snapshot is valid, not that shutdown or handoff passed. */
CR_STATUS CrPilGetShutdownEvidence(CONST CR_PIL_CONTEXT *Pil,
                                   CR_PIL_SHUTDOWN_EVIDENCE *Evidence);

/* Windows uses a separately obtained physical metadata address; virtual
   addresses are never submitted to PAS. Metadata and Carveout allocations are
   caller-owned normal noncacheable memory. Caller verifies full buffer sizes
   and reserves the physical carveout before invoking either Start form. */
CR_STATUS CrPilStartPhysical(CR_PIL_CONTEXT *, CONST VOID *Header, UINTN Size,
                             CONST CR_PIL_IMAGE *, VOID *Metadata,
                             UINT64 MetadataPhysical, VOID *Carveout,
                             CR_PIL_READ_SEGMENT, VOID *FileContext);
/* Revalidate remote readiness/fatal status while IO lease and serialization
   remain held. This does not borrow or stop a remote bootloader-owned image. */
CR_STATUS CrPilCheckReady(CR_PIL_CONTEXT *);
