/* PCIe backend callbacks must remain dormant during DAL discovery. MIT */
#include <assert.h>
#include <string.h>
#undef NULL
#include <Uefi.h>
UINT8 EFIAPI MmioRead8(UINTN A) {(void)A;assert(0);return 0;}
UINT16 EFIAPI MmioRead16(UINTN A) {(void)A;assert(0);return 0;}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V) {(void)A;(void)V;assert(0);return 0;}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V) {(void)A;(void)V;assert(0);return 0;}
UINT64 EFIAPI GetPerformanceCounter(VOID) {assert(0);return 0;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 T) {(void)T;assert(0);return 0;}
__attribute__((weak)) VOID *EFIAPI SetMem(VOID *B,UINTN N,UINT8 V) {return memset(B,V,N);}
