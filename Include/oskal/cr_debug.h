/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
// OS Independent debug print lib
#ifdef _KERNEL_MODE
#include <ntddk.h>
#include "trace.h"

/* COPY THEY FOLLOWING BLOCK TO TRACE.h */
// In WPP_CONTROL_GUIDS:
//   WPP_DEFINE_BIT(TRACE_OSKAL) \
// 
// In the end of TRACE.h:
// begin_wpp config
// FUNC Trace{FLAGS=MYDRIVER_ALL_INFO}(LEVEL, MSG, ...);
// FUNC TraceEvents(LEVEL, FLAGS, MSG, ...);
//
// FUNC TRACE_FUNCTION_ENTRY{LEVEL=TRACE_LEVEL_VERBOSE}(FLAGS, ...);
// USESUFFIX(TRACE_FUNCTION_ENTRY, "%!FUNC! Entry");
//
// FUNC TRACE_FUNCTION_EXIT{LEVEL=TRACE_LEVEL_VERBOSE}(FLAGS, ...);
// USESUFFIX(TRACE_FUNCTION_EXIT, "%!FUNC! Exit");
//
// FUNC log_raw{FLAGS=TRACE_OSKAL, LEVEL=TRACE_LEVEL_INFORMATION}(MSG, ...);
// FUNC log_info{FLAGS=TRACE_OSKAL, LEVEL=TRACE_LEVEL_INFORMATION}(MSG, ...);
// FUNC log_warn{FLAGS=TRACE_OSKAL, LEVEL=TRACE_LEVEL_WARNING}(MSG, ...);
// FUNC log_err{FLAGS=TRACE_OSKAL, LEVEL=TRACE_LEVEL_ERROR}(MSG, ...);
// end_wpp

/* Kernel-mode logging: TraceEvents is always available in this project. */
// #define log_raw(fmt, ...) \
//   Trace(TRACE_LEVEL_INFORMATION, fmt, ##__VA_ARGS__)

// #define log_info(fmt, ...) \
//   Trace(TRACE_LEVEL_INFORMATION, "[INFO] " fmt, ##__VA_ARGS__)

// #define log_warn(fmt, ...) \
//   Trace(TRACE_LEVEL_WARNING, "[WARN] " fmt, ##__VA_ARGS__)

// #define log_err(fmt, ...) \
//   Trace(TRACE_LEVEL_ERROR, "[ERROR] " fmt " (in %s:%d)", ##__VA_ARGS__, __FILE__, __LINE__)
/* WPP does not support colors T_T */
#define LOG_COLOR_INFO
#define LOG_COLOR_RESET
#define LOG_COLOR_INFO
#define LOG_COLOR_WARN
#define LOG_COLOR_ERROR

/* defined in WPP */
 //#define CR_LOG_CHAR8_STR_FMT "%s"

#else
#include <Library/DebugLib.h>

#define LOG_COLOR_RESET "\x1b[0m"
#define LOG_COLOR_INFO "\x1b[97m"       /* bright white */
#define LOG_COLOR_WARN "\x1b[38;5;208m" /* orange (256-color) */
#define LOG_COLOR_ERROR "\x1b[31m"      /* red */

#define CR_LOG_CHAR8_STR_FMT "%a"

#define log_raw(fmt, ...)                                                      \
  DebugPrint(DEBUG_WARN, fmt LOG_COLOR_RESET, ##__VA_ARGS__)
#define log_info(fmt, ...)                                                     \
  DebugPrint(                                                                  \
      DEBUG_WARN, LOG_COLOR_INFO "[INFO] " fmt LOG_COLOR_RESET "\n",           \
      ##__VA_ARGS__)
#define log_warn(fmt, ...)                                                     \
  DebugPrint(                                                                  \
      DEBUG_WARN, LOG_COLOR_WARN "[WARN] " fmt LOG_COLOR_RESET "\n",           \
      ##__VA_ARGS__)
#define log_err(fmt, ...)                                                      \
  DebugPrint(                                                                  \
      DEBUG_ERROR,                                                             \
      LOG_COLOR_ERROR "[ERROR] " fmt " (in " CR_LOG_CHAR8_STR_FMT ":%d)" LOG_COLOR_RESET "\n",       \
      ##__VA_ARGS__, __FILE__, __LINE__)

#endif // _KERNEL_MODE