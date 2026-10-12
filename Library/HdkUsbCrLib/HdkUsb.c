/* HDK8245/8350 USB2 fixed-host sequencing.
 * Public Qualcomm Android phy-msm-snps-hs.c / dwc3-msm.c / dwc3/core.c,
 * SM8250 cad09e061ef6cf689a4a1e54d27562e0f042236a; SM8350 equivalents
 * source-qualified by each immutable target. Copyright (c) 2013-2020,
 * The Linux Foundation; Synopsys. SPDX-License-Identifier: GPL-2.0-only */
#include <Library/HdkUsbCrLib.h>
#define B(N) (1U<<(N))
STATIC EFI_STATUS Exact(EFI_STATUS S) {return S==EFI_SUCCESS || EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Ownership(CR_HDK_USB *U,EFI_STATUS S,BOOLEAN Owned,BOOLEAN On) {
  S=Exact(S);
  if(S==EFI_SUCCESS && Owned!=On) {
    /* A malformed provider success without an owner is not authority to
     * release a different consumer's reference or unload this stable token. */
    if(On)U->UncertainOwner=TRUE;return EFI_NOT_READY;
  }
  return S;
}
STATIC EFI_STATUS Read(CR_HDK_USB *U,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 *V) {
  U->LastWindow=W;U->LastOffset=O;
  EFI_STATUS S=Exact(U->Io.Read(U->Io.Context,W,O,V));
  if(S==EFI_SUCCESS)U->LastValue=*V;return S;
}
STATIC EFI_STATUS Write(CR_HDK_USB *U,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 V) {
  U->LastWindow=W;U->LastOffset=O;U->LastValue=V;
  if(W==HdkUsbHs)U->HsTouched=TRUE;else U->Touched=TRUE;
  return Exact(U->Io.Write(U->Io.Context,W,O,V));
}
STATIC EFI_STATUS Update(CR_HDK_USB *U,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 M,UINT32 V) {
  UINT32 Current;EFI_STATUS S=Read(U,W,O,&Current);if(S!=EFI_SUCCESS)return S;
  S=Write(U,W,O,(Current&~M)|(V&M));if(S!=EFI_SUCCESS)return S;
  S=Read(U,W,O,&Current);if(S!=EFI_SUCCESS)return S;
  return (Current&M)==(V&M)?EFI_SUCCESS:EFI_DEVICE_ERROR;
}
STATIC EFI_STATUS Wait(CR_HDK_USB *U,UINT32 O,UINT32 M,UINT32 V,UINT32 Remaining) {
  UINT32 Current;for(;;){EFI_STATUS S=Read(U,HdkUsbController,O,&Current);
    if(S!=EFI_SUCCESS)return S;if((Current&M)==V)return EFI_SUCCESS;
    if(!Remaining)return EFI_TIMEOUT;UINT32 Step=Remaining<10?Remaining:10;
    U->Io.Delay(U->Io.Context,Step);Remaining-=Step;}
}
STATIC EFI_STATUS Pulse(CR_HDK_USB *U,CONST CHAR8 *Name) {
  U->ResetTouched=TRUE;EFI_STATUS S=Exact(U->Io.Reset(U->Io.Context,Name,TRUE));
  if(S!=EFI_SUCCESS)return S;U->Io.Delay(U->Io.Context,100);
  return Exact(U->Io.Reset(U->Io.Context,Name,FALSE));
}
STATIC EFI_STATUS Halt(CR_HDK_USB *U) {
  UINT32 Caps;EFI_STATUS S=Read(U,HdkUsbController,0,&Caps);if(S!=EFI_SUCCESS)return S;
  UINT32 Length=Caps&0xff;if(Length<0x20||Length>0x80||(Length&3))return EFI_DEVICE_ERROR;
  S=Update(U,HdkUsbController,Length,B(0)|B(2),0);if(S!=EFI_SUCCESS)return S;
  return Wait(U,Length+4,B(0),B(0),100000);
}
STATIC EFI_STATUS ConfigureHs(CR_HDK_USB *U) {
  EFI_STATUS S=Pulse(U,U->Port->HsReset);if(S!=EFI_SUCCESS)return S;
#define HS(O,M,V) do {S=Update(U,HdkUsbHs,(O),(M),(V));if(S!=EFI_SUCCESS)return S;}while(0)
  HS(0x94,2,2);HS(0x50,2,2);HS(0x54,0x70,0);HS(0x58,0x20,0x20);
  HS(0xa0,3,2);HS(0x58,0x10,0x10);HS(0x60,1,1);
  for(UINT32 I=0;I<U->Port->HsOverrideCount;I++) {
    CONST CR_HDK_USB_WRITE *W=&U->Port->HsOverrides[I];
    S=Write(U,HdkUsbHs,W->Offset,W->Value);if(S!=EFI_SUCCESS)return S;
    UINT32 V;S=Read(U,HdkUsbHs,W->Offset,&V);if(S!=EFI_SUCCESS)return S;
    /* Public FEMTO PARAM_OVRD_MASK is 0xff. The readl/writel access width
     * remains 32 bits, but upper read lanes are outside this tuning byte. */
    if((V&0xffU)!=W->Value)return EFI_DEVICE_ERROR;if(W->DelayUs)U->Io.Delay(U->Io.Context,W->DelayUs);
  }
  HS(0x5c,1,1);HS(0x64,0xc,0xc);HS(0x3c,1,1);HS(0x50,2,0);HS(0x64,8,0);HS(0x94,2,0);
#undef HS
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ConfigureCore(CR_HDK_USB *U) {
  UINT32 V;EFI_STATUS S=Read(U,HdkUsbController,0xc120,&U->Id);if(S!=EFI_SUCCESS)return S;
  if((U->Id>>16)!=0x5533 && (U->Id>>16)!=0x3331)return EFI_UNSUPPORTED;
  S=Read(U,HdkUsbController,0xc140,&V);if(S!=EFI_SUCCESS)return S;
  if((V&3)!=2)return EFI_UNSUPPORTED;
  U->Revision=U->Id;
  if((U->Id>>16)==0x3331){S=Read(U,HdkUsbController,0xc1a0,&U->Revision);if(S!=EFI_SUCCESS)return S;
    S=Read(U,HdkUsbController,0xc1a4,&U->VersionType);if(S!=EFI_SUCCESS)return S;}
#define CORE(O,M,V) do {S=Update(U,HdkUsbController,(O),(M),(V));if(S!=EFI_SUCCESS)return S;}while(0)
  /* Host, no scaledown/hibernation/scramble bypass; Android DT explicitly
   * disables clock gating. PHY is UTMI8 with USBTRDTIM=9. */
  CORE(0xc110,(3U<<12)|B(1)|B(3)|(3U<<4)|B(6)|B(7),B(12)|B(3));
  CORE(0xc200,B(31)|B(6)|B(8)|B(4)|B(3)|(15U<<10),9U<<10);
  CORE(0xc2c0,B(31)|B(17),0);
  /* Android host hardware automatic retry, DWC_usb31 IP-gap policy. */
  CORE(0xc12c,B(14),B(14));
  if((U->Id>>16)==0x3331 && U->Revision>=0x3137302a)
    CORE(0xc11c,7U<<21,1U<<21);
  if((U->Id>>16)==0x3331 && U->Revision==0x3137302a && U->VersionType==0x67612a2a)
    CORE(0xc60c,B(16),B(16));
  /* DWC3 >=2.90 has the documented FORCE20 clock bit. This is not the
   * SM8450-specific retry policy and never invents it for DWC_usb31 1.90. */
  if((U->Id>>16)==0x5533 && (U->Id&0xffff)>=0x290a)CORE(0xc11c,B(26),B(26));
  CORE(0xf8808,B(8),B(8));U->Io.Delay(U->Io.Context,5);
  CORE(0xf8808,B(0)|B(3),B(0)|B(3));U->Io.Delay(U->Io.Context,5);
  CORE(0xf8808,B(8),0);
  /* Only our ordinary event ring0 is retired. Extra GSI rings are outside
   * this fixed USB2 host implementation; wrapper reset precedes this. */
  CORE(0xc408,B(31)|0xffffU,B(31));
  S=Write(U,HdkUsbController,0xc400,0);if(S!=EFI_SUCCESS)return S;
  S=Write(U,HdkUsbController,0xc404,0);if(S!=EFI_SUCCESS)return S;
  S=Read(U,HdkUsbController,0xc40c,&V);if(S!=EFI_SUCCESS)return S;
  S=Write(U,HdkUsbController,0xc40c,V&0xfffc);if(S!=EFI_SUCCESS)return S;
  S=Read(U,HdkUsbController,0xc40c,&V);if(S!=EFI_SUCCESS)return S;
  if(V&0xfffc)return EFI_DEVICE_ERROR;
  CORE(0xc704,B(31),0);CORE(0xc708,~0U,0);CORE(0xc720,~0U,0);
#undef CORE
  return Halt(U);
}
EFI_STATUS CrHdkUsbInit(CR_HDK_USB *U,CONST CR_HDK_USB_TARGET *T,UINT32 Index,CONST CR_HDK_USB_IO *Io) {
  if(!U||!T||!Io||!Io->Context||!Io->Access||!Io->Floor||!Io->Read||!Io->Write||!Io->Delay||
    !Io->Path||!Io->Domain||!Io->Clock||!Io->Rail||!Io->Reset||!Io->Gpio||!Io->Attach)return EFI_INVALID_PARAMETER;
  if(U->Initialized)return EFI_ALREADY_STARTED;
  if(!T->Ports||T->PortCount!=2||Index>=T->PortCount ||
    !((T->Soc==0x8250 && T->Board==8245)||(T->Soc==0x8350&&T->Board==8350)))return EFI_UNSUPPORTED;
  CONST CR_HDK_USB_PORT *P=&T->Ports[Index];
  if(P->Index!=Index||(!Index&&!Io->AdmitHost)||(Index&&!P->FixedHost)||
    P->Controller!=(Index?0xa800000U:0xa600000U)||P->ControllerBytes<0xcd00||P->ControllerBytes>0x100000||
    P->HsPhy!=(Index?0x88e4000U:0x88e3000U)||P->HsPhyBytes<0xa4||P->HsPhyBytes>0x114||P->StreamId!=(Index?0x20:0))return EFI_UNSUPPORTED;
  if(!P->Gdsc||!P->CoreReset||!P->HsReset||!P->IccProvider||P->CoreClockCount>8||
    !P->CoreClockCount||P->PhyClockCount>8||P->HsPhyClockCount>P->PhyClockCount||
    P->RailCount>5||P->HsRailCount!=3||P->HsRailCount>P->RailCount||
    (P->HsOverrideCount&&!P->HsOverrides)||P->HsOverrideCount>4)return EFI_INVALID_PARAMETER;
  for(UINT32 I=0;I<P->HsOverrideCount;I++){CONST CR_HDK_USB_WRITE *W=&P->HsOverrides[I];
    if((W->Offset&3)||W->Offset<0x6c||W->Offset>0x78||W->Value>255||W->DelayUs>1000)return EFI_INVALID_PARAMETER;}
  for(UINT32 I=0;I<P->CoreClockCount;I++)if(!P->CoreClocks[I].Name)return EFI_INVALID_PARAMETER;
  for(UINT32 I=0;I<P->HsPhyClockCount;I++)if(!P->PhyClocks[I].Name)return EFI_INVALID_PARAMETER;
  for(UINT32 I=0;I<P->HsRailCount;I++)if(!P->Rails[I].Name||!P->Rails[I].Millivolts)return EFI_INVALID_PARAMETER;
  U->Target=T;U->Port=P;U->Io=*Io;U->Initialized=TRUE;U->LastStatus=EFI_NOT_READY;return EFI_SUCCESS;
}
EFI_STATUS CrHdkUsbStart(CR_HDK_USB *U) {
  EFI_STATUS S;if(!U||!U->Initialized)return EFI_NOT_READY;if(U->Closed)return EFI_ACCESS_DENIED;
  if(U->Ready||U->Stage)return EFI_ALREADY_STARTED;
#define STEP(EXPR) do {++U->Stage;S=Exact(EXPR);if(S!=EFI_SUCCESS)goto Failure;S=Exact(U->Io.Access(U->Io.Context));if(S!=EFI_SUCCESS)goto Failure;}while(0)
  STEP(U->Io.Access(U->Io.Context));STEP(U->Io.Floor(U->Io.Context));
  if(!U->Port->Index)STEP(U->Io.AdmitHost(U->Io.Context));
  if(U->Port->Eud){UINT32 E;STEP(Read(U,HdkUsbEud,0,&E));if(E){S=EFI_ACCESS_DENIED;goto Failure;}}
  /* Evaluate callbacks before reading their ownership out parameter. C does
   * not order function arguments, so never combine these in one expression. */
  for(UINT32 I=0;I<2;I++){S=U->Io.Path(U->Io.Context,I,TRUE,&U->Paths[I]);STEP(Ownership(U,S,U->Paths[I],TRUE));}
  S=U->Io.Domain(U->Io.Context,TRUE,&U->Domain);STEP(Ownership(U,S,U->Domain,TRUE));
  for(UINT32 I=0;I<U->Port->CoreClockCount;I++){S=U->Io.Clock(U->Io.Context,
    U->Port->CoreClocks[I].Name,U->Port->CoreClocks[I].RateHz,TRUE,&U->Clocks[I]);STEP(Ownership(U,S,U->Clocks[I],TRUE));}
  for(UINT32 I=0;I<U->Port->HsRailCount;I++){S=U->Io.Rail(U->Io.Context,&U->Port->Rails[I],TRUE,&U->Rails[I]);STEP(Ownership(U,S,U->Rails[I],TRUE));}
  for(UINT32 I=0;I<U->Port->HsPhyClockCount;I++){S=U->Io.Clock(U->Io.Context,
    U->Port->PhyClocks[I].Name,U->Port->PhyClocks[I].RateHz,TRUE,&U->PhyClocks[I]);STEP(Ownership(U,S,U->PhyClocks[I],TRUE));}
  STEP(Pulse(U,U->Port->CoreReset));STEP(ConfigureHs(U));STEP(ConfigureCore(U));
  STEP(U->Io.Attach(U->Io.Context));
  if(U->Port->VbusGpio!=CR_HDK_USB_NO_GPIO){S=U->Io.Gpio(U->Io.Context,0,TRUE,&U->Gpios[0]);STEP(Ownership(U,S,U->Gpios[0],TRUE));}
  if(U->Port->HubGpio!=CR_HDK_USB_NO_GPIO){S=U->Io.Gpio(U->Io.Context,1,TRUE,&U->Gpios[1]);STEP(Ownership(U,S,U->Gpios[1],TRUE));}
  U->Ready=TRUE;U->LastStatus=EFI_SUCCESS;return EFI_SUCCESS;
Failure:
  U->LastStatus=S;return S;
#undef STEP
}
EFI_STATUS CrHdkUsbRelease(CR_HDK_USB *U) {
  EFI_STATUS S;if(!U||!U->Initialized)return EFI_NOT_READY;if(U->Closed||U->UncertainOwner)return EFI_ACCESS_DENIED;
  U->Ready=FALSE;
  /* Caller has disconnected/stopped the child. A reset acknowledgment is
   * required before dropping any GPIO, rail, clock or interconnect owner. */
  if(U->ResetTouched){S=Pulse(U,U->Port->CoreReset);if(S!=EFI_SUCCESS)return S;}
  if(U->HsTouched){S=Pulse(U,U->Port->HsReset);if(S!=EFI_SUCCESS)return S;}
#define DROP(EXPR,OWNED) do {S=Exact(EXPR);if(S!=EFI_SUCCESS)return S;if(OWNED)return EFI_NOT_READY;}while(0)
  for(UINT32 I=2;I>0;I--)if(U->Gpios[I-1])DROP(U->Io.Gpio(U->Io.Context,I-1,FALSE,&U->Gpios[I-1]),U->Gpios[I-1]);
  for(UINT32 I=U->Port->HsPhyClockCount;I>0;I--)if(U->PhyClocks[I-1])DROP(U->Io.Clock(U->Io.Context,
    U->Port->PhyClocks[I-1].Name,U->Port->PhyClocks[I-1].RateHz,FALSE,&U->PhyClocks[I-1]),U->PhyClocks[I-1]);
  for(UINT32 I=U->Port->HsRailCount;I>0;I--)if(U->Rails[I-1])DROP(U->Io.Rail(U->Io.Context,&U->Port->Rails[I-1],FALSE,&U->Rails[I-1]),U->Rails[I-1]);
  for(UINT32 I=U->Port->CoreClockCount;I>0;I--)if(U->Clocks[I-1])DROP(U->Io.Clock(U->Io.Context,
    U->Port->CoreClocks[I-1].Name,U->Port->CoreClocks[I-1].RateHz,FALSE,&U->Clocks[I-1]),U->Clocks[I-1]);
  if(U->Domain)DROP(U->Io.Domain(U->Io.Context,FALSE,&U->Domain),U->Domain);
  for(UINT32 I=2;I>0;I--)if(U->Paths[I-1])DROP(U->Io.Path(U->Io.Context,I-1,FALSE,&U->Paths[I-1]),U->Paths[I-1]);
#undef DROP
  U->Stage=0;U->ResetTouched=U->Touched=U->HsTouched=FALSE;U->LastStatus=EFI_SUCCESS;return EFI_SUCCESS;
}
EFI_STATUS CrHdkUsbFreeze(CR_HDK_USB *U) {
  if(!U||!U->Initialized)return EFI_NOT_READY;U->Closed=TRUE;
  if(!U->Ready)return !U->Stage?EFI_SUCCESS:EFI_NOT_READY;
  EFI_STATUS S=Halt(U);if(S!=EFI_SUCCESS)return S;
  S=Update(U,HdkUsbController,0xc408,B(31),B(31));if(S!=EFI_SUCCESS)return S;
  S=Update(U,HdkUsbController,0xc708,~0U,0);if(S!=EFI_SUCCESS)return S;
  return EFI_SUCCESS;
}
EFI_STATUS CrHdkUsbVerifyStopped(CR_HDK_USB *U) {
  if(!U||!U->Initialized||U->Closed||!U->ResetTouched||!U->HsTouched||!U->Domain)return EFI_NOT_READY;
  for(UINT32 I=0;I<U->Port->CoreClockCount;I++)if(!U->Clocks[I])return EFI_NOT_READY;
  EFI_STATUS S=Exact(U->Io.Access(U->Io.Context));if(S!=EFI_SUCCESS)return S;
  UINT32 V;S=Read(U,HdkUsbController,0,&V);if(S!=EFI_SUCCESS)return S;
  UINT32 Cap=V&255;if(Cap<0x20||Cap>0x80||(Cap&3))return EFI_DEVICE_ERROR;
  CONST UINT32 Offsets[]={Cap,Cap+4,0xc110,0xc704,0xc708,0xc720,0xc400,0xc404,0xc408};
  CONST UINT32 Masks[]={5,1,3U<<12,B(31),~0U,~0U,~0U,~0U,B(31)|0xffffU};
  CONST UINT32 Values[]={0,1,B(12),0,0,0,0,0,B(31)};
  for(UINT32 I=0;I<ARRAY_SIZE(Offsets);I++){
    S=Read(U,HdkUsbController,Offsets[I],&V);if(S!=EFI_SUCCESS)return S;
    if((V&Masks[I])!=Values[I])return EFI_NOT_READY;
  }
  return EFI_SUCCESS;
}
