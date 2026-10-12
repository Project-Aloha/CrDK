/* Real shared ClockLib retirement; hardware MMIO acknowledgements are modeled. MIT */
#define CR_CLOCK_EXTERNAL_SOC_MAIN ClockSocPriorRegression
#include "ClockExternalSocTest.c"
static UINTN RetireAddress(CR_CLOCK_EXTERNAL *E,UINT32 Index) {
  UINT32 Group=Index<6?Index/3:(Index-6)/2;
  ClockNode *N=Index<6?(Group?E->SecondLeaf[Index%3]:E->Leaf[Index%3]):
    (Group?E->SecondSource[(Index-6)%2]:E->Source[(Index-6)%2]);
  return N->ParentController->Address+(Index<6?N->EnableRegister:N->CmdRegister);
}
static VOID InheritedOn(CR_CLOCK_EXTERNAL *E) {
  for(UINT32 I=0;I<10;++I)*Reg(RetireAddress(E,I))=BIT(22)|(I<6?BIT(0):BIT(1));
}
static VOID Retirement(UINT32 Soc,UINT32 Fault) {
  CR_CLOCK_EXTERNAL E;VOID *Storage=Fixture(&E,Soc,TRUE);ClockDriverContext *C=E.Core;
  memset(&E,0,sizeof(E));assert(CrClockExternalInitForSoc(&E,C,Soc)==CR_SUCCESS);
  int Owner,Foreign;assert(CrClockExternalReserve(&E,&Owner)==CR_SUCCESS);
  UINTN R=Reads,W=Writes;
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,0)==CR_INVALID_PARAMETER&&Reads==R&&Writes==W);
  assert(CrClockExternalRetireInheritedDsi(&E,&Foreign,17)==CR_BUSY&&Reads==R&&Writes==W);
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,17)==CR_NOT_READY&&Reads==R&&Writes==W);
  Access(&E,TRUE);InheritedOn(&E);
  E.SecondSource[1]->ReferenceCount=1;
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,17)==CR_BUSY&&Reads==R&&Writes==W);
  E.SecondSource[1]->ReferenceCount=0;
  BlockDisable=RetireAddress(&E,Fault);
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,17)==CR_TIMEOUT);
  assert(E.InheritedDsiOwner==&Owner&&E.InheritedDsiGeneration==17&&CrClockExternalNeedsAccess(&E));
  assert(E.InheritedDsiTouched&BIT(Fault));
  W=Writes;R=Reads;
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,18)==CR_INVALID_PARAMETER&&Reads==R&&Writes==W);
  assert(CrClockExternalReleaseOwner(&E,&Owner)==CR_NOT_READY&&E.Owner==&Owner&&Writes==W);
  assert(CrClockExternalLegacyGuard(&E,E.Access[0])==CR_BUSY);
  assert(CrClockExternalCheckInheritedDsiOff(&E,&Owner,17)==CR_NOT_READY&&Writes==W);
  BlockDisable=0;
  /* Retry must revalidate earlier acknowledgements, including a foreign gate drift. */
  if(Fault)*Reg(RetireAddress(&E,0))=BIT(0)|BIT(22);
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,17)==CR_SUCCESS);
  assert(E.InheritedDsiComplete&&E.InheritedDsiOff==0x3ff&& !CrClockExternalNeedsAccess(&E));
  W=Writes;assert(CrClockExternalCheckInheritedDsiOff(&E,&Owner,17)==CR_SUCCESS&&Writes==W);
  for(UINT32 I=0;I<10;++I)assert((*Reg(RetireAddress(&E,I))&(BIT(22)|BIT(31)))==(BIT(22)|BIT(31)));
  *Reg(RetireAddress(&E,0))|=BIT(0);
  assert(CrClockExternalCheckInheritedDsiOff(&E,&Owner,17)==CR_NOT_READY&&Writes==W);
  /* Completed operation is one-shot and cannot stop a new cold-owned epoch. */
  assert(CrClockExternalRetireInheritedDsi(&E,&Owner,17)==CR_SUCCESS&&Writes==W);
  *Reg(RetireAddress(&E,0))&=~BIT(0);
  Dispose(&E,Storage,&Owner);
}
int main(void) {
  assert(ClockSocPriorRegression()==0);
  for(UINT32 I=0;I<10;++I){Retirement(0x8250,I);Retirement(0x8350,I);}
  puts("Clock inherited DSI retirement: 20 gate/RCG faults, exact epoch, retained access, drift retry, readonly off proof and one-shot completion PASS");
  return 0;
}
