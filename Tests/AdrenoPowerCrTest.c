/* Actual A730 ARC/HFI core and source-generated profile facts. GPL-2.0-only */
#include <Library/AdrenoPowerCrLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "AdrenoPowerReference.h"
static unsigned Checks;
#define CHECK(X) do { ++Checks; if(!(X)) {fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#X);abort();} } while(0)
#ifdef _KERNEL_MODE
KIRQL KeGetCurrentIrql(VOID) { return PASSIVE_LEVEL; }
#else
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
#endif
static CR_RPMH_ARC_RESOURCE Gfx,Cx,Mx,Gmxc;
static CR_ADRENO_POWER_PROFILE Profile;
static CR_ADRENO_POWER_PLAN Plan;
static void Make(CR_RPMH_ARC_RESOURCE *R,const char *Name,UINT32 Address,
                 const UINT16 *Levels,UINT32 Count) {
  UINT8 Aux[64]={0}; CHECK(Count<=32);
  for(UINT32 I=0;I<Count;++I) {Aux[2*I]=(UINT8)Levels[I];Aux[2*I+1]=(UINT8)(Levels[I]>>8);}
  CHECK(CrRpmhArcQualify(R,Name,Address,CrRpmhArcPower,Aux,sizeof(Aux))==CR_SUCCESS);
}
static void Nominal(void) {
  static const UINT16 G[]={0,56,64,80,128,144,192,224,256,320,384};
  static const UINT16 C[]={0,64,128};
  static const UINT16 M[]={0,64,128,256,384};
  static const UINT16 X[]={0,56,64,80,128,144,192,224,256,320,384};
  Profile=*CrAdrenoSm8450PowerProfile();
  Make(&Gfx,"gfx.lvl",0x30000,G,11); Make(&Cx,"cx.lvl",0x30004,C,3);
  Make(&Mx,"mx.lvl",0x30008,M,5); Make(&Gmxc,"gmxc.lvl",0x3000c,X,11);
}
static CR_STATUS Build(const CR_RPMH_ARC_RESOURCE *X) {
  return CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,X,&Plan);
}
static void Fails(CR_STATUS Expected,const CR_RPMH_ARC_RESOURCE *X) {
  memset(&Plan,0xa7,sizeof(Plan));CR_ADRENO_POWER_PLAN Saved=Plan;
  CHECK(Build(X)==Expected);CHECK(memcmp(&Plan,&Saved,sizeof(Plan))==0);
}
static void ProfileFacts(void) {
  Nominal();CHECK(Profile.GpuCount==sizeof(ReferenceGpu)/sizeof(ReferenceGpu[0]));
  CHECK(Profile.GmuCount==sizeof(ReferenceGmu)/sizeof(ReferenceGmu[0]));
  CHECK(memcmp(Profile.Gpu,ReferenceGpu,sizeof(ReferenceGpu))==0);
  CHECK(memcmp(Profile.Gmu,ReferenceGmu,sizeof(ReferenceGmu))==0);
  CHECK(Profile.InitialGpuIndex==12 && Profile.Gpu[12].FrequencyHz==818000000);
  CHECK(Profile.GmuBootIndex==1 && Profile.GmuBootHz==200000000 && Profile.HubBootHz==150000000);
  CHECK(offsetof(CR_ADRENO_POWER_GX,Vote)==0 && offsetof(CR_ADRENO_POWER_GX,Acd)==4 &&
        offsetof(CR_ADRENO_POWER_GX,FrequencyKHz)==8);
  CHECK(Build(NULL)==CR_SUCCESS);CHECK(Plan.GxCount==13 && Plan.CxCount==3);
  CHECK(Plan.Version==1 && Plan.Size==256 && Plan.ChipId==0x07030001);
  CHECK(Plan.InitialGpuIndex==12 && Plan.GmuBootIndex==1 && Plan.GxSecondary==1);
  /* Literal independent expected HFI DWORD values for the sparse MX table. */
  static const UINT32 Expected[]={0,0x00380101,0x00380101,0x00400102,0x00400102,
    0x00500203,0x00800204,0x00900305,0x00c00306,0x00e00307,0x01000308,0x01400409,0x0180040a};
  for(UINT32 I=0;I<13;++I) {CHECK(Plan.Gx[I].Vote==Expected[I]);CHECK(Plan.Gx[I].Acd==0xffffffff);
    CHECK(Plan.Gx[I].FrequencyKHz==ReferenceGpu[I].FrequencyHz/1000);}
  CHECK(Plan.Cx[0].Vote==0 && Plan.Cx[1].Vote==0x00400101 && Plan.Cx[2].Vote==0x00800202);
  for(UINT32 I=13;I<16;++I) CHECK(memcmp(&Plan.Gx[I],&(CR_ADRENO_POWER_GX){0},12)==0);
  CHECK(memcmp(&Plan.Cx[3],&(CR_ADRENO_POWER_CX){0},8)==0);
  const UINT8 *Wire=(const UINT8 *)&Plan.Gx[4];
  static const UINT8 Hfi[]={2,1,0x40,0,255,255,255,255,0x30,0x57,5,0};
  CHECK(memcmp(Wire,Hfi,sizeof(Hfi))==0);
  CHECK(Build(&Gmxc)==CR_SUCCESS && Plan.GxSecondary==2);
  CHECK(Plan.Gx[12].Vote==0x01800a0a && Plan.Cx[2].Vote==0x00800202);
}
static void SourceDifferences(void) {
  Nominal();memset(Profile.Gpu,0,sizeof(Profile.Gpu));Profile.GpuCount=2;Profile.InitialGpuIndex=1;
  Profile.Gpu[1]=(CR_ADRENO_POWER_OPP){350000000,72,0};
  static const UINT16 P[]={0,96,96,384},S[]={0,64,80,96};
  Make(&Gfx,"gfx.lvl",0x30000,P,4);Make(&Gmxc,"gmxc.lvl",0x3000c,S,4);
  CHECK(Build(&Gmxc)==CR_SUCCESS);CHECK(Plan.Gx[1].Vote==0x00600201);
  /* Linux compares secondary to requested72, not rounded primary96. */
  CHECK(Plan.Gx[1].Vote!=0x00600301);
  Profile.Gpu[1].Level=384;CHECK(Build(&Gmxc)==CR_SUCCESS);
  CHECK(Plan.Gx[1].Vote==0x01800303); /* secondary saturates at last nonzero */
  Profile.Gpu[1].Level=385;Fails(CR_UNSUPPORTED,&Gmxc);
  Profile.Gpu[1].Level=96;CHECK(Build(&Gmxc)==CR_SUCCESS);
  CHECK((Plan.Gx[1].Vote&255)==1); /* duplicate primary keeps first index */
  /* Full corner namespace and full16/4 HFI capacities remain bounded. */
  Nominal();UINT16 Levels[32];for(UINT32 I=0;I<32;++I)Levels[I]=(UINT16)(I*100);
  Make(&Gfx,"gfx.lvl",0x30000,Levels,32);Make(&Cx,"cx.lvl",0x30004,Levels,32);
  Make(&Mx,"mx.lvl",0x30008,Levels,32);
  memset(Profile.Gpu,0,sizeof(Profile.Gpu));memset(Profile.Gmu,0,sizeof(Profile.Gmu));
  Profile.GpuCount=16;Profile.GmuCount=4;Profile.InitialGpuIndex=15;Profile.GmuBootIndex=1;
  Profile.GmuBootHz=1000;
  for(UINT32 I=1;I<16;++I)Profile.Gpu[I]=(CR_ADRENO_POWER_OPP){I*1000,3100,0};
  for(UINT32 I=1;I<4;++I)Profile.Gmu[I]=(CR_ADRENO_POWER_OPP){I*1000,3100,0};
  CHECK(Build(NULL)==CR_SUCCESS && Plan.Gx[15].Vote==0x0c1c1f1f && Plan.Cx[3].Vote==0x0c1c1f1f);
}
static void Malformed(void) {
#define BADP(Change,Status) do {Nominal();Change;Fails(Status,NULL);}while(0)
  BADP(Profile.Version=2,CR_UNSUPPORTED);BADP(Profile.ChipId=0x07040000,CR_UNSUPPORTED);
  BADP(Profile.GpuCount=0,CR_INVALID_PARAMETER);BADP(Profile.GpuCount=17,CR_INVALID_PARAMETER);
  BADP(Profile.GmuCount=5,CR_INVALID_PARAMETER);BADP(Profile.GmuCount=1,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[0].Level=1,CR_INVALID_PARAMETER);BADP(Profile.Gmu[0].FrequencyHz=1000,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[1].FrequencyHz=220000001,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[12].FrequencyHz=((UINT64)0x100000000)*1000,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[12].FrequencyHz=~(UINT64)0,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[1].Level=0,CR_INVALID_PARAMETER);BADP(Profile.Gpu[12].Level=65536,CR_INVALID_PARAMETER);
  BADP(Profile.Gpu[2].Level=1,CR_INVALID_PARAMETER);BADP(Profile.Gpu[2].FrequencyHz=220000000,CR_INVALID_PARAMETER);
  BADP(Profile.Gmu[1].Reserved=1,CR_INVALID_PARAMETER);BADP(Profile.Gpu[15].FrequencyHz=1,CR_INVALID_PARAMETER);
  BADP(Profile.Gmu[3].Level=1,CR_INVALID_PARAMETER);BADP(Profile.InitialGpuIndex=13,CR_INVALID_PARAMETER);
  BADP(Profile.InitialGpuIndex=0,CR_INVALID_PARAMETER);BADP(Profile.GmuBootIndex=3,CR_INVALID_PARAMETER);
  BADP(Profile.GmuBootHz=199999000,CR_INVALID_PARAMETER);BADP(Profile.HubBootHz=150000001,CR_INVALID_PARAMETER);
#define BADR(Change) do {Nominal();Change;Fails(CR_COMPROMISED_DATA,&Gmxc);}while(0)
  BADR(Gfx.Kind=CrRpmhArcXo);BADR(Gfx.Address=0x40000);BADR(Gfx.Address|=1);
  BADR(Cx.Address=Mx.Address);BADR(Gmxc.Address=Gfx.Address);BADR(Gfx.LevelCount=33);
  BADR(Gfx.LevelCount=1);BADR(Gfx.Levels[0]=1);BADR(Gfx.Levels[2]=0);
  BADR(Cx.Levels[2]=1);BADR(Mx.Levels[31]=1);BADR(Gmxc.LevelCount=0);
  BADR(Gfx.Name[8]='x');BADR(Cx.Name[0]='m');BADR(Gmxc.Name[8]='x');
  Nominal();Cx.Levels[2]=64;Fails(CR_UNSUPPORTED,NULL); /* CX primary must cover500MHz128 */
#undef BADR
#undef BADP
}
static void Aliases(void) {
  Nominal();memset(&Plan,0x71,sizeof(Plan));CR_ADRENO_POWER_PLAN Saved=Plan;
  CHECK(CrAdrenoPowerBuild(NULL,&Gfx,&Cx,&Mx,NULL,&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,NULL,&Cx,&Mx,NULL,&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,NULL,NULL)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild((void *)(uintptr_t)(UINTPTR_MAX-7),&Gfx,&Cx,&Mx,NULL,&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,(void *)(uintptr_t)(UINTPTR_MAX-3),&Cx,&Mx,NULL,&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,NULL,(void *)(uintptr_t)(UINTPTR_MAX-3))==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild((void *)((UINT8 *)&Profile+1),&Gfx,&Cx,&Mx,NULL,&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,(void *)((UINT8 *)&Gmxc+1),&Plan)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,NULL,(void *)((UINT8 *)&Plan+1))==CR_INVALID_PARAMETER);
  CHECK(memcmp(&Plan,&Saved,sizeof(Plan))==0);
  _Alignas(8) UINT8 Arena[1024];memset(Arena,0x4b,sizeof(Arena));
  memcpy(Arena,&Profile,sizeof(Profile));UINT8 Backup[1024];memcpy(Backup,Arena,sizeof(Arena));
  CHECK(CrAdrenoPowerBuild((void *)Arena,&Gfx,&Cx,&Mx,NULL,(void *)(Arena+96))==CR_INVALID_PARAMETER);
  CHECK(memcmp(Arena,Backup,sizeof(Arena))==0);
  memcpy(Arena,&Gmxc,sizeof(Gmxc));memcpy(Backup,Arena,sizeof(Arena));
  CHECK(CrAdrenoPowerBuild(&Profile,&Gfx,&Cx,&Mx,(void *)Arena,(void *)(Arena+4))==CR_INVALID_PARAMETER);
  CHECK(memcmp(Arena,Backup,sizeof(Arena))==0);
  /* Adjacent output is legal and source is immutable across success. */
  memcpy(Arena,&Profile,sizeof(Profile));memcpy(Backup,Arena,sizeof(Arena));
  CHECK(CrAdrenoPowerBuild((void *)Arena,&Gfx,&Cx,&Mx,NULL,(void *)(Arena+sizeof(Profile)))==CR_SUCCESS);
  CHECK(memcmp(Arena,Backup,sizeof(Profile))==0);
}
int main(void) {ProfileFacts();SourceDifferences();Malformed();Aliases();
  printf("A730 actual ARC/HFI power core: %u checks PASS\n",Checks);return 0;}
