/* Actual Android parser and actual repository libfdt. */
#include <Library/AndroidBootCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
#endif
typedef struct {
 _Alignas(8) unsigned char Fdt[65536],Other[65536],Initrd[65536];
 uint64_t FdtBase,InitrdBase;
 size_t FdtSize,InitrdSize,Calls,Deny,Maximum;
 int Torn,Unaligned;
} FIXTURE;
static FIXTURE F;
static unsigned Checks;
static void Put32(unsigned char *P,uint32_t V){P[0]=V;P[1]=V>>8;P[2]=V>>16;P[3]=V>>24;}
static void *Map(void *C,uint64_t Address,size_t N) {
 FIXTURE *X=C;++X->Calls;assert(N!=0);if(N>X->Maximum)X->Maximum=N;
 if(X->Deny==X->Calls)return NULL;
 if(Address==X->FdtBase && N<=X->FdtSize) {
  if(X->Torn && N!=40)return X->Other;
  return X->Fdt+(X->Unaligned?1:0);
 }
 if(Address>=X->InitrdBase && Address-X->InitrdBase<=X->InitrdSize &&
    N<=X->InitrdSize-(Address-X->InitrdBase))return X->Initrd+(Address-X->InitrdBase);
 return NULL;
}
static const void *MapConst(void *C,UINT64 A,UINTN N){return Map(C,A,N);}
static void Build(const char *Args,const char *Config,unsigned Pad,unsigned Cells) {
 memset(&F,0,sizeof(F));F.FdtBase=0x88000000;F.InitrdBase=Cells==8?0x190000000ULL:0x90000000;
 assert(fdt_create(F.Fdt,sizeof(F.Fdt))==0);assert(fdt_finish_reservemap(F.Fdt)==0);
 assert(fdt_begin_node(F.Fdt,"")==0);assert(fdt_begin_node(F.Fdt,"chosen")==0);
 if(Args)assert(fdt_property(F.Fdt,"bootargs",Args,strlen(Args)+1)==0);
 if(Config) {
  size_t N=strlen(Config);unsigned Sum=0;memset(F.Initrd,0x65,64);
  memcpy(F.Initrd+64,Config,N);
  for(size_t I=0;I<N;++I)Sum+=(unsigned char)Config[I];
  Put32(F.Initrd+64+N,N);Put32(F.Initrd+68+N,Sum);memcpy(F.Initrd+72+N,"#BOOTCONFIG\n",12);
  F.InitrdSize=84+N+Pad;memset(F.Initrd+84+N,0x69,Pad);
  if(Cells==8){fdt64_t B=cpu_to_fdt64(F.InitrdBase),E=cpu_to_fdt64(F.InitrdBase+F.InitrdSize);
   assert(fdt_property(F.Fdt,"linux,initrd-start",&B,8)==0);
   assert(fdt_property(F.Fdt,"linux,initrd-end",&E,8)==0);
  }else{fdt32_t B=cpu_to_fdt32(F.InitrdBase),E=cpu_to_fdt32(F.InitrdBase+F.InitrdSize);
   assert(fdt_property(F.Fdt,"linux,initrd-start",&B,4)==0);
   assert(fdt_property(F.Fdt,"linux,initrd-end",&E,4)==0);}
 }
 assert(fdt_end_node(F.Fdt)==0);assert(fdt_end_node(F.Fdt)==0);assert(fdt_finish(F.Fdt)==0);
 F.FdtSize=fdt_totalsize(F.Fdt);assert(fdt_check_full(F.Fdt,F.FdtSize)==0);
 memcpy(F.Other,F.Fdt,F.FdtSize);
}
static void Success(unsigned Mode,unsigned Source,unsigned Flags) {
 CR_ANDROID_BOOT_INFO I;memset(&I,0xcc,sizeof(I));
 assert(CrAndroidBootInspect(F.FdtBase,MapConst,&F,&I)==CR_SUCCESS);
 assert(I.Version==1 && I.Mode==Mode && I.Source==Source && I.FdtAddress==F.FdtBase);
 assert(I.Flags==Flags);++Checks;
}
static void Failure(void) {
 CR_ANDROID_BOOT_INFO I,Old;memset(&I,0x55,sizeof(I));memcpy(&Old,&I,sizeof(I));
 assert(CrAndroidBootInspect(F.FdtBase,MapConst,&F,&I)!=CR_SUCCESS);
 assert(memcmp(&I,&Old,sizeof(I))==0);++Checks;
}
static void Prop(const char *Name,const void *Data,int N) {
 assert(fdt_open_into(F.Fdt,F.Fdt,sizeof(F.Fdt))==0);
 assert(fdt_setprop(F.Fdt,fdt_path_offset(F.Fdt,"/chosen"),Name,Data,N)==0);
 assert(fdt_pack(F.Fdt)==0);F.FdtSize=fdt_totalsize(F.Fdt);
}
static void Duplicate(int Chosen) {
 memset(&F,0,sizeof(F));F.FdtBase=0x88000000;
 assert(fdt_create(F.Fdt,sizeof(F.Fdt))==0);assert(fdt_finish_reservemap(F.Fdt)==0);
 assert(fdt_begin_node(F.Fdt,"")==0);
 for(int N=0;N<(Chosen?2:1);++N){assert(fdt_begin_node(F.Fdt,"chosen")==0);
  assert(fdt_property_string(F.Fdt,"bootargs","androidboot.mode=charger")==0);
  if(!Chosen)assert(fdt_property_string(F.Fdt,"bootargs","androidboot.mode=normal")==0);
  assert(fdt_end_node(F.Fdt)==0);}
 assert(fdt_end_node(F.Fdt)==0);assert(fdt_finish(F.Fdt)==0);F.FdtSize=fdt_totalsize(F.Fdt);
}
int main(void) {
 const unsigned V=CR_ANDROID_BOOT_FDT_VALID,C=V|CR_ANDROID_BOOT_CMDLINE_PRESENT,
  B=V|CR_ANDROID_BOOT_BOOTCONFIG_PRESENT,CB=C|CR_ANDROID_BOOT_BOOTCONFIG_PRESENT;
 const char *Good[]={"androidboot.mode=charger","androidboot.mode = 'charger';",
  "androidboot { mode = \"charger\" }","androidboot { # comment\n mode=charger\n}",
  "foo=a,b,c; androidboot.mode=charger","foo=hello; foo+=world; androidboot.mode=charger",
  "foo='x};#,'; androidboot.mode=charger", "androidboot . mode=charger",
  "androidboot.mode=charger\nandroidboot.mode=charger\n"};
 const char *Bad[]={"androidboot.mode=charger\nandroidboot.mode=normal",
  "androidboot.mode=charger,normal","androidboot.mode+=charger","androidboot.mode:=charger",
  "androidboot { mode=charger", "androidboot.mode='charger", "androidboot.mode=",
  "androidboot.mode { foo=charger }", "androidboot.mode", "androidboot..mode=charger",
  "androidboot { mode=charger }}", "androidboot.mode='charger'garbage",
  "androidboot.mode='char\nger'", "androidboot.mode=charger\n bad@key=value"};
 Build(NULL,NULL,0,4);Success(0,0,V);
 Build("console=tty androidboot.mode=charger debug",NULL,0,4);Success(2,1,C);
 Build("androidboot.mode=\"charger\"",NULL,0,4);Success(2,1,C);
 Build("\"androidboot.mode=charger\"",NULL,0,4);Success(2,1,C);
 Build("androidboot.mode=charger androidboot.mode=charger",NULL,0,4);Success(2,1,C);
 Build("androidboot.mode=charger androidboot.mode=normal",NULL,0,4);Failure();
 Build("androidboot.mode=\"charger",NULL,0,4);Failure();
 Build("androidboot.mode=normal androidboot.mode=recovery",NULL,0,4);Failure();
 Build("xandroidboot.mode=charger androidboot.mode_extra=charger androidboot.mode=chargerXYZ",NULL,0,4);
 Success(1,1,C);
 Build("androidboot.\"mode\"=charger -- foo=\"androidboot.mode=charger\"",NULL,0,4);Success(0,0,C);
 for(unsigned P=0;P<4;++P)for(unsigned Cells=4;Cells<=8;Cells+=4)
  for(size_t I=0;I<sizeof(Good)/sizeof(*Good);++I){Build(NULL,Good[I],P,Cells);Success(2,2,B);}
 for(size_t I=0;I<sizeof(Bad)/sizeof(*Bad);++I){Build(NULL,Bad[I],0,8);Failure();}
 Build("androidboot.mode=normal","androidboot.mode=charger",3,8);Success(2,2,CB|8);
 Build("androidboot.mode=charger","androidboot.mode=normal",0,4);Success(1,2,CB|8);
 Build("androidboot.mode=charger","kernel.androidboot.mode=normal",0,8);Success(2,1,CB);
 Build(NULL,"kernel.androidboot.mode=charger",0,8);Success(0,0,B);
 Build(NULL,"androidboot.mode=charger",0,4);F.Initrd[64]^=1;Failure();
 Build(NULL,"androidboot.mode=charger",0,4);Put32(F.Initrd+F.InitrdSize-20,65536);Failure();
 Build("androidboot.mode=charger","androidboot.mode=normal",0,4);F.Initrd[F.InitrdSize-1]=0;Success(2,1,C);
 Build(NULL,NULL,0,4);Prop("bootargs","androidboot.mode=charger",23);Failure();
 Build(NULL,NULL,0,4);char Embedded[]="androidboot.mode=charger\0bad";Prop("bootargs",Embedded,sizeof(Embedded));Failure();
 Build(NULL,NULL,0,4);fdt32_t Z=0;Prop("linux,initrd-start",&Z,4);Failure();
 Build(NULL,NULL,0,4);Prop("linux,initrd-start",&Z,4);Prop("linux,initrd-end",&Z,4);Success(0,0,V);
 Build(NULL,NULL,0,4);fdt32_t N=cpu_to_fdt32(0x90000000);Prop("linux,initrd-start",&N,4);Prop("linux,initrd-end",&N,4);Success(0,0,V);
 Build(NULL,"androidboot.mode=charger",0,4);Prop("linux,initrd-end",&Z,4);Failure();
 Build(NULL,"androidboot.mode=charger",0,4);Prop("linux,initrd-end",&Z,3);Failure();
 Duplicate(0);Failure();Duplicate(1);Failure();
 for(unsigned D=1;D<=4;++D){Build(NULL,"androidboot.mode=charger",0,8);F.Deny=D;Failure();}
 Build(NULL,NULL,0,4);F.Torn=1;F.Other[19]^=1;Failure();
 Build(NULL,NULL,0,4);F.Unaligned=1;Failure();
 Build(NULL,NULL,0,4);fdt_set_off_mem_rsvmap(F.Fdt,fdt_off_mem_rsvmap(F.Fdt)+1);Failure();
 Build(NULL,NULL,0,4);fdt_set_off_dt_struct(F.Fdt,fdt_off_dt_struct(F.Fdt)+1);Failure();
 Build(NULL,NULL,0,4);F.FdtBase=UINT64_MAX-7;Failure();assert(F.Calls==0);
 Build(NULL,NULL,0,4);F.FdtBase|=1;Failure();assert(F.Calls==0);
 Build(NULL,NULL,0,4);F.Fdt[4]=0xff;Failure();assert(F.Calls==1);
 Build(NULL,NULL,0,4);fdt_set_off_mem_rsvmap(F.Fdt,fdt_off_dt_struct(F.Fdt));Failure();
 Build(NULL,NULL,0,4);fdt_set_size_dt_struct(F.Fdt,3);Failure();
 Build(NULL,NULL,0,4);fdt_set_off_dt_strings(F.Fdt,UINT32_MAX);Failure();
 Build(NULL,NULL,0,4);memcpy(F.Other,F.Fdt,F.FdtSize);
 size_t Total=F.FdtSize;unsigned Rng=0x456789ab;
 for(unsigned J=0;J<12000;++J) {
  memcpy(F.Fdt,F.Other,Total);Rng=Rng*1664525+1013904223;
  size_t Pos=Rng%Total;Rng=Rng*1664525+1013904223;F.Fdt[Pos]^=(Rng>>24)|1;
  CR_ANDROID_BOOT_INFO I,Old;memset(&I,0x41,sizeof(I));Old=I;
  CR_STATUS S=CrAndroidBootInspect(F.FdtBase,MapConst,&F,&I);
  if(S!=CR_SUCCESS)assert(memcmp(&I,&Old,sizeof(I))==0);
  else assert(I.Mode==0 || I.Mode==1 || I.Mode==2);
  ++Checks;
 }
 Build(NULL,"androidboot.mode=charger",0,8);
 CR_ANDROID_BOOT_INFO *Aliased=(void *)(F.Fdt+48);unsigned char Copy[65536];memcpy(Copy,F.Fdt,F.FdtSize);
 assert(CrAndroidBootInspect(F.FdtBase,MapConst,&F,Aliased)!=CR_SUCCESS);
 assert(memcmp(Copy,F.Fdt,F.FdtSize)==0);++Checks;
 printf("AndroidBootCr actual libfdt: %u checks PASS\n",Checks);return 0;
}
