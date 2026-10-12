/* Actual portable retained-resource transaction adversarial cases. MIT */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#undef NULL
#include <Library/SerialResourceLib.h>
typedef struct {UINT32 Call,Fail,Reserved[3],Applied[3],Released[3],Begun,Ended;BOOLEAN Owned,Healthy;VOID *Owner;CR_SERIAL_RESOURCES *R;} F;
static CR_STATUS Step(F *F){return ++F->Call==F->Fail?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS V(VOID *C,UINT32 G,UINT32 I){F *F=C;assert(G<3&&I<(G==2?2U:3U));return Step(F);}
static CR_STATUS B(VOID *C,VOID *O,BOOLEAN *Owned){F *F=C;F->Owner=O;*Owned=TRUE;F->Owned=TRUE;++F->Begun;return Step(F);}
static CR_STATUS Q(VOID *C,VOID *O,UINT32 G,UINT32 I){F *F=C;assert(O==F->Owner&&F->R->Owner==O&&I<3);++F->Reserved[G];return Step(F);}
static CR_STATUS A(VOID *C,VOID *O,UINT32 G,UINT32 I){F *F=C;assert(O==F->Owner&&F->Owned&&I<3);assert(F->Reserved[0]==3&&F->Reserved[1]==3&&F->Reserved[2]==2);++F->Applied[G];return Step(F);}
static CR_STATUS R(VOID *C,VOID *O,UINT32 G){F *F=C;assert(O==F->Owner&&F->Owned);CR_STATUS S=Step(F);if(S==CR_SUCCESS)++F->Released[G];return S;}
static CR_STATUS E(VOID *C,VOID *O){F *F=C;assert(O==F->Owner&&F->Owned);CR_STATUS S=Step(F);if(S==CR_SUCCESS){F->Owned=FALSE;++F->Ended;}return S;}
static BOOLEAN C(VOID *P,VOID *O){F *F=P;return F->Healthy&&F->Owned&&O==F->Owner;}
int main(void){CONST UINT32 Counts[]={3,3,2};int Owner;
 for(UINT32 Fail=1;Fail<=25;++Fail){CR_SERIAL_RESOURCES S={0};F F={.Fail=Fail,.Healthy=TRUE,.R=&S};CR_SERIAL_RESOURCE_IO I={&F,V,Q,B,A,R,E,C};
   assert(CrSerialResourcesStart(&S,&I,&Owner,Counts)!=CR_SUCCESS);
   assert(!CrSerialResourcesReady(&S));
   if(Fail<=8){assert(!S.Owner&&!F.Begun);continue;}
   assert(S.Owner==&Owner&&S.Io.Context==&F&&F.Owned);
   assert(CrSerialResourcesStart(&S,&I,&Owner,Counts)==CR_BUSY);
   F.Fail=F.Call+1;CR_SERIAL_RESOURCES Old=S;assert(CrSerialResourcesRetire(&S)!=CR_SUCCESS);
   assert(S.Owner==Old.Owner&&S.Io.Context==Old.Io.Context&&F.Owned);
   F.Fail=0;assert(CrSerialResourcesRetire(&S)==CR_SUCCESS&&!S.Owner&&!F.Owned&&F.Ended==1);
   for(UINT32 G=0;G<3;++G)assert(F.Released[G]<=1);
 }
 CR_SERIAL_RESOURCES S={0};F F={.Healthy=TRUE,.R=&S};CR_SERIAL_RESOURCE_IO I={&F,V,Q,B,A,R,E,C};
 assert(CrSerialResourcesStart(&S,&I,&Owner,Counts)==CR_SUCCESS&&CrSerialResourcesReady(&S));
 F.Healthy=FALSE;assert(!CrSerialResourcesReady(&S));F.Healthy=TRUE;
 UINT32 Before=F.Call;CrSerialResourcesQuarantine(&S);assert(F.Call==Before&&!CrSerialResourcesReady(&S));
 assert(CrSerialResourcesRetire(&S)==CR_NOT_READY&&F.Call==Before&&S.Owner==&Owner&&F.Owned);
 puts("serial resources: every startup/cleanup fault, fixed reservations, exact owner retention, pure health/quarantine PASS");return 0;
}
