/* Actual portable press/release core with hostile lower lifetimes. MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Old,UINT32 V){__atomic_compare_exchange_n(P,&Old,V,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Old;}
#endif
#include <Library/ButtonsInputLib.h>
static CONST ButtonTarget Keys[]={
 {"power",BUTTON_SOURCE_PON,BUTTON_CODE_POWER,"pmk",0,0x13,0,7,FALSE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
 {"up",BUTTON_SOURCE_PMIC_GPIO,BUTTON_CODE_VOLUME_UP,"pm",1,0x8d,6,0,TRUE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH},
 {"down",BUTTON_SOURCE_PON,BUTTON_CODE_VOLUME_DOWN,"pmk",0,0x13,0,6,FALSE,15000,CR_INTERRUPT_TRIGGER_EDGE_BOTH}};
static CONST ButtonsTargetContext Target={Keys,3};
static struct {
 UINT32 Reads,Acquire,Release,Register,Enable,Unregister,Wake,Submit;
 UINT32 FailRead,FailRegister,FailEnable,FailUnregister;
 BOOLEAN GpioOwned,GpioLevel,FailAcquire,FailRelease,FailSubmit,Foreign;
 UINT8 Subtype,Pressed,Report;CR_INTERRUPT_HANDLER Handlers[3];VOID *Params[3];
} F;
static UINT32 Index(CONST ButtonTarget *T){return (UINT32)(T-Keys);}
static CR_STATUS Read(VOID *C,UINT8 Sid,UINT16 A,UINT8 *D,UINTN N){assert(C==&F&&N==1);if(++F.Reads==F.FailRead)return CR_DEVICE_ERROR;
 if((A&255)==0x15)*D=F.Foreign?0xff:0;else if((A&255)==5)*D=F.Subtype;else{assert(Sid==0&&A==0x1310);*D=(UINT8)(((F.Pressed&1)?0x80:0)|((F.Pressed&4)?0x40:0));}return CR_SUCCESS;}
static CR_STATUS GpioRead(VOID *C,VOID *O,UINT16 Pin,BOOLEAN *V){assert(C==&F&&O&&Pin==6&&F.GpioOwned);*V=F.GpioLevel;return CR_SUCCESS;}
static CR_STATUS Acquire(VOID *C,VOID *O,UINT16 Pin){assert(C==&F&&O&&Pin==6);++F.Acquire;F.GpioOwned=TRUE;return F.FailAcquire?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS Release(VOID *C,VOID *O,UINT16 Pin){assert(C==&F&&O&&Pin==6&&F.GpioOwned);for(UINT32 I=0;I<3;++I)assert(!F.Handlers[I]);++F.Release;if(F.FailRelease)return CR_DEVICE_ERROR;F.GpioOwned=FALSE;return CR_SUCCESS;}
static CR_STATUS Register(VOID *C,CONST ButtonTarget *T,CR_INTERRUPT_HANDLER H,VOID *P){assert(C==&F&&F.GpioOwned);if(++F.Register==F.FailRegister)return CR_DEVICE_ERROR;UINT32 I=Index(T);assert(!F.Handlers[I]);F.Handlers[I]=H;F.Params[I]=P;return CR_SUCCESS;}
static CR_STATUS Enable(VOID *C,CONST ButtonTarget *T,BOOLEAN On){assert(C==&F&&F.Handlers[Index(T)]);++F.Enable;if(F.Enable==F.FailEnable)return CR_DEVICE_ERROR;(VOID)On;return CR_SUCCESS;}
static CR_STATUS Unregister(VOID *C,CONST ButtonTarget *T){assert(C==&F);if(++F.Unregister==F.FailUnregister)return CR_DEVICE_ERROR;F.Handlers[Index(T)]=NULL;F.Params[Index(T)]=NULL;return CR_SUCCESS;}
static VOID Wake(VOID *C){assert(C==&F);++F.Wake;}
static CR_STATUS Submit(VOID *C,UINT8 Mask){assert(C==&F&&Mask<8);++F.Submit;if(F.FailSubmit)return CR_DEVICE_ERROR;F.Report=Mask;return CR_SUCCESS;}
static VOID Setup(CR_BUTTON_INPUT *B){memset(&F,0,sizeof(F));memset(B,0,sizeof(*B));F.GpioLevel=TRUE;F.Subtype=9;CR_BUTTON_INPUT_IO Io={&F,Read,GpioRead,Acquire,Release,Register,Enable,Unregister,Wake,Submit};assert(CrButtonInputInit(B,&Target,&Io)==CR_SUCCESS&&!F.Reads&&!F.Acquire);}
static VOID Irq(UINT32 I){assert(F.Handlers[I]);F.Handlers[I](F.Params[I]);}
static VOID Clean(CR_BUTTON_INPUT *B){assert(CrButtonInputStop(B)==CR_SUCCESS&&!B->Initialized&&!B->GpioAttempted&&!F.GpioOwned);for(UINT32 I=0;I<3;++I)assert(!F.Handlers[I]);}
int main(void){CR_BUTTON_INPUT B;
 Setup(&B);F.Foreign=TRUE;assert(CrButtonInputStart(&B,0)==CR_NOT_READY&&!F.Acquire&&!F.Register);
 Setup(&B);F.Subtype=1;assert(CrButtonInputStart(&B,0)==CR_UNSUPPORTED&&!F.Acquire);
 for(UINT32 I=1;I<=5;++I){Setup(&B);F.FailRead=I;assert(CrButtonInputStart(&B,0)==CR_DEVICE_ERROR&&!F.Acquire&&!B.Initialized);}
 Setup(&B);F.FailAcquire=TRUE;assert(CrButtonInputStart(&B,0)==CR_DEVICE_ERROR&&B.GpioAttempted&&F.GpioOwned);F.FailRelease=TRUE;assert(CrButtonInputStop(&B)==CR_DEVICE_ERROR&&B.GpioAttempted);F.FailRelease=FALSE;Clean(&B);
 for(UINT32 I=1;I<=3;++I){Setup(&B);F.FailRegister=I;assert(CrButtonInputStart(&B,0)==CR_DEVICE_ERROR);Clean(&B);Setup(&B);F.FailEnable=I;assert(CrButtonInputStart(&B,0)==CR_DEVICE_ERROR);F.FailEnable=0;Clean(&B);}
 Setup(&B);assert(CrButtonInputStart(&B,0)==CR_SUCCESS&&CrButtonInputService(&B,0)==CR_SUCCESS&&F.Report==0&&!CrButtonInputNextUs(&B,0));UINT32 Reads=F.Reads;assert(CrButtonInputService(&B,100000)==CR_SUCCESS&&F.Reads==Reads); // idle never polls
 F.Pressed=1;Irq(0);assert(CrButtonInputService(&B,100000)==CR_SUCCESS&&F.Report==0&&CrButtonInputNextUs(&B,100000)==15000);
 assert(CrButtonInputService(&B,114999)==CR_SUCCESS&&F.Report==0);assert(CrButtonInputService(&B,115000)==CR_SUCCESS&&F.Report==1&&!CrButtonInputNextUs(&B,115000));
 F.Pressed=0;Irq(0);assert(CrButtonInputService(&B,120000)==CR_SUCCESS);F.Pressed=1;Irq(0);assert(CrButtonInputService(&B,125000)==CR_SUCCESS&&!CrButtonInputNextUs(&B,125000)&&F.Report==1); // bounce cancels pending release
 F.GpioLevel=FALSE;Irq(1);assert(CrButtonInputService(&B,130000)==CR_SUCCESS);F.FailSubmit=TRUE;assert(CrButtonInputService(&B,145000)==CR_DEVICE_ERROR&&B.ReportPending&&B.Reported==1);F.FailSubmit=FALSE;assert(CrButtonInputService(&B,145001)==CR_SUCCESS&&F.Report==3); // report failure retains state
 F.FailUnregister=1;assert(CrButtonInputStop(&B)==CR_DEVICE_ERROR&&F.GpioOwned&&F.Handlers[0]&&!F.Release);UINT32 W=F.Wake;Irq(0);assert(F.Wake==W+1);F.FailUnregister=0;Clean(&B); // callback context valid through failed drain
 Setup(&B);F.Pressed=1;F.GpioLevel=FALSE;assert(CrButtonInputStart(&B,0)==CR_SUCCESS&&CrButtonInputService(&B,0)==CR_SUCCESS&&F.Report==0);F.Pressed=0;F.GpioLevel=TRUE;Irq(0);Irq(1);assert(CrButtonInputService(&B,100)==CR_SUCCESS&&CrButtonInputService(&B,15100)==CR_SUCCESS&&F.Report==0);F.Pressed=1;Irq(0);assert(CrButtonInputService(&B,20000)==CR_SUCCESS&&CrButtonInputService(&B,35000)==CR_SUCCESS&&F.Report==1);Clean(&B);
 puts("ButtonsInput actual native/NT core: idle IRQ wake, physical polarities, debounce/boot-held suppression, partial acquisition, retained unregister/GPIO and report retry PASS");return 0;
}
