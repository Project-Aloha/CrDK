/** @file GENI FIFO receive flow-control regression. SPDX-License-Identifier: MIT */
#include "GeniTestCommon.h"
#include "../Driver/I2CCrDxe/I2CCrDxe.c"
#include "../Library/GpioLib/gpio.c"
#include "../Target/Sm8450/CrGpioTarget.c"

static UINT32 Registers[0x1000 / 4];
static UINT32 Registers0[0x1000 / 4];
static UINT32 CommandCount;
static UINT32 PinMask, ClockCalls[2];
static GpioDeviceContext *PinContext;
static UINT32 PinControl[208], PinIo[208];
static BOOLEAN DropEgpioOwner;
static UINT32 ClockRefs[2][3], ClockReleases;
static UINT32 ClockFailAcquire=MAX_UINT32,ClockFailRelease=MAX_UINT32;
static BOOLEAN ClockRetainFailure;
static UINT32 ClockIndex(CONST CHAR8 *Name) {
  if(strstr(Name,"_m_ahb_")) return 0;
  if(strstr(Name,"_s_ahb_")) return 1;
  return 2;
}
static EFI_STATUS EFIAPI ClockSet (EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Group,
                                  CONST CHAR8 *Name, UINT64 Frequency, BOOLEAN Enable) {
  (VOID)This;
  assert (strcmp (Group, "gcc") == 0 && !Enable && Frequency==0);
  BOOLEAN Qup0 = strstr (Name, "wrap_0") != NULL || strstr (Name, "wrap0") != NULL;
  UINT32 I=ClockIndex(Name);assert(ClockRefs[Qup0?0:1][I]>1);++ClockReleases;
  if(!Qup0 && I==ClockFailRelease) return EFI_TIMEOUT;
  --ClockRefs[Qup0?0:1][I];return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FifoClockAcquire(EFI_CLOCK_CR_PROTOCOL *This, CONST CHAR8 *Group,
  CONST CHAR8 *Name, UINT64 Frequency, BOOLEAN *Owned) {
  (VOID)This;assert(strcmp(Group,"gcc")==0 && Owned!=NULL);
  BOOLEAN Qup0=strstr(Name,"wrap_0")!=NULL || strstr(Name,"wrap0")!=NULL;
  assert (Qup0 || strstr (Name, "wrap_1") != NULL || strstr (Name, "wrap1") != NULL);
  assert (Frequency == (strstr (Name, "_s5_") != NULL || strstr (Name, "_s1_") != NULL ? 19200000U : 0U));
  ++ClockCalls[Qup0 ? 0 : 1];
  UINT32 I=ClockIndex(Name);
  *Owned=Qup0 || I!=ClockFailAcquire || ClockRetainFailure;
  if(*Owned) ++ClockRefs[Qup0?0:1][I];
  return !Qup0 && I==ClockFailAcquire ? EFI_TIMEOUT : EFI_SUCCESS;
}
static EFI_CLOCK_CR_PROTOCOL Clock = {.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,
  .SetClock = ClockSet,.AcquireClock=FifoClockAcquire};
static UINT32 InjectIrq, InjectCommand, AbortCount;
static BOOLEAN DropReadData, DropDone;
static EFI_STATUS EFIAPI Pins (EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  (VOID)This;
  assert (Config->PinNumber == 32 || Config->PinNumber == 33 ||
          Config->PinNumber == 206 || Config->PinNumber == 207);
  PinMask |= Config->PinNumber >= 206 ? 2 : 1;
  GpioSetConfig(PinContext, Config);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReadPins(EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config) {
  (VOID)This;
  GpioReadConfig(PinContext, Config);
  return EFI_SUCCESS;
}
static EFI_GPIO_CR_PROTOCOL Gpio = {.ConfigGpio = Pins, .GetGpioConfig = ReadPins};
static EFI_STATUS EFIAPI Locate (EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  (VOID)Registration;
  if (Guid == &gEfiGpioCrProtocolGuid) *Interface = &Gpio;
  else if (Guid == &gEfiQupCrProtocolGuid) *Interface = &mQupProtocol;
  else if (Guid == &gEfiClockCrProtocolGuid) *Interface = &Clock;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32 (UINTN Address) {
  if (Address >= 0xF100000U) {
    UINTN Pin = (Address - 0xF100000U) / 0x1000U;
    assert(Pin == 32 || Pin == 33 || Pin == 206 || Pin == 207);
    assert((Address & 0xFFFU) == 0 || (Address & 0xFFFU) == 4);
    return (Address & 4U) ? PinIo[Pin] : PinControl[Pin];
  }
  BOOLEAN Qup0 = Address >= 0x994000 && Address < 0x995000;
  UINT32 *Regs = Qup0 ? Registers0 : Registers;
  assert (IccVotedMask == (Qup0 ? 56 : 7));
  UINTN Offset = Address - (Qup0 ? 0x994000 : 0xa84000);
  assert (Offset < sizeof (Registers));
  if (Offset == SE_RX_FIFO) {
    Regs[SE_RX_FIFO_STATUS / 4] = 0;
    return 0x11;
  }
  return Regs[Offset / 4];
}
UINT32 EFIAPI MmioWrite32 (UINTN Address, UINT32 Value) {
  if (Address >= 0xF100000U) {
    UINTN Pin = (Address - 0xF100000U) / 0x1000U;
    assert(Pin == 32 || Pin == 33 || Pin == 206 || Pin == 207);
    assert((Address & 0xFFFU) == 0 || (Address & 0xFFFU) == 4);
    if (Address & 4U) PinIo[Pin] = Value;
    else PinControl[Pin] = DropEgpioOwner ? Value & ~BIT12 : Value;
    return Value;
  }
  BOOLEAN Qup0 = Address >= 0x994000 && Address < 0x995000;
  UINT32 *Regs = Qup0 ? Registers0 : Registers;
  assert (IccVotedMask == (Qup0 ? 56 : 7));
  UINTN Offset = Address - (Qup0 ? 0x994000 : 0xa84000);
  assert (Offset < sizeof (Registers));
  if (Offset == SE_M_IRQ_CLEAR) Regs[SE_M_IRQ_STATUS / 4] &= ~Value;
  else Regs[Offset / 4] = Value;
  if (Offset == SE_M_CMD0) {
    ++CommandCount;
    if (Qup0 && (!(PinControl[206] & BIT12) || !(PinControl[207] & BIT12))) {
      /* GENI loses arbitration while its selected pads are remote-owned. */
      Regs[SE_M_IRQ_STATUS / 4] = SE_M_CMD_DONE | SE_M_CMD_FAILURE | SE_M_GP_IRQ_4;
      return Value;
    }
    if ((Value >> 27) == I2C_CMD_READ) {
      /* A zero ready-for-receive threshold backpressures even an empty
         FIFO. A short chip-ID read must remain receivable below RX_WM. */
      if (Regs[SE_RX_RFR_WATERMARK / 4] != 0) {
        Regs[SE_RX_FIFO_STATUS / 4] = DropReadData ? 0 : 1;
        Regs[SE_M_IRQ_STATUS / 4] = SE_M_CMD_DONE | SE_M_RX_FIFO_LAST;
      }
    } else Regs[SE_M_IRQ_STATUS / 4] = SE_M_CMD_DONE;
    if ((Value >> 27) == InjectCommand) {
      Regs[SE_M_IRQ_STATUS / 4] |= InjectIrq;
      if (DropDone) Regs[SE_M_IRQ_STATUS / 4] &= ~SE_M_CMD_DONE;
    }
  }
  if (Offset == SE_M_CMD_CTRL) { ++AbortCount; Regs[SE_M_IRQ_STATUS / 4] = SE_M_CMD_ABORT; }
  return Value;
}
UINTN EFIAPI MicroSecondDelay (UINTN Delay) { return Delay; }
static void TestClockCleanup(void) {
  I2C_CR_INSTANCE Instance={0};Instance.Protocol.Instance=10;Instance.Target=&mSm8450I2c[0];
  for(UINTN Q=0;Q<2;++Q) for(UINTN I=0;I<3;++I) ClockRefs[Q][I]=1;
  BootServices.LocateProtocol=Locate;
  ClockFailAcquire=ClockFailRelease=2;ClockRetainFailure=TRUE;
  assert(I2cCrFifoPrepare(&Instance,400)==EFI_TIMEOUT && Instance.ClockMask==7 && Instance.ClockCleanupPending);
  UINT32 Acquires=ClockCalls[1];
  assert(I2cCrFifoPrepare(&Instance,400)==EFI_TIMEOUT && ClockCalls[1]==Acquires);
  ClockFailRelease=1;
  assert(I2cCrFifoPrepare(&Instance,400)==EFI_TIMEOUT && Instance.ClockMask==3 && ClockCalls[1]==Acquires);
  assert(ClockRefs[1][2]==1 && ClockRefs[1][1]==2 && ClockRefs[1][0]==2);
  ClockFailAcquire=ClockFailRelease=MAX_UINT32;
  assert(I2cCrReleaseClocks(&Instance,I2cCrTarget(&Instance))==EFI_SUCCESS && !Instance.ClockMask);
  for(UINTN I=0;I<3;++I) assert(ClockRefs[1][I]==1);
  ClockFailAcquire=1;ClockRetainFailure=FALSE;
  assert(I2cCrFifoPrepare(&Instance,400)==EFI_TIMEOUT && !Instance.ClockMask && !Instance.ClockCleanupPending);
  for(UINTN I=0;I<3;++I) assert(ClockRefs[1][I]==1);
  ClockFailAcquire=MAX_UINT32;ClockCalls[0]=ClockCalls[1]=ClockReleases=0;
  puts("FIFO clocks: retained acquire, reverse cleanup retry and foreign votes passed");
}
int main (void) {
  I2C_CR_INSTANCE Instance = {0};
  MU_I2C_SLAVE_CONFIG Config = {.SlaveAddress = 0x2b, .BusFrequencyKHz = 400,
                               .Mode = MU_I2C_MODE_I2C};
  UINT8 Register = 0, Id = 0;
  MU_I2C_DESCRIPTOR Descriptors[] = {
    {&Register, 1, MU_I2C_FLAG_START | MU_I2C_FLAG_WRITE},
    {&Id, 1, MU_I2C_FLAG_START | MU_I2C_FLAG_READ | MU_I2C_FLAG_STOP}
  };
  UINT32 Transferred;
  TestOwnership ();
  TestClockCleanup();
  PinContext=CrTargetGetGpioContext();
  assert(GpioLibInit(&PinContext)==CR_SUCCESS);
  BootServices.LocateProtocol = Locate;
  Instance.Protocol.Instance = 10;Instance.Target=&mSm8450I2c[0];
  Registers[SE_HW_PARAM_0 / 4] = 16U << 16;
  for (UINT32 Protocol = 0; Protocol < 3; ++Protocol) {
    Registers[SE_GENI_FW_REVISION_RO / 4] = Protocol << 8;
    assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
        NULL, NULL, 0, &Transferred) == EFI_UNSUPPORTED);
    assert (Transferred == 0 && CommandCount == 0 && mSeOwners[1][1].Busy == 0);
  }
  Registers[SE_GENI_FW_REVISION_RO / 4] = 3U << 8;
  Registers[SE_GENI_IF_DISABLE_RO / 4] = BIT (0);
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_UNSUPPORTED);
  assert (Transferred == 0 && CommandCount == 0 && mSeOwners[1][1].Busy == 0);
  Registers[SE_GENI_IF_DISABLE_RO / 4] = 0;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_SUCCESS);
  assert (Id == 0x11 && Transferred == 2 && CommandCount == 2);
  assert (Registers[SE_RX_WATERMARK / 4] == 15);
  assert (Registers[SE_RX_RFR_WATERMARK / 4] == 16);
  /* geni_se_init() common error/flow IRQs must remain enabled while the
     protocol-specific I2C and FIFO service bits are active. */
  assert ((Registers[SE_M_IRQ_EN / 4] &
      (BIT (4) | BIT (6) | BIT (22) | BIT (23) | BIT (24) |
       BIT (25) | BIT (28) | BIT (29))) ==
      (BIT (4) | BIT (6) | BIT (22) | BIT (23) | BIT (24) |
       BIT (25) | BIT (28) | BIT (29)));
  assert (mSeOwners[1][1].Busy == 0);
  /* Exercise the real repeated-start adapter and error cleanup: terminal
     read NACK is valid, address/write NACK and independent GP errors are not. */
  InjectCommand = I2C_CMD_READ;
  InjectIrq = SE_M_GP_IRQ_1 | SE_M_CMD_FAILURE;
  Registers[SE_M_GP_LENGTH / 4] = 1;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_SUCCESS);
  assert (Transferred == 2 && Id == 0x11 && AbortCount == 0);
  Registers[SE_M_GP_LENGTH / 4] = 0;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_DEVICE_ERROR);
  assert (Transferred == 1 && AbortCount == 1);
  Registers[SE_M_GP_LENGTH / 4] = 1;
  InjectCommand = I2C_CMD_WRITE;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_DEVICE_ERROR);
  assert (Transferred == 0 && AbortCount == 2);
  InjectCommand = I2C_CMD_READ;
  UINT32 Errors[] = {SE_M_GP_IRQ_0, SE_M_GP_IRQ_3, SE_M_GP_IRQ_4,
                    SE_M_CMD_OVERRUN, SE_M_ILLEGAL_CMD, SE_M_CMD_ABORT};
  for (UINTN i = 0; i < ARRAY_SIZE (Errors); ++i) {
    InjectIrq = SE_M_GP_IRQ_1 | SE_M_CMD_FAILURE | Errors[i];
    assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
        NULL, NULL, 0, &Transferred) == EFI_DEVICE_ERROR);
    assert (Transferred == 1 && mSeOwners[1][1].Busy == 0);
  }
  InjectIrq = SE_M_CMD_FAILURE; /* An unexplained failure is never ignored. */
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_DEVICE_ERROR);
  assert (Transferred == 1 && mSeOwners[1][1].Busy == 0);
  InjectIrq = SE_M_GP_IRQ_1 | SE_M_CMD_FAILURE;
  DropReadData = TRUE;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_DEVICE_ERROR);
  assert (Transferred == 1 && mSeOwners[1][1].Busy == 0);
  DropReadData = FALSE;
  DropDone = TRUE;
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_TIMEOUT);
  assert (Transferred == 1 && mSeOwners[1][1].Busy == 0);
  DropDone = FALSE;
  InjectIrq = InjectCommand = 0;
  I2C_CR_INSTANCE Instance0 = {0};
  Instance0.Protocol.Instance = 6;Instance0.Target=&mSm8450I2c[1];
  PinControl[206] = PinControl[207] = BIT11; /* EGPIO present, remotely owned */
  Registers0[SE_GENI_FW_REVISION_RO / 4] = 3U << 8;
  Registers0[SE_HW_PARAM_0 / 4] = 8U << 16;
  assert (I2cCrFifoMuTransfer (&Instance0.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_SUCCESS);
  assert (Transferred == 2 && PinMask == 3 && ClockCalls[0] == 3 && ClockCalls[1] == 3);
  assert ((PinControl[206]&(BIT11|BIT12))==(BIT11|BIT12));
  assert ((PinControl[207]&(BIT11|BIT12))==(BIT11|BIT12));
  assert (Instance0.FifoDepth == 8 && Instance.FifoDepth == 16);
  assert (Registers0[SE_RX_RFR_WATERMARK / 4] == 8 && Registers[SE_RX_RFR_WATERMARK / 4] == 16);
  assert (!mSeOwners[0][5].Busy && !mSeOwners[1][1].Busy && !IccAcquiredMask);
  DropEgpioOwner=TRUE; PinControl[206]=PinControl[207]=BIT11;
  UINT32 BeforeCommands=CommandCount;
  assert(I2cCrFifoMuTransfer(&Instance0.Protocol,&Config,Descriptors,2,
       NULL,NULL,0,&Transferred)==EFI_DEVICE_ERROR);
  assert(Transferred==0 && CommandCount==BeforeCommands && !mSeOwners[0][5].Busy);
  DropEgpioOwner=FALSE;
  assert(I2cCrFifoMuTransfer(&Instance0.Protocol,&Config,Descriptors,2,
       NULL,NULL,0,&Transferred)==EFI_SUCCESS && Transferred==2);
  assert (I2cCrFifoMuTransfer (&Instance.Protocol, &Config, Descriptors, 2,
      NULL, NULL, 0, &Transferred) == EFI_SUCCESS);
  assert (ClockCalls[0] == 3 && ClockCalls[1] == 3 && Instance.FifoDepth == 16);
  puts ("FIFO: receive thresholds, repeated-START chip ID and address/data NACK handling passed");
  return 0;
}
