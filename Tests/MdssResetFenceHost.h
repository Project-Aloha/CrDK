/* Host-only boundary types. Not firmware ABI definitions. SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef NULL
#define NULL ((void *)0)
#endif
#define STATIC static
#define CONST const
#define VOID void
#define EFIAPI
#define IN
#define OUT
#define TRUE 1
#define FALSE 0
typedef uint8_t BOOLEAN;
typedef uint8_t UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef uintptr_t UINTN;
typedef intptr_t INTN;
typedef char CHAR8;
typedef UINT64 EFI_STATUS;
typedef EFI_STATUS CR_STATUS;
typedef VOID *EFI_EVENT;
#define EFI_SUCCESS UINT64_C(0)
#define ERR(n) ((UINT64_C(1)<<63)|(n))
#define EFI_INVALID_PARAMETER ERR(2)
#define EFI_UNSUPPORTED ERR(3)
#define EFI_NOT_READY ERR(6)
#define EFI_DEVICE_ERROR ERR(7)
#define EFI_NOT_FOUND ERR(14)
#define EFI_ACCESS_DENIED ERR(15)
#define EFI_ALREADY_STARTED ERR(20)
#define EFI_ERROR(s) (((EFI_STATUS)(s)&(UINT64_C(1)<<63))!=0)
#define CR_SUCCESS EFI_SUCCESS
#define CR_BUSY EFI_ACCESS_DENIED
#define CR_INVALID_PARAMETER EFI_INVALID_PARAMETER
#define CR_UNSUPPORTED EFI_UNSUPPORTED
#define CR_DEVICE_ERROR EFI_DEVICE_ERROR
/* Only the public values consumed by the selected functions are modeled. */
#define GPIO_PINS_MAX 1024U
#define GPIO_FUNC_NORMAL 0U
#define GPIO_FUNC_1 1U
#define GPIO_FUNC_MAX 15U
#define GPIO_PULL_NONE 0U
#define GPIO_PULL_DOWN 1U
#define GPIO_PULL_UP 3U
#define GPIO_PULL_UP_NO_KEEPER 2U
#define GPIO_PULL_MAX 3U
#define GPIO_DRIVE_STRENGTH_2MA 0U
#define GPIO_DRIVE_STRENGTH_8MA 3U
#define GPIO_DRIVE_STRENGTH_MAX 7U
#define GPIO_VALUE_LOW 0U
#define GPIO_VALUE_HIGH 1U
#define GPIO_VALUE_MAX 1U
typedef UINT32 GpioPullType;
typedef UINT32 GpioValueType;
typedef struct {UINT16 PinNumber;BOOLEAN OutputEnable;UINT32 FunctionSel,Pull,DriveStrength,OutputValue;} GpioConfigParams;
#define EFI_RPMH_CR_PROTOCOL_REVISION UINT64_C(0x10005)
#define EFI_GPIO_CR_PROTOCOL_REVISION UINT64_C(0x10003)
typedef struct _EFI_RPMH_CR_PROTOCOL EFI_RPMH_CR_PROTOCOL;
typedef struct _EFI_GPIO_CR_PROTOCOL EFI_GPIO_CR_PROTOCOL;
typedef EFI_STATUS (*EFI_RPMH_CR_ENSURE_RETAINED_VREG_ENABLED)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *);
typedef EFI_STATUS (*EFI_RPMH_CR_ENSURE_RETAINED_VREG)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *,UINT32,UINT8);
typedef EFI_STATUS (*EFI_GPIO_CR_ACQUIRE_PIN)(EFI_GPIO_CR_PROTOCOL *,VOID *,CONST GpioConfigParams *,BOOLEAN *);
typedef EFI_STATUS (*EFI_GPIO_CR_SET_PIN_VALUE)(EFI_GPIO_CR_PROTOCOL *,VOID *,UINT16,UINT8);
typedef EFI_STATUS (*EFI_GPIO_CR_READ_PIN_VALUE)(EFI_GPIO_CR_PROTOCOL *,VOID *,UINT16,UINT8 *);
struct _EFI_RPMH_CR_PROTOCOL {
  UINT64 Revision;
  EFI_STATUS (*AcquireVreg)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *,UINT32,UINT8,BOOLEAN *);
  EFI_STATUS (*RpmhEnableVreg)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *,BOOLEAN);
  EFI_STATUS (*RpmhSetVregVoltage)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *,UINT32);
  EFI_STATUS (*RpmhSetVregMode)(EFI_RPMH_CR_PROTOCOL *,CONST CHAR8 *,UINT8);
  EFI_RPMH_CR_ENSURE_RETAINED_VREG_ENABLED EnsureRetainedVregEnabled;
  EFI_RPMH_CR_ENSURE_RETAINED_VREG EnsureRetainedVreg;
};
struct _EFI_GPIO_CR_PROTOCOL {
  UINT64 Revision;
  EFI_GPIO_CR_ACQUIRE_PIN AcquirePin;
  EFI_GPIO_CR_SET_PIN_VALUE SetPinValue;
  EFI_GPIO_CR_READ_PIN_VALUE ReadPinValue;
  EFI_STATUS (*ReleasePin)(EFI_GPIO_CR_PROTOCOL *,VOID *,UINT16);
};
VOID *EFIAPI ZeroMem(VOID *,UINTN);
VOID *EFIAPI CopyMem(VOID *,CONST VOID *,UINTN);
VOID *EFIAPI SetMem(VOID *,UINTN,UINT8);
INTN EFIAPI CompareMem(CONST VOID *,CONST VOID *,UINTN);
