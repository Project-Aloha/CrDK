/* Copyright (c) 2012-2021 The Linux Foundation.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc.
 * Adapted from Android SM8450 sde_dsc_helper.c and Linux dpu_hw_dsc_1_2.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDscCrLib.h>
#include <oskal/cr_string.h>
#define B(n) (1U << (n))
#define UP(v,d) (((v) + (d) - 1) / (d))
STATIC CONST UINT8 mMin[6][15] = {
 {0,0,1,1,3,3,3,3,3,3,5,5,5,7,13},
 {0,4,5,5,7,7,7,7,7,7,9,9,9,11,17},
 {0,4,5,6,7,7,7,7,7,7,9,9,9,11,15},
 {0,0,1,1,3,3,3,3,3,3,5,5,5,9,12},
 {0,4,5,5,7,7,7,7,7,7,9,9,9,13,16},
 {0,4,5,6,7,7,7,7,7,7,9,9,9,11,15}
};
STATIC CONST UINT8 mMax[6][15] = {
 {4,4,5,6,7,7,7,8,9,10,11,12,13,13,15},
 {4,8,9,10,11,11,11,12,13,14,15,16,17,17,19},
 {7,8,9,10,11,11,11,12,13,13,14,14,15,15,16},
 {4,4,5,6,7,7,7,8,9,10,10,11,11,12,13},
 {8,8,9,10,11,11,11,12,13,14,14,15,15,16,17},
 {7,8,9,10,11,11,11,12,13,13,14,14,15,15,16}
};
STATIC CONST INT8 mBpg[3][15] = {
 {2,0,0,-2,-4,-6,-8,-8,-8,-10,-10,-12,-12,-12,-12},
 {2,0,0,-2,-4,-6,-8,-8,-8,-10,-10,-12,-12,-12,-12},
 {2,0,0,-2,-4,-6,-8,-8,-8,-10,-10,-10,-12,-12,-12}
};
STATIC CONST UINT8 mThresholds[14] =
 {14,28,42,56,70,84,98,105,112,119,121,123,125,126};

CR_STATUS CrMdssDscBuild(CONST CR_MDSS_DSC_PARAMS *P, CR_MDSS_DSC_CONFIG *Out)
{
  CR_MDSS_DSC_CONFIG C;
  UINT32 Index, Type, Bpp, Groups, First, PreMux, MuxBits, FinalScale;
  UINT64 Bits, Size, Hrd, TotalGroups, Denom, Value;
  if (!P || !Out || P->Revision != CR_MDSS_DSC_REVISION ||
      (P->MinorVersion != 1 && P->MinorVersion != 2) ||
      P->ScrVersion > 1 || P->BlockPrediction > 1 ||
      !P->PictureWidth || !P->PictureHeight || !P->SliceWidth || P->SliceHeight < 2 ||
      P->PictureWidth > 65535 || P->PictureHeight > 65535 ||
      P->SliceWidth > 65535 || P->SliceHeight > 65535 ||
      P->PictureWidth % P->SliceWidth || P->PictureHeight % P->SliceHeight)
    return CR_INVALID_PARAMETER;
  if (P->BitsPerComponent == 8 && P->BppQ4 == 128) Type = 0;
  else if (P->BitsPerComponent == 10 && P->BppQ4 == 128) Type = 1;
  else if (P->BitsPerComponent == 10 && P->BppQ4 == 160) Type = 2;
  else return CR_UNSUPPORTED;
  cr_memset(&C, 0, sizeof(C));
  C.Params = *P;
  /* SCR is meaningful only to the DSC1.1 rate-control table. */
  if (P->MinorVersion == 2) C.Params.ScrVersion = 0;
  Bpp = P->BppQ4 >> 4;
  C.SliceCount = P->PictureWidth / P->SliceWidth;
  C.RcModelSize = 8192;
  C.LineBufferDepth = P->BitsPerComponent + 1;
  C.QuantLimit = P->BitsPerComponent == 8 ? 11 : 15;
  C.FlatnessMinQp = P->BitsPerComponent == 8 ? 3 : 7;
  C.FlatnessMaxQp = P->BitsPerComponent == 8 ? 12 : 16;
  C.InitialOffset = Bpp == 8 ? 6144 : 5632;
  C.InitialTransmitDelay = Bpp == 8 ? 512 : 410;
  if (P->MinorVersion == 1) First = P->ScrVersion ? 15 : 12;
  else if (P->SliceHeight < 8) First = 2 * (P->SliceHeight - 1);
  else if (P->SliceHeight < 20) First = 12;
  else if (P->SliceHeight <= 30) First = 13;
  else if (P->SliceHeight < 42) First = 14;
  else First = 15;
  C.FirstLineBpg = First;
  for (Index = 0; Index < 14; ++Index) C.Thresholds[Index] = mThresholds[Index];
  for (Index = 0; Index < 15; ++Index) {
    UINT32 Table = Type + ((P->MinorVersion == 2 || P->ScrVersion) ? 3 : 0);
    C.Ranges[Index].MinQp = mMin[Table][Index];
    C.Ranges[Index].MaxQp = mMax[Table][Index];
    C.Ranges[Index].BpgOffset = mBpg[Type][Index];
  }
  Groups = UP(P->SliceWidth, 3U);
  C.ChunkBytes = UP(P->SliceWidth * Bpp, 8U);
  Hrd = UP((UINT64)C.RcModelSize - C.InitialOffset +
           C.InitialTransmitDelay * Bpp + Groups * First, Bpp);
  if (Hrd < C.InitialTransmitDelay || Hrd - C.InitialTransmitDelay > 65535)
    return CR_BAD_BUFFER_SIZE;
  C.InitialDecodeDelay = (UINT32)Hrd - C.InitialTransmitDelay;
  C.InitialScale = 8 * C.RcModelSize / (C.RcModelSize - C.InitialOffset);
  Bits = 8ULL * C.ChunkBytes * P->SliceHeight;
  TotalGroups = (UINT64)Groups * P->SliceHeight;
  PreMux = 3 * (48 + 4 * P->BitsPerComponent + 4 - 2);
  if (Bits < PreMux) return CR_UNSUPPORTED;
  MuxBits = PreMux - (48 - (UINT32)((Bits - PreMux) % 48));
  C.NflBpg = UP(First * 2048U, P->SliceHeight - 1);
  Value = UP(2048ULL * (C.RcModelSize - C.InitialOffset + MuxBits), TotalGroups);
  if (Value > 65535) return CR_UNSUPPORTED;
  C.SliceBpg = (UINT32)Value;
  C.FinalOffset = C.RcModelSize - C.InitialTransmitDelay * Bpp + MuxBits;
  if (C.FinalOffset >= C.RcModelSize) return CR_UNSUPPORTED;
  FinalScale = 8 * C.RcModelSize / (C.RcModelSize - C.FinalOffset);
  if (FinalScale <= 9) return CR_UNSUPPORTED;
  Denom = (UINT64)(FinalScale - 9) * (C.NflBpg + C.SliceBpg);
  Value = 2048ULL * C.FinalOffset / Denom;
  C.ScaleDecrement = Groups / (C.InitialScale - 8);
  Size = (UINT64)C.ChunkBytes * C.SliceCount;
  if (Value > 65535 || C.ScaleDecrement > 0x7ff || C.ChunkBytes > 65535 ||
      C.NflBpg > 65535 || Size > 65535) return CR_UNSUPPORTED;
  C.ScaleIncrement = (UINT32)Value;
  *Out = C;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDscValidate(CONST CR_MDSS_DSC_CONFIG *C)
{
  CR_MDSS_DSC_CONFIG Expected;
  CR_STATUS S;
  if (!C) return CR_INVALID_PARAMETER;
  S = CrMdssDscBuild(&C->Params, &Expected);
  if (S != CR_SUCCESS) return S;
  return cr_memcmp(C, &Expected, sizeof(Expected)) ? CR_INVALID_PARAMETER : CR_SUCCESS;
}
STATIC VOID Be16(UINT8 *P, UINT32 V) { P[0] = (UINT8)(V >> 8); P[1] = (UINT8)V; }
CR_STATUS CrMdssDscPackPps(CONST CR_MDSS_DSC_CONFIG *C, UINT8 Id, UINT8 *P, UINTN Bytes)
{
  UINT32 I;
  CR_STATUS S;
  if (!P || Bytes < CR_MDSS_DSC_PPS_BYTES ||
      (UINTN)P > (UINTN)-1 - CR_MDSS_DSC_PPS_BYTES)
    return CR_BAD_BUFFER_SIZE;
  if (C && (UINTN)C <= (UINTN)-1 - sizeof(*C) &&
      (UINTN)P < (UINTN)C + sizeof(*C) &&
      (UINTN)C < (UINTN)P + CR_MDSS_DSC_PPS_BYTES)
    return CR_INVALID_PARAMETER;
  S = CrMdssDscValidate(C); if (S != CR_SUCCESS) return S;
  cr_memset(P, 0, CR_MDSS_DSC_PPS_BYTES);
  P[0] = (UINT8)(0x10 | C->Params.MinorVersion); P[1] = Id;
  P[3] = (UINT8)((C->Params.BitsPerComponent << 4) | C->LineBufferDepth);
  P[4] = (UINT8)((C->Params.BppQ4 >> 8) | (C->Params.BlockPrediction << 5) | B(4));
  P[5] = (UINT8)C->Params.BppQ4;
  Be16(P+6,C->Params.PictureHeight); Be16(P+8,C->Params.PictureWidth);
  Be16(P+10,C->Params.SliceHeight); Be16(P+12,C->Params.SliceWidth);
  Be16(P+14,C->ChunkBytes); Be16(P+16,C->InitialTransmitDelay);
  Be16(P+18,C->InitialDecodeDelay); P[21]=(UINT8)C->InitialScale;
  Be16(P+22,C->ScaleIncrement); Be16(P+24,C->ScaleDecrement);
  P[27]=(UINT8)C->FirstLineBpg; Be16(P+28,C->NflBpg); Be16(P+30,C->SliceBpg);
  Be16(P+32,C->InitialOffset); Be16(P+34,C->FinalOffset);
  P[36]=(UINT8)C->FlatnessMinQp; P[37]=(UINT8)C->FlatnessMaxQp;
  Be16(P+38,C->RcModelSize); P[40]=6; P[41]=(UINT8)C->QuantLimit;
  P[42]=(UINT8)C->QuantLimit; P[43]=0x33;
  for (I=0; I<14; ++I) P[44+I]=C->Thresholds[I];
  for (I=0; I<15; ++I) Be16(P+58+2*I,(C->Ranges[I].MinQp<<11)|
      (C->Ranges[I].MaxQp<<6)|((UINT8)C->Ranges[I].BpgOffset & 0x3f));
  return CR_SUCCESS;
}
STATIC CR_STATUS Exact(CR_STATUS S)
{ return S == CR_SUCCESS ? S : CR_ERROR(S) ? S : CR_NOT_READY; }
STATIC CR_STATUS Ready(CR_MDSS_DSC *D) { return Exact(D->Io.Ready(D->Io.Context)); }
STATIC CR_STATUS Read(CR_MDSS_DSC *D, UINT32 O, UINT32 *V)
{ CR_STATUS S=Ready(D); return S==CR_SUCCESS ? Exact(D->Io.Read32(D->Io.Context,O,V)) : S; }
STATIC CR_STATUS Write(CR_MDSS_DSC *D, UINT32 O, UINT32 V)
{ CR_STATUS S=Ready(D); return S==CR_SUCCESS ? Exact(D->Io.Write32(D->Io.Context,O,V)) : S; }
STATIC CR_STATUS Fence(CR_MDSS_DSC *D)
{ CR_STATUS S=Ready(D); return S==CR_SUCCESS ? Exact(D->Io.Fence(D->Io.Context)) : S; }
STATIC CR_STATUS Checked(CR_MDSS_DSC *D, UINT32 O, UINT32 V, UINT32 Mask)
{
  UINT32 Actual; CR_STATUS S=Write(D,O,V); if(S!=CR_SUCCESS)return S;
  S=Fence(D); if(S!=CR_SUCCESS)return S;
  S=Read(D,O,&Actual); if(S!=CR_SUCCESS)return S;
  return (Actual & Mask)==(V & Mask) ? CR_SUCCESS : CR_DEVICE_ERROR;
}
CR_STATUS CrMdssDscInit(CR_MDSS_DSC *D, CONST CR_MDSS_DSC_IO *Io,
                       CONST CR_MDSS_DSC_CONFIG *C, CONST CR_MDSS_DSC_ROUTE *R)
{
  CR_STATUS S; UINT32 Base, Enc, Ctl;
  if (!D || !Io || !R || !Io->Ready || !Io->Read32 || !Io->Write32 || !Io->Fence ||
      R->Encoder>3 || R->Pingpong>5 || R->EncoderCount<1 || R->EncoderCount>4 ||
      R->ActiveSoftSlices<1 || R->ActiveSoftSlices>2 ||
      R->InitialLines>255 || R->SplitPanel>1 || R->Multiplex>1 || R->Video>1)
    return CR_INVALID_PARAMETER;
  S=CrMdssDscValidate(C); if(S!=CR_SUCCESS)return S;
  /* Android supplies num_active_ss_per_enc separately from the full PPS.
     Split-panel/multiplex describe routing; neither changes the PPS picture. */
  if (C->SliceCount != R->ActiveSoftSlices * R->EncoderCount ||
      R->SplitPanel != (R->EncoderCount>1) || (R->Multiplex&&!R->SplitPanel))
    return CR_UNSUPPORTED;
  Base=R->Encoder<2?0x80000:0x81000;
  Enc=(R->Encoder&1)?0x200:0x100; Ctl=(R->Encoder&1)?0xf80:0xf00;
  if(Io->DpuBytes < (UINTN)Base+Ctl+0x10) return CR_BAD_BUFFER_SIZE;
  cr_memset(D,0,sizeof(*D)); D->Io=*Io; D->Config=*C; D->Route=*R;
  D->Base=Base; D->EncoderOffset=Enc; D->ControlOffset=Ctl; D->Initialized=TRUE;
  return CR_SUCCESS;
}
CR_STATUS CrMdssDscStart(CR_MDSS_DSC *D)
{
  CONST CR_MDSS_DSC_CONFIG *C; CONST CR_MDSS_DSC_ROUTE *R;
  UINT32 E,T,I,J,Min,Max,Bpg,V; CR_STATUS S;
  if(!D || !D->Initialized)return CR_INVALID_PARAMETER;
  if(D->Active && !D->CleanupPending)return Ready(D);
  if(D->Touched || D->CleanupPending)return CR_NOT_READY;
  S=Ready(D); if(S!=CR_SUCCESS)return S;
  C=&D->Config;R=&D->Route;E=D->Base+D->EncoderOffset;T=D->Base+D->ControlOffset;
  D->Touched=TRUE; D->CleanupPending=TRUE;
#define PUT(o,v) do { S=Write(D,(o),(v)); if(S!=CR_SUCCESS)return S; } while(0)
  /* Keep the wrapper disabled until thresholds, encoder and mux are complete. */
  S=Checked(D,T+4,0,1);if(S!=CR_SUCCESS)return S;
  PUT(D->Base,(R->SplitPanel?1U:0U)|(R->Multiplex?2U:0U)|(R->ActiveSoftSlices<<7));
  for(I=0;I<4;++I){ V=0;for(J=0;J<4 && I*4+J<14;++J)V|=(UINT32)C->Thresholds[I*4+J]<<(J*8);PUT(E+0x68+I*4,V); }
  for(I=0;I<3;++I){Min=Max=Bpg=0;for(J=0;J<5;++J){
    Min|=(UINT32)C->Ranges[I*5+J].MinQp<<(J*5);
    Max|=(UINT32)C->Ranges[I*5+J].MaxQp<<(J*5);
    Bpg|=((UINT32)(UINT8)C->Ranges[I*5+J].BpgOffset&0x3f)<<(J*6);
  }PUT(E+0x78+I*4,Min);PUT(E+0x84+I*4,Max);PUT(E+0x90+I*4,Bpg);}
  PUT(E,R->InitialLines|(R->Video?B(9):0)|
    (((2400U/R->ActiveSoftSlices/(R->Encoder>=2?2U:1U))-1)<<18));
  PUT(E+0x30,(C->Params.MinorVersion<<28)|(C->Params.BppQ4<<10)|
    (C->Params.BlockPrediction?B(20):0)|(C->LineBufferDepth<<6)|B(4)|C->Params.BitsPerComponent);
  PUT(E+0x34,C->Params.PictureWidth|(C->Params.PictureHeight<<16));
  PUT(E+0x38,C->Params.SliceWidth|(C->Params.SliceHeight<<16));PUT(E+0x3c,C->ChunkBytes);
  PUT(E+0x40,C->InitialTransmitDelay|(C->InitialDecodeDelay<<16));PUT(E+0x44,C->InitialScale);
  PUT(E+0x48,C->ScaleIncrement|(C->ScaleDecrement<<16));PUT(E+0x4c,C->FirstLineBpg);
  PUT(E+0x50,C->NflBpg|(C->SliceBpg<<16));PUT(E+0x54,C->InitialOffset|(C->FinalOffset<<16));
  PUT(E+0x58,0);PUT(E+0x5c,C->FlatnessMinQp|(C->FlatnessMaxQp<<5)|
    ((2U<<(C->Params.BitsPerComponent-8))<<10));PUT(E+0x60,C->RcModelSize);
  PUT(E+0x64,6U|(C->QuantLimit<<8)|(C->QuantLimit<<13)|(3U<<20)|(3U<<24));
  PUT(T,R->Pingpong);
  S=Fence(D);if(S!=CR_SUCCESS)return S;
  V=1U|(C->Params.BitsPerComponent==8?B(11):0)|(R->SplitPanel?B(12):0)|
    (R->Multiplex?B(13):0)|(R->Video?0:B(17));
  S=Checked(D,T+4,V,0x23f01U);if(S!=CR_SUCCESS)return S;
#undef PUT
  D->Active=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDscStop(CR_MDSS_DSC *D)
{
  UINT32 E,T; CR_STATUS S;
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(!D->Touched)return CR_SUCCESS;
  D->CleanupPending=TRUE;E=D->Base+D->EncoderOffset;T=D->Base+D->ControlOffset;
  S=Checked(D,T+4,0,0xffffffffU);if(S!=CR_SUCCESS)return S;
  S=Checked(D,E,0,0xffffffffU);if(S!=CR_SUCCESS)return S;
  S=Checked(D,E+0x30,0,0xffffffffU);if(S!=CR_SUCCESS)return S;
  D->Active=FALSE;D->Touched=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDscUninit(CR_MDSS_DSC *D)
{
  CR_STATUS S;if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  S=CrMdssDscStop(D);if(S!=CR_SUCCESS)return S;
  cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
