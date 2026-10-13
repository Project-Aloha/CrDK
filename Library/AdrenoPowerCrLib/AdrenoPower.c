/* Bounded A730 ARC/HFI records. SPDX-License-Identifier: GPL-2.0-only
 * Format/selection source: Linux a6xx_gmu.c and a6xx_hfi.c, Copyright(c)
 * 2017-2019 The Linux Foundation; inspected a53261b60834ce617cc4f3c7723dda8285c59789.
 * New validation preserves original corner indices and mainline saturation.
 */
#include <Library/AdrenoPowerCrLib.h>

STATIC BOOLEAN Span(CONST VOID *P, UINTN Size, UINTN Align) {
  return P!=NULL && (UINTN)P%Align==0 && (UINTN)P<=~(UINTN)0-Size;
}
STATIC BOOLEAN Overlap(CONST VOID *A, UINTN As, CONST VOID *B, UINTN Bs) {
  return (UINTN)A<(UINTN)B+Bs && (UINTN)B<(UINTN)A+As;
}
STATIC BOOLEAN Name(CONST CHAR8 *Actual, CONST CHAR8 *Expected) {
  UINT32 I=0;
  for(;Expected[I]!=0;++I) if(Actual[I]!=Expected[I]) return FALSE;
  for(;I<9;++I) if(Actual[I]!=0) return FALSE;
  return TRUE;
}
STATIC BOOLEAN Rail(CONST CR_RPMH_ARC_RESOURCE *R, CONST CHAR8 *Expected) {
  if(!Name(R->Name,Expected) || R->Kind!=CrRpmhArcPower ||
      (R->Address&0xffff0003U)!=0x00030000U ||
      R->LevelCount<2 || R->LevelCount>CR_RPMH_ARC_MAX_LEVELS || R->Levels[0]!=0)
    return FALSE;
  for(UINT32 I=1;I<R->LevelCount;++I)
    if(R->Levels[I]==0 || R->Levels[I]<R->Levels[I-1]) return FALSE;
  for(UINT32 I=R->LevelCount;I<CR_RPMH_ARC_MAX_LEVELS;++I)
    if(R->Levels[I]!=0) return FALSE;
  return TRUE;
}
STATIC BOOLEAN Opps(CONST CR_ADRENO_POWER_OPP *Rows, UINT32 Count, UINT32 Capacity) {
  if(Count<2 || Count>Capacity) return FALSE;
  for(UINT32 I=0;I<Capacity;++I) {
    CONST CR_ADRENO_POWER_OPP *R=&Rows[I];
    if(R->Reserved) return FALSE;
    if(I==0 || I>=Count) {
      if(R->FrequencyHz || R->Level) return FALSE;
    } else if(R->FrequencyHz<=Rows[I-1].FrequencyHz || R->FrequencyHz%1000 ||
              R->FrequencyHz/1000>0xffffffffU || R->Level==0 ||
              R->Level>0xffffU || R->Level<Rows[I-1].Level) return FALSE;
  }
  return TRUE;
}
STATIC CR_STATUS Vote(CONST CR_RPMH_ARC_RESOURCE *Pri,
    CONST CR_RPMH_ARC_RESOURCE *Sec, UINT32 Level, UINT32 *Result) {
  UINT32 P=0,S=0;
  while(P<Pri->LevelCount && Pri->Levels[P]<Level) ++P;
  if(P==Pri->LevelCount) return CR_UNSUPPORTED;
  while(S+1<Sec->LevelCount && Sec->Levels[S]<Level) ++S;
  *Result=((UINT32)Pri->Levels[P]<<16)|(S<<8)|P;
  return CR_SUCCESS;
}
CR_STATUS CrAdrenoPowerBuild(CONST CR_ADRENO_POWER_PROFILE *Profile,
    CONST CR_RPMH_ARC_RESOURCE *Gfx, CONST CR_RPMH_ARC_RESOURCE *Cx,
    CONST CR_RPMH_ARC_RESOURCE *Mx, CONST CR_RPMH_ARC_RESOURCE *Gmxc,
    CR_ADRENO_POWER_PLAN *Plan) {
  CONST CR_RPMH_ARC_RESOURCE *Rails[4]={Gfx,Cx,Mx,Gmxc};
  UINT32 RailCount=Gmxc?4:3;
  if(!Span(Profile,sizeof(*Profile),8) || !Span(Plan,sizeof(*Plan),4) ||
      Overlap(Profile,sizeof(*Profile),Plan,sizeof(*Plan))) return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<RailCount;++I) {
    if(!Span(Rails[I],sizeof(*Rails[I]),4) ||
        Overlap(Rails[I],sizeof(*Rails[I]),Plan,sizeof(*Plan))) return CR_INVALID_PARAMETER;
  }
  if(Profile->Version!=CR_ADRENO_POWER_VERSION ||
      Profile->ChipId!=CR_ADRENO_POWER_A730_CHIP_ID) return CR_UNSUPPORTED;
  if(!Opps(Profile->Gpu,Profile->GpuCount,CR_ADRENO_POWER_MAX_GX) ||
      !Opps(Profile->Gmu,Profile->GmuCount,CR_ADRENO_POWER_MAX_CX) ||
      Profile->InitialGpuIndex==0 || Profile->InitialGpuIndex>=Profile->GpuCount ||
      Profile->GmuBootIndex==0 || Profile->GmuBootIndex>=Profile->GmuCount ||
      Profile->GmuBootHz!=Profile->Gmu[Profile->GmuBootIndex].FrequencyHz ||
      Profile->HubBootHz==0 || Profile->HubBootHz%1000) return CR_INVALID_PARAMETER;
  if(!Rail(Gfx,"gfx.lvl") || !Rail(Cx,"cx.lvl") || !Rail(Mx,"mx.lvl") ||
      (Gmxc && !Rail(Gmxc,"gmxc.lvl"))) return CR_COMPROMISED_DATA;
  for(UINT32 I=0;I<RailCount;++I)
    for(UINT32 J=0;J<I;++J)
      if(Rails[I]->Address==Rails[J]->Address) return CR_COMPROMISED_DATA;
  CR_ADRENO_POWER_PLAN Copy={0};
  Copy.Version=CR_ADRENO_POWER_VERSION; Copy.Size=sizeof(Copy);
  Copy.ChipId=Profile->ChipId; Copy.GxCount=Profile->GpuCount; Copy.CxCount=Profile->GmuCount;
  Copy.InitialGpuIndex=Profile->InitialGpuIndex; Copy.GmuBootIndex=Profile->GmuBootIndex;
  Copy.GxSecondary=Gmxc?CR_ADRENO_POWER_SECONDARY_GMXC:CR_ADRENO_POWER_SECONDARY_MX;
  CONST CR_RPMH_ARC_RESOURCE *Secondary=Gmxc?Gmxc:Mx;
  for(UINT32 I=0;I<Copy.GxCount;++I) {
    CR_STATUS S=Vote(Gfx,Secondary,Profile->Gpu[I].Level,&Copy.Gx[I].Vote);
    if(S!=CR_SUCCESS) return S;
    Copy.Gx[I].FrequencyKHz=(UINT32)(Profile->Gpu[I].FrequencyHz/1000);
    Copy.Gx[I].Acd=0xffffffffU;
  }
  for(UINT32 I=0;I<Copy.CxCount;++I) {
    CR_STATUS S=Vote(Cx,Mx,Profile->Gmu[I].Level,&Copy.Cx[I].Vote);
    if(S!=CR_SUCCESS) return S;
    Copy.Cx[I].FrequencyKHz=(UINT32)(Profile->Gmu[I].FrequencyHz/1000);
  }
  *Plan=Copy;
  return CR_SUCCESS;
}
