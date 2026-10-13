/* Linux dpu_hw_sspp.c flip bits, dpu_hw_ctl.c DMA0 flush, SM8450 DPU8.1.
   SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDpu2dCrLib.h>
#include <oskal/cr_string.h>
#define DMA 0x24000U
#define CTL 0x15000U
#define INTF 0x35000U
#define X (1U<<13)
#define Y (1U<<14)
#define MASK (X|Y)
static CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
static CR_STATUS Ready(CR_MDSS_DPU_FETCH *D){return Exact(D->Io.Ready(D->Io.Context));}
static CR_STATUS Read(CR_MDSS_DPU_FETCH *D,UINT32 O,UINT32 *V){
 UINT32 T;CR_STATUS S=Ready(D);if(S!=CR_SUCCESS)return S;
 S=Exact(D->Io.Read32(D->Io.Context,O,&T));if(S==CR_SUCCESS)*V=T;return S;
}
static CR_STATUS Write(CR_MDSS_DPU_FETCH *D,UINT32 O,UINT32 V){
 CR_STATUS S=Ready(D);return S==CR_SUCCESS?Exact(D->Io.Write32(D->Io.Context,O,V)):S;
}
static CR_STATUS Fence(CR_MDSS_DPU_FETCH *D){CR_STATUS S=Ready(D);return S==CR_SUCCESS?Exact(D->Io.Fence(D->Io.Context)):S;}
static CR_STATUS Delay(CR_MDSS_DPU_FETCH *D){CR_STATUS S=Ready(D);return S==CR_SUCCESS?Exact(D->Io.DelayUs(D->Io.Context,50)):S;}
static UINT32 Bits(UINT32 T){return (T&1?X:0)|(T&2?Y:0);}
static UINT32 Transform(UINT32 V){return (V&X?1:0)|(V&Y?2:0);}
CR_STATUS CrMdssDpuFetchInit(CR_MDSS_DPU_FETCH *D,CONST CR_MDSS_DPU_FETCH_IO *Io,
 CONST CR_MDSS_MODE *M,UINT64 Iova,UINTN Bytes){
 if(!D||!Io||!M||!Io->Context||!Io->Ready||!Io->Read32||!Io->Write32||!Io->Fence||!Io->DelayUs)return CR_INVALID_PARAMETER;
 if(Io->Physical!=0xae01000ULL||M->Soc!=CR_MDSS_SOC_SM8450||M->Controller)return CR_UNSUPPORTED;
 if(Io->Bytes<INTF+0x270||Io->Bytes>CR_MAX_UINT64-Io->Physical)return CR_BAD_BUFFER_SIZE;
 CR_STATUS S=CrMdssValidateMode(M,Bytes);if(S!=CR_SUCCESS)return S;
 UINT64 Size=(UINT64)M->Stride*M->Height;
 if(M->Width>2560||!Iova||(Iova&3)||Iova>CR_MAX_UINT32||Size-1>CR_MAX_UINT32-Iova)return CR_BAD_BUFFER_SIZE;
 if(D->Initialized)return CR_BUSY;
 cr_memset(D,0,sizeof(*D));D->Io=*Io;D->Mode=*M;D->DeviceAddress=Iova;D->Initialized=TRUE;return CR_SUCCESS;
}
static CR_STATUS Active(CR_MDSS_DPU_FETCH *D,UINT32 *Op){
 UINT32 V;CR_STATUS S;
#define CHECK(o,w) do{S=Read(D,(o),&V);if(S!=CR_SUCCESS)return S;if(V!=(w))return CR_NOT_READY;}while(0)
 /* This exact producer uses full-raster DMA0, no source crop/scaler/UBWC,
    LM0 BASE and single DSI0. Reject changed hardware rather than overwrite. */
 CHECK(DMA,D->Mode.Width|(D->Mode.Height<<16));CHECK(DMA+8,0);
 CHECK(DMA+0xc,D->Mode.Width|(D->Mode.Height<<16));CHECK(DMA+0x10,0);
 CHECK(DMA+0x14,(UINT32)D->DeviceAddress);CHECK(DMA+0x18,0);
 CHECK(DMA+0x1c,0);CHECK(DMA+0x20,0);CHECK(DMA+0x24,D->Mode.Stride);
 CHECK(DMA+0x30,0x236ff);CHECK(DMA+0x34,0x03020001);CHECK(DMA+0x170,0);
 CHECK(CTL,((1U<<24)|(1U<<18)));CHECK(CTL+0xfc,1);CHECK(CTL+0xf4,2);
 S=Read(D,INTF,&V);if(S!=CR_SUCCESS)return S;if(!(V&1))return CR_NOT_READY;
 S=Read(D,INTF+0x26c,&V);if(S!=CR_SUCCESS)return S;if(!(V&1))return CR_NOT_READY;
 S=Read(D,DMA+0x38,Op);if(S!=CR_SUCCESS)return S;
 if(!(*Op&(1U<<31))||(*Op&((1U<<0)|(1U<<22))))return CR_UNSUPPORTED;
 if(D->Confirmed&&Transform(*Op)!=D->Transform)return CR_NOT_READY;
#undef CHECK
 return CR_SUCCESS;
}
CR_STATUS CrMdssDpuFetchQuery(CR_MDSS_DPU_FETCH *D,UINT32 *Out){
 if(!D||!D->Initialized||!Out)return CR_INVALID_PARAMETER;
 if(D->CleanupPending)return CR_NOT_READY;
 UINT32 V;CR_STATUS S=Active(D,&V);if(S!=CR_SUCCESS)return S;
 S=Ready(D);if(S==CR_SUCCESS)*Out=Transform(V);return S;
}
CR_STATUS CrMdssDpuFetchSet(CR_MDSS_DPU_FETCH *D,UINT32 T){
 if(!D||!D->Initialized||(T&~CR_MDSS_DPU_REFLECT_MASK))return CR_INVALID_PARAMETER;
 if(D->CleanupPending)return CR_NOT_READY;
 UINT32 Op,V,First;CR_STATUS S=Active(D,&Op);if(S!=CR_SUCCESS)return S;
 S=Read(D,CTL+0x18,&V);if(S!=CR_SUCCESS)return S;if(V)return CR_BUSY;
 if((Op&MASK)==Bits(T)){D->Transform=T;D->Confirmed=TRUE;return Ready(D);}
 S=Read(D,INTF+0xac,&First);if(S!=CR_SUCCESS)return S;
 D->Touched=TRUE;D->CleanupPending=TRUE;
 UINT32 Desired=(Op&~MASK)|Bits(T);
 S=Write(D,DMA+0x38,Desired);if(S!=CR_SUCCESS)return S;
 S=Fence(D);if(S!=CR_SUCCESS)return S;
 S=Read(D,DMA+0x38,&V);if(S!=CR_SUCCESS)return S;if(V!=Desired)return CR_DEVICE_ERROR;
 S=Write(D,CTL+0x18,1U<<11);if(S!=CR_SUCCESS)return S;
 S=Fence(D);if(S!=CR_SUCCESS)return S;
 for(UINT32 Us=0;Us<=250000;Us+=50){
  UINT32 Frame,Flush;S=Read(D,INTF+0x26c,&V);if(S!=CR_SUCCESS)return S;if(!(V&1))return CR_NOT_READY;
  S=Read(D,CTL+0x18,&Flush);if(S!=CR_SUCCESS)return S;
  S=Read(D,INTF+0xac,&Frame);if(S!=CR_SUCCESS)return S;
  if(!Flush&&Frame!=First){
   S=Read(D,DMA+0x38,&V);if(S!=CR_SUCCESS)return S;if(V!=Desired)return CR_DEVICE_ERROR;
   S=Ready(D);if(S!=CR_SUCCESS)return S;
   D->Transform=T;D->Confirmed=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
  }
  if(Us==250000)break;S=Delay(D);if(S!=CR_SUCCESS)return S;
 }
 return CR_TIMEOUT;
}
CR_STATUS CrMdssDpuFetchRetire(CR_MDSS_DPU_FETCH *D){
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 UINT32 V;CR_STATUS S=Read(D,INTF,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
 S=Read(D,INTF+0x26c,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
 S=Read(D,CTL+0x30,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
 S=Ready(D);if(S==CR_SUCCESS)cr_memset(D,0,sizeof(*D));return S;
}

CR_STATUS CrMdssDpuReversePixels(VOID *Cpu,CONST CR_MDSS_MODE *M,UINTN Bytes){
 if(!Cpu||!M||Bytes>(UINTN)-1-(UINTN)Cpu)return CR_INVALID_PARAMETER;
 CR_STATUS S=CrMdssValidateMode(M,Bytes);if(S!=CR_SUCCESS)return S;
 UINT64 Count=(UINT64)M->Width*M->Height;volatile UINT8 *P=Cpu;
 for(UINT64 I=0;I<Count/2;++I){UINT64 J=Count-1-I;
  UINTN A=(UINTN)(I/M->Width)*M->Stride+(UINTN)(I%M->Width)*4;
  UINTN B=(UINTN)(J/M->Width)*M->Stride+(UINTN)(J%M->Width)*4;
  for(UINT32 K=0;K<4;++K){UINT8 V=P[A+K];P[A+K]=P[B+K];P[B+K]=V;}
 }
 return CR_SUCCESS;
}
