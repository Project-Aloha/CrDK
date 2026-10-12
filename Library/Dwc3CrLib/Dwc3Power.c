/* Native SM8450 dwc3-qcom prerequisite ordering, shared across operating systems.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/Dwc3PowerCrLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_time.h>
CONST CR_DWC3_POWER_CLOCK CrDwc3PowerClocks[CR_DWC3_POWER_CLOCKS]={
  {"gcc_usb3_0_clkref_en",0},
  {"gcc_cfg_noc_usb3_prim_axi_clk",200000000},
  {"gcc_usb30_prim_master_clk",200000000},
  {"gcc_aggre_usb3_prim_axi_clk",200000000},
  {"gcc_usb30_prim_sleep_clk",0},
  {"gcc_usb30_prim_mock_utmi_clk",19200000}
};
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
STATIC CR_STATUS Check(CR_DWC3_POWER *P){
  return P&&P->Initialized&&CrMutexCheckLevel()==CR_SUCCESS?CR_SUCCESS:CR_INVALID_PARAMETER;
}
STATIC CR_STATUS Access(CR_DWC3_POWER *P){return Exact(P->Io.Access(P->Io.Context));}
STATIC CR_STATUS Acquired(CR_STATUS S,BOOLEAN Got,BOOLEAN *Retained){
  *Retained=Got?TRUE:FALSE;
  /* An invalid provider success cannot certify that no mutation occurred. */
  if(Got>1 || (S==CR_SUCCESS&&!Got)){*Retained=TRUE;return CR_DEVICE_ERROR;}
  return Exact(S);
}
CR_STATUS CrDwc3PowerInit(CR_DWC3_POWER *P,CONST CR_DWC3_POWER_IO *Io){
  if(!P||!Io||!Io->Context||!Io->Access||!Io->Path||!Io->Domain||!Io->Clock||
     !Io->Reset||!Io->UtmiDivider||CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(P->Initialized)return CR_ALREADY_STARTED;
  *P=(CR_DWC3_POWER){.Io=*Io,.Initialized=TRUE};return CR_SUCCESS;
}
STATIC CR_STATUS ContinueAcquire(CR_DWC3_POWER *P){
  CR_STATUS S;
  /* Advance only after exact acknowledgement. A retained uncertain owner
     remains recorded while the same absolute callback is retried. */
  while(P->AcquirePhase<6U+CR_DWC3_POWER_CLOCKS){
    UINT32 Phase=P->AcquirePhase;
    if(Phase==3)cr_sleep(1000);
    S=Access(P);if(S!=CR_SUCCESS)return S;
    BOOLEAN *Retained=NULL,Owned=FALSE,Prior=FALSE;
    if(Phase<CR_DWC3_POWER_PATHS){
      Retained=&P->Paths[Phase];Prior=Owned=*Retained;
      S=P->Io.Path(P->Io.Context,Phase,TRUE,Phase==0?(P->SuperSpeed?1000000:240000):0,
        Phase==0?(P->SuperSpeed?2500000:700000):40000,&Owned);
    }else if(Phase==2){P->ResetTouched=TRUE;S=Exact(P->Io.Reset(P->Io.Context,TRUE));}
    else if(Phase==3)S=Exact(P->Io.Reset(P->Io.Context,FALSE));
    else if(Phase==4){Retained=&P->Domain;Prior=Owned=*Retained;S=P->Io.Domain(P->Io.Context,TRUE,&Owned);}
    else if(Phase==5)S=Exact(P->Io.UtmiDivider(P->Io.Context));
    else{
      UINT32 I=Phase-6;Retained=&P->Clocks[I];Prior=Owned=*Retained;
      S=P->Io.Clock(P->Io.Context,CrDwc3PowerClocks[I].Name,CrDwc3PowerClocks[I].RateHz,TRUE,&Owned);
    }
    if(Retained){S=Acquired(S,Owned,Retained);if(Prior)*Retained=TRUE;}
    if(S!=CR_SUCCESS)return S;++P->AcquirePhase;
  }
  S=Access(P);if(S==CR_SUCCESS){P->Ready=TRUE;P->AcquirePending=FALSE;}return S;
}
CR_STATUS CrDwc3PowerAcquire(CR_DWC3_POWER *P,BOOLEAN SuperSpeed){
  CR_STATUS S=Check(P);if(S!=CR_SUCCESS)return S;
  if(SuperSpeed>1)return CR_INVALID_PARAMETER;
  if(P->Started)return CR_ALREADY_STARTED;
  S=Access(P);if(S!=CR_SUCCESS)return S;
  P->Started=TRUE;P->SuperSpeed=SuperSpeed;P->AcquirePhase=0;P->AcquirePending=TRUE;
  return ContinueAcquire(P);
}
CR_STATUS CrDwc3PowerResume(CR_DWC3_POWER *P,BOOLEAN SuperSpeed){
  CR_STATUS S=Check(P);if(S!=CR_SUCCESS)return S;
  if(SuperSpeed>1||SuperSpeed!=P->SuperSpeed)return CR_INVALID_PARAMETER;
  if(!P->Started||!P->AcquirePending||P->Ready)return CR_NOT_READY;
  if(P->AcquirePhase>6U+CR_DWC3_POWER_CLOCKS)return CR_DEVICE_ERROR;
  return ContinueAcquire(P);
}
CR_STATUS CrDwc3PowerQuiesce(CR_DWC3_POWER *P){
  CR_STATUS S=Check(P);if(S!=CR_SUCCESS)return S;
  P->Ready=FALSE;P->AcquirePending=FALSE;
  if(!P->ResetTouched)return CR_SUCCESS;
  S=Access(P);if(S!=CR_SUCCESS)return S;
  S=Exact(P->Io.Reset(P->Io.Context,TRUE));if(S!=CR_SUCCESS)return S;
  cr_sleep(10);
  S=Access(P);if(S!=CR_SUCCESS)return S;
  S=Exact(P->Io.Reset(P->Io.Context,FALSE));if(S!=CR_SUCCESS)return S;
  P->ResetTouched=FALSE;return CR_SUCCESS;
}
CR_STATUS CrDwc3PowerRelease(CR_DWC3_POWER *P){
  CR_STATUS S=Check(P);if(S!=CR_SUCCESS)return S;
  if(P->Ready||P->ResetTouched)return CR_BUSY;
  for(UINT32 I=CR_DWC3_POWER_CLOCKS;I-->0;)if(P->Clocks[I]){
    S=Access(P);if(S!=CR_SUCCESS)return S;BOOLEAN Owned=TRUE;
    S=Exact(P->Io.Clock(P->Io.Context,CrDwc3PowerClocks[I].Name,0,FALSE,&Owned));
    if(S!=CR_SUCCESS)return S;if(Owned)return CR_DEVICE_ERROR;P->Clocks[I]=FALSE;
  }
  if(P->Domain){
    S=Access(P);if(S!=CR_SUCCESS)return S;BOOLEAN Owned=TRUE;
    S=Exact(P->Io.Domain(P->Io.Context,FALSE,&Owned));
    if(S!=CR_SUCCESS)return S;if(Owned)return CR_DEVICE_ERROR;P->Domain=FALSE;
  }
  for(UINT32 I=CR_DWC3_POWER_PATHS;I-->0;)if(P->Paths[I]){
    S=Access(P);if(S!=CR_SUCCESS)return S;BOOLEAN Owned=TRUE;
    S=Exact(P->Io.Path(P->Io.Context,I,FALSE,0,0,&Owned));
    if(S!=CR_SUCCESS)return S;if(Owned)return CR_DEVICE_ERROR;P->Paths[I]=FALSE;
  }
  P->Started=P->SuperSpeed=FALSE;P->AcquirePhase=0;return CR_SUCCESS;
}
CR_STATUS CrDwc3PowerUninit(CR_DWC3_POWER *P){
  CR_STATUS S=Check(P);if(S!=CR_SUCCESS)return S;
  if(P->Started||P->Ready||P->ResetTouched||P->Domain)return CR_BUSY;
  for(UINT32 I=0;I<CR_DWC3_POWER_CLOCKS;++I)if(P->Clocks[I])return CR_BUSY;
  for(UINT32 I=0;I<CR_DWC3_POWER_PATHS;++I)if(P->Paths[I])return CR_BUSY;
  P->Initialized=FALSE;return CR_SUCCESS;
}
