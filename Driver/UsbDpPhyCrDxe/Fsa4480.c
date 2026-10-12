/** FSA4480 SBU switch, adapted from Linux typec/mux/fsa4480.c.
    Copyright (C) 2021-2022 Linaro Ltd.
    SPDX-License-Identifier: GPL-2.0-only */
#include "Fsa4480.h"
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>

STATIC EFI_STATUS PrepareBus(CR_FSA4480 *Fsa)
{
  UINTN Frequency = 100000;
  EFI_STATUS Status;
  if (Fsa->Bus->SetBusFrequency == NULL) return EFI_UNSUPPORTED;
  /* sm8450-hdk.dts selects standard mode on this shared board bus. */
  Status = Fsa->Bus->SetBusFrequency(Fsa->Bus, &Frequency);
  if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
  return Frequency == 100000 ? EFI_SUCCESS : EFI_PROTOCOL_ERROR;
}

EFI_STATUS CrFsaWrite(CR_FSA4480 *Fsa, UINT8 Reg, UINT8 Value, BOOLEAN *Dispatched)
{
  UINT8 Data[2] = {Reg, Value};
  EFI_I2C_REQUEST_PACKET Packet = {0};
  if (Dispatched != NULL) *Dispatched = FALSE;
  if (Fsa == NULL || Fsa->Bus == NULL || Fsa->Bus->StartRequest == NULL || Dispatched == NULL)
    return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = PrepareBus(Fsa);
  if (Status != EFI_SUCCESS) return Status;
  Packet.OperationCount = 1;
  Packet.Operation[0].LengthInBytes = sizeof(Data);
  Packet.Operation[0].Buffer = Data;
  *Dispatched = TRUE;
  Status = Fsa->Bus->StartRequest(Fsa->Bus, 0x42, &Packet, NULL, NULL);
  return Status == EFI_SUCCESS ? Status : EFI_ERROR(Status) ? Status : EFI_NOT_READY;
}

EFI_STATUS CrFsaRead(CR_FSA4480 *Fsa, UINT8 Reg, UINT8 *Value)
{
  struct { UINTN Count; EFI_I2C_OPERATION Op[2]; } Packet = {0};
  if (Fsa == NULL || Fsa->Bus == NULL || Fsa->Bus->StartRequest == NULL || Value == NULL)
    return EFI_INVALID_PARAMETER;
  EFI_STATUS Status = PrepareBus(Fsa);
  if (Status != EFI_SUCCESS) return Status;
  Packet.Count = 2;
  Packet.Op[0].LengthInBytes = 1;
  Packet.Op[0].Buffer = &Reg;
  Packet.Op[1].Flags = I2C_FLAG_READ;
  Packet.Op[1].LengthInBytes = 1;
  Packet.Op[1].Buffer = Value;
  Status = Fsa->Bus->StartRequest(Fsa->Bus, 0x42,
                               (EFI_I2C_REQUEST_PACKET *)&Packet, NULL, NULL);
  return Status == EFI_SUCCESS ? Status : EFI_ERROR(Status) ? Status : EFI_NOT_READY;
}
