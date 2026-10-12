/** Actual pixel generator/cache ordering with guard bytes. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Driver/MDSSDpCrDxe/DpLink.h"
static UINT8 Pixels[2+3*68+2];
static UINT8 Source[3+3*72+3];
static UINTN Flushes,Fences;
static BOOLEAN Copying;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN N){
  assert(P==Pixels+2&&N==3*68&&Flushes==Fences);
  const UINT32 Colors[]={0xffffffff,0xffffff00,0xff00ffff,0xff00ff00,0xffff00ff,0xffff0000,0xff0000ff,0xff000000};
  for(UINTN Y=0;Y<3;++Y){for(UINTN X=0;X<16;++X){UINT32 V;memcpy(&V,Pixels+2+Y*68+X*4,4);
      if(Copying)assert(memcmp(Pixels+2+Y*68+X*4,Source+3+Y*72+X*4,4)==0);
      else assert(V==Colors[X/2]);}
    for(UINTN X=64;X<68;++X)assert(!Pixels[2+Y*68+X]);}
  assert(Pixels[0]==0xa5&&Pixels[1]==0xa5&&Pixels[sizeof(Pixels)-1]==0xa5&&Pixels[sizeof(Pixels)-2]==0xa5);
  ++Flushes;return P;
}
VOID EFIAPI MemoryFence(VOID){assert(Flushes==Fences+1);++Fences;}
int main(void){
  memset(Pixels,0xa5,sizeof(Pixels));
  assert(CrDpFillColorBars(NULL,16,3,68)==EFI_INVALID_PARAMETER);
  assert(CrDpFillColorBars(Pixels+2,0,3,68)==EFI_INVALID_PARAMETER);
  assert(CrDpFillColorBars(Pixels+2,16,3,63)==EFI_INVALID_PARAMETER);
  assert(!Flushes&&!Fences&&Pixels[2]==0xa5);
  assert(CrDpFillColorBars(Pixels+2,16,3,68)==EFI_SUCCESS&&Flushes==1&&Fences==1);
  memset(Source,0xde,sizeof(Source));
  for(UINTN Y=0;Y<3;++Y)for(UINTN X=0;X<64;++X)Source[3+Y*72+X]=(UINT8)(Y*67+X);
  UINT8 Saved[sizeof(Source)];memcpy(Saved,Source,sizeof(Saved));
  assert(CrDpCopyFramebuffer(Pixels+2,16,3,68,NULL,72)==EFI_INVALID_PARAMETER);
  assert(CrDpCopyFramebuffer(Pixels+2,16,3,68,Source+3,63)==EFI_INVALID_PARAMETER);
  assert(CrDpCopyFramebuffer(Pixels+2,16,3,68,Pixels+3,68)==EFI_INVALID_PARAMETER);
  assert(CrDpCopyFramebuffer(Pixels+2,16,3,68,(VOID *)(MAX_UINTN-100),72)==EFI_INVALID_PARAMETER);
  assert(CrDpCopyFramebuffer((VOID *)(MAX_UINTN-100),16,3,68,Source+3,72)==EFI_INVALID_PARAMETER);
  assert(Flushes==1&&Fences==1);
  Copying=TRUE;
  assert(CrDpCopyFramebuffer(Pixels+2,16,3,68,Source+3,72)==EFI_SUCCESS&&Flushes==2&&Fences==2);
  assert(memcmp(Saved,Source,sizeof(Saved))==0); /* preserve source pixels/padding/guards */
  puts("DP pixels: actual opaque BGRA bars, bounded BGRX GOP copy, independent padded/unaligned strides, guards and cache-before-fence PASS");
  return 0;
}
