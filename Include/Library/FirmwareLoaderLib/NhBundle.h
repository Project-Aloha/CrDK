/* SPDX-License-Identifier: MIT. Same-candidate immutable firmware group. */
#ifndef NH_BUNDLE_H
#define NH_BUNDLE_H
#include <Library/FirmwareLoaderLib/NhFs.h>
#define NH_PARTS 101U
#define NH_HEADER_PART 100U
#define NH_BUNDLE_FILE 0U
#define NH_BUNDLE_PIL 1U
typedef struct {void *Data;uint32_t Bytes,Index;} NH_PART;
typedef struct {uint32_t Version,Count,Kind,Origin;NH_PART Parts[NH_PARTS];} NH_BUNDLE;
typedef NH_STATUS (*NH_LOAD_FILE)(void *,const char *,void **,uint32_t *);
NH_STATUS NhBundleRead(void *,NH_LOAD_FILE,NH_ALLOC,NH_FREE,const char *,uint32_t,NH_BUNDLE **);
void NhBundleFree(void *,NH_FREE,NH_BUNDLE *);
#endif
