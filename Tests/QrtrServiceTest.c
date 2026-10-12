/** Independent Linux QRTR/QMI wire fixtures. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Library/DebugLib.h>
#include <ucsi/Qrtr.h>

static CR_QRTR Router;
static UINT8 Sent[256][1024];
static UINTN SentSize[256], SentCount;
static EFI_STATUS SendStatus;
VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst,Src,Size); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN Size) { return memset(Dst,0,Size); }
VOID *EFIAPI SetMem(VOID *Dst, UINTN Size, UINT8 Value) { return memset(Dst,Value,Size); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A,B,Size); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
static void W16(UINT8 *P, UINT16 V) { P[0]=(UINT8)V; P[1]=(UINT8)(V>>8); }
static void W32(UINT8 *P, UINT32 V) { for(UINTN I=0;I<4;++I) P[I]=(UINT8)(V>>(8*I)); }
static UINT16 R16(CONST UINT8 *P) { return P[0]|((UINT16)P[1]<<8); }
static UINT32 R32(CONST UINT8 *P) { return R16(P)|((UINT32)R16(P+2)<<16); }
static EFI_STATUS Send(VOID *Context, CONST VOID *Data, UINTN Length)
{
  assert(Context==&Router && Length<=1024 && SentCount<256);
  if(EFI_ERROR(SendStatus)) return SendStatus;
  memcpy(Sent[SentCount],Data,Length); SentSize[SentCount++]=Length;
  return EFI_SUCCESS;
}
static EFI_STATUS Packet(UINT8 Version, UINT32 Type, UINT32 Node, UINT32 Port,
                         UINT32 Dest, UINT32 Confirm, CONST UINT8 *Data, UINTN Length)
{
  UINT8 Wire[1100]={0};
  UINTN Header=Version==1?32:20, Size=Header+((Length+3)&~(UINTN)3);
  if(Version==1) {
    W32(Wire,1); W32(Wire+4,Type); W32(Wire+8,Node); W32(Wire+12,Port);
    W32(Wire+16,Confirm); W32(Wire+20,(UINT32)Length); W32(Wire+24,1); W32(Wire+28,Dest);
  } else {
    Wire[0]=3; Wire[1]=(UINT8)Type; Wire[2]=(UINT8)Confirm; Wire[3]=4;
    W32(Wire+4,(UINT32)Length); W16(Wire+8,(UINT16)Node); W16(Wire+10,(UINT16)Port);
    W16(Wire+12,1); W16(Wire+14,(UINT16)Dest);
    memset(Wire+16,0xa5,4); /* Optional header bytes must not become QMI data. */
  }
  memcpy(Wire+Header,Data,Length);
  return CrQrtrReceive(&Router,Wire,Size);
}
static void Ctrl(UINT32 Type, UINT32 Node, UINT32 A, UINT32 B, UINT32 C, UINT32 D)
{
  UINT8 Data[20];
  W32(Data,Type); W32(Data+4,A); W32(Data+8,B); W32(Data+12,C); W32(Data+16,D);
  assert(Packet(1,Type,Node,0xfffffffeU,0xfffffffeU,0,Data,20)==EFI_SUCCESS);
}
static UINTN Tlv(UINT8 *Data, UINTN At, UINT8 Type, CONST VOID *Value, UINT16 Length)
{
  Data[At]=Type; W16(Data+At+1,Length); memcpy(Data+At+3,Value,Length);
  return At+3+Length;
}
static void Qmi(UINT8 *Data, UINT8 Type, UINT16 Txn, UINT16 Msg, UINTN Size)
{ Data[0]=Type; W16(Data+1,Txn); W16(Data+3,Msg); W16(Data+5,(UINT16)(Size-7)); }
static CONST UINT8 *FindTlv(CONST UINT8 *Data, UINTN Size, UINT8 Type, UINT16 *Length)
{
  assert(Size>=7 && R16(Data+5)==Size-7);
  for(UINTN At=7;At<Size;) {
    assert(Size-At>=3); UINT16 Len=R16(Data+At+1); assert(Len<=Size-At-3);
    if(Data[At]==Type) { *Length=Len; return Data+At+3; }
    At+=3+Len;
  }
  *Length=0; return NULL;
}
static CONST UINT8 *Payload(UINTN Index, UINT32 Src, UINT32 Node, UINT32 Port)
{
  assert(Index<SentCount && R32(Sent[Index])==1 && R32(Sent[Index]+4)==1);
  assert(R32(Sent[Index]+8)==1 && R32(Sent[Index]+12)==Src);
  assert(R32(Sent[Index]+24)==Node && R32(Sent[Index]+28)==Port);
  return Sent[Index]+32;
}
static void Fixture(void)
{
  SentCount=0; SendStatus=EFI_SUCCESS;
  CrQrtrInitialize(&Router,Send,&Router); CrQrtrOnline(&Router,TRUE);
  assert(CrQrtrTick(&Router,0)==EFI_SUCCESS);
  assert(CrQrtrTick(&Router,1)==EFI_SUCCESS && SentCount==2);
  /* v1 header and HELLO/control payload encoded independently of production. */
  static CONST UINT8 Hello[52]={
    1,0,0,0, 2,0,0,0, 1,0,0,0, 0xfe,0xff,0xff,0xff,
    0,0,0,0, 20,0,0,0, 0xff,0xff,0xff,0xff, 0xfe,0xff,0xff,0xff,
    2,0,0,0
  };
  assert(SentSize[0]==52 && memcmp(Sent[0],Hello,52)==0);
  assert(SentSize[1]==52 && R32(Sent[1]+4)==4 && R32(Sent[1]+32)==4);
  assert(R32(Sent[1]+36)==0x40 && R32(Sent[1]+40)==0x101);
  assert(R32(Sent[1]+44)==1 && R32(Sent[1]+48)==0x4000);
}
static UINT16 Listen(UINT32 Port, UINT64 Now)
{
  Ctrl(4,2,0x42,0x4a01,2,Port);
  assert(Router.ServerFound && Router.PdKnown && !Router.PdReady);
  assert(CrQrtrTick(&Router,Now)==EFI_SUCCESS && Router.Pending);
  assert(CrQrtrTick(&Router,Now+1)==EFI_SUCCESS);
  CONST UINT8 *Data=Payload(SentCount-1,0x4001,2,Port);
  UINTN Size=R32(Sent[SentCount-1]+20); UINT16 Length;
  assert(Data[0]==0 && R16(Data+3)==0x20);
  CONST UINT8 *Enable=FindTlv(Data,Size,1,&Length); assert(Length==1 && Enable[0]==1);
  CONST UINT8 *Path=FindTlv(Data,Size,2,&Length);
  assert(Length==19 && memcmp(Path,"msm/adsp/charger_pd",19)==0);
  return R16(Data+1);
}
static void Response(UINT16 Txn, UINT32 Port, UINT32 State, UINT16 Result)
{
  UINT8 Data[32], Value[4]; UINTN Size=7;
  W16(Value,Result); W16(Value+2,Result?0x1f:0);
  Size=Tlv(Data,Size,2,Value,4); W32(Value,State); Size=Tlv(Data,Size,0x10,Value,4);
  Qmi(Data,2,Txn,0x20,Size);
  assert(Packet(1,1,2,Port,0x4001,0,Data,Size)==EFI_SUCCESS);
}
static void Indication(UINT32 State, UINT16 Id, CONST CHAR8 *Path)
{
  UINT8 Data[100], Value[4]; UINTN Size=7;
  W32(Value,State); Size=Tlv(Data,Size,1,Value,4);
  Size=Tlv(Data,Size,2,Path,(UINT16)strlen(Path));
  W16(Value,Id); Size=Tlv(Data,Size,3,Value,2); Qmi(Data,4,0,0x22,Size);
  assert(Packet(1,1,2,55,0x4001,0,Data,Size)==EFI_SUCCESS);
}
static void PdrRegressions(void)
{
  Fixture();
  Ctrl(5,2,0x42,0x4a01,0,0); Ctrl(3,0,0,0,0,0);
  assert(!Router.PdKnown && !Router.ServerFound); /* no bound notifier was deleted */
  Ctrl(4,2,0x42,1,2,55); assert(!Router.ServerFound); /* instance != 74 */
  UINT8 Announcement[20]; W32(Announcement,4); W32(Announcement+4,0x42);
  W32(Announcement+8,0x4a01); W32(Announcement+12,2); W32(Announcement+16,55);
  assert(Packet(3,4,5,0xfffe,0xfffe,0,Announcement,20)==EFI_SUCCESS);
  assert(Router.ServerFound && Router.ServerNode==2 && Router.ServerPort==55);
  UINT16 Txn=Listen(55,10); UINT32 Generation=Router.PdGeneration;
  Response(Txn+1,55,0x1fffffff,0); assert(Router.Pending && !Router.PdReady);
  Response(Txn,56,0x1fffffff,0); assert(Router.Pending && !Router.PdReady);
  Response(Txn,55,0x1fffffff,0);
  assert(Router.Registered && !Router.Pending && Router.PdReady && Router.PdGeneration==Generation+1);
  Ctrl(4,2,0x42,0x4a01,2,55); assert(Router.Registered && Router.PdReady);
  Indication(0xfffffff,0x1234,"msm/adsp/audio_pd"); assert(Router.PdReady && Router.QueueCount==0);
  Indication(0xfffffff,0x1234,"msm/adsp/charger_pd");
  assert(!Router.PdReady && Router.QueueCount==1 && Router.PdGeneration==Generation+2);
  assert(CrQrtrTick(&Router,20)==EFI_SUCCESS);
  CONST UINT8 *Ack=Payload(SentCount-1,0x4001,2,55); UINT16 Len;
  assert(Ack[0]==0 && R16(Ack+3)==0x23);
  CONST UINT8 *Path=FindTlv(Ack,R32(Sent[SentCount-1]+20),1,&Len);
  assert(Len==19 && memcmp(Path,"msm/adsp/charger_pd",19)==0);
  CONST UINT8 *Id=FindTlv(Ack,R32(Sent[SentCount-1]+20),2,&Len);
  assert(Len==2 && R16(Id)==0x1234);
  Indication(0x1fffffff,0x1235,"msm/adsp/charger_pd"); assert(Router.PdReady);
  Generation=Router.PdGeneration;
  CrQrtrOnline(&Router,FALSE);
  assert(Router.PdKnown && !Router.PdReady && Router.QueueCount==0 && Router.PdGeneration==Generation+1);
  CrQrtrOnline(&Router,TRUE);
  assert(Router.PdKnown && !Router.PdReady && Router.Transaction!=0);
  assert(CrQrtrTick(&Router,30)==EFI_SUCCESS && CrQrtrTick(&Router,31)==EFI_SUCCESS);
  UINT16 NewTxn=Listen(55,40); assert(NewTxn!=Txn);
  Response(Txn,55,0x1fffffff,0); assert(Router.Pending && !Router.PdReady);
  Response(NewTxn,55,0x1fffffff,0); assert(Router.PdReady);
  Indication(0x2fffffff,0x1236,"msm/adsp/charger_pd"); assert(!Router.PdReady);
  Ctrl(5,2,0x42,0x4a01,2,55);
  assert(!Router.ServerFound && !Router.Registered && !Router.PdReady && Router.QueueCount==0);
  NewTxn=Listen(55,50);
  Response(NewTxn,55,0x1fffffff,1);
  assert(!Router.Registered && !Router.Pending && !Router.PdReady && Router.LastError==EFI_DEVICE_ERROR);
  assert(CrQrtrTick(&Router,Router.RetryUs)==EFI_SUCCESS && Router.Pending);
  assert(CrQrtrTick(&Router,Router.DeadlineUs)==EFI_SUCCESS && !Router.Pending && Router.LastError==EFI_TIMEOUT);
  Response(NewTxn,55,0x1fffffff,0); assert(!Router.Registered && !Router.PdReady);
  Ctrl(3,2,0,0,0,0); assert(!Router.ServerFound && Router.PdKnown && !Router.PdReady);
}
static EFI_STATUS Query(UINT8 Version, CONST CHAR8 *Service, UINT32 Offset, UINT32 Confirm)
{
  UINT8 Data[100], Value[4]; UINTN Size=Tlv(Data,7,1,Service,(UINT16)strlen(Service));
  if(Offset!=0) { W32(Value,Offset); Size=Tlv(Data,Size,0x10,Value,4); }
  Qmi(Data,0,0x3210,0x21,Size);
  return Packet(Version,1,2,42,0x4000,Confirm,Data,Size);
}
static void MapperRegressions(void)
{
  static CONST CHAR8 *Names[]={"msm/adsp/audio_pd","msm/adsp/root_pd","msm/adsp/charger_pd",
    "msm/cdsp/root_pd","msm/modem/root_pd","msm/slpi/root_pd","msm/slpi/sensor_pd"};
  static CONST UINT32 Instances[]={74,74,74,76,180,90,90};
  Fixture();
  for(UINT32 Offset=0;Offset<=7;Offset+=Offset==0?2:5) {
    assert(Query(Offset==2?3:1,"tms/servreg",Offset,1)==EFI_SUCCESS);
    assert(CrQrtrTick(&Router,100)==EFI_SUCCESS);
    CONST UINT8 *Data=Payload(SentCount-2,0x4000,2,42); UINT16 Length;
    UINTN Size=R32(Sent[SentCount-2]+20);
    assert(Data[0]==2 && R16(Data+1)==0x3210 && R16(Data+3)==0x21);
    CONST UINT8 *Result=FindTlv(Data,Size,2,&Length); assert(Length==4 && R32(Result)==0);
    CONST UINT8 *Total=FindTlv(Data,Size,0x10,&Length); assert(Length==2 && R16(Total)==7);
    CONST UINT8 *Revision=FindTlv(Data,Size,0x11,&Length); assert(Length==2 && R16(Revision)==1);
    CONST UINT8 *List=FindTlv(Data,Size,0x12,&Length); assert(List!=NULL && List[0]==7-Offset);
    UINTN At=1;
    for(UINTN I=Offset;I<7;++I) {
      UINTN NameLength=strlen(Names[I]); assert(List[At++]==NameLength);
      assert(memcmp(List+At,Names[I],NameLength)==0); At+=NameLength;
      assert(R32(List+At)==Instances[I] && List[At+4]==0 && R32(List+At+5)==0); At+=9;
    }
    assert(At==Length);
    UINT8 *Resume=Sent[SentCount-1];
    assert(R32(Resume+4)==7 && R32(Resume+24)==2 && R32(Resume+28)==42);
    assert(R32(Resume+32)==7 && R32(Resume+36)==1 && R32(Resume+40)==0x4000);
  }
  assert(Query(1,"no/such_service",0,0)==EFI_SUCCESS && CrQrtrTick(&Router,101)==EFI_SUCCESS);
  UINT16 Length; CONST UINT8 *Data=Payload(SentCount-1,0x4000,2,42);
  UINTN Size=R32(Sent[SentCount-1]+20);
  assert(R16(FindTlv(Data,Size,0x10,&Length))==0 && FindTlv(Data,Size,0x12,&Length)==NULL);
  assert(Query(1,"tms/pdr_enabled",0,0)==EFI_SUCCESS && CrQrtrTick(&Router,102)==EFI_SUCCESS);
  Data=Payload(SentCount-1,0x4000,2,42); Size=R32(Sent[SentCount-1]+20);
  CONST UINT8 *List=FindTlv(Data,Size,0x12,&Length);
  assert(List[0]==1 && List[1]==16 && memcmp(List+2,"msm/adsp/root_pd",16)==0);
}
static void BackpressureAndBounds(void)
{
  Fixture(); SendStatus=EFI_NOT_READY;
  for(UINTN I=0;I<8;++I) assert(Query(1,"avs/audio",0,1)==EFI_SUCCESS);
  assert(Router.QueueCount==16);
  UINT32 Received=Router.Received;
  assert(Query(1,"avs/audio",0,1)==EFI_NOT_READY && Router.Received==Received && Router.DomainRequests==8);
  Ctrl(2,2,0,0,0,0); assert(!Router.Advertised);
  assert(CrQrtrTick(&Router,1)==EFI_NOT_READY && Router.QueueCount==16);
  SendStatus=EFI_SUCCESS;
  assert(CrQrtrTick(&Router,2)==EFI_SUCCESS && Router.Advertised && Router.QueueCount==13);
  assert(Query(1,"avs/audio",0,1)==EFI_SUCCESS && Router.DomainRequests==9);
  while(Router.QueueCount!=0) assert(CrQrtrTick(&Router,3)==EFI_SUCCESS);
  Fixture(); UINT16 Txn=Listen(55,10);
  Response(Txn,55,0x1fffffff,0);
  SendStatus=EFI_NOT_READY; Indication(0xfffffff,3,"msm/adsp/charger_pd");
  Ctrl(5,2,0x42,0x4a01,2,55); assert(Router.QueueCount==0);
  SendStatus=EFI_SUCCESS; Ctrl(4,2,0x42,0x4a01,2,55);
  assert(CrQrtrTick(&Router,20)==EFI_SUCCESS && Router.Pending && Router.QueueCount==1);
  SendStatus=EFI_NOT_READY;
  assert(CrQrtrTick(&Router,Router.DeadlineUs)==EFI_SUCCESS && !Router.Pending && Router.QueueCount==0);
  /* Every prefix of a valid packet is rejected without an out-of-bounds read. */
  UINT8 Valid[52]={0}; W32(Valid,1); W32(Valid+4,2); W32(Valid+8,2);
  W32(Valid+12,0xfffffffe); W32(Valid+20,20); W32(Valid+24,1);
  W32(Valid+28,0xfffffffe); W32(Valid+32,2);
  for(UINTN I=0;I<52;++I) assert(CrQrtrReceive(&Router,Valid,I)==EFI_SUCCESS);
  UINT32 Rejected=Router.Malformed;
  W32(Valid+20,0xffffffff); assert(CrQrtrReceive(&Router,Valid,52)==EFI_SUCCESS && Router.Malformed==Rejected+1);
  UINT8 BadQmi[12]={0}; Qmi(BadQmi,0,1,0x21,12); BadQmi[7]=1; W16(BadQmi+8,99);
  assert(Packet(1,1,2,42,0x4000,1,BadQmi,12)==EFI_SUCCESS && Router.Malformed==Rejected+2);
  SendStatus=EFI_SUCCESS; assert(CrQrtrTick(&Router,100)==EFI_SUCCESS);
  assert(R32(Sent[SentCount-1]+4)==7 && R32(Sent[SentCount-1]+28)==42);
  UINT8 Random[1024]; UINT32 Seed=0x45231;
  for(UINTN N=0;N<2000;++N) {
    for(UINTN I=0;I<sizeof(Random);++I) { Seed=Seed*1664525U+1013904223U; Random[I]=(UINT8)(Seed>>24); }
    Random[0]=(UINT8)((N&1)?1:3);
    assert(CrQrtrReceive(&Router,Random,N%sizeof(Random))==EFI_SUCCESS);
  }
  CrQrtrOnline(&Router,FALSE);
  assert(CrQrtrReceive(&Router,Valid,sizeof(Valid))==EFI_NOT_READY && CrQrtrTick(&Router,0)==EFI_NOT_READY);
}
int main(void)
{
  PdrRegressions(); MapperRegressions(); BackpressureAndBounds();
  puts("QRTR services: v1/v3 wire, SM8450 mapper, PDR correlation/restart, flow control, bounded backpressure and malformed packets passed");
  return 0;
}
