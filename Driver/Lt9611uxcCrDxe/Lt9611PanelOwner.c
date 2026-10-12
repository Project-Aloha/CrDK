/* Source-qualified HDK GPIO9/109 supplies and GPIO107 reset; same physical
 * owner as native LT9611 startup, with exact retained cleanup. MIT */
#include "Lt9611PanelOwner.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC LT9611_PANEL_OWNER *State(EFI_LT9611_PANEL_CR_PROTOCOL *P){return BASE_CR(P,LT9611_PANEL_OWNER,Protocol);}
STATIC EFI_STATUS Ready(LT9611_PANEL_OWNER *P,VOID *Owner){
  if(!P||!P->Initialized||!Owner)return EFI_INVALID_PARAMETER;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  if(P->Owner&&P->Owner!=Owner)return EFI_ACCESS_DENIED;
  return Exact(P->Io.Ready(P->Io.Context));
}
STATIC EFI_STATUS Pin(LT9611_PANEL_OWNER *P,UINT32 I,BOOLEAN High,BOOLEAN Write){
  EFI_STATUS S=Exact(P->Io.Ready(P->Io.Context));
  GpioConfigParams Config={.PinNumber=P->Pins[I],.FunctionSel=GPIO_FUNC_UNCHANGE,
    .OutputEnable=FALSE,.OutputValue=GPIO_VALUE_UNCHANGE};
  if(S==EFI_SUCCESS&&Write){P->Touched[I]=TRUE;S=Exact(P->Gpio->SetIoValue(P->Gpio,P->Pins[I],High?GPIO_VALUE_HIGH:GPIO_VALUE_LOW));}
  if(S==EFI_SUCCESS)S=Exact(P->Io.Ready(P->Io.Context));
  /* TLMM input sampling is distinct from the commanded output latch. Read
   * the complete output configuration so a delayed input sample does not
   * abort the reset sequence before its specified settling interval. This
   * verifies controller state, not the voltage at the physical pin. */
  if(S==EFI_SUCCESS)S=Exact(P->Gpio->GetGpioConfig(P->Gpio,&Config));
  if(S==EFI_SUCCESS&&(!Config.OutputEnable||Config.FunctionSel!=GPIO_FUNC_NORMAL||
    Config.OutputValue!=(High?GPIO_VALUE_HIGH:GPIO_VALUE_LOW)))S=EFI_DEVICE_ERROR;
  if(P->Io.Observe)P->Io.Observe(P->Io.Context,I,High,S);
  if(S==EFI_SUCCESS&&!High)P->Touched[I]=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Current(EFI_LT9611_PANEL_CR_PROTOCOL *This,VOID *Owner){
  if(!This)return EFI_INVALID_PARAMETER;return Ready(State(This),Owner);
}
STATIC EFI_STATUS Begin(LT9611_PANEL_OWNER *P,VOID *Owner){
  EFI_STATUS S=Ready(P,Owner);if(S!=EFI_SUCCESS)return S;
  if(!P->Io.Lock(P->Io.Context))return EFI_NOT_READY;
  S=Ready(P,Owner);if(S!=EFI_SUCCESS)P->Io.Unlock(P->Io.Context);return S;
}
STATIC EFI_STATUS End(LT9611_PANEL_OWNER *P,EFI_STATUS S){P->Io.Unlock(P->Io.Context);return S;}
STATIC EFI_STATUS EFIAPI Acquire(EFI_LT9611_PANEL_CR_PROTOCOL *This,VOID *Owner,BOOLEAN *Owned){
  if(!This||!Owned)return EFI_INVALID_PARAMETER;LT9611_PANEL_OWNER *P=State(This);
  EFI_STATUS S=Begin(P,Owner);if(S!=EFI_SUCCESS)return S;
  P->Owner=Owner;*Owned=TRUE;
  /* Commit exact ownership before any pin mutation. Powered is only restored
     after every actual pin readback; a failed off journal may be retried. */
  BOOLEAN WasPowered=P->Powered;P->Powered=FALSE;
  for(UINT32 I=0;I<3;++I){S=Pin(P,I,TRUE,!WasPowered);if(S!=EFI_SUCCESS)return End(P,S);
    if(!WasPowered&&I==0)MicroSecondDelay(1000);}
  P->Powered=TRUE;return End(P,EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI Release(EFI_LT9611_PANEL_CR_PROTOCOL *This,VOID *Owner,BOOLEAN *Owned){
  if(!This||!Owned)return EFI_INVALID_PARAMETER;LT9611_PANEL_OWNER *P=State(This);
  EFI_STATUS S=Begin(P,Owner);if(S!=EFI_SUCCESS)return S;
  if(!P->Owner){*Owned=FALSE;return End(P,EFI_SUCCESS);}
  *Owned=TRUE;P->Powered=FALSE;
  /* DPU/DSI are fenced by the consumer before this physical reset/power off.
     MCU worker is excluded by the same lock and stays gated while off. */
  for(UINT32 I=3;I-- >0;){
    if(P->RetainSupplies&&I<2)S=Pin(P,I,TRUE,FALSE);
    else S=Pin(P,I,FALSE,TRUE);
    if(S!=EFI_SUCCESS)return End(P,S);
  }
  P->Owner=NULL;*Owned=FALSE;return End(P,EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI Reset(EFI_LT9611_PANEL_CR_PROTOCOL *This,VOID *Owner,BOOLEAN High){
  if(!This||High>1)return EFI_INVALID_PARAMETER;LT9611_PANEL_OWNER *P=State(This);
  EFI_STATUS S=Begin(P,Owner);if(S!=EFI_SUCCESS)return S;
  /* The shared panel cleanup fences reset low before releasing a retained
     partial power-on claim. Low is safe with those exact owned GPIOs even
     when all supply readbacks have not completed; high still needs power. */
  if(P->Owner!=Owner||(High&&!P->Powered))return End(P,EFI_NOT_READY);
  return End(P,Pin(P,2,High,TRUE));
}
STATIC EFI_STATUS EFIAPI Prepare(EFI_LT9611_PANEL_CR_PROTOCOL *This,VOID *Owner,CONST EFI_MDP_DSI_TIMING *T){
  if(!This||!T)return EFI_INVALID_PARAMETER;LT9611_PANEL_OWNER *P=State(This);
  EFI_STATUS S=Begin(P,Owner);if(S!=EFI_SUCCESS)return S;
  if(P->Owner!=Owner||!P->Powered)return End(P,EFI_NOT_READY);
  for(UINT32 I=0;I<3;++I){S=Pin(P,I,TRUE,FALSE);if(S!=EFI_SUCCESS)return End(P,S);}
  S=Exact(P->Io.Prepare(P->Io.Context,T));return End(P,S);
}
EFI_STATUS Lt9611PanelOwnerInitRoute(LT9611_PANEL_OWNER *P,EFI_GPIO_CR_PROTOCOL *G,
 CONST LT9611_PANEL_OWNER_IO *Io,CONST UINT16 Pins[3],BOOLEAN RetainSupplies){
  if(!P||!G||G->Revision<EFI_GPIO_CR_PROTOCOL_REVISION||!G->SetIoValue||!G->GetGpioConfig||
    !Io||!Io->Context||!Io->Ready||!Io->Lock||!Io->Unlock||!Io->Prepare||!Pins||
    Pins[0]>255||Pins[1]>255||Pins[2]>255||Pins[0]==Pins[1]||Pins[0]==Pins[2]||
    Pins[1]==Pins[2]||RetainSupplies>1)return EFI_INVALID_PARAMETER;
  if(P->Initialized)return EFI_ALREADY_STARTED;if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  ZeroMem(P,sizeof(*P));P->Gpio=G;P->Io=*Io;P->Initialized=TRUE;
  CopyMem(P->Pins,Pins,sizeof(P->Pins));P->RetainSupplies=RetainSupplies;
  P->Protocol=(EFI_LT9611_PANEL_CR_PROTOCOL){EFI_LT9611_PANEL_CR_PROTOCOL_REVISION,Current,Acquire,Release,Reset,Prepare};
  for(UINT32 I=0;I<3;++I){EFI_STATUS S=Pin(P,I,TRUE,FALSE);if(S!=EFI_SUCCESS)return S;P->Touched[I]=TRUE;}
  P->Powered=TRUE;return EFI_SUCCESS;
}
EFI_STATUS Lt9611PanelOwnerInit(LT9611_PANEL_OWNER *P,EFI_GPIO_CR_PROTOCOL *G,CONST LT9611_PANEL_OWNER_IO *Io){
  STATIC CONST UINT16 Pins[]={9,109,107};
  return Lt9611PanelOwnerInitRoute(P,G,Io,Pins,FALSE);
}
