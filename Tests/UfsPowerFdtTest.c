/* Actual no-allocation UFS power parser and repository libfdt. SPDX-License-Identifier: MIT */
#include <Library/UfsPowerFdtCrLib.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libfdt.h>
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static _Alignas(8) unsigned char Blob[2*1024*1024],Original[2*1024*1024];
static unsigned Checks;
static CR_UFS_POWER_FDT Parse(CR_STATUS Expected) {
  CR_UFS_POWER_FDT R,Before;memset(&R,0xa5,sizeof(R));Before=R;
  CR_STATUS S=CrUfsPowerFdtParse(Blob,fdt_totalsize(Blob),&R);
  if(S!=Expected)fprintf(stderr,"case%u status%zx expected%zx\n",Checks,(size_t)S,(size_t)Expected);
  assert(S==Expected);
  if(S!=CR_SUCCESS)assert(memcmp(&R,&Before,sizeof(R))==0);
  else assert(R.Version==1 && R.Size==sizeof(R) && sizeof(R)==544);
  ++Checks;return R;
}

static int Node(const char *Path){int N=fdt_path_offset(Blob,Path);assert(N>=0);return N;}
static void Set(const char *Path,const char *Name,const void *Value,int Size){assert(fdt_setprop(Blob,Node(Path),Name,Value,Size)==0);}
static void Cell(const char *Path,const char *Name,unsigned Value){fdt32_t V=cpu_to_fdt32(Value);Set(Path,Name,&V,4);}
static void Text(const char *Path,const char *Name,const char *Value){Set(Path,Name,Value,(int)strlen(Value)+1);}
static void Add(const char *Parent,const char *Name){assert(fdt_add_subnode(Blob,Node(Parent),Name)>=0);}
static void Del(const char *Path,const char *Name){assert(fdt_delprop(Blob,Node(Path),Name)==0);}
static void Rpmh(const char *Name,unsigned Id,unsigned Low,unsigned High,unsigned Init){
 char P[96],C[96];Add("/soc",Name);snprintf(P,sizeof(P),"/soc/%s",Name);Text(P,"compatible","qcom,rpmh-vrm-regulator");Text(P,"qcom,resource-name",Name);
 Add(P,"regulator");snprintf(C,sizeof(C),"%s/regulator",P);Cell(C,"phandle",Id);Cell(C,"regulator-min-microvolt",Low);Cell(C,"regulator-max-microvolt",High);Cell(C,"qcom,init-voltage",Init);Cell(C,"qcom,set",3);
}
static void Small(void){
 assert(fdt_create_empty_tree(Blob,sizeof(Blob))==0);Cell("/","#address-cells",2);Cell("/","#size-cells",2);
 Add("/","soc");Cell("/soc","#address-cells",1);Cell("/soc","#size-cells",1);fdt32_t R[]={0,0,cpu_to_fdt32(0x1d00000),cpu_to_fdt32(0x100000)};Set("/soc","ranges",R,sizeof(R));
 Add("/soc","ufs");Text("/soc/ufs","compatible","qcom,ufshc");Text("/soc/ufs","phy-names","ufsphy");Cell("/soc/ufs","phys",2);fdt32_t H[]={cpu_to_fdt32(0x84000),cpu_to_fdt32(0x3000)};Set("/soc/ufs","reg",H,sizeof(H));
 Add("/soc","phy");Text("/soc/phy","compatible","qcom,sm8250-qmp-ufs-phy");Cell("/soc/phy","phandle",2);Cell("/soc/phy","#phy-cells",0);fdt32_t P[]={cpu_to_fdt32(0x87000),cpu_to_fdt32(0x1000)};Set("/soc/phy","reg",P,sizeof(P));
 Rpmh("ldoa17",11,2496000,3008000,2496000);Rpmh("ldoa6",12,1200000,1200000,1200000);Rpmh("ldoa5",13,880000,880000,880000);Rpmh("ldoa9",14,1200000,1200000,1200000);
 Cell("/soc/ufs","vcc-supply",11);Cell("/soc/ufs","vccq-supply",12);Cell("/soc/ufs","qcom,vddp-ref-clk-supply",12);Cell("/soc/phy","vdda-phy-supply",13);Cell("/soc/phy","vdda-pll-supply",14);
 fdt32_t V[]={cpu_to_fdt32(2504000),cpu_to_fdt32(2950000)};Set("/soc/ufs","vcc-voltage-level",V,sizeof(V));
 Add("/","fixed");Cell("/fixed","phandle",20);Text("/fixed","compatible","regulator-fixed");Cell("/fixed","regulator-min-microvolt",1800000);Cell("/fixed","regulator-max-microvolt",1800000);Set("/fixed","regulator-always-on",NULL,0);Cell("/soc/ufs","vccq2-supply",20);
}
static void Duplicate(int Kind){
 assert(fdt_create(Blob,sizeof(Blob))==0);assert(fdt_finish_reservemap(Blob)==0);assert(fdt_begin_node(Blob,"")==0);
 if(Kind==0){assert(fdt_property_u32(Blob,"phandle",1)==0);assert(fdt_property_u32(Blob,"phandle",1)==0);}
 else for(int I=0;I<2;++I){assert(fdt_begin_node(Blob,"same")==0);assert(fdt_end_node(Blob)==0);}
 assert(fdt_end_node(Blob)==0);assert(fdt_finish(Blob)==0);Parse(CR_COMPROMISED_DATA);
}
static void Adversarial(void){
 CR_UFS_POWER_FDT R;
 Small();R=Parse(CR_SUCCESS);assert(R.HostBase==0x1d84000 && R.PhyBase==0x1d87000);assert(R.Supplies[0].InitUv==2496000 && R.Supplies[0].ConsumerMinUv==2504000);
 Small();Text("/soc/ufs","status","disabled");Parse(CR_NOT_FOUND);
 Small();Text("/soc","status","disabled");Parse(CR_NOT_FOUND);
 Small();Set("/soc/ufs","status","ok",2);Parse(CR_COMPROMISED_DATA);
 Small();Add("/soc","other-ufs");Text("/soc/other-ufs","compatible","qcom,ufshc");Parse(CR_COMPROMISED_DATA);
 Small();Text("/soc/phy","status","disabled");Parse(CR_UNSUPPORTED);
 Small();Text("/soc/ldoa5","status","disabled");Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/phy","#phy-cells",1);Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ufs","phys",9999);Parse(CR_COMPROMISED_DATA);
 Small();fdt32_t More[]={cpu_to_fdt32(2),0};Set("/soc/ufs","phys",More,sizeof(More));Parse(CR_COMPROMISED_DATA);
 Small();Cell("/fixed","phandle",11);Parse(CR_COMPROMISED_DATA);
 Small();Cell("/fixed","linux,phandle",21);Parse(CR_COMPROMISED_DATA);
 Small();Cell("/fixed","linux,phandle",20);Parse(CR_SUCCESS);
 Small();Cell("/fixed","phandle",0);Parse(CR_COMPROMISED_DATA);
 Small();Text("/soc/ldoa17","qcom,resource-name","mx.lvl");Parse(CR_UNSUPPORTED);
 Small();Text("/soc/ldoa17","compatible","vendor,unknown-regulator");Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ldoa17/regulator","qcom,set",2);Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ldoa17/regulator","qcom,set",1);Parse(CR_SUCCESS);
 Small();Cell("/soc/ldoa17/regulator","qcom,init-voltage",3009000);Parse(CR_COMPROMISED_DATA);
 Small();Del("/soc/ldoa17/regulator","qcom,init-voltage");R=Parse(CR_SUCCESS);assert(!(R.Supplies[0].Flags&CR_UFS_POWER_INIT_PRESENT));
 Small();Cell("/soc/ldoa17/regulator","regulator-max-microvolt",2000000);Parse(CR_COMPROMISED_DATA);
 Small();Del("/soc/ldoa17/regulator","regulator-max-microvolt");Parse(CR_COMPROMISED_DATA);
 Small();Cell("/soc/ufs","vcc-voltage-level",2504000);Parse(CR_COMPROMISED_DATA);
 Small();Del("/soc/phy","vdda-phy-supply");Parse(CR_UNSUPPORTED);
 Small();Del("/soc/ufs","vccq2-supply");R=Parse(CR_SUCCESS);assert(R.Supplies[2].Kind==CrUfsPowerAbsentInherited);
 Small();Cell("/soc/ufs","qcom,vddp-ref-clk-supply",11);Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ufs","unknown-supply",11);Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ufs","qcom,vccq-parent-supply",11);R=Parse(CR_SUCCESS);assert(!strcmp(R.VccqParent.Resource,"ldoa17"));
 Small();Cell("/fixed","gpio",1);Parse(CR_UNSUPPORTED);
 Small();Cell("/fixed","clocks",1);Parse(CR_UNSUPPORTED);
 Small();Del("/fixed","regulator-always-on");Set("/fixed","regulator-boot-on",NULL,0);Parse(CR_UNSUPPORTED);
 Small();Cell("/fixed","regulator-max-microvolt",1900000);Parse(CR_UNSUPPORTED);
 Small();Cell("/fixed","regulator-always-on",1);Parse(CR_COMPROMISED_DATA);
 Small();Del("/soc","ranges");Parse(CR_UNSUPPORTED);
 Small();fdt32_t Overlap[]={0,0,cpu_to_fdt32(0x1d00000),cpu_to_fdt32(0x100000),0,0,cpu_to_fdt32(0x1d00000),cpu_to_fdt32(0x100000)};Set("/soc","ranges",Overlap,sizeof(Overlap));Parse(CR_COMPROMISED_DATA);
 Small();Cell("/soc","ranges",0);Parse(CR_COMPROMISED_DATA);
 Small();Cell("/soc","#address-cells",3);Parse(CR_UNSUPPORTED);
 Small();fdt32_t Wrap[]={cpu_to_fdt32(0x87000),0};Set("/soc/ufs","reg",Wrap,sizeof(Wrap));Parse(CR_COMPROMISED_DATA);
 Small();Wrap[1]=cpu_to_fdt32(0x1000);Set("/soc/ufs","reg",Wrap,sizeof(Wrap));Parse(CR_COMPROMISED_DATA);
 Small();Add("/","vph");Text("/vph","compatible","regulator-fixed");Cell("/vph","phandle",40);Cell("/vph","regulator-min-microvolt",3700000);Cell("/vph","regulator-max-microvolt",3700000);Cell("/fixed","vin-supply",40);Parse(CR_SUCCESS);
 Cell("/vph","gpio",1);Parse(CR_UNSUPPORTED);Del("/vph","gpio");Cell("/vph","vin-supply",20);Parse(CR_COMPROMISED_DATA);
 Small();Set("/fixed","enable-active-high",NULL,0);Parse(CR_UNSUPPORTED);
 Small();Cell("/soc/ufs","qcom,vddp-ref-clk-min-microvolt",1300000);Cell("/soc/ufs","qcom,vddp-ref-clk-max-microvolt",1400000);Parse(CR_UNSUPPORTED);
 Small();fdt_set_off_mem_rsvmap(Blob,41);Parse(CR_COMPROMISED_DATA);
 Small();fdt_set_off_dt_struct(Blob,73);Parse(CR_COMPROMISED_DATA);
 Duplicate(0);Duplicate(1);
 Small();assert(fdt_pack(Blob)==0);size_t N=fdt_totalsize(Blob);memcpy(Original,Blob,N);unsigned Seed=0x7261756c;
 for(unsigned I=0;I<256;++I){memcpy(Blob,Original,N);Seed=Seed*1664525+1013904223;size_t Pos=Seed%N;Blob[Pos]^=(unsigned char)((Seed>>24)|1);CR_UFS_POWER_FDT A,B;memset(&A,0xa5,sizeof(A));B=A;CR_STATUS S=CrUfsPowerFdtParse(Blob,N,&A);if(S!=CR_SUCCESS)assert(!memcmp(&A,&B,sizeof(A)));++Checks;}
 memcpy(Blob,Original,N);assert(CrUfsPowerFdtParse(Blob,N,(void*)(Blob+64))==CR_INVALID_PARAMETER);assert(!memcmp(Blob,Original,N));++Checks;
 assert(CrUfsPowerFdtParse(Blob+1,N-1,&R)==CR_INVALID_PARAMETER);++Checks;
 assert(CrUfsPowerFdtParse(Blob,39,&R)==CR_INVALID_PARAMETER);++Checks;
}

int main(int argc,char **argv) {
  for(int I=1;I<argc;++I){
    FILE *F=fopen(argv[I],"rb");assert(F);size_t N=fread(Blob,1,sizeof(Blob),F);fclose(F);assert(N>40 && N==fdt_totalsize(Blob));
    CR_UFS_POWER_FDT R=Parse(CR_SUCCESS);
    assert(R.HostBase==0x1d84000 && R.HostSize==0x3000 && R.PhyBase==0x1d87000);
    if(strstr(argv[I],"8250")){
      assert(!strcmp(R.Supplies[0].Resource,"ldoa17") && R.Supplies[0].MinUv==2496000 && R.Supplies[0].MaxUv==3008000);
      assert(!strcmp(R.Supplies[4].Resource,"ldoa5") && R.Supplies[4].MinUv==880000 && R.Supplies[4].MaxUv==880000);
      if(strstr(argv[I],"android"))assert(R.Supplies[0].InitUv==2496000 && R.Supplies[0].ConsumerMinUv==2504000 && R.Supplies[2].Kind==CrUfsPowerRpmh && !strcmp(R.Supplies[2].Resource,"smpa4") && R.Supplies[3].ProviderBase==0x177004 && R.PhySize==0xe00);
      else assert(R.Supplies[2].Kind==CrUfsPowerFixedAlwaysOn && R.Supplies[2].MinUv==1800000 && R.PhySize==0x1000);
    }else{
      assert(!strcmp(R.Supplies[0].Resource,"ldob7") && R.Supplies[0].MinUv==2504000 && R.Supplies[0].MaxUv==2504000 && R.Supplies[2].Kind==CrUfsPowerAbsentInherited);
      assert(!strcmp(R.Supplies[4].Resource,"ldob5") && R.Supplies[4].MinUv==880000);
      if(strstr(argv[I],"android"))assert(R.Supplies[4].MaxUv==912000 && R.Supplies[4].InitUv==880000 && R.Supplies[3].ProviderBase==0x187004 && R.PhySize==0xe10);
      else assert(R.Supplies[4].MaxUv==888000 && R.Supplies[3].Phandle==R.Supplies[1].Phandle && R.PhySize==0x1000);
    }
    printf("%s host=%llx/%llx phy=%llx/%llx\n",argv[I],(unsigned long long)R.HostBase,(unsigned long long)R.HostSize,(unsigned long long)R.PhyBase,(unsigned long long)R.PhySize);
    for(unsigned J=0;J<6;++J){CR_UFS_POWER_SUPPLY *S=&R.Supplies[J];printf("role%u kind%u %s %u..%u init%u consumer%u..%u flags%x\n",J,S->Kind,S->Resource,S->MinUv,S->MaxUv,S->InitUv,S->ConsumerMinUv,S->ConsumerMaxUv,S->Flags);}
    memcpy(Original,Blob,N);
  }
  Adversarial();
  printf("UFS power FDT PASS %u\n",Checks);return 0;
}
