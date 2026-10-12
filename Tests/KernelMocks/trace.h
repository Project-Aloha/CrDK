/* Host-only WPP call shape; deliberately no printf format reinterpretation.
   SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
static inline VOID OskTestLog(CONST CHAR *format, ...) { (VOID)format; }
#define log_raw(...) OskTestLog(__VA_ARGS__)
#define log_info(...) OskTestLog(__VA_ARGS__)
#define log_warn(...) OskTestLog(__VA_ARGS__)
#define log_err(...) OskTestLog(__VA_ARGS__)
#define TRACE_OSKAL 1
#define TRACE_FUNCTION_ENTRY(flags) ((void)(flags))
#define TRACE_FUNCTION_EXIT(flags) ((void)(flags))
