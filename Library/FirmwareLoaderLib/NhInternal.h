/* SPDX-License-Identifier: MIT */
#ifndef NH_INTERNAL_H
#define NH_INTERNAL_H
#include <Library/FirmwareLoaderLib/NhFs.h>
#include "NhString.h"
#define NH_MIN(a,b) ((a)<(b)?(a):(b))
#define NH_DIR 0040000U
#define NH_REG 0100000U
#define NH_LINK 0120000U
#define NH_MODE 0170000U
NH_STATUS NhExtOpen(NH_FS *); NH_STATUS NhFatOpen(NH_FS *); NH_STATUS NhErofsOpen(NH_FS *); NH_STATUS NhF2fsOpen(NH_FS *);
NH_STATUS NhExtInode(NH_FS *,uint64_t,NH_FILE *); NH_STATUS NhErofsInode(NH_FS *,uint64_t,NH_FILE *); NH_STATUS NhF2fsInode(NH_FS *,uint64_t,NH_FILE *);
NH_STATUS NhExtRead(NH_FILE *,uint64_t,void *,size_t); NH_STATUS NhFatRead(NH_FILE *,uint64_t,void *,size_t); NH_STATUS NhErofsRead(NH_FILE *,uint64_t,void *,size_t); NH_STATUS NhF2fsRead(NH_FILE *,uint64_t,void *,size_t);
NH_STATUS NhExtCheckDirectory(NH_FILE *,uint64_t,uint8_t *,size_t);
NH_STATUS NhFatLookup(NH_FILE *,const char *,size_t,NH_FILE *); NH_STATUS NhF2fsLookup(NH_FILE *,const char *,size_t,NH_FILE *);
#endif
