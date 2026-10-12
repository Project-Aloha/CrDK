/** Deterministic DP-owned diagnostic pixels. SPDX-License-Identifier: GPL-2.0-only */
#include "DpLink.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>

EFI_STATUS CrDpFillColorBars(VOID *Framebuffer,UINT32 Width,UINT32 Height,UINT32 Stride)
{
  STATIC CONST UINT32 Colors[]={0xFFFFFFFF,0xFFFFFF00,0xFF00FFFF,0xFF00FF00,
                               0xFFFF00FF,0xFFFF0000,0xFF0000FF,0xFF000000};
  UINT8 *Pixels=Framebuffer;
  UINT32 X,Y;
  if(Pixels==NULL||Width==0||Height==0||Width>4096||Height>4096||Stride<Width*4||Stride>0xFFFF)
    return EFI_INVALID_PARAMETER;
  for(Y=0;Y<Height;++Y){
    UINT8 *Row=Pixels+(UINTN)Y*Stride;
    for(X=0;X<Width;++X){UINT32 Color=Colors[(X*ARRAY_SIZE(Colors))/Width];
      /* Native DMA1 unpack uses BGRA little-endian memory. All bars are
       * opaque, including black; no primary GOP pixels or console are read. */
      Row[X*4]=(UINT8)Color;Row[X*4+1]=(UINT8)(Color>>8);
      Row[X*4+2]=(UINT8)(Color>>16);Row[X*4+3]=(UINT8)(Color>>24);
    }
    for(X=Width*4;X<Stride;++X)Row[X]=0;
  }
  WriteBackDataCacheRange(Framebuffer,(UINTN)Stride*Height);
  MemoryFence();
  return EFI_SUCCESS;
}

EFI_STATUS CrDpCopyFramebuffer(VOID *Framebuffer,UINT32 Width,UINT32 Height,
                              UINT32 Stride,CONST VOID *Source,UINT32 SourceStride)
{
  UINTN Bytes,SourceBytes,D=(UINTN)Framebuffer,S=(UINTN)Source;
  UINT32 Y,X;
  if(Framebuffer==NULL||Source==NULL||Width==0||Height==0||Width>4096||Height>4096||
      Stride<Width*4||SourceStride<Width*4||Stride>0xFFFF||SourceStride>0xFFFF)
    return EFI_INVALID_PARAMETER;
  Bytes=(UINTN)Stride*Height;SourceBytes=(UINTN)SourceStride*Height;
  if(D>MAX_UINTN-Bytes||S>MAX_UINTN-SourceBytes||
      (D<S+SourceBytes&&S<D+Bytes))return EFI_INVALID_PARAMETER;
  for(Y=0;Y<Height;++Y){
    UINT8 *Row=(UINT8 *)Framebuffer+(UINTN)Y*Stride;
    CopyMem(Row,(CONST UINT8 *)Source+(UINTN)Y*SourceStride,(UINTN)Width*4);
    for(X=Width*4;X<Stride;++X)Row[X]=0;
  }
  WriteBackDataCacheRange(Framebuffer,Bytes);
  MemoryFence();
  return EFI_SUCCESS;
}
