/* SPDX-License-Identifier: MIT. Read-only clean-checkpoint F2FS reader.
 * Linux f2fs_fs.h on-disk layout; no roll-forward, writes or fscrypt keys.
 */
#include "NhInternal.h"
typedef struct {uint8_t Checkpoint[4096],Journal[512];uint32_t NatOffset,NatBytes,NatBlocks,Features;} F2_STATE;
static NH_STATUS cp_read(NH_FS *f,uint64_t block,uint8_t *b) {
 NH_TRY(NhRead(&f->Io,block*4096,b,4096));uint32_t at=NhU32(b+164),count=NhU32(b+136);if(at<192||at>4092||count<2||count>512||block>f->Blocks-count)return NH_CORRUPT;
 if(NhCrc32(0xf2f52010U,b,at,0xedb88320U)!=NhU32(b+at))return NH_CORRUPT;
 uint8_t *tail=f->Io.Alloc(f->Io.Context,4096);if(!tail)return NH_MEMORY;NH_STATUS s=NhRead(&f->Io,(block+count-1)*4096,tail,4096);if(s==NH_OK&&memcmp(tail,b,4096))s=NH_CORRUPT;f->Io.Free(f->Io.Context,tail);return s;
}
NH_STATUS NhF2fsOpen(NH_FS *f) {
 uint8_t *tmp=f->Io.Alloc(f->Io.Context,8192);if(!tmp)return NH_MEMORY;NH_STATUS s=NhRead(&f->Io,1024,tmp,3072);if(s!=NH_OK)goto done;
 if(NhU32(tmp)!=0xf2f52010U||NhU32(tmp+16)!=12||NhU32(tmp+20)!=9){s=NH_UNSUPPORTED;goto done;}
 uint32_t features=NhU32(tmp+2180),payload=NhU32(tmp+1664),natsegs=NhU32(tmp+60);f->Blocks=NhU64(tmp+36);f->BlockSize=4096;f->A=NhU32(tmp+84);f->B=NhU32(tmp+92);f->C=NhU32(tmp+76);f->Root=NhU32(tmp+96);memcpy(f->Uuid,tmp+108,16);
 if(!f->Blocks||f->Blocks>f->Io.Bytes/4096||f->A>=f->B||f->B>=f->Blocks||natsegs<2||natsegs%2||payload>16||features&~0x7fffU){s=NH_UNSUPPORTED;goto done;}
 f->Io.Bytes=f->Blocks*4096;
 /* Multi-device filesystems require an independently identified device set. */
 if(tmp[2201]){s=NH_UNSUPPORTED;goto done;}
 if(features&0x800){uint32_t off=NhU32(tmp+32);if(off>3068||off<2184||NhCrc32(0xf2f52010U,tmp,off,0xedb88320U)!=NhU32(tmp+off)){s=NH_CORRUPT;goto done;}}
 s=NhRead(&f->Io,5120,tmp+4096,3072);if(s!=NH_OK)goto done;if(memcmp(tmp,tmp+4096,3072)){s=NH_CORRUPT;goto done;}
 s=cp_read(f,f->C,tmp);if(s!=NH_OK)goto done;s=cp_read(f,f->C+512,tmp+4096);if(s!=NH_OK)goto done;
 if((int64_t)(NhU64(tmp+4096)-NhU64(tmp))>0){memcpy(tmp,tmp+4096,4096);f->C+=512;}
 uint32_t flags=NhU32(tmp+132),start=NhU32(tmp+140),total=NhU32(tmp+136),natbytes=NhU32(tmp+160),offset=192;
 if(!(flags&1)||(flags&(2U|8U|16U|0x1000U|0x2000U|0x4000U))){s=NH_NOT_READY;goto done;}
 if(flags&0x400)offset+=4;else if(!payload)offset+=NhU32(tmp+156);
 if(offset>4092||natbytes>4092-offset||natbytes!=(natsegs/2)*64||start<1+payload||start>=total-1){s=NH_CORRUPT;goto done;}
 F2_STATE *state=f->Io.Alloc(f->Io.Context,sizeof(*state));if(!state){s=NH_MEMORY;goto done;}f->Private=state;memset(state,0,sizeof(*state));memcpy(state->Checkpoint,tmp,4096);state->NatOffset=offset;state->NatBytes=natbytes;state->NatBlocks=(natsegs/2)*512;state->Features=features;
 s=NhRead(&f->Io,(f->C+start)*4096+((flags&4)?0:3584),state->Journal,507);if(s!=NH_OK)goto done;if(NhU16(state->Journal)>38){s=NH_CORRUPT;goto done;}
 for(unsigned i=0;i<NhU16(state->Journal);i++)for(unsigned j=0;j<i;j++)if(NhU32(state->Journal+2+13*i)==NhU32(state->Journal+2+13*j)){s=NH_CORRUPT;goto done;}
 s=NH_OK;
done:f->Io.Free(f->Io.Context,tmp);return s;
}
static NH_STATUS node_address(NH_FS *f,uint32_t id,uint32_t owner,uint64_t *address) {
 F2_STATE *st=f->Private;uint8_t entry[9],footer[24];uint32_t idx=id/455;if(id<3||idx>=st->NatBlocks)return NH_CORRUPT;int journal=0;
 for(unsigned i=0;i<NhU16(st->Journal);i++)if(NhU32(st->Journal+2+13*i)==id){memcpy(entry,st->Journal+6+13*i,9);journal=1;break;}
 if(!journal){uint64_t block=f->A+(idx/512)*1024+idx%512;if(st->Checkpoint[st->NatOffset+idx/8]&(1U<<(7-idx%8)))block+=512;NH_TRY(NhRead(&f->Io,block*4096+(id%455)*9,entry,9));}
 uint64_t block=NhU32(entry+5);if(NhU32(entry+1)!=owner||block<f->B||block>=f->Blocks)return NH_CORRUPT;
 NH_TRY(NhRead(&f->Io,block*4096+4072,footer,24));if(NhU32(footer)!=id||NhU32(footer+4)!=owner)return NH_CORRUPT;*address=block*4096;return NH_OK;
}
NH_STATUS NhF2fsInode(NH_FS *f,uint64_t id,NH_FILE *n) {
 if(id>UINT32_MAX)return NH_CORRUPT;memset(n,0,sizeof(*n));n->Fs=f;n->Id=id;NH_TRY(node_address(f,(uint32_t)id,(uint32_t)id,&n->Address));NH_TRY(NhRead(&f->Io,n->Address,n->Raw,100));
 n->Mode=NhU16(n->Raw);n->Size=NhU64(n->Raw+16);n->Flags=NhU32(n->Raw+80);n->Layout=n->Raw[3];if(n->Size>NH_MAX_FILE)return NH_TOO_LARGE;
 if(n->Raw[2]&4||n->Flags&(4U|0x40000000U)||n->Layout&0x80)return NH_UNSUPPORTED;
 uint8_t extra[12];NH_TRY(NhRead(&f->Io,n->Address+360,extra,12));n->Extra=(n->Layout&0x20)?NhU16(extra):0;F2_STATE *st=f->Private;
 if(n->Extra%4||n->Extra>256)return NH_CORRUPT;uint32_t xa=(st->Features&0x40)?NhU16(extra+2):((n->Layout&5)?50:0);if(xa>200||n->Extra/4+xa+1>=923)return NH_CORRUPT;n->Raw[200]=(uint8_t)xa;
 if((n->Layout&6)&&n->Size>4*(923-n->Extra/4-xa-1))return NH_CORRUPT;
 if((st->Features&0x20)&&n->Extra>=12){uint8_t *page=f->Io.Alloc(f->Io.Context,4096);if(!page)return NH_MEMORY;NH_STATUS s=NhRead(&f->Io,n->Address,page,4096);if(s==NH_OK){uint32_t actual=NhU32(page+368);memset(page+368,0,4);uint32_t seed=NhCrc32(~0U,f->Uuid,16,0xedb88320U);seed=NhCrc32(seed,page+4076,4,0xedb88320U);seed=NhCrc32(seed,page+68,4,0xedb88320U);if(NhCrc32(seed,page,4096,0xedb88320U)!=actual)s=NH_CORRUPT;}f->Io.Free(f->Io.Context,page);if(s!=NH_OK)return s;}
 return NH_OK;
}
static NH_STATUS f2_map(NH_FILE *n,uint32_t logical,uint64_t *out) {
 NH_FS *f=n->Fs;uint32_t direct=923-n->Extra/4-n->Raw[200];uint8_t b[4];uint64_t at;uint32_t child;
 if(logical<direct)at=n->Address+360+n->Extra+(uint64_t)logical*4;
 else {logical-=direct;unsigned slot,depth;uint32_t idx[2];if(logical<2*1018){slot=logical/1018;depth=1;idx[0]=logical%1018;}
  else {logical-=2*1018;if(logical>=2*1018U*1018U)return NH_TOO_LARGE;slot=2+logical/(1018U*1018U);depth=2;idx[0]=(logical/1018)%1018;idx[1]=logical%1018;}
  at=n->Address+4052+slot*4;for(unsigned i=0;i<depth;i++){NH_TRY(NhRead(&f->Io,at,b,4));child=NhU32(b);if(!child){*out=0;return NH_OK;}NH_TRY(node_address(f,child,(uint32_t)n->Id,&at));at+=idx[i]*4;}
 }
 NH_TRY(NhRead(&f->Io,at,b,4));uint32_t block=NhU32(b);if(!block||block==UINT32_MAX){*out=0;return NH_OK;}if(block<f->B||block>=f->Blocks)return NH_CORRUPT;*out=(uint64_t)block*4096;return NH_OK;
}
NH_STATUS NhF2fsRead(NH_FILE *n,uint64_t o,void *dest,size_t bytes) {
 NH_FS *f=n->Fs;uint8_t *b=dest;if(n->Layout&6)return NhRead(&f->Io,n->Address+364+n->Extra+o,b,bytes);
 while(bytes){uint64_t at;NH_TRY(f2_map(n,(uint32_t)(o/4096),&at));size_t pos=(size_t)(o%4096),take=NH_MIN(bytes,4096-pos);if(at)NH_TRY(NhRead(&f->Io,at+pos,b,take));else memset(b,0,take);o+=take;b+=take;bytes-=take;}return NH_OK;
}
NH_STATUS NhF2fsLookup(NH_FILE *dir,const char *name,size_t len,NH_FILE *out) {
 NH_FS *f=dir->Fs;uint8_t *b=f->Io.Alloc(f->Io.Context,4096);if(!b)return NH_MEMORY;NH_STATUS s=NH_NOT_FOUND;uint64_t size=dir->Size;unsigned count=214,entry=30;
 if(dir->Layout&4){size=4*(923-dir->Extra/4-dir->Raw[200]-1);count=(unsigned)size*8/153;entry=(unsigned)size-count*19;}
 for(uint64_t at=0;at<size;at+=4096){size_t z=(size_t)NH_MIN(size-at,4096);if(z<entry+count*19){s=NH_CORRUPT;goto done;}s=NhF2fsRead(dir,at,b,z);if(s!=NH_OK)goto done;
  for(unsigned i=0;i<count;){if(!(b[i/8]&(1U<<(i%8)))){i++;continue;}uint8_t *d=b+entry+i*11;unsigned nm=NhU16(d+8),slots=(nm+7)/8;if(!nm||nm>255||slots>count-i){s=NH_CORRUPT;goto done;}
   for(unsigned j=0;j<slots;j++)if(!(b[(i+j)/8]&(1U<<((i+j)%8)))){s=NH_CORRUPT;goto done;}
   if(nm==len&&!memcmp(b+entry+count*11+i*8,name,len)){s=NhF2fsInode(f,NhU32(d+4),out);goto done;}i+=slots;
  }
 }s=NH_NOT_FOUND;
done:f->Io.Free(f->Io.Context,b);return s;
}
