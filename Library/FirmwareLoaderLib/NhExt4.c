/* SPDX-License-Identifier: MIT
 * ext2/3/4 read-only subset: direct/indirect blocks and extent trees. Reject
 * recovery, encryption, inline-data, bigalloc and unknown incompatible modes.
 */
#include "NhInternal.h"
NH_STATUS NhExtOpen(NH_FS *f) {
 uint8_t s[1024];NH_TRY(NhRead(&f->Io,1024,s,sizeof(s)));if(NhU16(s+56)!=0xef53)return NH_UNSUPPORTED;
 uint32_t log=NhU32(s+24),inc=NhU32(s+96),ro=NhU32(s+100),is=NhU16(s+88),desc=NhU16(s+254);
 if(log>5||(inc&~(2U|0x10U|0x40U|0x80U|0x200U|0x2000U))||(ro&0x200U))return NH_UNSUPPORTED;
 if(!(NhU16(s+58)&1U))return NH_NOT_READY;
 f->BlockSize=1024U<<log;f->Blocks=NhU32(s+4);if(inc&0x80)f->Blocks|=(uint64_t)NhU32(s+336)<<32;
 if(!f->Blocks||f->Blocks>f->Io.Bytes/f->BlockSize)return NH_CORRUPT;
 f->Io.Bytes=f->Blocks*f->BlockSize;
 if(!(inc&0x80))desc=32;if(desc<32||desc>64||desc%8||is<128||is>256||!NhU32(s+32)||!NhU32(s+40))return NH_UNSUPPORTED;
 f->A=(uint64_t)(NhU32(s+20)+1)*f->BlockSize;f->B=NhU32(s);f->C=NhU32(s+32);f->D=NhU32(s+40);f->E=is;f->F=desc;f->G=inc;f->H=ro;f->Root=2;memcpy(f->Uuid,s+104,16);f->Checksum=(inc&0x2000)?NhU32(s+624):NhCrc32(~0U,f->Uuid,16,0x82f63b78U);
 if(ro&0x400U){uint32_t crc=NhCrc32(~0U,s,1020,0x82f63b78U);if(crc!=NhU32(s+1020))return NH_CORRUPT;}
 return NH_OK;
}
static uint16_t crc16(uint16_t crc,const void *data,size_t bytes){const uint8_t *p=data;while(bytes--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(uint16_t)((crc>>1)^(0xa001U&(0U-(crc&1))));}return crc;}
static void put32(uint8_t b[4],uint32_t x){for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(x>>(8*i));}
NH_STATUS NhExtInode(NH_FS *f,uint64_t id,NH_FILE *out) {
 uint8_t d[64];if(!id||id>f->B)return NH_CORRUPT;uint64_t group=(id-1)/f->D,index=(id-1)%f->D;
 NH_TRY(NhRead(&f->Io,f->A+group*f->F,d,NH_MIN(f->F,64)));uint8_t group_le[4];put32(group_le,(uint32_t)group);uint16_t actual=NhU16(d+30);
 if(f->H&0x400){d[30]=d[31]=0;uint32_t sum=NhCrc32(f->Checksum,group_le,4,0x82f63b78U);sum=NhCrc32(sum,d,f->F,0x82f63b78U);if((uint16_t)sum!=actual)return NH_CORRUPT;}
 else if(f->H&0x10){uint16_t sum=crc16(0xffff,f->Uuid,16);sum=crc16(sum,group_le,4);sum=crc16(sum,d,30);if(f->F>32)sum=crc16(sum,d+32,f->F-32);if(sum!=actual)return NH_CORRUPT;}
 uint64_t table=NhU32(d+8);if(f->G&0x80)table|=(uint64_t)NhU32(d+40)<<32;
 if(!table||table>=f->Blocks)return NH_CORRUPT;memset(out,0,sizeof(*out));NH_TRY(NhRead(&f->Io,table*f->BlockSize+index*f->E,out->Raw,f->E));
 out->Fs=f;out->Id=id;out->Mode=NhU16(out->Raw);out->Size=NhU32(out->Raw+4)|((uint64_t)NhU32(out->Raw+108)<<32);out->Flags=NhU32(out->Raw+32);
 uint8_t ino_le[4];put32(ino_le,(uint32_t)id);out->Extra=NhCrc32(f->Checksum,ino_le,4,0x82f63b78U);out->Extra=NhCrc32(out->Extra,out->Raw+100,4,0x82f63b78U);
 if(f->H&0x400){uint32_t actual_inode=NhU16(out->Raw+124),mask=0xffffU;out->Raw[124]=out->Raw[125]=0;
  if(f->E>=132&&NhU16(out->Raw+128)>=4){actual_inode|=(uint32_t)NhU16(out->Raw+130)<<16;out->Raw[130]=out->Raw[131]=0;mask=~0U;}
  if((NhCrc32(out->Extra,out->Raw,f->E,0x82f63b78U)&mask)!=actual_inode)return NH_CORRUPT;
 }
 if(out->Flags&(0x800U|4U|0x10000000U))return NH_UNSUPPORTED;
 if(out->Size>NH_MAX_FILE)return NH_TOO_LARGE;return NH_OK;
}
static NH_STATUS ext_map(NH_FILE *n,uint32_t logical,uint64_t *physical) {
 NH_FS *f=n->Fs;*physical=0;
 if(n->Flags&0x00080000U) {
  uint8_t h[12],e[12];uint64_t at=0;uint32_t cap=60,expected=UINT32_MAX;
  for(unsigned level=0;level<6;level++) {
   if(at)NH_TRY(NhRead(&f->Io,at,h,12));else memcpy(h,n->Raw+40,12);
   uint32_t count=NhU16(h+2),max=NhU16(h+4),depth=NhU16(h+6);
   if(NhU16(h)!=0xf30a||depth>5||(expected!=UINT32_MAX&&depth!=expected)||!max||max>(cap-12)/12||count>max)return NH_CORRUPT;
   uint64_t selected=0;uint32_t last=0;int found=0;
   for(uint32_t i=0;i<count;i++) {
    if(at)NH_TRY(NhRead(&f->Io,at+12+(uint64_t)i*12,e,12));else memcpy(e,n->Raw+52+i*12,12);
    uint32_t start=NhU32(e);if(i&&start<=last)return NH_CORRUPT;last=start;
    if(depth){if(start<=logical){selected=NhU32(e+4)|((uint64_t)NhU16(e+8)<<32);found=1;}}
    else {uint32_t len=NhU16(e+4);int unwritten=len>32768;if(unwritten)len-=32768;
     uint64_t block=NhU32(e+8)|((uint64_t)NhU16(e+6)<<32);
     if(!len||!block||block>=f->Blocks||len>f->Blocks-block||start>UINT32_MAX-len)return NH_CORRUPT;
     if(logical>=start&&logical-start<len){if(found)return NH_CORRUPT;*physical=unwritten?0:block+logical-start;found=1;}
    }
   }
   if(!depth)return NH_OK;if(!found)return NH_OK;if(!selected||selected>=f->Blocks)return NH_CORRUPT;
   at=selected*f->BlockSize;cap=f->BlockSize;expected=depth-1;
   if(f->H&0x400){uint8_t *block=f->Io.Alloc(f->Io.Context,f->BlockSize);if(!block)return NH_MEMORY;NH_STATUS status=NhRead(&f->Io,at,block,f->BlockSize);
    if(status==NH_OK){size_t tail=12+(size_t)NhU16(block+4)*12;if(tail>f->BlockSize-4||NhCrc32(n->Extra,block,tail,0x82f63b78U)!=NhU32(block+tail))status=NH_CORRUPT;}f->Io.Free(f->Io.Context,block);if(status!=NH_OK)return status;}

  }return NH_CORRUPT;
 }
 uint32_t ppb=f->BlockSize/4,depth,index[3]={0};uint64_t block;
 if(logical<12){block=NhU32(n->Raw+40+logical*4);if(block>=f->Blocks)return NH_CORRUPT;*physical=block;return NH_OK;}
 uint64_t l=logical-12;
 if(l<ppb){depth=1;index[0]=(uint32_t)l;block=NhU32(n->Raw+88);}
 else if((l-=ppb)<(uint64_t)ppb*ppb){depth=2;index[0]=(uint32_t)(l/ppb);index[1]=(uint32_t)(l%ppb);block=NhU32(n->Raw+92);}
 else {l-=(uint64_t)ppb*ppb;if(l>=(uint64_t)ppb*ppb*ppb)return NH_CORRUPT;depth=3;index[0]=(uint32_t)(l/ppb/ppb);index[1]=(uint32_t)((l/ppb)%ppb);index[2]=(uint32_t)(l%ppb);block=NhU32(n->Raw+96);}
 for(unsigned i=0;i<depth;i++){uint8_t b[4];if(!block)return NH_OK;if(block>=f->Blocks)return NH_CORRUPT;NH_TRY(NhRead(&f->Io,block*f->BlockSize+(uint64_t)index[i]*4,b,4));block=NhU32(b);}
 if(block>=f->Blocks)return NH_CORRUPT;*physical=block;return NH_OK;
}
NH_STATUS NhExtRead(NH_FILE *n,uint64_t o,void *p,size_t bytes) {
 NH_FS *f=n->Fs;uint8_t *b=p;
 if((n->Mode&NH_MODE)==NH_LINK&&n->Size<=60&&!(n->Flags&0x80000U)){memcpy(p,n->Raw+40+(size_t)o,bytes);return NH_OK;}
 while(bytes){uint64_t block;uint32_t offset=(uint32_t)(o%f->BlockSize);size_t take=NH_MIN(bytes,f->BlockSize-offset);NH_TRY(ext_map(n,(uint32_t)(o/f->BlockSize),&block));
  if(block)NH_TRY(NhRead(&f->Io,block*f->BlockSize+offset,b,take));else memset(b,0,take);bytes-=take;b+=take;o+=take;
 }return NH_OK;
}

NH_STATUS NhExtCheckDirectory(NH_FILE *n,uint64_t offset,uint8_t *b,size_t bytes) {
 NH_FS *f=n->Fs;if(!(f->H&0x400))return NH_OK;if(bytes!=f->BlockSize||bytes<40)return NH_CORRUPT;
 /* HTree root/interior blocks use dx_tail; directory leaves use dirent_tail.
  * Linear lookup still verifies every internal block it passes. */
 size_t entries=0;
 if(n->Flags&0x1000){if(!offset){if(NhU16(b+4)!=12||b[6]!=1||b[8]!='.'||NhU16(b+16)!=bytes-12||b[18]!=2||memcmp(b+20,"..",2)||NhU32(b+24)||b[29]!=8||b[30]>2||b[31])return NH_CORRUPT;entries=32;}
  else if(!NhU32(b)&&NhU16(b+4)==bytes&&!b[6])entries=8;}
 if(entries){unsigned limit=NhU16(b+entries),count=NhU16(b+entries+2);if(!count||count>limit||limit!=(bytes-entries-8)/8)return NH_CORRUPT;
  size_t tail=entries+(size_t)limit*8;uint32_t actual=NhU32(b+tail+4);if(NhU32(b+tail))return NH_CORRUPT;uint8_t zeros[8]={0};uint32_t sum=NhCrc32(n->Extra,b,entries+count*8,0x82f63b78U);sum=NhCrc32(sum,zeros,8,0x82f63b78U);return sum==actual?NH_OK:NH_CORRUPT;}
 const uint8_t *tail=b+bytes-12;if(NhU32(tail)||NhU16(tail+4)!=12||tail[6]||tail[7]!=0xde)return NH_CORRUPT;
 return NhCrc32(n->Extra,b,bytes-12,0x82f63b78U)==NhU32(tail+8)?NH_OK:NH_CORRUPT;
}
