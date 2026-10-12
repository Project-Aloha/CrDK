/* Real shared native events + real OSKAL; no timer substitutes for frame IRQ. */
#include "MdpNativeEventsFixture.h"
static MDP_NATIVE_EVENTS E;
static UINT32 DuringService;
static EFI_STATUS TestService(VOID *C,UINT64 Generation){
  assert(C==&E&&Generation==91&&EventEois==1&&EventTpl==TPL_CALLBACK);++EventServices;
  if(DuringService==1)assert(MdpNativeEventsSeal(&E)==EFI_NOT_READY&&E.Running&&!E.Registered);
  if(DuringService==2)++E.Incarnation;
  if(DuringService==4)assert(MdpNativeEventsFreeze(&E)==EFI_NOT_READY&&E.Running&&E.Registered);
  return DuringService==3?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static void Setup(UINT32 Irq){EventSetup(Irq);memset(&E,0,sizeof(E));DuringService=0;}
static void Open(void){assert(MdpNativeEventsOpen(&E,&E,91,EventIrq,TestService)==EFI_SUCCESS&&MdpNativeEventsOwned(&E)&&E.Registered&&E.Admission);}
int main(void){
  for(UINT32 Irq=115;Irq<=1019;Irq+=904){
    Setup(Irq);EventTpl=TPL_HIGH_LEVEL;assert(MdpNativeEventsOpen(&E,&E,91,Irq,TestService)==EFI_INVALID_PARAMETER&&!E.Initialized);
    EventTpl=TPL_APPLICATION;Open();EventFire();assert(EventEois==1&&!EventServices);EventPump();assert(EventServices==1&&EventEnabled);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS&&EventClosed&&!EventEnabled);
  }
  Setup(115);assert(MdpNativeEventsOpen(&E,&E,91,31,TestService)==EFI_INVALID_PARAMETER);
  assert(MdpNativeEventsOpen(&E,&E,91,1020,TestService)==EFI_INVALID_PARAMETER&&!E.Initialized);
  Setup(115);Open();EventFire();assert(MdpNativeEventsSeal(&E)==EFI_NOT_READY&&!E.Registered&&!EventClosed);
  UINT32 Enables=EventEnables;EventPump();assert(!EventServices&&EventEnables==Enables);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();DuringService=1;EventFire();Enables=EventEnables;EventPump();assert(EventServices==1&&!EventEnabled&&EventEnables==Enables);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();DuringService=2;EventFire();Enables=EventEnables;EventPump();assert(E.Faulted&&EventEnables==Enables);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();DuringService=3;EventFire();EventPump();assert(E.Faulted&&E.LastStatus==EFI_DEVICE_ERROR);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();EventDropEnable=TRUE;EventFire();EventPump();assert(E.Faulted&&!EventEnabled);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();EventFailDisable=EFI_WARN_UNKNOWN_GLYPH;assert(MdpNativeEventsSeal(&E)==EFI_NOT_READY&&E.Registered&&!EventClosed);
  EventFailDisable=0;EventFailUnregister=EFI_WARN_UNKNOWN_GLYPH;assert(MdpNativeEventsSeal(&E)==EFI_NOT_READY&&E.Registered);
  EventFailUnregister=0;EventFailClose=EFI_DEVICE_ERROR;assert(MdpNativeEventsSeal(&E)==EFI_DEVICE_ERROR&&!E.Registered);EventFailClose=0;assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);EventGic.GetInterruptSourceState=NULL;assert(MdpNativeEventsOpen(&E,&E,91,115,TestService)==EFI_UNSUPPORTED&&!E.Registered);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);EventFailCreate=EFI_DEVICE_ERROR;assert(MdpNativeEventsOpen(&E,&E,91,115,TestService)==EFI_DEVICE_ERROR&&E.Initialized&&!E.Work);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  Setup(115);Open();EventFailSignal=EFI_DEVICE_ERROR;EventFire();assert(E.Queued&&E.Faulted);assert(MdpNativeEventsSeal(&E)==EFI_NOT_READY);EventPump();assert(!EventServices);assert(MdpNativeEventsSeal(&E)==EFI_SUCCESS);
  /* Freeze never unregisters, closes or fabricates IRQ lease release. */
  Setup(115);Open();EventTpl=TPL_NOTIFY;assert(MdpNativeEventsFreeze(&E)==EFI_SUCCESS&&E.Frozen&&E.Registered&&!EventEnabled&&MdpNativeEventsOwned(&E));
  assert(!EventUnregisters&&!EventCloses);assert(MdpNativeEventsFreeze(&E)==EFI_SUCCESS);EventTpl=TPL_APPLICATION;assert(MdpNativeEventsSeal(&E)==EFI_ACCESS_DENIED);
  Setup(115);Open();EventFire();Enables=EventEnables;EventTpl=TPL_NOTIFY;
  assert(MdpNativeEventsFreeze(&E)==EFI_NOT_READY&&E.Registered&&!E.Frozen&&!EventClosed);EventTpl=TPL_APPLICATION;EventPump();
  assert(!EventServices&&EventEnables==Enables);assert(MdpNativeEventsFreeze(&E)==EFI_SUCCESS&&E.Frozen&&E.Registered&&!EventUnregisters&&!EventCloses);
  Setup(115);Open();DuringService=4;EventFire();Enables=EventEnables;EventPump();assert(E.Sealed&&!E.Frozen&&E.Registered&&EventEnables==Enables);
  assert(MdpNativeEventsFreeze(&E)==EFI_SUCCESS&&E.Frozen&&!EventUnregisters&&!EventCloses);
  Setup(115);Open();EventFailDisable=EFI_WARN_UNKNOWN_GLYPH;assert(MdpNativeEventsFreeze(&E)==EFI_NOT_READY&&E.Registered&&!E.Frozen);
  EventFailDisable=0;assert(MdpNativeEventsFreeze(&E)==EFI_SUCCESS&&E.Frozen);
  Setup(115);Open();EventTpl=TPL_HIGH_LEVEL;MDP_NATIVE_EVENTS Before=E;assert(MdpNativeEventsFreeze(&E)==EFI_ACCESS_DENIED&&!memcmp(&Before,&E,sizeof(E)));
  puts("PASS: actual shared event/OSKAL IRQ descriptor, EOI/deferred ordering, seal/rearm races, queued/running freeze and retained registration");return 0;
}
