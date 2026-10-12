/* FocalTech firmware report contract from public Qualcomm Android FTS.
 * Copyright (c) 2012-2019 Focaltech Ltd. Protocol definitions adapted from
 * GPLv2 focaltech_touch at a9d9833cb5121c979b503d612b50595f1b5a28a7.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/FocalTechTouchCrLib.h>
#include <Library/BaseMemoryLib.h>
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS?S:CR_ERROR(S)?S:CR_NOT_READY;}
STATIC BOOLEAN Owned(CONST CR_FTS_TOUCH *T){return T->PinOwned[0]||T->PinOwned[1]||T->RailOwned[0]||T->RailOwned[1];}
CR_STATUS CrFtsInit(CR_FTS_TOUCH *T,CONST CR_TARGET_TOUCH_CONTEXT *D,CONST CR_FTS_IO *Io){
 if(!T||!D||!Io||!Io->AcquirePin||!Io->SetReset||!Io->ReleasePin||!Io->AcquireRail||!Io->ReleaseRail||!Io->Read||!Io->DelayUs||!Io->ArmReport||!Io->TakeReport)
   return CR_INVALID_PARAMETER;
 if(T->Initialized||Owned(T)||T->UncertainOwner)return CR_ALREADY_STARTED;
 if(D->Revision!=1||D->Size!=sizeof(*D)||D->I2cInstance!=14||D->Address!=0x38||D->MaxContacts!=5||
    D->ChipIdHigh!=0x54||D->ChipIdLow!=0x52||D->XMax!=1080||D->YMax!=2340||
    D->PollPeriodUs<10000||D->PollPeriodUs>50000||D->Reset.PinNumber!=38||!D->Reset.OutputEnable||
    D->Reset.FunctionSel!=GPIO_FUNC_NORMAL||D->Reset.Pull!=GPIO_PULL_UP||D->Reset.DriveStrength!=GPIO_DRIVE_STRENGTH_8MA||D->Reset.OutputValue!=GPIO_VALUE_LOW||
    D->Interrupt.PinNumber!=39||D->Interrupt.OutputEnable||D->Interrupt.FunctionSel!=GPIO_FUNC_NORMAL||D->Interrupt.Pull!=GPIO_PULL_UP||D->Interrupt.DriveStrength!=GPIO_DRIVE_STRENGTH_8MA||
    !D->Rails[0].Name||!D->Rails[1].Name||D->Rails[0].Millivolts!=3000||D->Rails[1].Millivolts!=1800||D->Rails[0].Mode!=7||D->Rails[1].Mode!=7)
   return CR_UNSUPPORTED;
 ZeroMem(T,sizeof(*T));T->Target=D;T->Io=*Io;T->Selected=0xff;T->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrFtsStart(CR_FTS_TOUCH *T){
 if(!T||!T->Initialized)return CR_INVALID_PARAMETER;
 if(T->Active||Owned(T)||T->UncertainOwner)return CR_ALREADY_STARTED;
 T->Head=T->Count=0;T->Selected=0xff;T->LastError=CR_SUCCESS;ZeroMem(&T->Physical,sizeof(T->Physical));ZeroMem(&T->Delivered,sizeof(T->Delivered));
 CONST CR_TARGET_TOUCH_CONTEXT *D=T->Target;CR_STATUS S;
 for(UINT32 I=0;I<2;++I){S=Exact(T->Io.AcquirePin(T->Io.Context,T,I?&D->Interrupt:&D->Reset,&T->PinOwned[I]));
   if(S!=CR_SUCCESS)return S;if(!T->PinOwned[I]){T->UncertainOwner=TRUE;return CR_NOT_READY;}}
 S=Exact(T->Io.SetReset(T->Io.Context,T,D->Reset.PinNumber,0));if(S!=CR_SUCCESS)return S;T->Io.DelayUs(T->Io.Context,1000);
 for(UINT32 I=0;I<2;++I){S=Exact(T->Io.AcquireRail(T->Io.Context,&D->Rails[I],&T->RailOwned[I]));
   if(S!=CR_SUCCESS)return S;if(!T->RailOwned[I]){T->UncertainOwner=TRUE;return CR_NOT_READY;}}
 T->Io.DelayUs(T->Io.Context,1000);S=Exact(T->Io.SetReset(T->Io.Context,T,D->Reset.PinNumber,1));if(S!=CR_SUCCESS)return S;
 T->Io.DelayUs(T->Io.Context,200000);
 CR_STATUS Last=CR_UNSUPPORTED;
 for(UINT32 I=0;I<10;++I){UINT8 High=0,Low=0;
   S=Exact(T->Io.Read(T->Io.Context,0xa3,&High,1));
   if(S==CR_SUCCESS)S=Exact(T->Io.Read(T->Io.Context,0x9f,&Low,1));
   Last=S==CR_SUCCESS?CR_UNSUPPORTED:S;
   if(S==CR_SUCCESS&&High==D->ChipIdHigh&&Low==D->ChipIdLow){
     S=Exact(T->Io.ArmReport(T->Io.Context,T,D->Interrupt.PinNumber));
     if(S!=CR_SUCCESS){T->LastError=S;return S;}T->Active=TRUE;return CR_SUCCESS;}
   if(I!=9)T->Io.DelayUs(T->Io.Context,100000);
 }
 T->LastError=Last;return Last;
}
STATIC CR_STATUS Push(CR_FTS_TOUCH *T,CR_FTS_STATE S){
 if(T->Count==CR_FTS_QUEUE_CAPACITY){
   /* A queue overflow cannot invent a click or leave a held pointer. Drop
    * the incomplete sequence and deliver one release at the last position. */
   T->Head=0;T->Count=1;T->Queue[0]=T->Physical;T->Queue[0].Pressed=FALSE;
   T->Physical.Pressed=FALSE;T->Selected=0xff;T->LastError=CR_OUT_OF_RESOURCES;return CR_OUT_OF_RESOURCES;
 }
 T->Queue[(T->Head+T->Count)%CR_FTS_QUEUE_CAPACITY]=S;++T->Count;T->Physical=S;return CR_SUCCESS;
}
VOID CrFtsCancel(CR_FTS_TOUCH *T,CR_STATUS S){
 if(!T||!T->Initialized)return;
 if(T->Physical.Pressed){CR_FTS_STATE Release=T->Physical;Release.Pressed=FALSE;(VOID)Push(T,Release);}
 T->Selected=0xff;T->LastError=S;
}
CR_STATUS CrFtsService(CR_FTS_TOUCH *T){
 if(!T||!T->Initialized)return CR_INVALID_PARAMETER;if(!T->Active)return CR_NOT_READY;
 /* Android FTS reads touch data only from its falling-edge IRQ handler.
    Empty firmware report storage is not evidence of failed chip startup. */
 BOOLEAN Pending=FALSE;CR_STATUS S=Exact(T->Io.TakeReport(T->Io.Context,T,T->Target->Interrupt.PinNumber,&Pending));
 if(S!=CR_SUCCESS){CrFtsCancel(T,S);return S;}if(!Pending)return CR_SUCCESS;
 /* Public FTS reads registers1..32 into logicalB1..B32. B0 is only the
  * command/prefix slot; device-mode register0 is outside this report. */
 UINT8 B[CR_FTS_REPORT_SIZE]={0};S=Exact(T->Io.Read(T->Io.Context,1,B+1,sizeof(B)-1));
 if(S!=CR_SUCCESS){CrFtsCancel(T,S);return S;}
 UINT8 Count=B[2]&15,Active=0,Mask=0;UINT16 X[5]={0},Y[5]={0};
 if(Count>5){CrFtsCancel(T,CR_COMPROMISED_DATA);return CR_COMPROMISED_DATA;}
 for(UINT32 I=0;I<5;++I){UINT32 N=3+6*I;UINT8 Id=B[N+2]>>4,Event=B[N]>>6;
   if(Id>=0xa)break;
   if(Id>=5||(Mask&(1U<<Id))||Event>2){CrFtsCancel(T,CR_COMPROMISED_DATA);return CR_COMPROMISED_DATA;}
   Mask|=(UINT8)(1U<<Id);X[Id]=(UINT16)(((B[N]&15)<<8)|B[N+1]);Y[Id]=(UINT16)(((B[N+2]&15)<<8)|B[N+3]);
   if(Event!=1){if(X[Id]>T->Target->XMax||Y[Id]>T->Target->YMax||!Count){CrFtsCancel(T,CR_COMPROMISED_DATA);return CR_COMPROMISED_DATA;}
     Active|=(UINT8)(1U<<Id);}
 }
 UINT32 ActiveCount=0;for(UINT32 I=0;I<5;++I)if(Active&(1U<<I))++ActiveCount;
 if(ActiveCount!=Count){CrFtsCancel(T,CR_COMPROMISED_DATA);return CR_COMPROMISED_DATA;}
 T->LastError=CR_SUCCESS;
 if(T->Selected<5&&(Active&(1U<<T->Selected))){CR_FTS_STATE New={X[T->Selected],Y[T->Selected],TRUE};
   return New.X!=T->Physical.X||New.Y!=T->Physical.Y?Push(T,New):CR_SUCCESS;}
 if(T->Physical.Pressed){CR_FTS_STATE Release=T->Physical;Release.Pressed=FALSE;S=Push(T,Release);T->Selected=0xff;if(S!=CR_SUCCESS)return S;}
 for(UINT8 I=0;I<5;++I)if(Active&(1U<<I)){T->Selected=I;CR_FTS_STATE New={X[I],Y[I],TRUE};return Push(T,New);}
 return CR_SUCCESS;
}
CR_STATUS CrFtsGetState(CR_FTS_TOUCH *T,CR_FTS_STATE *S){
 if(!T||!S||!T->Initialized)return CR_INVALID_PARAMETER;
 if(T->Count){*S=T->Queue[T->Head];T->Delivered=*S;T->Head=(UINT8)((T->Head+1)%CR_FTS_QUEUE_CAPACITY);--T->Count;return CR_SUCCESS;}
 return T->LastError!=CR_SUCCESS?T->LastError:CR_NOT_READY;
}
CR_STATUS CrFtsStop(CR_FTS_TOUCH *T){
 if(!T||!T->Initialized)return CR_INVALID_PARAMETER;T->Active=FALSE;T->Head=T->Count=0;T->Physical.Pressed=FALSE;T->Selected=0xff;
 /* Success without the contract's ownership witness is not authority to
  * restore/drop supplier resources or free an identity it may still retain. */
 if(T->UncertainOwner)return CR_NOT_READY;
 CR_STATUS S;
 if(T->PinOwned[0]){S=Exact(T->Io.SetReset(T->Io.Context,T,T->Target->Reset.PinNumber,0));if(S!=CR_SUCCESS)return S;T->Io.DelayUs(T->Io.Context,1000);}
 /* Public FTS power_source_ctrl withdraws VDD before VCC_I2C. */
 for(UINT32 I=0;I<2;++I)if(T->RailOwned[I]){S=Exact(T->Io.ReleaseRail(T->Io.Context,&T->Target->Rails[I]));if(S!=CR_SUCCESS)return S;T->RailOwned[I]=FALSE;}
 for(UINT32 I=2;I;--I)if(T->PinOwned[I-1]){S=Exact(T->Io.ReleasePin(T->Io.Context,T,I==2?T->Target->Interrupt.PinNumber:T->Target->Reset.PinNumber));if(S!=CR_SUCCESS)return S;T->PinOwned[I-1]=FALSE;}
 T->LastError=CR_SUCCESS;return CR_SUCCESS;
}
