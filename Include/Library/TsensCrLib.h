/* Source-qualified SM8450 TSENS v2 temperature reader. SPDX-License-Identifier: MIT */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#define CR_TSENS_BANKS 2U
#define CR_TSENS_SENSORS 16U
#define CR_TSENS_WINDOW_SIZE 0x1000U
#define CR_TSENS_SROT_VERSION 0U
#define CR_TSENS_SROT_CONTROL 4U
#define CR_TSENS_TM_SENSOR_STATUS 0xa0U
#define CR_TSENS_VALID (1U<<21)
typedef enum {CrTsensTm,CrTsensSrot} CR_TSENS_WINDOW;
typedef struct {
  VOID *Context;
  CR_STATUS (*Read32)(VOID *,UINT32 Bank,CR_TSENS_WINDOW,UINT32 Offset,UINT32 *);
} CR_TSENS_IO;
typedef struct {
  UINT32 SoC,Board,Bank;
  UINT64 TmBase,SrotBase;
  UINT32 TmLength,SrotLength,UpperLowerIrq,CriticalIrq;
} CR_TSENS_DESCRIPTOR;
typedef struct {
  CR_TSENS_IO Io;
  UINT32 Version[CR_TSENS_BANKS],Control[CR_TSENS_BANKS];
  UINT16 SensorMask[CR_TSENS_BANKS];
  BOOLEAN Initialized;
} CR_TSENS;
typedef struct {
  INT32 MilliCelsius;
  UINT32 RawStatus,HardwareVersion;
  UINT16 SensorMask;
  UINT8 ReadCount;
  BOOLEAN ValidSample,StableFallback;
} CR_TSENS_SAMPLE;
/* Descriptor qualification is pure. Initialize reads SROT only, never writes,
 * calibrates, enables or assumes a disabled/ADC-format bank is temperature.
 * The owner serializes all methods and retains actual mappings while callable.
 * Every failing operation leaves its output unchanged. */
CR_STATUS CrTsensQualify(CONST CR_TSENS_DESCRIPTOR *,UINT32 Count);
CR_STATUS CrTsensInitialize(CR_TSENS *,CONST CR_TSENS_IO *,CONST CR_TSENS_DESCRIPTOR *,UINT32 Count);
CR_STATUS CrTsensRead(CR_TSENS *,UINT32 Bank,UINT32 Sensor,CR_TSENS_SAMPLE *);
VOID CrTsensClear(CR_TSENS *);
