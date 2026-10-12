/** Qualcomm DWC3 core sequencing adapted from Linux dwc3/core.c/dwc3-qcom.c.
    Copyright (c) 2010-2020, The Linux Foundation and Synopsys.
    SPDX-License-Identifier: GPL-2.0-only */
#include <Library/Dwc3CrLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_time.h>
#define B(N) (1U<<(N))
#define GCTL 0x10U
#define GUCTL1 0x1cU
#define GSNPSID 0x20U
#define GHWPARAMS0 0x40U
#define GHWPARAMS1 0x44U
#define VERSION 0xa0U
#define VERSION_TYPE 0xa4U
#define USB2PHYCFG 0x100U
#define USB3PIPECTL 0x1c0U
#define EVENTS 0x300U
#define MANAGED_EVENTS 1U
#define GUCTL3 0x50cU
#define DCTL 0x604U
#define DSTS 0x60cU

STATIC BOOLEAN Fits(CONST CR_DWC3_WINDOW *W, UINT32 Offset) {
  return W && W->Address && !(W->Address&3U) && W->Size>=4 &&
    W->Address<=(UINTN)-1-W->Size && !(Offset&3U) && Offset<=W->Size-4;
}
STATIC UINT32 Read(CONST CR_DWC3_WINDOW *W,UINT32 O,CR_DWC3_REPORT *R) {
  R->LastAddress=W->Address+O;
  R->LastValue=CrMmioRead32(R->LastAddress);
  return R->LastValue;
}
STATIC VOID Write(CONST CR_DWC3_WINDOW *W,UINT32 O,UINT32 V,CR_DWC3_REPORT *R) {
  R->Touched=TRUE;R->LastAddress=W->Address+O;R->LastValue=V;
  CrMmioWrite32(R->LastAddress,V);
}
STATIC CR_STATUS Update(CONST CR_DWC3_WINDOW *W,UINT32 O,UINT32 Mask,
    UINT32 V,CR_DWC3_REPORT *R) {
  Write(W,O,(Read(W,O,R)&~Mask)|(V&Mask),R);
  CrMemoryFence();
  return (Read(W,O,R)&Mask)==(V&Mask) ? CR_SUCCESS : CR_DEVICE_ERROR;
}
CR_STATUS CrDwc3Wait32(CONST CR_DWC3_WINDOW *W,UINT32 Offset,UINT32 Mask,
    UINT32 Expected,UINT32 TimeoutUs,UINT32 *LastValue) {
  if (!LastValue) return CR_INVALID_PARAMETER;
  *LastValue=0;
  if (CR_ERROR(CrMutexCheckLevel()) || !Fits(W,Offset) || !Mask ||
      (Expected&~Mask) || TimeoutUs>CR_DWC3_MAX_WAIT_US) return CR_INVALID_PARAMETER;
  /* Saturating remaining time handles zero/non-ten-multiple deadlines
     without an overflowing elapsed counter or an extra MMIO after timeout. */
  for (;;) {
    *LastValue=CrMmioRead32(W->Address+Offset);
    if ((*LastValue&Mask)==Expected) return CR_SUCCESS;
    if (!TimeoutUs) return CR_TIMEOUT;
    UINT32 Step=TimeoutUs<10?TimeoutUs:10;
    cr_sleep(Step);TimeoutUs-=Step;
  }
}
CR_STATUS CrDwc3Halt(CR_DWC3_ROLE Role,CONST CR_DWC3_WINDOW *W,UINT32 *LastValue) {
  if (!LastValue) return CR_INVALID_PARAMETER;
  *LastValue=0;
  if (CR_ERROR(CrMutexCheckLevel()) || (Role!=CrDwc3Host && Role!=CrDwc3Device) ||
      !Fits(W,Role==CrDwc3Host?0x84:DSTS)) return CR_INVALID_PARAMETER;
  UINT32 Control,Status,Run,Halted;
  if (Role==CrDwc3Host) {
    Control=CrMmioRead32(W->Address)&0xffU;
    if (Control<0x20 || Control>0x80 || (Control&3)) return CR_DEVICE_ERROR;
    Status=Control+4;Run=B(0);Halted=B(0);
  } else {Control=DCTL;Status=DSTS;Run=B(31);Halted=B(22);}
  UINT32 Value=CrMmioRead32(W->Address+Control);
  CrMmioWrite32(W->Address+Control,Value&~Run);CrMemoryFence();
  *LastValue=CrMmioRead32(W->Address+Control);
  if (*LastValue&Run) return CR_DEVICE_ERROR;
  return CrDwc3Wait32(W,Status,Halted,Halted,100000,LastValue);
}
CR_STATUS CrDwc3Configure(CONST CR_DWC3_REGIONS *Regions,CR_DWC3_ROLE Role,
    BOOLEAN SuperSpeed,CR_DWC3_REPORT *R) {
  if (!R) return CR_INVALID_PARAMETER;
  *R=(CR_DWC3_REPORT){0};
  if (CR_ERROR(CrMutexCheckLevel()) || !Regions || Regions->Soc!=0x8450 ||
      (Role!=CrDwc3Host && Role!=CrDwc3Device) || SuperSpeed>1 ||
      !Fits(&Regions->Global,DSTS) || !Fits(&Regions->Qscratch,0x30))
    return CR_INVALID_PARAMETER;
  CONST CR_DWC3_WINDOW *G=&Regions->Global,*Q=&Regions->Qscratch;
  if (G->Address<Q->Address+Q->Size && Q->Address<G->Address+G->Size)
    return CR_INVALID_PARAMETER;
  R->Id=Read(G,GSNPSID,R);
  if ((R->Id>>16)!=0x5533 && (R->Id>>16)!=0x3331) return CR_UNSUPPORTED;
  if ((Read(G,GHWPARAMS0,R)&3)!=2) return CR_UNSUPPORTED;
  R->Revision=R->Id;
  if ((R->Id>>16)==0x3331) {
    R->Revision=Read(G,VERSION,R);R->VersionType=Read(G,VERSION_TYPE,R);
    switch(R->Revision) {
      case 0x3137302a:case 0x3138302a:case 0x3139302a:case 0x3230302a:
        R->HostGap=Role==CrDwc3Host;break;
      default:break;
    }
    /* Android Waipio also applies USB2 retry-disable on 1.90a. Keep that
       exact qualified policy separate from generic 1.70a/1.80a behavior. */
    R->RetryDisable=R->HostGap && R->Revision!=0x3230302a;
  }
  R->HwParams1=Read(G,GHWPARAMS1,R);
  /* Generic Linux/Android and our function child manage event ring 0. NUM_INT
     is diagnostic, not permission to write every GEVNT register bank: the
     HDK reports 18 while GEVNTSIZ(4) is read-as-zero/write-ignored. The
     required wrapper reset retires inherited rings before this cold setup. */
  R->EventBuffers=MANAGED_EVENTS;
#define SET(W,O,M,V) do {CR_STATUS S=Update(W,O,M,V,R);if(S!=CR_SUCCESS)return S;}while(0)
  /* GCC wrapper reset and shared PHY reset belong to external owners. */
  SET(G,GCTL,(3U<<12)|B(3)|(3U<<4)|(3U<<6)|B(1),(UINT32)Role<<12);
  /* SM8450 HS Femto uses 8-bit UTMI. DT disables suspend-PHY/ENBLSLPM. */
  SET(G,USB2PHYCFG,B(31)|B(6)|B(8)|B(4)|B(3)|(15U<<10),9U<<10);
  SET(G,USB3PIPECTL,B(31)|B(17),0);
  SET(G,GUCTL1,B(26),SuperSpeed?0:B(26));
  if (R->HostGap) {
    SET(G,GUCTL1,7U<<21,1U<<21);
    R->Guctl3=Read(G,GUCTL3,R);
    if (R->RetryDisable) {
      Write(G,GUCTL3,R->Guctl3|B(16),R);CrMemoryFence();
      R->Guctl3=Read(G,GUCTL3,R);
      if (!(R->Guctl3&B(16))) return CR_DEVICE_ERROR;
    }
  }
  /* Qcom selects UTMI as PIPE clock for HS-only DP pin assignments. */
  SET(Q,8,B(8),B(8));cr_sleep(100);
  SET(Q,8,B(0)|B(3),SuperSpeed?0:B(0)|B(3));cr_sleep(100);
  SET(Q,8,B(8),0);
  SET(Q,0x10,B(20)|B(28),B(20)|B(28));SET(Q,0x30,B(24),B(24));
  if (Role==CrDwc3Device) {
    Write(G,DCTL,(Read(G,DCTL,R)&~B(31))|B(30),R);CrMemoryFence();
    R->LastAddress=G->Address+DCTL;
    CR_STATUS S=CrDwc3Wait32(G,DCTL,B(30),0,(R->Id>>16)==0x3331?200000:100000,&R->LastValue);
    if(S!=CR_SUCCESS)return S;
    /* Preserve the deployed adapter's conservative DWC31 synchronization
       delay; both upstream/Android require at least 50ms for <=1.80a. */
    if ((R->Id>>16)==0x3331)cr_sleep(50000);
  }
  /* Reset/old-role retirement must precede removal of inherited DMA rings. */
  for(UINT32 I=0;I<R->EventBuffers;++I) {
    UINT32 O=EVENTS+I*16;
    SET(G,O+8,B(31)|0xffffU,B(31));
    Write(G,O,0,R);Write(G,O+4,0,R);CrMemoryFence();
    if(Read(G,O,R) || Read(G,O+4,R)) return CR_DEVICE_ERROR;
    Write(G,O+12,Read(G,O+12,R)&0xfffcU,R);
    CrMemoryFence();if(Read(G,O+12,R)&0xfffcU)return CR_DEVICE_ERROR;
  }
  CrMemoryFence();R->Guctl1=Read(G,GUCTL1,R);
  return CR_SUCCESS;
#undef SET
}

CR_STATUS CrDwc3Retire(CONST CR_DWC3_REGIONS *Regions,CONST CR_DWC3_WINDOW *Xhci,
    CR_DWC3_ROLE Role,CR_USB_CONTROLLER_STOP *Stop,CR_STATUS *Halt,CR_STATUS *Irq) {
  if(!Stop || !Halt || !Irq || Halt==Irq)return CR_INVALID_PARAMETER;
  *Stop=(CR_USB_CONTROLLER_STOP){0};*Halt=*Irq=CR_NOT_READY;
  if(CR_ERROR(CrMutexCheckLevel()) || !Regions || Regions->Soc!=0x8450 ||
     (Role!=CrDwc3Host && Role!=CrDwc3Device) || !Fits(&Regions->Global,0x620))
    return CR_INVALID_PARAMETER;
  CONST CR_DWC3_WINDOW *G=&Regions->Global;
  UINT32 Cap=0,Id=CrMmioRead32(G->Address+GSNPSID);
  UINT32 Mode=CrMmioRead32(G->Address+GCTL);
  UINT32 Events=MANAGED_EVENTS;
  if(((Id>>16)!=0x5533 && (Id>>16)!=0x3331) ||
     ((Mode>>12)&3U)!=(UINT32)Role ||
     (CrMmioRead32(G->Address+GHWPARAMS0)&3U)!=2)return CR_UNSUPPORTED;
  if(Role==CrDwc3Host) {
    if(!Fits(Xhci,0x84) ||
       (G->Address<Xhci->Address+Xhci->Size && Xhci->Address<G->Address+G->Size))
      return CR_INVALID_PARAMETER;
    Cap=CrMmioRead32(Xhci->Address)&255U;
    if(Cap<0x20 || Cap>0x80 || (Cap&3U))return CR_DEVICE_ERROR;
  }
  Stop->Role=(UINT32)Role;Stop->Id=Id;Stop->GlobalControl=Mode;
  Stop->Revision=(Id>>16)==0x3331?CrMmioRead32(G->Address+VERSION):Id;
  Stop->EventBuffers=Events;
  UINT32 Last;
  *Halt=CrDwc3Halt(Role,Role==CrDwc3Host?Xhci:G,&Last);
  if(Role==CrDwc3Host) {
    UINT32 Cmd=CrMmioRead32(Xhci->Address+Cap);
    CrMmioWrite32(Xhci->Address+Cap,Cmd&~(B(0)|B(2)|B(3)));
  } else {
    CrMmioWrite32(G->Address+0x608,0); // DEVTEN
    CrMmioWrite32(G->Address+0x620,0); // DALEPENA
  }
  for(UINT32 I=0;I<Events;++I) {
    UINTN Address=G->Address+EVENTS+I*16+8;
    CrMmioWrite32(Address,CrMmioRead32(Address)|B(31));
  }
  CrMemoryFence();
  for(UINT32 I=0;I<Events;++I)
    if(CrMmioRead32(G->Address+EVENTS+I*16+8)&B(31))++Stop->MaskedEventBuffers;
  if(Role==CrDwc3Host) {
    Stop->HostCommand=CrMmioRead32(Xhci->Address+Cap);
    Stop->HostStatus=CrMmioRead32(Xhci->Address+Cap+4);
    *Irq=!(Stop->HostCommand&(B(2)|B(3)))?CR_SUCCESS:CR_DEVICE_ERROR;
  } else {
    Stop->DeviceControl=CrMmioRead32(G->Address+DCTL);
    Stop->DeviceStatus=CrMmioRead32(G->Address+DSTS);
    Stop->DeviceEvents=CrMmioRead32(G->Address+0x608);
    Stop->DeviceEndpoints=CrMmioRead32(G->Address+0x620);
    *Irq=!Stop->DeviceEvents && !Stop->DeviceEndpoints ? CR_SUCCESS:CR_DEVICE_ERROR;
  }
  if(Stop->MaskedEventBuffers!=Events || CrMmioRead32(G->Address+GSNPSID)!=Id ||
     CrMmioRead32(G->Address+GCTL)!=Mode)*Irq=CR_DEVICE_ERROR;
  return *Halt!=CR_SUCCESS?*Halt:*Irq;
}
