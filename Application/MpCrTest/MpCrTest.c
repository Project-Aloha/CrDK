/** Bounded AP work to validate the firmware's actual MP Services. MIT */
#include <PiDxe.h>
#include <Library/ArmLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/MpService.h>

#define MP_CR_TEST_MAX_CPUS 64U
#define MP_CR_TEST_DONE 0x43524D50U
#define MPIDR_AFFINITY_MASK 0xff00ffffffULL
typedef struct {
  UINT64 Mpidr;
  UINT32 Checksum;
  volatile UINT32 Done;
} MP_RESULT;
typedef struct {
  EFI_MP_SERVICES_PROTOCOL *Mp;
  MP_RESULT *Results;
  UINTN Count;
} MP_WORK;

STATIC UINT32 Calculate(UINTN Cpu){
  UINT32 V=(UINT32)Cpu^0x8450;
  for(UINT32 I=0;I<4096;++I)V=(V<<5)^(V>>27)^I;
  return V;
}
STATIC VOID EFIAPI ApWork(VOID *Argument){
  MP_WORK *Work=Argument;UINTN Cpu;
  /* WhoAmI is an AP-callable MP method. No allocation, console output or
   * other Boot Services run on secondary processors. */
  if(Work->Mp->WhoAmI(Work->Mp,&Cpu)!=EFI_SUCCESS||Cpu>=Work->Count)return;
  MP_RESULT *R=&Work->Results[Cpu];
  R->Mpidr=ArmReadMpidr()&MPIDR_AFFINITY_MASK;R->Checksum=Calculate(Cpu);
  MemoryFence();R->Done=MP_CR_TEST_DONE;
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE ImageHandle,EFI_SYSTEM_TABLE *SystemTable){
  (VOID)ImageHandle;(VOID)SystemTable;
  EFI_MP_SERVICES_PROTOCOL *Mp=NULL;
  EFI_STATUS S=gBS->LocateProtocol(&gEfiMpServiceProtocolGuid,NULL,(VOID **)&Mp);
  if(S!=EFI_SUCCESS){Print(L"MP Services unavailable: %r\n",S);return S;}
  UINTN Count=0,Enabled=0,Bsp=0;
  S=Mp->GetNumberOfProcessors(Mp,&Count,&Enabled);if(S!=EFI_SUCCESS)return S;
  if(!Count||Count>MP_CR_TEST_MAX_CPUS||Enabled>Count)return EFI_COMPROMISED_DATA;
  S=Mp->WhoAmI(Mp,&Bsp);if(S!=EFI_SUCCESS)return S;if(Bsp>=Count)return EFI_COMPROMISED_DATA;
  Print(L"MP: %u processors, %u enabled, BSP %u\n",Count,Enabled,Bsp);
  MP_RESULT *Results=AllocateZeroPool(Count*sizeof(*Results));if(!Results)return EFI_OUT_OF_RESOURCES;
  MP_WORK Work={Mp,Results,Count};UINTN *Failed=NULL;
  /* The callback above is bounded. Timeout=0 avoids requesting cancellation
   * on providers that cannot forcibly terminate an arbitrary AP callback. */
  S=Mp->StartupAllAPs(Mp,ApWork,FALSE,NULL,0,&Work,&Failed);
  if(S!=EFI_SUCCESS){Print(L"MP dispatch failed: %r\n",S);goto Done;}
  UINTN Verified=0;
  for(UINTN I=0;I<Count;++I){
    EFI_PROCESSOR_INFORMATION Info;S=Mp->GetProcessorInfo(Mp,I,&Info);if(S!=EFI_SUCCESS)goto Done;
    if(I==Bsp||!(Info.StatusFlag&PROCESSOR_ENABLED_BIT))continue;
    UINT32 Complete=Results[I].Done;MemoryFence();
    if(Complete!=MP_CR_TEST_DONE||Results[I].Checksum!=Calculate(I)||
       Results[I].Mpidr!=(Info.ProcessorId&MPIDR_AFFINITY_MASK)){
      Print(L"AP %u verification failed\n",I);S=EFI_DEVICE_ERROR;goto Done;
    }
    ++Verified;Print(L"AP %u: MPIDR %lx, checksum %08x PASS\n",I,Results[I].Mpidr,Results[I].Checksum);
  }
  if(Verified+1!=Enabled)S=EFI_DEVICE_ERROR;
  Print(L"MP test: %u secondary processors verified: %r\n",Verified,S);
Done:
  if(Failed)FreePool(Failed);FreePool(Results);return S;
}
