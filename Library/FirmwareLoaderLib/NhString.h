/* SPDX-License-Identifier: MIT. Environment glue only; no on-disk policy. */
#ifndef NH_STRING_H
#define NH_STRING_H
#if defined(NH_FIRMWARE_UEFI)
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#define memcpy(d,s,n) CopyMem((d),(s),(n))
#define memmove(d,s,n) CopyMem((d),(s),(n))
#define memset(d,c,n) SetMem((d),(n),(UINT8)(c))
#define memcmp(a,b,n) CompareMem((a),(b),(n))
#define strlen(s) AsciiStrLen(s)
#define strcmp(a,b) AsciiStrCmp((a),(b))
#define strncmp(a,b,n) AsciiStrnCmp((a),(b),(n))
#define strstr(a,b) AsciiStrStr((a),(b))
static inline char *NhStrChr(const char *s, int c) {
  do { if (*s == (char)c) return (char *)s; } while (*s++);
  return NULL;
}
#define strchr(s,c) NhStrChr((s),(c))
#else
#include <string.h>
#endif
#endif
