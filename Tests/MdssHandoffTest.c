/* Actual pointer-free record builder/parser with adversarial native evidence. MIT */
#include <Library/MdssHandoffCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
static CR_MDSS_NATIVE_HANDOFF R,Scratch,Out,Saved;
static CR_MDSS_HANDOFF_PARTICIPANT P;
static unsigned Fences,TearAt;
#ifndef CR_MDSS_TEST_NO_FENCE
#ifdef _KERNEL_MODE
VOID KeMemoryBarrier(VOID){atomic_thread_fence(memory_order_seq_cst);if(++Fences==TearAt)R.Sequence+=2;}
#else
VOID EFIAPI MemoryFence(VOID){atomic_thread_fence(memory_order_seq_cst);if(++Fences==TearAt)R.Sequence+=2;}
#endif
#endif
static void Text(CHAR8 *D,size_t N,const char *S){assert(strlen(S)<N);memcpy(D,S,strlen(S));}
static void RouteSnapshot(UINT32 Owner,UINT32 State){
  memset(&P,0,sizeof(P));P.Owner=Owner;CR_MDSS_NATIVE_ROUTE *Q=&P.Route;Q->Owner=Owner;Q->State=State;
  Q->Flags=State==CR_MDSS_NATIVE_LIVE?CR_MDSS_NATIVE_OWNED:0;
  Q->Dpu=0xae01000;Q->DpuBytes=0x8f000;Q->InterfaceBase=Q->Dpu+(Owner==1?0x35000:0x34000);
  Q->PipeBase=Q->Dpu+(Owner==1?0x24000:0x26000);Q->CtlBase=Q->Dpu+(Owner==1?0x15000:0x16000);
  Q->MixerBase=Q->Dpu+(Owner==1?0x44000:0x46000);Q->InterfaceBytes=Q->PipeBytes=Q->CtlBytes=Q->MixerBytes=0x400;
  Q->LinkBase=Owner==1?0xae94000:0xae90000;Q->LinkBytes=Owner==1?0x400:0x1400;
  Q->PhyBase=Owner==1?0xae94400:0x88eaa00;Q->PhyBytes=0x800;
  Q->PllBase=Owner==1?0xae94900:0x88e9000;Q->PllBytes=0x200;
  Q->Mode=(CR_MDSS_NATIVE_MODE){1920,1080,7680,CR_MDSS_NATIVE_FORMAT_BGRX8888,88,44,148,4,5,36,148500000};
  Q->FrameBuffer=Owner==1?0xfc800000:0xfd000000;Q->FrameBufferBytes=8294400;
  Q->DeviceAddress=Owner==1?0x10000000:0x10800000;
  P.ClockCount=12;for(UINT32 I=0;I<P.ClockCount;++I){P.Clocks[I].Owner=Owner;P.Clocks[I].Flags=1;
    Text(P.Clocks[I].Controller,8,"disp_cc");char S[24];snprintf(S,sizeof(S),"route%u_clock%u",Owner,I);Text(P.Clocks[I].Name,48,S);P.Clocks[I].RateHz=19200000;}
  if(Owner==1){P.RailCount=2;for(UINT32 I=0;I<2;++I){P.Rails[I].Owner=Owner;P.Rails[I].Flags=1;P.Rails[I].Millivolts=880;P.Rails[I].Mode=7;
    Text(P.Rails[I].Name,24,I?"ldoh4":"ldoh3");}}
  if(Owner==1){P.DomainCount=1;P.Domains[0].Owner=Owner;P.Domains[0].Flags=1;
    Text(P.Domains[0].Controller,8,"dispcc");Text(P.Domains[0].Name,32,"disp_cc_mdss_core_gdsc");}
  P.PathCount=1;P.Paths[0].Owner=Owner;P.Paths[0].Flags=1;P.Paths[0].Source=0x8000;P.Paths[0].Destination=0x8001;
  Text(P.Paths[0].Provider,24,"qcom,sm8450-mdss");P.Paths[0].Average=50000;P.Paths[0].Peak=100000;
}
static void MappingSnapshot(void){
  static const UINT16 Sids[4]={0x2800,0x2802,0x2c00,0x2c02};
  memset(&P,0,sizeof(P));P.Owner=8;
  for(UINT32 N=0;N<2;++N){CR_MDSS_NATIVE_MAPPING *M=&P.Mappings[N];M->Owner=N?2:1;
    M->State=N?CR_MDSS_NATIVE_MAPPING_REVOKED:CR_MDSS_NATIVE_MAPPING_PRESERVED;M->MappingId=N+1;M->StreamCount=4;
    for(UINT32 I=0;I<4;++I){SmmuMappingEvidence *E=&M->Streams[I];E->Base=0x15000000;E->BankBase=0x15080000+I*4096;
      E->Tables=0xfe000000+I*0x22000;E->TableBytes=0x22000;E->Iova=N?0x10800000:0x10000000;
      E->Physical=N?0:0xfc800000;E->Bytes=8294400;E->Access=N?0:1;E->Sid=Sids[I];E->Bank=I;E->Stream=I;E->Asid=I+1;
      E->Smr=0x80000000|E->Sid;E->S2cr=I;E->Sctlr=0x27;E->SctlrIgnoreMask=0x1000;E->Cbar=0x1f300;
      E->Cba2r=1;E->Tcr=0x803519;E->Tcr2=0x38013;E->Ttbr0=E->Tables|((UINT64)E->Asid<<48);E->Ttbr1=(UINT64)E->Asid<<48;E->Mair0=0x44;}
  }
}
static void BridgeSnapshot(void){
  memset(&P,0,sizeof(P));P.Owner=4;P.Bridge=(CR_MDSS_NATIVE_BRIDGE_STATE){1,1,10,0x2b,400000,500,9,109,107,44,7,0};
}
static void PhySnapshot(void){
  memset(&P,0,sizeof(P));P.Owner=16;P.Phy=(CR_MDSS_NATIVE_PHY_STATE){.State=1,.Flags=1,.UsbOwners=1,.Base=0x88e8000,.Bytes=0x4000,.Orientation=1,
    .ReadyMask=CR_MDSS_NATIVE_PHY_COMMON|CR_MDSS_NATIVE_PHY_USB|CR_MDSS_NATIVE_PHY_FSA,
    .TouchedMask=CR_MDSS_NATIVE_PHY_COMMON|CR_MDSS_NATIVE_PHY_USB|CR_MDSS_NATIVE_PHY_FSA};
  P.ClockCount=4;for(UINT32 I=0;I<4;++I){P.Clocks[I].Owner=16;P.Clocks[I].Flags=1;Text(P.Clocks[I].Controller,8,"gcc");
    char S[24];snprintf(S,sizeof(S),"phy_clock%u",I);Text(P.Clocks[I].Name,48,S);P.Clocks[I].RateHz=19200000;}
  P.RailCount=6;for(UINT32 I=0;I<6;++I){P.Rails[I].Owner=16;P.Rails[I].Flags=1;P.Rails[I].Millivolts=1800;P.Rails[I].Mode=7;
    char S[24];snprintf(S,sizeof(S),"phy_rail%u",I);Text(P.Rails[I].Name,24,S);}
}
static void Build(unsigned MappingFirst){
  memset(&R,0,sizeof(R));assert(CrMdssHandoffInit(&R,13,19,0xfc400000,8192,31,31)==CR_SUCCESS);
  if(MappingFirst){MappingSnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);}
  RouteSnapshot(1,1);assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
  RouteSnapshot(2,2);assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
  BridgeSnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
  PhySnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
  if(!MappingFirst){MappingSnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);}
  assert(CrMdssHandoffFinalize(&R,2)==CR_SUCCESS);assert(CrMdssHandoffValidate(&R)==CR_SUCCESS);assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
}
static void Recrc(void){R.Crc32=CrMdssHandoffCrc32(&R);}
static void BadSchema(void){Recrc();assert(CrMdssHandoffValidate(&R)!=CR_SUCCESS);R=Saved;}
int main(void){
  Build(0);Saved=R;assert(R.ClockCount==28&&R.RailCount==8&&R.Routes[0].ClockFirst==0&&R.Routes[1].ClockFirst==12);
  assert(CrMdssHandoffInit(&R,13,19,0xfc400000,8192,31,31)==CR_ALREADY_STARTED);
  assert(CrMdssHandoffFinalize(&R,4)==CR_ALREADY_STARTED);
  memset(&Out,0x5a,sizeof(Out));CR_MDSS_NATIVE_HANDOFF Preserved=Out;
  Fences=0;TearAt=0;assert(CrMdssHandoffSnapshot(&R,sizeof(R),&Scratch,&Out)==CR_SUCCESS&&!memcmp(&Out,&R,sizeof(R)));
  Out=Preserved;R.Sequence=3;assert(CrMdssHandoffSnapshot(&R,sizeof(R),&Scratch,&Out)==CR_NOT_READY&&!memcmp(&Out,&Preserved,sizeof(Out)));R=Saved;
  TearAt=2;Fences=0;assert(CrMdssHandoffSnapshot(&R,sizeof(R),&Scratch,&Out)==CR_NOT_READY&&!memcmp(&Out,&Preserved,sizeof(Out)));R=Saved;TearAt=0;
  assert(CrMdssHandoffSnapshot(&R,sizeof(R)-1,&Scratch,&Out)==CR_INVALID_PARAMETER);
  assert(CrMdssHandoffSnapshot(&R,sizeof(R),&R,&Out)==CR_INVALID_PARAMETER);
  R.Crc32^=1;assert(CrMdssHandoffValidate(&R)==CR_COMPROMISED_DATA);R=Saved;
  R.ClockCount=33;BadSchema();R.RailCount=9;BadSchema();R.PathCount=5;BadSchema();R.DomainCount=5;BadSchema();
  R.RequiredOwners=15;BadSchema();R.FrozenOwners=15;BadSchema();R.RegisteredOwners=63;BadSchema();R.Reserved1=1;BadSchema();
  R.RecordPhysical=CR_MAX_UINT64-4095;BadSchema();R.RecordBytes=4096;BadSchema();R.RecordPhysical|=1;BadSchema();
  R.Routes[1].ClockFirst=0;BadSchema();R.Routes[0].ClockCount=0xffffffffU;BadSchema();R.Clocks[0].Owner=16;BadSchema();
  R.Clocks[0].Name[47]='x';BadSchema();R.Clocks[28].Owner=1;BadSchema();R.Clocks[1]=R.Clocks[0];BadSchema();
  R.Routes[0].InterfaceBytes=0;BadSchema();R.Routes[0].PipeBase+=4;BadSchema();R.Routes[0].PllBase=CR_MAX_UINT64-3;BadSchema();
  R.Routes[0].Mode.Format=2;BadSchema();R.Routes[0].Mode.Stride=1;BadSchema();R.Routes[0].FrameBufferBytes=1;BadSchema();
  R.Routes[0].MappingOwner=1;BadSchema();R.Routes[0].Streams[0].Physical+=4096;BadSchema();R.Routes[1].Streams[0].Access=1;BadSchema();
  R.Routes[0].Streams[0].Smr^=1;BadSchema();R.Routes[0].Streams[0].Ttbr0^=4096;BadSchema();R.Routes[0].Streams[1]=R.Routes[0].Streams[0];BadSchema();
  R.Routes[0].Streams[0].SctlrIgnoreMask=0xffffffffU;BadSchema();R.Bridge.IrqGpio=24;BadSchema();
  R.Phy.Base+=4;BadSchema();R.Phy.Bytes=0x1000;BadSchema();R.Phy.UsbOwners=2;BadSchema();R.Phy.DpOwners=2;BadSchema();
  R.Phy.ReadyMask|=32;BadSchema();R.Phy.TouchedMask|=16;BadSchema();R.Phy.ReadyMask=CR_MDSS_NATIVE_PHY_USB_SS;BadSchema();
  R.Phy.UsbOwners=0;BadSchema();R.Phy.TouchedMask=0;BadSchema();R.Phy.Orientation=0;BadSchema();
  R.Phy.LaneCount=3;BadSchema();R.Phy.LinkRate=2700;BadSchema();R.Phy.Status=0x8000000000000007ULL;BadSchema();
  R.Phy.State=CR_MDSS_NATIVE_UNCERTAIN;R.Phy.Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;R.Phy.Status=0x8000000000000007ULL;
  Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  R.Phy.State=CR_MDSS_NATIVE_UNCERTAIN;R.Phy.Flags|=CR_MDSS_NATIVE_RETAINED_ERROR;R.Phy.Status=7;BadSchema();
  R.Routes[0].State=3;Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  /* Preserve actual DSI1 diagnostics through both append orders, including
     a revoked frame mapping. Qualified legacy claims stay controller0. */
  for(UINT32 MappingFirst=0;MappingFirst<2;++MappingFirst){
    memset(&R,0,sizeof(R));assert(CrMdssHandoffInit(&R,13,19,0xfc400000,8192,31,31)==CR_SUCCESS);
    if(MappingFirst){MappingSnapshot();P.Mappings[0].State=CR_MDSS_NATIVE_MAPPING_REVOKED;
      for(UINT32 I=0;I<4;++I){P.Mappings[0].Streams[I].Physical=0;P.Mappings[0].Streams[I].Access=0;}
      assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);}
    RouteSnapshot(1,CR_MDSS_NATIVE_UNCERTAIN);P.Route.Flags=CR_MDSS_NATIVE_OWNED|CR_MDSS_NATIVE_RETAINED_ERROR;
    P.Route.Controller=1;P.Route.InterfaceBase+=0x1000;P.Route.LinkBase+=0x2000;
    P.Route.PhyBase+=0x2000;P.Route.PllBase+=0x2000;
    CR_MDSS_HANDOFF_PARTICIPANT Diagnostic=P;CR_MDSS_NATIVE_HANDOFF Before=R;
    P.Route.State=CR_MDSS_NATIVE_LIVE;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Before,sizeof(R)));
    P=Diagnostic;P.Route.InterfaceBase-=0x1000;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Before,sizeof(R)));
    P=Diagnostic;P.Route.Controller=2;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Before,sizeof(R)));
    P=Diagnostic;assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
    RouteSnapshot(2,CR_MDSS_NATIVE_STOPPED);assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
    BridgeSnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);PhySnapshot();assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);
    if(!MappingFirst){MappingSnapshot();P.Mappings[0].State=CR_MDSS_NATIVE_MAPPING_REVOKED;
      for(UINT32 I=0;I<4;++I){P.Mappings[0].Streams[I].Physical=0;P.Mappings[0].Streams[I].Access=0;}
      assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);}
    assert(CrMdssHandoffFinalize(&R,2)==CR_SUCCESS&&CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);
  }
  R=Saved;
  R.Routes[1].MappingState=0;R.Routes[1].MappingStatus=0x8000000000000006ULL;R.Routes[1].StreamCount=0;memset(R.Routes[1].Streams,0,sizeof(R.Routes[1].Streams));
  Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  R.Routes[0].StreamCount=3;memset(&R.Routes[0].Streams[3],0,sizeof(R.Routes[0].Streams[3]));Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  R.Clocks[0].Flags|=2;Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  /* Source DP has no independent GDSC journal. Live mapping qualification
     uses the actual retained shared core-domain owner in the aggregate. */
  assert(!R.Routes[1].DomainCount&&R.DomainCount==1);
  R.Routes[1].State=CR_MDSS_NATIVE_LIVE;R.Routes[1].Flags=CR_MDSS_NATIVE_OWNED;
  R.Routes[1].MappingState=CR_MDSS_NATIVE_MAPPING_PRESERVED;
  for(UINT32 I=0;I<4;++I){R.Routes[1].Streams[I].Physical=R.Routes[1].FrameBuffer;R.Routes[1].Streams[I].Access=SMMU_ACCESS_READ;}
  Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
  memset(R.Domains[0].Name,0,sizeof(R.Domains[0].Name));Text(R.Domains[0].Name,32,"mdss_int2_gdsc");
  Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);
  memset(R.Domains,0,sizeof(R.Domains));R.DomainCount=0;R.Routes[0].DomainCount=0;
  Recrc();assert(CrMdssHandoffValidate(&R)==CR_SUCCESS&&CrMdssHandoffCanClaim(&R)==CR_NOT_READY);R=Saved;
  Build(1);assert(CrMdssHandoffCanClaim(&R)==CR_SUCCESS);
  memset(&R,0,sizeof(R));assert(CrMdssHandoffInit(&R,1,1,0xfc400000,8192,31,31)==CR_SUCCESS);Saved=R;
  assert(CrMdssHandoffFinalize(&R,2)==CR_NOT_READY&&!memcmp(&R,&Saved,sizeof(R)));
  RouteSnapshot(1,1);P.ClockCount=33;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Saved,sizeof(R)));
  RouteSnapshot(1,1);P.Clocks[1]=P.Clocks[0];assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Saved,sizeof(R)));
  RouteSnapshot(1,1);P.Route.MappingId=1;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Saved,sizeof(R)));
  RouteSnapshot(1,1);assert(CrMdssHandoffAppend(&R,&P)==CR_SUCCESS);Saved=R;
  assert(CrMdssHandoffAppend(&R,&P)==CR_ALREADY_STARTED&&!memcmp(&R,&Saved,sizeof(R)));
  MappingSnapshot();P.Mappings[0].Streams[0].Physical+=4096;assert(CrMdssHandoffAppend(&R,&P)==CR_INVALID_PARAMETER&&!memcmp(&R,&Saved,sizeof(R)));
  puts("PASS actual native MDSS handoff: exact layout, owner normalization, live/revoked evidence, torn publication, retained errors and adversarial schema");
  return 0;
}
