/* SPDX-License-Identifier: MIT */
#include "NhInternal.h"
uint16_t NhU16(const void *p) {const uint8_t *s=p;return (uint16_t)(s[0]|s[1]<<8);}
uint32_t NhU32(const void *p) {const uint8_t *s=p;return (uint32_t)s[0]|(uint32_t)s[1]<<8|(uint32_t)s[2]<<16|(uint32_t)s[3]<<24;}
uint64_t NhU64(const void *p) {const uint8_t *s=p;return NhU32(s)|((uint64_t)NhU32(s+4)<<32);}
NH_STATUS NhRead(const NH_READER *r,uint64_t o,void *b,size_t n) {
 if(!r||!r->Read||(!b&&n)||o>r->Bytes||n>r->Bytes-o)return NH_CORRUPT;
 return n?r->Read(r->Context,o,b,n):NH_OK;
}
NH_STATUS NhPathCheck(const char *p,int empty) {
 size_t i=0,start=0;if(!p)return NH_INVALID;
 for(;i<NH_MAX_PATH&&p[i];++i) {
  unsigned char c=(unsigned char)p[i];if(c<32||c>=127||c=='\\'||c==':')return NH_INVALID;
  if(c=='/') {if(i==start||(i-start==1&&p[start]=='.')||(i-start==2&&p[start]=='.'&&p[start+1]=='.'))return NH_INVALID;start=i+1;}
  else if(i-start>=255)return NH_INVALID;
 }
 if(i==NH_MAX_PATH||(!i&&!empty)||(i&&i==start)||(i-start==1&&p[start]=='.')||(i-start==2&&p[start]=='.'&&p[start+1]=='.'))return NH_INVALID;
 return NH_OK;
}
uint32_t NhCrc32(uint32_t crc,const void *data,size_t n,uint32_t poly) {
 const uint8_t *p=data;while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^(poly&(0U-(crc&1)));}return crc;
}
static uint32_t ror(uint32_t x,unsigned n){return (x>>n)|(x<<(32-n));}
static void sha_block(uint32_t h[8],const uint8_t p[64]) {
 static const uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
 uint32_t w[64],a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
 for(unsigned i=0;i<16;i++)w[i]=(uint32_t)p[i*4]<<24|(uint32_t)p[i*4+1]<<16|(uint32_t)p[i*4+2]<<8|p[i*4+3];
 for(unsigned i=16;i<64;i++){uint32_t x=w[i-15],y=w[i-2];w[i]=w[i-16]+(ror(x,7)^ror(x,18)^(x>>3))+w[i-7]+(ror(y,17)^ror(y,19)^(y>>10));}
 for(unsigned i=0;i<64;i++){uint32_t t=z+(ror(e,6)^ror(e,11)^ror(e,25))+((e&f)^((~e)&g))+k[i]+w[i];uint32_t u=(ror(a,2)^ror(a,13)^ror(a,22))+((a&b)^(a&c)^(b&c));z=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;}
 h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
}
void NhSha256(const void *data,size_t n,uint8_t out[32]) {
 uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 const uint8_t *p=data;uint64_t bits=(uint64_t)n*8;uint8_t tail[128]={0};
 while(n>=64){sha_block(h,p);p+=64;n-=64;}memcpy(tail,p,n);tail[n]=0x80;size_t end=n<56?64:128;for(unsigned i=0;i<8;i++)tail[end-1-i]=(uint8_t)(bits>>(8*i));sha_block(h,tail);if(end==128)sha_block(h,tail+64);
 for(unsigned i=0;i<32;i++)out[i]=(uint8_t)(h[i/4]>>(24-8*(i%4)));
}
static NH_STATUS view_read(void *c,uint64_t o,void *b,size_t n) {
 NH_VIEW *v=c;uint8_t *p=b;uint32_t lo=0,hi=v->Count;
 while(lo<hi){uint32_t mid=lo+(hi-lo)/2;if(v->Extents[mid].Logical<=o)lo=mid+1;else hi=mid;}if(!lo)return NH_CORRUPT;lo--;
 while(n) {if(lo>=v->Count)return NH_CORRUPT;NH_EXTENT *e=&v->Extents[lo++];if(o<e->Logical||o-e->Logical>=e->Bytes)return NH_CORRUPT;
  uint64_t d=o-e->Logical;size_t take=(size_t)NH_MIN((uint64_t)n,e->Bytes-d);
  if(e->Kind==0)NH_TRY(NhRead(&v->Parent,e->Physical+d,p,take));
  else if(e->Kind==1)for(size_t j=0;j<take;j++)p[j]=(uint8_t)(e->Fill>>(8*((d+j)&3)));
  else if(e->Kind==2)memset(p,0,take);else return NH_CORRUPT;
  p+=take;o+=take;n-=take;
 }return NH_OK;
}
static void *view_alloc(void *c,size_t n){NH_VIEW *v=c;return v->Parent.Alloc(v->Parent.Context,n);}
static void view_free(void *c,void *p){NH_VIEW *v=c;v->Parent.Free(v->Parent.Context,p);}
static void finish_view(NH_VIEW *v,uint64_t bytes){v->Reader=v->Parent;v->Reader.Context=v;v->Reader.Read=view_read;v->Reader.Bytes=bytes;v->Reader.Alloc=view_alloc;v->Reader.Free=view_free;}
void NhViewClose(NH_VIEW *v){if(v&&v->Extents)v->Parent.Free(v->Parent.Context,v->Extents);if(v)memset(v,0,sizeof(*v));}
NH_STATUS NhSparseOpen(const NH_READER *r,NH_VIEW *v) {
 uint8_t h[28],c[16];if(!r||!v||!r->Alloc||!r->Free)return NH_INVALID;memset(v,0,sizeof(*v));NH_TRY(NhRead(r,0,h,sizeof(h)));if(NhU32(h)!=0xed26ff3a)return NH_UNSUPPORTED;
 /* Checked sparse CRC streams need an eager whole-image pass; production
  * disks are unsparsed. Reject these offline variants rather than ignore CRC. */
 if(NhU32(h+24))return NH_UNSUPPORTED;
 uint32_t block=NhU32(h+12),chunks=NhU32(h+20);uint16_t hs=NhU16(h+8),cs=NhU16(h+10);
 if(NhU16(h+4)!=1||NhU16(h+6)!=0||hs<28||cs<12||!block||block>1048576||block%4||!chunks||chunks>NH_MAX_MAP)return NH_CORRUPT;
 v->Parent=*r;v->Extents=r->Alloc(r->Context,(size_t)chunks*sizeof(NH_EXTENT));if(!v->Extents)return NH_MEMORY;
 uint64_t pos=hs,logical=0,total=(uint64_t)NhU32(h+16)*block;NH_STATUS s=NH_OK;
 for(uint32_t i=0;i<chunks;i++){s=NhRead(r,pos,c,12);if(s!=NH_OK)goto bad;uint32_t bytes=NhU32(c+8),count=NhU32(c+4);uint16_t type=NhU16(c);uint64_t len=(uint64_t)count*block;
  if(bytes<cs||pos>r->Bytes||bytes>r->Bytes-pos||len>total-logical){s=NH_CORRUPT;goto bad;}
  NH_EXTENT *e=&v->Extents[v->Count];e->Logical=logical;e->Physical=pos+cs;e->Bytes=len;e->Fill=0;
  if(type==0xcac1&&bytes-cs==len)e->Kind=0;
  else if(type==0xcac2&&bytes-cs==4){s=NhRead(r,pos+cs,c,4);if(s!=NH_OK)goto bad;e->Kind=1;e->Fill=NhU32(c);}
  else if(type==0xcac3&&bytes==cs)e->Kind=2;
  else if(type==0xcac4&&bytes-cs==4&&!count){s=NH_UNSUPPORTED;goto bad;}
  else {s=NH_CORRUPT;goto bad;}
  if(!len){s=NH_CORRUPT;goto bad;}v->Count++;logical+=len;pos+=bytes;
 }
 if(logical!=total||pos!=r->Bytes){s=NH_CORRUPT;goto bad;}finish_view(v,total);return NH_OK;
bad:NhViewClose(v);return s;
}
static int fixed_name(const uint8_t *p,size_t n,const char *s){size_t z=strlen(s);if(z>=n||memcmp(p,s,z)||p[z])return 0;for(size_t i=z+1;i<n;i++)if(p[i])return 0;return 1;}
NH_STATUS NhSuperOpen(const NH_READER *r,uint32_t slot,const char *name,NH_VIEW *v) {
 uint8_t geom[52],backup[52],sum[32],head[256];uint8_t *tables=NULL;NH_STATUS s;uint64_t logical=0;
 if(!r||!v||!r->Alloc||!r->Free||NhPathCheck(name,0)!=NH_OK||strchr(name,'/'))return NH_INVALID;memset(v,0,sizeof(*v));v->Parent=*r;
 NH_TRY(NhRead(r,4096,geom,52));NH_TRY(NhRead(r,8192,backup,52));if(memcmp(geom,backup,52)||NhU32(geom)!=0x616c4467||NhU32(geom+4)!=52)return NH_CORRUPT;
 memcpy(sum,geom+8,32);memset(geom+8,0,32);NhSha256(geom,52,backup);if(memcmp(sum,backup,32))return NH_CORRUPT;
 uint32_t max=NhU32(geom+40),slots=NhU32(geom+44);if(max<512||max>1048576||max%512||!slots||slots>4||slot>=slots||NhU32(geom+48)!=4096)return NH_UNSUPPORTED;
 uint64_t off=12288+(uint64_t)slot*max;NH_TRY(NhRead(r,off,head,128));uint32_t hs=NhU32(head+8),ts=NhU32(head+44);
 if(NhU32(head)!=0x414c5030||NhU16(head+4)!=10||NhU16(head+6)>2||hs<128||hs>256||ts>max-hs)return NH_UNSUPPORTED;
 NH_TRY(NhRead(r,off,head,hs));memcpy(sum,head+12,32);memset(head+12,0,32);NhSha256(head,hs,backup);if(memcmp(sum,backup,32))return NH_CORRUPT;
 /* Virtual A/B snapshots need dm-snapshot/COW resolution. Never silently read
  * old base extents during an update: runtime admission checks misc merge
  * status separately. Bit 0 advertises Virtual A/B capability, not activity. */
 if(hs>=132&&(NhU32(head+128)&~1U)!=0)return NH_NOT_READY;
 v->Flags=hs>=132?NhU32(head+128):0;
 tables=r->Alloc(r->Context,ts?ts:1);if(!tables)return NH_MEMORY;s=NhRead(r,off+hs,tables,ts);if(s!=NH_OK)goto done;NhSha256(tables,ts,sum);if(memcmp(sum,head+48,32)){s=NH_CORRUPT;goto done;}
 uint32_t td[4][3];const uint32_t minsize[4]={52,24,48,64};
 for(unsigned i=0;i<4;i++){for(unsigned j=0;j<3;j++)td[i][j]=NhU32(head+80+12*i+4*j);if(td[i][2]!=minsize[i]||td[i][0]>ts||td[i][1]>(ts-td[i][0])/td[i][2]){s=NH_CORRUPT;goto done;}}
 for(unsigned i=0;i<4;i++)for(unsigned j=0;j<i;j++)if(td[i][1]&&td[j][1]&&td[i][0]<(uint64_t)td[j][0]+td[j][1]*td[j][2]&&td[j][0]<(uint64_t)td[i][0]+td[i][1]*td[i][2]){s=NH_CORRUPT;goto done;}
 if(td[3][1]!=1){s=NH_UNSUPPORTED;goto done;}const uint8_t *dev=tables+td[3][0];uint64_t first=NhU64(dev),devbytes=NhU64(dev+16);
 if(first<((12288+(uint64_t)max*slots*2+511)/512)||first>r->Bytes/512||devbytes>r->Bytes||devbytes/512<first||NhU32(dev+60)){s=NH_CORRUPT;goto done;}
 const uint8_t *part=NULL;for(uint32_t i=0;i<td[0][1];i++){const uint8_t *p=tables+td[0][0]+i*52;if(fixed_name(p,36,name)){if(part){s=NH_CORRUPT;goto done;}part=p;}}
 if(!part){s=NH_NOT_FOUND;goto done;}uint32_t start=NhU32(part+40),count=NhU32(part+44),group=NhU32(part+48);
 if(NhU32(part+36)&~1U||group>=td[2][1]||!count||count>4096||start>td[1][1]||count>td[1][1]-start){s=NH_CORRUPT;goto done;}
 v->Extents=r->Alloc(r->Context,(size_t)count*sizeof(NH_EXTENT));if(!v->Extents){s=NH_MEMORY;goto done;}
 for(uint32_t i=0;i<count;i++){const uint8_t *e=tables+td[1][0]+(start+i)*24;uint64_t sectors=NhU64(e),where=NhU64(e+12);uint32_t type=NhU32(e+8);
  if(!sectors||sectors>UINT64_MAX/512||logical>UINT64_MAX-sectors*512||NhU32(e+20)||type>1||(type==1&&where)||(type==0&&(where<first||where>devbytes/512||sectors>devbytes/512-where))){s=NH_CORRUPT;goto done;}
  if(type==0)for(uint32_t j=0;j<i;j++){NH_EXTENT *prev=&v->Extents[j];if(!prev->Kind&&where*512<prev->Physical+prev->Bytes&&prev->Physical<(where+sectors)*512){s=NH_CORRUPT;goto done;}}
  NH_EXTENT *dst=&v->Extents[i];dst->Logical=logical;dst->Physical=where*512;dst->Bytes=sectors*512;dst->Kind=type?2:0;dst->Fill=0;logical+=dst->Bytes;v->Count++;
 }
 finish_view(v,logical);s=NH_OK;
done:r->Free(r->Context,tables);if(s!=NH_OK){uint32_t flags=v->Flags;NhViewClose(v);if(s==NH_NOT_FOUND)v->Flags=flags;}return s;
}
