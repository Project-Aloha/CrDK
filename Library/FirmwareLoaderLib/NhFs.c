/* SPDX-License-Identifier: MIT */
#include "NhInternal.h"
NH_STATUS NhFsOpen(const NH_READER *r,NH_FS *f) {
 uint8_t s[64];if(!r||!r->Alloc||!r->Free||!f)return NH_INVALID;memset(f,0,sizeof(*f));f->Io=*r;NH_TRY(NhRead(r,1024,s,64));
 NH_STATUS status;if(NhU32(s)==0xe0f5e1e2){f->Type=NH_FS_EROFS;status=NhErofsOpen(f);}else if(NhU32(s)==0xf2f52010){f->Type=NH_FS_F2FS;status=NhF2fsOpen(f);}else if(NhU16(s+56)==0xef53){f->Type=NH_FS_EXT4;status=NhExtOpen(f);}else{f->Type=NH_FS_FAT;status=NhFatOpen(f);}
 if(status!=NH_OK)NhFsClose(f);return status;
}
void NhFsClose(NH_FS *f){if(!f)return;if(f->Private)f->Io.Free(f->Io.Context,f->Private);memset(f,0,sizeof(*f));}
NH_STATUS NhFileRead(NH_FILE *n,uint64_t o,void *b,size_t bytes) {
 if(!n||!n->Fs||(!b&&bytes)||o>n->Size||bytes>n->Size-o)return NH_INVALID;
 switch(n->Fs->Type){case NH_FS_EXT4:return NhExtRead(n,o,b,bytes);case NH_FS_FAT:return NhFatRead(n,o,b,bytes);case NH_FS_EROFS:return NhErofsRead(n,o,b,bytes);case NH_FS_F2FS:return NhF2fsRead(n,o,b,bytes);default:return NH_INVALID;}
}
static NH_STATUS inode(NH_FS *f,uint64_t id,NH_FILE *n){switch(f->Type){case NH_FS_EXT4:return NhExtInode(f,id,n);case NH_FS_EROFS:return NhErofsInode(f,id,n);case NH_FS_F2FS:return NhF2fsInode(f,id,n);case NH_FS_FAT:memset(n,0,sizeof(*n));n->Fs=f;n->Id=id;n->Mode=NH_DIR;n->Size=NH_MAX_FILE;return NH_OK;default:return NH_INVALID;}}
static NH_STATUS lookup(NH_FILE *dir,const char *name,size_t len,NH_FILE *n){
 if((dir->Mode&NH_MODE)!=NH_DIR)return NH_CORRUPT;NH_FS *f=dir->Fs;
 if(f->Type==NH_FS_FAT)return NhFatLookup(dir,name,len,n);if(f->Type==NH_FS_F2FS)return NhF2fsLookup(dir,name,len,n);
 uint8_t *b=f->Io.Alloc(f->Io.Context,f->BlockSize);if(!b)return NH_MEMORY;NH_STATUS s=NH_NOT_FOUND;uint64_t id=0;
 for(uint64_t o=0;o<dir->Size;o+=f->BlockSize){size_t z=(size_t)NH_MIN((uint64_t)f->BlockSize,dir->Size-o);s=NhFileRead(dir,o,b,z);if(s!=NH_OK)goto done;
  if(f->Type==NH_FS_EXT4){s=NhExtCheckDirectory(dir,o,b,z);if(s!=NH_OK)goto done;for(size_t i=0;i<z;){if(z-i<8){s=NH_CORRUPT;goto done;}uint32_t child=NhU32(b+i);size_t rec=NhU16(b+i+4),nm=b[i+6];if(rec<8||rec%4||rec>z-i||nm>rec-8){s=NH_CORRUPT;goto done;}if(child&&nm==len&&!memcmp(b+i+8,name,len)){id=child;goto found;}i+=rec;}}
  else {if(z<12){s=NH_CORRUPT;goto done;}size_t first=NhU16(b+8);if(first<12||first%12||first>z){s=NH_CORRUPT;goto done;}
   for(size_t i=0;i<first;i+=12){size_t at=NhU16(b+i+8),end=i+12<first?NhU16(b+i+20):z;if(at<first||at>=end||end>z){s=NH_CORRUPT;goto done;}size_t nm=end-at;if(i+12==first)while(nm&&!b[at+nm-1])nm--;if(!nm||nm>255){s=NH_CORRUPT;goto done;}if(nm==len&&!memcmp(b+at,name,len)){id=NhU64(b+i);goto found;}}
  }
 }s=NH_NOT_FOUND;goto done;
found:s=inode(f,id,n);
done:f->Io.Free(f->Io.Context,b);return s;
}
NH_STATUS NhFileOpen(NH_FS *f,const char *path,NH_FILE *out) {
 if(!f||!out)return NH_INVALID;NH_TRY(NhPathCheck(path,1));NH_FILE current,next;NH_TRY(inode(f,f->Root,&current));
 const char *p=path;while(*p){const char *end=strchr(p,'/');size_t len=end?(size_t)(end-p):strlen(p);NH_TRY(lookup(&current,p,len,&next));
  /* Do not follow a symlink out of a selected directory/partition or resolve
   * Android absolute mountpoint names in the Windows object namespace. INF
   * candidates must name the actual regular file; links are explicit errors. */
  if((next.Mode&NH_MODE)==NH_LINK)return NH_UNSUPPORTED;current=next;if(!end)break;p=end+1;
 }*out=current;return NH_OK;
}
