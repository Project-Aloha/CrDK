/** Actual SMEM RAM decoder hostile input tests. SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/RamPartitionCrLib.h>
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI SetMem(VOID *D, UINTN N, UINT8 V) { return memset(D,V,N); }
#endif

static unsigned Checks;
static UINT8 Wire[CR_RAM_PARTITION_MAX_WIRE_SIZE + 16];
static CR_RAM_PARTITION_TABLE Out, Old;
static void Put32(UINT8 *P, UINT32 V) {
  for(unsigned I=0;I<4;++I)P[I]=(UINT8)(V>>(8*I));
}
static void Put64(UINT8 *P, UINT64 V) {
  for(unsigned I=0;I<8;++I)P[I]=(UINT8)(V>>(8*I));
}
static size_t H(unsigned V) {return V==0?20:24;}
static size_t E(unsigned V) {return V==0?56:(V==1?64:72);}
static UINT8 *Entry(unsigned V,unsigned I) {return Wire+H(V)+E(V)*I;}
static size_t Build(unsigned V,unsigned N) {
  memset(Wire,0,sizeof(Wire));Put32(Wire,0x9da5e0a8);Put32(Wire+4,0xaf9ec4e2);
  Put32(Wire+8,V);Put32(Wire+16,N);return H(V)+E(V)*N;
}
/* Hand-coded wire fixtures use independent offsets rather than parser structs. */
static void Bank(unsigned V,unsigned I,UINT64 B,UINT64 L,UINT64 A,unsigned D) {
  UINT8 *P=Entry(V,I);memset(P,'N',16);
  if(V==0){Put32(P+16,(UINT32)B);Put32(P+20,(UINT32)L);}
  else {Put64(P+16,B);Put64(P+24,L);}
  unsigned O=V==0?24:32;
  Put32(P+O,1);Put32(P+O+4,14);Put32(P+O+8,D);Put32(P+O+12,1);
  Put32(P+O+16,3);
  if(V){Put32(P+52,0x76543210);memset(P+56,0xac,8);}
  if(V>=2)Put64(P+64,A);
  if(V==3){P[56]=16;Put32(P+60,128);}
}
static void Expect(CONST VOID *P,size_t N,CR_STATUS Status) {
  memset(&Out,0xa5,sizeof(Out));memcpy(&Old,&Out,sizeof(Out));
  assert(CrRamPartitionParse(P,N,&Out)==Status);
  if(Status!=CR_SUCCESS)assert(!memcmp(&Out,&Old,sizeof(Out)));
  ++Checks;
}
static void Layouts(void) {
  for(unsigned V=0;V<=3;++V) {
    size_t N=Build(V,3);
    Bank(V,0,0xc0000000ULL,0x10000000,0x08000000,1);
    Bank(V,1,0x80000000ULL,0x40000000,0x3f000000,3);
    Bank(V,2,0x20000000ULL,0x10000000,0x10000000,2);
    Expect(Wire,N,CR_SUCCESS);
    assert(Out.Version==V&&Out.EntryCount==3&&Out.BankCount==2);
    assert(Out.Banks[0].Base==0x80000000ULL&&Out.Banks[1].Base==0xc0000000ULL);
    assert(Out.Banks[0].Length==0x40000000&&Out.Banks[0].Domain==3);
    assert(Out.Banks[0].AvailableLength==(V<2?0x40000000U:0x3f000000U));
    assert(Out.PhysicalBytes==0x50000000&&Out.AvailableBytes==(V<2?0x50000000U:0x47000000U));
    assert(!memchr(Out.Banks[0].Name,0,16));
    assert(Out.Banks[0].NumPartitions==3&&Out.Banks[0].HardwareInfo==(V?0x76543210U:0));
    assert(Out.Banks[0].HighestBankBit==(V==3?16U:0));
    assert(Out.Banks[0].MinPasrSize==(V==3?128U:0));
    Expect(Wire,N+8,CR_SUCCESS); /* padded SMEM allocation */
    for(size_t Short=1;Short<N;++Short)Expect(Wire,Short,CR_BAD_BUFFER_SIZE);
    UINT8 Unaligned[sizeof(Wire)+1];memcpy(Unaligned+1,Wire,N);
    Expect(Unaligned+1,N,CR_SUCCESS);
    N=Build(V,32);
    for(unsigned I=0;I<32;++I)Bank(V,I,0x80000000ULL+(31U-I)*0x100000,0x100000,0x100000,1);
    Expect(Wire,N,CR_SUCCESS);assert(Out.BankCount==32&&Out.PhysicalBytes==0x2000000);
    for(unsigned I=0;I<32;++I)assert(Out.Banks[I].Base==0x80000000ULL+I*0x100000);
  }
  size_t N=Build(3,2);Bank(3,0,0x100000000ULL,0x200000000ULL,0x180000000ULL,1);
  Bank(3,1,0x400000000ULL,0x100000000ULL,0,1);
  Expect(Wire,N,CR_SUCCESS);assert(Out.PhysicalBytes==0x300000000ULL&&Out.AvailableBytes==0x180000000ULL);
  assert(Out.Banks[1].AvailableLength==0); /* no invented fallback */
}
static void Hostile(void) {
  size_t N=Build(3,1);Bank(3,0,0x80000000,0x10000000,0x08000000,1);
  Expect(NULL,N,CR_INVALID_PARAMETER);Expect(Wire,0,CR_INVALID_PARAMETER);
  assert(CrRamPartitionParse(Wire,N,NULL)==CR_INVALID_PARAMETER);
  assert(CrRamPartitionParse(Wire,N,(CR_RAM_PARTITION_TABLE*)(Wire+8))==CR_INVALID_PARAMETER);
  assert(CrRamPartitionParse(Wire,N,(CR_RAM_PARTITION_TABLE*)((UINTN)-1-7))==CR_INVALID_PARAMETER);
  Expect((VOID*)((UINTN)-1-1),64,CR_INVALID_PARAMETER);
  UINT8 BadOut[sizeof(Out)+8];assert(CrRamPartitionParse(Wire,N,(VOID*)(BadOut+1))==CR_INVALID_PARAMETER);
  for(unsigned I=0;I<8;++I){Wire[I]^=1;Expect(Wire,N,CR_COMPROMISED_DATA);Wire[I]^=1;}
  Put32(Wire+8,4);Expect(Wire,N,CR_UNSUPPORTED);Put32(Wire+8,3);
  Put32(Wire+16,33);Expect(Wire,N,CR_COMPROMISED_DATA);
  Put32(Wire+16,0xffffffff);Expect(Wire,N,CR_COMPROMISED_DATA);
  Build(3,0);Expect(Wire,24,CR_NOT_FOUND);
  for(unsigned D=0;D<8;++D){N=Build(3,1);Bank(3,0,0x80000000,0x10000000,0,D);
    Expect(Wire,N,D>3?CR_COMPROMISED_DATA:(D==2?CR_NOT_FOUND:CR_SUCCESS));}
  /* XBL may leave SDRAM's domain unspecified. It must not erase usable
     prefixes or permit explicit modem-only entries into the inventory. */
  for(unsigned V=0;V<=3;++V){
    N=Build(V,2);Bank(V,0,0x80000000,0x10000000,0,0);
    Bank(V,1,0xc0000000,0x10000000,0x10000000,2);
    Expect(Wire,N,CR_SUCCESS);assert(Out.BankCount==1&&Out.Banks[0].Domain==0);
    assert(Out.Banks[0].AvailableLength==(V<2?0x10000000U:0));
    if(V>=2){Bank(V,0,0x80000000,0x10000000,0x08000000,0);
      Expect(Wire,N,CR_SUCCESS);assert(Out.AvailableBytes==0x08000000);}
  }
  N=Build(3,1);Bank(3,0,0x80000000,0x10000000,0x10000000,1);
  Put32(Entry(3,0)+36,5);Expect(Wire,N,CR_NOT_FOUND);Put32(Entry(3,0)+36,14);
  Put32(Entry(3,0)+44,2);Expect(Wire,N,CR_NOT_FOUND);Put32(Entry(3,0)+44,1);
  Put32(Entry(3,0)+32,0);Expect(Wire,N,CR_SUCCESS);assert(Out.Banks[0].Attribute==0);
  for(unsigned V=0;V<=3;++V){
    N=Build(V,1);Bank(V,0,0x80000001,0x10000000,0x10000000,1);Expect(Wire,N,CR_COMPROMISED_DATA);
    Bank(V,0,0x80000000,0,0,1);Expect(Wire,N,CR_NOT_FOUND);
    Bank(V,0,0x80000001,0,0,1);Expect(Wire,N,CR_COMPROMISED_DATA);
    if(V>=2){Bank(V,0,0x80000000,0,0x1000,1);Expect(Wire,N,CR_COMPROMISED_DATA);}
    Bank(V,0,0x80000000,0x10000001,0x10000000,1);Expect(Wire,N,CR_COMPROMISED_DATA);
    if(V>=2){Bank(V,0,0x80000000,0x10000000,0x10001000,1);Expect(Wire,N,CR_COMPROMISED_DATA);
      Bank(V,0,0x80000000,0x10000000,1,1);Expect(Wire,N,CR_COMPROMISED_DATA);}
    if(V){Bank(V,0,0xfffffffffffff000ULL,0x1000,0x1000,1);Expect(Wire,N,CR_COMPROMISED_DATA);}
    N=Build(V,2);Bank(V,0,0x80000000,0x20000000,0x10000000,1);
    Bank(V,1,0x90000000,0x10000000,0x10000000,1);Expect(Wire,N,CR_COMPROMISED_DATA);
    Bank(V,1,0x80000000,0x20000000,0x10000000,1);Expect(Wire,N,CR_COMPROMISED_DATA);
    Bank(V,1,0xa0000000,0x10000000,0x10000000,1);Expect(Wire,N,CR_SUCCESS);
    assert(Out.BankCount==2); /* adjacent physical banks must not merge */
  }
}
static void LahainaLoggedTable(void) {
  /* Independent reconstruction of all seven records printed by the board.
     The final empty record lies inside bank0's physical (unavailable) tail. */
  Build(3,7);
  Bank(3,0,0x80000000,0x40000000,0x37200000,0);
  Bank(3,1,0x200000000ULL,0x180000000ULL,0x180000000ULL,0);
  Bank(3,2,0x14800000,0x180000,0x180000,0);Put32(Entry(3,2)+36,4);
  Bank(3,3,0x14680000,0x40000,0x40000,0);Put32(Entry(3,3)+36,5);
  Bank(3,4,0x80700000,0xff000,0xff000,0);Put32(Entry(3,4)+44,2);
  Bank(3,5,0xc0000000,0x140000000ULL,0x140000000ULL,0);
  Bank(3,6,0xb9600000,0,0,0);
  Expect(Wire,0x918,CR_SUCCESS);
  assert(Out.EntryCount==7&&Out.BankCount==3);
  assert(Out.PhysicalBytes==0x300000000ULL&&Out.AvailableBytes==0x2f7200000ULL);
  assert(Out.Banks[0].Base==0x80000000&&Out.Banks[0].AvailableLength==0x37200000);
  assert(Out.Banks[1].Base==0xc0000000&&Out.Banks[2].Base==0x200000000ULL);
  Put64(Entry(3,6)+64,0x1000);Expect(Wire,0x918,CR_COMPROMISED_DATA);
  Put64(Entry(3,6)+64,0);Put32(Entry(3,6)+40,4);Expect(Wire,0x918,CR_COMPROMISED_DATA);
  Put32(Entry(3,6)+40,0);Put64(Entry(3,6)+24,0x1000);Expect(Wire,0x918,CR_COMPROMISED_DATA);
}
static void Mutations(void) {
  UINT8 Original[sizeof(Wire)];size_t N=Build(3,3);
  Bank(3,0,0x80000000,0x40000000,0x3f000000,1);
  Bank(3,1,0x100000000ULL,0x100000000ULL,0x80000000,3);
  Bank(3,2,0x300000000ULL,0x10000000,0,1);memcpy(Original,Wire,sizeof(Wire));
  /* Every wire bit, including untrusted count/range/masks, under sanitizers. */
  for(size_t I=0;I<N;++I)for(unsigned B=0;B<8;++B){
    memcpy(Wire,Original,sizeof(Wire));Wire[I]^=(UINT8)(1U<<B);
    memset(&Out,0xa5,sizeof(Out));memcpy(&Old,&Out,sizeof(Out));
    CR_STATUS S=CrRamPartitionParse(Wire,N,&Out);
    if(S!=CR_SUCCESS)assert(!memcmp(&Out,&Old,sizeof(Out)));
    else {assert(Out.BankCount<=32&&Out.BankCount>0);
      for(unsigned J=0;J<Out.BankCount;++J){CR_RAM_PARTITION_ENTRY *P=&Out.Banks[J];
        assert(P->Length&&P->AvailableLength<=P->Length&&P->Base<=CR_MAX_UINT64-P->Length);
        if(J)assert(Out.Banks[J-1].Base+Out.Banks[J-1].Length<=P->Base);
      }}++Checks;
  }
  /* Exact-sized allocations ensure truncation cannot read beyond the input. */
  for(size_t I=1;I<N;++I){UINT8 *P=malloc(I);assert(P);memcpy(P,Original,I);
    Expect(P,I,CR_BAD_BUFFER_SIZE);free(P);}
}
int main(void) {
  Layouts();Hostile();LahainaLoggedTable();Mutations();
  printf("RAM partition native/NT exact v0..3, available carveouts, sorted 32 banks, aliases/truncations/overlaps and %u adversarial checks PASS\n",Checks);
  return 0;
}
