/* Actual portable TSENS core. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/TsensCrLib.h>
static UINT32 Count,Fail,StatusRead,Ctrl[2],Version[2],Values[3];
static int Context;
static CR_STATUS Read(VOID *C,UINT32 Bank,CR_TSENS_WINDOW W,UINT32 Offset,UINT32 *V) {
 assert(C==&Context && Bank<2);if (++Count==Fail) return CR_TIMEOUT;
 if(W==CrTsensSrot) {*V=Offset==0?Version[Bank]:Ctrl[Bank];assert(Offset==0||Offset==4);}
 else {assert(Offset>=0xa0 && Offset<=0xdc && !(Offset&3));assert(StatusRead<3);*V=Values[StatusRead++];}
 return CR_SUCCESS;
}
static CR_TSENS_DESCRIPTOR D[2]={{0x8450,1,0,0xc263000,0xc222000,0x1000,0x1000,538,540},
 {0x8450,1,1,0xc265000,0xc223000,0x1000,0x1000,539,541}};
static VOID Reset(VOID) {Count=Fail=StatusRead=0;Ctrl[0]=Ctrl[1]=1|(1U<<21)|(0xffffU<<3);Version[0]=Version[1]=0x20030001;Values[0]=Values[1]=Values[2]=0;}
int main(void) {
 CR_TSENS T={0};CR_TSENS_IO Io={&Context,Read};CR_TSENS_SAMPLE Sample,Before;memset(&Before,0xa5,sizeof(Before));
 assert(CrTsensQualify(D,2)==CR_SUCCESS);CR_TSENS_DESCRIPTOR Bad[2];memcpy(Bad,D,sizeof(Bad));Bad[1].Bank=0;assert(CrTsensQualify(Bad,2)==CR_UNSUPPORTED);
 for(UINT32 I=1;I<=4;I++){Reset();Fail=I;assert(CrTsensInitialize(&T,&Io,D,2)==CR_TIMEOUT&&!T.Initialized);}
 Reset();Version[1]=0x10000000;assert(CrTsensInitialize(&T,&Io,D,2)==CR_UNSUPPORTED&&!T.Initialized);
 Reset();Ctrl[1]&=~(1U<<21);assert(CrTsensInitialize(&T,&Io,D,2)==CR_NOT_READY&&!T.Initialized);
 Reset();assert(CrTsensInitialize(&T,&Io,D,2)==CR_SUCCESS&&Count==4);
 for(UINT32 Code=0;Code<4096;++Code) {Reset();Values[0]=CR_TSENS_VALID|Code;Sample=Before;assert(CrTsensRead(&T,1,15,&Sample)==CR_SUCCESS);assert(Sample.MilliCelsius==(Code<2048?(INT32)Code:(INT32)Code-4096)*100&&Sample.ReadCount==1&&Sample.ValidSample&&!Sample.StableFallback&&Count==2);}
 Reset();Values[0]=31;Values[1]=CR_TSENS_VALID|0xf38;assert(CrTsensRead(&T,0,0,&Sample)==CR_SUCCESS&&Sample.MilliCelsius==-20000&&Sample.ReadCount==2);
 Reset();Values[0]=31|(1U<<17);Values[1]=31|(1U<<18);Values[2]=90;assert(CrTsensRead(&T,0,0,&Sample)==CR_SUCCESS&&Sample.MilliCelsius==3100&&Sample.ReadCount==3&&Sample.StableFallback&&!Sample.ValidSample);
 Reset();Values[0]=1;Values[1]=Values[2]=0xfff;assert(CrTsensRead(&T,0,0,&Sample)==CR_SUCCESS&&Sample.MilliCelsius==-100);
 Reset();Values[0]=1;Values[1]=2;Values[2]=3;Sample=Before;assert(CrTsensRead(&T,0,0,&Sample)==CR_NOT_READY&&!memcmp(&Sample,&Before,sizeof(Sample)));
 for(UINT32 I=1;I<=4;++I) {Reset();Fail=I;Values[0]=1;Values[1]=2;Values[2]=3;Sample=Before;assert(CrTsensRead(&T,0,0,&Sample)==CR_TIMEOUT&&!memcmp(&Sample,&Before,sizeof(Sample)));}
 Reset();Ctrl[0]&=~8U;Sample=Before;assert(CrTsensRead(&T,0,0,&Sample)==CR_NOT_READY&&Count==1&&!memcmp(&Sample,&Before,sizeof(Sample)));
 assert(CrTsensRead(&T,2,0,&Sample)==CR_INVALID_PARAMETER&&CrTsensRead(&T,0,16,&Sample)==CR_INVALID_PARAMETER);
 CrTsensClear(&T);assert(!T.Initialized&&CrTsensRead(&T,0,0,&Sample)==CR_NOT_READY);
 puts("TSENS exact descriptor/runtime format, all signed12-bit values, valid/stable retry and failed output preservation PASS");return 0;
}
