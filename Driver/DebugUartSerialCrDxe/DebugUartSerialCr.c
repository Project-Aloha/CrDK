#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Protocol/SerialIo.h>

#include <Library/DevicePathLib.h>
#include <Library/debug_uart.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_DEBUG_SERIAL_PORT_DEVICE_GUID                                       \
  {0x13F2452F, 0x1948, 0x46A0, {0xB4, 0x0D, 0x2D, 0xF0, 0x05, 0x48, 0x14, 0x06}}

STATIC CrDebugUartContext *mDebugUartContext                    = NULL;
STATIC EFI_HANDLE          mSerialHandle                        = NULL;
STATIC UINT8               mInputBuf[DEBUG_UART_INPUT_BUF_SIZE] = {0};

struct SERIAL_DEVICE_PATH {
  VENDOR_DEVICE_PATH       VendorDevicePath;
  UART_DEVICE_PATH         UartDevicePath;
  EFI_DEVICE_PATH_PROTOCOL End;
};

STATIC struct SERIAL_DEVICE_PATH mCrSerialDevicePath = {
    .VendorDevicePath =
        {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
         CR_DEBUG_SERIAL_PORT_DEVICE_GUID},
    .UartDevicePath =
        {
            {MESSAGING_DEVICE_PATH, MSG_UART_DP, {sizeof(UART_DEVICE_PATH), 0}},
            0, // Reserved
            0, // BaudRate
            0, // DataBits
            0, // Parity
            0  // StopBits
        },
    .End = {
        END_DEVICE_PATH_TYPE,
        END_ENTIRE_DEVICE_PATH_SUBTYPE,
        {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}};

STATIC
EFI_STATUS
EFIAPI
SerialReset(IN EFI_SERIAL_IO_PROTOCOL *This) { return EFI_SUCCESS; }

STATIC
EFI_STATUS
EFIAPI
SerialSetAttributes(
    IN EFI_SERIAL_IO_PROTOCOL *This, IN UINT64 BaudRate,
    IN UINT32 ReceiveFifoDepth, IN UINT32 Timeout, IN EFI_PARITY_TYPE Parity,
    IN UINT8 DataBits, IN EFI_STOP_BITS_TYPE StopBits)
{
  (VOID) This;
  (VOID) BaudRate;
  (VOID) ReceiveFifoDepth;
  (VOID) Timeout;
  (VOID) Parity;
  (VOID) DataBits;
  (VOID) StopBits;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialSetControl(IN EFI_SERIAL_IO_PROTOCOL *This, IN UINT32 Control)
{
  (VOID) This;
  (VOID) Control;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialGetControl(IN EFI_SERIAL_IO_PROTOCOL *This, OUT UINT32 *Control)
{
  if (Control != NULL)
    *Control = 0;
  (VOID) This;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialWrite(
    IN EFI_SERIAL_IO_PROTOCOL *This, IN OUT UINTN *BufferSize, IN VOID *Buffer)
{
  (VOID) This;
  if (Buffer == NULL || BufferSize == NULL)
    return EFI_INVALID_PARAMETER;
  if (mDebugUartContext == NULL)
    return EFI_DEVICE_ERROR;

  MsmGeniWriteTxFifo(mDebugUartContext, Buffer, *BufferSize);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
SerialRead(
    IN EFI_SERIAL_IO_PROTOCOL *This, IN OUT UINTN *BufferSize, OUT VOID *Buffer)
{
  (VOID) This;
  if (BufferSize == NULL || Buffer == NULL)
    goto exit;

  if (mDebugUartContext == NULL || mDebugUartContext->InputBuffer == NULL || *BufferSize == 0) {
    *BufferSize = 0;
    goto exit;
  }

  UINTN Available = (UINTN)mDebugUartContext->InputBufferSize;
  if (Available == 0) {
    *BufferSize = 0;
    goto exit;
  }

  UINTN ToRead = (*BufferSize < Available) ? *BufferSize : Available;
  for (UINTN i = 0; i < ToRead; i++) {
    ((UINT8 *)Buffer)[i] =
        ((UINT8 *)mDebugUartContext
             ->InputBuffer)[mDebugUartContext->InputBufferHead];
    mDebugUartContext->InputBufferHead++;
    if (mDebugUartContext->InputBufferHead >=
        mDebugUartContext->InputBufferCapacity)
      mDebugUartContext->InputBufferHead = 0;
  }

  mDebugUartContext->InputBufferSize = (UINT32)(Available - ToRead);
  *BufferSize                        = ToRead;

exit:
  return EFI_SUCCESS;
}

EFI_SERIAL_IO_MODE mCrSerialIoMode = {
    .ControlMask      = 0,
    .Timeout          = 1000000,
    .BaudRate         = 0,
    .ReceiveFifoDepth = 1,
    .DataBits         = 0,
    .Parity           = 0,
    .StopBits         = 0,
};

STATIC EFI_SERIAL_IO_PROTOCOL mSerialIo = {
    SERIAL_IO_INTERFACE_REVISION,
    SerialReset,
    SerialSetAttributes,
    SerialSetControl,
    SerialGetControl,
    SerialWrite,
    SerialRead,
    &mCrSerialIoMode,
};

CR_STATUS
DriverEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  CR_STATUS Status = CR_SUCCESS;

  // Initialize debug uart library
  Status = CrDebugUartLibInit(&mDebugUartContext);
  if (Status != CR_SUCCESS) {
    log_err("Failed to init debug uart library: %d\n", Status);
    return Status;
  }

  mDebugUartContext->InputBuffer         = mInputBuf;
  mDebugUartContext->InputBufferSize     = 0;
  mDebugUartContext->InputBufferHead     = 0;
  mDebugUartContext->InputBufferTail     = 0;
  mDebugUartContext->InputBufferCapacity = DEBUG_UART_INPUT_BUF_SIZE;

  mCrSerialIoMode.BaudRate         = mDebugUartContext->BaudRate;
  mCrSerialIoMode.DataBits         = mDebugUartContext->DataBits;
  mCrSerialIoMode.Parity           = mDebugUartContext->Parity;
  mCrSerialIoMode.StopBits         = mDebugUartContext->StopBits;
  mCrSerialIoMode.ReceiveFifoDepth = mDebugUartContext->RxFifoDepth;

  mCrSerialDevicePath.UartDevicePath.BaudRate = mDebugUartContext->BaudRate;
  mCrSerialDevicePath.UartDevicePath.DataBits = mDebugUartContext->DataBits;
  mCrSerialDevicePath.UartDevicePath.Parity   = mDebugUartContext->Parity;
  mCrSerialDevicePath.UartDevicePath.StopBits = mDebugUartContext->StopBits;

  // Install Serial IO protocol
  Status = gBS->InstallMultipleProtocolInterfaces(
      &mSerialHandle, &gEfiSerialIoProtocolGuid, &mSerialIo,
      &gEfiDevicePathProtocolGuid, &mCrSerialDevicePath, NULL);

  if (CR_ERROR(Status)) {
    log_err("Failed to install Debug Uart Serial IO protocol: 0x%X", Status);
    return Status;
  }

  return CR_SUCCESS;
}
