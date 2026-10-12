/** SM8450 DWC3 register/ownership contract, from Linux dwc3/core.h.
    SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>
#include <Interface/CrUsbHandoff.h>
#include <Protocol/IoMmu.h>
#include <Protocol/UsbFunctionIo.h>
#include <Protocol/EFIUsbTypeCCrProtocol.h>

#define CR_USB_BASE 0x0A600000U
#define CR_USB_QSCRATCH (CR_USB_BASE + 0xF8800)
#define USB_REG(Offset) (CR_USB_BASE + (Offset))
#define USB_GCTL USB_REG(0xC110)
#define USB_DCFG USB_REG(0xC700)
#define USB_DCTL USB_REG(0xC704)
#define USB_DEVTEN USB_REG(0xC708)
#define USB_DSTS USB_REG(0xC70C)
#define USB_DALEPENA USB_REG(0xC720)
#define USB_EP(Endpoint) USB_REG(0xC800 + (Endpoint) * 16)
#define USB_EVENT_SIZE 4096U
#define USB_MAX_TRANSFER 65536U
#define USB_MAX_EP 32U

typedef struct {
  VOID *Cpu;
  EFI_PHYSICAL_ADDRESS Device;
  VOID *Mapping;
  UINTN Pages;
} CR_USB_DMA;

EFI_STATUS CrUsbWait(UINTN Address, UINT32 Mask, UINT32 Expected, UINT32 TimeoutUs);
EFI_STATUS CrUsbCoreInit(EFI_USB_TYPEC_DATA_ROLE Role, BOOLEAN SuperSpeed);
EFI_STATUS CrUsbCoreHalt(EFI_USB_TYPEC_DATA_ROLE Role);
EFI_STATUS CrUsbCoreRetire(EFI_USB_TYPEC_DATA_ROLE Role,CR_USB_CONTROLLER_STOP *Stop,EFI_STATUS *Halt,EFI_STATUS *Interrupt);
EFI_STATUS CrUsbDmaAllocate(CR_USB_DMA *Dma, UINTN Size);
EFI_STATUS CrUsbDmaFree(CR_USB_DMA *Dma);
VOID CrUsbFnInit(EDKII_IOMMU_PROTOCOL *Iommu);
VOID CrUsbFnSetAvailable(BOOLEAN Available);
EFI_STATUS CrUsbFnPoll(VOID);
EFI_STATUS CrUsbFnStop(VOID);
/* One-way BSP/TPL_NOTIFY software close. No halt, DMA unmap or free. */
EFI_STATUS CrUsbFnFreeze(UINT32 *ActiveCalls);
BOOLEAN CrUsbFnCanUnload(VOID);
extern EFI_USBFN_IO_PROTOCOL gCrUsbFunctionIo;
