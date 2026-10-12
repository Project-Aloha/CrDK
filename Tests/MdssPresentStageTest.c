/* Independent raster oracle for actual portable Present staging. MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#include <Uefi.h>
#endif
#include <Library/MdssCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P, UINTN N, UINT8 V) { return memset(P,V,N); }
#endif

#define CAP 8192U
_Alignas(8) static UINT8 Primary[CAP], Saved[CAP], Source[CAP], Pixels[CAP], Before[CAP];
static UINT8 Expected[CAP], Scratch[CAP], PixelSaved[CAP], BeforeSaved[CAP];
static UINT32 Seed=0x58924017U;
static UINTN Cases,Rejected;
static UINT32 Random(UINT32 N) {
  Seed^=Seed<<13;Seed^=Seed>>17;Seed^=Seed<<5;return N ? Seed%N : 0;
}
static VOID NoPublish(VOID *C,VOID *P,UINTN N) {
  (void)C;(void)P;(void)N;assert(!"staging must never publish primary/cache writes");
}
static CR_MDSS Display(UINT32 W,UINT32 H,UINT32 Padding) {
  CR_MDSS D={0};D.Initialized=D.Active=TRUE;D.FrameBuffer=Primary;D.BufferBytes=CAP;
  D.Mode.Soc=CR_MDSS_SOC_SM8450;D.Mode.Width=W;D.Mode.Height=H;D.Mode.Stride=W*4+Padding;
  D.Mode.HSync=D.Mode.VSync=1;D.Mode.Lanes=4;D.Mode.RefreshQ16=60U<<16;
  D.Io.PublishCpuWrites=NoPublish;
  assert((UINTN)D.Mode.Stride*H<CAP);
  return D;
}
static VOID Initialize(VOID) {
  for(UINTN I=0;I<CAP;++I){Primary[I]=(UINT8)Random(256);Source[I]=(UINT8)Random(256);}
  memset(Pixels,0xC3,CAP);memset(Before,0x5A,CAP);memcpy(Saved,Primary,CAP);
}
static CR_MDSS_RECT Rect(UINT32 W,UINT32 H) {
  CR_MDSS_RECT R;
  R.Left=(INT32)Random(W+1);R.Top=(INT32)Random(H+1);
  R.Right=R.Left+(INT32)Random(W+1-(UINT32)R.Left);
  R.Bottom=R.Top+(INT32)Random(H+1-(UINT32)R.Top);return R;
}
/* Oracle gathers from an immutable source for every operation. It does not
   share the implementation's in-place reverse or directional move loops. */
static VOID Oracle(CONST CR_MDSS *D,CONST UINT8 *Src,UINT32 Pitch,
                   CONST CR_MDSS_MOVE *M,UINTN Mc,CONST CR_MDSS_RECT *R,UINTN Dc,
                   BOOLEAN Rotate) {
  UINT32 W=D->Mode.Width,H=D->Mode.Height,S=D->Mode.Stride;
  UINTN Size=(UINTN)S*H;
  memset(Expected,0xC3,CAP);memcpy(Expected,Saved,Size);
  if(Rotate)for(UINT32 Y=0;Y<H;++Y)for(UINT32 X=0;X<W;++X)
    memcpy(Expected+(UINTN)Y*S+X*4,Saved+(UINTN)(H-1-Y)*S+(W-1-X)*4,4);
  for(UINTN I=0;I<Mc;++I){
    memcpy(Scratch,Expected,Size);
    for(INT32 Y=M[I].Destination.Top;Y<M[I].Destination.Bottom;++Y)
      for(INT32 X=M[I].Destination.Left;X<M[I].Destination.Right;++X)
        memcpy(Expected+(UINTN)(UINT32)Y*S+(UINT32)X*4,
          Scratch+(UINTN)(UINT32)(M[I].SourceY+Y-M[I].Destination.Top)*S+
          (UINT32)(M[I].SourceX+X-M[I].Destination.Left)*4,4);
  }
  for(UINTN I=0;I<Dc;++I)for(INT32 Y=R[I].Top;Y<R[I].Bottom;++Y)
    for(INT32 X=R[I].Left;X<R[I].Right;++X)
      memcpy(Expected+(UINTN)(UINT32)Y*S+(UINT32)X*4,Src+(UINTN)(UINT32)Y*Pitch+(UINT32)X*4,4);
}
static VOID Positive(UINT32 W,UINT32 H,UINT32 Padding,UINT32 Old,UINT32 Next,UINT32 Kind) {
  Initialize();CR_MDSS D=Display(W,H,Padding),Original=D;
  UINT32 Pitch=W*4+20;UINTN Size=(UINTN)D.Mode.Stride*H;
  CR_MDSS_MOVE M[8];CR_MDSS_RECT R[6];UINTN Mc=(Kind&1)?8:0,Dc=(Kind&2)?6:0;
  for(UINTN I=0;I<Mc;++I){M[I].Destination=Rect(W,H);
    M[I].SourceX=(INT32)Random(W+1-(UINT32)(M[I].Destination.Right-M[I].Destination.Left));
    M[I].SourceY=(INT32)Random(H+1-(UINT32)(M[I].Destination.Bottom-M[I].Destination.Top));}
  for(UINTN I=0;I<Dc;++I)R[I]=Rect(W,H);
  /* Include full-screen dirty, then an overlapping smaller update. */
  if(Dc && Kind==3){R[0]=(CR_MDSS_RECT){0,0,(INT32)W,(INT32)H};}
  CR_MDSS_PRESENT_STAGE P={Pixels,Before,CAP,CAP,Old,Next},P0=P;
  Oracle(&D,Source,Pitch,M,Mc,R,Dc,Old!=Next);
  assert(CrMdssPresentStage(&D,Dc?Source:NULL,Dc?CAP:0,Pitch,M,Mc,R,Dc,&P)==CR_SUCCESS);
  assert(!memcmp(Pixels,Expected,CAP));assert(!memcmp(Before,Saved,Size));
  for(UINTN I=Size;I<CAP;++I)assert(Before[I]==0x5A);
  assert(!memcmp(Primary,Saved,CAP));assert(!memcmp(&D,&Original,sizeof(D)));assert(!memcmp(&P,&P0,sizeof(P)));
  ++Cases;
}
static VOID RejectedCall(CR_MDSS *D,CONST VOID *Src,UINTN N,UINT32 Pitch,
                        CONST CR_MDSS_MOVE *M,UINTN Mc,CONST CR_MDSS_RECT *R,UINTN Dc,
                        CR_MDSS_PRESENT_STAGE *P) {
  CR_MDSS Original=*D;CR_MDSS_PRESENT_STAGE P0=*P;
  memcpy(PixelSaved,Pixels,CAP);memcpy(BeforeSaved,Before,CAP);memcpy(Saved,Primary,CAP);
  assert(CrMdssPresentStage(D,Src,N,Pitch,M,Mc,R,Dc,P)!=CR_SUCCESS);
  assert(!memcmp(Pixels,PixelSaved,CAP)&&!memcmp(Before,BeforeSaved,CAP)&&!memcmp(Primary,Saved,CAP));
  assert(!memcmp(D,&Original,sizeof(*D))&&!memcmp(P,&P0,sizeof(*P)));++Rejected;
}
static VOID InvalidCases(VOID) {
  Initialize();CR_MDSS Base=Display(9,7,12);
  CR_MDSS_PRESENT_STAGE Stage={Pixels,Before,CAP,CAP,0,3};
  CR_MDSS_RECT R[2]={{0,0,9,7},{0,0,10,7}};
  CR_MDSS_MOVE M[2]={{{1,0,9,7},0,0},{{0,0,9,7},0,1}};
  RejectedCall(&Base,Source,CAP,56,NULL,0,R,2,&Stage);
  RejectedCall(&Base,Source,CAP,56,M,2,R,1,&Stage);
  RejectedCall(&Base,Source,10,56,NULL,0,R,1,&Stage);
  RejectedCall(&Base,Source,CAP,20,NULL,0,R,1,&Stage);
  RejectedCall(&Base,NULL,CAP,56,NULL,0,R,1,&Stage);
  for(UINT32 K=0;K<18;++K) {
    CR_MDSS D=Base;CR_MDSS_PRESENT_STAGE P=Stage;
    switch(K){
    case 0:P.PixelBytes=D.Mode.Stride*D.Mode.Height-1;break;
    case 1:P.BeforeBytes=D.Mode.Stride*D.Mode.Height-1;break;
    case 2:P.Pixels=NULL;break;
    case 3:P.Before=NULL;break;
    case 4:P.PixelBytes=(UINTN)-1;break;
    case 5:P.BeforeBytes=(UINTN)-1;break;
    case 6:P.PixelsTransform=1;break;
    case 7:P.NextPixelsTransform=2;break;
    case 8:P.Pixels=Primary;break;
    case 9:P.Before=Primary+4;break;
    case 10:P.Pixels=Before+4;break;
    case 11:P.Pixels=Source;break;
    case 12:P.Before=Source+4;break;
    case 13:D.Active=FALSE;break;
    case 14:D.CleanupPending=TRUE;break;
    case 15:D.Mode.Stride=4;break;
    case 16:D.BufferBytes=4;break;
    default:D.FrameBuffer=(VOID *)((UINTN)-1-4);break;
    }
    RejectedCall(&D,Source,CAP,56,NULL,0,R,1,&P);
  }
  RejectedCall(&Base,Source,CAP,56,NULL,CR_MDSS_MAX_RECTS+1,R,1,&Stage);
  RejectedCall(&Base,Source,CAP,56,NULL,0,NULL,CR_MDSS_MAX_RECTS+1,&Stage);
  RejectedCall(&Base,Source,CAP,56,(CR_MDSS_MOVE *)((UINTN)-1-1),1,R,1,&Stage);
  RejectedCall(&Base,Source,CAP,56,NULL,0,(CR_MDSS_RECT *)((UINTN)-1-1),1,&Stage);
  RejectedCall(&Base,(VOID *)((UINTN)-1-1),CAP,56,NULL,0,R,1,&Stage);
  CR_MDSS_PRESENT_STAGE Alias=Stage;
  Alias.Pixels=&Base;RejectedCall(&Base,Source,CAP,56,NULL,0,R,1,&Alias);
  Alias=Stage;Alias.Before=&Base;RejectedCall(&Base,Source,CAP,56,NULL,0,R,1,&Alias);
  Alias=Stage;Alias.Pixels=&Alias;RejectedCall(&Base,Source,CAP,56,NULL,0,R,1,&Alias);
  Alias=Stage;Alias.Before=&Alias;RejectedCall(&Base,Source,CAP,56,NULL,0,R,1,&Alias);
  memcpy(Pixels,M,sizeof(M[0]));
  RejectedCall(&Base,Source,CAP,56,(CR_MDSS_MOVE *)Pixels,1,R,1,&Stage);
  memcpy(Before,R,sizeof(R[0]));
  RejectedCall(&Base,Source,CAP,56,NULL,0,(CR_MDSS_RECT *)Before,1,&Stage);
  /* Exact adjacency is accepted; no blanket rejection of a shared allocation. */
  CR_MDSS_PRESENT_STAGE Adjacent={Pixels,Pixels+4096,4096,4096,0,0};
  assert(CrMdssPresentStage(&Base,NULL,0,0,NULL,0,NULL,0,&Adjacent)==CR_SUCCESS);
  assert(!memcmp(Pixels,Primary,Base.Mode.Stride*Base.Mode.Height));
  assert(!memcmp(Pixels+4096,Primary,Base.Mode.Stride*Base.Mode.Height));++Cases;
}
static VOID OrderedOverlap(VOID) {
  Initialize();CR_MDSS D=Display(9,7,12);UINTN Size=(UINTN)D.Mode.Stride*D.Mode.Height;
  /* Both vertical directions, both horizontal directions, then consume the
     previous move's result. Dirty overwrites part of that final move. */
  CR_MDSS_MOVE M[]={{{0,1,9,7},0,0},{{0,0,9,6},0,1},{{1,0,9,7},0,0},
                    {{0,0,8,7},1,0},{{2,2,7,5},0,0}};
  CR_MDSS_RECT R[]={{3,1,6,6},{3,3,3,3}};
  CR_MDSS_PRESENT_STAGE P={Pixels,Before,CAP,CAP,3,0};
  Oracle(&D,Source,56,M,5,R,2,TRUE);
  assert(CrMdssPresentStage(&D,Source,CAP,56,M,5,R,2,&P)==CR_SUCCESS);
  assert(!memcmp(Expected,Pixels,CAP)&&!memcmp(Before,Saved,Size)&&!memcmp(Primary,Saved,CAP));++Cases;
  /* Dirty source may be the read-only primary; it remains immutable while
     moves operate on staging and therefore still supplies original pixels. */
  memset(Pixels,0xC3,CAP);Oracle(&D,Primary,D.Mode.Stride,M,5,R,2,TRUE);
  assert(CrMdssPresentStage(&D,Primary,CAP,D.Mode.Stride,M,5,R,2,&P)==CR_SUCCESS);
  assert(!memcmp(Expected,Pixels,CAP)&&!memcmp(Primary,Saved,CAP));++Cases;
}
int main(void) {
  CONST UINT32 Widths[]={1,2,3,4,5,8,13,16,31},Heights[]={1,2,3,4,7,12,17};
  for(UINTN W=0;W<sizeof(Widths)/sizeof(Widths[0]);++W)
    for(UINTN H=0;H<sizeof(Heights)/sizeof(Heights[0]);++H)
      for(UINT32 Pad=0;Pad<2;++Pad)for(UINT32 Old=0;Old<=3;Old+=3)
        for(UINT32 Next=0;Next<=3;Next+=3)for(UINT32 Kind=0;Kind<4;++Kind)
          Positive(Widths[W],Heights[H],Pad*12,Old,Next,Kind);
  OrderedOverlap();InvalidCases();
  printf("Present staging PASS: %zu raster-oracle cases, %zu rejected preparations, primary/metadata/padding invariants\n",
         (size_t)Cases,(size_t)Rejected);return 0;
}
