/* A730 boot wire profile; GPL-2.0-only.
 * Linux a6xx_hfi.c a53261b6, Copyright2017-2018 Linux Foundation.
 * Source table retained; new validation and bounded output. */
#include "Internal.h"
STATIC CONST UINT32 Ddr[12][3]={
      {0x40000000,0x40000000,0x40000000},{0x600002e8,0x600003d0,0x60000008},
      {0x6000068d,0x6000089a,0x60000008},{0x600007f2,0x60000a6e,0x60000008},
      {0x600009e5,0x60000cfd,0x60000008},{0x60000b29,0x60000ea6,0x60000008},
      {0x60001698,0x60001da8,0x60000008},{0x600018d2,0x60002093,0x60000008},
      {0x60001e66,0x600027e6,0x60000008},{0x600027c2,0x6000342f,0x60000008},
      {0x60002e71,0x60003cf5,0x60000008},{0x600030ae,0x60003fe5,0x60000008}};
STATIC BOOLEAN Span(CONST VOID *P,UINTN N) {
  return P && (UINTN)P%4==0 && (UINTN)P<=~(UINTN)0-N;
}
CR_STATUS CrAdrenoHfiValidateBoot(CONST CR_ADRENO_HFI_BOOT_PLAN *B) {
  if(!Span(B,sizeof(*B)))return CR_INVALID_PARAMETER;
  CONST CR_ADRENO_POWER_PLAN *P=&B->Power;
  if(B->Version!=1 || B->Size!=sizeof(*B) || B->ChipId!=CR_ADRENO_POWER_A730_CHIP_ID ||
      B->AcdEnabled || B->IfpcEnabled)return CR_UNSUPPORTED;
  if(P->Version!=1 || P->Size!=sizeof(*P) || P->ChipId!=B->ChipId ||
      P->GxCount<2 || P->GxCount>16 || P->CxCount<2 || P->CxCount>4 ||
      P->InitialGpuIndex==0 || P->InitialGpuIndex>=P->GxCount ||
      P->GmuBootIndex==0 || P->GmuBootIndex>=P->CxCount ||
      (P->GxSecondary!=1 && P->GxSecondary!=2))return CR_COMPROMISED_DATA;
  for(UINT32 N=0;N<16;++N){
    CONST CR_ADRENO_POWER_GX *R=&P->Gx[N];
    if(N>=P->GxCount){if(R->Vote||R->Acd||R->FrequencyKHz)return CR_COMPROMISED_DATA;}
    else if(R->Acd!=0xffffffffU || (N==0 && (R->Vote||R->FrequencyKHz)) ||
        (N && (!R->Vote || R->FrequencyKHz<=P->Gx[N-1].FrequencyKHz)))return CR_COMPROMISED_DATA;
  }
  for(UINT32 N=0;N<4;++N){
    CONST CR_ADRENO_POWER_CX *R=&P->Cx[N];
    if(N>=P->CxCount){if(R->Vote||R->FrequencyKHz)return CR_COMPROMISED_DATA;}
    else if((N==0 && (R->Vote||R->FrequencyKHz)) ||
        (N && (!R->Vote || R->FrequencyKHz<=P->Cx[N-1].FrequencyKHz)))return CR_COMPROMISED_DATA;
    if(!B->BcmAddress[N] || (B->BcmAddress[N]&0xff000003U))return CR_COMPROMISED_DATA;
    for(UINT32 J=0;J<N;++J)if(B->BcmAddress[N]==B->BcmAddress[J])return CR_COMPROMISED_DATA;
  }
  if(P->Cx[P->GmuBootIndex].FrequencyKHz!=200000)return CR_UNSUPPORTED;
  return CR_SUCCESS;
}
STATIC VOID Bandwidth(CR_ADRENO_HFI_COMMAND *M,CONST UINT32 Address[4]) {
    M->Id=3;M->Count=160;M->Words[1]=12;M->Words[2]=1;M->Words[3]=3;M->Words[4]=1;M->Words[5]=7;
    M->Words[6]=Address[3];M->Words[12]=0x40000000;M->Words[18]=0x60000001;
    for(UINT32 N=0;N<3;++N)M->Words[24+N]=Address[N];
    for(UINT32 N=0;N<12;++N)for(UINT32 J=0;J<3;++J)M->Words[32+N*8+J]=Ddr[N][J];
}
CR_STATUS CrAdrenoHfiBuildBootCommand(CONST CR_ADRENO_HFI_BOOT_PLAN *B,UINT32 Index,
    CR_ADRENO_HFI_COMMAND *Out) {
  if(!Span(B,sizeof(*B)) || !Span(Out,sizeof(*Out)) ||
      ((UINTN)Out<(UINTN)B+sizeof(*B) && (UINTN)B<(UINTN)Out+sizeof(*Out)) || Index>=4)
    return CR_INVALID_PARAMETER;
  CR_STATUS S=CrAdrenoHfiValidateBoot(B);if(S!=CR_SUCCESS)return S;
  CR_ADRENO_HFI_COMMAND M={0};
  if(Index==0){
    M.Id=4;M.Count=59;M.Words[1]=B->Power.GxCount;M.Words[2]=B->Power.CxCount;
    for(UINT32 N=0;N<16;++N){M.Words[3+N*3]=B->Power.Gx[N].Vote;
      M.Words[4+N*3]=B->Power.Gx[N].Acd;M.Words[5+N*3]=B->Power.Gx[N].FrequencyKHz;}
    for(UINT32 N=0;N<4;++N){M.Words[51+N*2]=B->Power.Cx[N].Vote;
      M.Words[52+N*2]=B->Power.Cx[N].FrequencyKHz;}
  }else if(Index==1){

    Bandwidth(&M,B->BcmAddress);
  }else if(Index==2){M.Id=14;M.Count=2;}else{M.Id=10;M.Count=1;}
  *Out=M;return CR_SUCCESS;
}

BOOLEAN AdrenoHfiCommandValid(CONST CR_ADRENO_HFI_COMMAND *M) {
  if(M->Words[0] || !((M->Id==4 && M->Count==59)||(M->Id==3 && M->Count==160)||
      (M->Id==14 && M->Count==2)||(M->Id==10 && M->Count==1)))return FALSE;
  for(UINT32 N=M->Count;N<160;++N)if(M->Words[N])return FALSE;
  if(M->Id==14)return M->Words[1]==0;
  if(M->Id==10)return TRUE;
  if(M->Id==4){
    UINT32 Gx=M->Words[1],Cx=M->Words[2];
    if(Gx<2||Gx>16||Cx<2||Cx>4)return FALSE;
    for(UINT32 N=0;N<16;++N){UINT32 Vote=M->Words[3+3*N],Acd=M->Words[4+3*N],Hz=M->Words[5+3*N];
      if(N>=Gx){if(Vote||Acd||Hz)return FALSE;}
      else if(Acd!=0xffffffffU || (N==0&&(Vote||Hz)) ||
          (N&&(!Vote||Hz<=M->Words[5+3*(N-1)])))return FALSE;}
    for(UINT32 N=0;N<4;++N){UINT32 Vote=M->Words[51+2*N],Hz=M->Words[52+2*N];
      if(N>=Cx){if(Vote||Hz)return FALSE;}
      else if((N==0&&(Vote||Hz)) || (N&&(!Vote||Hz<=M->Words[52+2*(N-1)])))return FALSE;}
    return TRUE;
  }
  UINT32 Address[4]={M->Words[24],M->Words[25],M->Words[26],M->Words[6]};
  for(UINT32 N=0;N<4;++N){if(!Address[N]||(Address[N]&0xff000003U))return FALSE;
    for(UINT32 J=0;J<N;++J)if(Address[N]==Address[J])return FALSE;}
  CR_ADRENO_HFI_COMMAND Expected={0};Bandwidth(&Expected,Address);
  for(UINT32 N=0;N<160;++N)if(M->Words[N]!=Expected.Words[N])return FALSE;
  return TRUE;
}
