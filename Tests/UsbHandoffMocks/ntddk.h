/* Deterministic publication interleaving at the OS barrier boundary only. */
#pragma once
#define KeMemoryBarrier CrUsbTestUnusedBarrier
#include "../KernelMocks/ntddk.h"
#undef KeMemoryBarrier
VOID CrUsbTestBarrier(VOID);
#define KeMemoryBarrier CrUsbTestBarrier
