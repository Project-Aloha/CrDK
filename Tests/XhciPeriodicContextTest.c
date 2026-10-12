/* Actual endpoint constructors against independent USB descriptor vectors.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Xhci.h"

STATIC USB_XHCI_INSTANCE Hc;
STATIC INPUT_CONTEXT Context32;
STATIC INPUT_CONTEXT_64 Context64;
STATIC TRANSFER_RING Rings[31];
STATIC TRB_TEMPLATE Trbs[31][TR_RING_TRB_NUMBER];
STATIC UINTN Cases, Maps;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) {}
VOID *EFIAPI AllocateZeroPool(UINTN Size) { assert(0); return NULL; }
UINT64 EFIAPI RShiftU64(UINT64 Value, UINTN Count) { return Value >> Count; }
INTN EFIAPI HighBitSet32(UINT32 Value) {
  INTN Bit = -1;
  while (Value) { ++Bit; Value >>= 1; }
  return Bit;
}
EFI_PHYSICAL_ADDRESS UsbHcGetPciAddrForHostAddr(USBHC_MEM_POOL *Pool,
  VOID *Host, UINTN Size, BOOLEAN AssertMapping) {
  assert(Size == sizeof(Trbs[0]) && AssertMapping); ++Maps;
  return 0x12000000U + ((UINTN)Host - (UINTN)Trbs);
}
VOID CreateTransferRing(USB_XHCI_INSTANCE *Xhc, UINTN Count, TRANSFER_RING *Ring) {
  assert(0); /* Existing qualified ring must be reused, not reallocated. */
}

#include "XhciPeriodicProduction.c"

STATIC VOID Vector(BOOLEAN Wide, UINT8 Speed, UINT8 Type, UINT8 Address,
                   UINT16 Packet, UINT8 Interval, UINT8 ExpectedInterval) {
  struct { USB_INTERFACE_DESCRIPTOR Interface; USB_ENDPOINT_DESCRIPTOR Endpoint; } Descriptor;
  memset(&Hc, 0, sizeof(Hc));
  memset(&Context32, 0, sizeof(Context32));
  memset(&Context64, 0, sizeof(Context64));
  memset(&Descriptor, 0, sizeof(Descriptor));
  Descriptor.Interface.Length = sizeof(Descriptor.Interface);
  Descriptor.Interface.DescriptorType = USB_DESC_TYPE_INTERFACE;
  Descriptor.Interface.NumEndpoints = 1;
  Descriptor.Endpoint.Length = sizeof(Descriptor.Endpoint);
  Descriptor.Endpoint.DescriptorType = USB_DESC_TYPE_ENDPOINT;
  Descriptor.Endpoint.EndpointAddress = Address;
  Descriptor.Endpoint.Attributes = Type;
  Descriptor.Endpoint.MaxPacketSize = Packet;
  Descriptor.Endpoint.Interval = Interval;
  UINT8 Dci = XhcEndpointToDci(Address & 15, (Address & 0x80) ? EfiUsbDataIn : EfiUsbDataOut);
  Rings[Dci-1].RingSeg0 = Trbs[Dci-1];
  Rings[Dci-1].RingPCS = 1;
  Hc.UsbDevContext[1].EndpointTransferRing[Dci-1] = &Rings[Dci-1];
  UINTN PreviousMaps = Maps;
  UINT8 Maximum = Wide ? XhcInitializeEndpointContext64(&Hc, 1, Speed, &Context64, &Descriptor.Interface)
                       : XhcInitializeEndpointContext(&Hc, 1, Speed, &Context32, &Descriptor.Interface);
  assert(Maximum == Dci && Maps == PreviousMaps + 1);
  /* Independent constraints: one LS/FS transaction per ESIT; no bulk
     estimate may be substituted for its expected periodic TRB payload. */
  if (Wide) {
    assert(Context64.InputControlContext.Dword2 == (BIT0 << Dci));
    assert(Context64.EP[Dci-1].MaxPacketSize == Packet);
    assert(Context64.EP[Dci-1].CErr == 3 && Context64.EP[Dci-1].MaxBurstSize == 0);
    assert(Context64.EP[Dci-1].AverageTRBLength == (Type == USB_ENDPOINT_INTERRUPT ? Packet : 4096));
    assert(Context64.EP[Dci-1].Interval == ExpectedInterval);
    assert(Context64.EP[Dci-1].MaxESITPayload == (Type == USB_ENDPOINT_INTERRUPT ? Packet : 0));
    assert(Context64.EP[Dci-1].PtrLo & 1);
  } else {
    assert(Context32.InputControlContext.Dword2 == (BIT0 << Dci));
    assert(Context32.EP[Dci-1].MaxPacketSize == Packet);
    assert(Context32.EP[Dci-1].CErr == 3 && Context32.EP[Dci-1].MaxBurstSize == 0);
    assert(Context32.EP[Dci-1].AverageTRBLength == (Type == USB_ENDPOINT_INTERRUPT ? Packet : 4096));
    assert(Context32.EP[Dci-1].Interval == ExpectedInterval);
    assert(Context32.EP[Dci-1].MaxESITPayload == (Type == USB_ENDPOINT_INTERRUPT ? Packet : 0));
    assert(Context32.EP[Dci-1].PtrLo & 1);
  }
  assert(Hc.UsbDevContext[1].EndpointTransferRing[Dci-1] == &Rings[Dci-1]);
  ++Cases;
}

int main(void) {
  for (BOOLEAN Wide = 0; Wide != 2; ++Wide) {
    Vector(Wide, EFI_USB_SPEED_FULL, USB_ENDPOINT_INTERRUPT, 0x81, 8, 1, 3); /* capture39 wired KB */
    Vector(Wide, EFI_USB_SPEED_FULL, USB_ENDPOINT_INTERRUPT, 0x82, 16, 1, 3); /* composite second EP */
    Vector(Wide, EFI_USB_SPEED_FULL, USB_ENDPOINT_INTERRUPT, 0x03, 64, 10, 6);
    Vector(Wide, EFI_USB_SPEED_FULL, USB_ENDPOINT_INTERRUPT, 0x84, 32, 255, 10);
    Vector(Wide, EFI_USB_SPEED_LOW, USB_ENDPOINT_INTERRUPT, 0x81, 8, 10, 6);
    Vector(Wide, EFI_USB_SPEED_LOW, USB_ENDPOINT_INTERRUPT, 0x02, 4, 255, 10);
    Vector(Wide, EFI_USB_SPEED_FULL, USB_ENDPOINT_BULK, 0x85, 64, 0, 0);
  }
  printf("PASS: %lu actual32/64 LS/FS periodic+bulk descriptor vectors\n", (unsigned long)Cases);
  return 0;
}
