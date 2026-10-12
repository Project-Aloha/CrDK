/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_ANDROID_BOOT_VERSION 1U
#define CR_ANDROID_BOOT_UNKNOWN 0U
#define CR_ANDROID_BOOT_NORMAL 1U
#define CR_ANDROID_BOOT_CHARGER 2U
#define CR_ANDROID_BOOT_SOURCE_NONE 0U
#define CR_ANDROID_BOOT_SOURCE_CMDLINE 1U
#define CR_ANDROID_BOOT_SOURCE_BOOTCONFIG 2U
#define CR_ANDROID_BOOT_FDT_VALID 1U
#define CR_ANDROID_BOOT_CMDLINE_PRESENT 2U
#define CR_ANDROID_BOOT_BOOTCONFIG_PRESENT 4U
#define CR_ANDROID_BOOT_BOOTCONFIG_OVERRIDES 8U
#define CR_ANDROID_BOOT_FDT_MAX (2U * 1024U * 1024U)
#define CR_ANDROID_BOOT_BOOTCONFIG_MAX (64U * 1024U)

typedef struct {
  UINT32 Version;
  UINT32 Mode;
  UINT32 Source;
  UINT32 Flags;
  UINT64 FdtAddress;
} CR_ANDROID_BOOT_INFO;

CR_STATIC_ASSERT(sizeof(CR_ANDROID_BOOT_INFO) == 24, "Android boot info layout");
#ifndef _KERNEL_MODE
extern EFI_GUID gCrAndroidBootInfoGuid;
#endif

/* Each returned range must remain readable and immutable until this call
   returns. MapRead is the sole authority for physical reads, including the
   complete DTB, initrd trailer and bootconfig text. No allocation or physical
   dereference occurs here. Info is unchanged on failure and must not overlap
   a returned input range. An absent mode succeeds with UNKNOWN/SOURCE_NONE;
   an explicit non-charger mode yields NORMAL. Conflicting mode assignments
   within one source fail; a bootconfig mode takes precedence over cmdline.
   Only the exact androidboot.mode key and exact charger value select CHARGER.
   Bootconfig accepts dotted/nested keys, scalar/array values and comments;
   mode arrays and append/override operators are deliberately rejected. */
CR_STATUS CrAndroidBootInspect(
    UINT64 FdtAddress,
    CONST VOID *(*MapRead)(VOID *Context, UINT64 Address, UINTN Size),
    VOID *Context,
    CR_ANDROID_BOOT_INFO *Info);
