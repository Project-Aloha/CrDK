/* SPDX-License-Identifier: MIT */
#include <Library/FirmwareLoaderLib/NhBundle.h>
#include "NhString.h"
void NhBundleFree(void *ctx,NH_FREE release,NH_BUNDLE *b){if(!b)return;for(unsigned i=0;i<NH_PARTS;i++)if(b->Parts[i].Data)release(ctx,b->Parts[i].Data);release(ctx,b);}
NH_STATUS NhBundleRead(void *ctx,NH_LOAD_FILE load,NH_ALLOC alloc,NH_FREE release,const char *name,uint32_t kind,NH_BUNDLE **out) {
 if(!load||!alloc||!release||!out||kind>1||NhPathCheck(name,0)!=NH_OK)return NH_INVALID;*out=NULL;
 NH_BUNDLE *b=alloc(ctx,sizeof(*b));if(!b)return NH_MEMORY;memset(b,0,sizeof(*b));b->Version=1;b->Kind=kind;
 NH_PART *header=&b->Parts[NH_HEADER_PART];header->Index=NH_HEADER_PART;NH_STATUS s=load(ctx,name,&header->Data,&header->Bytes);if(s!=NH_OK)goto fail;b->Count=1;
 if(header->Bytes>NH_MAX_FILE){s=NH_TOO_LARGE;goto fail;}if(kind==NH_BUNDLE_FILE){*out=b;return NH_OK;}
 uint8_t *p=header->Data;uint32_t size=header->Bytes;if(size<52||memcmp(p,"\177ELF",4)||(p[4]!=1&&p[4]!=2)||p[5]!=1||p[6]!=1||NhU32(p+20)!=1){s=NH_CORRUPT;goto fail;}
 int wide=p[4]==2;uint32_t eh=wide?64:52,ph=wide?56:32;if(size<eh||NhU16(p+(wide?52:40))!=eh||NhU16(p+(wide?54:42))!=ph){s=NH_CORRUPT;goto fail;}
 uint64_t offset=wide?NhU64(p+32):NhU32(p+28);uint32_t count=NhU16(p+(wide?56:44));if(offset<eh||offset>size||count<2||count>100||(uint64_t)count*ph>size-offset){s=NH_CORRUPT;goto fail;}
 int split=0;for(unsigned i=0;i<count;i++){uint8_t *e=p+(size_t)offset+i*ph;uint64_t off=wide?NhU64(e+8):NhU32(e+4),len=wide?NhU64(e+32):NhU32(e+16);if(len&&(off>size||len>size-off))split=1;}
 if(!split){*out=b;return NH_OK;}size_t stem=strlen(name);if(stem<5||name[stem-4]!='.'){s=NH_INVALID;goto fail;}stem-=4;char part[NH_MAX_PATH];memcpy(part,name,stem);memcpy(part+stem,".b00",5);
 uint64_t total=size;uint32_t hashes=0;for(unsigned i=0;i<count;i++){const uint8_t *e=p+(size_t)offset+i*ph;uint64_t len=wide?NhU64(e+32):NhU32(e+16),mem=wide?NhU64(e+40):NhU32(e+20),off=wide?NhU64(e+8):NhU32(e+4);uint32_t flags=NhU32(e+(wide?4:24));int hash=(flags&(7U<<24))==(2U<<24);
  if(hash)hashes++;if(!len)continue;if(!hash&&(NhU32(e)!=1||!mem))continue;if(len>NH_MAX_FILE||len>UINT32_MAX||(!hash&&len>mem)){s=NH_CORRUPT;goto fail;}
  /* Packed MDT stores the authentication hash after the ELF metadata; avoid
   * requiring optional b01 if the complete hash is already present. PIL is
   * still the authority on signatures, relocation and destination ranges. */
  const uint8_t *first=p+(size_t)offset;uint64_t headlen=wide?NhU64(first+32):NhU32(first+16);
  if(hash&&((off<=size&&len<=size-off)||(headlen<=size&&len==size-headlen)))continue;
  part[stem+2]=(char)('0'+i/10);part[stem+3]=(char)('0'+i%10);NH_PART *q=&b->Parts[i];q->Index=i;
  s=load(ctx,part,&q->Data,&q->Bytes);if(s==NH_NOT_FOUND)s=NH_CORRUPT;if(s!=NH_OK)goto fail;
  if(q->Bytes!=len||q->Bytes>NH_MAX_FILE-total){s=NH_CORRUPT;goto fail;}total+=q->Bytes;b->Count++;
 }
 if(hashes!=1){s=NH_CORRUPT;goto fail;}*out=b;return NH_OK;
fail:NhBundleFree(ctx,release,b);return s;
}
