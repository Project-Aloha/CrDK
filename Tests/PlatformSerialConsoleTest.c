/** @file Native UART console path and SerialIo receive integration regression.
  SPDX-License-Identifier: MIT
**/
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Uefi.h>

VOID *EFIAPI ZeroMem (VOID *P, UINTN N) { return memset(P,0,N); }
static BOOLEAN SerialConsoleEnabled, DpConsoleEnabled;
#define _PCD_VALUE_PcdPlatformDisplayPortConsoleEnable DpConsoleEnabled
#define _PCD_VALUE_PcdPlatformSerialConsoleEnable SerialConsoleEnabled
#define _PCD_VALUE_PcdUartSerialBase 0
#define _PCD_VALUE_PcdUartDefaultBaudRate 115200
#define _PCD_GET_MODE_64_PcdUartDefaultBaudRate 115200
#define _PCD_GET_MODE_8_PcdUartDefaultDataBits 8
#define _PCD_GET_MODE_8_PcdUartDefaultParity NoParity
#define _PCD_GET_MODE_8_PcdUartDefaultStopBits OneStopBit
#define _PCD_GET_MODE_16_PcdUartDefaultReceiveFifoDepth 1
#define _PCD_GET_MODE_32_PcdMaximumDevicePathNodeCount 0

#include "../../MUCommonCrPkg/Library/QcomGeniSerialPortLib/GeniSerial.h"
static UINT32 Registers[0x1000 / sizeof (UINT32)];
static UINT32 RxWord, RxWords, RxPartial;
static BOOLEAN RxLast, SerialInstalled;
static UINT32 LocateCalls, HandleBuffers, Stalls;
static EFI_HANDLE *LocatedHandles;

static UINT32 TestRead (UINT32 Group, UINT32 Offset, UINT32 Mask) {
  UINT32 Address = Group + Offset;
  assert (Address < sizeof (Registers));
  if (Address == GENI4_DATA + GENI_RX_FIFO_STATUS_ADDR)
    return (RxWords | (RxPartial << RX_LAST_VALID_BYTES_SHIFT) |
            (RxLast ? RX_LAST : 0)) & Mask;
  if (Address == GENI4_DATA + GENI_RX_FIFO_ADDR) {
    assert (RxWords == 1);
    RxWords = 0;
    return RxWord & Mask;
  }
  return Registers[Address / sizeof (UINT32)] & Mask;
}

static VOID TestWrite (UINT32 Group, UINT32 Offset, UINT32 Value) {
  UINT32 Address = Group + Offset;
  assert (Address < sizeof (Registers));
  if (Address == GENI4_DATA + GENI_S_IRQ_CLEAR_ADDR)
    Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / sizeof (UINT32)] &= ~Value;
  else if (Address == GENI4_DATA + GENI_M_IRQ_CLEAR_ADDR)
    Registers[(GENI4_DATA + GENI_M_IRQ_STATUS_ADDR) / sizeof (UINT32)] &= ~Value;
  else
    Registers[Address / sizeof (UINT32)] = Value;
}

#undef MemRead
#undef MemWrite
#define MemRead(Group, Offset, Mask) TestRead ((Group), (Offset), (Mask))
#define MemWrite(Group, Offset, Value) TestWrite ((Group), (Offset), (Value))
#include "../../MUCommonCrPkg/Library/QcomGeniSerialPortLib/QcomGeniSerialPortLib.c"
#include "../Override/MU_BASECORE/MdeModulePkg/Universal/SerialDxe/SerialIo.c"
#include "../../MUCommonCrPkg/Library/MsPlatformDevicesLib/MsPlatformDevicesLib.c"
#include "../../../MU_BASECORE/MdePkg/Library/UefiDevicePathLib/DevicePathUtilities.c"
#include "../../../MU_BASECORE/MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.c"
#include "../../../MU_BASECORE/MdePkg/Library/UefiDevicePathLib/DevicePathUtilitiesDxeSmm.c"

EFI_GUID gEfiSerialIoProtocolGuid = EFI_SERIAL_IO_PROTOCOL_GUID;
EFI_GUID gEfiDevicePathProtocolGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
EFI_GUID gEfiTtyTermGuid = EFI_TTY_TERM_GUID;
EFI_GUID gEfiGraphicsOutputProtocolGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_BOOT_SERVICES BootServices;
EFI_BOOT_SERVICES *gBS = &BootServices;
static BOOLEAN GraphicsPathInstalled, PublicGopInstalled, PartialGraphicsPath;
static BOOLEAN GraphicsHasGopChild, GraphicsSuppressGop;
static EFI_STATUS GraphicsConnectStatus;
static UINT32 GraphicsConnectCalls, GopChildPathCalls;
static UINT8 GraphicsController;
static UINT8 GraphicsGop;

UINTN EFIAPI MicroSecondDelay (UINTN Delay) { return Delay; }
VOID *EFIAPI CopyMem (VOID *Destination, CONST VOID *Source, UINTN Length) {
  return memcpy (Destination, Source, Length);
}
EFI_GUID *EFIAPI CopyGuid (EFI_GUID *Destination, CONST EFI_GUID *Source) {
  return memcpy (Destination, Source, sizeof (*Destination));
}
UINT16 EFIAPI ReadUnaligned16 (CONST UINT16 *Buffer) {
  UINT16 Value;
  memcpy (&Value, Buffer, sizeof (Value));
  return Value;
}
UINT16 EFIAPI WriteUnaligned16 (UINT16 *Buffer, UINT16 Value) {
  memcpy (Buffer, &Value, sizeof (Value));
  return Value;
}
VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
VOID *EFIAPI AllocateCopyPool (UINTN Size, CONST VOID *Source) {
  VOID *Buffer = malloc (Size);
  if (Buffer != NULL) memcpy (Buffer, Source, Size);
  return Buffer;
}
VOID EFIAPI FreePool (VOID *Buffer) {
  if (Buffer == LocatedHandles) {
    assert (HandleBuffers == 1);
    --HandleBuffers;
    LocatedHandles = NULL;
  }
  free (Buffer);
}

static EFI_STATUS EFIAPI TestInstall (EFI_HANDLE *Handle, ...) {
  va_list Args;
  va_start (Args, Handle);
  assert (va_arg (Args, EFI_GUID *) == &gEfiSerialIoProtocolGuid);
  assert (va_arg (Args, VOID *) == &mSerialIoTemplate);
  assert (va_arg (Args, EFI_GUID *) == &gEfiDevicePathProtocolGuid);
  assert (va_arg (Args, VOID *) == &mSerialDevicePath);
  assert (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  SerialInstalled = TRUE;
  *Handle = (EFI_HANDLE)&SerialInstalled;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestLocate (EFI_LOCATE_SEARCH_TYPE SearchType,
  EFI_GUID *Protocol, VOID *SearchKey, UINTN *Count, EFI_HANDLE **Handles) {
  assert (SearchType == ByProtocol && Protocol == &gEfiSerialIoProtocolGuid);
  ++LocateCalls;
  if (!SerialInstalled) return EFI_NOT_FOUND;
  *Count = 1;
  *Handles = malloc (sizeof (**Handles));
  assert (*Handles != NULL);
  assert (LocatedHandles == NULL);
  LocatedHandles = *Handles;
  (*Handles)[0] = mSerialHandle;
  ++HandleBuffers;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestHandle (EFI_HANDLE Handle, EFI_GUID *Protocol,
                                    VOID **Interface) {
  if (Handle == &GraphicsController) {
    if (Protocol == &gEfiDevicePathProtocolGuid) {
      *Interface = &DisplayDevicePath;
      return EFI_SUCCESS;
    }
    assert (Protocol == &gEfiGraphicsOutputProtocolGuid);
    if (!PublicGopInstalled || GraphicsHasGopChild) return EFI_NOT_FOUND;
    *Interface = &GraphicsGop;
    return EFI_SUCCESS;
  }
  assert (SerialInstalled && Handle == mSerialHandle);
  assert (Protocol == &gEfiDevicePathProtocolGuid);
  *Interface = &mSerialDevicePath;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI TestStall (UINTN Delay) {
  assert (Delay == 10);
  ++Stalls;
  return EFI_SUCCESS;
}
static EFI_TPL EFIAPI TestRaiseTpl (EFI_TPL Tpl) { return TPL_APPLICATION; }
static VOID EFIAPI TestRestoreTpl (EFI_TPL Tpl) { }

static EFI_STATUS EFIAPI TestLocateGraphicsPath (EFI_GUID *Protocol,
  EFI_DEVICE_PATH_PROTOCOL **Path, EFI_HANDLE *Handle) {
  /* The override owns the GOP before connection, but the path survives. */
  assert (Protocol == &gEfiDevicePathProtocolGuid);
  assert (*Path == (VOID *)&DisplayDevicePath);
  if (!GraphicsPathInstalled) return EFI_NOT_FOUND;
  *Handle = &GraphicsController;
  if (!PartialGraphicsPath) *Path = &DisplayDevicePath.EndDevicePath;
  return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI TestConnectGraphics (EFI_HANDLE Handle,
  EFI_HANDLE *Drivers, EFI_DEVICE_PATH_PROTOCOL *RemainingPath, BOOLEAN Recursive) {
  assert (Handle == &GraphicsController && Drivers == NULL);
  assert (RemainingPath == NULL && Recursive);
  ++GraphicsConnectCalls;
  if (!EFI_ERROR (GraphicsConnectStatus)) PublicGopInstalled = !GraphicsSuppressGop;
  return GraphicsConnectStatus;
}

EFI_DEVICE_PATH_PROTOCOL *EFIAPI EfiBootManagerGetGopDevicePath (EFI_HANDLE Handle) {
  assert (Handle == &GraphicsController && GraphicsConnectCalls != 0);
  ++GopChildPathCalls;
  /* Like the production helper, return only GOP child paths. */
  if (!PublicGopInstalled || !GraphicsHasGopChild) return NULL;
  return AllocateCopyPool (sizeof (DisplayDevicePath), &DisplayDevicePath);
}

static VOID TestPreferredGraphicsConsole (VOID) {
  EFI_DEVICE_PATH_PROTOCOL *Path = (VOID *)1;
  BootServices.LocateDevicePath = TestLocateGraphicsPath;
  BootServices.ConnectController = TestConnectGraphics;
  assert (GetPlatformPreferredConsole (NULL) == NULL);
  assert (GetPlatformPreferredConsole (&Path) == NULL && Path == NULL);
  assert (GraphicsConnectCalls == 0);

  GraphicsPathInstalled = PartialGraphicsPath = TRUE;
  assert (GetPlatformPreferredConsole (&Path) == NULL && Path == NULL);
  assert (GraphicsConnectCalls == 0);
  PartialGraphicsPath = FALSE;
  GraphicsConnectStatus = EFI_DEVICE_ERROR;
  assert (GetPlatformPreferredConsole (&Path) == NULL && Path == NULL);
  assert (!PublicGopInstalled);

  GraphicsConnectStatus = EFI_SUCCESS;
  assert (GetPlatformPreferredConsole (&Path) == &GraphicsController);
  assert (PublicGopInstalled && Path != NULL && GopChildPathCalls == 0);
  assert (GetDevicePathSize (Path) == sizeof (DisplayDevicePath));
  assert (memcmp (Path, &DisplayDevicePath, sizeof (DisplayDevicePath)) == 0);
  FreePool (Path);

  /* Already managed controller: no new driver starts, existing GOP is valid. */
  GraphicsConnectStatus = EFI_NOT_FOUND;
  assert (GetPlatformPreferredConsole (&Path) == &GraphicsController && Path != NULL);
  FreePool (Path);
  PublicGopInstalled = FALSE;
  assert (GetPlatformPreferredConsole (&Path) == NULL && Path == NULL);
  PublicGopInstalled = TRUE;
  GraphicsConnectStatus = EFI_SUCCESS;

  UINT32 BeforeChildLookup = GopChildPathCalls;
  GraphicsHasGopChild = TRUE;
  assert (GetPlatformPreferredConsole (&Path) == &GraphicsController);
  assert (Path != NULL && GopChildPathCalls == BeforeChildLookup + 1);
  FreePool (Path);
  GraphicsSuppressGop = TRUE;
  Path = (VOID *)1;
  assert (GetPlatformPreferredConsole (&Path) == NULL && Path == NULL);
}

int main (void) {
  BDS_CONSOLE_CONNECT_ENTRY *Consoles;
  EFI_DEVICE_PATH_PROTOCOL *Node, *ConsolePath;
  EFI_GUID SerialGuid = EDKII_SERIAL_PORT_LIB_VENDOR_GUID;
  UINTN ParentLength, Size;
  UINT32 SavedLocateCalls;
  UINT8 Input[8];

  BootServices.InstallMultipleProtocolInterfaces = TestInstall;
  BootServices.LocateHandleBuffer = TestLocate;
  BootServices.HandleProtocol = TestHandle;
  BootServices.Stall = TestStall;
  BootServices.RaiseTPL = TestRaiseTpl;
  BootServices.RestoreTPL = TestRestoreTpl;

  /* Other devices retain the keypad path.  Early lookup may fail and must
     retry after SerialDxe publishes the native protocol. */
  Consoles = GetPlatformConsoleList ();
  assert (Consoles[0].DevicePath == (VOID *)&KeypadDevicePath);
  assert (LocateCalls == 0);
  SerialConsoleEnabled = TRUE;
  Consoles = GetPlatformConsoleList ();
  assert (mSerialConsoleDevicePath == NULL && LocateCalls == 1);

  Registers[(QUPV3_SE_DMA + SE_HW_PARAM_0_ADDR) / sizeof (UINT32)] = 32U << 16;
  Registers[(QUPV3_SE_DMA + SE_HW_PARAM_1_ADDR) / sizeof (UINT32)] = 32U << 16;
  assert (SerialDxeInitialize (NULL, NULL) == EFI_SUCCESS && SerialInstalled);
  assert (memcmp (&mSerialDevicePath.Guid.Guid, &SerialGuid, sizeof (SerialGuid)) == 0);
  assert (mSerialDevicePath.Uart.BaudRate == 115200);
  assert (mSerialDevicePath.Uart.DataBits == 8);
  assert (mSerialDevicePath.Uart.Parity == NoParity);
  assert (mSerialDevicePath.Uart.StopBits == OneStopBit);

  Consoles = GetPlatformConsoleList ();
  assert (Consoles[0].DevicePath == (VOID *)&KeypadDevicePath);
  assert (Consoles[0].ConnectType == CONSOLE_IN);
  ConsolePath = Consoles[2].DevicePath;
  assert (Consoles[2].ConnectType == CONSOLE_ALL && HandleBuffers == 0);
  ParentLength = GetDevicePathSize ((VOID *)&mSerialDevicePath) - END_DEVICE_PATH_LENGTH;
  assert (GetDevicePathSize (ConsolePath) == ParentLength + sizeof (VENDOR_DEVICE_PATH) + END_DEVICE_PATH_LENGTH);
  assert (memcmp (ConsolePath, &mSerialDevicePath, ParentLength) == 0);
  Node = (VOID *)((UINT8 *)ConsolePath + ParentLength);
  assert (DevicePathType (Node) == MESSAGING_DEVICE_PATH);
  assert (DevicePathSubType (Node) == MSG_VENDOR_DP);
  assert (memcmp (&((VENDOR_DEVICE_PATH *)Node)->Guid, &gEfiTtyTermGuid, sizeof (EFI_GUID)) == 0);
  assert (IsDevicePathEnd (NextDevicePathNode (Node)));
  SavedLocateCalls = LocateCalls;
  assert (GetPlatformConsoleList ()[2].DevicePath == ConsolePath);
  assert (LocateCalls == SavedLocateCalls);

  /* DP is an independent physical console, including when serial is absent.
     No NULL optional entry may truncate the BDS console list. */
  DpConsoleEnabled = TRUE;
  Consoles = GetPlatformConsoleList ();
  assert (Consoles[1].DevicePath == (VOID *)&DisplayDevicePath);
  assert (Consoles[2].DevicePath == (VOID *)&DpDisplayDevicePath);
  assert (Consoles[2].ConnectType == (CONSOLE_OUT | STD_ERROR));
  assert (memcmp (&DisplayDevicePath, &DpDisplayDevicePath, sizeof (DisplayDevicePath)) != 0);
  assert (Consoles[3].DevicePath == ConsolePath && Consoles[3].ConnectType == CONSOLE_ALL);
  assert (Consoles[4].DevicePath == NULL);
  SerialConsoleEnabled = FALSE;
  assert (GetPlatformConsoleList ()[2].DevicePath == (VOID *)&DpDisplayDevicePath);
  assert (Consoles[3].DevicePath == NULL && !Consoles[3].ConnectType);
  DpConsoleEnabled = FALSE;
  assert (GetPlatformConsoleList ()[2].DevicePath == NULL);
  SerialConsoleEnabled = TRUE;
  assert (GetPlatformConsoleList ()[2].DevicePath == ConsolePath && Consoles[3].DevicePath == NULL);

  /* Terminal requests only a shorter timeout.  SerialDxe must accept it even
     when fixed UART attributes cannot be changed by SerialPortLib. */
  assert (mSerialIoTemplate.SetAttributes (&mSerialIoTemplate, 115200, 1, 30,
          NoParity, 8, OneStopBit) == EFI_SUCCESS);
  assert (mSerialIoTemplate.Reset (&mSerialIoTemplate) == EFI_SUCCESS);

  /* A partial escape sequence reaches SerialIo exactly once; its timeout
     reports the actual three bytes instead of synthesizing unread bytes. */
  RxWord = 0x005B5B1B;
  RxWords = 1;
  RxPartial = 3;
  RxLast = TRUE;
  Registers[(GENI4_DATA + GENI_S_IRQ_STATUS_ADDR) / sizeof (UINT32)] = RX_LAST_IRQ;
  memset (Input, 0xA5, sizeof (Input));
  Size = sizeof (Input);
  assert (mSerialIoTemplate.Read (&mSerialIoTemplate, &Size, Input) == EFI_TIMEOUT);
  assert (Size == 3 && memcmp (Input, "\x1B[[", 3) == 0 && Input[3] == 0xA5);
  Size = 1;
  assert (mSerialIoTemplate.Read (&mSerialIoTemplate, &Size, Input) == EFI_TIMEOUT);
  assert (Size == 0 && Stalls == 6);

  free (mSerialConsoleDevicePath);
  mSerialConsoleDevicePath = NULL;
  TestPreferredGraphicsConsole ();
  puts ("Native serial console passed: SerialDxe protocol, TTY child path, timeout-only attributes and GENI short reads");
  return 0;
}
