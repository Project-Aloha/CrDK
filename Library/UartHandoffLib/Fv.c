/* Strict data-only source-FV runtime identity. SPDX-License-Identifier: MIT */
#include <Library/UartHandoffLib.h>
STATIC UINT16 Le16(CONST UINT8 *P){return (UINT16)(P[0]|((UINT16)P[1]<<8));}
STATIC UINT32 Le24(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16);}
STATIC UINT32 Le32(CONST UINT8 *P){return Le16(P)|((UINT32)Le16(P+2)<<16);}
STATIC UINT64 Le64(CONST UINT8 *P){return Le32(P)|((UINT64)Le32(P+4)<<32);}
STATIC BOOLEAN Same(CONST UINT8 *A,CONST UINT8 *B,UINTN N){
  for(UINTN I=0;I<N;++I)if(A[I]!=B[I])return FALSE;return TRUE;
}
STATIC BOOLEAN Erased(CONST UINT8 *P,UINTN N){for(UINTN I=0;I<N;++I)if(P[I]!=255)return FALSE;return TRUE;}
STATIC UINT32 Crc(CONST UINT8 *P,UINTN N){
  UINT32 C=~0U;for(UINTN I=0;I<N;++I){C^=P[I];
    for(UINT32 B=0;B<8;++B)C=(C>>1)^((0U-(C&1))&0xedb88320U);}return ~C;
}
STATIC CR_STATUS Pe(CONST UINT8 *P,UINTN N,UINT16 *Subsystem){
  if(N<64 || P[0]!='M' || P[1]!='Z')return CR_COMPROMISED_DATA;
  UINT32 Offset=Le32(P+60);
  if(Offset>N-24 || !Same(P+Offset,(CONST UINT8 *)"PE\0\0",4) ||
     Le16(P+Offset+4)!=0xaa64 || Le16(P+Offset+20)<70 ||
     Le16(P+Offset+20)>N-Offset-24 || Le16(P+Offset+24)!=0x20b)
    return CR_COMPROMISED_DATA;
  *Subsystem=Le16(P+Offset+24+68);
  return *Subsystem>=10 && *Subsystem<=12 ? CR_SUCCESS:CR_COMPROMISED_DATA;
}
CR_STATUS CrUartCoverageVerifyFv(CONST CR_UART_FIRMWARE_COVERAGE *C,
    CONST VOID *Fv,UINTN Bytes){
  if(!Fv || Bytes<56 || Bytes>32U*1024U*1024U ||
     (UINTN)Fv>~(UINTN)0-Bytes || CrUartCoverageValidate(C)!=CR_SUCCESS)
    return CR_INVALID_PARAMETER;
  CONST UINT8 *P=Fv;
  if(!Same(P+40,(CONST UINT8 *)"_FVH",4) || Le64(P+32)!=Bytes)return CR_COMPROMISED_DATA;
  UINT16 Header=Le16(P+48),Sum=0;
  if(Header<56 || (Header&1) || Header>Bytes)return CR_COMPROMISED_DATA;
  for(UINTN I=0;I<Header;I+=2)Sum=(UINT16)(Sum+Le16(P+I));
  if(Sum)return CR_COMPROMISED_DATA;
  UINT32 Seen=0,CodeCount=0;UINTN Offset=(Header+7U)&~7U;
  while(Offset+24<=Bytes){
    CONST UINT8 *File=P+Offset;
    if(Erased(File,24))return Erased(File,Bytes-Offset) && Seen==((1ULL<<C->Count)-1) ?
      CR_SUCCESS:CR_COMPROMISED_DATA;
    UINT32 Size=Le24(File+20);UINT8 Type=File[18];
    if(Size<24 || Size==0xffffff || (File[19]&1) || Size>Bytes-Offset)return CR_COMPROMISED_DATA;
    UINTN End=Offset+Size;
    if(Type>=3 && Type<=9){
      if(++CodeCount>1024)return CR_COMPROMISED_DATA;
      UINTN Position=Offset+24;UINT32 PeCount=0;
      while(Position<End){
        if(End-Position<4)return CR_COMPROMISED_DATA;
        UINT32 Section=Le24(P+Position);UINT8 Kind=P[Position+3];
        if(Section<4 || Section==0xffffff || Section>End-Position)return CR_COMPROMISED_DATA;
        if(Kind==1 || Kind==2 || Kind==0x17 || Kind==0x12)return CR_UNSUPPORTED;
        if(Kind==0x10){
          if(++PeCount>1)return CR_COMPROMISED_DATA;
          UINT16 Sub;CR_STATUS Status=Pe(P+Position+4,Section-4,&Sub);
          if(Status!=CR_SUCCESS)return Status;
          if(Sub==12){
            UINT32 Index=0;for(;Index<C->Count;++Index)if(Same(File,C->Images[Index].Guid,16))break;
            if(Index==C->Count || (Seen&(1U<<Index)))return CR_NOT_FOUND;
            if(C->Images[Index].PeBytes!=Section-4 ||
               C->Images[Index].PeChecksum!=Crc(P+Position+4,Section-4))return CR_COMPROMISED_DATA;
            Seen|=1U<<Index;
          }
        }
        Position=(Position+Section+3)&~(UINTN)3;
      }
      if(PeCount!=1)return CR_COMPROMISED_DATA;
    }
    Offset=(End+7)&~(UINTN)7;
  }
  return Offset<=Bytes && Seen==((1ULL<<C->Count)-1) ? CR_SUCCESS:CR_COMPROMISED_DATA;
}
