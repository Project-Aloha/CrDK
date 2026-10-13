/* Standard SDHCI + source-qualified Qualcomm v5 power/clock behavior. MIT */
#include <Library/SdhciMsmLib.h>
#include <oskal/cr_string.h>
#define BIT_(N) (1U<<(N))
STATIC BOOLEAN Allowed(UINT32 O,UINT8 W,BOOLEAN Write){
  if(Write&&(O==0xfe||O==0x24||O==0x40||O==0x44||O==0x21c||O==0x240||(O>=0x10&&O<0x20)))return FALSE;
  if(W==1)return O==0x20||O==0x22||(O>=0x28&&O<=0x2b)||O==0x2e||O==0x2f;
  if(W==2)return O==4||O==6||O==0xc||O==0xe||O==0x20||O==0x2c||
    (O>=0x30&&O<=0x3e&&!(O&1))||O==0xfe;
  if(W==4)return O==8||(O>=0x10&&O<=0x24&&!(O&3))||O==0x30||O==0x38||O==0x40||O==0x44||
    O==0x200||O==0x20c||O==0x21c||(O>=0x240&&O<=0x24c&&!(O&3))||O==0x250;
  return FALSE;
}
STATIC CR_STATUS Check(CR_SDHC *C,UINT32 O,UINT8 W,BOOLEAN Write){
  if(C==NULL||!C->Initialized)return CR_NOT_READY;
  if(!Allowed(O,W,Write)||O>C->Io.MappingSize||W>C->Io.MappingSize-O)return CR_INVALID_PARAMETER;
  UINT32 Window=O>=CR_SDHC_POWER_BEGIN?CR_SDHC_WINDOW_POWER:CR_SDHC_WINDOW_HOST;
  if(!(C->Io.Windows&Window))return CR_UNSUPPORTED;
  return C->Io.Ready(C->Io.Context);
}
CR_STATUS CrSdhcInit(CR_SDHC *C,CONST CR_SDHC_IO *I){
  if(C==NULL||I==NULL||I->Ready==NULL||I->Read==NULL||I->Write==NULL||I->StallUs==NULL||I->Barrier==NULL||
     !I->Windows||(I->Windows&~CR_SDHC_WINDOW_ALL)||
     ((I->Windows&CR_SDHC_WINDOW_HOST)&&I->MappingSize<CR_SDHC_HOST_END)||
     ((I->Windows&CR_SDHC_WINDOW_POWER)&&I->MappingSize<CR_SDHC_POWER_END))
    return CR_INVALID_PARAMETER;
  /* Keep the input valid even when it aliases C->Io, and use the OS memory
   * adapter rather than a compiler-generated freestanding memmove call. */
  CR_SDHC New;
  cr_memset(&New,0,sizeof(New));
  cr_memcpy(&New.Io,I,sizeof(*I));
  New.Initialized=TRUE;
  cr_memcpy(C,&New,sizeof(New));
  return CR_SUCCESS;
}
CR_STATUS CrSdhcRead(CR_SDHC *C,UINT32 O,UINT8 W,UINT32 *V){
  if(V==NULL)return CR_INVALID_PARAMETER;*V=0;
  CR_STATUS S=Check(C,O,W,FALSE);if(S!=CR_SUCCESS)return S;
  S=C->Io.Read(C->Io.Context,O,W,V);if(S!=CR_SUCCESS)*V=0;return S;
}
CR_STATUS CrSdhcWrite(CR_SDHC *C,UINT32 O,UINT8 W,UINT32 V){
  CR_STATUS S=Check(C,O,W,TRUE);if(S!=CR_SUCCESS)return S;
  if((W==1&&V>0xffU)||(W==2&&V>0xffffU))return CR_INVALID_PARAMETER;
  return C->Io.Write(C->Io.Context,O,W,V);
}
STATIC CR_STATUS Poll(CR_SDHC *C,UINT32 O,UINT8 W,UINT32 Mask,UINT32 Expected,UINT32 Limit){
  for(UINT32 T=0;T<Limit;T+=10){UINT32 V;CR_STATUS S=CrSdhcRead(C,O,W,&V);if(S!=CR_SUCCESS)return S;
    if((V&Mask)==Expected)return CR_SUCCESS;
    S=C->Io.StallUs(C->Io.Context,10);if(S!=CR_SUCCESS)return S;
  }
  return CR_TIMEOUT;
}
STATIC CR_STATUS Rmw(CR_SDHC *C,UINT32 O,UINT8 W,UINT32 Mask,UINT32 Value){
  UINT32 V;CR_STATUS S=CrSdhcRead(C,O,W,&V);if(S!=CR_SUCCESS)return S;
  return CrSdhcWrite(C,O,W,(V&~Mask)|(Value&Mask));
}
STATIC CR_STATUS PowerWrite(CR_SDHC *C,UINT32 O,UINT8 W,UINT32 V,UINT32 Request){
  CR_STATUS S=Check(C,O,W,TRUE);if(S!=CR_SUCCESS)return S;
  if(C->SyncPower&&Request){S=C->SyncPower(C,Request,TRUE,CR_SUCCESS);if(S!=CR_SUCCESS)return S;}
  S=CrSdhcWrite(C,O,W,V);
  if(S==CR_SUCCESS)S=C->Io.Barrier(C->Io.Context);
  if(C->SyncPower&&Request){CR_STATUS Finish=C->SyncPower(C,Request,FALSE,S);if(S==CR_SUCCESS)S=Finish;}
  return S;
}
CR_STATUS CrSdhcReset(CR_SDHC *C,UINT8 Mask){
  if(Mask!=1&&Mask!=2&&Mask!=4&&Mask!=6)return CR_INVALID_PARAMETER;
  UINT32 Power=0;CR_STATUS S=CrSdhcRead(C,0x29,1,&Power);if(S!=CR_SUCCESS)return S;
  C->NeedsReset=TRUE;
  /* Qualcomm RESET_ALL while powered generates BUS_OFF. Its IRQ effects
   * must complete before polling reset, as in Linux's writeb wrapper. */
  S=PowerWrite(C,0x2f,1,Mask,(Mask==1&&(Power&1))?CR_SDHC_POWER_OFF:0);if(S!=CR_SUCCESS)return S;
  S=Poll(C,0x2f,1,Mask,0,100000);if(S!=CR_SUCCESS){C->NeedsReset=TRUE;return S;}
  if(Mask==1){S=CrSdhcRead(C,0x29,1,&Power);if(S!=CR_SUCCESS)return S;if(Power&1)return CR_DEVICE_ERROR;}
  if(Mask&3)C->LastCommand=0;
  if(Mask&5){C->DataActive=FALSE;C->Buffer=NULL;C->Length=C->Transferred=C->BlockProgress=0;C->Remaining=0;}
  /* Command-only reset cannot certify recovery of an uncertain data path. */
  if(!C->DataActive)C->NeedsReset=FALSE;
  return CR_SUCCESS;
}
CR_STATUS CrSdhcColdInit(CR_SDHC *C){
  if(C==NULL||!C->Initialized||C->NeedsReset||C->DataActive)return CR_NOT_READY;
  UINT32 V;CR_STATUS S=CrSdhcRead(C,0x2c,2,&V);if(S!=CR_SUCCESS)return S;
  if(V&BIT_(2))return CR_NOT_READY;
  S=CrSdhcRead(C,0x29,1,&V);if(S!=CR_SUCCESS)return S;
  if(V&BIT_(0))return CR_NOT_READY;
  /* Linux sdhci-msm v5 probe restores CORE_VENDOR_SPEC_POR_VAL before
   * selecting the default MCLK. register_vreg then enables IO pad selection.
   * POR already selects MCLK2, disables HC_SELECT_IN, and selects high IO.
   * Only the reset, inactive controller may discard inherited tuning here. */
  C->NeedsReset=TRUE;
  S=CrSdhcWrite(C,0x20c,4,0xa9c);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  S=CrSdhcRead(C,0x20c,4,&V);if(S!=CR_SUCCESS)return S;
  if(V!=0xa9c)return CR_DEVICE_ERROR;
  S=CrSdhcWrite(C,0x20c,4,V|BIT_(15));if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  S=CrSdhcRead(C,0x20c,4,&V);if(S!=CR_SUCCESS)return S;
  if(V!=(0xa9c|BIT_(15)))return CR_DEVICE_ERROR;
  C->NeedsReset=FALSE;return CR_SUCCESS;
}
CR_STATUS CrSdhcClock(CR_SDHC *C,BOOLEAN Enable){
  if(Enable>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrSdhcWrite(C,0x2c,2,0);if(S!=CR_SUCCESS||!Enable)return S;
  /* No internal divider: external GCC apps rate is the actual card rate. */
  S=CrSdhcWrite(C,0x2c,2,1);if(S!=CR_SUCCESS)return S;
  S=Poll(C,0x2c,2,2,2,20000);if(S!=CR_SUCCESS)return S;
  /* INT_STABLE is read-only. Publish only the two enable requests and wait
   * for their readback instead of assuming the final write is synchronous. */
  S=CrSdhcWrite(C,0x2c,2,5);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  return Poll(C,0x2c,2,7,7,20000);
}
CR_STATUS CrSdhcBusWidth(CR_SDHC *C,UINT8 Width){
  if(Width!=1&&Width!=4)return CR_UNSUPPORTED;
  return Rmw(C,0x28,1,BIT_(1)|BIT_(5),Width==4?BIT_(1):0);
}
CR_STATUS CrSdhcSdrSpeed(CR_SDHC *C,BOOLEAN HighSpeed){
  if(HighSpeed>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=Rmw(C,0x28,1,BIT_(2),HighSpeed?BIT_(2):0);if(S!=CR_SUCCESS)return S;
  UINT32 V;S=CrSdhcRead(C,0x3e,2,&V);if(S!=CR_SUCCESS)return S;
  S=PowerWrite(C,0x3e,2,V&~(7U|BIT_(3)|BIT_(15)),CR_SDHC_IO_HIGH);
  if(S!=CR_SUCCESS){C->NeedsReset=TRUE;return S;}
  /* Feedback clock, no DLL/tuning for the advertised <=50MHz SDR modes. */
  S=Rmw(C,0x200,4,BIT_(30),BIT_(30));if(S!=CR_SUCCESS)return S;
  return Rmw(C,0x200,4,BIT_(29),BIT_(29));
}
CR_STATUS CrSdhcDefaultMclk(CR_SDHC *C){
  /* Linux msm_hc_select_default: MCLK selector2, HC_SELECT_IN disabled. */
  CR_STATUS S=Rmw(C,0x20c,4,(3U<<8)|BIT_(18)|(7U<<19),2U<<8);
  if(S!=CR_SUCCESS)return S;
  return C->Io.Barrier(C->Io.Context);
}
CR_STATUS CrSdhcPrepareSdr(CR_SDHC *C){
  /* Android sdhci-msm v5 probe clears FIFO_ALT_EN (vendor3 bit10) for
   * the standard PIO buffer port. RESET_ALL does not prove its reset value.
   * Also disable DLL power savings for SDR as before; preserve neighbours. */
  if(C==NULL||!C->Initialized||C->DataActive)return CR_NOT_READY;
  UINT32 Mask=BIT_(3)|BIT_(10),V;
  CR_STATUS S=Rmw(C,0x250,4,Mask,0);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  S=CrSdhcRead(C,0x250,4,&V);if(S!=CR_SUCCESS)return S;
  return (V&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}
CR_STATUS CrSdhcIssue(CR_SDHC *C,CONST CR_SDHC_COMMAND *D,UINT32 *Events){
  if(C==NULL||D==NULL||Events==NULL||D->Index>63||D->Type>3||D->Response>CrSdResponse48NoCrc||
     D->Data>1||D->Read>1||D->AutoCmd12>1)return CR_INVALID_PARAMETER;
  *Events=0;if(C->NeedsReset)return CR_NOT_READY;
  if(C->DataActive&&!(D->Index==12&&D->Type==3&&!D->Data))return CR_BUSY;
  if(D->Data&&(D->Buffer==NULL||!D->BlockSize||D->BlockSize>2048||!D->BlockCount||
     D->Length!=(UINT32)D->BlockSize*D->BlockCount))return CR_INVALID_PARAMETER;
  if(!D->Data&&(D->BlockSize||D->BlockCount||D->Length||D->Buffer||D->AutoCmd12))return CR_INVALID_PARAMETER;
  if(D->AutoCmd12&&D->BlockCount<2)return CR_INVALID_PARAMETER;
  UINT32 Inhibit=1;BOOLEAN Busy=D->Response==CrSdResponse48Busy;
  if((D->Data||Busy)&&!(D->Index==12&&D->Type==3))Inhibit|=2;
  UINT32 State;CR_STATUS S=CrSdhcRead(C,0x24,4,&State);if(S!=CR_SUCCESS)return S;
  if(State&Inhibit)return CR_BUSY;
  UINT16 Cmd=(UINT16)((D->Index<<8)|(D->Type<<6));
  switch(D->Response){
    case CrSdResponseNone:break;
    case CrSdResponse48:Cmd|=2|8|16;break;
    case CrSdResponse48Busy:Cmd|=3|8|16;break;
    case CrSdResponse136:Cmd|=1|8;break;
    case CrSdResponse48NoCrc:Cmd|=2;break;
    default:return CR_INVALID_PARAMETER;
  }
  UINT16 Transfer=0;UINT32 Required=CR_SDHC_EVENT_COMMAND;
  if(Busy)Required|=CR_SDHC_EVENT_TRANSFER;
  if(D->Data){
    Cmd|=BIT_(5);Transfer=BIT_(1)|(D->Read?BIT_(4):0)|(D->BlockCount>1?BIT_(5):0)|(D->AutoCmd12?BIT_(2):0);
    Required|=D->Read?CR_SDHC_EVENT_READ_READY:CR_SDHC_EVENT_WRITE_READY;
    S=CrSdhcWrite(C,4,2,D->BlockSize);if(S!=CR_SUCCESS)return S;
    S=CrSdhcWrite(C,6,2,D->BlockCount);if(S!=CR_SUCCESS)return S;
  }
  S=CrSdhcWrite(C,0xc,2,Transfer);if(S!=CR_SUCCESS)return S;
  S=CrSdhcWrite(C,8,4,D->Argument);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  /* Android sdhci-msm __sdhci_msm_check_write: the CRC engine needs three
   * MCLKs to clear after a response without CRC. At the supported minimum
   * 400kHz that is 7.5us. In particular ACMD41 -> CMD55/CMD2 must not race
   * the cleanup even if command completion was already observed. */
  if((Cmd&BIT_(3))&&(C->LastCommand&0xffU)&&!(C->LastCommand&BIT_(3))){
    S=C->Io.StallUs(C->Io.Context,8);if(S!=CR_SUCCESS)return S;
  }
  /* Commit before a command write whose completion may be uncertain. */
  C->NeedsReset=TRUE;
  C->LastCommand=Cmd;
  if(D->Data){C->DataActive=TRUE;C->Buffer=D->Buffer;C->Length=D->Length;C->Transferred=C->BlockProgress=0;
    C->BlockSize=D->BlockSize;C->Remaining=D->BlockCount;C->Read=D->Read;}
  S=CrSdhcWrite(C,0xe,2,Cmd);if(S!=CR_SUCCESS)return S;
  C->NeedsReset=FALSE;*Events=Required;return CR_SUCCESS;
}
CR_STATUS CrSdhcPioBlock(CR_SDHC *C,UINT8 *Buffer,UINT32 Length,UINT16 *Remaining){
  if(C==NULL||Remaining==NULL||!C->DataActive||C->NeedsReset||!C->Remaining||C->BlockProgress||
     Buffer!=C->Buffer+C->Transferred||Length!=C->BlockSize)return CR_INVALID_PARAMETER;
  *Remaining=C->Remaining;UINT32 State;CR_STATUS S=CrSdhcRead(C,0x24,4,&State);if(S!=CR_SUCCESS)return S;
  if(!(State&(C->Read?BIT_(11):BIT_(10))))return CR_NOT_READY;
  for(UINT32 I=0;I<Length;){
    UINT32 N=Length-I>=4?4:Length-I>=2?2:1;
    /* Match SDHCI data-port lane order for 16/8bit tails without over-reading
     * FIFO bytes outside this block. Unaligned buffers never cast to UINT32. */
    UINT32 Offset=0x20+((N==1&&(I&2))?2:0),V=0;
    if(C->Read)S=CrSdhcRead(C,Offset,(UINT8)N,&V);
    else {for(UINT32 B=0;B<N;++B)V|=(UINT32)Buffer[I+B]<<(8*B);S=CrSdhcWrite(C,Offset,(UINT8)N,V);}
    if(S!=CR_SUCCESS){C->NeedsReset=TRUE;return S;}
    if(C->Read)for(UINT32 B=0;B<N;++B)Buffer[I+B]=(UINT8)(V>>(8*B));
    I+=N;C->Transferred+=N;C->BlockProgress+=N;
  }
  C->BlockProgress=0;--C->Remaining;*Remaining=C->Remaining;return CR_SUCCESS;
}
CR_STATUS CrSdhcResponse(CR_SDHC *C,CR_SDHC_RESPONSE Type,UINT8 R[16]){
  if(R==NULL||Type>CrSdResponse48NoCrc)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<16;++I)R[I]=0;
  if(Type==CrSdResponseNone)return CR_SUCCESS;
  for(UINT32 I=0;I<(Type==CrSdResponse136?4U:1U);++I){UINT32 V;
    CR_STATUS S=CrSdhcRead(C,0x10+4*I,4,&V);if(S!=CR_SUCCESS)return S;
    for(UINT32 B=0;B<4;++B)R[4*I+B]=(UINT8)(V>>(8*B));
  }
  return CR_SUCCESS;
}
CR_STATUS CrSdhcInterrupt(CR_SDHC *C,UINT32 *Events,UINT32 *Errors){
  if(Events==NULL||Errors==NULL)return CR_INVALID_PARAMETER;*Events=*Errors=0;
  /* Linux/Android sdhci_irq snapshots and acknowledges the combined 32-bit
   * status register. Keep normal/error causes from the same observation,
   * and never acknowledge a cause which arrived after that snapshot. */
  UINT32 Raw,Mask,Err=0,Auto=0;CR_STATUS S=CrSdhcRead(C,0x30,4,&Raw);if(S!=CR_SUCCESS)return S;
  C->InterruptStatus=Raw;
  S=CrSdhcRead(C,0x38,4,&Mask);if(S!=CR_SUCCESS)return S;
  /* Normal signal/status enable bit15 is read-only zero in SDHCI. The
   * error summary must be qualified by the individual error enables, not
   * by that reserved normal-enable bit (HDK reads 0033 here). */
  UINT32 Status=Raw&Mask&0x7fffU;
  if(Raw&0x8000){
    Err=(Raw&Mask)>>16;
    if(Err)Status|=0x8000;
    if(Err&BIT_(8)){S=CrSdhcRead(C,0x3c,2,&Auto);if(S!=CR_SUCCESS)return S;}
  }
  if(!Status)return CR_SUCCESS;
  /* Error-summary bit15 is read-only; individual upper-half causes clear it. */
  S=CrSdhcWrite(C,0x30,4,(Status&0x7fffU)|(Err<<16));if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  /* WDK SDPORT_AUTO_CMD12 status uses the SDHCI bit positions at16..23. */
  Err|=(Auto&0x9fU)<<16;
  *Events=Status;*Errors=Err;
  if(Err)C->NeedsReset=TRUE;
  if((Status&CR_SDHC_EVENT_TRANSFER)&&C->DataActive&&!C->Remaining)C->DataActive=FALSE;
  return CR_SUCCESS;
}
CR_STATUS CrSdhcEvents(CR_SDHC *C,UINT32 Events,BOOLEAN Enable){
  if(Events>0xffffU||Enable>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=Rmw(C,0x34,2,Events,Enable?Events:0);if(S!=CR_SUCCESS)return S;
  S=Rmw(C,0x38,2,Events,Enable?Events:0);if(S!=CR_SUCCESS)return S;
  if(Events&0x8000){S=CrSdhcWrite(C,0x36,2,Enable?0x01ff:0);if(S!=CR_SUCCESS)return S;
    S=CrSdhcWrite(C,0x3a,2,Enable?0x01ff:0);}
  return S;
}
CR_STATUS CrSdhcClearEvents(CR_SDHC *C,UINT32 E){
  if(E>0xffffU)return CR_INVALID_PARAMETER;
  UINT32 Raw;CR_STATUS S=CrSdhcRead(C,0x30,4,&Raw);if(S!=CR_SUCCESS)return S;
  UINT32 Clear=Raw&((E&0x7fffU)|((E&0x8000)?0xffff0000U:0));
  /* A blanket W1C immediately before launch can retire a later cause. Only
   * clear observed stale bits; an empty status needs no register write. */
  if(!Clear)return CR_SUCCESS;
  S=CrSdhcWrite(C,0x30,4,Clear);if(S!=CR_SUCCESS)return S;
  return C->Io.Barrier(C->Io.Context);
}
CR_STATUS CrSdhcPowerMask(CR_SDHC *C,BOOLEAN Enable){
  if(Enable>1)return CR_INVALID_PARAMETER;
  return CrSdhcWrite(C,0x244,4,Enable?15:0);
}
CR_STATUS CrSdhcPowerCapture(CR_SDHC *C,UINT32 *Observed){
  if(Observed==NULL)return CR_INVALID_PARAMETER;*Observed=0;UINT32 V;
  CR_STATUS S=CrSdhcRead(C,0x240,4,&V);if(S!=CR_SUCCESS)return S;
  V&=15;if(!V)return CR_SUCCESS;
  C->PowerPending|=V;*Observed=V;
  S=CrSdhcPowerMask(C,FALSE);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<10;++I){
    S=CrSdhcWrite(C,0x248,4,V);if(S!=CR_SUCCESS)return S;
    UINT32 Current;S=CrSdhcRead(C,0x240,4,&Current);if(S!=CR_SUCCESS)return S;
    if(!(Current&V))return CR_SUCCESS;
    S=C->Io.StallUs(C->Io.Context,10);if(S!=CR_SUCCESS)return S;
  }
  return CR_TIMEOUT;
}
CR_STATUS CrSdhcPowerComplete(CR_SDHC *C,UINT32 V,BOOLEAN BusSuccess,BOOLEAN IoSuccess){
  if(C==NULL||!V||(V&~15U)||(V&~C->PowerPending)||BusSuccess>1||IoSuccess>1)return CR_INVALID_PARAMETER;
  UINT32 Status;CR_STATUS S=CrSdhcRead(C,0x240,4,&Status);if(S!=CR_SUCCESS)return S;
  if(Status&V)return CR_NOT_READY;
  UINT32 Ack=0;if(V&3)Ack|=BusSuccess?1:2;
  /* BUS_ON/OFF also imply an IO-level request, exactly as msm power IRQ. */
  Ack|=IoSuccess?4:8;
  S=CrSdhcWrite(C,0x24c,4,Ack);if(S!=CR_SUCCESS)return S;
  S=CrSdhcPowerMask(C,TRUE);if(S!=CR_SUCCESS)return S;
  C->PowerPending&=~V;return CR_SUCCESS;
}
CR_STATUS CrSdhcPowerRequest(CR_SDHC *C,BOOLEAN On){
  if(On>1)return CR_INVALID_PARAMETER;
  CR_STATUS S=Check(C,0x29,1,TRUE);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context);if(S!=CR_SUCCESS)return S;
  /* Standard 3.0V selector; actual BOB/L9C/L6C votes are handled by the
   * qualified PASSIVE lower power worker, acknowledged by PWR IRQ255. */
  return CrSdhcWrite(C,0x29,1,On?0x0d:0);
}
CR_STATUS CrSdhcIoPadHigh(CR_SDHC *C,BOOLEAN High){
  if(High>1)return CR_INVALID_PARAMETER;
  return Rmw(C,0x20c,4,BIT_(16),High?0:BIT_(16));
}
