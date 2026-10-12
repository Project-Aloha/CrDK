/* Exact Android HDK8150/HDK8245 topology qualification. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Library/FdtLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Protocol/EFICrSpmiProtocol.h>
STATIC BOOLEAN String(CONST VOID *F,INT32 N,CONST CHAR8 *Name,CONST CHAR8 *Value) {
 INT32 L;CONST CHAR8 *P=FdtGetProp(F,N,Name,&L);
 return P && L==(INT32)AsciiStrLen(Value)+1 && CompareMem(P,Value,(UINTN)L)==0;
}
STATIC BOOLEAN Bytes(CONST VOID *F,INT32 N,CONST CHAR8 *Name,CONST VOID *V,UINTN Count) {
 INT32 L;CONST VOID *P=FdtGetProp(F,N,Name,&L);
 return P && L==(INT32)Count && CompareMem(P,V,Count)==0;
}
STATIC BOOLEAN Cells(CONST VOID *F,INT32 N,CONST CHAR8 *Name,CONST UINT32 *V,UINTN Count) {
 INT32 L;CONST UINT32 *P=FdtGetProp(F,N,Name,&L);if(!P||L!=(INT32)(Count*4))return FALSE;
 for(UINTN I=0;I<Count;I++)if(Fdt32ToCpu(P[I])!=V[I])return FALSE;return TRUE;
}
STATIC BOOLEAN Enabled(CONST VOID *F,INT32 N) {
 for(INT32 P=N;P>=0;P=FdtParentOffset(F,P)) {
  INT32 L;CONST VOID *S=FdtGetProp(F,P,"status",&L);
  if(S && !String(F,P,"status","ok") && !String(F,P,"status","okay"))return FALSE;
  if(!S && L!=-FDT_ERR_NOTFOUND)return FALSE;
 }
 return N>=0;
}
EFI_STATUS SpmiCrQualifyTypeC(CONST VOID *F,UINTN Size,CONST SpmiDeviceContext *Bus,CR_PMIC_CC_TARGET *Out) {
 if(!F||!Out||!Bus||Size<sizeof(FDT_HEADER)||FdtCheckHeader(F)!=0||FdtTotalSize(F)>Size)return EFI_INVALID_PARAMETER;
 UINT32 M8150[]={339,0x20000},B8150[]={0x1001f,0},M8245[]={481,0x20001},B8245[]={0x1001f,3};
 CR_PMIC_CC_TARGET Candidate={0};
 if(Cells(F,0,"qcom,msm-id",M8150,2)&&Cells(F,0,"qcom,board-id",B8150,2))Candidate.Board=8150;
 else if(Cells(F,0,"qcom,msm-id",M8245,2)&&Cells(F,0,"qcom,board-id",B8245,2)){Candidate.Board=8245;Candidate.Batteryless=TRUE;}
 else return EFI_UNSUPPORTED;
 INT32 S=-1,Found=-1,D=0;
 while((S=FdtNextNode(F,S,&D))>=0) {
  INT32 L;CONST CHAR8 *P=FdtGetProp(F,S,"compatible",&L);
  if(P && L>0 && FdtStringListContains(P,L,"qcom,qpnp-smb5")) {
   if(Found>=0)return EFI_UNSUPPORTED;Found=S;
  }
 }
 if(S!=-FDT_ERR_NOTFOUND || !Enabled(F,Found))return EFI_UNSUPPORTED;
 INT32 Pmic=FdtParentOffset(F,Found),Arb=FdtParentOffset(F,Pmic);
 UINT32 Sid[]={2,0},Addr[]={1},SizeCells[]={1},ArbReg[]={0xc440000,0x1100,0xc600000,0x2000000,0xe600000,0x100000,0xe700000,0xa0000,0xc40a000,0x26000};
 if(!Cells(F,Pmic,"reg",Sid,2)||!Cells(F,Found,"#address-cells",Addr,1)||!Cells(F,Found,"#size-cells",SizeCells,1)||
    !Cells(F,Arb,"reg",ArbReg,10))return EFI_UNSUPPORTED;
 UINT32 Zero[]={0},Two[]={2},Four[]={4};
 CONST CHAR8 Names[]="core\0chnls\0obsrvr\0intr\0cnfg";
 if(!String(F,Arb,"compatible","qcom,spmi-pmic-arb")||!String(F,Pmic,"compatible","qcom,spmi-pmic")||
    !Bytes(F,Arb,"reg-names",Names,sizeof(Names))||!String(F,Arb,"interrupt-names","periph_irq")||
    !Cells(F,Arb,"qcom,ee",Zero,1)||!Cells(F,Arb,"qcom,channel",Zero,1)||!Cells(F,Arb,"cell-index",Zero,1)||
    !Cells(F,Arb,"#interrupt-cells",Four,1))return EFI_UNSUPPORTED;
 INT32 RouteLength;
 if(Candidate.Board==8150) {
  /* Android SM8150 uses the legacy PDC binding with GIC-style three-cell
     SPI481, while HDK8245 uses the two-cell PDC pin1 binding. */
  UINT32 GicIrq[]={0,481,0};
  if(!Cells(F,Arb,"interrupts",GicIrq,3))return EFI_UNSUPPORTED;
  CONST UINT32 *Parent=NULL;
  for(INT32 Node=Arb;Node>=0&&!Parent;Node=FdtParentOffset(F,Node))Parent=FdtGetProp(F,Node,"interrupt-parent",&RouteLength);
  if(!Parent||RouteLength!=4)return EFI_UNSUPPORTED;
  INT32 LegacyPdc=FdtNodeOffsetByPhandle(F,Fdt32ToCpu(*Parent));UINT32 Three[]={3},PdcReg[]={0xb220000,0x400};
  if(!Enabled(F,LegacyPdc)||!String(F,LegacyPdc,"compatible","qcom,pdc-sm8150")||
     !Cells(F,LegacyPdc,"#interrupt-cells",Three,1)||!Cells(F,LegacyPdc,"reg",PdcReg,2))return EFI_UNSUPPORTED;
  Parent=FdtGetProp(F,LegacyPdc,"interrupt-parent",&RouteLength);if(!Parent||RouteLength!=4)return EFI_UNSUPPORTED;
  INT32 Gic=FdtNodeOffsetByPhandle(F,Fdt32ToCpu(*Parent));
  if(!Enabled(F,Gic)||!String(F,Gic,"compatible","arm,gic-v3")||!Cells(F,Gic,"#interrupt-cells",Three,1))return EFI_UNSUPPORTED;
 }else {
  CONST UINT32 *Route=FdtGetProp(F,Arb,"interrupts-extended",&RouteLength);
  if(!Route||RouteLength!=12||Fdt32ToCpu(Route[1])!=1||Fdt32ToCpu(Route[2])!=4)return EFI_UNSUPPORTED;
  INT32 Pdc=FdtNodeOffsetByPhandle(F,Fdt32ToCpu(Route[0]));
  if(!Enabled(F,Pdc)||!Cells(F,Pdc,"#interrupt-cells",Two,1))return EFI_UNSUPPORTED;
  CONST UINT32 *Ranges=FdtGetProp(F,Pdc,"qcom,pdc-ranges",&RouteLength);
  if(!Ranges||RouteLength<12||RouteLength%12||Fdt32ToCpu(Ranges[0])!=0||
     Fdt32ToCpu(Ranges[1])!=480||Fdt32ToCpu(Ranges[2])<2)return EFI_UNSUPPORTED;
 }
 for(UINTN I=0;I<5;I++)if(Bus->Regions[I].BaseAddress!=ArbReg[I*2]||Bus->Regions[I].Size!=ArbReg[I*2+1])return EFI_UNSUPPORTED;
 if(Bus->PdcPinNumber!=1||Bus->ActiveEE||Bus->Channel||Bus->BusId)return EFI_UNSUPPORTED;
 INT32 L;CONST VOID *Batteryless=FdtGetProp(F,Found,"qcom,batteryless-platform",&L);
 if((Batteryless!=NULL)!=Candidate.Batteryless || (Batteryless&&L!=0))return EFI_UNSUPPORTED;
 CONST UINT32 *Ph=FdtGetProp(F,Found,"qcom,pmic-revid",&L);
 if(!Ph||L!=4)return EFI_UNSUPPORTED;
 INT32 Rev=FdtNodeOffsetByPhandle(F,Fdt32ToCpu(*Ph));UINT32 RevReg[]={0x100,0x100};
 if(Rev<0 || FdtParentOffset(F,Rev)!=Pmic || !String(F,Rev,"compatible","qcom,qpnp-revid")||
    !Cells(F,Rev,"reg",RevReg,2)||!Enabled(F,Rev))return EFI_UNSUPPORTED;
 INT32 TypeC=FdtSubnodeOffset(F,Found,"qcom,typec@1500");UINT32 TypeCReg[]={0x1500,0x100};
 if(!Enabled(F,TypeC)||!Cells(F,TypeC,"reg",TypeCReg,2))return EFI_UNSUPPORTED;
 UINT32 Irqs[32];for(UINT32 I=0;I<8;I++){Irqs[I*4]=2;Irqs[I*4+1]=0x15;Irqs[I*4+2]=I;Irqs[I*4+3]=(I==1||I==3||I==6)?3:1;}
 CONST CHAR8 IrqNames[]="typec-or-rid-detect-change\0typec-vpd-detect\0typec-cc-state-change\0typec-vconn-oc\0typec-vbus-change\0typec-attach-detach\0typec-legacy-cable-detect\0typec-try-snk-src-detect";
 if(!Cells(F,TypeC,"interrupts",Irqs,32)||!Bytes(F,TypeC,"interrupt-names",IrqNames,sizeof(IrqNames)))return EFI_UNSUPPORTED;
 for(INT32 Node=TypeC;Node!=Arb;Node=FdtParentOffset(F,Node)) {
  CONST UINT32 *Parent=FdtGetProp(F,Node,"interrupt-parent",&L);
  if((Parent&&(L!=4||FdtNodeOffsetByPhandle(F,Fdt32ToCpu(*Parent))!=Arb))||(!Parent&&L!=-FDT_ERR_NOTFOUND))return EFI_UNSUPPORTED;
 }
 INT32 Pd=FdtSubnodeOffset(F,Pmic,"qcom,usb-pdphy@1700");UINT32 PdReg[]={0x1700,0x100};
 if(!Enabled(F,Pd)||!String(F,Pd,"compatible","qcom,qpnp-pdphy")||!Cells(F,Pd,"reg",PdReg,2))return EFI_UNSUPPORTED;
 *Out=Candidate;return EFI_SUCCESS;
}
