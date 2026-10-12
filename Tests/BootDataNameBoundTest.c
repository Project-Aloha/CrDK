/* Actual private transport walk; runner counts property-name byte reads in
 * a copied TU, keeping production free of instrumentation. MIT. */
#include <Uefi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static UINT64 NameReads;
#include BOOT_DATA_PROBE_SOURCE
static void Emit(UINT8 *P, UINTN *At, UINT32 V) {
  P[*At]=V>>24;P[*At+1]=V>>16;P[*At+2]=V>>8;P[*At+3]=V;*At+=4;
}
int main(void) {
  const UINTN Count=1024, NameSize=65537;
  UINT8 *P=calloc(1,128*1024);UINTN At=56;UINT64 Original,Length;
  Emit(P,&At,1);At+=4;Emit(P,&At,1);memcpy(P+At,"chosen",7);At+=8;
  for(UINTN I=0;I<Count;++I) { Emit(P,&At,3);Emit(P,&At,0);Emit(P,&At,0); }
  Emit(P,&At,2);Emit(P,&At,2);Emit(P,&At,9);
  UINTN Strings=At, H=0;
  memset(P+Strings,'x',NameSize-1);P[Strings+NameSize-1]=0;
  Emit(P,&H,0xd00dfeed);Emit(P,&H,(UINT32)(Strings+NameSize));Emit(P,&H,56);
  Emit(P,&H,(UINT32)Strings);Emit(P,&H,40);Emit(P,&H,17);Emit(P,&H,16);
  Emit(P,&H,0);Emit(P,&H,(UINT32)NameSize);Emit(P,&H,(UINT32)Strings-56);
  EFI_STATUS S=InitrdTail(P,Strings+NameSize,&Original,&Length);
  printf("status=%llx properties=%llu shared-name-bytes=%llu property-name-byte-reads=%llu bound=%llu\n",
    (unsigned long long)S,(unsigned long long)Count,(unsigned long long)NameSize,
    (unsigned long long)NameReads,(unsigned long long)(Count*(sizeof("linux,initrd-start")+sizeof("linux,initrd-end"))));
  free(P);
  return S!=EFI_SUCCESS || NameReads>Count*(sizeof("linux,initrd-start")+sizeof("linux,initrd-end"));
}
