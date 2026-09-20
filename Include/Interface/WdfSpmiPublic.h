/** @file
 *  Kernel query-interface for Crane SPMI clients.
 *
 *  Windows has no public SPMI class extension. This interface is confined to
 *  kernel-mode consumers and preserves the OS-independent SpmiLib contract.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/spmi.h>
#include <wdm.h>

#define WDF_SPMI_CR_INTERFACE_REVISION 0x2U

DEFINE_GUID(GUID_DEVINTERFACE_SPMI_CR, 0xbab3ba01, 0x3a80, 0x46cd, 0xb7, 0x34,
            0x55, 0x29, 0x01, 0x1b, 0xd2, 0x12);
DEFINE_GUID(GUID_SPMI_CR_INTERFACE, 0xcd106317, 0x0b75, 0x4592, 0x90, 0x20,
            0x93, 0xbc, 0x7b, 0xae, 0x9b, 0xed);

typedef NTSTATUS (*SPMI_CR_READ)(IN VOID *Context, IN UINT8 Sid,
                                 IN UINT16 Address, OUT UINT8 *Buffer,
                                 IN UINTN Length);
typedef NTSTATUS (*SPMI_CR_WRITE)(IN VOID *Context, IN UINT8 Sid,
                                  IN UINT16 Address, IN CONST UINT8 *Buffer,
                                  IN UINTN Length);
typedef NTSTATUS (*SPMI_CR_MASKED_WRITE)(IN VOID *Context, IN UINT8 Sid,
                                         IN UINT16 Address,
                                         IN CONST UINT8 *Buffer,
                                         IN CONST UINT8 *Mask, IN UINTN Length);
typedef NTSTATUS (*SPMI_CR_COMMAND)(IN VOID *Context, IN UINT8 Opcode,
                                    IN UINT8 Sid);
typedef NTSTATUS (*SPMI_CR_REGISTER_IRQ)(IN VOID *Context, IN UINT8 Sid,
                                         IN UINT8 Peripheral, IN UINT8 Irq,
                                         IN CR_INTERRUPT_TRIGGER_TYPE Trigger,
                                         IN CR_INTERRUPT_HANDLER Handler,
                                         IN VOID *HandlerContext);
typedef NTSTATUS (*SPMI_CR_UNREGISTER_IRQ)(IN VOID *Context, IN UINT8 Sid,
                                           IN UINT8 Peripheral, IN UINT8 Irq);
typedef NTSTATUS (*SPMI_CR_ENABLE_IRQ)(IN VOID *Context, IN UINT8 Sid,
                                       IN UINT8 Peripheral, IN UINT8 Irq,
                                       IN BOOLEAN Enable);
typedef NTSTATUS (*SPMI_CR_GET_INFO)(IN VOID *Context,
                                     OUT SPMI_ARB_VERSION *Version,
                                     OUT UINT8 *ActiveEe, OUT UINT8 *BusId);
typedef NTSTATUS (*SPMI_CR_DISPATCH_IRQS)(IN VOID *Context);

typedef struct _WDF_SPMI_CR_INTERFACE {
  INTERFACE Header;
  SPMI_CR_READ Read;
  SPMI_CR_WRITE Write;
  SPMI_CR_MASKED_WRITE MaskedWrite;
  SPMI_CR_COMMAND Command;
  SPMI_CR_REGISTER_IRQ RegisterIrq;
  SPMI_CR_UNREGISTER_IRQ UnregisterIrq;
  SPMI_CR_ENABLE_IRQ EnableIrq;
  SPMI_CR_GET_INFO GetInfo;
  SPMI_CR_DISPATCH_IRQS DispatchIrqs;
} WDF_SPMI_CR_INTERFACE;
