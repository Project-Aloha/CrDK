/* Actual native retained-enable and leased-GPIO panel supply adapter.
 * No inferred SWIRE control, bias OFF proof or parent headroom programming.
 * SPDX-License-Identifier: MIT */
#include "MdpPanelPower.h"
#include <Library/BaseMemoryLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC BOOLEAN Overlap(UINTN A,UINTN Al,UINTN B,UINTN Bl){return A<B+Bl&&B<A+Al;}
STATIC BOOLEAN OutputValid(CONST MDP_PANEL_POWER *P,CONST BOOLEAN *O){
  return O&&(UINTN)O<=(UINTN)-1-sizeof(*O)&&
    !Overlap((UINTN)O,sizeof(*O),(UINTN)P,sizeof(*P));
}
STATIC BOOLEAN SameName(CONST CHAR8 *A,CONST CHAR8 *B){
  for(UINT32 I=0;I<MDP_PANEL_POWER_RAIL_NAME;++I){
    if(A[I]!=B[I])return FALSE;if(!A[I])return TRUE;
  }
  return FALSE;
}
STATIC BOOLEAN PinValid(CONST GpioConfigParams *G){
  return G->PinNumber<GPIO_PINS_MAX&&G->OutputEnable<=1&&
    G->FunctionSel<=GPIO_FUNC_MAX&&G->Pull<=GPIO_PULL_MAX&&
    G->DriveStrength<=GPIO_DRIVE_STRENGTH_MAX&&G->OutputValue<=GPIO_VALUE_MAX;
}
STATIC EFI_STATUS ConfigValid(CONST MDP_PANEL_POWER_CONFIG *C){
  if(!C||C->Revision!=MDP_PANEL_POWER_CONFIG_REVISION||C->Size!=sizeof(*C)||
     !C->RailCount||C->RailCount>MDP_PANEL_POWER_MAX_RAILS||C->HasTe>1||
     C->PostBiasDelayUs>120000||!PinValid(&C->Reset)||!C->Reset.OutputEnable||
     C->Reset.FunctionSel!=GPIO_FUNC_NORMAL||C->Reset.OutputValue!=GPIO_VALUE_LOW||
     (C->HasTe&&(!PinValid(&C->Te)||C->Te.OutputEnable||C->Te.PinNumber==C->Reset.PinNumber)))
    return EFI_INVALID_PARAMETER;
  if(C->BiasPolicy!=MdpPanelSwirePermanentRetention||C->BiasSid>15||
     !C->AbBase||!C->IbbBase||C->AbBase==C->IbbBase)return EFI_UNSUPPORTED;
  for(UINT32 I=0;I<C->RailCount;++I){
    CONST MDP_PANEL_POWER_RAIL *R=&C->Rails[I];UINT32 L=0;
    while(L<MDP_PANEL_POWER_RAIL_NAME&&R->Name[L])++L;
    if(!L||L==MDP_PANEL_POWER_RAIL_NAME||R->PostOnDelayUs>120000)return EFI_INVALID_PARAMETER;
    if(R->Policy==MdpPanelRetainFixedVote){
      if(!R->NominalMillivolts||R->NominalMillivolts>0x1fff||
         (R->NominalMode!=2&&R->NominalMode!=3&&R->NominalMode!=4&&
          R->NominalMode!=6&&R->NominalMode!=7))return EFI_INVALID_PARAMETER;
    }else if(R->Policy!=MdpPanelRetainInheritedEnable)return EFI_UNSUPPORTED;
    for(UINT32 J=0;J<I;++J)
      if(SameName(R->Name,C->Rails[J].Name))return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Current(MDP_PANEL_POWER *P){
  if(!P||!P->Initialized||P->Self!=P)return EFI_INVALID_PARAMETER;
  /* Check revision before reading each suffix. Pointer/method drift is not a
   * grant to replace an installed provider after a retained operation. */
  if(!P->Rpmh||P->Rpmh->Revision<EFI_RPMH_CR_PROTOCOL_REVISION||
     P->Rpmh->EnsureRetainedVregEnabled!=P->RetainEnable||
     P->Rpmh->EnsureRetainedVreg!=P->RetainFixed||
     !P->Gpio||P->Gpio->Revision<EFI_GPIO_CR_PROTOCOL_REVISION||
     P->Gpio->AcquirePin!=P->AcquirePin||P->Gpio->SetPinValue!=P->SetPin||
     P->Gpio->ReadPinValue!=P->ReadPin)return EFI_NOT_READY;
  return Exact(P->NativeReady(P->NativeContext));
}
EFI_STATUS MdpPanelPowerInit(MDP_PANEL_POWER *P,EFI_RPMH_CR_PROTOCOL *R,EFI_GPIO_CR_PROTOCOL *G,
    CONST MDP_PANEL_POWER_CONFIG *C,VOID *Context,MDP_PANEL_POWER_READY Ready,MDP_PANEL_POWER_DELAY Delay){
  if(!P||!R||!G||!Context||!Ready||!Delay)return EFI_INVALID_PARAMETER;
  if(P->Initialized)return EFI_ALREADY_STARTED;
  EFI_STATUS S=ConfigValid(C);if(S!=EFI_SUCCESS)return S;
  if(R->Revision<EFI_RPMH_CR_PROTOCOL_REVISION||!R->EnsureRetainedVregEnabled||
     G->Revision<EFI_GPIO_CR_PROTOCOL_REVISION||!G->AcquirePin||!G->SetPinValue||!G->ReadPinValue)
    return EFI_UNSUPPORTED;
  for(UINT32 I=0;I<C->RailCount;++I)
    if(C->Rails[I].Policy==MdpPanelRetainFixedVote&&!R->EnsureRetainedVreg)return EFI_UNSUPPORTED;
  MDP_PANEL_POWER N={0};N.Self=P;N.NativeContext=Context;N.Rpmh=R;N.Gpio=G;
  N.NativeReady=Ready;N.DelayUs=Delay;N.RetainEnable=R->EnsureRetainedVregEnabled;
  N.RetainFixed=R->EnsureRetainedVreg;
  N.AcquirePin=G->AcquirePin;N.SetPin=G->SetPinValue;N.ReadPin=G->ReadPinValue;
  N.Config=*C;N.Initialized=TRUE;*P=N;return EFI_SUCCESS;
}
EFI_STATUS MdpPanelPowerReady(VOID *Context){
  MDP_PANEL_POWER *P=Context;EFI_STATUS S=Current(P);if(S!=EFI_SUCCESS)return S;
  return P->Closing?EFI_NOT_READY:EFI_SUCCESS;
}
EFI_STATUS MdpPanelPowerBindPins(MDP_PANEL_POWER *P,BOOLEAN *Owned){
  if(!P||!OutputValid(P,Owned)||!P->Initialized||P->Self!=P)return EFI_INVALID_PARAMETER;
  *Owned=P->PermanentHold;EFI_STATUS S=Current(P);if(S!=EFI_SUCCESS)return S;
  if(P->Closing)return EFI_NOT_READY;
  for(UINT32 I=0;I<(P->Config.HasTe?2U:1U);++I)if(!P->PinReady[I]){
    S=Current(P);if(S!=EFI_SUCCESS)return S;BOOLEAN PinOwned=P->PinOwned[I];
    S=Exact(P->AcquirePin(P->Gpio,P,I?&P->Config.Te:&P->Config.Reset,&PinOwned));
    /* Never erase ownership already returned by a partial provider effect. */
    if(PinOwned){P->PinOwned[I]=TRUE;P->PermanentHold=TRUE;*Owned=TRUE;}
    if(S!=EFI_SUCCESS)return S;
    if(!P->PinOwned[I])return EFI_NOT_READY;
    P->PinReady[I]=TRUE;
  }
  return EFI_SUCCESS;
}
EFI_STATUS MdpPanelPowerSet(VOID *Context,BOOLEAN On,BOOLEAN *Owned){
  MDP_PANEL_POWER *P=Context;
  if(!P||!OutputValid(P,Owned)||!P->Initialized||P->Self!=P||On>1)return EFI_INVALID_PARAMETER;
  *Owned=P->PermanentHold;
  if(!On){
    P->Closing=TRUE;P->PowerReady=FALSE;
    /* No release RPC is issued without a qualified physical bias OFF fence.
     * Keep GPIO, retained floors and enclosing clocks/access/parent storage. */
    return P->PermanentHold?EFI_UNSUPPORTED:EFI_SUCCESS;
  }
  if(P->Closing)return EFI_NOT_READY;
  EFI_STATUS S=MdpPanelPowerBindPins(P,Owned);if(S!=EFI_SUCCESS)return S;
  for(UINT32 I=0;I<P->Config.RailCount;++I){
    if(!P->RailReady[I]){
      S=Current(P);if(S!=EFI_SUCCESS)return S;
      /* The provider journals an uncertain permanent enable before its RPC.
       * Its API has no withdraw operation; our native owner mirrors that
       * retention before the call, including a failed acknowledgement. */
      P->RailAttempted[I]=TRUE;P->PermanentHold=TRUE;*Owned=TRUE;
      CONST MDP_PANEL_POWER_RAIL *Rail=&P->Config.Rails[I];
      S=Rail->Policy==MdpPanelRetainFixedVote?
        Exact(P->RetainFixed(P->Rpmh,Rail->Name,Rail->NominalMillivolts,Rail->NominalMode)):
        Exact(P->RetainEnable(P->Rpmh,Rail->Name));
      if(S!=EFI_SUCCESS)return S;
      P->RailReady[I]=TRUE;
    }
    if(!P->RailSettled[I]){
      S=Current(P);if(S!=EFI_SUCCESS)return S;
      if(P->Config.Rails[I].PostOnDelayUs){
        S=Exact(P->DelayUs(P->NativeContext,P->Config.Rails[I].PostOnDelayUs));if(S!=EFI_SUCCESS)return S;}
      P->RailSettled[I]=TRUE;
    }
  }
  if(!P->BiasSettled){
    S=Current(P);if(S!=EFI_SUCCESS)return S;
    if(P->Config.PostBiasDelayUs){S=Exact(P->DelayUs(P->NativeContext,P->Config.PostBiasDelayUs));if(S!=EFI_SUCCESS)return S;}
    P->BiasSettled=TRUE;
  }
  /* This means supplier requests/pin leases completed. It is not a PMIC
   * voltage measurement or an observation that the SWIRE bias is already ON. */
  P->PowerReady=TRUE;return EFI_SUCCESS;
}
EFI_STATUS MdpPanelPowerReset(VOID *Context,BOOLEAN High){
  MDP_PANEL_POWER *P=Context;if(!P||High>1)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Current(P);if(S!=EFI_SUCCESS)return S;
  if(!P->PinOwned[0]||!P->PinReady[0]||(P->Closing&&High))return EFI_NOT_READY;
  S=Exact(P->SetPin(P->Gpio,P,P->Config.Reset.PinNumber,(UINT8)High));if(S!=EFI_SUCCESS)return S;
  /* The owner-scoped SetPinValue uses GpioApplyConfigChecked: SUCCESS
   * already verifies mux, output enable and the commanded output latch.
   * ReadPinValue samples the input, which can lag the output transition.
   * Do not abort before the caller's reset settling delay on that sample.
   * Neither the latch acknowledgement nor reset LOW proves bias power OFF. */
  return Current(P);
}
EFI_STATUS MdpPanelPowerClose(MDP_PANEL_POWER *P){
  if(!P||!P->Initialized||P->Self!=P)return EFI_INVALID_PARAMETER;
  if(P->PermanentHold){P->Closing=TRUE;P->PowerReady=FALSE;return EFI_UNSUPPORTED;}
  ZeroMem(P,sizeof(*P));return EFI_SUCCESS;
}

EFI_STATUS MdpPanelPowerR66451Config(MDP_PANEL_POWER_CONFIG *C){
  if(!C)return EFI_INVALID_PARAMETER;
  *C=(MDP_PANEL_POWER_CONFIG){.Revision=MDP_PANEL_POWER_CONFIG_REVISION,.Size=sizeof(*C),.RailCount=3,
    /* Same qualified BOB input vote as the SM8250 UFS/Wi-Fi/touch/SD graph.
     * An enable-only BOB owner would block later child voltage contracts.
     * LDO voltages/modes remain inherited; do not derive BOB headroom from
     * the panel's nominal LDO voltage or switch off its shared supplies. */
    .Rails={{"bobc1",MdpPanelRetainFixedVote,3296,0,6},
            {"ldoa14",MdpPanelRetainInheritedEnable,1800,20000,7},
            {"ldoc11",MdpPanelRetainInheritedEnable,3300,0,7}},
    .Reset={75,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_LOW},
    .Te={66,FALSE,GPIO_FUNC_1,GPIO_PULL_DOWN,GPIO_DRIVE_STRENGTH_2MA,GPIO_VALUE_LOW},.HasTe=TRUE,
    .BiasPolicy=MdpPanelSwirePermanentRetention,.PostBiasDelayUs=20000,.BiasSid=5,.AbBase=0xde00,.IbbBase=0xdc00};
  return ConfigValid(C);
}
