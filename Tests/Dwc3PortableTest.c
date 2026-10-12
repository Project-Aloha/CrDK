/* Actual portable USB core: split MMIO ownership, cold configuration and faults.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/Dwc3CrLib.h>
#include <oskal/cr_memory.h>
static UINT32 Global[0x624/4],Scratch[0x34/4],Host[0x88/4];
static CONST CR_DWC3_REGIONS Regions={0x8450,{0x100002000ULL,sizeof(Global)},{0x300005000ULL,sizeof(Scratch)}};
static CONST CR_DWC3_WINDOW Xhci={0x200007000ULL,sizeof(Host)};
static UINT32 Reads,Writes,FailWrite,Time,WaitReadyAt,FailReadback;
static BOOLEAN ResetStuck,HaltStuck;
static UINTN DroppedAddress;
#ifdef _KERNEL_MODE
static KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID){return Irql;}
VOID KeStallExecutionProcessor(ULONG Us){Time+=Us;}
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE M,BOOLEAN A,LARGE_INTEGER *I){(void)M;(void)A;Time+=(UINT32)(-I->QuadPart/10);return STATUS_SUCCESS;}
#else
VOID EFIAPI MemoryFence(VOID){}
UINTN EFIAPI MicroSecondDelay(UINTN Us){Time+=(UINT32)Us;return Us;}
#endif
static UINT32 *At(UINTN Address) {
 if(Address>=Regions.Global.Address&&Address<Regions.Global.Address+sizeof(Global))return &Global[(Address-Regions.Global.Address)/4];
 if(Address>=Regions.Qscratch.Address&&Address<Regions.Qscratch.Address+sizeof(Scratch))return &Scratch[(Address-Regions.Qscratch.Address)/4];
 assert(Address>=Xhci.Address&&Address<Xhci.Address+sizeof(Host));return &Host[(Address-Xhci.Address)/4];
}
static UINT32 ReadIo(UINTN A) {
 assert(!(A&3));++Reads;
 assert(A<Regions.Global.Address+0x310 || A>=Regions.Global.Address+0x500);
 if(WaitReadyAt && A==Regions.Global.Address+0x44)return Time>=WaitReadyAt?1:0;
 if(A==Xhci.Address+0x44)return HaltStuck?0:(!(Host[0x40/4]&1)?1:0);
 if(A==Regions.Global.Address+0x60c)return HaltStuck?0:(!(Global[0x604/4]&(1U<<31))?(1U<<22):0);
 UINT32 V=*At(A);
 if(FailReadback==Writes && Writes && A==Regions.Global.Address+0x10)return V^(1U<<12);
 return V;
}
static UINT32 WriteIo(UINTN A,UINT32 V) {
 assert(!(A&3));++Writes;
 assert(A<Regions.Global.Address+0x310 || A>=Regions.Global.Address+0x500);
 if(Writes==FailWrite){DroppedAddress=A;return V;}
 if(A==Regions.Global.Address+0x604&&!ResetStuck)V&=~(1U<<30);
 if(A>=Regions.Global.Address+0x300&&A<Regions.Global.Address+0x500&&((A-Regions.Global.Address)&15)==12){*At(A)-=V&0xfffc;return V;}
 *At(A)=V;return V;
}
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A){return ReadIo(A);}
UINT32 CrMmioWrite32(UINTN A,UINT32 V){return WriteIo(A,V);}
#else
UINT32 EFIAPI MmioRead32(UINTN A){return ReadIo(A);}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){return WriteIo(A,V);}
#endif
static VOID Reset(UINT32 Events) {
 memset(Global,0,sizeof(Global));memset(Scratch,0,sizeof(Scratch));memset(Host,0,sizeof(Host));
 Global[0x20/4]=0x33313130;Global[0x40/4]=2;Global[0x44/4]=Events<<15;
 Global[0xa0/4]=0x3139302a;Global[0xa4/4]=0x67610000;Host[0]=0x40;
 Global[0x10/4]=0xffffffff;Global[0x100/4]=0xffffffff;Global[0x1c0/4]=0xffffffff;
 Scratch[8/4]=0x1000;
 for(UINT32 I=0;I<32;++I){Global[(0x300+I*16)/4]=0xa5a50000+I*4096;Global[(0x304+I*16)/4]=7;Global[(0x308+I*16)/4]=4096;Global[(0x30c+I*16)/4]=16;}
 Reads=Writes=Time=FailWrite=WaitReadyAt=FailReadback=0;DroppedAddress=0;ResetStuck=HaltStuck=FALSE;
}
static VOID RetireTest(VOID) {
 CR_USB_CONTROLLER_STOP Stop;CR_STATUS Halt,Irq;
 for(UINT32 Role=1;Role<=2;++Role)for(UINT32 N=0;N<=63;++N) {
  Reset(N);Global[0x10/4]=Role<<12;Host[0x40/4]=13;
  Global[0x604/4]=1U<<31;Global[0x608/4]=0xffff;Global[0x620/4]=0xffffffff;
  assert(CrDwc3Retire(&Regions,&Xhci,(CR_DWC3_ROLE)Role,&Stop,&Halt,&Irq)==CR_SUCCESS);
  assert(Halt==CR_SUCCESS && Irq==CR_SUCCESS && Stop.Role==Role && Stop.EventBuffers==1 && Stop.MaskedEventBuffers==1);
  for(UINT32 I=0;I<32;++I)assert(Global[(0x300+I*16)/4]==0xa5a50000+I*4096 && Global[(0x304+I*16)/4]==7 && Global[(0x30c+I*16)/4]==16);
  if(Role==1)assert(Stop.HostCommand==0 && (Stop.HostStatus&1));
  else assert(Stop.DeviceControl==0 && (Stop.DeviceStatus&(1U<<22)) && !Stop.DeviceEvents && !Stop.DeviceEndpoints);
  UINT32 Count=Writes;
  for(UINT32 W=1;W<=Count;++W){
   Reset(N);Global[0x10/4]=Role<<12;Host[0x40/4]=13;
   Global[0x604/4]=1U<<31;Global[0x608/4]=0xffff;Global[0x620/4]=0xffffffff;FailWrite=W;
   assert(CrDwc3Retire(&Regions,&Xhci,(CR_DWC3_ROLE)Role,&Stop,&Halt,&Irq)!=CR_SUCCESS);
   assert(Halt!=CR_SUCCESS || Irq!=CR_SUCCESS);
  }
  Reset(N);Global[0x10/4]=Role<<12;HaltStuck=TRUE;
  assert(CrDwc3Retire(&Regions,&Xhci,(CR_DWC3_ROLE)Role,&Stop,&Halt,&Irq)==CR_TIMEOUT && Time==100000);
 }
 for(UINT32 Mode=0;Mode<=3;++Mode)if(Mode!=2){
  Reset(18);Global[0x10/4]=1U<<12;Global[0x40/4]=Mode;
  assert(CrDwc3Retire(&Regions,&Xhci,CrDwc3Host,&Stop,&Halt,&Irq)==CR_UNSUPPORTED && !Writes);
 }
 Reset(1);Global[0x10/4]=2U<<12;
 assert(CrDwc3Retire(&Regions,&Xhci,CrDwc3Host,&Stop,&Halt,&Irq)==CR_UNSUPPORTED && !Writes);
 CR_DWC3_REGIONS Bad=Regions;Bad.Global.Size=0x623;Reset(1);
 assert(CrDwc3Retire(&Bad,&Xhci,CrDwc3Host,&Stop,&Halt,&Irq)==CR_INVALID_PARAMETER && !Reads && !Writes);
#ifdef _KERNEL_MODE
 Reset(1);Irql=DISPATCH_LEVEL;
 assert(CrDwc3Retire(&Regions,&Xhci,CrDwc3Host,&Stop,&Halt,&Irq)==CR_INVALID_PARAMETER && !Reads && !Writes);
 Irql=PASSIVE_LEVEL;
#endif
 puts("DWC3 retire: 128 role/interrupt-count layouts, every dropped retirement write, retained ring addresses, bounded stuck halt, invalid hardware-mode/role/view/IRQL PASS");
}
int main(void) {
 RetireTest();
 CR_DWC3_REPORT R;UINT32 Last;
 for(UINT32 N=0;N<=63;++N)for(UINT32 Role=1;Role<=2;++Role)for(UINT32 Ss=0;Ss<=1;++Ss) {
   Reset(N);assert(CrDwc3Configure(&Regions,(CR_DWC3_ROLE)Role,(BOOLEAN)Ss,&R)==CR_SUCCESS);
   assert(R.Touched&&R.Id==0x33313130&&R.Revision==0x3139302a&&R.EventBuffers==1&&R.HwParams1==(N<<15));
   assert(R.HostGap==(Role==1)&&R.RetryDisable==(Role==1));
   assert(((Global[0x10/4]>>12)&3)==Role);
   assert((Scratch[2]&(1|8|256))==(Ss?0:9));
   assert(!!(Global[0x1c/4]&(1U<<26))==!Ss);
   for(UINT32 I=0;I<1;++I){assert(Global[(0x300+I*16)/4]==0&&Global[(0x304+I*16)/4]==0);assert((Global[(0x308+I*16)/4]&0x8000ffffU)==0x80000000U&&Global[(0x30c+I*16)/4]==0);}
   for(UINT32 I=1;I<32;++I){assert(Global[(0x300+I*16)/4]==0xa5a50000+I*4096);assert(Global[(0x308+I*16)/4]==4096);}
   assert(CrDwc3Halt((CR_DWC3_ROLE)Role,Role==1?&Xhci:&Regions.Global,&Last)==CR_SUCCESS);
 }
 /* Validate every mapped boundary before IO. Virtual mappings need not be
    contiguous and cannot be inferred from a physical base in the driver. */
 for(UINT32 I=0;I<8;++I){Reset(1);CR_DWC3_REGIONS Bad=Regions;
   switch(I){case 0:Bad.Global.Address=0;break;case 1:++Bad.Global.Address;break;case 2:Bad.Global.Size=0x60f;break;case 3:Bad.Qscratch.Size=0x33;break;case 4:Bad.Global.Address=(UINTN)-4;break;case 5:Bad.Qscratch.Address=Bad.Global.Address+0x100;break;case 6:Bad.Soc=0;break;case 7:Bad.Qscratch.Address=0;break;}
   assert(CrDwc3Configure(&Bad,CrDwc3Host,TRUE,&R)==CR_INVALID_PARAMETER&&!R.Touched&&!Reads&&!Writes);
 }
 Reset(1);assert(CrDwc3Configure(&Regions,0,TRUE,&R)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
 assert(CrDwc3Configure(&Regions,CrDwc3Host,2,&R)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
 /* Drop each cold-programming write. Seed fields that otherwise match so
    discarded writes cannot masquerade as the requested operation. */
 Reset(2);Scratch[0x10/4]=Scratch[0x30/4]=0;
 Global[0x1c/4]=1U<<26;
 assert(CrDwc3Configure(&Regions,CrDwc3Host,TRUE,&R)==CR_SUCCESS);UINT32 Total=Writes,Detected=0;
 for(UINT32 I=1;I<=Total;++I){Reset(2);Global[0x1c/4]=1U<<26;FailWrite=I;
   CR_STATUS S=CrDwc3Configure(&Regions,CrDwc3Host,TRUE,&R);
   /* Some writes are idempotent (PIPE selector already SS); they may
      succeed only when the requested readback already matches. */
   if(DroppedAddress>=Regions.Global.Address+0x300 && DroppedAddress<Regions.Global.Address+0x310)
     assert(S==CR_DEVICE_ERROR); /* Every managed ring mutation is checked. */
   if(S==CR_DEVICE_ERROR){assert(R.Touched);++Detected;}else assert(S==CR_SUCCESS);
 }
 assert(Detected>0);
 Reset(1);ResetStuck=TRUE;
 assert(CrDwc3Configure(&Regions,CrDwc3Device,FALSE,&R)==CR_TIMEOUT&&R.Touched&&Time==200200);
 assert(Global[0x300/4]!=0); /* No event-ring clearing after reset timeout. */
 Reset(0);Host[0x40/4]=1;FailWrite=1;
 assert(CrDwc3Halt(CrDwc3Host,&Xhci,&Last)==CR_DEVICE_ERROR&&(Last&1));
 Reset(0);HaltStuck=TRUE;
 assert(CrDwc3Halt(CrDwc3Host,&Xhci,&Last)==CR_TIMEOUT&&Time==100000);
 Reset(0);Global[0x604/4]=1U<<31;FailWrite=1;
 assert(CrDwc3Halt(CrDwc3Device,&Regions.Global,&Last)==CR_DEVICE_ERROR&&(Last&(1U<<31)));
 for(UINT32 Cap=0;Cap<=255;++Cap){Reset(0);Host[0]=Cap;
   CR_STATUS S=CrDwc3Halt(CrDwc3Host,&Xhci,&Last);
   if(Cap<0x20||Cap>0x80||(Cap&3))assert(S==CR_DEVICE_ERROR&&!Writes);
 }
 /* Waits use the exact finite deadline, including a final read there. */
 Reset(0);WaitReadyAt=7;
 assert(CrDwc3Wait32(&Regions.Global,0x44,1,1,7,&Last)==CR_SUCCESS&&Last==1&&Time==7&&Reads==2);
 Reset(0);WaitReadyAt=8;
 assert(CrDwc3Wait32(&Regions.Global,0x44,1,1,7,&Last)==CR_TIMEOUT&&Time==7&&Reads==2);
 Reset(0);assert(CrDwc3Wait32(&Regions.Global,0x44,1,1,0,&Last)==CR_TIMEOUT&&!Time&&Reads==1);
 Reset(0);assert(CrDwc3Wait32(&Regions.Global,0x44,1,1,CR_MAX_UINT32,&Last)==CR_INVALID_PARAMETER&&!Reads);
 assert(CrDwc3Wait32(&Regions.Global,0x44,1,2,1,&Last)==CR_INVALID_PARAMETER&&!Reads);
 assert(CrDwc3Wait32(&Regions.Global,0x44,0,0,1,&Last)==CR_INVALID_PARAMETER&&!Reads);
#ifdef _KERNEL_MODE
 Reset(1);Irql=DISPATCH_LEVEL;
 assert(CrDwc3Configure(&Regions,CrDwc3Host,TRUE,&R)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
 assert(CrDwc3Halt(CrDwc3Host,&Xhci,&Last)==CR_INVALID_PARAMETER&&!Reads&&!Writes);
 assert(CrDwc3Wait32(&Regions.Global,0x44,1,1,1,&Last)==CR_INVALID_PARAMETER&&!Reads);
 Irql=PASSIVE_LEVEL;
#endif
 printf("DWC3 portable: 256 role/SS/interrupt-count configurations, only managed ring0 accessed, split bounds, %u/%u dropped writes detected, halt/reset/deadline/IRQL PASS\n",Detected,Total);
 return 0;
}
