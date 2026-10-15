/* SPDX-License-Identifier: GPL-2.0-or-later
 * Compact-index address decoding adapted from U-Boot fs/erofs/zmap.c at
 * e37de002fac3895e8d0b60ae2015e17bb33e2b5b (GPL-2.0+).
 * Remaining bounds, inode, byte-reader and LZ4 decoder are original.
 */
#include "NhInternal.h"
NH_STATUS NhErofsOpen(NH_FS *f) {
 uint8_t s[128];NH_TRY(NhRead(&f->Io,1024,s,128));if(NhU32(s)!=0xe0f5e1e2)return NH_UNSUPPORTED;
 if(s[12]!=12||s[13]||(NhU32(s+80)&~1U)||NhU16(s+86))return NH_UNSUPPORTED;
 f->BlockSize=4096;f->Blocks=NhU32(s+36);if(!f->Blocks||f->Blocks>f->Io.Bytes/4096)return NH_CORRUPT;
 f->Io.Bytes=f->Blocks*4096;
 f->Root=NhU16(s+14);f->A=(uint64_t)NhU32(s+40)*4096;f->E=NhU32(s+80);memcpy(f->Uuid,s+48,16);
 if(f->A>=f->Blocks*4096)return NH_CORRUPT;
 if(NhU32(s+8)&1){uint8_t *b=f->Io.Alloc(f->Io.Context,3072);if(!b)return NH_MEMORY;NH_STATUS r=NhRead(&f->Io,1024,b,3072);if(r==NH_OK){uint32_t c=NhU32(b+4);memset(b+4,0,4);if(NhCrc32(~0U,b,3072,0x82f63b78U)!=c)r=NH_CORRUPT;}f->Io.Free(f->Io.Context,b);if(r!=NH_OK)return r;}
 return NH_OK;
}
NH_STATUS NhErofsInode(NH_FS *f,uint64_t id,NH_FILE *n) {
 if(id>(f->Blocks*4096-f->A)/32)return NH_CORRUPT;memset(n,0,sizeof(*n));n->Fs=f;n->Id=id;n->Address=f->A+id*32;
 NH_TRY(NhRead(&f->Io,n->Address,n->Raw,32));uint16_t format=NhU16(n->Raw);if(format&~15U)return NH_UNSUPPORTED;n->Layout=(format>>1)&7;if(n->Layout>3)return NH_UNSUPPORTED;
 uint32_t is=32;if(format&1){is=64;NH_TRY(NhRead(&f->Io,n->Address+32,n->Raw+32,32));n->Size=NhU64(n->Raw+8);}else n->Size=NhU32(n->Raw+8);
 n->Mode=NhU16(n->Raw+4);uint32_t xa=NhU16(n->Raw+2);n->Extra=is+(xa?12+(xa-1)*4:0);n->Flags=NhU32(n->Raw+16);
 if(n->Size>NH_MAX_FILE)return NH_TOO_LARGE;
 if(n->Layout==1||n->Layout==3){uint64_t base=(n->Address+n->Extra+7)&~7ULL;uint8_t h[8];NH_TRY(NhRead(&f->Io,base,h,8));
  if(n->Layout==3&&(NhU16(h+4)&~1U||h[6]||h[7]))return NH_UNSUPPORTED;
  n->Raw[72]=(n->Layout==3)?h[4]:0;
 }
 return NH_OK;
}
typedef struct {uint32_t Type, Offset, Back, Block;} ERO_INDEX;
static uint32_t bits(const uint8_t *p,unsigned bit,unsigned count){uint32_t n=0;for(unsigned i=0;i<count;i++)n|=((p[(bit+i)/8]>>((bit+i)%8))&1U)<<i;return n;}
static NH_STATUS ero_index(NH_FILE *n,uint32_t lcn,ERO_INDEX *x) {
 NH_FS *f=n->Fs;uint64_t count=(n->Size+4095)/4096,base=(n->Address+n->Extra+7)&~7ULL;if(lcn>=count)return NH_CORRUPT;memset(x,0,sizeof(*x));
 if(n->Layout==1){uint8_t e[8];NH_TRY(NhRead(&f->Io,base+16+(uint64_t)lcn*8,e,8));if(NhU16(e)&~3U)return NH_UNSUPPORTED;x->Type=NhU16(e)&3;x->Offset=NhU16(e+2);if(x->Type==2)x->Back=NhU16(e+4);else x->Block=NhU32(e+4);}
 else {
  base+=8;uint32_t initial=(uint32_t)((32-base%32)/4);if(initial==8)initial=0;uint32_t two=(n->Raw[72]&1)&&initial<count?(uint32_t)((count-initial)/16*16):0;
  unsigned shift;uint64_t pos;
  if(lcn<initial){shift=2;pos=base+(uint64_t)lcn*4;}
  else if(lcn-initial<two){shift=1;pos=base+(uint64_t)initial*4+(uint64_t)(lcn-initial)*2;}
  else {shift=2;pos=base+(uint64_t)initial*4+(uint64_t)two*2+(uint64_t)(lcn-initial-two)*4;}
  unsigned entries=shift==1?16:2,pack=entries<<shift,encode=(pack-4)*8/entries,i=(unsigned)(pos%pack)>>shift;uint8_t b[32];NH_TRY(NhRead(&f->Io,pos&~((uint64_t)pack-1),b,pack));
  uint32_t v=bits(b,i*encode,14),lo=v&4095;x->Type=v>>12;
  if(x->Type==2){if(i+1!=entries)x->Back=lo;else{v=bits(b,(i-1)*encode,14);x->Back=(v>>12)==2?(v&4095)+1:1;}}
  else {x->Offset=lo;unsigned blocks=1;int j=(int)i;while(j>0){j--;v=bits(b,(unsigned)j*encode,14);if((v>>12)==2)j-=(int)(v&4095);if(j>=0)blocks++;}uint32_t bno=NhU32(b+pack-4);if(bno>UINT32_MAX-blocks)return NH_CORRUPT;x->Block=bno+blocks;}
 }
 if(x->Type>2||(x->Type==2&&(!x->Back||(x->Back&0x800)))||(x->Type!=2&&x->Offset>=4096))return NH_CORRUPT;
 return NH_OK;
}
static NH_STATUS lz4(const uint8_t *in,size_t bytes,uint8_t *out,size_t length) {
 size_t ip=0,op=0;
 while(ip<bytes){uint8_t token=in[ip++];size_t lit=token>>4,match=token&15;
  if(lit==15){unsigned v;do{if(ip==bytes)return NH_CORRUPT;v=in[ip++];if(lit>SIZE_MAX-v)return NH_CORRUPT;lit+=v;}while(v==255);}
  if(lit>bytes-ip||lit>length-op)return NH_CORRUPT;memcpy(out+op,in+ip,lit);ip+=lit;op+=lit;if(ip==bytes)return op==length?NH_OK:NH_CORRUPT;
  if(bytes-ip<2)return NH_CORRUPT;size_t back=NhU16(in+ip);ip+=2;if(!back||back>op)return NH_CORRUPT;
  if(match==15){unsigned v;do{if(ip==bytes)return NH_CORRUPT;v=in[ip++];if(match>SIZE_MAX-v)return NH_CORRUPT;match+=v;}while(v==255);}
  match+=4;if(match>length-op)return NH_CORRUPT;for(size_t i=0;i<match;i++){out[op]=out[op-back];op++;}
 }return op==length?NH_OK:NH_CORRUPT;
}
NH_STATUS NhErofsRead(NH_FILE *n,uint64_t o,void *dest,size_t bytes) {
 NH_FS *f=n->Fs;uint8_t *out=dest;
 if(n->Layout==0||n->Layout==2){uint64_t full=(n->Size+4095)/4096;if(n->Layout==2&&full)full--;full*=4096;
  while(bytes){uint64_t at;size_t take;if(o<full){at=(uint64_t)n->Flags*4096+o;take=(size_t)NH_MIN((uint64_t)bytes,full-o);}else{at=n->Address+n->Extra+o%4096;take=bytes;if(at%4096+take>4096)return NH_CORRUPT;}
   NH_TRY(NhRead(&f->Io,at,out,take));out+=take;bytes-=take;o+=take;
  }return NH_OK;
 }
 while(bytes){uint32_t lcn=(uint32_t)(o/4096),head=lcn;ERO_INDEX x;NH_TRY(ero_index(n,head,&x));
  if(x.Type!=2&&o%4096<x.Offset){if(!head)return NH_CORRUPT;head--;NH_TRY(ero_index(n,head,&x));}
  while(x.Type==2){if(x.Back>head)return NH_CORRUPT;head-=x.Back;NH_TRY(ero_index(n,head,&x));}
  if(!x.Block||x.Block>=f->Blocks)return NH_CORRUPT;
  uint64_t start=(uint64_t)head*4096+x.Offset,end=n->Size;uint32_t max=(uint32_t)((n->Size+4095)/4096);
  for(uint32_t i=head+1;i<max;i++){ERO_INDEX next;NH_TRY(ero_index(n,i,&next));if(next.Type!=2){end=(uint64_t)i*4096+next.Offset;break;}}
  if(start>o||end<=o||end>n->Size||end-start>1048576)return NH_CORRUPT;size_t decoded=(size_t)(end-start),take=(size_t)NH_MIN((uint64_t)bytes,end-o);
  if(x.Type==0){if(decoded>4096)return NH_CORRUPT;NH_TRY(NhRead(&f->Io,(uint64_t)x.Block*4096+(o-start),out,take));}
  else {uint8_t *p=f->Io.Alloc(f->Io.Context,4096+decoded);if(!p)return NH_MEMORY;NH_STATUS s=NhRead(&f->Io,(uint64_t)x.Block*4096,p,4096);size_t skip=0;
   if(s==NH_OK){if(f->E&1)while(skip<4096&&!p[skip])skip++;if(skip==4096)s=NH_CORRUPT;else s=lz4(p+skip,4096-skip,p+4096,decoded);}
   if(s==NH_OK)memcpy(out,p+4096+(size_t)(o-start),take);f->Io.Free(f->Io.Context,p);if(s!=NH_OK)return s;
  }out+=take;bytes-=take;o+=take;
 }return NH_OK;
}
