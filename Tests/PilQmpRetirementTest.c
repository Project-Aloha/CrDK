/* Source-bound consumer retirement model; remote state is not a local echo.
   SPDX-License-Identifier: MIT */
#define main ExistingPilMain
#include "PilCoreTest.c"
#undef main
static UINT32 DownWrites;
static BOOLEAN HoldOff;
static CR_PIL_CONTEXT *Active;
static UINT32 FaultOffset,FaultValue,FaultReads;
static BOOLEAN Inject,SecondPass;
static UINT32 PeerRead(VOID *C,UINT32 A) {
  UINT32 V=Read32(C,A);
  if(Inject && Active && AlreadyStopped && !Active->LoadState && A==Target.QmpBase+FaultOffset) {
    ++FaultReads;
    if(!SecondPass || !(FaultReads&1)) return FaultValue;
  }
  return V;
}
static VOID PeerWrite(VOID *C,UINT32 A,UINT32 V) {
  if(A!=Target.IpccSend || V!=0) {Write32(C,A,V);return;}
  /* AOSS transport service remains alive after PAS shutdown. A peer may
     process channel work only while its application link stays established;
     consumer release is not a transport-provider removal operation. */
  if(Qmp[0x24/4]!=0xffff) {++DownWrites;return;}
  Qmp[0x28/4]=0xffff;Qmp[0x14/4]=0xffff;
  Qmp[0x30/4]=Qmp[0x2c/4];
  if(Qmp[0x2c/4]==0xffff && Qmp[0x80/4] && !HoldOff) Qmp[0x80/4]=0;
}
static VOID Setup(CR_PIL_CONTEXT *P,CR_PIL_IMAGE *I,VOID *M) {
  Fixture();DownWrites=0;HoldOff=Inject=SecondPass=FALSE;FaultReads=0;Active=P;
  Qmp[0x0c/4]=0xffff;
  CR_PIL_IO Bound=Io;Bound.Write32=PeerWrite;Bound.Read32=PeerRead;
  assert(CrPilCoreInitialize(P,&Target,&Bound)==CR_SUCCESS);
  assert(CrPilInspect(Header,sizeof(Header),&Target,I)==CR_SUCCESS);
  assert(CrPilBuildMetadata(Header,sizeof(Header),I,NULL,NULL,M,I->HeaderSize+I->HashSize)==CR_SUCCESS);
  assert(CrPilStartPhysical(P,Header,sizeof(Header),I,M,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
}
int main(int Argc,char **Argv) {
  CR_PIL_CONTEXT P;CR_PIL_IMAGE I;VOID *M=aligned_alloc(4096,4096);assert(M);
  Setup(&P,&I,M);
  CR_STATUS S=CrPilStop(&P);
  printf("stop=%llx pas=%u complete=%u qmp=%u/%u down=%u votes=%u crypto=%u\n",
         (unsigned long long)(UINT64)S,P.PasOwned,P.Shutdown.Complete,P.QmpOpen,P.QmpClosing,DownWrites,P.Votes,Crypto);
  if(Argc>1 && !strcmp(Argv[1],"before")) {
    assert(S==CR_SUCCESS && DownWrites==0);free(M);return 0;
  }
  assert(S==CR_SUCCESS && !P.PasOwned && P.Shutdown.Complete && !P.QmpOpen && !P.QmpClosing && !P.QmpChanged && !P.LoadState && !P.Votes && !Crypto);
  assert(DownWrites==0 && Qmp[0x24/4]==0xffff && Qmp[0x28/4]==0xffff && Qmp[0x2c/4]==0xffff && Qmp[0x30/4]==0xffff);
  UINT32 Before=Calls;assert(CrPilStop(&P)==CR_SUCCESS && Calls==Before && Stops==1);
  Setup(&P,&I,M);HoldOff=TRUE;
  assert(CrPilStop(&P)==CR_TIMEOUT && !P.PasOwned && P.LoadState && P.QmpOpen && P.Votes==7 && Crypto && Qmp[0x80/4]==64 && !DownWrites);
  HoldOff=FALSE;Qmp[0x80/4]=0;assert(CrPilStop(&P)==CR_SUCCESS && Stops==1 && !Crypto);
  /* A mismatched acknowledgement is not a retired QMP producer. */
  Setup(&P,&I,M);P.LoadState=FALSE;Qmp[0x28/4]=0xffff0000;
  assert(CrPilStop(&P)==CR_TIMEOUT && P.QmpOpen && Crypto && !DownWrites);
  Qmp[0x28/4]=0xffff;assert(CrPilStop(&P)==CR_SUCCESS && Stops==1 && !Crypto);
  /* Already borrowed transport remains established and is not withdrawn. */
  Fixture();CR_PIL_IO Bound=Io;Bound.Write32=PeerWrite;Bound.Read32=PeerRead;
  Qmp[0xc/4]=Qmp[0x10/4]=Qmp[0x14/4]=Qmp[0x18/4]=Qmp[0x24/4]=Qmp[0x28/4]=Qmp[0x2c/4]=Qmp[0x30/4]=0xffff;
  assert(CrPilCoreInitialize(&P,&Target,&Bound)==CR_SUCCESS);
  assert(CrPilStartPhysical(&P,Header,sizeof(Header),&I,M,0x12345000,Memory,NULL,NULL)==CR_SUCCESS && !P.QmpChanged);
  assert(CrPilStop(&P)==CR_SUCCESS && !Crypto);
  /* Every state/ACK, descriptor and pending field must qualify after off;
     a real peer can change independently of the caller's writes. */
  CONST UINT32 Offsets[]={0,4,0x34,0x38,0xc,0x10,0x14,0x18,0x24,0x28,0x2c,0x30,0x80};
  for(UINT32 N=0;N<sizeof(Offsets)/sizeof(Offsets[0]);++N) {
    Setup(&P,&I,M);FaultOffset=Offsets[N];FaultValue=FaultOffset==0x80 ? 64 : 0;Inject=TRUE;
    assert(CrPilStop(&P)==CR_TIMEOUT && P.Shutdown.Complete && !P.PasOwned && !P.LoadState && P.QmpOpen && Crypto && Stops==1 && !DownWrites);
    assert(FaultReads>=1000);Inject=FALSE;
    assert(CrPilStop(&P)==CR_SUCCESS && Stops==1 && !P.QmpOpen && !P.QmpEstablished && !Crypto);
  }
  Setup(&P,&I,M);FaultOffset=0x30;FaultValue=0xffff0000;Inject=SecondPass=TRUE;
  assert(CrPilStop(&P)==CR_TIMEOUT && Crypto && P.QmpOpen && FaultReads==2000);
  Inject=FALSE;assert(CrPilStop(&P)==CR_SUCCESS && Stops==1 && !Crypto);
  free(M);puts("PIL QMP consumed off/established transport retirement,13 field faults,second snapshot and retained uncertainty PASS");return 0;
}
