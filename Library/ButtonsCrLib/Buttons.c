/** @file
  IRQ-woken PMK8350 PON / PM8350 GPIO navigation buttons.
  SPDX-License-Identifier: MIT
**/
#include <Library/ButtonsCrLib.h>
#include <oskal/cr_string.h>
#ifdef _KERNEL_MODE
#include "Buttons.tmh"
#endif

STATIC CR_STATUS ReadBus(CR_BUTTON_CONTEXT *C,UINT8 S,UINT16 A,UINT8 *B,UINTN N) {
  return C->SharedBus?C->SharedBus->Read(C->SharedBus,S,A,B,N):SpmiRead(C->Spmi,S,A,B,N);
}

STATIC UINT32 ExchangeDirty(volatile UINT32 *Value, UINT32 NewValue) {
#ifdef _KERNEL_MODE
  return (UINT32)InterlockedExchange((volatile LONG *)Value, (LONG)NewValue);
#else
  UINT32 Old;
  do { Old = *Value; }
  while (InterlockedCompareExchange32(Value, Old, NewValue) != Old);
  return Old;
#endif
}

STATIC VOID ButtonIrq(VOID *Parameter) {
  CR_BUTTON_CONTEXT *Context = Parameter;
  if (!Context->Stopping) {
    (VOID)ExchangeDirty(&Context->Dirty, 1);
    Context->Wake(Context->CallbackContext);
  }
}

STATIC UINTN PicAddress(CR_BUTTON_CONTEXT *Context, CR_BUTTON_STATE *State) {
  UINT16 Apid;
  if (CR_ERROR(SpmiGetApid(Context->Spmi, State->Target->Sid,
                          State->Target->Peripheral, &Apid))) return 0;
  return Context->Spmi->PmicArb.Ops->GetAccEnable(Context->Spmi, Apid);
}

STATIC UINT32 ReadPic(CR_BUTTON_CONTEXT *Context, UINTN Address) {
  if (Context->Spmi->Io.Read32 != NULL)
    return Context->Spmi->Io.Read32(Context->Spmi->Io.Context, Address);
  return CrMmioRead32(Address);
}

STATIC VOID WritePic(CR_BUTTON_CONTEXT *Context, UINTN Address, UINT32 Value) {
  if (Context->Spmi->Io.Write32 != NULL)
    Context->Spmi->Io.Write32(Context->Spmi->Io.Context, Address, Value);
  else CrMmioWrite32(Address, Value);
}

STATIC CR_STATUS ReadPressed(CR_BUTTON_CONTEXT *Context,
                            CR_BUTTON_STATE *State, BOOLEAN *Pressed) {
  UINT8 Value;
  CR_STATUS Status;
  if (State->Target->Source == BUTTON_SOURCE_PON) {
    Status = ReadBus(Context, State->Target->Sid,
                     (UINT16)((State->Target->Peripheral << 8) + 0x10),
                     &Value, 1);
    if (Status==CR_SUCCESS)
      *Pressed = ((Value & (1U << State->Target->Irq)) != 0);
  } else {
    Status = PmicGpioGetValue(Context->Gpio, State->Controller,
                             State->Target->Pin, Pressed);
  }
  if (!CR_ERROR(Status) && State->Target->ActiveLow) *Pressed = !*Pressed;
  /* Record actual successful hardware observations, not debounced keystrokes
     or repeats. IRQ-woken service preserves taps between consumer polls. */
  if (Status == CR_SUCCESS) {
    if (State->Target->Code == BUTTON_CODE_POWER && *Pressed &&
        State->PhysicalKnown && !State->PhysicalPressed &&
        Context->PowerPressSequence != ~(UINT64)0)
      Context->PowerPressSequence++;
    State->PhysicalKnown=TRUE;State->PhysicalPressed=*Pressed;
  }
  return Status;
}

STATIC VOID Enqueue(CR_BUTTON_CONTEXT *Context, BUTTON_CODE Code) {
  if (Context->Count == CR_BUTTON_QUEUE_SIZE) return;
  Context->Queue[(Context->Head + Context->Count) % CR_BUTTON_QUEUE_SIZE] = Code;
  Context->Count++;
  if (Context->KeyReady != NULL) Context->KeyReady(Context->CallbackContext, Code);
}

CR_STATUS CrButtonsReset(CR_BUTTON_CONTEXT *Context, UINT64 NowUs) {
  CR_STATUS Status;
  BOOLEAN Pressed;
  UINT16 Index;
  if (Context == NULL || !Context->Initialized || Context->Stopping)
    return CR_NOT_READY;
  Context->Head = Context->Count = 0;
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    Context->State[Index].Armed = FALSE;
    Context->State[Index].RepeatAt = 0;
  }
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    CR_BUTTON_STATE *State = &Context->State[Index];
    Status = ReadPressed(Context, State, &Pressed);
    if (Status!=CR_SUCCESS) {
      Context->IoFailed = TRUE;
      return Status;
    }
    State->Stable = State->Candidate = Pressed;
    State->Armed = !Pressed; /* A boot/reset-held key must first be released. */
    State->Since = NowUs;
    State->RepeatAt = 0;
  }
  Context->IoFailed = FALSE;
  return CR_SUCCESS;
}

STATIC CR_STATUS Start(CR_BUTTON_CONTEXT *Context,
                        CONST ButtonsTargetContext *Target,
                        SpmiDeviceContext *Spmi, CR_SPMI_BUS *Shared, PmicGpioDeviceContext *Gpio,
                        CR_BUTTON_WAKE Wake, CR_BUTTON_KEY_READY KeyReady,
                        VOID *CallbackContext, UINT64 NowUs) {
  CR_STATUS Status;
  UINT16 Index, Controller;
  UINT8 Value;
  UINTN Address;
  UINT32 Codes = 0;
  if (Context == NULL || Target == NULL || Gpio == NULL ||
      (Shared==NULL && (Spmi==NULL || !Spmi->Initialized || !Spmi->HasInterrupt)) ||
      (Shared!=NULL && (!Shared->Read || !Shared->Subscribe || !Shared->Enable || !Shared->Unsubscribe || !Shared->BeginRetire)) || !Gpio->Initialized ||
      Wake == NULL || Target->Buttons == NULL || Target->ButtonCount == 0 ||
      Target->ButtonCount > BUTTON_CODE_MAX) return CR_INVALID_PARAMETER;
  if (Context->Initialized) return CR_ALREADY_STARTED;
  /* Validate the complete target before changing any PMIC register. */
  for (Index = 0; Index < Target->ButtonCount; Index++) {
    CONST ButtonTarget *Button = &Target->Buttons[Index];
    if ((UINT32)Button->Code >= BUTTON_CODE_MAX || Button->Sid > 15 || Button->Irq >= 8 ||
        Button->DebounceUs == 0 || Button->DebounceUs > 1000000 ||
        (Codes & (1U << Button->Code)) != 0 ||
        (Button->Source != BUTTON_SOURCE_PON &&
         Button->Source != BUTTON_SOURCE_PMIC_GPIO)) return CR_INVALID_PARAMETER;
    Codes |= 1U << Button->Code;
  }
  cr_memset(Context, 0, sizeof(*Context));
  Context->Target = Target; Context->Spmi = Spmi; Context->SharedBus=Shared; Context->Gpio = Gpio;
  Context->Wake = Wake; Context->KeyReady = KeyReady;
  Context->CallbackContext = CallbackContext;
  Context->Initialized = TRUE;
  for (Index = 0; Index < Target->ButtonCount; Index++) {
    CR_BUTTON_STATE *State = &Context->State[Index];
    CONST ButtonTarget *Button = &Target->Buttons[Index];
    State->Target = Button;
    if (Button->Source == BUTTON_SOURCE_PMIC_GPIO) {
      for (Controller = 0; Controller < Gpio->Target->ControllerCount; Controller++)
        if (Button->Controller != NULL &&
            cr_strcmp(Button->Controller, Gpio->Target->Controllers[Controller].Name) == 0)
          break;
      if (Controller == Gpio->Target->ControllerCount || Button->Pin == 0 ||
          Button->Sid != Gpio->Target->Controllers[Controller].Sid ||
          Button->Pin > Gpio->Target->Controllers[Controller].PinCount ||
          Button->Peripheral != ((Gpio->Target->Controllers[Controller].PeripheralBase +
                                 (Button->Pin - 1) * 0x100) >> 8)) {
        Status = CR_INVALID_PARAMETER; goto Failure;
      }
      State->Controller = Controller;
      Status = PmicGpioGetConfig(Gpio, Controller, Button->Pin, &State->SavedGpio);
      if (Status!=CR_SUCCESS) goto Failure;
      State->GpioSaved = TRUE;
      PmicGpioConfig Config = State->SavedGpio;
      Config.Enabled = TRUE; Config.Direction = PMIC_GPIO_DIRECTION_INPUT;
      Config.Function = PMIC_GPIO_FUNCTION_NORMAL; Config.PowerSource = 1;
      Config.Pull = PMIC_GPIO_PULL_UP_30UA; Config.DtestBuffer = 0;
      Status = PmicGpioConfigure(Gpio, Controller, Button->Pin, &Config);
      if (Status!=CR_SUCCESS) goto Failure;
    } else {
      /* Linux pm8941-pwrkey: Gen2 primary/secondary subtype4/5 uses
         power/resin bits0/1; Gen3 HLOS subtype9 uses7/6. Qualify the actual
         peripheral before accepting target IRQ bits. */
      Status = ReadBus(Context, Button->Sid,
                       (UINT16)((Button->Peripheral << 8) + 5), &Value, 1);
      if (Status!=CR_SUCCESS) goto Failure;
      if (Button->ActiveLow || !((Value==9 && (Button->Irq==6 || Button->Irq==7)) ||
           ((Value==4 || Value==5) && (Button->Irq==0 || Button->Irq==1)))) {
        Status = CR_UNSUPPORTED; goto Failure;
      }
    }
    if (Shared) {
      Status=Shared->Subscribe(Shared,Button->Sid,Button->Peripheral,Button->Irq,
                               Button->Trigger,ButtonIrq,Context,&State->SharedIrq);
      if (Status!=CR_SUCCESS) goto Failure;
      State->IrqRegistered=TRUE;continue;
    }
    Status = ReadBus(Context, Button->Sid,
                     (UINT16)((Button->Peripheral << 8) + 0x11), State->SavedIrq, 3);
    if (Status!=CR_SUCCESS) goto Failure;
    Status = ReadBus(Context, Button->Sid,
                     (UINT16)((Button->Peripheral << 8) + 0x15), &State->SavedIrq[3], 1);
    if (Status!=CR_SUCCESS) goto Failure;
    Address = PicAddress(Context, State);
    if (Address == 0) { Status = CR_NOT_FOUND; goto Failure; }
    State->SavedPic = ReadPic(Context, Address);
    State->IrqSaved = TRUE;
    Status = SpmiRegisterIrq(Spmi, Button->Sid, Button->Peripheral, Button->Irq,
                            Button->Trigger, ButtonIrq, Context);
    if (Status!=CR_SUCCESS) goto Failure;
    State->IrqRegistered = TRUE;
  }
  Status = CrButtonsReset(Context, NowUs);
  if (Status!=CR_SUCCESS) goto Failure;
  for (Index = 0; Index < Target->ButtonCount; Index++) {
    CONST ButtonTarget *Button = &Target->Buttons[Index];
    Status = Shared?Shared->Enable(Shared,&Context->State[Index].SharedIrq,TRUE):
      SpmiEnableIrq(Spmi, Button->Sid, Button->Peripheral, Button->Irq, TRUE);
    if (Status!=CR_SUCCESS) goto Failure;
  }
  return CR_SUCCESS;
Failure:
  /* The owner stops delivery before retrying partial cleanup. */
  Context->Stopping = TRUE;
  return Status;
}

CR_STATUS CrButtonsStop(CR_BUTTON_CONTEXT *Context) {
  CR_STATUS Status, FirstError = CR_SUCCESS;
  UINT16 Index;
  UINT8 Offset, Mask, Value;
  UINTN Address;
  if (Context == NULL) return CR_INVALID_PARAMETER;
  if (!Context->Initialized) return CR_SUCCESS;
  Context->Stopping = TRUE;
  if (Context->SharedBus) {
    Status=Context->SharedBus->BeginRetire(Context->SharedBus);
    if (Status!=CR_SUCCESS) return Status;
  }
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    CR_BUTTON_STATE *State = &Context->State[Index];
    if (Context->SharedBus) {
      if (!State->SharedIrq.Provider) continue;
      Status=Context->SharedBus->Unsubscribe(Context->SharedBus,&State->SharedIrq);
      if (Status!=CR_SUCCESS) {if (FirstError==CR_SUCCESS) FirstError=Status;}
      else State->IrqRegistered=FALSE;
      continue;
    }
    if (!State->IrqRegistered) continue;
    Status = SpmiUnregisterIrq(Context->Spmi, State->Target->Sid,
                              State->Target->Peripheral, State->Target->Irq);
    if (Status!=CR_SUCCESS) { if (FirstError==CR_SUCCESS) FirstError = Status; }
    else State->IrqRegistered = FALSE;
  }
  /* All IRQs must be detached before restoring shared PIC/PON state. */
  if (FirstError!=CR_SUCCESS) return FirstError;
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    CR_BUTTON_STATE *State = &Context->State[Index];
    if (State->IrqSaved) {
      Mask = (UINT8)(1U << State->Target->Irq);
      Status = CR_SUCCESS;
      for (Offset = 0; Offset < 3 && !CR_ERROR(Status); Offset++)
        Status = SpmiMaskedWrite(Context->Spmi, State->Target->Sid,
                                 (UINT16)((State->Target->Peripheral << 8) + 0x11 + Offset),
                                 &State->SavedIrq[Offset], &Mask, 1);
      if (Status==CR_SUCCESS) {
        Value = Mask;
        Status = SpmiWrite(Context->Spmi, State->Target->Sid,
                           (UINT16)((State->Target->Peripheral << 8) +
                                    ((State->SavedIrq[3] & Mask) ? 0x15 : 0x16)), &Value, 1);
      }
      if (Status==CR_SUCCESS) {
        Address = PicAddress(Context, State);
        if (Address == 0) Status = CR_NOT_FOUND;
        else {
          WritePic(Context, Address, (ReadPic(Context, Address) & ~1U) |
                                    (State->SavedPic & 1U));
          State->IrqSaved = FALSE;
        }
      }
      if (CR_ERROR(Status) && !CR_ERROR(FirstError)) FirstError = Status;
    }
    if (State->GpioSaved) {
      Status = PmicGpioConfigure(Context->Gpio, State->Controller,
                                 State->Target->Pin, &State->SavedGpio);
      if (Status==CR_SUCCESS) State->GpioSaved = FALSE;
      else if (FirstError==CR_SUCCESS) FirstError = Status;
    }
  }
  if (FirstError==CR_SUCCESS) {
    Context->Initialized = FALSE;
    Context->Head = Context->Count = 0;
  }
  return FirstError;
}

CR_STATUS CrButtonsService(CR_BUTTON_CONTEXT *Context, UINT64 NowUs) {
  UINT16 Index;
  BOOLEAN Pressed;
  CR_STATUS Status, FirstError = CR_SUCCESS;
  if (Context == NULL || !Context->Initialized || Context->Stopping) return CR_NOT_READY;
  if (CrButtonsNextServiceUs(Context) == 0) return CR_SUCCESS;
  (VOID)ExchangeDirty(&Context->Dirty, 0);
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    CR_BUTTON_STATE *State = &Context->State[Index];
    Status = ReadPressed(Context, State, &Pressed);
    if (Status!=CR_SUCCESS) {
      State->Candidate = State->Stable;
      State->Since = NowUs;
      if (FirstError==CR_SUCCESS) FirstError = Status;
      continue;
    }
    if (Pressed != State->Candidate) {
      State->Candidate = Pressed; State->Since = NowUs;
    }
    if (State->Candidate != State->Stable &&
        NowUs - State->Since >= State->Target->DebounceUs) {
      State->Stable = State->Candidate;
      if (!State->Stable) { State->Armed = TRUE; State->RepeatAt = 0; }
      else if (State->Armed) {
        Enqueue(Context, State->Target->Code);
        State->RepeatAt = NowUs + 500000;
      }
    } else if (State->Stable && State->Candidate && State->Armed &&
               State->Target->Code != BUTTON_CODE_POWER && State->RepeatAt != 0 &&
               NowUs >= State->RepeatAt) {
      Enqueue(Context, State->Target->Code);
      State->RepeatAt = NowUs + 100000; /* Never burst after a delayed worker. */
    }
  }
  Context->IoFailed = CR_ERROR(FirstError);
  return FirstError;
}

BOOLEAN CrButtonsHasKey(CONST CR_BUTTON_CONTEXT *Context) {
  return Context != NULL && Context->Initialized && !Context->Stopping && Context->Count != 0;
}

CR_STATUS CrButtonsRead(CR_BUTTON_CONTEXT *Context, BUTTON_CODE *Code) {
  if (Context == NULL || Code == NULL) return CR_INVALID_PARAMETER;
  if (!CrButtonsHasKey(Context)) return CR_NOT_READY;
  *Code = Context->Queue[Context->Head];
  Context->Head = (UINT16)((Context->Head + 1) % CR_BUTTON_QUEUE_SIZE);
  Context->Count--;
  return CR_SUCCESS;
}

UINT32 CrButtonsNextServiceUs(CONST CR_BUTTON_CONTEXT *Context) {
  UINT16 Index;
  if (Context == NULL || !Context->Initialized || Context->Stopping) return 0;
  if (Context->Dirty != 0) return 20000;
  if (Context->IoFailed) return 1000000;
  for (Index = 0; Index < Context->Target->ButtonCount; Index++) {
    CONST CR_BUTTON_STATE *State = &Context->State[Index];
    if (State->Candidate != State->Stable ||
        (State->Stable && State->Armed && State->Target->Code != BUTTON_CODE_POWER))
      return 20000;
  }
  return 0;
}

CR_STATUS CrButtonsGetPressed(CR_BUTTON_CONTEXT *Context, UINT32 *Mask) {
  UINT32 Candidate=0; BOOLEAN Pressed; CR_STATUS S; UINT16 I;
  if (!Context || !Mask) return CR_INVALID_PARAMETER;
  if (!Context->Initialized || Context->Stopping) return CR_NOT_READY;
  for (I=0; I<Context->Target->ButtonCount; ++I) {
    S=ReadPressed(Context,&Context->State[I],&Pressed);
    if (S!=CR_SUCCESS) return CR_ERROR(S)?S:CR_NOT_READY;
    if (Pressed) Candidate|=1U<<Context->State[I].Target->Code;
  }
  *Mask=Candidate; return CR_SUCCESS;
}

CR_STATUS CrButtonsGetSnapshot(CR_BUTTON_CONTEXT *Context,UINT32 *Mask,UINT64 *Sequence) {
  UINT32 Candidate;CR_STATUS S;
  if (!Mask || !Sequence) return CR_INVALID_PARAMETER;
  S=CrButtonsGetPressed(Context,&Candidate);if(S!=CR_SUCCESS)return S;
  *Mask=Candidate;*Sequence=Context->PowerPressSequence;return CR_SUCCESS;
}

CR_STATUS CrButtonsStart(CR_BUTTON_CONTEXT *C,CONST ButtonsTargetContext *T,
 SpmiDeviceContext *S,PmicGpioDeviceContext *G,CR_BUTTON_WAKE W,CR_BUTTON_KEY_READY K,
 VOID *X,UINT64 N){return Start(C,T,S,NULL,G,W,K,X,N);}
CR_STATUS CrButtonsStartShared(CR_BUTTON_CONTEXT *C,CONST ButtonsTargetContext *T,
 CR_SPMI_BUS *S,PmicGpioDeviceContext *G,CR_BUTTON_WAKE W,CR_BUTTON_KEY_READY K,
 VOID *X,UINT64 N){if(!S)return CR_INVALID_PARAMETER;return Start(C,T,NULL,S,G,W,K,X,N);}
