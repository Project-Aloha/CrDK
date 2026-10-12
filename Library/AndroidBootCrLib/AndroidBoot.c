/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Library/AndroidBootCrLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/FdtLib.h>

/* BaseFdtLib already builds libfdt's complete validator. Its public EDK
   wrapper exposes only header checking, which is insufficient for untrusted
   property names and structure tokens. This is the actual libfdt C ABI. */
extern int fdt_check_full(CONST VOID *Fdt, UINTN Size);

#define MODE_MAX 256U
#define KEY_MAX 256U
#define DEPTH_MAX 16U
#define XBC_DATA_MAX 32767U

typedef struct {
  BOOLEAN Present;
  UINTN Length;
  CHAR8 Value[MODE_MAX];
} MODE_VALUE;

STATIC UINT32 Be32(CONST UINT8 *P)
{
  return ((UINT32)P[0]<<24)|((UINT32)P[1]<<16)|((UINT32)P[2]<<8)|P[3];
}
STATIC UINT32 Le32(CONST UINT8 *P)
{
  return ((UINT32)P[3]<<24)|((UINT32)P[2]<<16)|((UINT32)P[1]<<8)|P[0];
}
STATIC BOOLEAN Equal(CONST VOID *A, CONST VOID *B, UINTN N)
{
  return CompareMem(A,B,N)==0;
}
STATIC BOOLEAN Text(CONST CHAR8 *A, UINTN N, CONST CHAR8 *B, UINTN M)
{
  return N==M && Equal(A,B,N);
}
STATIC BOOLEAN Space(CHAR8 C)
{
  return C==' ' || C=='\t' || C=='\r' || C=='\n' || C=='\v' || C=='\f';
}
STATIC BOOLEAN Keyword(CHAR8 C)
{
  return (C>='a'&&C<='z') || (C>='A'&&C<='Z') ||
         (C>='0'&&C<='9') || C=='-' || C=='_';
}
STATIC BOOLEAN Overlap(CONST VOID *A, UINTN N, CONST VOID *B, UINTN M)
{
  UINTN X=(UINTN)A,Y=(UINTN)B;
  if(N>(~(UINTN)0)-X || M>(~(UINTN)0)-Y)return TRUE;
  return X<Y+M && Y<X+N;
}
STATIC CR_STATUS SetMode(MODE_VALUE *M, CONST CHAR8 *Value, UINTN Length)
{
  UINTN I;
  if(Length==0 || Length>=MODE_MAX)return CR_INVALID_PARAMETER;
  for(I=0;I<Length;++I)
    if((UINT8)Value[I]<0x20 || (UINT8)Value[I]>0x7e)return CR_INVALID_PARAMETER;
  if(M->Present)
    return Text(M->Value,M->Length,Value,Length)?CR_SUCCESS:CR_INVALID_PARAMETER;
  CopyMem(M->Value,Value,Length);
  M->Length=Length;M->Present=TRUE;
  return CR_SUCCESS;
}

/* Linux next_arg supports a quoted whole argument or a quoted value. Keep
   interior quotes literal; removing every quote could create a false key. */
STATIC CR_STATUS Cmdline(CONST CHAR8 *P, UINTN Size, MODE_VALUE *Mode)
{
  UINTN At=0;
  while(At<Size) {
    UINTN Start,End,Eq,Value,Length;
    BOOLEAN Quoted=FALSE,InQuote=FALSE;
    while(At<Size && Space(P[At]))++At;
    if(At==Size)break;
    if(P[At]=='"'){Quoted=InQuote=TRUE;++At;}
    Start=At;Eq=Size;
    while(At<Size && (!Space(P[At]) || InQuote)) {
      if((UINT8)P[At]<0x20 && !Space(P[At]))return CR_INVALID_PARAMETER;
      if(P[At]=='=' && Eq==Size)Eq=At;
      if(P[At]=='"')InQuote=!InQuote;
      ++At;
    }
    if(InQuote)return CR_INVALID_PARAMETER;
    End=At;
    if(Quoted && End>Start && P[End-1]=='"')--End;
    if(Eq>=End || !Text(P+Start,Eq-Start,"androidboot.mode",16))continue;
    Value=Eq+1;Length=End-Value;
    if(Length>0 && P[Value]=='"') {
      if(Length<2 || P[End-1]!='"')return CR_INVALID_PARAMETER;
      ++Value;Length-=2;
    }
    if(SetMode(Mode,P+Value,Length)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  }
  return CR_SUCCESS;
}

STATIC VOID Skip(CONST CHAR8 *P, UINTN N, UINTN *At, BOOLEAN Newlines)
{
  for(;;) {
    while(*At<N && Space(P[*At]) && (Newlines || P[*At]!='\n'))++*At;
    if(Newlines && *At<N && P[*At]=='#') {
      while(*At<N && P[*At]!='\n')++*At;
      continue;
    }
    break;
  }
}
STATIC BOOLEAN Delimiter(CHAR8 C)
{
  return C==',' || C==';' || C=='\n' || C=='#' || C=='}';
}
STATIC CR_STATUS Value(CONST CHAR8 *P, UINTN N, UINTN *At,
                       CONST CHAR8 **Out, UINTN *Length)
{
  UINTN Start,End;
  CHAR8 Quote;
  Skip(P,N,At,FALSE);Start=*At;
  Quote=Start<N && (P[Start]=='"'||P[Start]=='\'')?P[Start]:0;
  if(Quote) {
    ++*At;Start=*At;
    while(*At<N && P[*At]!=Quote) {
      if(((UINT8)P[*At]<0x20 && !Space(P[*At])) || (UINT8)P[*At]>0x7e)
        return CR_INVALID_PARAMETER;
      ++*At;
    }
    if(*At==N)return CR_INVALID_PARAMETER;
    End=*At;++*At;Skip(P,N,At,FALSE);
    if(*At<N && !Delimiter(P[*At]))return CR_INVALID_PARAMETER;
  } else {
    while(*At<N && !Delimiter(P[*At])) {
      if(((UINT8)P[*At]<0x20 && !Space(P[*At])) || (UINT8)P[*At]>0x7e)
        return CR_INVALID_PARAMETER;
      ++*At;
    }
    End=*At;
    while(End>Start && Space(P[End-1]))--End;
  }
  *Out=P+Start;*Length=End-Start;return CR_SUCCESS;
}

/* A bounded read-only subset of the documented bootconfig grammar. No tree
   allocation is needed to resolve one exact key. Operators on other keys and
   their arrays are consumed, but are never converted into Android mode. */
STATIC CR_STATUS Bootconfig(CONST CHAR8 *P, UINTN N, MODE_VALUE *Mode)
{
  CHAR8 Path[KEY_MAX];
  UINTN Saved[DEPTH_MAX+1],Words[DEPTH_MAX+1];
  UINTN At=0,Prefix=0,PrefixWords=0,Depth=0,I;
  while(N>0 && P[N-1]==0)--N;
  for(I=0;I<N;++I)if(P[I]==0)return CR_INVALID_PARAMETER;
  while(At<N) {
    UINTN Length=Prefix,KeyWords=PrefixWords,Count=0;
    BOOLEAN IsMode;
    CHAR8 Op;
    Skip(P,N,&At,TRUE);
    if(At==N)break;
    if(P[At]==';'){++At;continue;}
    if(P[At]=='}') {
      if(Depth==0)return CR_INVALID_PARAMETER;
      --Depth;Prefix=Saved[Depth];PrefixWords=Words[Depth];++At;continue;
    }
    if(Length>0)Path[Length++]='.';
    for(;;) {
      UINTN Start=At;
      while(At<N && Keyword(P[At])) {
        if(Length>=KEY_MAX-1)return CR_INVALID_PARAMETER;
        Path[Length++]=P[At++];
      }
      if(At==Start || ++KeyWords>DEPTH_MAX)return CR_INVALID_PARAMETER;
      Skip(P,N,&At,FALSE);
      if(At<N && P[At]=='.') {
        if(Length>=KEY_MAX-1)return CR_INVALID_PARAMETER;
        Path[Length++]='.';++At;Skip(P,N,&At,FALSE);continue;
      }
      break;
    }
    IsMode=Text(Path,Length,"androidboot.mode",16);
    if(At<N && P[At]=='{') {
      if(Depth==DEPTH_MAX || IsMode)return CR_INVALID_PARAMETER;
      Saved[Depth]=Prefix;Words[Depth]=PrefixWords;++Depth;
      Prefix=Length;PrefixWords=KeyWords;++At;continue;
    }
    if(At==N || P[At]==';'||P[At]=='\n'||P[At]=='#'||P[At]=='}') {
      if(IsMode)return CR_INVALID_PARAMETER;
      continue;
    }
    Op=P[At++];
    if(Op==':'||Op=='+') {
      if(At==N || P[At++]!='=' || IsMode)return CR_INVALID_PARAMETER;
    } else if(Op!='=')return CR_INVALID_PARAMETER;
    do {
      CONST CHAR8 *V;UINTN VL;
      if(Count>0)Skip(P,N,&At,TRUE);
      if(Value(P,N,&At,&V,&VL)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
      ++Count;
      if(IsMode && (Count!=1 || SetMode(Mode,V,VL)!=CR_SUCCESS))
        return CR_INVALID_PARAMETER;
      if(At==N || P[At]!=',')break;
      ++At;
      if(IsMode)return CR_INVALID_PARAMETER;
    } while(TRUE);
  }
  return Depth==0?CR_SUCCESS:CR_INVALID_PARAMETER;
}

STATIC CR_STATUS Initrd(UINT64 Start,UINT64 End,
    CONST VOID *(*MapRead)(VOID *,UINT64,UINTN),VOID *Context,
    CR_ANDROID_BOOT_INFO *Info,MODE_VALUE *Mode,BOOLEAN *Present)
{
  STATIC CONST CHAR8 Magic[]="#BOOTCONFIG\n";
  CONST UINT8 *Tail,*Data;
  UINTN Size,Padding,TailSize;
  UINT32 Sum=0,Checksum;UINTN I;
  /* ABL may publish a paired empty initrd. It is absence, not permission to
     inspect address zero or to subtract a trailer from an empty range. */
  if(End<Start)return CR_INVALID_PARAMETER;
  if(End==Start)return CR_SUCCESS;
  if(Start==0)return CR_INVALID_PARAMETER;
  if(End-Start<20)return CR_SUCCESS;
  TailSize=(UINTN)((End-Start<23)?End-Start:23);
  Tail=MapRead(Context,End-TailSize,TailSize);
  if(!Tail)return CR_NOT_READY;
  if(Overlap(Tail,TailSize,Info,sizeof(*Info)))return CR_INVALID_PARAMETER;
  /* Linux checks magic at the last twelve bytes and up to three bytes
     earlier, regardless of the value of bootloader alignment padding. */
  for(Padding=0;Padding<4;++Padding) {
    if(TailSize<20+Padding)break;
    if(Equal(Tail+TailSize-12-Padding,Magic,12))break;
  }
  if(Padding==4 || TailSize<20+Padding)return CR_SUCCESS;
  Size=Le32(Tail+TailSize-20-Padding);
  Checksum=Le32(Tail+TailSize-16-Padding);
  /* The enclosing inspection bound is 64KiB. Actual Linux XBC syntax caps
     text at 32767 bytes; never accept a charger request Linux cannot parse. */
  if(Size==0 || Size>CR_ANDROID_BOOT_BOOTCONFIG_MAX || Size>XBC_DATA_MAX ||
     Size>End-Start-20-Padding)return CR_INVALID_PARAMETER;
  Data=MapRead(Context,End-20-Padding-Size,Size);
  if(!Data)return CR_NOT_READY;
  if(Overlap(Data,Size,Info,sizeof(*Info)))return CR_INVALID_PARAMETER;
  for(I=0;I<Size;++I)Sum+=Data[I];
  if(Sum!=Checksum)return CR_INVALID_PARAMETER;
  if(Bootconfig((CONST CHAR8 *)Data,Size,Mode)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  *Present=TRUE;return CR_SUCCESS;
}

CR_STATUS CrAndroidBootInspect(UINT64 FdtAddress,
    CONST VOID *(*MapRead)(VOID *,UINT64,UINTN),VOID *Context,
    CR_ANDROID_BOOT_INFO *Info)
{
  UINT64 Header[5];
  CONST UINT8 *P,*Fdt,*Begin=NULL,*End=NULL;
  CONST CHAR8 *Args=NULL;
  UINTN Size,ArgsSize=0,BeginSize=0,EndSize=0;
  UINT32 Struct,Strings,Rsv,StructSize,StringsSize;
  INT32 Node,Chosen=-1,Property,Len,NameLen,Reserves;
  MODE_VALUE Command={0},Config={0};
  CR_ANDROID_BOOT_INFO Result={CR_ANDROID_BOOT_VERSION,0,0,0,0};
  BOOLEAN ConfigPresent=FALSE;
  CR_STATUS Status;
  if(!MapRead || !Info || ((UINTN)Info&7)!=0 || FdtAddress==0 ||
     (FdtAddress&7)!=0 || FdtAddress>(~(UINT64)0)-sizeof(Header))
    return CR_INVALID_PARAMETER;
  P=MapRead(Context,FdtAddress,sizeof(Header));
  if(!P)return CR_NOT_READY;
  if(Overlap(P,sizeof(Header),Info,sizeof(*Info)))return CR_INVALID_PARAMETER;
  CopyMem(Header,P,sizeof(Header));P=(CONST UINT8 *)Header;
  Size=Be32(P+4);
  if(Be32(P)!=0xd00dfeed || Size<40 || Size>CR_ANDROID_BOOT_FDT_MAX ||
     FdtAddress>(~(UINT64)0)-Size)return CR_INVALID_PARAMETER;
  Fdt=MapRead(Context,FdtAddress,Size);
  if(!Fdt)return CR_NOT_READY;
  if(((UINTN)Fdt&7)!=0 || Overlap(Fdt,Size,Info,sizeof(*Info)) ||
     !Equal(Header,Fdt,sizeof(Header)))return CR_INVALID_PARAMETER;
  Struct=Be32(Fdt+8);Strings=Be32(Fdt+12);Rsv=Be32(Fdt+16);
  StructSize=Be32(Fdt+36);StringsSize=Be32(Fdt+32);
  /* libfdt's reserve-map walker dereferences typed 64-bit entries. Reject
     malformed offsets before invoking that walker, even in its validator. */
  if((Struct&3)!=0 || (Rsv&7)!=0 || FdtCheckHeader(Fdt)!=0 ||
     Be32(Fdt+20)!=17 || fdt_check_full(Fdt,Size)!=0)return CR_INVALID_PARAMETER;
  Reserves=FdtGetNumberOfReserveMapEntries(Fdt);
  if(Reserves<0 || (Struct&3)!=0 || (Rsv&7)!=0 ||
     (UINTN)Reserves+1>(Size-Rsv)/16)return CR_INVALID_PARAMETER;
  /* Full libfdt bounds checking does not reject overlapping blocks. */
  if(Overlap(Fdt+Struct,StructSize,Fdt+Strings,StringsSize) ||
     Overlap(Fdt+Rsv,((UINTN)Reserves+1)*16,Fdt+Struct,StructSize) ||
     Overlap(Fdt+Rsv,((UINTN)Reserves+1)*16,Fdt+Strings,StringsSize))
    return CR_INVALID_PARAMETER;
  if(!FdtGetName(Fdt,0,&Len) || Len!=0)return CR_INVALID_PARAMETER;
  for(Node=FdtFirstSubnode(Fdt,0);Node>=0;Node=FdtNextSubnode(Fdt,Node)) {
    CONST CHAR8 *Name=FdtGetName(Fdt,Node,&NameLen);
    if(!Name)return CR_INVALID_PARAMETER;
    if(Text(Name,(UINTN)NameLen,"chosen",6)) {
      if(Chosen>=0)return CR_INVALID_PARAMETER;
      Chosen=Node;
    }
  }
  Result.FdtAddress=FdtAddress;Result.Flags=CR_ANDROID_BOOT_FDT_VALID;
  if(Chosen>=0) {
    for(Property=FdtFirstPropertyOffset(Fdt,Chosen);Property>=0;
        Property=FdtNextPropertyOffset(Fdt,Property)) {
      CONST FDT_PROPERTY *Prop=FdtGetPropertyByOffset(Fdt,Property,&Len);
      CONST CHAR8 *Name;
      if(!Prop || Len<0)return CR_INVALID_PARAMETER;
      Name=FdtGetString(Fdt,(INT32)Be32((CONST UINT8 *)&Prop->NameOffset),&NameLen);
      if(!Name)return CR_INVALID_PARAMETER;
      if(Text(Name,(UINTN)NameLen,"bootargs",8)) {
        if(Args)return CR_INVALID_PARAMETER;
        Args=(CONST CHAR8 *)Prop->Data;ArgsSize=(UINTN)Len;
      } else if(Text(Name,(UINTN)NameLen,"linux,initrd-start",18)) {
        if(Begin)return CR_INVALID_PARAMETER;
        Begin=(CONST UINT8 *)Prop->Data;BeginSize=(UINTN)Len;
      } else if(Text(Name,(UINTN)NameLen,"linux,initrd-end",16)) {
        if(End)return CR_INVALID_PARAMETER;
        End=(CONST UINT8 *)Prop->Data;EndSize=(UINTN)Len;
      }
    }
  }
  if(Args) {
    UINTN I;
    if(ArgsSize==0 || ArgsSize>CR_ANDROID_BOOT_BOOTCONFIG_MAX || Args[ArgsSize-1]!=0)
      return CR_INVALID_PARAMETER;
    for(I=0;I+1<ArgsSize;++I)if(Args[I]==0)return CR_INVALID_PARAMETER;
    if(Cmdline(Args,ArgsSize-1,&Command)!=CR_SUCCESS)return CR_INVALID_PARAMETER;
    Result.Flags|=CR_ANDROID_BOOT_CMDLINE_PRESENT;
  }
  if((Begin!=NULL)!=(End!=NULL))return CR_INVALID_PARAMETER;
  if(Begin) {
    UINT64 B,E;
    if((BeginSize!=4 && BeginSize!=8) || EndSize!=BeginSize)return CR_INVALID_PARAMETER;
    B=Be32(Begin);E=Be32(End);
    if(BeginSize==8){B=(B<<32)|Be32(Begin+4);E=(E<<32)|Be32(End+4);}
    Status=Initrd(B,E,MapRead,Context,Info,&Config,&ConfigPresent);
    if(Status!=CR_SUCCESS)return Status;
    if(ConfigPresent)Result.Flags|=CR_ANDROID_BOOT_BOOTCONFIG_PRESENT;
  }
  if(Config.Present) {
    Result.Source=CR_ANDROID_BOOT_SOURCE_BOOTCONFIG;
    if(Command.Present)Result.Flags|=CR_ANDROID_BOOT_BOOTCONFIG_OVERRIDES;
  } else if(Command.Present)Result.Source=CR_ANDROID_BOOT_SOURCE_CMDLINE;
  if(Result.Source!=0) {
    MODE_VALUE *M=Config.Present?&Config:&Command;
    Result.Mode=Text(M->Value,M->Length,"charger",7)?CR_ANDROID_BOOT_CHARGER:CR_ANDROID_BOOT_NORMAL;
  }
  CopyMem(Info,&Result,sizeof(Result));return CR_SUCCESS;
}
