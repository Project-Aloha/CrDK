/** @file
 *  Host regression coverage for the Crane-owned I2C adapter.
 *
 *  Native SE1 FIFO behavior is covered by I2cFifoTransportTest.  This test
 *  covers the native-only protocol boundary and verifies that the driver does
 *  not discover or invoke a packaged Qualcomm MU or standard PI backend.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "MuBusTestCommon.h"
#include <Library/CrTargetQupLib.h>

EFI_GUID gEfiQupCrProtocolGuid;
EFI_GUID gEfiI2cCrProtocolGuid;
EFI_GUID gEfiClockCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid;
#include "../Target/Sm8450/CrQupTarget.c"
#define TestContext mSm8450QupContext
static UINT32 TargetIoCalls;
UINT32 EFIAPI MmioRead32(UINTN A) {(VOID)A;++TargetIoCalls;return 0;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) {(VOID)A;++TargetIoCalls;return V;}
UINTN EFIAPI MicroSecondDelay(UINTN Us) {return Us;}
#define CRANE_MMIO_TEST 1
CONST CR_TARGET_QUP_CONTEXT *CrDalGetQupContext (VOID) { return &TestContext; }
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }

#include "../Driver/QUPCrDxe/QUPCrDxe.c"
/* Publication-only fixture; native mapped transfers use the actual GENI
   composition tests with qualified GCD descriptors. */
EFI_DXE_SERVICES *gDS = NULL;
#include "../Driver/I2CCrDxe/I2CCrDxe.c"

#define TEST_CONFIGURE_PINS() I2cCrConfigurePins (&TestContext.I2c[0])
#include "GeniPinmuxTest.h"

static EFI_STATUS EFIAPI LocateNativeQup (
    EFI_GUID *Guid, VOID *Registration, VOID **Interface) {
  assert (Registration == NULL);
  if (Guid == &gEfiQupCrProtocolGuid) {
    *Interface = &mQupProtocol;
    return EFI_SUCCESS;
  }
  *Interface = NULL;
  return EFI_NOT_FOUND;
}

static unsigned PublishAttempts, FailPublishAt, RemoveCalls;
static EFI_STATUS RemoveStatus;
static EFI_I2C_CR_PROTOCOL *Published[2];
static EFI_STATUS EFIAPI PublishNative (EFI_HANDLE *Handle, ...) {
  va_list Args;
  assert (*Handle == NULL);
  va_start (Args, Handle);
  assert (va_arg (Args, EFI_GUID *) == &gEfiI2cCrProtocolGuid);
  EFI_I2C_CR_PROTOCOL *Protocol = va_arg (Args, VOID *);
  assert (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  if (++PublishAttempts == FailPublishAt) return EFI_DEVICE_ERROR;
  assert (PublishAttempts <= 2);
  Published[PublishAttempts - 1] = Protocol;
  *Handle = (EFI_HANDLE)(UINTN)PublishAttempts;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RemoveNative (EFI_HANDLE Handle, ...) {
  va_list Args;
  UINTN Index = (UINTN)Handle - 1;
  assert (Index < 2 && Published[Index] != NULL);
  va_start (Args, Handle);
  assert (va_arg (Args, EFI_GUID *) == &gEfiI2cCrProtocolGuid);
  assert (va_arg (Args, VOID *) == Published[Index]);
  assert (va_arg (Args, VOID *) == NULL);
  va_end (Args);
  ++RemoveCalls;
  if (!EFI_ERROR (RemoveStatus)) Published[Index] = NULL;
  return RemoveStatus;
}
static void ResetPublication (void) {
  PublishAttempts = RemoveCalls = FailPublishAt = 0;
  RemoveStatus = EFI_SUCCESS;
  ZeroMem (Published, sizeof (Published));
  BootServices.InstallMultipleProtocolInterfaces = PublishNative;
  BootServices.UninstallMultipleProtocolInterfaces = RemoveNative;
}
static void FreePublished (void) {
  for (UINTN Index = 0; Index < 2; ++Index)
    if (Published[Index] != NULL)
      assert (RemoveNative ((EFI_HANDLE)(Index + 1), &gEfiI2cCrProtocolGuid,
                            Published[Index], NULL) == EFI_SUCCESS);
  FreePool (mInstances);
  mInstances = NULL;
}

static void TestNativeEntry (void) {
  mQupContext = &TestContext;
  OptionalQupGuid = &gEfiQupCrProtocolGuid;
  PublishedGuid = &gEfiI2cCrProtocolGuid;
  InstallStatus = EFI_SUCCESS;
  InitEntryServices ();
  BootServices.LocateProtocol = LocateNativeQup;
  ResetPublication ();
  assert (I2CCrEntryPoint ((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  assert (mInstances != NULL && mInstances[0].Protocol.Instance == 10);
  assert (mInstances[1].Protocol.Instance == 6 && PublishAttempts == 2);
  assert (Published[0] == &mInstances[0].Protocol && Published[1] == &mInstances[1].Protocol);
  assert (PrivateLocateCalls == 0);
  assert (I2CCrEntryPoint ((EFI_HANDLE)0xaaaa, NULL) == EFI_ALREADY_STARTED);
  FreePublished ();
  for (FailPublishAt = 1; FailPublishAt <= 2; ++FailPublishAt) {
    PublishAttempts = RemoveCalls = 0;
    assert (I2CCrEntryPoint ((EFI_HANDLE)0xaaaa, NULL) == EFI_DEVICE_ERROR);
    assert (mInstances == NULL && Allocations == 0);
    assert (RemoveCalls == FailPublishAt - 1);
    assert (Published[0] == NULL && Published[1] == NULL);
  }
  ResetPublication ();
  FailPublishAt = 2;
  RemoveStatus = EFI_ACCESS_DENIED;
  assert (I2CCrEntryPoint ((EFI_HANDLE)0xaaaa, NULL) == EFI_SUCCESS);
  assert (mInstances != NULL && Published[0] == &mInstances[0].Protocol);
  assert (Published[0]->SetBusFrequency != NULL && Allocations == 1);
  RemoveStatus = EFI_SUCCESS;
  FreePublished ();
}

static void TestTargetPublication(void) {
  CR_TARGET_QUP_CONTEXT Saved=TestContext;
  CR_TARGET_QUP_I2C_ENDPOINT Endpoints[2];
  BootServices.LocateProtocol=LocateNativeQup;
  for(UINT32 Fault=0;Fault<20;++Fault) {
    ResetPublication();TargetIoCalls=0;
    memcpy(Endpoints,Saved.I2c,sizeof(Endpoints));
    TestContext=Saved;TestContext.I2c=Endpoints;
    switch(Fault) {
      case 0:Endpoints[1].Instance=Endpoints[0].Instance;break;
      case 1:Endpoints[1].Wrapper=1;Endpoints[1].Engine=1;Endpoints[1].Base=Endpoints[0].Base;break;
      case 2:Endpoints[1].Wrapper=255;break;
      case 3:Endpoints[1].Engine=8;break;
      case 4:Endpoints[1].Base+=4;break;
      case 5:Endpoints[1].Size-=4;break;
      case 6:Endpoints[1].SourceClockHz=20000000;break;
      case 7:Endpoints[1].DefaultFrequencyKHz=200;break;
      case 8:Endpoints[1].Clocks[2]=NULL;break;
      case 9:Endpoints[1].Clocks[1]="";break;
      case 10:Endpoints[1].ClockController=NULL;break;
      case 11:Endpoints[1].Pins[0].Function=GPIO_FUNC_UNCHANGE;break;
      case 12:Endpoints[1].Pins[0].Pin=GPIO_PINS_MAX;break;
      case 13:Endpoints[1].Pins[1].Pin=Endpoints[1].Pins[0].Pin;break;
      case 14:Endpoints[1].Pins[0].Pull=(GpioPullType)-1;break;
      case 15:Endpoints[1].Pins[0].DriveStrength=(GpioDriveStrength)8;break;
      case 16:TestContext.I2cCount=CR_TARGET_QUP_MAX_I2C_ENDPOINTS+1;break;
      case 17:TestContext.I2c=NULL;break;
      case 18:Endpoints[1].Base=MAX_UINT64-3;break;
      case 19:Endpoints[1].Pins[0].Pin=Endpoints[0].Pins[1].Pin;break;
    }
    assert(I2CCrEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA);
    assert(!PublishAttempts && !TargetIoCalls && !Allocations && !mInstances);
  }
  TestContext=Saved;TestContext.I2c=NULL;TestContext.I2cCount=0;
  assert(I2CCrEntryPoint(NULL,NULL)==EFI_NOT_FOUND && !PublishAttempts && !TargetIoCalls);
  TestContext=Saved;
  EFI_QUP_CR_GET_CONTEXT Get=mQupProtocol.GetContext;
  mQupProtocol.GetContext=NULL;
  assert(I2CCrEntryPoint(NULL,NULL)==EFI_UNSUPPORTED && !PublishAttempts);
  mQupProtocol.GetContext=Get;
  mQupProtocol.Revision=0x10001;
  assert(I2CCrEntryPoint(NULL,NULL)==EFI_UNSUPPORTED && !PublishAttempts);
  mQupProtocol.Revision=0x20000;
  assert(I2CCrEntryPoint(NULL,NULL)==EFI_UNSUPPORTED && !PublishAttempts);
  mQupProtocol.Revision=EFI_QUP_CR_PROTOCOL_REVISION;
  puts("I2C targets: real SM8450 descriptors,20 malformed sets rejected before publication/MMIO, absent endpoints and old/future ABI passed");
}

static void TestEntry (void) {
  StandardGuid = NULL;
  OptionalQupGuid = &gEfiQupCrProtocolGuid;
  PublishedGuid = &gEfiI2cCrProtocolGuid;
  PrivateBackend = NULL;
  InitEntryServices ();

  /* Native discovery fails here.  The entry point must stop instead of
     looking for a private MU or another standard I2C master. */
  assert (I2CCrEntryPoint ((EFI_HANDLE)0xaaaa, NULL) == EFI_NOT_FOUND);
  assert (PrivateLocateCalls == 0 && Allocations == 0 && mInstances == NULL);
}

int main (void) {
  I2C_CR_INSTANCE Instance = { 0 };
  EFI_I2C_REQUEST_PACKET Packet = { 0 };
  UINT8 Buffer[2] = { 0 };
  UINTN Hz;

  InitBootServices ();
  TestNativeEntry ();
  TestTargetPublication();
  TestPinmux ();
  Instance.Protocol.Instance = 1;
  Instance.Protocol.BusFrequencyKHz = 400;
  Packet.OperationCount = 1;
  Packet.Operation[0].LengthInBytes = sizeof (Buffer);
  Packet.Operation[0].Buffer = Buffer;

  /* No native instance means unsupported; this is the intentional boundary
     that prevents a packaged MU fallback. */
  assert (I2cCrStartRequest (&Instance.Protocol, 0x36, &Packet,
                             NULL, NULL) == EFI_UNSUPPORTED);
  assert (I2cCrMuTransfer (&Instance.Protocol, NULL, NULL, 0,
                           NULL, NULL, 0, NULL) == EFI_UNSUPPORTED);

  Hz = 99999;
  assert (I2cCrSetBusFrequency (&Instance.Protocol, &Hz) == EFI_UNSUPPORTED);
  Hz = 399999;
  assert (I2cCrSetBusFrequency (&Instance.Protocol, &Hz) == EFI_SUCCESS &&
          Hz == 100000);
  Hz = 999999;
  assert (I2cCrSetBusFrequency (&Instance.Protocol, &Hz) == EFI_SUCCESS &&
          Hz == 400000);
  Hz = 1000001;
  assert (I2cCrSetBusFrequency (&Instance.Protocol, &Hz) == EFI_SUCCESS &&
          Hz == 1000000);
  assert (I2cCrReset (&Instance.Protocol) == EFI_UNSUPPORTED);

  TestEntry ();
  assert (Allocations == 0);
  puts ("I2C: native-only boundary, pinmux, validation and no-fallback policy passed");
  return 0;
}
