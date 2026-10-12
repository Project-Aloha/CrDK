/* SPDX-License-Identifier: MIT */
#include <Library/UfsPowerFdtCrLib.h>
#include <Library/FdtLib.h>

/* Exported by the same BaseFdtLib implementation as the public wrappers. */
extern int fdt_check_full(CONST VOID *Fdt, UINTN Size);
#define BAD CR_COMPROMISED_DATA
#define TRY(X) do { CR_STATUS S_=(X); if(S_!=CR_SUCCESS)return S_; } while(0)
#define PHANDLE_SLOTS 4096U
#define MAX_NODES 8192U
#define MAX_DEPTH 32
#define MAX_PROPERTIES 4096U
#define MAX_WORK 4000000U

static UINT32 Be32(CONST VOID *Data) {
  CONST UINT8 *P=Data;
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}
static BOOLEAN Same(CONST CHAR8 *A,CONST CHAR8 *B) {
  while(*A && *A==*B){++A;++B;}return *A==*B;
}
static VOID Copy(VOID *D,CONST VOID *S,UINTN N) {
  UINT8 *A=D;CONST UINT8 *B=S;while(N--)*A++=*B++;
}
static CONST VOID *Prop(CONST VOID *F,INT32 N,CONST CHAR8 *Name,INT32 *Size) {
  return FdtGetProp(F,N,Name,Size);
}
static CR_STATUS U32(CONST VOID *F,INT32 N,CONST CHAR8 *Name,UINT32 *V,BOOLEAN *Has) {
  INT32 L;CONST VOID *P=Prop(F,N,Name,&L);*Has=P!=NULL;
  if(!P)return L==-FDT_ERR_NOTFOUND?CR_SUCCESS:BAD;
  if(L!=4)return BAD;*V=Be32(P);return CR_SUCCESS;
}
static CR_STATUS Bool(CONST VOID *F,INT32 N,CONST CHAR8 *Name,UINT32 Bit,UINT32 *Flags) {
  INT32 L;CONST VOID *P=Prop(F,N,Name,&L);
  if(!P)return L==-FDT_ERR_NOTFOUND?CR_SUCCESS:BAD;
  if(L!=0)return BAD;*Flags|=Bit;return CR_SUCCESS;
}
static CR_STATUS String(CONST VOID *F,INT32 N,CONST CHAR8 *Name,CONST CHAR8 **V) {
  INT32 L;CONST CHAR8 *P=Prop(F,N,Name,&L);*V=NULL;
  if(!P)return L==-FDT_ERR_NOTFOUND?CR_SUCCESS:BAD;
  if(L<1 || P[L-1]!=0)return BAD;
  for(INT32 I=0;I<L-1;++I)if(P[I]==0)return BAD;
  *V=P;return CR_SUCCESS;
}
static BOOLEAN HasCompatible(CONST VOID *F,INT32 N,CONST CHAR8 *Want) {
  INT32 L;CONST CHAR8 *P=Prop(F,N,"compatible",&L);
  if(!P)return FALSE;
  for(INT32 I=0;I<L;){if(Same(P+I,Want))return TRUE;while(I<L && P[I])++I;++I;}
  return FALSE;
}
static CR_STATUS Enabled(CONST VOID *F,INT32 N,BOOLEAN *On) {
  *On=TRUE;
  while(N>=0){
    CONST CHAR8 *S;TRY(String(F,N,"status",&S));
    if(S && !Same(S,"okay") && !Same(S,"ok"))*On=FALSE;
    N=FdtParentOffset(F,N);
  }
  return CR_SUCCESS;
}
/* libfdt checks structure bounds, but permits duplicate properties/phandles.
 * Validate the namespace before any first-match lookup. A parent lookup in
 * libfdt starts two walks from the root; doing it for every node made the
 * 4291-node Android tree consume over 131 million tag visits before UART.
 * Retain predecessor links between siblings in preorder instead. Returning
 * from a subtree preserves its parent's chain; descending into a new parent
 * clears only the next depth's head. Property facts are reused per node.
 * Hashes are filters only: equal hashes always receive an exact comparison.
 */
typedef struct {
  INT32 Offset;
  UINT16 Previous;
  UINT16 Hash;
} NAMESPACE_NODE;

static CR_STATUS NameHash(CONST CHAR8 *Name,INT32 Length,UINT16 *Hash,UINT32 *Work) {
  UINT32 Value=2166136261U;
  for(INT32 I=0;I<Length;++I){
    if(++*Work>MAX_WORK)return CR_UNSUPPORTED;
    Value=(Value^(UINT8)Name[I])*16777619U;
  }
  *Hash=(UINT16)(Value^(Value>>16));return CR_SUCCESS;
}
static CR_STATUS NamesEqual(CONST CHAR8 *A,CONST CHAR8 *B,BOOLEAN *Equal,UINT32 *Work) {
  for(;;){
    if(++*Work>MAX_WORK)return CR_UNSUPPORTED;
    if(*A!=*B){*Equal=FALSE;return CR_SUCCESS;}
    if(!*A){*Equal=TRUE;return CR_SUCCESS;}
    ++A;++B;
  }
}
static CR_STATUS Namespace(CONST VOID *F) {
  UINT32 Handles[PHANDLE_SLOTS]={0},Count=0,Used=0,Work=0;
  NAMESPACE_NODE Nodes[MAX_NODES];
  UINT16 Heads[MAX_DEPTH+1]={0},PropertyHashes[MAX_PROPERTIES];
  UINT32 PropertyNames[MAX_PROPERTIES];
  /* Preserve the original depth convention: root is depth 1. */
  INT32 Depth=0,PreviousDepth=0,N=-1;
  while((N=FdtNextNode(F,N,&Depth))>=0 && Depth>=0){
    if(++Count>MAX_NODES || Depth>MAX_DEPTH)return CR_UNSUPPORTED;
    if(Depth>PreviousDepth){
      if(Depth!=PreviousDepth+1)return BAD;
      Heads[Depth]=0;
    }
    PreviousDepth=Depth;
    INT32 NameLen;CONST CHAR8 *Name=FdtGetName(F,N,&NameLen);
    if(!Name || (N==0?NameLen!=0:NameLen==0))return BAD;
    for(INT32 I=0;I<NameLen;++I)if(Name[I]=='/')return BAD;
    UINT16 Hash;TRY(NameHash(Name,NameLen,&Hash,&Work));
    for(UINT16 S=Heads[Depth];S;S=Nodes[S-1].Previous){
      if(++Work>MAX_WORK)return CR_UNSUPPORTED;
      if(Hash==Nodes[S-1].Hash){
        CONST CHAR8 *Other=FdtGetName(F,Nodes[S-1].Offset,NULL);
        BOOLEAN Equal;if(!Other)return BAD;
        TRY(NamesEqual(Name,Other,&Equal,&Work));if(Equal)return BAD;
      }
    }
    Nodes[Count-1]=(NAMESPACE_NODE){N,Heads[Depth],Hash};
    Heads[Depth]=(UINT16)Count;
    UINT32 Pc=0;
    for(INT32 P=FdtFirstPropertyOffset(F,N);P>=0;P=FdtNextPropertyOffset(F,P)){
      INT32 Len,SL;CONST FDT_PROPERTY *A=FdtGetPropertyByOffset(F,P,&Len);
      if(!A || Pc==MAX_PROPERTIES)return CR_UNSUPPORTED;
      UINT32 NameOffset=Be32(&A->NameOffset);
      CONST CHAR8 *An=FdtGetString(F,(INT32)NameOffset,&SL);
      if(!An || SL<1)return BAD;
      TRY(NameHash(An,SL,&Hash,&Work));
      for(UINT32 Q=0;Q<Pc;++Q){
        if(++Work>MAX_WORK)return CR_UNSUPPORTED;
        if(Hash==PropertyHashes[Q]){
          CONST CHAR8 *Bn=FdtGetString(F,(INT32)PropertyNames[Q],&SL);
          BOOLEAN Equal;if(!Bn)return BAD;
          TRY(NamesEqual(An,Bn,&Equal,&Work));if(Equal)return BAD;
        }
      }
      PropertyNames[Pc]=NameOffset;PropertyHashes[Pc++]=Hash;
    }
    INT32 L;CONST CHAR8 *C=Prop(F,N,"compatible",&L);
    if(C){if(L<1 || C[L-1]!=0)return BAD;for(INT32 I=0;I<L;++I)if(C[I]==0 && (I==0 || C[I-1]==0))return BAD;}
    UINT32 A=0,B=0;BOOLEAN Ha,Hb;TRY(U32(F,N,"phandle",&A,&Ha));TRY(U32(F,N,"linux,phandle",&B,&Hb));
    if(Ha && Hb && A!=B)return BAD;
    if(Ha || Hb){
      UINT32 V=Ha?A:B;if(V==0 || V==0xffffffffU)return BAD;
      if(++Used>PHANDLE_SLOTS/2)return CR_UNSUPPORTED;
      UINT32 I=(V*2654435761U)&(PHANDLE_SLOTS-1);
      while(Handles[I]){
        if(++Work>MAX_WORK)return CR_UNSUPPORTED;
        if(Handles[I]==V)return BAD;I=(I+1)&(PHANDLE_SLOTS-1);
      }
      Handles[I]=V;
    }
  }
  return N==-FDT_ERR_NOTFOUND || (N>=0 && Depth<0)?CR_SUCCESS:BAD;
}
static CR_STATUS Link(CONST VOID *F,INT32 N,CONST CHAR8 *Name,INT32 *Provider,UINT32 *Phandle) {
  BOOLEAN Has;UINT32 V=0;TRY(U32(F,N,Name,&V,&Has));
  if(!Has){*Provider=-1;*Phandle=0;return CR_SUCCESS;}
  if(V==0 || V==0xffffffffU)return BAD;
  INT32 P=FdtNodeOffsetByPhandle(F,V);if(P<0)return BAD;
  BOOLEAN On;TRY(Enabled(F,P,&On));if(!On)return CR_UNSUPPORTED;
  *Provider=P;*Phandle=V;return CR_SUCCESS;
}
static CR_STATUS Cells(CONST VOID *F,INT32 N,CONST CHAR8 *Name,UINT32 *Value) {
  BOOLEAN Has;TRY(U32(F,N,Name,Value,&Has));
  if(!Has || *Value<1 || *Value>2)return CR_UNSUPPORTED;
  return CR_SUCCESS;
}
static UINT64 Number(CONST UINT8 *P,UINT32 N) {
  return N==1?Be32(P):((UINT64)Be32(P)<<32)|Be32(P+4);
}
static CR_STATUS Reg(CONST VOID *F,INT32 Node,UINT64 *Base,UINT64 *Size) {
  INT32 Bus=FdtParentOffset(F,Node),L;
  if(Bus<0)return CR_UNSUPPORTED;
  UINT32 Ac,Sc;TRY(Cells(F,Bus,"#address-cells",&Ac));TRY(Cells(F,Bus,"#size-cells",&Sc));
  CONST UINT8 *P=Prop(F,Node,"reg",&L);UINT32 Tuple=(Ac+Sc)*4;
  if(!P || L<0 || (UINT32)L<Tuple || (UINT32)L%Tuple)return BAD;
  /* The qualified host bindings place HCI first; all additional windows are
   * still checked for complete nonwrapping tuples. */
  for(UINT32 I=0;I<(UINT32)L;I+=Tuple){UINT64 A=Number(P+I,Ac),Z=Number(P+I+Ac*4,Sc);if(!Z || A>CR_MAX_UINT64-Z)return BAD;}
  *Base=Number(P,Ac);*Size=Number(P+Ac*4,Sc);
  while(Bus>0){
    INT32 Parent=FdtParentOffset(F,Bus);if(Parent<0)return BAD;
    UINT32 Pac;TRY(Cells(F,Parent,"#address-cells",&Pac));
    P=Prop(F,Bus,"ranges",&L);
    if(!P)return L==-FDT_ERR_NOTFOUND?CR_UNSUPPORTED:BAD;
    if(L){
      UINT32 Rt=(Ac+Pac+Sc)*4;if(L<0 || (UINT32)L%Rt)return BAD;
      BOOLEAN Found=FALSE;UINT64 Result=0;
      for(UINT32 I=0;I<(UINT32)L;I+=Rt){
        UINT64 Child=Number(P+I,Ac),Up=Number(P+I+Ac*4,Pac),Span=Number(P+I+(Ac+Pac)*4,Sc);
        if(!Span || Child>CR_MAX_UINT64-Span || Up>CR_MAX_UINT64-Span)return BAD;
        if(*Base>=Child && *Base-Child<Span && *Size<=Span-(*Base-Child)){
          if(Found)return BAD;Found=TRUE;Result=Up+(*Base-Child);
        }
      }
      if(!Found)return CR_UNSUPPORTED;*Base=Result;
    }
    if(Pac==1 && (*Base>0xffffffffULL || *Size-1>0xffffffffULL-*Base))return BAD;
    Bus=Parent;Ac=Pac;TRY(Cells(F,Bus,"#size-cells",&Sc));
  }
  return CR_SUCCESS;
}
static CR_STATUS Range(CONST VOID *F,INT32 N,CR_UFS_POWER_SUPPLY *S) {
  BOOLEAN A,B;TRY(U32(F,N,"regulator-min-microvolt",&S->MinUv,&A));
  TRY(U32(F,N,"regulator-max-microvolt",&S->MaxUv,&B));
  if(A!=B || (A && (!S->MinUv || S->MinUv>S->MaxUv)))return BAD;
  if(A)S->Flags|=CR_UFS_POWER_RANGE_PRESENT;
  TRY(U32(F,N,"qcom,init-voltage",&S->InitUv,&B));
  if(B){if(!A || S->InitUv<S->MinUv || S->InitUv>S->MaxUv)return BAD;S->Flags|=CR_UFS_POWER_INIT_PRESENT;}
  TRY(Bool(F,N,"regulator-boot-on",CR_UFS_POWER_BOOT_ON,&S->Flags));
  TRY(Bool(F,N,"regulator-always-on",CR_UFS_POWER_ALWAYS_ON,&S->Flags));
  return CR_SUCCESS;
}
static CR_STATUS Resource(CONST CHAR8 *Name,CHAR8 *Dest) {
  UINTN Len=0;while(Name[Len]){if(++Len>=CR_UFS_POWER_RESOURCE_SIZE)return CR_UNSUPPORTED;}
  /* CMD DB regulator names only, not caller-directed arbitrary resources. */
  BOOLEAN L=Len>=5 && Name[0]=='l' && Name[1]=='d' && Name[2]=='o';
  BOOLEAN S=Len>=5 && Name[0]=='s' && Name[1]=='m' && Name[2]=='p';
  if((!L && !S) || Name[3]<'a' || Name[3]>'h' || Name[4]<'1' || Name[4]>'9')return CR_UNSUPPORTED;
  UINT32 Number_=0;for(UINTN I=4;I<Len;++I){if(Name[I]<'0'||Name[I]>'9')return CR_UNSUPPORTED;Number_=Number_*10+(UINT32)(Name[I]-'0');if(Number_>99)return CR_UNSUPPORTED;}
  Copy(Dest,Name,Len+1);return CR_SUCCESS;
}
/* An always-on output is not necessarily powered if its upstream input has
 * an enable control. Prove the entire fixed vin chain; root battery/VPH nodes
 * can omit always-on when they have no switch, clock or upstream dependency. */
static CR_STATUS FixedChain(CONST VOID *F,INT32 Node) {
  INT32 Seen[MAX_DEPTH];UINT32 Count=0;
  while(Node>=0){
    if(Count==MAX_DEPTH)return CR_UNSUPPORTED;
    for(UINT32 I=0;I<Count;++I)if(Seen[I]==Node)return BAD;
    Seen[Count++]=Node;
    if(!HasCompatible(F,Node,"regulator-fixed"))return CR_UNSUPPORTED;
    static CONST CHAR8 *Controls[]={"gpio","gpios","enable-gpios","clocks",
      "power-domains","enable-active-high","gpio-open-drain","parent-supply"};
    for(UINT32 I=0;I<sizeof(Controls)/sizeof(Controls[0]);++I){
      INT32 L;if(Prop(F,Node,Controls[I],&L))return CR_UNSUPPORTED;
    }
    CR_UFS_POWER_SUPPLY S={0};TRY(Range(F,Node,&S));
    if(!(S.Flags&CR_UFS_POWER_RANGE_PRESENT) || S.MinUv!=S.MaxUv)return CR_UNSUPPORTED;
    UINT32 Handle;INT32 Next;TRY(Link(F,Node,"vin-supply",&Next,&Handle));Node=Next;
  }
  return CR_SUCCESS;
}
static CR_STATUS Supply(CONST VOID *F,INT32 N,UINT32 Role,CR_UFS_POWER_SUPPLY *S) {
  static CONST CHAR8 *Names[]={"vcc","vccq","vccq2","vdd-hba","vdda-phy","vdda-pll","qcom,vccq-parent"};
  CHAR8 Property[48];UINTN K=0;while(Names[Role][K]){Property[K]=Names[Role][K];++K;}
  Copy(Property+K,"-supply",8);INT32 P;TRY(Link(F,N,Property,&P,&S->Phandle));
  Copy(Property+K,"-fixed-regulator",17);TRY(Bool(F,N,Property,CR_UFS_POWER_CONSUMER_FIXED,&S->Flags));
  Copy(Property+K,"-always-on",11);TRY(Bool(F,N,Property,CR_UFS_POWER_CONSUMER_ALWAYS_ON,&S->Flags));
  Copy(Property+K,"-max-microamp",14);BOOLEAN Has;TRY(U32(F,N,Property,&S->MaxUa,&Has));
  if(Has)S->Flags|=CR_UFS_POWER_MAX_UA_PRESENT;
  if(Role==CrUfsPowerVcc){
    TRY(Bool(F,N,"vcc-supply-1p8",CR_UFS_POWER_VCC_1P8,&S->Flags));
    TRY(Bool(F,N,"vcc-low-voltage-sup",CR_UFS_POWER_VCC_LOW_VOLTAGE,&S->Flags));
    INT32 L;CONST UINT8 *V=Prop(F,N,"vcc-voltage-level",&L);
    if(V){if(L!=8)return BAD;S->ConsumerMinUv=Be32(V);S->ConsumerMaxUv=Be32(V+4);
      if(!S->ConsumerMinUv || S->ConsumerMinUv>S->ConsumerMaxUv)return BAD;
      S->Flags|=CR_UFS_POWER_CONSUMER_RANGE;
    }
  }
  if(P<0)return Role==CrUfsPowerVddaPhy || Role==CrUfsPowerVddaPll?CR_UNSUPPORTED:CR_SUCCESS;
  TRY(Range(F,P,S));
  if(HasCompatible(F,P,"qcom,gdsc")){
    if(Role!=CrUfsPowerVddHba || (S->Flags&(CR_UFS_POWER_RANGE_PRESENT|CR_UFS_POWER_INIT_PRESENT)))return CR_UNSUPPORTED;
    S->Kind=CrUfsPowerGdsc;return Reg(F,P,&S->ProviderBase,&S->ProviderSize);
  }
  if(HasCompatible(F,P,"regulator-fixed")){
    if(!(S->Flags&CR_UFS_POWER_ALWAYS_ON))return CR_UNSUPPORTED;
    TRY(FixedChain(F,P));
    S->Kind=CrUfsPowerFixedAlwaysOn;return CR_SUCCESS;
  }
  INT32 Parent=FdtParentOffset(F,P);if(Parent<0)return CR_UNSUPPORTED;
  CONST CHAR8 *Name;
  if(HasCompatible(F,Parent,"qcom,rpmh-vrm-regulator")){
    TRY(String(F,Parent,"qcom,resource-name",&Name));if(!Name)return BAD;TRY(Resource(Name,S->Resource));
    TRY(U32(F,P,"qcom,set",&S->VoteSet,&Has));if(!Has)return BAD;
    if(S->VoteSet!=1 && S->VoteSet!=3)return CR_UNSUPPORTED;
    S->Flags|=CR_UFS_POWER_VOTE_SET_PRESENT;
  }else{
    BOOLEAN P8150=HasCompatible(F,Parent,"qcom,pm8150-rpmh-regulators");
    BOOLEAN P8350=HasCompatible(F,Parent,"qcom,pm8350-rpmh-regulators");
    if(!P8150 && !P8350)return CR_UNSUPPORTED;
    CONST CHAR8 *Id;TRY(String(F,Parent,"qcom,pmic-id",&Id));
    if(!Id || Id[0]<'a' || Id[0]>'h' || Id[1])return BAD;
    Name=FdtGetName(F,P,NULL);UINT32 Number_=0;UINTN Start;
    CHAR8 R[16]={0};
    if(Name[0]=='l' && Name[1]=='d' && Name[2]=='o'){Copy(R,"ldo",3);Start=3;}
    else if(Name[0]=='s' && Name[1]=='m' && Name[2]=='p' && Name[3]=='s'){Copy(R,"smp",3);Start=4;}
    else return CR_UNSUPPORTED;
    UINTN I=Start;if(Name[I]<'1'||Name[I]>'9')return CR_UNSUPPORTED;
    while(Name[I]){if(Name[I]<'0'||Name[I]>'9'||I-Start>=2)return CR_UNSUPPORTED;Number_=Number_*10+(UINT32)(Name[I++]-'0');}
    if(Number_>(R[0]=='l'?(P8150?18U:10U):(P8150?10U:12U)))return CR_UNSUPPORTED;
    R[3]=Id[0];Copy(R+4,Name+Start,I-Start+1);TRY(Resource(R,S->Resource));
  }
  if(!(S->Flags&CR_UFS_POWER_RANGE_PRESENT))return CR_UNSUPPORTED;
  S->Kind=CrUfsPowerRpmh;return CR_SUCCESS;
}
static CR_STATUS KnownSupplies(CONST VOID *F,INT32 N,BOOLEAN Host) {
  static CONST CHAR8 *Names[]={"vcc-supply","vccq-supply","vccq2-supply","vdd-hba-supply",
    "qcom,vddp-ref-clk-supply","qcom,vccq-parent-supply","vdda-phy-supply","vdda-pll-supply"};
  for(INT32 P=FdtFirstPropertyOffset(F,N);P>=0;P=FdtNextPropertyOffset(F,P)){
    INT32 L;CONST FDT_PROPERTY *V=FdtGetPropertyByOffset(F,P,&L);
    CONST CHAR8 *Name=FdtGetString(F,(INT32)Be32(&V->NameOffset),&L);
    if(L<7 || !Same(Name+L-7,"-supply"))continue;
    BOOLEAN Found=FALSE;
    for(UINT32 I=Host?0U:6U;I<(Host?6U:8U);++I)if(Same(Name,Names[I]))Found=TRUE;
    if(!Found)return CR_UNSUPPORTED;
  }
  return CR_SUCCESS;
}
CR_STATUS CrUfsPowerFdtParse(CONST VOID *Fdt,UINTN FdtSize,CR_UFS_POWER_FDT *Out) {
  CR_UFS_POWER_FDT Result={0};UINTN A=(UINTN)Fdt,B=(UINTN)Out;
  if(!Fdt || !Out || (A&7) || (B&7) || FdtSize<40 || FdtSize>CR_UFS_POWER_FDT_MAX ||
     A>~(UINTN)0-FdtSize || B>~(UINTN)0-sizeof(*Out) ||
     (A<B+sizeof(*Out) && B<A+FdtSize))return CR_INVALID_PARAMETER;
  CONST UINT8 *Bytes=Fdt;UINT32 Total=Be32(Bytes+4);
  UINT32 Struct=Be32(Bytes+8),Strings=Be32(Bytes+12),Rsv=Be32(Bytes+16),Ss=Be32(Bytes+36),Ts=Be32(Bytes+32);
  /* libfdt's reserve entry accessor uses naturally aligned structure fields.
   * Reject header offsets before asking the full validator to dereference. */
  if(Be32(Bytes)!=0xd00dfeed || Total<40 || Total>FdtSize || Be32(Bytes+20)!=17 ||
     (Struct&3) || (Rsv&7) || Rsv<40 || Rsv>Total-16 || Struct<40 || Struct>Total ||
     Ss>Total-Struct || Strings<40 || Strings>Total || Ts>Total-Strings ||
     FdtCheckHeader(Fdt)!=0 || fdt_check_full(Fdt,Total)!=0)return BAD;
  INT32 Entries=FdtGetNumberOfReserveMapEntries(Fdt);if(Entries<0)return BAD;
  UINT64 Re=(UINT64)Rsv+((UINT64)Entries+1)*16;
  if(Struct<40 || Strings<40 || Rsv<40 || Re>Total ||
     ((UINT64)Struct+Ss>Strings && (UINT64)Strings+Ts>Struct) ||
     (Re>Struct && (UINT64)Struct+Ss>Rsv) || (Re>Strings && (UINT64)Strings+Ts>Rsv))return BAD;
  TRY(Namespace(Fdt));
  INT32 Host=-1,Depth=0,N=-1;
  while((N=FdtNextNode(Fdt,N,&Depth))>=0 && Depth>=0){
    if(!HasCompatible(Fdt,N,"qcom,ufshc"))continue;
    BOOLEAN On;TRY(Enabled(Fdt,N,&On));if(!On)continue;
    if(Host>=0)return BAD;Host=N;
  }
  if(Host<0)return CR_NOT_FOUND;
  Result.Version=CR_UFS_POWER_FDT_VERSION;Result.Size=sizeof(Result);
  TRY(Reg(Fdt,Host,&Result.HostBase,&Result.HostSize));
  CONST CHAR8 *Name;TRY(String(Fdt,Host,"phy-names",&Name));if(!Name || !Same(Name,"ufsphy"))return CR_UNSUPPORTED;
  INT32 Phy;UINT32 Handle;TRY(Link(Fdt,Host,"phys",&Phy,&Handle));if(Phy<0)return BAD;
  UINT32 C=1;BOOLEAN Has;TRY(U32(Fdt,Phy,"#phy-cells",&C,&Has));if(!Has || C!=0)return CR_UNSUPPORTED;
  if(!HasCompatible(Fdt,Phy,"qcom,ufs-phy-qmp-v4") && !HasCompatible(Fdt,Phy,"qcom,ufs-phy-qmp-v4-waipio") &&
     !HasCompatible(Fdt,Phy,"qcom,ufs-phy-qmp-v4-lahaina") &&
     !HasCompatible(Fdt,Phy,"qcom,sm8350-qmp-ufs-phy") &&
     !HasCompatible(Fdt,Phy,"qcom,sm8250-qmp-ufs-phy") && !HasCompatible(Fdt,Phy,"qcom,sm8450-qmp-ufs-phy"))return CR_UNSUPPORTED;
  TRY(Reg(Fdt,Phy,&Result.PhyBase,&Result.PhySize));
  if(Result.HostBase<Result.PhyBase+Result.PhySize && Result.PhyBase<Result.HostBase+Result.HostSize)return BAD;
  TRY(KnownSupplies(Fdt,Host,TRUE));TRY(KnownSupplies(Fdt,Phy,FALSE));
  for(UINT32 I=0;I<CR_UFS_POWER_SUPPLY_COUNT;++I)TRY(Supply(Fdt,I<4?Host:Phy,I,&Result.Supplies[I]));
  TRY(Supply(Fdt,Host,CR_UFS_POWER_SUPPLY_COUNT,&Result.VccqParent));
  /* Qualified Android ref-clock supply is exactly the VCCQ rail. It is not
   * another rail to vote. A conflicting override cannot be ignored. */
  INT32 Ref;UINT32 RefHandle;TRY(Link(Fdt,Host,"qcom,vddp-ref-clk-supply",&Ref,&RefHandle));
  if(Ref>=0 && RefHandle!=Result.Supplies[CrUfsPowerVccq].Phandle)return CR_UNSUPPORTED;
  /* The pinned Android binding only supplies the alias and load. No qualified
   * semantics exist for these voltage extensions, so do not silently ignore
   * a request that could conflict with the VCCQ vote. */
  static CONST CHAR8 *RefVoltage[]={"qcom,vddp-ref-clk-min-microvolt",
    "qcom,vddp-ref-clk-max-microvolt","qcom,vddp-ref-clk-voltage-level"};
  for(UINT32 I=0;I<sizeof(RefVoltage)/sizeof(RefVoltage[0]);++I){
    INT32 L;if(Prop(Fdt,Host,RefVoltage[I],&L))return CR_UNSUPPORTED;
  }
  Copy(Out,&Result,sizeof(Result));return CR_SUCCESS;
}
