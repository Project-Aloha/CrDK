/* Inactive HDK DXE/runtime SerialPortLib variant. MIT
 * Reuse the actual native console implementation behind one shared gate.
 * No BootServices call or shared physical pointer is used after local EBS.
 * The native build chooses a separate runtime INF defining RUNTIME_COPY=1.
 */
#include <Library/UartAdmissionUefiLib.h>
#include <Library/SerialPortLib.h>

#ifndef CR_UART_RUNTIME_COPY
#define CR_UART_RUNTIME_COPY FALSE
#endif
#ifdef CR_UART_IMAGE_PREFIX
#define CR_UART_JOIN_INNER(A,B) A##B
#define CR_UART_JOIN(A,B) CR_UART_JOIN_INNER(A,B)
#define CR_UART_NAME(Name) CR_UART_JOIN(CR_UART_IMAGE_PREFIX,Name)
#else
#define CR_UART_NAME(Name) Name
#endif
/* These private names have no public header or independent caller. Include
 * the existing code once per linked image; never duplicate its UART setup. */
#define SerialPortRead CR_UART_NAME(CrUartInnerRead)
#define SerialPortWrite CR_UART_NAME(CrUartInnerWrite)
#define SerialPortPoll CR_UART_NAME(CrUartInnerPoll)
#define SerialPortControl CR_UART_NAME(CrUartInnerControl)
#define SerialPortSetAttributes CR_UART_NAME(CrUartInnerSetAttributes)
#define SerialPortGetControl CR_UART_NAME(CrUartInnerGetControl)
#define SerialPortSetControl CR_UART_NAME(CrUartInnerSetControl)
#define SerialPortInitialize CR_UART_NAME(CrUartInnerInitialize)
#ifndef CR_UART_NATIVE_SOURCE
#define CR_UART_NATIVE_SOURCE "../../../MUCommonCrPkg/Library/QcomGeniSerialPortLib/QcomGeniSerialPortLib.c"
#endif
#include CR_UART_NATIVE_SOURCE
#undef SerialPortRead
#undef SerialPortWrite
#undef SerialPortPoll
#undef SerialPortControl
#undef SerialPortSetAttributes
#undef SerialPortGetControl
#undef SerialPortSetControl
#undef SerialPortInitialize

/* Starts closed even before library constructors: recursive allocation/debug
 * and early DXE-core diagnostics cannot bypass common writer admission. SEC
 * and PEI keep their original library; all normal DXE boot diagnostics remain
 * available once this constructor finishes, before DebugLib constructors. */
static CR_UART_UEFI_CLIENT mUartClient = {.Closed=1};

EFI_STATUS EFIAPI CR_UART_NAME(CrUartSerialConstructor)(EFI_HANDLE Image,
    EFI_SYSTEM_TABLE *Table) {
  (VOID)Image;
  return CrUartUefiConstruct(&mUartClient,Table,CR_UART_RUNTIME_COPY);
}
EFI_STATUS EFIAPI CR_UART_NAME(CrUartSerialDestructor)(EFI_HANDLE Image,
    EFI_SYSTEM_TABLE *Table) {
  (VOID)Image;(VOID)Table;
  return CrUartUefiDestruct(&mUartClient);
}
UINTN EFIAPI CR_UART_NAME(SerialPortRead)(UINT8 *Buffer,UINTN Bytes) {
  if(!CrUartUefiEnter(&mUartClient))return 0;
  UINTN Done=CR_UART_NAME(CrUartInnerRead)(Buffer,Bytes);
  CrUartUefiLeave(&mUartClient);return Done;
}
UINTN EFIAPI CR_UART_NAME(SerialPortWrite)(UINT8 *Buffer,UINTN Bytes) {
  if(!CrUartUefiEnter(&mUartClient))return 0;
  UINTN Done=CR_UART_NAME(CrUartInnerWrite)(Buffer,Bytes);
  CrUartUefiLeave(&mUartClient);return Done;
}
BOOLEAN EFIAPI CR_UART_NAME(SerialPortPoll)(VOID) {
  if(!CrUartUefiEnter(&mUartClient))return FALSE;
  BOOLEAN Ready=CR_UART_NAME(CrUartInnerPoll)();
  CrUartUefiLeave(&mUartClient);return Ready;
}
UINTN EFIAPI CR_UART_NAME(SerialPortControl)(UINTN Arg,UINTN Param) {
  if(!CrUartUefiEnter(&mUartClient))return 0;
  UINTN Done=CR_UART_NAME(CrUartInnerControl)(Arg,Param);
  CrUartUefiLeave(&mUartClient);return Done;
}
RETURN_STATUS EFIAPI CR_UART_NAME(SerialPortInitialize)(VOID) {
  if(!CrUartUefiEnter(&mUartClient))return EFI_NOT_READY;
  RETURN_STATUS S=CR_UART_NAME(CrUartInnerInitialize)();
  CrUartUefiLeave(&mUartClient);return S;
}
RETURN_STATUS EFIAPI CR_UART_NAME(SerialPortSetAttributes)(UINT64 *Baud,
    UINT32 *Depth,UINT32 *Timeout,EFI_PARITY_TYPE *Parity,UINT8 *Bits,
    EFI_STOP_BITS_TYPE *Stop) {
  if(!CrUartUefiEnter(&mUartClient))return EFI_NOT_READY;
  RETURN_STATUS S=CR_UART_NAME(CrUartInnerSetAttributes)(Baud,Depth,Timeout,
      Parity,Bits,Stop);
  CrUartUefiLeave(&mUartClient);return S;
}
RETURN_STATUS EFIAPI CR_UART_NAME(SerialPortGetControl)(UINT32 *Control) {
  if(!CrUartUefiEnter(&mUartClient))return EFI_NOT_READY;
  RETURN_STATUS S=CR_UART_NAME(CrUartInnerGetControl)(Control);
  CrUartUefiLeave(&mUartClient);return S;
}
RETURN_STATUS EFIAPI CR_UART_NAME(SerialPortSetControl)(UINT32 Control) {
  if(!CrUartUefiEnter(&mUartClient))return EFI_NOT_READY;
  RETURN_STATUS S=CR_UART_NAME(CrUartInnerSetControl)(Control);
  CrUartUefiLeave(&mUartClient);return S;
}
