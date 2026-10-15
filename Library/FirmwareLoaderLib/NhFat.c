/* SPDX-License-Identifier: MIT. FAT12/16/32, read-only ASCII firmware names. */
#include "NhInternal.h"
NH_STATUS NhFatOpen(NH_FS *f) {
 uint8_t b[512];NH_TRY(NhRead(&f->Io,0,b,512));uint32_t sector=NhU16(b+11),cluster=b[13],reserved=NhU16(b+14),fats=b[16],entries=NhU16(b+17),total=NhU16(b+19),fs=NhU16(b+22);
 if(NhU16(b+510)!=0xaa55||sector<512||sector>4096||(sector&(sector-1))||!cluster||(cluster&(cluster-1))||cluster>128||!reserved||!fats||fats>2)return NH_UNSUPPORTED;
 if(!total)total=NhU32(b+32);if(!fs)fs=NhU32(b+36);uint64_t first=(uint64_t)reserved+(uint64_t)fats*fs+(entries*32U+sector-1)/sector;
 if(!fs||first>=total)return NH_CORRUPT;uint64_t clusters=(total-first)/cluster;uint32_t bits=clusters<4085?12:clusters<65525?16:32;
 if(bits==32&&(entries||NhU16(b+42)||(NhU16(b+40)&0x80)))return NH_UNSUPPORTED;
 if(clusters+2>(uint64_t)fs*sector*8/bits)return NH_CORRUPT;
 if((uint64_t)total*sector<f->Io.Bytes)f->Io.Bytes=(uint64_t)total*sector;
 f->BlockSize=sector*cluster;f->Blocks=clusters+2;f->A=(uint64_t)reserved*sector;f->B=(uint64_t)fs*sector;f->C=first*sector;f->D=((uint64_t)reserved+(uint64_t)fats*fs)*sector;f->E=bits;f->F=entries*32;f->G=fats;f->H=sector;f->Root=bits==32?(NhU32(b+44)&0x0fffffff):0;
 if(bits==32&&f->Root<2)return NH_CORRUPT;
 /* Flash images may omit an unused zero tail. Every referenced FAT/directory/
  * data read is still bounded by the physical image length in NhRead. */
 if(f->C>f->Io.Bytes)return NH_CORRUPT;
 if(bits!=12){uint8_t state[4];NH_TRY(NhRead(&f->Io,f->A+bits/8,state,bits/8));uint32_t v=bits==16?NhU16(state):NhU32(state),mask=bits==16?0xc000U:0x0c000000U;if((v&mask)!=mask)return NH_NOT_READY;}return NH_OK;
}
static NH_STATUS next_cluster(NH_FS *f,uint32_t n,uint32_t *out) {
 uint8_t b[4],copy[4];if(n<2||n>=f->Blocks)return NH_CORRUPT;uint64_t off=f->E==12?n+n/2:(uint64_t)n*(f->E/8);size_t z=f->E==32?4:2;
 if(off+z>f->B)return NH_CORRUPT;NH_TRY(NhRead(&f->Io,f->A+off,b,z));if(f->G==2){NH_TRY(NhRead(&f->Io,f->A+f->B+off,copy,z));if(memcmp(b,copy,z))return NH_CORRUPT;}
 uint32_t v=f->E==32?(NhU32(b)&0x0fffffff):NhU16(b);if(f->E==12)v=n&1?v>>4:v&0xfff;
 uint32_t end=f->E==12?0xff8:f->E==16?0xfff8:0xffffff8;if(v>=end){*out=0;return NH_OK;}if(v<2||v>=f->Blocks||v==n)return NH_CORRUPT;*out=v;return NH_OK;
}
static NH_STATUS dir_size(NH_FS *f,uint32_t first,uint64_t *size) {
 if(!first&&f->E!=32){*size=f->F;return NH_OK;}uint32_t c=first,fast=first;uint64_t count=0;
 while(c){if(++count>f->Blocks||count>(NH_MAX_FILE/f->BlockSize))return NH_CORRUPT;NH_TRY(next_cluster(f,c,&c));
  for(unsigned step=0;fast&&step<2;step++)NH_TRY(next_cluster(f,fast,&fast));if(c&&c==fast)return NH_CORRUPT;
 }*size=count*f->BlockSize;return NH_OK;
}
NH_STATUS NhFatRead(NH_FILE *n,uint64_t o,void *p,size_t bytes) {
 NH_FS *f=n->Fs;uint8_t *b=p;if(!n->Id&&f->E!=32)return NhRead(&f->Io,f->D+o,b,bytes);
 uint32_t c=(uint32_t)n->Id;uint64_t skip=o/f->BlockSize;if(skip>f->Blocks)return NH_CORRUPT;
 for(uint64_t i=0;i<skip;i++){NH_TRY(next_cluster(f,c,&c));if(!c)return NH_CORRUPT;}
 while(bytes){if(c<2||c>=f->Blocks)return NH_CORRUPT;size_t offset=(size_t)(o%f->BlockSize),take=NH_MIN(bytes,f->BlockSize-offset);
  NH_TRY(NhRead(&f->Io,f->C+(uint64_t)(c-2)*f->BlockSize+offset,b,take));b+=take;o+=take;bytes-=take;
  if(bytes){NH_TRY(next_cluster(f,c,&c));if(!c)return NH_CORRUPT;}
 }return NH_OK;
}
static unsigned lower(unsigned c){return c>='A'&&c<='Z'?c+32:c;}
static int equal_name(const char *a,size_t n,const char *b,size_t m){if(n!=m)return 0;for(size_t i=0;i<n;i++)if(lower((unsigned char)a[i])!=lower((unsigned char)b[i]))return 0;return 1;}
NH_STATUS NhFatLookup(NH_FILE *dir,const char *name,size_t len,NH_FILE *out) {
 NH_FS *f=dir->Fs;uint8_t d[32];char longname[261]={0};unsigned seq=0,checksum=0,pending=0;uint64_t size;NH_TRY(dir_size(f,(uint32_t)dir->Id,&size));
 for(uint64_t o=0;o<size;o+=32) {
  NH_TRY(NhFatRead(dir,o,d,32));if(!d[0])return pending?NH_CORRUPT:NH_NOT_FOUND;if(d[0]==0xe5){seq=pending=0;longname[0]=0;continue;}
  if(d[11]==0x0f){unsigned index=d[0]&31;if(!index||index>20||(d[0]&0xa0)||d[12]||NhU16(d+26))return NH_CORRUPT;
   if(d[0]&0x40){if(pending)return NH_CORRUPT;pending=1;memset(longname,0,sizeof(longname));seq=index;checksum=d[13];}
   if(!pending||index!=seq||d[13]!=checksum)return NH_CORRUPT;static const uint8_t pos[13]={1,3,5,7,9,14,16,18,20,22,24,28,30};
   for(unsigned i=0;i<13;i++){uint16_t u=NhU16(d+pos[i]);if(u==0xffff)u=0;if(u>127)return NH_UNSUPPORTED;longname[(index-1)*13+i]=(char)u;}if(seq)seq--;continue;
  }
  if(d[11]&8){if(pending)return NH_CORRUPT;seq=0;longname[0]=0;continue;}unsigned sum=0;for(unsigned i=0;i<11;i++)sum=(((sum&1)?128:0)+(sum>>1)+d[i])&255;
  if(pending&&(seq||!longname[0]||sum!=checksum))return NH_CORRUPT;
  int match=0;if(pending){size_t z=0;while(z<260&&longname[z])z++;match=equal_name(name,len,longname,z);}
  char shortname[13];size_t z=0;for(unsigned i=0;i<8&&d[i]!=' ';i++)shortname[z++]=(char)d[i];if(d[8]!=' '){shortname[z++]='.';for(unsigned i=8;i<11&&d[i]!=' ';i++)shortname[z++]=(char)d[i];}
  if(equal_name(name,len,shortname,z))match=1;seq=pending=0;longname[0]=0;if(!match)continue;
  memset(out,0,sizeof(*out));out->Fs=f;out->Id=NhU16(d+26);if(f->E==32)out->Id|=(uint64_t)NhU16(d+20)<<16;out->Mode=(d[11]&16)?NH_DIR:NH_REG;out->Size=NhU32(d+28);
  if(out->Mode==NH_DIR){if(out->Id<2)return NH_CORRUPT;NH_TRY(dir_size(f,(uint32_t)out->Id,&out->Size));}else {if(out->Size>NH_MAX_FILE)return NH_TOO_LARGE;if(out->Size){if(out->Id<2)return NH_CORRUPT;uint64_t chain;NH_TRY(dir_size(f,(uint32_t)out->Id,&chain));if(chain!=(out->Size+f->BlockSize-1)/f->BlockSize*f->BlockSize)return NH_CORRUPT;}else if(out->Id)return NH_CORRUPT;}
  return NH_OK;
 }return pending?NH_CORRUPT:NH_NOT_FOUND;
}
