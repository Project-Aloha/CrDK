/* Native MmcHost adaptation; shared SDHCI core owns register semantics. MIT */
#include "SdhciCrDxe.h"
#include "SdResources.h"
#include <Protocol/DevicePath.h>

CR_SD_NATIVE mSdHost;
STATIC EFI_GUID mSdCardPathGuid={0x9d7d72ab,0x187c,0x4f29,{0x9d,0xa2,0x84,0x50,0x53,0x44,0x43,0x32}};

STATIC EFI_STATUS Enter(EFI_MMC_HOST_PROTOCOL *This){
  if(This!=&mSdHost.Host)return EFI_INVALID_PARAMETER;
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);EFI_STATUS S=EFI_SUCCESS;
  if(mSdHost.Exiting||!mSdHost.Acquired)S=EFI_NOT_READY;
  else if(mSdHost.Busy)S=EFI_ACCESS_DENIED;
  else mSdHost.Busy=TRUE;
  gBS->RestoreTPL(T);return S;
}
STATIC EFI_STATUS Leave(EFI_STATUS S){
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);mSdHost.Busy=FALSE;gBS->RestoreTPL(T);return S;
}
STATIC CR_STATUS Ready(VOID *C){(VOID)C;if(mSdHost.Exiting&&!mSdHost.Fencing)return CR_NOT_READY;return CrSdResourcesReady();}
STATIC CR_STATUS Read(VOID *C,UINT32 O,UINT8 W,UINT32 *V){
  (VOID)C;if(!mSdHost.Target||!V||O>=mSdHost.Target->Bytes||W>mSdHost.Target->Bytes-O)return CR_INVALID_PARAMETER;
  if(W==1)*V=MmioRead8((UINTN)mSdHost.Target->Base+O);else if(W==2)*V=MmioRead16((UINTN)mSdHost.Target->Base+O);
  else if(W==4)*V=MmioRead32((UINTN)mSdHost.Target->Base+O);else return CR_INVALID_PARAMETER;return CR_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,UINT32 O,UINT8 W,UINT32 V){
  (VOID)C;if(!mSdHost.Target||O>=mSdHost.Target->Bytes||W>mSdHost.Target->Bytes-O)return CR_INVALID_PARAMETER;
  if(W==1)MmioWrite8((UINTN)mSdHost.Target->Base+O,(UINT8)V);else if(W==2)MmioWrite16((UINTN)mSdHost.Target->Base+O,(UINT16)V);
  else if(W==4)MmioWrite32((UINTN)mSdHost.Target->Base+O,V);else return CR_INVALID_PARAMETER;return CR_SUCCESS;
}
STATIC CR_STATUS Stall(VOID *C,UINT32 Us){(VOID)C;MicroSecondDelay(Us);return CR_SUCCESS;}
STATIC CR_STATUS Barrier(VOID *C){(VOID)C;MemoryFence();return CR_SUCCESS;}

STATIC BOOLEAN EFIAPI Present(EFI_MMC_HOST_PROTOCOL *This){
  if(Enter(This)!=EFI_SUCCESS)return FALSE;
  BOOLEAN P=FALSE;EFI_STATUS S=CrSdResourcesCardPresent(&P);Leave(S);return S==EFI_SUCCESS&&P;
}
STATIC BOOLEAN EFIAPI ReadOnly(EFI_MMC_HOST_PROTOCOL *This){return This!=&mSdHost.Host||mSdHost.Exiting;}
STATIC BOOLEAN EFIAPI Multi(EFI_MMC_HOST_PROTOCOL *This){(VOID)This;return FALSE;}
STATIC EFI_STATUS EFIAPI DevicePath(EFI_MMC_HOST_PROTOCOL *This,EFI_DEVICE_PATH_PROTOCOL **Out){
  if(!Out||This!=&mSdHost.Host)return EFI_INVALID_PARAMETER;*Out=NULL;
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  VENDOR_DEVICE_PATH *P=AllocateZeroPool(sizeof(*P));if(!P)return Leave(EFI_OUT_OF_RESOURCES);
  P->Header.Type=HARDWARE_DEVICE_PATH;P->Header.SubType=HW_VENDOR_DP;
  P->Header.Length[0]=(UINT8)sizeof(*P);P->Header.Length[1]=0;CopyMem(&P->Guid,&mSdCardPathGuid,sizeof(P->Guid));
  *Out=&P->Header;return Leave(EFI_SUCCESS);
}

/* No host ISR is needed for the PIO request engine. Hardware status and FIFO
 * readiness are polled with a bounded delay; the separate power IRQ255 is
 * captured by the OSKAL resource owner and serviced here without waiting for
 * another TPL_CALLBACK worker. No TPL_NOTIFY lock spans this wait. */
STATIC EFI_STATUS Wait(UINT32 Required,UINT32 LimitUs){
  for(UINT32 T=0;T<LimitUs;T+=10){
    EFI_STATUS S=CrSdResourcesServicePower(&mSdHost.Core);if(S!=EFI_SUCCESS)return S;
    BOOLEAN P=FALSE;S=CrSdResourcesCardPresent(&P);if(S!=EFI_SUCCESS)return S;if(!P)return EFI_NO_MEDIA;
    UINT32 Events,Errors;S=CrSdhcInterrupt(&mSdHost.Core,&Events,&Errors);if(S!=EFI_SUCCESS)return S;
    if(!mSdHost.WaitPolls)mSdHost.FirstIrq=mSdHost.Core.InterruptStatus;
    ++mSdHost.WaitPolls;mSdHost.SeenIrq|=mSdHost.Core.InterruptStatus;
    mSdHost.Events|=Events;mSdHost.Errors|=Errors;
    if(mSdHost.Errors){return mSdHost.Errors&(BIT0|BIT4)?EFI_TIMEOUT:EFI_DEVICE_ERROR;}
    if((mSdHost.Events&Required)==Required){mSdHost.Events&=~Required;return EFI_SUCCESS;}
    MicroSecondDelay(10);
  }
  return EFI_TIMEOUT;
}
STATIC EFI_STATUS Recover(EFI_STATUS Original){
  /* Capture before reset/W1C destroys the command and interrupt evidence.
   * Read through the core so a lost resource lease never becomes raw MMIO. */
  CONST UINT32 Offset[]={0x24,0x30,0x32,0x2c,0xe,0x10,0x34,0x36,0x38,0x3a,8,0x29,0x28};
  CONST UINT8 Width[]={4,2,2,2,2,4,2,2,2,2,4,1,1};
  UINT32 Value[ARRAY_SIZE(Offset)]={0},Valid=0,Events=mSdHost.Events,Errors=mSdHost.Errors;
  for(UINT32 I=0;I<ARRAY_SIZE(Offset);++I){
    if(CrSdhcRead(&mSdHost.Core,Offset[I],Width[I],&Value[I])==CR_SUCCESS)Valid|=1U<<I;
  }
  DEBUG((DEBUG_ERROR,"SdhciCr: %aCMD%u arg=%08x stage=%a failed %r events=%04x errors=%04x\n",
    mSdHost.LastApplication?"A":"",mSdHost.LastCommand,mSdHost.LastArgument,
    mSdHost.RequestStage,Original,Events,Errors));
  DEBUG((DEBUG_ERROR,"SdhciCr: before reset valid=%02x state=%08x irq=%04x/%04x clock=%04x cmd=%04x response=%08x\n",
    Valid,Value[0],Value[1],Value[2],Value[3],Value[4],Value[5]));
  DEBUG((DEBUG_ERROR,"SdhciCr: enables=%04x/%04x signals=%04x/%04x argument=%08x power=%02x host=%02x\n",
    Value[6],Value[7],Value[8],Value[9],Value[10],Value[11],Value[12]));
  DEBUG((DEBUG_ERROR,"SdhciCr: polls=%u first=%08x seen=%08x last=%08x rate=%u\n",
    mSdHost.WaitPolls,mSdHost.FirstIrq,mSdHost.SeenIrq,mSdHost.Core.InterruptStatus,mSdHost.Clock));
  mSdHost.DataPending=mSdHost.ResponseValid=FALSE;
  EFI_STATUS S=CrSdhcReset(&mSdHost.Core,6);
  if(S==EFI_SUCCESS)S=CrSdhcClearEvents(&mSdHost.Core,0xffff);
  mSdHost.Events=mSdHost.Errors=0;
  DEBUG((DEBUG_ERROR,"SdhciCr: command/data reset %r\n",S));
  return Original;
}
STATIC EFI_STATUS CardResponse(UINT32 Index,BOOLEAN App,CONST CR_SDHC_COMMAND *D){
  if(D->Response==CrSdResponseNone||D->Response==CrSdResponse136||D->Response==CrSdResponse48NoCrc||Index==8)return EFI_SUCCESS;
  UINT32 Status=0;CopyMem(&Status,mSdHost.Response,sizeof(Status));
  if(Index==3&&!App){
    /* R6 carries RCA in bits31:16, and CRC/illegal/general errors at15/14/13.
     * Do not decode its high RCA bits as the R1 out-of-range/status bitmap. */
    if(Status&BIT15)return EFI_CRC_ERROR;
    if(Status&BIT14)return EFI_UNSUPPORTED;
    if((Status&BIT13)||!(Status>>16))return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
  }
  /* SD native R1 error flags (Linux mmc.h and Zephyr sd_spec.h). Bits25,14,
   * and13 are status; bits17/18 and7 are reserved for this SD-only host. */
  UINT32 Errors=Status&(BIT31|BIT30|BIT29|BIT28|BIT27|BIT26|BIT24|BIT23|BIT22|BIT21|BIT20|BIT19|BIT16|BIT15|BIT3);
  if(Errors&BIT26)return EFI_WRITE_PROTECTED;
  if(Errors&BIT23)return EFI_CRC_ERROR;
  if(Errors&BIT22)return EFI_UNSUPPORTED;
  if(Errors&BIT3)return EFI_ACCESS_DENIED;
  if(Errors)return EFI_DEVICE_ERROR;
  if(D->Data&&(Status&BIT25))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
EFI_STATUS CrSdHostStopClock(VOID){
  mSdHost.CardClockStopped=FALSE;
  EFI_STATUS S=CrSdhcClock(&mSdHost.Core,FALSE);if(S!=EFI_SUCCESS)return S;
  UINT32 Control=0;S=CrSdhcRead(&mSdHost.Core,0x2c,2,&Control);if(S!=EFI_SUCCESS)return S;
  if(Control&BIT2)return EFI_DEVICE_ERROR;mSdHost.CardClockStopped=TRUE;return EFI_SUCCESS;
}
STATIC EFI_STATUS Clock(UINT32 Frequency,UINT32 Width,UINT32 Timing){
  if((Width!=1&&Width!=4)||Timing!=EMMCBACKWARD)return EFI_UNSUPPORTED;
  UINT32 Hz=Frequency?Frequency:mSdHost.Clock;
  if(Hz!=400000&&Hz!=25000000&&Hz!=50000000)return EFI_UNSUPPORTED;
  /* A failed apps-clock reacquire leaves the card clock off but removes MMIO
   * prerequisites. Restore that exact prerequisite before retrying MMIO. */
  EFI_STATUS S=CrSdResourcesReady();
  if(S!=EFI_SUCCESS){
    if(!mSdHost.CardClockStopped)return S;
    S=CrSdResourcesSetClock(Hz);if(S!=EFI_SUCCESS)return S;
  }
  CONST CHAR8 *Stage="card clock stop";
  S=CrSdHostStopClock();if(S!=EFI_SUCCESS)goto Out;
  Stage="SDR prepare";S=CrSdhcPrepareSdr(&mSdHost.Core);if(S!=EFI_SUCCESS)goto Out;
  Stage="MCLK select";S=CrSdhcDefaultMclk(&mSdHost.Core);if(S!=EFI_SUCCESS)goto Out;
  Stage="apps clock";S=CrSdResourcesSetClock(Hz);if(S!=EFI_SUCCESS)goto Out;
  Stage="bus width";S=CrSdhcBusWidth(&mSdHost.Core,(UINT8)Width);if(S!=EFI_SUCCESS)goto Out;
  Stage="SDR/IO handshake";S=CrSdhcSdrSpeed(&mSdHost.Core,Hz==50000000);if(S!=EFI_SUCCESS)goto Out;
  /* Enabling may have committed even if a later operation fails. */
  Stage="card clock enable";mSdHost.CardClockStopped=FALSE;S=CrSdhcClock(&mSdHost.Core,TRUE);
  if(S==EFI_SUCCESS)mSdHost.Clock=Hz;
Out:
  if(S!=EFI_SUCCESS){
    DEBUG((DEBUG_ERROR,"SdhciCr: %uHz %a: %r\n",Hz,Stage,S));
    if(CrSdResourcesReady()==EFI_SUCCESS){
      CONST UINT32 Offset[]={0x2c,0x29,0x2f,0x3e,0x20c,0x200,0x240,0x244,0x24c};
      CONST UINT8 Widths[]={2,1,1,2,4,4,4,4,4};
      for(UINT32 I=0;I<ARRAY_SIZE(Offset);++I){UINT32 V=0;EFI_STATUS Readback=CrSdhcRead(&mSdHost.Core,Offset[I],Widths[I],&V);
        DEBUG((DEBUG_ERROR,"SdhciCr: clock readback +%03x=%08x: %r\n",Offset[I],V,Readback));}
    }
  }
  return S;
}
STATIC EFI_STATUS EFIAPI SetIos(EFI_MMC_HOST_PROTOCOL *This,UINT32 Frequency,UINT32 Width,UINT32 Timing){
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  if(!mSdHost.Initialized||mSdHost.DataPending)return Leave(EFI_NOT_READY);
  return Leave(Clock(Frequency,Width,Timing));
}
STATIC EFI_STATUS EFIAPI Notify(EFI_MMC_HOST_PROTOCOL *This,MMC_STATE State){
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  if(State==MmcHwInitializationState){
    CONST CHAR8 *Stage="resources";
    mSdHost.Initialized=mSdHost.Application=mSdHost.DataPending=mSdHost.ResponseValid=FALSE;
    mSdHost.Events=mSdHost.Errors=0;
    mSdHost.TraceCommands=0;
    S=CrSdResourcesReady();
    if(S!=EFI_SUCCESS&&mSdHost.CardClockStopped)S=CrSdResourcesSetClock(400000);
    if(S==EFI_SUCCESS){Stage="reset";S=CrSdhcReset(&mSdHost.Core,1);}
    if(S==EFI_SUCCESS){Stage="clock stop";S=CrSdHostStopClock();}
    if(S==EFI_SUCCESS){Stage="cold setup";S=CrSdhcColdInit(&mSdHost.Core);}
    if(S==EFI_SUCCESS){Stage="events";S=CrSdhcWrite(&mSdHost.Core,0x2e,1,0x0e);}
    if(S==EFI_SUCCESS)S=CrSdhcClearEvents(&mSdHost.Core,0xffff);
    if(S==EFI_SUCCESS)S=CrSdhcEvents(&mSdHost.Core,0x8033,TRUE);
    if(S==EFI_SUCCESS)S=CrSdhcPowerMask(&mSdHost.Core,TRUE);
    if(S==EFI_SUCCESS)S=CrSdhcIoPadHigh(&mSdHost.Core,TRUE);
    /* The owner must observe and acknowledge a new BUS_ON cause after this
     * request; an empty current pending journal cannot certify completion. */
    if(S==EFI_SUCCESS){Stage="power handshake";S=CrSdResourcesRequestPower(&mSdHost.Core,TRUE);}
    if(S==EFI_SUCCESS){Stage="400kHz clock";S=Clock(400000,1,EMMCBACKWARD);}
    if(S==EFI_SUCCESS){mSdHost.Initialized=TRUE;MicroSecondDelay(1000);}
    DEBUG((S==EFI_SUCCESS?DEBUG_INFO:DEBUG_ERROR,
      "SdhciCr: SDCC2 init SDR 400kHz/1bit PIO: %r stage=%a pending=%x\n",S,Stage,mSdHost.Core.PowerPending));
  }else if(State<=MmcInvalidState||State>MmcDisconnectState)S=EFI_INVALID_PARAMETER;
  else if(!mSdHost.Initialized)S=EFI_NOT_READY;
  return Leave(S);
}
STATIC EFI_STATUS EFIAPI Send(EFI_MMC_HOST_PROTOCOL *This,MMC_CMD Cmd,UINT32 Argument){
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  UINT32 Index=MMC_GET_INDX(Cmd);BOOLEAN App=mSdHost.Application;
  if(!mSdHost.Initialized)return Leave(EFI_NOT_READY);
  if(mSdHost.DataPending&&Index!=12)return Leave(EFI_NOT_READY);
  if(Index>63||(Cmd&~(0xffffU|MMC_CMD_WAIT_RESPONSE|MMC_CMD_LONG_RESPONSE|MMC_CMD_NO_CRC_RESPONSE)))return Leave(EFI_INVALID_PARAMETER);
  /* SD-only identification: CMD1 and CMD5 probes fail promptly; neither MMC
   * nor SDIO nor a 1.8V/UHS voltage switch is advertised by this host. */
  switch(Index){case 0:case 2:case 3:case 6:case 7:case 8:case 9:case 12:case 13:case 16:case 17:case 24:case 41:case 51:case 55:break;
    default:return Leave(EFI_UNSUPPORTED);}
  if((Index==41||Index==51)&&!App)return Leave(EFI_UNSUPPORTED);
  if(App&&Index!=0&&Index!=6&&Index!=41&&Index!=51&&Index!=55)return Leave(EFI_UNSUPPORTED);
  if(Index==16&&Argument!=512)return Leave(EFI_UNSUPPORTED);
  CR_SDHC_COMMAND D={0};D.Index=(UINT8)Index;D.Argument=Argument;
  if(Cmd&MMC_CMD_WAIT_RESPONSE)D.Response=Cmd&MMC_CMD_LONG_RESPONSE?CrSdResponse136:
    Cmd&MMC_CMD_NO_CRC_RESPONSE?CrSdResponse48NoCrc:CrSdResponse48;
  if(Index==7||Index==12)D.Response=CrSdResponse48Busy;
  if(Index==12){D.Type=3;if(mSdHost.Core.NeedsReset){S=CrSdhcReset(&mSdHost.Core,6);if(S!=EFI_SUCCESS)return Leave(S);}mSdHost.DataPending=FALSE;}
  if(Index==17||Index==24||Index==51||(Index==6&&!App)){
    D.Data=TRUE;D.Read=Index!=24;D.BlockSize=Index==51?8:Index==6?64:512;D.BlockCount=1;
    D.Buffer=mSdHost.Scratch;D.Length=D.BlockSize;
  }
  mSdHost.LastCommand=Index;mSdHost.LastArgument=Argument;mSdHost.LastApplication=App;
  mSdHost.RequestStage="issue";
  mSdHost.ResponseValid=FALSE;mSdHost.Events=mSdHost.Errors=0;
  mSdHost.WaitPolls=mSdHost.FirstIrq=mSdHost.SeenIrq=0;
  S=CrSdhcClearEvents(&mSdHost.Core,0xffff);
  UINT32 Required=0;if(S==EFI_SUCCESS)S=CrSdhcIssue(&mSdHost.Core,&D,&Required);
  /* Rejecting a command before the command-register mutation must preserve
   * the card's pending CMD55 context. A committed/uncertain command consumes it. */
  if(S==EFI_SUCCESS||mSdHost.Core.NeedsReset)mSdHost.Application=FALSE;
  if(S==EFI_SUCCESS){mSdHost.RequestStage="command completion";S=Wait(CR_SDHC_EVENT_COMMAND|(D.Response==CrSdResponse48Busy?CR_SDHC_EVENT_TRANSFER:0),1000000);}
  if(S==EFI_SUCCESS){mSdHost.RequestStage="response";S=CrSdhcResponse(&mSdHost.Core,D.Response,mSdHost.Response);}
  if(S==EFI_SUCCESS){mSdHost.RequestStage="card status";S=CardResponse(Index,App,&D);}
  if(S!=EFI_SUCCESS)return Leave(Recover(S));
  /* Bounded first-command evidence, without printing or extra MMIO during
   * completion polling. This includes the OCR query and first busy cycles. */
  if(mSdHost.TraceCommands++<16){
    UINT32 Reply=0;CopyMem(&Reply,mSdHost.Response,sizeof(Reply));
    DEBUG((DEBUG_WARN,"SdhciCr: %aCMD%u arg=%08x response=%08x polls=%u irq=%08x\n",
      App?"A":"",Index,Argument,Reply,mSdHost.WaitPolls,mSdHost.SeenIrq));
  }
  mSdHost.ResponseType=D.Response;mSdHost.ResponseValid=D.Response!=CrSdResponseNone;
  mSdHost.DataPending=D.Data;mSdHost.DataRead=D.Read;mSdHost.DataLength=D.BlockSize;
  if(Index==55)mSdHost.Application=(mSdHost.Response[0]&MMC_STATUS_APP_CMD)!=0;
  return Leave(EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI Response(EFI_MMC_HOST_PROTOCOL *This,MMC_RESPONSE_TYPE Type,UINT32 *Buffer){
  if(!Buffer||Type>1)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  if(!mSdHost.ResponseValid||(Type==MMC_RESPONSE_TYPE_R2)!=(mSdHost.ResponseType==CrSdResponse136))return Leave(EFI_NOT_READY);
  if(Type==MMC_RESPONSE_TYPE_R2){
    /* SDHCI omits the CRC/end byte. MmcDxe's packed CID/CSD fields use the
     * complete little-endian128-bit register with that low byte restored. */
    UINT32 Previous=0;
    for(UINT32 I=0;I<4;++I){UINT32 Raw;CopyMem(&Raw,mSdHost.Response+4*I,4);Buffer[I]=(Raw<<8)|(Previous>>24);Previous=Raw;}
  }else CopyMem(Buffer,mSdHost.Response,4);
  return Leave(EFI_SUCCESS);
}
STATIC EFI_STATUS Data(EFI_MMC_HOST_PROTOCOL *This,UINTN Length,UINT32 *Buffer,BOOLEAN Reading){
  if(!Buffer)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(This);if(S!=EFI_SUCCESS)return S;
  if(!mSdHost.DataPending||Reading!=mSdHost.DataRead||Length!=mSdHost.DataLength)return Leave(EFI_BAD_BUFFER_SIZE);
  if(!Reading)CopyMem(mSdHost.Scratch,Buffer,Length);
  mSdHost.RequestStage="buffer ready";
  S=Wait(Reading?CR_SDHC_EVENT_READ_READY:CR_SDHC_EVENT_WRITE_READY,1000000);
  UINT16 Remaining=1;if(S==EFI_SUCCESS){mSdHost.RequestStage="PIO";S=CrSdhcPioBlock(&mSdHost.Core,mSdHost.Scratch,(UINT32)Length,&Remaining);}
  if(S==EFI_SUCCESS&&Remaining)S=EFI_DEVICE_ERROR;
  if(S==EFI_SUCCESS){mSdHost.RequestStage="transfer completion";S=Wait(CR_SDHC_EVENT_TRANSFER,5000000);}
  if(S!=EFI_SUCCESS)return Leave(Recover(S));
  if(Reading)CopyMem(Buffer,mSdHost.Scratch,Length);
  /* FIFO has drained and the exact transfer-complete event was observed. */
  mSdHost.Core.DataActive=FALSE;
  mSdHost.DataPending=FALSE;return Leave(EFI_SUCCESS);
}
STATIC EFI_STATUS EFIAPI ReadData(EFI_MMC_HOST_PROTOCOL *This,EFI_LBA Lba,UINTN Length,UINT32 *Buffer){(VOID)Lba;return Data(This,Length,Buffer,TRUE);}
STATIC EFI_STATUS EFIAPI WriteData(EFI_MMC_HOST_PROTOCOL *This,EFI_LBA Lba,UINTN Length,UINT32 *Buffer){(VOID)Lba;return Data(This,Length,Buffer,FALSE);}

EFI_STATUS CrSdHostInit(VOID){
  CONST CR_TARGET_SD_CONTEXT *Target=CrDalGetSdContext();
  if(!Target)return EFI_UNSUPPORTED;
  if(!CrTargetSdValid(Target))return EFI_COMPROMISED_DATA;
  mSdHost.Target=Target;
  CR_SDHC_IO Io={NULL,Ready,Read,Write,Stall,Barrier,Target->Bytes,CR_SDHC_WINDOW_ALL};
  EFI_STATUS S=CrSdhcInit(&mSdHost.Core,&Io);if(S!=EFI_SUCCESS)return S;
  mSdHost.Host=(EFI_MMC_HOST_PROTOCOL){MMC_HOST_PROTOCOL_REVISION,Present,ReadOnly,DevicePath,Notify,Send,Response,ReadData,WriteData,SetIos,Multi};
  return EFI_SUCCESS;
}
EFI_STATUS CrSdHostQuiesce(VOID){
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);mSdHost.Exiting=TRUE;BOOLEAN Busy=mSdHost.Busy;gBS->RestoreTPL(T);
  mSdHost.Fencing=TRUE;
  EFI_STATUS S=CrSdResourcesFreeze(&mSdHost.Core);if(S!=EFI_SUCCESS){mSdHost.Fencing=FALSE;return S;}
  if(Busy){mSdHost.Fencing=FALSE;return EFI_NOT_READY;}
  S=CrSdhcEvents(&mSdHost.Core,0xffff,FALSE);
  if(S==EFI_SUCCESS)S=CrSdHostStopClock();
  /* Startup's successful RESET_ALL retired inherited DMA before publication.
   * Every later command is PIO. Stop those command/data state machines without
   * RESET_ALL's BUS_OFF transaction, which cannot run rail RPC at EBS NOTIFY.
   * A failed startup without that evidence still requires the full fence. */
  if(S==EFI_SUCCESS)S=CrSdhcReset(&mSdHost.Core,mSdHost.InheritedDmaStopped?6:1);
  if(S==EFI_SUCCESS)mSdHost.Initialized=mSdHost.DataPending=FALSE;
  mSdHost.Fencing=FALSE;
  return S;
}
