/* Production DXE SMEM protocol/locking regression. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Driver/SmemCrDxe/SmemCrDxe.c"

static UINT8 Heap[32768] __attribute__((aligned(8)));
static UINT32 LockOwner;
static UINTN LockWrites, LockReleases, Delays;
static BOOLEAN Interrupts = TRUE;
static BOOLEAN InjectForeignOwner;
static UINT32 InjectAfterCursor;
static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;
EFI_GUID gEfiSMEMProtocolGuid = EFI_SMEM_PROTOCOL_GUID;
EFI_GUID gEfiSmemCrProtocolGuid = EFI_SMEM_CR_PROTOCOL_GUID;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (void)Level; return TRUE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{ (void)Level; (void)Format; }
VOID EFIAPI DebugAssert(CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description)
{ fprintf(stderr, "%s:%zu %s\n", File, (size_t)Line, Description); assert(0); }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
VOID *EFIAPI ZeroMem(VOID *Buffer, UINTN Length) { return memset(Buffer,0,Length); }
VOID EFIAPI MemoryFence(VOID) { __sync_synchronize(); }
BOOLEAN EFIAPI SaveAndDisableInterrupts(VOID)
{ BOOLEAN Saved=Interrupts; Interrupts=FALSE; return Saved; }
BOOLEAN EFIAPI SetInterruptState(BOOLEAN State) { Interrupts=State; return State; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *Value, UINT32 Compare, UINT32 Exchange)
{ return __sync_val_compare_and_swap(Value,Compare,Exchange); }
UINTN EFIAPI MicroSecondDelay(UINTN Us) { ++Delays; return Us; }
UINT32 EFIAPI MmioWrite32(UINTN Address,UINT32 Value)
{
  assert(Address==CRANE_SMEM_MUTEX_ADDRESS);
  assert(!Interrupts);
  if(Value==CRANE_SMEM_MUTEX_OWNER) {
    ++LockWrites;
    if(!LockOwner) LockOwner=Value;
  } else {
    assert(Value==0 && LockOwner==CRANE_SMEM_MUTEX_OWNER);
    ++LockReleases; LockOwner=0;
  }
  return Value;
}
UINT32 EFIAPI MmioRead32(UINTN Address)
{
  assert(Address==CRANE_SMEM_MUTEX_ADDRESS);
  UINT32 Cursor;
  memcpy(&Cursor,Heap+4096+12,4);
  if(InjectForeignOwner && Cursor>InjectAfterCursor) {
    InjectForeignOwner=FALSE; LockOwner=2;
  }
  return LockOwner;
}
EFI_STATUS EFIAPI LocateMemoryMapAreaByName(CHAR8 *Name, ARM_MEMORY_REGION_DESCRIPTOR_EX *Region)
{ (void)Name; (void)Region; return EFI_NOT_FOUND; }
static void Put32(UINTN Offset,UINT32 Value) { memcpy(Heap+Offset,&Value,4); }
static void Put16(UINTN Offset,UINT16 Value) { memcpy(Heap+Offset,&Value,2); }
static UINT32 Get32(UINTN Offset) { UINT32 Value; memcpy(&Value,Heap+Offset,4); return Value; }
static void Fixture(void)
{
  UINTN Table=sizeof(Heap)-4096, Entry=Table+32, Part=4096;
  memset(Heap,0,sizeof(Heap));
  Put32(192,1); Put32(92,12U<<16);
  Put32(Table,0x434f5424); Put32(Table+4,1); Put32(Table+8,2);
  Put32(Entry,Part); Put32(Entry+4,0x4000);
  Put16(Entry+12,SMEM_GLOBAL_HOST); Put16(Entry+14,SMEM_GLOBAL_HOST);
  Put32(Entry+16,32);
  Put32(Part,0x54525024); Put16(Part+4,SMEM_GLOBAL_HOST); Put16(Part+6,SMEM_GLOBAL_HOST);
  Put32(Part+8,0x4000); Put32(Part+12,32); Put32(Part+16,0x4000);
  UINTN Private=0x5000, PrivateEntry=Entry+48;
  Put32(PrivateEntry,Private); Put32(PrivateEntry+4,0x1000);
  Put16(PrivateEntry+12,0); Put16(PrivateEntry+14,2); Put32(PrivateEntry+16,32);
  Put32(Private,0x54525024); Put16(Private+4,0); Put16(Private+6,2);
  Put32(Private+8,0x1000); Put32(Private+12,32); Put32(Private+16,0x1000);
  assert(SmemInitialize(&mSmem,Heap,sizeof(Heap),0x80900000)==CR_SUCCESS);
  LockOwner=0; LockWrites=LockReleases=Delays=0; Interrupts=TRUE;
  mAllocationBusy=0;
  mHardwareLock.Held=FALSE; InjectForeignOwner=FALSE;
}
int main(void)
{
  VOID *Address=(VOID *)1,*First;
  UINT32 Cursor,Size;
  Fixture();
  assert(CraneSmemAllocate(404,0x2000,&Address)==EFI_SUCCESS);
  assert(Address==Heap+4096+48 && Interrupts && !LockOwner);
  assert(LockWrites==1 && LockReleases==1 && mAllocationBusy==0);
  First=Address; Cursor=Get32(4096+12);
  memset(First,0x5a,0x2000);
  /* Boot firmware may publish DAL item 404 before DXE: retry returns the
   * original object with intact data and no second allocation. */
  assert(CraneSmemAllocate(404,0x2000,&Address)==EFI_SUCCESS && Address==First);
  assert(Get32(4096+12)==Cursor && ((UINT8 *)Address)[0]==0x5a);
  assert(CraneSmemAllocate(404,7,&Address)==EFI_SUCCESS && Address==First);
  assert(CraneSmemAllocate(404,0x2001,&Address)==EFI_BAD_BUFFER_SIZE && !Address);
  assert(Get32(4096+12)==Cursor && Interrupts && !LockOwner && !mAllocationBusy);
  assert(CraneSmemGet(404,&Size,&Address)==EFI_SUCCESS && Address==First && Size==0x2000);
  assert(CraneSmemGet(405,&Size,&Address)==EFI_NOT_FOUND && !Address && Size==0);
  assert(CraneSmemScopedGet(&mScopedProtocol,SMEM_HOST_ANY,404,&Size,&Address)==EFI_SUCCESS);
  assert(Address==First && Size==0x2000);
  assert(CraneSmemScopedAllocate(&mScopedProtocol,SMEM_HOST_ANY,404,8,&Address)==EFI_ALREADY_STARTED);
  assert(!Address && Get32(4096+12)==Cursor && !LockOwner && Interrupts);
  assert(CraneSmemScopedGet(&mScopedProtocol,SMEM_HOST_COUNT,404,&Size,&Address)==EFI_INVALID_PARAMETER);
  assert(!Address && !Size);
  assert(CraneSmemScopedAllocate(&mScopedProtocol,2,478,32,&Address)==EFI_SUCCESS);
  assert(Address==Heap+0x5000+48 && !LockOwner && Interrupts);
  memset(Address,0x6b,32);
  assert(CraneSmemScopedGet(&mScopedProtocol,2,478,&Size,&Address)==EFI_SUCCESS);
  assert(Size==32 && ((UINT8 *)Address)[0]==0x6b);
  assert(CraneSmemGet(478,&Size,&Address)==EFI_NOT_FOUND && !Address);
  assert(CraneSmemAllocate(-1,8,&Address)==EFI_INVALID_PARAMETER && !Address);
  assert(CraneSmemAllocate(404,MAX_UINT32,&Address)==EFI_INVALID_PARAMETER && !Address);
  assert(CraneSmemAllocate(404,8,NULL)==EFI_INVALID_PARAMETER);
  /* Remote contention cannot alter the shared cursor or release its owner. */
  LockOwner=2; LockWrites=LockReleases=Delays=0;
  assert(CraneSmemAllocate(405,8,&Address)==EFI_TIMEOUT && !Address);
  assert(Get32(4096+12)==Cursor && LockOwner==2 && LockReleases==0);
  assert(LockWrites==1000 && Delays==1000 && Interrupts && !mAllocationBusy);
  /* A nested APPS invocation cannot treat the same owner ID as ownership. */
  LockOwner=1; mAllocationBusy=1; LockWrites=0;
  assert(CraneSmemAllocate(405,8,&Address)==EFI_ACCESS_DENIED && !Address);
  assert(mAllocationBusy==1 && LockOwner==1 && !LockWrites && Interrupts);
  mAllocationBusy=0;
  /* Unlock must preserve a lock that changed to a remote owner. */
  mHardwareLock.Held=TRUE;
  LockOwner=2; assert(CraneSmemReleaseLock()==EFI_DEVICE_ERROR && LockOwner==2);
  assert(mHardwareLock.Held);
  LockOwner=0; assert(CraneSmemReleaseLock()==EFI_SUCCESS && !mHardwareLock.Held);
  /* Publication before a failed unlock is durable. A later allocation must
   * recover the retained release rather than writing APPS ownership again. */
  Fixture(); Cursor=Get32(4096+12);
  InjectAfterCursor=Cursor; InjectForeignOwner=TRUE;
  assert(CraneSmemAllocate(405,8,&Address)==EFI_DEVICE_ERROR && !Address);
  assert(mHardwareLock.Held && LockOwner==2 && Get32(4096+12)>Cursor);
  Cursor=Get32(4096+12); UINTN Writes=LockWrites;
  assert(CraneSmemAllocate(406,8,&Address)==EFI_DEVICE_ERROR && !Address);
  assert(LockWrites==Writes && Get32(4096+12)==Cursor && mHardwareLock.Held);
  LockOwner=0;
  assert(CraneSmemAllocate(406,8,&Address)==EFI_SUCCESS && Address);
  assert(LockWrites==Writes+1 && !mHardwareLock.Held && !LockOwner);
  /* Existing disabled interrupt state is preserved on normal/error exits. */
  LockOwner=0; Interrupts=FALSE;
  assert(CraneSmemAllocate(406,8,&Address)==EFI_SUCCESS && !Interrupts && !LockOwner);
  puts("SMEM DXE: duplicate item 404, size mismatch, remote/local lock ownership and interrupts passed");
  return 0;
}
