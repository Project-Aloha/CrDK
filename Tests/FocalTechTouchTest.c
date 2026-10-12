/* Actual report/resource core with read and partial-resource faults. MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/FocalTechTouchCrLib.h>
VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
static struct {UINT8 Report[33],IdHigh,IdLow;UINT32 Reads,Acquires,Releases,ResetCalls,Delays,FailAcquire,FailRelease,BadOwned;BOOLEAN FailRead,Pins[2],Rails[2],Idle,FailArm,FailEdge; } F;
static CR_STATUS Arm(VOID *C,VOID *O,UINT16 Pin){assert(C==&F&&O&&Pin==39&&F.Pins[1]);return F.FailArm?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS Edge(VOID *C,VOID *O,UINT16 Pin,BOOLEAN *Pending){assert(C==&F&&O&&Pin==39&&F.Pins[1]);*Pending=!F.Idle;return F.FailEdge?CR_DEVICE_ERROR:CR_SUCCESS;}
static UINT32 Pin(UINT16 N){assert(N==38||N==39);return N-38;}
static CR_STATUS AcquirePin(VOID *C,VOID *O,CONST GpioConfigParams *D,BOOLEAN *Owned){assert(C==&F&&O);F.Pins[Pin(D->PinNumber)]=TRUE;++F.Acquires;*Owned=F.Acquires!=F.BadOwned;return F.Acquires==F.FailAcquire?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS Reset(VOID *C,VOID *O,UINT16 N,UINT8 V){assert(C==&F&&O&&N==38&&F.Pins[0]&&V<2);++F.ResetCalls;return CR_SUCCESS;}
static CR_STATUS ReleasePin(VOID *C,VOID *O,UINT16 N){assert(C==&F&&O&&F.Pins[Pin(N)]);if(++F.Releases==F.FailRelease)return CR_DEVICE_ERROR;F.Pins[Pin(N)]=FALSE;return CR_SUCCESS;}
static UINT32 Rail(CONST CR_TOUCH_RAIL *R){return R->Millivolts==3000?0:1;}
static CR_STATUS AcquireRail(VOID *C,CONST CR_TOUCH_RAIL *R,BOOLEAN *Owned){assert(C==&F&&F.Pins[0]);F.Rails[Rail(R)]=TRUE;++F.Acquires;*Owned=F.Acquires!=F.BadOwned;return F.Acquires==F.FailAcquire?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS ReleaseRail(VOID *C,CONST CR_TOUCH_RAIL *R){assert(C==&F&&F.Rails[Rail(R)]);if(++F.Releases==F.FailRelease)return CR_DEVICE_ERROR;F.Rails[Rail(R)]=FALSE;return CR_SUCCESS;}
static CR_STATUS Read(VOID *C,UINT8 R,UINT8 *D,UINT32 N){assert(C==&F&&F.Pins[0]&&F.Pins[1]&&F.Rails[0]&&F.Rails[1]);++F.Reads;if(F.FailRead)return CR_DEVICE_ERROR;
 if(R==0xA3){assert(N==1);*D=F.IdHigh;}else if(R==0x9F){assert(N==1);*D=F.IdLow;}else{assert(R==1&&N==32);memcpy(D,F.Report+1,N);}return CR_SUCCESS;}
static VOID Delay(VOID *C,UINT32 N){assert(C==&F&&(N==1000||N==200000||N==100000));++F.Delays;}
static VOID Report(UINT8 Count){memset(F.Report,0xff,33);F.Report[0]=F.Report[1]=0;F.Report[2]=Count;}
static VOID Contact(UINT8 Slot,UINT8 Id,UINT8 Event,UINT16 X,UINT16 Y){UINT32 B=3+6*Slot;F.Report[B]=(UINT8)((Event<<6)|(X>>8));F.Report[B+1]=(UINT8)X;F.Report[B+2]=(UINT8)((Id<<4)|(Y>>8));F.Report[B+3]=(UINT8)Y;F.Report[B+4]=1;F.Report[B+5]=0x10;}
static VOID Setup(CR_FTS_TOUCH *T){memset(T,0,sizeof(*T));memset(&F,0,sizeof(F));F.IdHigh=0x54;F.IdLow=0x52;Report(0);
 CR_FTS_IO Io={&F,AcquirePin,Reset,ReleasePin,AcquireRail,ReleaseRail,Read,Delay,Arm,Edge};assert(CrFtsInit(T,CrTargetGetTouchContext(),&Io)==CR_SUCCESS&&!F.Reads&&!F.Acquires);}
static VOID Stop(CR_FTS_TOUCH *T){F.FailRelease=0;assert(CrFtsStop(T)==CR_SUCCESS&&!T->Active);for(UINT32 I=0;I<2;++I)assert(!F.Pins[I]&&!F.Rails[I]);}
int main(void){CR_FTS_TOUCH T;CR_FTS_STATE S;
 Setup(&T);F.FailArm=TRUE;assert(CrFtsStart(&T)==CR_DEVICE_ERROR&&!T.Active);Stop(&T);
 Setup(&T);F.Idle=TRUE;memset(F.Report,0xff,sizeof(F.Report));
 assert(CrFtsStart(&T)==CR_SUCCESS&&CrFtsService(&T)==CR_SUCCESS&&F.Reads==2);
 F.Idle=FALSE;assert(CrFtsService(&T)==CR_COMPROMISED_DATA&&F.Reads==3);
 F.FailEdge=TRUE;assert(CrFtsService(&T)==CR_DEVICE_ERROR&&F.Reads==3);Stop(&T);
 Setup(&T);F.IdHigh=0x12;assert(CrFtsStart(&T)==CR_UNSUPPORTED&&!T.Active&&F.Reads==20);Stop(&T);
 for(UINT32 I=1;I<=4;++I){Setup(&T);F.BadOwned=I;assert(CrFtsStart(&T)==CR_NOT_READY&&!T.Active);assert(CrFtsStop(&T)==CR_NOT_READY&&!F.Releases);}
 for(UINT32 I=1;I<=4;++I){Setup(&T);F.FailAcquire=I;assert(CrFtsStart(&T)==CR_DEVICE_ERROR&&!T.Active);F.FailRelease=1;assert(CrFtsStop(&T)==CR_DEVICE_ERROR);Stop(&T);}
 Setup(&T);F.Report[0]=0x40;assert(CrFtsStart(&T)==CR_SUCCESS&&T.Active&&CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_NOT_READY);
 Report(1);Contact(0,2,0,100,200);assert(CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_SUCCESS&&S.Pressed&&S.X==100&&S.Y==200);
 assert(CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_NOT_READY);Contact(0,2,2,150,210);assert(CrFtsService(&T)==CR_SUCCESS);
 Report(0);Contact(0,2,1,150,210);assert(CrFtsService(&T)==CR_SUCCESS);assert(CrFtsGetState(&T,&S)==CR_SUCCESS&&S.Pressed&&S.X==150);assert(CrFtsGetState(&T,&S)==CR_SUCCESS&&!S.Pressed&&S.X==150);
 Report(2);Contact(0,3,0,300,500);Contact(1,1,0,200,400);assert(CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_SUCCESS&&S.Pressed&&S.X==200);
 Report(1);Contact(0,3,2,301,501);assert(CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_SUCCESS&&!S.Pressed&&CrFtsGetState(&T,&S)==CR_SUCCESS&&S.Pressed&&S.X==301);
 Contact(0,3,3,301,501);assert(CrFtsService(&T)==CR_COMPROMISED_DATA&&CrFtsGetState(&T,&S)==CR_SUCCESS&&!S.Pressed&&CrFtsGetState(&T,&S)==CR_COMPROMISED_DATA);
 Report(1);Contact(0,0,0,1081,10);assert(CrFtsService(&T)==CR_COMPROMISED_DATA);Contact(0,0,0,1080,2340);assert(CrFtsService(&T)==CR_SUCCESS&&CrFtsGetState(&T,&S)==CR_SUCCESS&&S.X==1080&&S.Y==2340);
 F.FailRead=TRUE;assert(CrFtsService(&T)==CR_DEVICE_ERROR&&CrFtsGetState(&T,&S)==CR_SUCCESS&&!S.Pressed);F.FailRead=FALSE;
 Report(2);Contact(0,0,0,1,2);Contact(1,0,0,3,4);assert(CrFtsService(&T)==CR_COMPROMISED_DATA);Report(6);assert(CrFtsService(&T)==CR_COMPROMISED_DATA);
 memset(F.Report,0,sizeof(F.Report));assert(CrFtsService(&T)==CR_COMPROMISED_DATA);
 Stop(&T);UINT32 Reads=F.Reads;assert(CrFtsService(&T)==CR_NOT_READY&&F.Reads==Reads);
 Setup(&T);assert(CrFtsStart(&T)==CR_SUCCESS);for(UINT32 I=0;I<9;++I){Report(1);Contact(0,0,0,1,2);CR_STATUS A=CrFtsService(&T);Report(0);CR_STATUS B=CrFtsService(&T);if(I<8)assert(A==CR_SUCCESS&&B==CR_SUCCESS);else assert(A==CR_OUT_OF_RESOURCES&&B==CR_SUCCESS);}
 assert(T.Count<=CR_FTS_QUEUE_CAPACITY&&CrFtsGetState(&T,&S)==CR_SUCCESS&&!S.Pressed);Stop(&T);
 puts("FTS actual core: firmware-ID gate, 5-contact bounds, stable pointer/press-release FIFO, failures, overflow cancel and retained cleanup PASS");return 0;
}
