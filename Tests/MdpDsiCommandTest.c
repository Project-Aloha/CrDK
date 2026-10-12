#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include "../Driver/MDSSCrDxe/MdpDsiCommand.c"
static UINT32 banks[2][256];
static UINT32 active;
#define regs banks[active]
static unsigned accesses;
static EFI_STATUS active_status;
static EFI_STATUS ready_status;
static UINTN address_base (VOID) { return active == 0 ? HOST0 : HOST1; }
static UINT8 buffer[4096];
static unsigned triggers, releases, resets, acquisitions;
static int mode, release_fail;
static BOOLEAN expected_lp = TRUE;
static UINTN capacity=sizeof(buffer);
UINT32 EFIAPI MmioRead32 (UINTN a) { accesses++; assert(a>=address_base() && a<address_base()+sizeof(regs)); return regs[(a-address_base())/4]; }
UINT32 EFIAPI MmioWrite32 (UINTN a, UINT32 v) {
 UINTN r=a-address_base(); accesses++; assert(a>=address_base() && r<sizeof(regs));
 if(r==INTR) { assert(!(v&IRQ_MASK)); regs[r/4]=(regs[r/4]&~(v&IRQ_STATUS)&IRQ_STATUS)|(v&IRQ_MASK); }
 else if(r==RESET) { resets++; if(!v && mode!=3) regs[STATUS/4]=0; }
 else if(r==TRIG_DMA) {
  triggers++; assert(regs[DMA_CTRL/4]==(BIT28 | (expected_lp ? BIT26 : 0))); assert(regs[DMA_BASE/4]==0x12340000); assert(regs[TRIG_CTRL/4]==(4U | (((buffer[2] >> 6) & 3U) << 8)));
  if(mode==0) regs[INTR/4]|=BIT0;
  if(mode==1) regs[INTR/4]|=BIT24;
  if(mode>=2) regs[STATUS/4]=BIT1;
  if(mode==4) regs[INTR/4]|=BIT0;
 } else if(r==0x64 || r==0xbc || r==0xb0 || r==0x08 || r==0x11c) regs[r/4]&=~v;
 else regs[r/4]=v;
 return v;
}
VOID EFIAPI MemoryFence (VOID) {}
UINTN EFIAPI MicroSecondDelay (UINTN n) { return n; }
VOID *EFIAPI SetMem (VOID *p, UINTN n, UINT8 v) { return memset(p,v,n); }
VOID *EFIAPI CopyMem (VOID *p, CONST VOID *q, UINTN n) { return memcpy(p,q,n); }
VOID *EFIAPI WriteBackDataCacheRange (VOID *p, UINTN n) { assert(p==buffer && n<=sizeof(buffer)); return p; }
EFI_STATUS MdpGetActiveController (UINT32 *controller) { *controller=active; return active_status; }
EFI_STATUS MdpGetHostClockTiming (UINT32 *timing) { *timing=0x123; return ready_status; }
EFI_STATUS MdpAcquireCommandBuffer (VOID **p, EFI_PHYSICAL_ADDRESS *d, UINTN *n) { acquisitions++; *p=buffer; *d=0x12340000; *n=capacity; return EFI_SUCCESS; }
EFI_STATUS MdpReleaseCommandBuffer (VOID) { assert(!(regs[STATUS/4]&BUSY)); releases++; return release_fail ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
int main(void) {
 UINT8 p[]={0x11,0x22,0x33,0x44,0x55}; unsigned old, old_releases;
 regs[DMA_CTRL/4]=0x123; regs[TRIG_CTRL/4]=0x456; regs[CLK_CTRL/4]=0x789;
 assert(MdpDsiCommandTransfer(0,2,0x39,p,5,TRUE)==EFI_SUCCESS);
 assert(!memcmp(buffer,(UINT8[]){5,0,0xb9,0xc0,0x11,0x22,0x33,0x44,0x55,0xff,0xff,0xff},12));
 assert(regs[DMA_CTRL/4]==0x123 && regs[TRIG_CTRL/4]==0x456 && regs[CLK_CTRL/4]==0x789 && !regs[INTR/4]);
 expected_lp=FALSE;
 assert(MdpDsiCommandTransfer(0,0,0x15,p,2,FALSE)==EFI_SUCCESS);
 assert(!memcmp(buffer,(UINT8[]){0x11,0x22,0x15,0x80},4));
 expected_lp=TRUE; old=triggers;
 assert(MdpDsiCommandTransfer(0,0,0x06,p,1,TRUE)==EFI_UNSUPPORTED);
 assert(MdpDsiCommandTransfer(2,0,0x05,p,1,TRUE)==EFI_UNSUPPORTED);
 assert(MdpDsiCommandTransfer(0,4,0x05,p,1,TRUE)==EFI_INVALID_PARAMETER);
 assert(MdpDsiCommandTransfer(0,0,0x05,p,2,TRUE)==EFI_INVALID_PARAMETER);
 regs[CTRL/4]=BIT1;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 regs[CTRL/4]=0; assert(triggers==old);
 regs[INTR/4]=BIT0 | BIT24; regs[0x64/4]=7;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_SUCCESS);
 old=acquisitions;
 assert(MdpDsiCommandTransfer(0,0,0x39,p,4093,TRUE)==EFI_BAD_BUFFER_SIZE);
 assert(acquisitions==old);
 capacity=4; assert(MdpDsiCommandTransfer(0,0,0x39,p,5,TRUE)==EFI_BAD_BUFFER_SIZE); capacity=sizeof(buffer);
 mode=1; assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_DEVICE_ERROR);
 mode=2; assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_TIMEOUT); assert(resets==4);
 mode=4; assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_TIMEOUT);
 mode=3; old=releases;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(releases==old && mOwned && mSaved && !(regs[INTR/4]&IRQ_MASK));
 old=triggers; buffer[0]=0xab;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(triggers==old && buffer[0]==0xab);
 old=accesses; old_releases=releases;
 assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(MdpDsiCommandQuiesce(1)==EFI_NOT_READY);
 assert(accesses==old && releases==old_releases);
 active=1; assert(MdpDsiCommandQuiesce(0)==EFI_NOT_READY);
 assert(accesses==old && releases==old_releases); active=0;
 assert(MdpDsiCommandQuiesce(0)==EFI_NOT_READY);
 mode=0; assert(MdpDsiCommandQuiesce(0)==EFI_SUCCESS && !mOwned);
 release_fail=1; assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(mOwned && !mSaved); release_fail=0; assert(MdpDsiCommandQuiesce(0)==EFI_SUCCESS);
 old=accesses;
 assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(accesses==old);
 ready_status=EFI_NOT_READY; old=accesses; old_releases=acquisitions;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(accesses==old && acquisitions==old_releases);
 assert(MdpDsiCommandQuiesce(0)==EFI_SUCCESS && accesses==old);
 ready_status=EFI_SUCCESS;
 active=1;
 assert(MdpDsiCommandTransfer(1,2,0x39,p,5,TRUE)==EFI_SUCCESS);
 assert(!memcmp(buffer,(UINT8[]){5,0,0xb9,0xc0,0x11,0x22,0x33,0x44,0x55,0xff,0xff,0xff},12));
 old=accesses;
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(accesses==old);
 active_status=EFI_NOT_READY;
 assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(accesses==old); active_status=EFI_SUCCESS;
 mode=3; assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 old=accesses;
 assert(MdpDsiCommandQuiesce(0)==EFI_NOT_READY);
 assert(MdpDsiCommandTransfer(0,0,0x05,p,1,TRUE)==EFI_NOT_READY);
 assert(accesses==old);
 ready_status=EFI_NOT_READY; /* retained fencing does not require new-transfer readiness */
 mode=0; assert(MdpDsiCommandQuiesce(1)==EFI_SUCCESS);
 /* Exit admission never frees/revokes storage; SMMU owns the later pass. */
 old=accesses;old_releases=releases;
 assert(MdpDsiCommandFreeze()==EFI_SUCCESS);
 assert(MdpDsiCommandFreeze()==EFI_SUCCESS && accesses==old && releases==old_releases);
 assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_ACCESS_DENIED);
 assert(MdpDsiCommandQuiesce(1)==EFI_ACCESS_DENIED && accesses==old);
 for(unsigned f=0;f<2;f++) {
   mFrozen=FALSE;ready_status=EFI_SUCCESS;mode=3;
   assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_NOT_READY);
   old_releases=releases;if(f)mode=0;
   EFI_STATUS expected=f?EFI_NOT_READY:EFI_DEVICE_ERROR;
   assert(MdpDsiCommandFreeze()==expected && mOwned && releases==old_releases);
   old=accesses;
   assert(MdpDsiCommandFreeze()==expected && accesses==old);
   assert(MdpDsiCommandTransfer(1,0,0x05,p,1,TRUE)==EFI_ACCESS_DENIED);
   /* Test cleanup, not a supported firmware thaw. */
   mFrozen=FALSE;mode=0;assert(MdpDsiCommandQuiesce(1)==EFI_SUCCESS);
 }
 puts("DSI command: packing, VC, padding, validation, error, timeout fence, retained DMA and revoke retry passed");
}
