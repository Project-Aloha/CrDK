/* Host-only NT API shape for portable-library tests, not a Windows SDK.
   SPDX-License-Identifier: MIT */
#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>

#define IN
#define OUT
#define CONST const
#define TRUE 1
#define FALSE 0
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define ASSERT(x) assert(x)
typedef void VOID;
typedef char CHAR;
typedef uint8_t BOOLEAN, UINT8;
typedef int8_t INT8;
typedef uint16_t UINT16;
typedef int16_t INT16;
typedef uint32_t UINT32, ULONG;
typedef int32_t INT32, LONG, NTSTATUS;
typedef uint64_t UINT64;
typedef int64_t INT64;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef size_t SIZE_T;
typedef ptrdiff_t SSIZE_T;
typedef unsigned char KIRQL;
#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2
#define NT_SUCCESS(x) ((NTSTATUS)(x) >= 0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_CANCELLED ((NTSTATUS)0xc0000120U)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dU)
#define STATUS_NOT_FOUND ((NTSTATUS)0xc0000225U)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xc000009aU)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bbU)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xc000009cU)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xc00000b5U)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011U)
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023U)
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xc0000206U)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3U)
#define STATUS_OBJECT_NAME_COLLISION ((NTSTATUS)0xc0000035U)
#define DPFLTR_IHVDRIVER_ID 77
#define DPFLTR_ERROR_LEVEL 0
#define KdPrintEx(x) ((void)0)
static inline VOID DbgBreakPoint(VOID) { assert(FALSE); }
static inline VOID KeMemoryBarrier(VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#define REGISTER_FENCE() KeMemoryBarrier()
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG old) {
  __atomic_compare_exchange_n(p, &old, value, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return old;
}
static inline LONG InterlockedOr(volatile LONG *p, LONG bits) {
  return __atomic_fetch_or(p, bits, __ATOMIC_SEQ_CST);
}
static inline LONG InterlockedAnd(volatile LONG *p, LONG bits) {
  return __atomic_fetch_and(p, bits, __ATOMIC_SEQ_CST);
}
static inline SIZE_T RtlCompareMemory(CONST VOID *a, CONST VOID *b, SIZE_T n) {
  SIZE_T i = 0;
  while (i < n && ((CONST UINT8 *)a)[i] == ((CONST UINT8 *)b)[i]) ++i;
  return i;
}
#define RtlCopyMemory(a,b,n) memcpy(a,b,n)
#define RtlFillMemory(a,n,v) memset(a,v,n)
static inline LONG InterlockedExchange(volatile LONG *p, LONG value) {
  return __atomic_exchange_n(p, value, __ATOMIC_SEQ_CST);
}
typedef uintptr_t KSPIN_LOCK;
typedef union { LONGLONG QuadPart; } LARGE_INTEGER;
typedef enum { KernelMode, UserMode } KPROCESSOR_MODE;
typedef enum { Executive } KWAIT_REASON;
typedef uintptr_t KMUTEX;
VOID KeInitializeMutex(KMUTEX *mutex, ULONG level);
NTSTATUS KeWaitForSingleObject(VOID *object, KWAIT_REASON reason,
    KPROCESSOR_MODE mode, BOOLEAN alertable, LARGE_INTEGER *timeout);
LONG KeReleaseMutex(KMUTEX *mutex, BOOLEAN wait);
VOID KeInitializeSpinLock(KSPIN_LOCK *lock);
VOID KeAcquireSpinLock(KSPIN_LOCK *lock, KIRQL *irql);
VOID KeReleaseSpinLock(KSPIN_LOCK *lock, KIRQL irql);
VOID KeStallExecutionProcessor(ULONG microseconds);
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, LARGE_INTEGER *interval);
typedef struct {
  pthread_mutex_t Mutex;
  pthread_cond_t Condition;
  SIZE_T Active;
  BOOLEAN Closing;
} EX_RUNDOWN_REF;
VOID ExInitializeRundownProtection(EX_RUNDOWN_REF *ref);
VOID ExReInitializeRundownProtection(EX_RUNDOWN_REF *ref);
BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *ref);
VOID ExReleaseRundownProtection(EX_RUNDOWN_REF *ref);
VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *ref);
KIRQL KeGetCurrentIrql(VOID);
