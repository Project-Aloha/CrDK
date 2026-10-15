/* SPDX-License-Identifier: MIT
 * Read-only on-disk reader. No operating system, mutable globals or packed
 * native-struct casts. All offsets are byte offsets and all reads are exact.
 */
#ifndef NH_FS_H
#define NH_FS_H
#if !defined(NH_HOST_TEST) && (defined(NH_FIRMWARE_UEFI) || defined(__BASE_H__))
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef INT32 int32_t;
typedef INT64 int64_t;
typedef UINTN size_t;
#ifndef UINT32_MAX
#define UINT32_MAX 0xffffffffU
#endif
#ifndef UINT64_MAX
#define UINT64_MAX 0xffffffffffffffffULL
#endif
#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif
#else
#include <stdint.h>
#include <stddef.h>
#endif
typedef int32_t NH_STATUS;
#define NH_OK ((NH_STATUS)0)
#define NH_NOT_FOUND ((NH_STATUS)-1)
#define NH_CORRUPT ((NH_STATUS)-2)
#define NH_UNSUPPORTED ((NH_STATUS)-3)
#define NH_IO ((NH_STATUS)-4)
#define NH_MEMORY ((NH_STATUS)-5)
#define NH_INVALID ((NH_STATUS)-6)
#define NH_NOT_READY ((NH_STATUS)-7)
#define NH_TOO_LARGE ((NH_STATUS)-8)
#define NH_MAX_PATH 512U
#define NH_MAX_FILE (128U * 1024U * 1024U)
#define NH_MAX_MAP 65536U
#define NH_TRY(e) do { NH_STATUS nh_result_ = (e); if (nh_result_ != NH_OK) return nh_result_; } while (0)
typedef NH_STATUS (*NH_READ)(void *, uint64_t, void *, size_t);
typedef void *(*NH_ALLOC)(void *, size_t);
typedef void (*NH_FREE)(void *, void *);
typedef struct {void *Context; NH_READ Read; uint64_t Bytes; NH_ALLOC Alloc; NH_FREE Free;} NH_READER;
typedef struct {uint64_t Logical, Physical, Bytes; uint32_t Kind, Fill;} NH_EXTENT;
typedef struct {NH_READER Reader, Parent; NH_EXTENT *Extents; uint32_t Count, Flags;} NH_VIEW;
typedef struct NH_FS {NH_READER Io; uint32_t Type, BlockSize; uint64_t Root, Blocks;
 uint64_t A, B, C, D; uint32_t E, F, G, H, Checksum; uint8_t Uuid[16]; void *Private;} NH_FS;
typedef struct {NH_FS *Fs; uint64_t Id, Size, Address; uint32_t Mode, Layout, Flags, Extra; uint8_t Raw[256];} NH_FILE;
NH_STATUS NhRead(const NH_READER *, uint64_t, void *, size_t);
/* Strict read-only GPT lookup, including matching redundant tables. Only a
 * valid GPT without Name returns NOT_FOUND. Both missing GPT signatures return
 * UNSUPPORTED (raw boot LUN); malformed/unknown GPT layouts return CORRUPT. */
NH_STATUS NhGptFind(const NH_READER *, uint32_t SectorSize, const char *Name,
                    uint64_t *Offset, uint64_t *Bytes);
NH_STATUS NhSparseOpen(const NH_READER *, NH_VIEW *);
/* On NH_NOT_FOUND, Flags still reports validated LP header flags, so callers
 * must admit any Virtual A/B state before treating absence as fallback. */
NH_STATUS NhSuperOpen(const NH_READER *, uint32_t Slot, const char *Partition, NH_VIEW *);
void NhViewClose(NH_VIEW *);
NH_STATUS NhFsOpen(const NH_READER *, NH_FS *);
void NhFsClose(NH_FS *);
NH_STATUS NhFileOpen(NH_FS *, const char *, NH_FILE *);
NH_STATUS NhFileRead(NH_FILE *, uint64_t, void *, size_t);
NH_STATUS NhPathCheck(const char *, int AllowEmpty);
uint16_t NhU16(const void *); uint32_t NhU32(const void *); uint64_t NhU64(const void *);
void NhSha256(const void *, size_t, uint8_t[32]);
uint32_t NhCrc32(uint32_t, const void *, size_t, uint32_t);
#define NH_FS_EXT4 1U
#define NH_FS_FAT 2U
#define NH_FS_EROFS 3U
#define NH_FS_F2FS 4U
#endif
