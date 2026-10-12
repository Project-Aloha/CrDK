/* Synthetic shared-memory fixtures. SPDX-License-Identifier: MIT */
#include <Library/smem.h>
#include <Library/DebugLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...)
{
  (void)Level;
  (void)Format;
}
static UINTN FenceCount;
VOID EFIAPI MemoryFence(VOID) { ++FenceCount; __sync_synchronize(); }
static UINT8 arena[32768] __attribute__((aligned(8)));
static SmemContext c;
static void w16(UINTN p, UINT16 v) { arena[p]=v; arena[p+1]=v>>8; }
static void w32(UINTN p, UINT32 v) { w16(p,v); w16(p+2,v>>16); }
static void base(UINT32 version) {
  memset(arena,0,sizeof(arena)); w32(192,1); w32(92,version<<16);
}
static void part(UINT32 index, UINT32 off, UINT16 a, UINT16 b) {
  UINTN t=sizeof(arena)-4096, e=t+32+index*48;
  w32(t,0x434f5424); w32(t+4,1); w32(t+8,index+1);
  w32(e,off); w32(e+4,1024); w16(e+12,a); w16(e+14,b); w32(e+16,32);
  w32(off,0x54525024); w16(off+4,a); w16(off+6,b); w32(off+8,1024);
  w32(off+12,32); w32(off+16,1024);
}
static void part_sized(UINT32 index, UINT32 off, UINT32 size,
                       UINT16 a, UINT16 b) {
  UINTN t=sizeof(arena)-4096, e=t+32+index*48;
  w32(t,0x434f5424); w32(t+4,1); w32(t+8,index+1);
  w32(e,off); w32(e+4,size); w16(e+12,a); w16(e+14,b); w32(e+16,32);
  w32(off,0x54525024); w16(off+4,a); w16(off+6,b); w32(off+8,size);
  w32(off+12,32); w32(off+16,size);
}
static void uncached(UINT32 off, UINT16 item) {
  w32(off+12,64); w16(off+32,0xa5a5); w16(off+34,item);
  w32(off+36,8); w16(off+40,3); w16(off+42,8);
  memcpy(arena+off+56,"hello",5);
}
static CR_STATUS get(UINT32 host, UINT32 item, UINTN expected, UINT32 len) {
  UINT32 size=123; VOID *ptr=(VOID*)1;
  CR_STATUS s=SmemGetItem(&c,host,item,&size,&ptr);
  if (s==CR_SUCCESS) { assert(ptr==arena+expected); assert(size==len); }
  else { assert(ptr==NULL); assert(size==0); }
  return s;
}
int main(void) {
  UINT8 saved[sizeof(arena)]; UINTN e=208+9*16;
  base(11); w32(e,1); w32(e+4,9000); w32(e+8,8);
  assert(SmemInitialize(&c,arena,sizeof(arena),0x80900000)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,9000,8)==CR_SUCCESS);
  assert(get(1,10,0,0)==CR_NOT_FOUND);
  w32(e+12,0x80900003); assert(get(1,9,9000,8)==CR_SUCCESS);
  w32(e+12,0x90900000); assert(get(1,9,0,0)==CR_UNSUPPORTED);
  w32(e+12,0); w32(e+4,0xfffffff0); assert(get(1,9,0,0)==CR_DEVICE_ERROR);
  base(11); part(0,9000,0,1); uncached(9000,9);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(1,9,9056,5)==CR_SUCCESS);
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST); uncached(9000,9);
  part(1,11000,0,1);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  memcpy(saved,arena,sizeof(arena));
  assert(get(2,9,9056,5)==CR_SUCCESS);
  assert(get(1,9,0,0)==CR_NOT_FOUND);
  assert(memcmp(saved,arena,sizeof(arena))==0);
  /* Cached entry has a cacheline-sized header and reverse-growing payload. */
  w32(11000+16,984); w16(11000+992,0xa5a5); w16(11000+994,10);
  w32(11000+996,8); w16(11000+1000,2);
  assert(get(1,10,11984,6)==CR_SUCCESS);
  w32(11000+996,0xffffffff); assert(get(1,10,0,0)==CR_DEVICE_ERROR);
  w32(11000+996,8); w16(11000+1000,9); assert(get(1,10,0,0)==CR_DEVICE_ERROR);
  w16(11000+1000,2); w16(11000+992,0); assert(get(1,10,0,0)==CR_DEVICE_ERROR);
  w16(9000+40,9); assert(get(2,9,0,0)==CR_DEVICE_ERROR);
  w16(9000+40,3); w16(9000+42,65535); assert(get(2,9,0,0)==CR_DEVICE_ERROR);
  base(12); assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  /* Cacheline/cached metadata must not prevent probing or an uncached hit.
   * Validate them only if a lookup actually needs to walk cached entries. */
  uncached(9000,9);
  w32(sizeof(arena)-4096+48,0);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,9056,5)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,10,0,0)==CR_NOT_FOUND);
  w32(9000+16,984);
  assert(get(SMEM_HOST_ANY,9,9056,5)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,10,0,0)==CR_DEVICE_ERROR);
  w32(sizeof(arena)-4096+48,24);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,9056,5)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,10,0,0)==CR_DEVICE_ERROR);
  w32(sizeof(arena)-4096+48,32); w32(9000+16,0xffffffff);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,9056,5)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,10,0,0)==CR_DEVICE_ERROR);
  w32(9000+12,1025);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  /* A published cursor must never point into the 32-byte partition header.
   * Check both initial metadata and a corrupt live update after probing. */
  for (UINT32 n=0;n<32;n++) {
    base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
    w32(9000+12,n);
    assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  }
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  w32(9000+12,24);
  assert(get(SMEM_HOST_ANY,9,0,0)==CR_DEVICE_ERROR);
  w32(9000+12,32); w32(9000+16,24);
  assert(get(SMEM_HOST_ANY,9,0,0)==CR_DEVICE_ERROR);
  part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST); part(1,9504,0,1);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  part(1,11000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  /* Partition headers are consumed through aligned live cursor loads.  An
   * unaligned selected partition must be rejected before those loads. */
  base(12); part(0,9001,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  w32(sizeof(arena)-4096+8,0xffffffff);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  base(13); assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_UNSUPPORTED);
  base(11);
  for (UINTN n=0;n<8400;n++) assert(CR_ERROR(SmemInitialize(&c,arena,n,0)));
  base(11); w32(e,1); w32(e+4,9000); w32(e+8,8); w32(e+12,0x80900000);
  assert(SmemInitialize(&c,arena,sizeof(arena),0x180900000ULL)==CR_SUCCESS);
  assert(get(1,9,9000,8)==CR_SUCCESS);
  w32(e+12,0);
  { VOID *ptr=NULL; assert(SmemGetItem(&c,1,9,NULL,&ptr)==CR_SUCCESS); assert(ptr==arena+9000); }
  w32(204,1); assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_DEVICE_ERROR);
  base(11); assert(SmemInitialize(&c,arena,sizeof(arena),~(UINT64)0-4)==CR_INVALID_PARAMETER);
  base(11); assert(SmemInitialize(&c,arena,sizeof(arena),0x80900001)==CR_INVALID_PARAMETER);
  base(12); part(0,4096,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST); uncached(4096,9);
  w32(sizeof(arena)-4096+80,0x49494953); w16(sizeof(arena)-4096+96,600);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS); assert(c.ItemCount==600);
  assert(get(SMEM_HOST_ANY,9,4152,5)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,599,0,0)==CR_NOT_FOUND);
  assert(get(SMEM_HOST_ANY,600,0,0)==CR_INVALID_PARAMETER);
  /* Linux falls back to 512 when the optional SIII record is absent or
   * invalid, and ignores partition-table entries for unrelated host pairs. */
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  w32(sizeof(arena)-4096+8,2);
  w32(sizeof(arena)-4096+80,0x7fff000); w32(sizeof(arena)-4096+84,0x1000);
  w16(sizeof(arena)-4096+92,3); w16(sizeof(arena)-4096+94,4);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(c.ItemCount==512);
  base(12); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  w32(sizeof(arena)-4096+80,0x49494953); w16(sizeof(arena)-4096+96,0);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(c.ItemCount==512);
  /* Offset-zero records describe the legacy area; Linux skips them before
   * host selection and duplicate checks, even with a nonzero size. */
  base(12); part(0,0,0,1);
  part(1,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST); uncached(9000,9);
  part(2,11000,0,1); uncached(11000,10);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,9056,5)==CR_SUCCESS);
  assert(get(1,10,11056,5)==CR_SUCCESS);
  /* v11 uses the legacy heap, not a v12 global partition record. */
  base(11); part(0,9000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST); w32(9000,0);
  w32(e,1); w32(e+4,12000); w32(e+8,8);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  assert(get(SMEM_HOST_ANY,9,12000,8)==CR_SUCCESS);
  /* v12 global partitions use Linux's uncached private-entry allocator. */
  base(12); part_sized(0,9000,0x4000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { VOID *ptr = NULL;
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,0x2000,&ptr)==CR_SUCCESS);
    assert(ptr == arena + 9000 + 48);
    assert(get(SMEM_HOST_ANY,404,9048,0x2000)==CR_SUCCESS);
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,0x2000,&ptr)==CR_BUSY);
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,405,0x2000,&ptr)==CR_OUT_OF_RESOURCES);
  }
  /* An explicit host routes to its APPS/private partition. Odd sizes retain
   * their requested length while the wire entry rounds payload to 8 bytes. */
  base(12); part_sized(0,9000,0x3000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  part_sized(1,22000,0x1000,SMEM_HOST_APPS,1);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { VOID *ptr = NULL; UINTN before = FenceCount;
    assert(SmemAllocateItem(&c,1,404,7,&ptr)==CR_SUCCESS);
    assert(ptr == arena + 22000 + 48);
    assert(arena[22000+36] == 8 && arena[22000+37] == 0);
    assert(arena[22000+40] == 1 && arena[22000+41] == 0);
    assert(get(1,404,22048,7)==CR_SUCCESS);
    assert(FenceCount > before);
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,7,&ptr)==CR_SUCCESS);
    assert(ptr == arena + 9000 + 48);
  }
  /* A valid cached entry is detected as a duplicate before any write. */
  base(12); part_sized(0,9000,0x1000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  w32(9000+16,32); w16(9000+0x1000-32,0xa5a5);
  w16(9000+0x1000-30,405); w32(9000+0x1000-28,8);
  w16(9000+0x1000-24,1); memcpy(arena+9000+0x1000-40,"guard",5);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { UINT8 saved[sizeof(arena)]; VOID *ptr = NULL;
    memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,405,7,&ptr)==CR_BUSY);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
  }
  /* Corrupt cursors/lists and UINT32 size overflow fail without publishing. */
  base(12); part_sized(0,9000,0x1000,SMEM_GLOBAL_HOST,SMEM_GLOBAL_HOST);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { UINT8 saved[sizeof(arena)]; VOID *ptr = NULL;
    w32(9000+12,31); memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,7,&ptr)==CR_DEVICE_ERROR);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
    w32(9000+12,64); memset(arena+9000+32,0,sizeof(arena)-9032);
    memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,7,&ptr)==CR_DEVICE_ERROR);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
    w32(9000+12,32); memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,404,0xfffffff9U,&ptr)==CR_INVALID_PARAMETER);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
  }
  /* v11 retains the legacy global TOC/free-offset format. */
  base(11); w32(196,12000); w32(200,64);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { VOID *ptr = NULL;
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,9,7,&ptr)==CR_SUCCESS);
    assert(ptr == arena + 12000);
    assert(get(SMEM_HOST_ANY,9,12000,8)==CR_SUCCESS);
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,9,7,&ptr)==CR_BUSY);
  }
  base(11); w32(196,8392); w32(200,64);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { UINT8 saved[sizeof(arena)]; VOID *ptr = NULL;
    memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,9,8,&ptr)==CR_DEVICE_ERROR);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
  }
  /* A v11 private partition also reserves the address range from the legacy
   * heap, even though Host ANY does not select that partition. */
  base(11); part_sized(0,9000,0x1000,SMEM_HOST_APPS,1);
  w32(196,8400); w32(200,0x400);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { UINT8 saved[sizeof(arena)]; VOID *ptr = NULL;
    memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,9,8,&ptr)==CR_DEVICE_ERROR);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
  }
  base(11); w32(196,0xfffffff8U); w32(200,0x20);
  assert(SmemInitialize(&c,arena,sizeof(arena),0)==CR_SUCCESS);
  { UINT8 saved[sizeof(arena)]; VOID *ptr = NULL;
    memcpy(saved,arena,sizeof(arena));
    assert(SmemAllocateItem(&c,SMEM_HOST_ANY,9,8,&ptr)==CR_DEVICE_ERROR);
    assert(ptr == NULL && memcmp(saved,arena,sizeof(arena))==0);
  }
  assert(SmemInitialize(&c,arena,0x4001,0)==CR_INVALID_PARAMETER);
  puts("SMEM fixtures: PASS"); return 0;
}
