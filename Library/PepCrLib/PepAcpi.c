/* Bounded Windows ACPI method-output decoder; no OS header or heap dependency.
 * Layout is checked against real acpiioct.h by the WDK compile/link checker.
 * SPDX-License-Identifier: MIT */
#include <Library/PepCrLib.h>

#define ACPI_OUTPUT_V1 0x426f6541U /* ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1 */
#define ACPI_OUTPUT_V2 0x4b6f6541U /* ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V2 */
#define ACPI_MAX_BYTES 0x100000U
#define ACPI_MAX_NODES 65536U
#define ACPI_MAX_DEPTH 16U

typedef struct {
  CR_PEP_VALUE *Nodes;
  UINTN Capacity, Used, ArgumentHeader;
} PEP_ACPI_BUILD;
STATIC UINT64 ReadLittle(CONST UINT8 *P, UINTN Size) {
  UINT64 Value = 0;
  for (UINTN I = 0; I < Size; ++I) Value |= (UINT64)P[I] << (I * 8U);
  return Value;
}
STATIC CR_STATUS Step(CONST UINT8 *Data, UINTN Length, UINTN Header,
                      UINTN *Payload, UINTN *Stride) {
  if (Length < Header + 4U) return CR_DEVICE_ERROR;
  *Payload = (UINTN)ReadLittle(Data + (Header == 4U ? 2U : 4U),
                             Header == 4U ? 2U : 4U);
  if (*Payload > Length - Header) return CR_DEVICE_ERROR;
  *Stride = Header + (*Payload < 4U ? 4U : *Payload);
  return *Stride <= Length ? CR_SUCCESS : CR_DEVICE_ERROR;
}
STATIC CR_STATUS List(CONST UINT8 *Data, UINTN Length, UINTN Depth,
                      PEP_ACPI_BUILD *Build, CR_PEP_VALUE *Parent) {
  UINTN Offset = 0, Count = 0, Payload, Stride, First = Build->Used;
  CR_STATUS Status;
  if (Depth > ACPI_MAX_DEPTH) return CR_DEVICE_ERROR;
  while (Offset < Length) {
    Status = Step(Data + Offset, Length - Offset, Build->ArgumentHeader, &Payload, &Stride);
    if (CR_ERROR(Status)) return Status;
    Offset += Stride; ++Count;
  }
  if (Count > ACPI_MAX_NODES - Build->Used) return CR_OUT_OF_RESOURCES;
  if (Build->Nodes != NULL && Count > Build->Capacity - Build->Used)
    return CR_BUFFER_TOO_SMALL;
  Build->Used += Count;
  if (Parent != NULL) {
    Parent->Type = CrPepPackage;
    Parent->Package.Count = Count;
    Parent->Package.Values = Count == 0 ? NULL : &Build->Nodes[First];
  }
  Offset = 0;
  for (UINTN I = 0; I < Count; ++I) {
    CONST UINT8 *Argument = Data + Offset;
    UINT64 Type = ReadLittle(Argument, 2);
    CR_PEP_VALUE *Node = Build->Nodes == NULL ? NULL : &Build->Nodes[First + I];
    Status = Step(Argument, Length - Offset, Build->ArgumentHeader, &Payload, &Stride);
    if (CR_ERROR(Status)) return Status;
    CONST UINT8 *Value = Argument + Build->ArgumentHeader;
    if (Type == 0) { /* ACPI_METHOD_ARGUMENT_INTEGER */
      if (Payload != 4 && Payload != 8) return CR_DEVICE_ERROR;
      if (Node != NULL) { Node->Type = CrPepInteger; Node->Integer = ReadLittle(Value, Payload); }
    } else if (Type == 1) { /* ACPI_METHOD_ARGUMENT_STRING */
      if (Payload == 0 || Value[Payload - 1] != 0) return CR_DEVICE_ERROR;
      for (UINTN J = 0; J + 1 < Payload; ++J)
        if (Value[J] == 0) return CR_DEVICE_ERROR;
      if (Node != NULL) {
        Node->Type = CrPepString;
        Node->String.Data = (CONST CHAR8 *)Value; Node->String.Length = Payload - 1;
      }
    } else if (Type == 2) { /* ACPI_METHOD_ARGUMENT_BUFFER */
      if (Node != NULL) {
        Node->Type = CrPepBuffer;
        Node->Buffer.Data = Value; Node->Buffer.Length = Payload;
      }
    } else if (Type == 3 || Type == 4) { /* PACKAGE and PACKAGE_EX */
      Status = List(Value, Payload, Depth + 1, Build, Node);
      if (CR_ERROR(Status)) return Status;
    } else return CR_UNSUPPORTED;
    Offset += Stride;
  }
  return CR_SUCCESS;
}
CR_STATUS CrPepDecodeAcpiOutput(CONST VOID *Buffer, UINTN Length,
                               CR_PEP_VALUE *Nodes, UINTN Capacity,
                               UINTN *Required, CONST CR_PEP_VALUE **Output) {
  CONST UINT8 *Data = Buffer;
  UINTN Header, Actual, DirectCount = 0, Offset = 12U, Payload, Stride;
  CR_STATUS Status;
  PEP_ACPI_BUILD Build;
  if (Required == NULL || Output == NULL) return CR_INVALID_PARAMETER;
  *Required = 0; *Output = NULL;
  if (Buffer == NULL || Length < 12U || Length > ACPI_MAX_BYTES ||
      (Nodes == NULL && Capacity != 0)) return CR_INVALID_PARAMETER;
  UINT64 Signature = ReadLittle(Data, 4);
  if (Signature == ACPI_OUTPUT_V1) Header = 4;
  else if (Signature == ACPI_OUTPUT_V2) Header = 8;
  else return CR_DEVICE_ERROR;
  Actual = (UINTN)ReadLittle(Data + 4, 4);
  if (Actual < 12U || Actual > Length) return CR_DEVICE_ERROR;
  while (Offset < Actual) {
    Status = Step(Data + Offset, Actual - Offset, Header, &Payload, &Stride);
    if (CR_ERROR(Status)) return Status;
    Offset += Stride; ++DirectCount;
  }
  if (DirectCount != ReadLittle(Data + 8, 4)) return CR_DEVICE_ERROR;
  Build = (PEP_ACPI_BUILD){NULL, 0, 1, Header};
  Status = List(Data + 12, Actual - 12U, 1, &Build, NULL);
  if (CR_ERROR(Status)) return Status;
  *Required = Build.Used;
  if (Nodes == NULL || Capacity < Build.Used) return CR_BUFFER_TOO_SMALL;
  Build = (PEP_ACPI_BUILD){Nodes, Capacity, 1, Header};
  Status = List(Data + 12, Actual - 12U, 1, &Build, &Nodes[0]);
  if (!CR_ERROR(Status)) *Output = &Nodes[0];
  return Status;
}
