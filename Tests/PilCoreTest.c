/* Independent native/NT core IO and physical address/lifetime vectors. MIT */
#include <Library/PILCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef PIL_TEST_SMCCC
#include <Library/PilSmcccLib.h>
static CR_PIL_SMCCC Transport;
#ifdef _KERNEL_MODE
KIRQL KeGetCurrentIrql(VOID) {return PASSIVE_LEVEL;}
#endif
#endif
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) {return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V) {return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...) {(VOID)Level;(VOID)Format;}
#endif
static UINT32 In[85],Out[85],Qmp[256],Votes[3],Calls,Stops,FailStop,FailAuth,FailCallback;
static UINT64 MetaAddress,Handle;static BOOLEAN Crypto;static UINT8 Header[196],Memory[0x4000];
static UINT32 InterruptFunction,ResumeReplies,ResumeCalls;
static BOOLEAN FailResume,AlreadyStopped,RejectRepeatShutdown;
static UINT64 ResumeCookie,ResumeResult;
static INT32 ResumeTransport;
static UINT32 QmpFailDownAck, QmpCloses;
static BOOLEAN QmpHoldMessage;
static CR_PIL_SMC_ARGS ExpectedResume;
static CR_PIL_TARGET Target={1,0x85e00000,sizeof(Memory),2,443,429,0x0ed1800c,3,
                            0x0c300000,0x400,NULL,"test-pil",0x7000,0x7001,0};
static UINT16 ProxyMaxLevel;
static void Put32(UINT8 *P,UINT32 V) {for(UINT32 I=0;I<4;++I) P[I]=(UINT8)(V>>(8*I));}
static void Ready(UINT32 State) {
  In[0]=0x504d5324;In[1]=0x101;In[2]=2;In[3]=16|(1U<<16);
  memcpy((UINT8 *)In+20,"slave-kernel",12);In[9]=State;
}
static CR_STATUS SecureModel(VOID *C,CR_PIL_SMC_ARGS *A) {
  assert(C==&Calls);++Calls;
  if (FailCallback) {FailCallback=0;return CR_DEVICE_ERROR;}
  UINT32 Fid=(UINT32)A->X[0];
  if (Fid==1) {
    assert(InterruptFunction!=0 && A->X[6]==ResumeCookie);
    for(UINT32 I=1;I<8;++I) if(I!=6) assert(A->X[I]==ExpectedResume.X[I]);
    ++ResumeCalls;
    if(FailResume) return CR_DEVICE_ERROR;
    if(ResumeReplies!=0) {
      if(ResumeReplies!=CR_MAX_UINT32) --ResumeReplies;
      A->X[0]=1;A->X[1]=99;A->X[3]=0;A->X[6]=++ResumeCookie;return CR_SUCCESS;
    }
    if(InterruptFunction==0x206 && ResumeTransport==0 && ResumeResult==0) AlreadyStopped=TRUE;
    InterruptFunction=0;A->X[0]=(UINT64)(INT64)ResumeTransport;A->X[1]=ResumeResult;return CR_SUCCESS;
  }
  if ((Fid&0xffff)==InterruptFunction) {
    if(InterruptFunction==0x206) ++Stops;
    ExpectedResume=*A;ResumeCookie=0xfedcba9876543210ULL;
    A->X[0]=1;A->X[1]=99;A->X[3]=0;A->X[6]=ResumeCookie;return CR_SUCCESS;
  }
  if ((Fid&0xffff)==0x601 || (Fid&0xffff)==0x207) {A->X[0]=0;A->X[1]=1;return CR_SUCCESS;}
  assert(Fid==0x42000201 || Fid==0x42000202 || Fid==0x42000205 || Fid==0x42000206);
  assert(A->X[2]==1 && A->X[5]==0 && A->X[6]==0 && A->X[7]==0 && Crypto);
  if ((Fid&255)==1) {assert(A->X[1]==0x82);MetaAddress=A->X[3];}
  if ((Fid&255)==5 && !FailAuth) Ready(2);
  if ((Fid&255)==6) ++Stops;
  A->X[0]=0;A->X[1]=((Fid&255)==5 && FailAuth) ||
    ((Fid&255)==6 && (FailStop || (AlreadyStopped && RejectRepeatShutdown))) ? 22 : 0;
  if((Fid&255)==6 && A->X[1]==0) AlreadyStopped=TRUE;
  return CR_SUCCESS;
}
#ifdef PIL_TEST_SMCCC
VOID CrPilArm64Smccc(CR_PIL_SMC_ARGS *A) {assert(SecureModel(&Calls,A)==CR_SUCCESS);}
UINT32 CrPilArm64CurrentEl(VOID) {return 1;}
static CR_STATUS Secure(VOID *C,CR_PIL_SMC_ARGS *A) {
  assert(C==&Calls);
  if(FailCallback || (A->X[0]==1 && FailResume)) return SecureModel(C,A);
  return CrPilSmcccCall(&Transport,A);
}
#else
#define Secure SecureModel
#endif
static UINT32 Read32(VOID *C,UINT32 A) {
  assert(C==&Calls && A>=Target.QmpBase && A<Target.QmpBase+Target.QmpSize && (A&3)==0);
  return Qmp[(A-Target.QmpBase)/4];
}
static VOID Write32(VOID *C,UINT32 A,UINT32 V) {
  assert(C==&Calls);
  if (A==Target.IpccSend) {
    if (V==0) {
      if (Qmp[0x24/4]==0xffff0000U) ++QmpCloses;
      if (!(QmpFailDownAck&1)) Qmp[0x28/4]=Qmp[0x24/4];
      Qmp[0x14/4]=0xffff;
      if (!(QmpFailDownAck&2)) Qmp[0x30/4]=Qmp[0x2c/4];
      if (!QmpHoldMessage) Qmp[0x80/4]=0;
    } else {assert(V==(3U<<16|2));if (Out[9]&1) In[9]|=8;}
  } else {assert(A>=Target.QmpBase && A<Target.QmpBase+Target.QmpSize && (A&3)==0);Qmp[(A-Target.QmpBase)/4]=V;}
}
static VOID Fence(VOID *C) {assert(C==&Calls);}
static VOID Delay(VOID *C,UINT32 Us) {assert(C==&Calls && Us!=0);}
static CR_STATUS Get(VOID *C,UINT32 Host,UINT32 Item,UINT32 *Size,VOID **Address) {
  assert(C==&Calls && Host==2);*Size=sizeof(In);*Address=Item==443 ? In : Out;
  return Item==443 && In[0]==0 ? CR_NOT_FOUND : CR_SUCCESS;
}
static CR_STATUS Allocate(VOID *C,UINT32 Host,UINT32 Item,UINT32 Size,VOID **Address) {
  assert(C==&Calls && Host==2 && Item==429 && Size==340);*Address=Out;return CR_SUCCESS;
}
static UINT32 Index(CONST CHAR8 *Name) {
  if (!strcmp(Name,"lmx.lvl")) return 0;if (!strcmp(Name,"lcx.lvl")) return 1;
  assert(!strcmp(Name,"xo.lvl"));return 2;
}
static CR_STATUS Address(VOID *C,CONST CHAR8 *Name,UINT32 *V) {assert(C==&Calls);*V=0x1000+Index(Name)*0x100;return CR_SUCCESS;}
static CR_STATUS Aux(VOID *C,CONST CHAR8 *Name,VOID *Data,UINT32 *Size) {
  UINT8 Levels[]={0,0,0x40,0,0x80,0,(UINT8)ProxyMaxLevel,(UINT8)(ProxyMaxLevel>>8)};assert(C==&Calls && Index(Name)<2 && *Size>=8);
  memcpy(Data,Levels,8);*Size=8;return CR_SUCCESS;
}
static CR_STATUS Proxy(VOID *C,UINT32 Addr,UINT32 V) {assert(C==&Calls && Addr>=0x1000 && Addr<=0x1200);Votes[(Addr-0x1000)/0x100]=V;return CR_SUCCESS;}
static CR_STATUS Acquire(VOID *C,CONST CHAR8 *Name,UINT32 Source,UINT32 Destination,UINT64 *H) {
  assert(C==&Calls && !strcmp(Name,Target.InterconnectProvider) && Source==0x7000 && Destination==0x7001 && !Crypto);
  Crypto=TRUE;Handle=*H=0x1122334455667788ULL;return CR_SUCCESS;
}
static CR_STATUS Bandwidth(VOID *C,UINT64 H,UINT64 Average,UINT64 Peak) {assert(C==&Calls && H==Handle && Crypto && Average==0 && Peak==0xffffffff);return CR_SUCCESS;}
static CR_STATUS Release(VOID *C,UINT64 H) {assert(C==&Calls && H==Handle && Crypto);Crypto=FALSE;return CR_SUCCESS;}
static CR_PIL_IO Io={&Calls,Secure,Read32,Write32,Fence,Delay,Get,Allocate,Address,Aux,Proxy,Acquire,Bandwidth,Release};
static void Fixture(void) {
  ProxyMaxLevel=Target.ProxyMinimumLevel?384:192;
#ifdef PIL_TEST_SMCCC
  if(Transport.Initialized) assert(CrPilSmcccUninitialize(&Transport)==CR_SUCCESS);
  assert(CrPilSmcccInitialize(&Transport,&Target)==CR_SUCCESS);
#endif
  memset(In,0,sizeof(In));memset(Out,0,sizeof(Out));memset(Qmp,0,sizeof(Qmp));memset(Votes,0,sizeof(Votes));
  memset(Header,0,sizeof(Header));memset(Memory,0xaa,sizeof(Memory));Calls=Stops=FailStop=FailAuth=FailCallback=0;Crypto=FALSE;MetaAddress=0;
  InterruptFunction=ResumeReplies=ResumeCalls=0;FailResume=FALSE;ResumeCookie=0;
  AlreadyStopped=RejectRepeatShutdown=FALSE;ResumeResult=0;ResumeTransport=0;
  QmpFailDownAck=QmpCloses=0;QmpHoldMessage=FALSE;
  Qmp[0]=0x4d41494c;Qmp[1]=1;Qmp[0xc/4]=0xffff;Qmp[0x34/4]=0x100;Qmp[0x38/4]=0x80;
  Header[0]=0x7f;Header[1]='E';Header[2]='L';Header[3]='F';Header[4]=1;Header[5]=1;Header[6]=1;
  Put32(Header+20,1);Put32(Header+28,52);Header[40]=52;Header[42]=32;Header[44]=3;
  Put32(Header+52+16,148);
  Put32(Header+84,1);Put32(Header+84+4,180);Put32(Header+84+16,16);Put32(Header+84+24,2U<<24);
  Put32(Header+116,1);Put32(Header+116+4,148);Put32(Header+116+12,Target.MemoryBase);Put32(Header+116+16,4);Put32(Header+116+20,0x1000);
  memset(Header+148,0x5a,4);memset(Header+180,0x6b,16);
}
static void TestQmpClose(VOID *Metadata, CR_PIL_IMAGE *Image) {
  CR_PIL_CONTEXT Pil;
  for (UINT32 Fault=1;Fault<=3;++Fault) {
    Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
    assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
    QmpFailDownAck=Fault;
    if(Fault&1) Qmp[0x28/4]=0xffff0000U;
    if(Fault&2) Qmp[0x30/4]=0xffff0000U;
    assert(CrPilStop(&Pil)==CR_TIMEOUT && !Pil.PasOwned && Pil.QmpOpen && Pil.QmpChanged && Crypto);
#ifndef PIL_TEST_QMP_ONLY
    /* Secure shutdown proof does not imply QMP/resources are retired. */
    assert(Pil.Shutdown.Complete && Pil.Shutdown.X0==0 && Pil.Shutdown.X1==0);
#endif
    UINT32 Before=Calls;
    assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_ALREADY_STARTED);
    assert(Calls==Before && Stops==1);
    assert(CrPilStop(&Pil)==CR_TIMEOUT && Crypto && Stops==1);
    QmpFailDownAck=0;
    Qmp[0x28/4]=Qmp[0x30/4]=0xffff;
    assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto && !Pil.QmpOpen && !Pil.QmpChanged && Stops==1);
    assert(Qmp[0x28/4]==0xffffU && Qmp[0x30/4]==0xffffU && QmpCloses==0);
  }
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  /* A borrowed, already established QMP link must remain up after PIL. */
  Qmp[0x24/4]=Qmp[0x28/4]=Qmp[0x2c/4]=Qmp[0x30/4]=0xffff;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
  assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto && QmpCloses==0 && Qmp[0x24/4]==0xffff);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
  QmpHoldMessage=TRUE;
  assert(CrPilStop(&Pil)==CR_TIMEOUT && !Pil.PasOwned && Pil.LoadState && Pil.QmpOpen && Crypto && Stops==1);
  assert(Qmp[0x80/4]==64 && QmpCloses==0);
  QmpHoldMessage=FALSE;Qmp[0x80/4]=0;
  assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto && Stops==1);
}
int main(void) {
  CR_PIL_CONTEXT Pil;CR_PIL_IMAGE Image;VOID *Metadata=aligned_alloc(4096,4096);assert(Metadata!=NULL);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  CR_PIL_IO Bad=Io;Bad.Smc=NULL;assert(CrPilCoreInitialize(&Pil,&Target,&Bad)==CR_INVALID_PARAMETER && Calls==0);
  assert(CrPilInspect(Header,sizeof(Header),&Target,&Image)==CR_SUCCESS);
  assert(CrPilBuildMetadata(Header,sizeof(Header),&Image,NULL,NULL,Metadata,Image.HeaderSize+Image.HashSize)==CR_SUCCESS);
  TestQmpClose(Metadata,&Image);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
#ifdef PIL_TEST_QMP_ONLY
  free(Metadata);puts("PIL: coherent QMP producer retirement and message drain PASS");return 0;
#else
  CR_PIL_SHUTDOWN_EVIDENCE Evidence;
  assert(CrPilStop(&Pil)==CR_SUCCESS);
  assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && !Evidence.Complete && !Evidence.Attempted);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
  FailCallback=1;
  assert(CrPilStop(&Pil)==CR_DEVICE_ERROR && Pil.PasOwned && Crypto);
  assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && Evidence.Attempted && !Evidence.Returned &&
         !Evidence.Complete && Evidence.CallStatus==CR_DEVICE_ERROR);
  assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto && Pil.Shutdown.Complete);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
  assert(MetaAddress==0x12345000 && MetaAddress!=(UINT64)(UINTN)Metadata && Pil.Running && Pil.PasOwned);
  assert(Memory[0]==0x5a && Memory[4]==0 && CrPilCheckReady(&Pil)==CR_SUCCESS);
  FailStop=1;assert(CrPilStop(&Pil)==CR_DEVICE_ERROR && Pil.PasOwned && Crypto && Pil.Votes==7);
  assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && Evidence.Attempted && Evidence.Returned &&
         !Evidence.Complete && Evidence.X0==0 && Evidence.X1==22 && Evidence.FunctionId==0x42000206);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_ALREADY_STARTED);
  FailStop=0;assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto && !Pil.PasOwned && Stops==2);
  assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && Evidence.Complete && Evidence.Generation==1 &&
         Evidence.X0==0 && Evidence.X1==0 && Evidence.CallStatus==CR_SUCCESS);
  assert(CrPilStop(&Pil)==CR_SUCCESS && Stops==2);
  /* Another startup attempt observing somebody else's live ADSP invalidates
     prior owned shutdown evidence, without issuing PAS or taking ownership. */
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_ALREADY_STARTED);
  assert(!Pil.PasOwned && Pil.StartGeneration==2);
  assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && !Evidence.Complete && !Evidence.Attempted);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);FailAuth=1;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_PIL_SECURITY_VIOLATION && Pil.PasOwned && !Pil.Running);
  assert(CrPilStop(&Pil)==CR_SUCCESS && !Crypto);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0xfffff000,Memory,NULL,NULL)==CR_SUCCESS);
  assert(CrPilStop(&Pil)==CR_SUCCESS);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x100000000ULL,Memory,NULL,NULL)==CR_INVALID_PARAMETER);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345001,Memory,NULL,NULL)==CR_INVALID_PARAMETER);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);Ready(3);
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_DEVICE_ERROR && !Crypto && Calls==0);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  InterruptFunction=0x201;ResumeReplies=CR_MAX_UINT32;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_TIMEOUT);
  assert(Pil.SmcInterrupted && Pil.SmcCookie==ResumeCookie && ResumeCalls==255 && Pil.PasOwned && Crypto && Pil.Votes==7);
  assert(CrPilCheckReady(&Pil)==CR_NOT_READY);
  UINT32 BeforeCalls=Calls;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_ALREADY_STARTED && Calls==BeforeCalls);
  assert(CrPilStop(&Pil)==CR_TIMEOUT && ResumeCalls==511 && Stops==0 && Pil.SmcInterrupted && Pil.PasOwned && Crypto && Pil.Votes==7);
  FailResume=TRUE;UINT64 BeforeCookie=Pil.SmcCookie;
  assert(CrPilStop(&Pil)==CR_DEVICE_ERROR && Pil.SmcCookie==BeforeCookie && Pil.SmcInterrupted && Stops==0 && Crypto);
  FailResume=FALSE;ResumeReplies=0;
  assert(CrPilStop(&Pil)==CR_SUCCESS && Stops==1 && !Pil.SmcInterrupted && !Pil.PasOwned && !Crypto && Pil.Votes==0);
  /* Even a capability probe can be interrupted before PAS/resources exist.
     Teardown must retire it and restart must not erase its opaque cookie. */
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  InterruptFunction=0x601;ResumeReplies=CR_MAX_UINT32;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_UNSUPPORTED && Pil.SmcInterrupted && !Pil.PasOwned && !Crypto);
  assert(CrPilStop(&Pil)==CR_TIMEOUT && Pil.SmcInterrupted && Stops==0);
  ResumeReplies=0;assert(CrPilStop(&Pil)==CR_SUCCESS && !Pil.SmcInterrupted && Stops==0);
  Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
  InterruptFunction=0x207;ResumeReplies=CR_MAX_UINT32;
  assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_NOT_READY && Pil.SmcInterrupted && !Pil.PasOwned && !Crypto);
  ResumeReplies=0;assert(CrPilStop(&Pil)==CR_SUCCESS && !Pil.SmcInterrupted && Stops==0);
  /* A completed interrupted shutdown is already off. A monitor that rejects
     repeated shutdown must still allow prerequisite release; real failure
     replies must retain secure ownership and permit a separate retry. */
  for(UINT32 ReplyCase=0;ReplyCase<3;++ReplyCase) {
    Fixture();assert(CrPilCoreInitialize(&Pil,&Target,&Io)==CR_SUCCESS);
    assert(CrPilStartPhysical(&Pil,Header,sizeof(Header),&Image,Metadata,0x12345000,Memory,NULL,NULL)==CR_SUCCESS);
    InterruptFunction=0x206;ResumeReplies=CR_MAX_UINT32;RejectRepeatShutdown=TRUE;
    assert(CrPilStop(&Pil)==CR_TIMEOUT && Pil.SmcInterrupted && Pil.PasOwned && Stops==1 && Crypto && Pil.Votes==7);
    assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && !Evidence.Complete && Evidence.Returned && Evidence.X0==1);
    assert(CrPilStop(&Pil)==CR_TIMEOUT && Stops==1 && Pil.PasOwned && Crypto);
    ResumeReplies=0;
    if(ReplyCase==0) {
      assert(CrPilStop(&Pil)==CR_SUCCESS && AlreadyStopped && Stops==1 && !Pil.PasOwned && !Pil.Running &&
             !Pil.SmcInterrupted && !Crypto && Pil.Votes==0 && (Out[9]&1)==0);
      assert(CrPilGetShutdownEvidence(&Pil,&Evidence)==CR_SUCCESS && Evidence.Complete && Evidence.X0==0 && Evidence.X1==0);
    } else {
      ResumeResult=ReplyCase==1 ? 22 : 0;ResumeTransport=ReplyCase==2 ? -4 : 0;FailStop=1;
      assert(CrPilStop(&Pil)==CR_DEVICE_ERROR && !AlreadyStopped && Stops==2 && Pil.PasOwned &&
             !Pil.SmcInterrupted && Crypto && Pil.Votes==7);
      FailStop=0;assert(CrPilStop(&Pil)==CR_SUCCESS && Stops==3 && !Pil.PasOwned && !Crypto);
    }
  }
#ifdef PIL_TEST_SMCCC
  assert(CrPilSmcccUninitialize(&Transport)==CR_SUCCESS);
  puts("PIL actual core + qualified SMCCC helper continuation interoperability PASS");
#endif
  free(Metadata);puts("PIL portable: physical metadata, 64-bit path handles, auth refusal, retained interrupted continuation/shutdown and remote fatal vectors PASS");return 0;
#endif
}
