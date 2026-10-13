/* HDK mainline pad profile, actual TLMM shared core. MIT */
#include <Library/GpioSdLib.h>
#include <oskal/cr_memory.h>
#define SD_CTL_OFFSET 0x1d6000U
STATIC CONST CR_GPIO_SD_PROFILE mLegacy={.CdPin=92,.FirstPad=211,.PadControlOffset=SD_CTL_OFFSET};
#define SD_PAD_MASK 0xdfffU
STATIC CR_STATUS PadsQualify(CONST GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F){
  if(C==NULL||F==NULL||!C->TlmmBaseAddress||!C->PinCount||C->PinCount>GPIO_PINS_MAX)return CR_NOT_READY;
  if(!CrGpioSdProfileDrivesValid(F))return CR_INVALID_PARAMETER;
  if(C->PinCount<3||F->FirstPad>C->PinCount-3U||!F->PadControlOffset||(F->PadControlOffset&3)||
     C->TlmmBaseAddress>(UINT64)(~(UINTN)0)-F->PadControlOffset-4U)return CR_INVALID_PARAMETER;
  CONST UINT32 Pulls[]={3U<<14,3U<<11,3U<<9},Drives[]={7U<<6,7U<<3,7U};
  for(UINT32 I=0;I<3;++I){CONST GpioPinInfo *P=&C->GpioPins[F->FirstPad+I];CONST GpioConfigRegInfo *R=P->pRegCfg;
    if(P->Reserved||R==NULL||R->Index==0||P->PinAddress!=C->TlmmBaseAddress||R->ControlRegOffset!=F->PadControlOffset||
       R->PullMsk!=Pulls[I]||R->DriveStrengthMsk!=Drives[I]||R->MuxSelMsk||R->OutputEnableMsk||R->InputMsk||R->OutputMsk)
      return CR_INVALID_PARAMETER;
  }
  return CR_SUCCESS;
}
CR_STATUS CrGpioSdQualifyProfile(CONST GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F){
  CR_STATUS S=PadsQualify(C,F);if(S!=CR_SUCCESS)return S;
  if(F->CdPin>=C->PinCount||(F->CdPin>=F->FirstPad&&F->CdPin<F->FirstPad+3))return CR_INVALID_PARAMETER;
  CONST GpioPinInfo *P=&C->GpioPins[F->CdPin];CONST GpioConfigRegInfo *R=P->pRegCfg;
  if(P->Reserved||R==NULL||R->Index!=0||P->pTileInfo==NULL||!P->PinAddress||!R->InputMsk||!R->OutputMsk||!R->MuxSelMsk||!R->OutputEnableMsk||
     !R->PullMsk||!R->DriveStrengthMsk||!(P->FuncMuxMap&1U)||
     C->TlmmBaseAddress>(UINT64)(~(UINTN)0)-P->pTileInfo->TileOffset-F->CdPin*GPIO_TLMM_PIN_STRIDE||
     P->PinAddress!=C->TlmmBaseAddress+P->pTileInfo->TileOffset+F->CdPin*GPIO_TLMM_PIN_STRIDE||
     P->PinAddress>(UINT64)(~(UINTN)0)-R->ControlRegOffset-4U||
     P->PinAddress>(UINT64)(~(UINTN)0)-R->IoRegOffset-4U||
     P->PinAddress>(UINT64)(~(UINTN)0)-R->IntCfgRegOffset-4U)return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
STATIC CR_STATUS Pads(GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F,UINT32 Value){
  UINTN A=(UINTN)C->TlmmBaseAddress+F->PadControlOffset;
  UINT32 Old=CrMmioRead32(A),New=(Old&~SD_PAD_MASK)|(Value&SD_PAD_MASK);
  CrMmioWrite32(A,New);
  return (CrMmioRead32(A)&SD_PAD_MASK)==(New&SD_PAD_MASK)?CR_SUCCESS:CR_DEVICE_ERROR;
}
CR_STATUS CrGpioSdSnapshotProfile(GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F,CR_GPIO_SD_SNAPSHOT *D){
  if(D==NULL)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrGpioSdQualifyProfile(C,F);if(S!=CR_SUCCESS)return S;
  if(D->Snapshot)return D->OriginalCd.PinNumber==F->CdPin?CR_SUCCESS:CR_INVALID_PARAMETER;
  D->OriginalPads=CrMmioRead32((UINTN)C->TlmmBaseAddress+F->PadControlOffset)&SD_PAD_MASK;
  GpioInitConfigParams(&D->OriginalCd,(UINT16)F->CdPin);GpioReadConfig(C,&D->OriginalCd);
  D->OriginalCd.OutputValue=GpioReadOutputVal(C,(UINT16)F->CdPin);
  if(D->OriginalCd.FunctionSel>GPIO_FUNC_MAX||D->OriginalCd.Pull>GPIO_PULL_MAX||
     D->OriginalCd.DriveStrength>GPIO_DRIVE_STRENGTH_MAX||
     (C->EgpioFunction&&D->OriginalCd.FunctionSel==C->EgpioFunction))return CR_UNSUPPORTED;
  D->Snapshot=TRUE;return CR_SUCCESS;
}
CR_STATUS CrGpioSdConfigureCdProfile(GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F,CONST CR_GPIO_SD_SNAPSHOT *D){
  if(D==NULL||!D->Snapshot)return CR_NOT_READY;
  CR_STATUS S=CrGpioSdQualifyProfile(C,F);if(S!=CR_SUCCESS)return S;
  if(D->OriginalCd.PinNumber!=F->CdPin)return CR_INVALID_PARAMETER;
  GpioConfigParams Cd=D->OriginalCd;Cd.FunctionSel=GPIO_FUNC_NORMAL;Cd.OutputEnable=FALSE;
  Cd.Pull=GPIO_PULL_UP;Cd.DriveStrength=GPIO_DRIVE_STRENGTH_2MA;
  return GpioApplyConfigChecked(C,&Cd);
}
CR_STATUS CrGpioSdSetActiveProfile(GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F,BOOLEAN Active){
  if(Active>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrGpioSdQualifyProfile(C,F);if(S!=CR_SUCCESS)return S;
  UINT32 Drive[3];
  for(UINT32 I=0;I<3;++I) {
    UINT32 Ma=F->ActiveDriveMa[I]?F->ActiveDriveMa[I]:16U;
    Drive[I]=Active?(Ma/2U-1U):0U;
  }
  return Pads(C,F,(Drive[0]<<6)|(Drive[1]<<3)|Drive[2]|(3U<<11)|(3U<<9));
}
CR_STATUS CrGpioSdRestoreProfile(GpioDeviceContext *C,CONST CR_GPIO_SD_PROFILE *F,CONST CR_GPIO_SD_SNAPSHOT *D){
  if(D==NULL||!D->Snapshot)return CR_NOT_READY;
  CR_STATUS S=CrGpioSdQualifyProfile(C,F);if(S!=CR_SUCCESS)return S;
  if(D->OriginalCd.PinNumber!=F->CdPin)return CR_INVALID_PARAMETER;
  S=GpioApplyConfigChecked(C,&D->OriginalCd);if(S!=CR_SUCCESS)return S;
  return Pads(C,F,D->OriginalPads);
}

CR_STATUS CrGpioSdPadsQualify(CONST GpioDeviceContext *C){
  if(C==NULL||C->PinCount!=214)return CR_NOT_READY;
  return PadsQualify(C,&mLegacy);
}
CR_STATUS CrGpioSdQualify(CONST GpioDeviceContext *C){
  CR_STATUS S=CrGpioSdPadsQualify(C);return S==CR_SUCCESS?CrGpioSdQualifyProfile(C,&mLegacy):S;
}
CR_STATUS CrGpioSdSnapshot(GpioDeviceContext *C,CR_GPIO_SD_SNAPSHOT *D){
  CR_STATUS S=CrGpioSdQualify(C);return S==CR_SUCCESS?CrGpioSdSnapshotProfile(C,&mLegacy,D):S;
}
CR_STATUS CrGpioSdConfigureCd(GpioDeviceContext *C,CONST CR_GPIO_SD_SNAPSHOT *D){
  CR_STATUS S=CrGpioSdQualify(C);return S==CR_SUCCESS?CrGpioSdConfigureCdProfile(C,&mLegacy,D):S;
}
CR_STATUS CrGpioSdSetActive(GpioDeviceContext *C,BOOLEAN A){
  CR_STATUS S=CrGpioSdQualify(C);return S==CR_SUCCESS?CrGpioSdSetActiveProfile(C,&mLegacy,A):S;
}
CR_STATUS CrGpioSdRestore(GpioDeviceContext *C,CONST CR_GPIO_SD_SNAPSHOT *D){
  CR_STATUS S=CrGpioSdQualify(C);return S==CR_SUCCESS?CrGpioSdRestoreProfile(C,&mLegacy,D):S;
}
CR_STATUS CrGpioSdPadsSnapshot(GpioDeviceContext *C,CR_GPIO_SD_PADS_SNAPSHOT *D){
  if(D==NULL)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrGpioSdPadsQualify(C);if(S!=CR_SUCCESS)return S;
  if(!D->Snapshot){D->OriginalPads=CrMmioRead32((UINTN)C->TlmmBaseAddress+SD_CTL_OFFSET)&SD_PAD_MASK;D->Snapshot=TRUE;}
  return CR_SUCCESS;
}
CR_STATUS CrGpioSdPadsSetActive(GpioDeviceContext *C,BOOLEAN Active){
  if(Active>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrGpioSdPadsQualify(C);if(S!=CR_SUCCESS)return S;
  UINT32 Drive=Active?7U:0U;
  return Pads(C,&mLegacy,(Drive<<6)|(Drive<<3)|Drive|(3U<<11)|(3U<<9));
}
CR_STATUS CrGpioSdPadsRestore(GpioDeviceContext *C,CONST CR_GPIO_SD_PADS_SNAPSHOT *D){
  if(D==NULL||!D->Snapshot)return CR_NOT_READY;
  CR_STATUS S=CrGpioSdPadsQualify(C);if(S!=CR_SUCCESS)return S;
  return Pads(C,&mLegacy,D->OriginalPads);
}
