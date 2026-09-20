/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_lock.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>

/* PMIC Arbiter configuration registers */
#define SPMI_PMIC_ARB_REG_VERSION 0x0000
#define SPMI_PMIC_ARB_INT_EN 0x0004
#define SPMI_PMIC_ARB_FEATURES 0x0004

#define SPMI_PMIC_ARB_FEATURES_PERIPH_MASK GEN_MSK(10, 0)
#define SPMI_PMIC_ARB_FEATURES_V8_PERIPH_MASK GEN_MSK(12, 0)

/* PMIC Arbiter channel registers */
#define SPMI_PMIC_ARB_CMD 0x00
#define SPMI_PMIC_ARB_CONFIG 0x04
#define SPMI_PMIC_ARB_STATUS 0x08
#define SPMI_PMIC_ARB_WDATA0 0x10
#define SPMI_PMIC_ARB_WDATA1 0x14
#define SPMI_PMIC_ARB_RDATA0 0x18
#define SPMI_PMIC_ARB_RDATA1 0x1C

/* Mapping table (v1) */
#define SPMI_MAPPING_TABLE_REG(N) (0x0B00 + (4 * (N)))
#define SPMI_MAPPING_BIT_INDEX(X) (((X) >> 18) & 0xF)
#define SPMI_MAPPING_BIT_IS_0_FLAG(X) (((X) >> 17) & 0x1)
#define SPMI_MAPPING_BIT_IS_0_RESULT(X) (((X) >> 9) & 0xFF)
#define SPMI_MAPPING_BIT_IS_1_FLAG(X) (((X) >> 8) & 0x1)
#define SPMI_MAPPING_BIT_IS_1_RESULT(X) (((X) >> 0) & 0xFF)
#define SPMI_MAPPING_TABLE_TREE_DEPTH 16

/* Ownership table */
#define SPMI_OWNERSHIP_PERIPH2OWNER(X) ((X) & 0x7)

/* Version limits */
#define SPMI_PMIC_ARB_MAX_BUSES 4
#define SPMI_PMIC_ARB_MAX_PPID BIT(13)
#define SPMI_PMIC_ARB_MAX_PERIPHS 512
#define SPMI_PMIC_ARB_MAX_PERIPHS_V7 1024
#define SPMI_PMIC_ARB_MAX_PERIPHS_V8 8192
#define SPMI_PMIC_ARB_TIMEOUT_US 1000
#define SPMI_PMIC_ARB_MAX_TRANS_BYTES 8

#define SPMI_PMIC_ARB_APID_VALID BIT(15)
#define SPMI_PMIC_ARB_CHAN_IS_IRQ_OWNER_MASK BIT(24)
#define SPMI_PMIC_ARB_V8_CHAN_IS_IRQ_OWNER_MASK BIT(31)
#define SPMI_PMIC_ARB_APID_MASK 0xFF
#define SPMI_PMIC_ARB_PPID_MASK GEN_MSK(11, 0)
#define SPMI_PMIC_ARB_V8_PPID_MASK GEN_MSK(12, 0)
#define SPMI_PMIC_ARB_PPID_SHIFT_V5 8
#define SPMI_PMIC_ARB_PPID_SHIFT_V8 0
#define SPMI_PMIC_ARB_INVALID_EE 0xFF

/* SPMI command opcodes (subset used by the PMIC arbiter) */
#define SPMI_CMD_EXT_WRITE 0x00
#define SPMI_CMD_EXT_READ 0x20
#define SPMI_CMD_EXT_WRITEL 0x30
#define SPMI_CMD_EXT_READL 0x38
#define SPMI_CMD_WRITE 0x40
#define SPMI_CMD_READ 0x60
#define SPMI_CMD_ZERO_WRITE 0x80
#define SPMI_CMD_RESET 0x10
#define SPMI_CMD_SLEEP 0x11
#define SPMI_CMD_SHUTDOWN 0x12
#define SPMI_CMD_WAKEUP 0x13

/* QPNPINT (Qualcomm PMIC interrupt block) registers */
#define SPMI_QPNPINT_REG_RT_STS 0x10
#define SPMI_QPNPINT_REG_SET_TYPE 0x11
#define SPMI_QPNPINT_REG_POLARITY_HIGH 0x12
#define SPMI_QPNPINT_REG_POLARITY_LOW 0x13
#define SPMI_QPNPINT_REG_LATCHED_CLR 0x14
#define SPMI_QPNPINT_REG_EN_SET 0x15
#define SPMI_QPNPINT_REG_EN_CLR 0x16
#define SPMI_QPNPINT_REG_LATCHED_STS 0x18

#define SPMI_PIC_ACC_ENABLE_BIT BIT(0)
#define SPMI_PMIC_ARB_MAX_IRQ_ENTRIES 64

typedef enum {
  SPMI_ARB_VERSION_UNK = 0,
  SPMI_ARB_VERSION_1,
  SPMI_ARB_VERSION_2,
  SPMI_ARB_VERSION_3,
  SPMI_ARB_VERSION_5,
  SPMI_ARB_VERSION_7,
  SPMI_ARB_VERSION_8,
  SPMI_ARB_VERSION_8P5,
  SPMI_ARB_VERSION_MAX
} SPMI_ARB_VERSION;

typedef enum {
  SPMI_MEMORY_REGION_TYPE_CORE = 0,
  SPMI_MEMORY_REGION_TYPE_CH_SLAVES,
  SPMI_MEMORY_REGION_TYPE_OBSERVER,
  SPMI_MEMORY_REGION_TYPE_INTERRUPT,
  SPMI_MEMORY_REGION_TYPE_CONFIG,
  SPMI_MEMORY_REGION_TYPE_CH_MAP,
  SPMI_MEMORY_REGION_TYPE_CH_OWNER,
  SPMI_MEMORY_REGION_TYPE_MAX
} SPMI_MEMORY_REGION_TYPE;

typedef enum {
  SPMI_ARB_CHANNEL_RW = 0,
  SPMI_ARB_CHANNEL_OBS
} SPMI_ARB_CHANNEL_TYPE;

typedef struct {
  UINTN BaseAddress;
  UINTN Size;
} SpmiMemoryRegion;

typedef struct SpmiDeviceContext SpmiDeviceContext;

typedef UINT32 (*SPMI_MMIO_READ32)(IN VOID *Context, IN UINTN Address);
typedef VOID (*SPMI_MMIO_WRITE32)(IN VOID *Context, IN UINTN Address,
                                  IN UINT32 Value);
typedef VOID (*SPMI_STALL_US)(IN VOID *Context, IN UINT32 Microseconds);

typedef struct {
  SPMI_MMIO_READ32 Read32;
  SPMI_MMIO_WRITE32 Write32;
  SPMI_STALL_US StallUs;
  VOID *Context;
} SpmiIoOps;

typedef struct {
  UINT8 WriteEe;
  UINT8 IrqEe;
  UINT16 Ppid;
} SpmiApidData;

typedef struct SpmiPmicArbOps SpmiPmicArbOps;

typedef struct {
  UINTN ReadAddr;
  UINTN WriteAddr;
  UINTN ApidMapAddr;
  UINTN CoreAddr;
  UINT32 CoreSize;
  UINT32 MaxPeriphs;
  UINT32 BusAvailable;
  CONST SpmiPmicArbOps *Ops;
} SpmiPmicArb;

struct SpmiPmicArbOps {
  CR_STATUS (*GetCoreResource)(SpmiDeviceContext *Ctx);
  CR_STATUS (*InitApid)(SpmiDeviceContext *Ctx, UINT32 Index);
  CR_STATUS (*PpidToApid)(SpmiDeviceContext *Ctx, UINT16 Ppid, UINT16 *Apid);
  UINT32 (*GetChannelOffset)(SpmiDeviceContext *Ctx, UINT8 Sid, UINT16 Addr,
                             SPMI_ARB_CHANNEL_TYPE ChannelType);
  UINT32 (*FormatCmd)(UINT8 Opc, UINT8 Sid, UINT16 Addr, UINT8 Bc);
  CR_STATUS (*CheckChannelStatus)(SpmiDeviceContext *Ctx, UINT32 Status,
                                  UINT8 Sid, UINT16 Addr, UINT32 Offset);
  UINTN (*GetOwnerAccStatus)(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index);
  UINTN (*GetAccEnable)(SpmiDeviceContext *Ctx, UINT16 Apid);
  UINTN (*GetIrqStatus)(SpmiDeviceContext *Ctx, UINT16 Apid);
  UINTN (*GetIrqClear)(SpmiDeviceContext *Ctx, UINT16 Apid);
  UINTN (*GetApidOwner)(SpmiDeviceContext *Ctx, UINT16 Apid);
  UINT32 (*GetApidMapOffset)(UINT16 Apid);
};

typedef struct {
  UINTN InterruptAddr;
  UINTN ConfigAddr;
  UINTN ApidOwnerAddr;
  UINT16 BaseApid;
  UINT32 ApidCount;
  UINT16 PpidToApid[SPMI_PMIC_ARB_MAX_PPID];
  SpmiApidData ApidData[SPMI_PMIC_ARB_MAX_PERIPHS_V8];
  UINT16 LastApid;
  UINT16 MinApid;
  UINT16 MaxApid;
  UINT8 Id;
  BOOLEAN ApidMapValid;
  CR_LOCK Lock;
  CR_LOCK IrqLock;
} SpmiPmicArbBus;

typedef struct {
  UINT8 Sid;
  UINT8 Periph;
  UINT8 Irq;
  UINT16 Apid;
  CR_INTERRUPT_TRIGGER_TYPE TriggerType;
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  BOOLEAN Enabled;
} SpmiIrqEntry;

struct SpmiDeviceContext {
  SPMI_ARB_VERSION Version;
  SpmiMemoryRegion Regions[SPMI_MEMORY_REGION_TYPE_MAX];
  UINT32 PdcPinNumber;
  UINT8 ActiveEE;
  UINT8 Channel;
  UINT8 BusId;
  CR_INTERRUPT_CONFIG InterruptConfig;
  SpmiIoOps Io;
  SpmiPmicArb PmicArb;
  SpmiPmicArbBus Bus;
  SpmiIrqEntry IrqEntries[SPMI_PMIC_ARB_MAX_IRQ_ENTRIES];
  BOOLEAN HasInterrupt;
  BOOLEAN InterruptExternallyManaged;
  BOOLEAN Initialized;
  BOOLEAN InterruptRegistered;
};

/**
 * Initialize the SPMI PMIC arbiter. If *Ctx is NULL the context is
 * obtained from the target library.
 */
CR_STATUS
SpmiLibInit(IN OUT SpmiDeviceContext **Ctx);

/**
 * Stop interrupt delivery and release all runtime state owned by the SPMI
 * controller. The target-supplied resource description remains reusable by a
 * subsequent SpmiLibInit call.
 */
CR_STATUS
SpmiLibDeinit(IN OUT SpmiDeviceContext *Ctx);

/**
 * Read 1..8 bytes from a SPMI peripheral.
 */
CR_STATUS
SpmiRead(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
         OUT UINT8 *Buf, IN UINTN Len);

/**
 * Write 1..8 bytes to a SPMI peripheral.
 */
CR_STATUS
SpmiWrite(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
          IN CONST UINT8 *Buf, IN UINTN Len);

/**
 * Read-modify-write a SPMI peripheral register. Bits with Mask[i] == 1
 * are replaced by Buf[i].
 */
CR_STATUS
SpmiMaskedWrite(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT16 Addr,
                IN CONST UINT8 *Buf, IN CONST UINT8 *Mask, IN UINTN Len);

/**
 * Send a non-data command (reset/sleep/shutdown/wakeup) to a slave.
 */
CR_STATUS
SpmiCommand(IN SpmiDeviceContext *Ctx, IN UINT8 Opc, IN UINT8 Sid);

/**
 * Register an interrupt handler for a QPNPINT capable peripheral.
 */
CR_STATUS
SpmiRegisterIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
                IN UINT8 Irq, IN CR_INTERRUPT_TRIGGER_TYPE TriggerType,
                IN CR_INTERRUPT_HANDLER Handler, IN VOID *Param);

/**
 * Unregister an interrupt handler.
 */
CR_STATUS
SpmiUnregisterIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
                  IN UINT8 Irq);

/**
 * Enable or disable a peripheral interrupt at the PMIC and PIC level.
 */
CR_STATUS
SpmiEnableIrq(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
              IN UINT8 Irq, IN BOOLEAN Enable);

/**
 * Drain all pending PMIC-arbiter interrupts and invoke registered handlers.
 * This is used both by the controller ISR and by an external interrupt owner
 * such as the Windows GPIO class extension.
 */
CR_STATUS
SpmiDispatchIrqs(IN SpmiDeviceContext *Ctx);

/**
 * Translate a slave/peripheral id pair (PPID) to an APID.
 */
CR_STATUS
SpmiGetApid(IN SpmiDeviceContext *Ctx, IN UINT8 Sid, IN UINT8 Periph,
            OUT UINT16 *Apid);
