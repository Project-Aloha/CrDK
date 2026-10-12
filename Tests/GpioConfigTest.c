/* Configuration must reject malformed ACPI before publishing partial state.
 * SPDX-License-Identifier: MIT */
#include <Library/GpioConfigLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "GpioConfigFixture.h"

static GpioDeviceContext Gpio, Saved;
static PdcDeviceContext Pdc, PdcSaved;
// Route/table resolution is strictly read-only and must not touch hardware.
#ifdef _KERNEL_MODE
UINT32 CrMmioRead32(UINTN A) { (VOID)A; assert(0); return 0; }
UINT32 CrMmioWrite32(UINTN A,UINT32 V) { (VOID)A;(VOID)V; assert(0); return 0; }
#else
UINT32 EFIAPI MmioRead32(UINTN A) { (VOID)A; assert(0); return 0; }
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V) { (VOID)A;(VOID)V; assert(0); return 0; }
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...) { (VOID)L;(VOID)F; }
#endif
static CR_GPIO_BANK_ROUTE Routes[GPIO_PINS_MAX], BeforeRoutes[GPIO_PINS_MAX];
static void RejectRoutes(const CR_PEP_VALUE *DirectPins,const UINT16 *Gsis,UINT16 Count) {
  memcpy(BeforeRoutes,Routes,sizeof(Routes));
  assert(CrGpioPdcRoutesLoad(&Gpio,&Pdc,DirectPins,Gsis,Count,Routes)==CR_INVALID_PARAMETER);
  assert(memcmp(BeforeRoutes,Routes,sizeof(Routes))==0);
}
static void Rejected(const CR_PEP_VALUE *Registers, const CR_PEP_VALUE *Pins,
                     const CR_PEP_VALUE *Ranges, const CR_PEP_VALUE *Reserved, UINTN Size) {
  Saved = Gpio; PdcSaved = Pdc;
  assert(CrGpioConfigLoad(Registers, &Tiles, Pins, Ranges, Reserved, Size, &Gpio, &Pdc) ==
         CR_INVALID_PARAMETER);
  assert(memcmp(&Gpio, &Saved, sizeof(Gpio)) == 0);
  assert(memcmp(&Pdc, &PdcSaved, sizeof(Pdc)) == 0);
}
int main(void) {
  Gpio.TlmmBaseAddress = 0x12345600;
  Pdc.BaseAddress = 0xabcdef00; Pdc.Size = 0x30000;
  assert(CrGpioConfigLoad(&Registers, &Tiles, &Pins, &Ranges, &Reserved, 0x1000000,
                          &Gpio, &Pdc) == CR_SUCCESS);
  assert(Gpio.PinCount == 214 && Pdc.PdcRangeCount == 6);
  assert(Gpio.TlmmBaseAddress == 0x12345600 && Pdc.BaseAddress == 0xabcdef00);
  assert(Gpio.RegCfg[0].MuxSelMsk == 0x3c && Gpio.RegCfg[0].InterruptDetectionMsk == 0xc);
  assert(Gpio.RegCfg[1].ControlRegOffset == 0x1de000 && Gpio.RegCfg[1].IoRegOffset == 0x1de004);
  assert(Gpio.GpioPins[210].pRegCfg == &Gpio.RegCfg[1]);
  assert(Gpio.GpioPins[212].WakeBit == 0xff && Gpio.GpioPins[212].WakeRegOffset == 0xffffffff);
  assert(Gpio.GpioPins[92].PdcPinNumber == 46 && Gpio.GpioPins[92].pTileInfo == &Gpio.Tiles[0]);
  assert(Gpio.GpioPins[28].Reserved && Gpio.GpioPins[31].Reserved && !Gpio.GpioPins[32].Reserved);

  CR_PEP_VALUE WrappedRegisters = {.Type=CrPepPackage,.Package={&Registers,1}};
  CR_PEP_VALUE WrappedTiles = {.Type=CrPepPackage,.Package={&Tiles,1}};
  CR_PEP_VALUE WrappedPins = {.Type=CrPepPackage,.Package={&Pins,1}};
  CR_PEP_VALUE WrappedRanges = {.Type=CrPepPackage,.Package={&Ranges,1}};
  CR_PEP_VALUE WrappedReserved = {.Type=CrPepPackage,.Package={&Reserved,1}};
  Saved = Gpio; PdcSaved = Pdc;
  assert(CrGpioConfigLoad(&WrappedRegisters,&WrappedTiles,&WrappedPins,&WrappedRanges,
                        &WrappedReserved,0x1000000,&Gpio,&Pdc) == CR_SUCCESS);
  assert(memcmp(&Gpio,&Saved,sizeof(Gpio)) == 0 && memcmp(&Pdc,&PdcSaved,sizeof(Pdc)) == 0);

  CR_PEP_VALUE PinRows[GPIO_PINS_MAX], PinFields[7], BadPins = Pins;
  memcpy(PinRows,Pins.Package.Values,Pins.Package.Count*sizeof(*PinRows));
  memcpy(PinFields,PinRows[212].Package.Values,sizeof(PinFields));
  PinRows[212].Package.Values = PinFields; BadPins.Package.Values = PinRows;
  PinFields[0].Integer = 0x100000000ULL; Rejected(&Registers,&BadPins,&Ranges,&Reserved,0x1000000);
  PinFields[0].Integer = 0; Rejected(&Registers,&BadPins,&Ranges,&Reserved,0x1000000); // Duplicate.
  PinFields[0].Integer = 212;
  PinFields[2].Integer = 15; Rejected(&Registers,&BadPins,&Ranges,&Reserved,0x1000000); // Undefined cfg.
  PinFields[2].Integer = 1;
  PinFields[4].Integer = 32; Rejected(&Registers,&BadPins,&Ranges,&Reserved,0x1000000);
  Rejected(&Registers,&Pins,&Ranges,&Reserved,0x10000); // Mapped register bounds.
  Rejected(&Registers,&Pins,&Ranges,&Pins,0x1000000); // Wrong optional schema.
  CR_PEP_VALUE Integer = {.Type=CrPepInteger,.Integer=0};
  Rejected(&Registers,&Pins,&Ranges,&Integer,0x1000000);
  CR_PEP_VALUE BadRegisters = Registers, RegRows[GPIO_MAX_REG_CFG], RegFields[21];
  memcpy(RegRows,Registers.Package.Values,Registers.Package.Count*sizeof(*RegRows));
  memcpy(RegFields,RegRows[0].Package.Values,sizeof(RegFields));
  BadRegisters.Package.Values = RegRows; RegRows[0].Package.Values = RegFields;
  RegFields[5].Integer = 31; Rejected(&BadRegisters,&Pins,&Ranges,&Reserved,0x1000000);
  RegFields[5].Integer = 2; RegFields[20].Integer = 3;
  Rejected(&BadRegisters,&Pins,&Ranges,&Reserved,0x1000000);
  CR_PEP_VALUE BadRanges = Ranges, RangeRows[PDC_RANGE_COUNT_MAX], RangeFields[3];
  memcpy(RangeRows,Ranges.Package.Values,Ranges.Package.Count*sizeof(*RangeRows));
  memcpy(RangeFields,RangeRows[5].Package.Values,sizeof(RangeFields));
  RangeRows[5].Package.Values = RangeFields; BadRanges.Package.Values = RangeRows;
  RangeFields[0].Integer = 94; Rejected(&Registers,&Pins,&BadRanges,&Reserved,0x1000000);
  RangeFields[0].Integer = 126; RangeFields[1].Integer = 987;
  Rejected(&Registers,&Pins,&BadRanges,&Reserved,0x1000000);
  assert(CrGpioConfigLoad(&Registers,&Tiles,&Pins,&Ranges,NULL,0x1000000,&Gpio,&Pdc) == CR_SUCCESS);
  assert(!Gpio.GpioPins[28].Reserved);
  UINT32 Banks=99, Total=99; UINT64 Valid=0; UINT16 Physical=0xffff;
  UINT16 Active[2]={92,63};
  assert(CrGpioBankLayout(214,2,&Banks,&Total)==CR_SUCCESS && Banks==4 && Total==321);
  assert(CrGpioBankLayout(214,0,&Banks,&Total)==CR_SUCCESS && Total==214);
  assert(CrGpioBankMask(214,2,0,&Valid)==CR_SUCCESS && Valid==CR_MAX_UINT64);
  assert(CrGpioBankMask(214,2,3,&Valid)==CR_SUCCESS && Valid==0x3fffff);
  assert(CrGpioBankMask(214,2,5,&Valid)==CR_SUCCESS && Valid==1);
  assert(CrGpioBankMask(214,2,6,&Valid)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankPin(214,Active,2,0,63,&Physical)==CR_SUCCESS && Physical==63);
  assert(CrGpioResolveBankPin(214,Active,2,3,21,&Physical)==CR_SUCCESS && Physical==213);
  assert(CrGpioResolveBankPin(214,Active,2,3,22,&Physical)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankPin(214,Active,2,4,0,&Physical)==CR_SUCCESS && Physical==92);
  assert(CrGpioResolveBankPin(214,Active,2,5,0,&Physical)==CR_SUCCESS && Physical==63);
  Physical=0xffff;
  assert(CrGpioResolveBankPin(214,Active,2,5,1,&Physical)==CR_INVALID_PARAMETER && Physical==0xffff);
  assert(CrGpioResolveBankPin(214,Active,2,0,64,&Physical)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankPin(214,NULL,2,4,0,&Physical)==CR_INVALID_PARAMETER);
  Active[1]=214;
  assert(CrGpioResolveBankPin(214,Active,2,5,0,&Physical)==CR_INVALID_PARAMETER);
  assert(CrGpioBankLayout(0,2,&Banks,&Total)==CR_INVALID_PARAMETER);
  assert(CrGpioBankLayout(301,2,&Banks,&Total)==CR_INVALID_PARAMETER);
  UINT16 Mapping[64], Before[64]; memset(Mapping,0xee,sizeof(Mapping));
  memcpy(Before,Mapping,sizeof(Mapping));
  assert(CrGpioResolveBankMask(214,Active,2,3,1ULL<<63,Mapping)==CR_INVALID_PARAMETER);
  assert(memcmp(Mapping,Before,sizeof(Mapping))==0);
  assert(CrGpioResolveBankMask(214,Active,2,5,1,Mapping)==CR_INVALID_PARAMETER);
  assert(memcmp(Mapping,Before,sizeof(Mapping))==0);
  assert(CrGpioResolveBankMask(214,Active,2,0,(1ULL<<63)|1,Mapping)==CR_SUCCESS);
  assert(Mapping[0]==0 && Mapping[1]==0xffff && Mapping[63]==63);
  assert(CrGpioResolveBankMask(214,Active,2,4,1,Mapping)==CR_SUCCESS && Mapping[0]==92);
  assert(CrGpioResolveBankMask(214,Active,2,4,2,Mapping)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankMask(214,Active,2,4,0,NULL)==CR_INVALID_PARAMETER);
  UINT16 Gsis[3]={558,603,513};
  assert(CrGpioPdcRoutesLoad(&Gpio,&Pdc,NULL,Gsis,2,Routes)==CR_SUCCESS);
  assert(Routes[0].PhysicalPin==92 && Routes[0].PdcPin==46);
  assert(Routes[1].PhysicalPin==63 && Routes[1].PdcPin==91);
  assert(CrGpioPdcRoutesLoad(&Gpio,&Pdc,&Direct,Gsis,3,Routes)==CR_SUCCESS);
  assert(Routes[2].PhysicalPin==0xffff && Routes[2].PdcPin==1 && Gpio.PinCount==214);
  CR_PEP_VALUE WrappedDirect={.Type=CrPepPackage,.Package={&Direct,1}};
  assert(CrGpioPdcRoutesLoad(&Gpio,&Pdc,&WrappedDirect,Gsis,3,Routes)==CR_SUCCESS);
  assert(CrGpioBankLayout(214,3,&Banks,&Total)==CR_SUCCESS && Banks==4 && Total==385);
  CR_GPIO_BANK_ROUTE Resolved={0xabcd,0xdcba};
  assert(CrGpioResolveBankRoute(214,Routes,3,6,0,&Resolved)==CR_SUCCESS);
  assert(Resolved.PhysicalPin==0xffff && Resolved.PdcPin==1);
  assert(CrGpioResolveBankRoute(214,Routes,3,4,0,&Resolved)==CR_SUCCESS);
  assert(Resolved.PhysicalPin==92 && Resolved.PdcPin==46);
  assert(CrGpioResolveBankRoute(214,Routes,3,0,63,&Resolved)==CR_SUCCESS);
  assert(Resolved.PhysicalPin==63 && Resolved.PdcPin==0xffff);
  Resolved=(CR_GPIO_BANK_ROUTE){0xabcd,0xdcba};
  assert(CrGpioResolveBankRoute(214,Routes,3,6,1,&Resolved)==CR_INVALID_PARAMETER);
  assert(Resolved.PhysicalPin==0xabcd && Resolved.PdcPin==0xdcba);
  assert(CrGpioResolveBankRoute(214,Routes,3,0,64,&Resolved)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankRoute(214,Routes,3,7,0,&Resolved)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankRoute(214,Routes,3,3,22,&Resolved)==CR_INVALID_PARAMETER);
  CR_GPIO_BANK_ROUTE Batch[64], BeforeBatch[64];
  memset(Batch,0xab,sizeof(Batch)); memcpy(BeforeBatch,Batch,sizeof(Batch));
  assert(CrGpioResolveBankRouteMask(214,Routes,3,6,3,Batch)==CR_INVALID_PARAMETER);
  assert(memcmp(Batch,BeforeBatch,sizeof(Batch))==0);
  assert(CrGpioResolveBankRouteMask(214,NULL,3,6,0,Batch)==CR_INVALID_PARAMETER);
  assert(CrGpioResolveBankRouteMask(214,Routes,3,6,1,Batch)==CR_SUCCESS);
  assert(Batch[0].PhysicalPin==0xffff && Batch[0].PdcPin==1 && Batch[1].PdcPin==0xffff);
  UINT16 Active3[3]={92,63,0xffff}; Physical=0xaaaa;
  assert(CrGpioResolveBankPin(214,Active3,3,6,0,&Physical)==CR_INVALID_PARAMETER && Physical==0xaaaa);
  RejectRoutes(NULL,Gsis,3); // Non-TLMM route requires explicit ownership.
  RejectRoutes(&Direct,Gsis,2); // Whitelist cannot advertise an absent IRQ.
  RejectRoutes(&Integer,Gsis,3);
  CR_PEP_VALUE DirectPins[2]={{.Type=CrPepInteger,.Integer=46},{.Type=CrPepInteger,.Integer=1}};
  CR_PEP_VALUE BadDirect={.Type=CrPepPackage,.Package={DirectPins,1}};
  RejectRoutes(&BadDirect,Gsis,3); // GPIO-backed pin must not masquerade as direct.
  DirectPins[0].Integer=0x100000001ULL; RejectRoutes(&BadDirect,Gsis,3);
  DirectPins[0].Integer=0xffff; RejectRoutes(&BadDirect,Gsis,3);
  DirectPins[0].Integer=13; RejectRoutes(&BadDirect,Gsis,3); // PDC range hole.
  DirectPins[0].Integer=1; BadDirect.Package.Count=2; RejectRoutes(&BadDirect,Gsis,3);
  Gsis[2]=558; RejectRoutes(&Direct,Gsis,3);
  Gsis[2]=0xffff; RejectRoutes(&Direct,Gsis,3);
  Gsis[2]=513; Gsis[1]=559; RejectRoutes(&Direct,Gsis,3); // Separate routes, same RMW words.
  Gsis[1]=603;
  RejectRoutes(&Direct,Gsis,GPIO_PINS_MAX+1);
  RejectRoutes(&Direct,NULL,3);
  assert(CrGpioPdcRoutesLoad(&Gpio,&Pdc,&Direct,Gsis,3,NULL)==CR_INVALID_PARAMETER);
  Gpio.GpioPins[92].Reserved=TRUE; RejectRoutes(&Direct,Gsis,3); Gpio.GpioPins[92].Reserved=FALSE;
  // Isolate shared-SPI and shared-enable conflicts using distinct ranges/pins.
  GpioDeviceContext Small={.PinCount=1}; Small.GpioPins[0].PdcPinNumber=0xffff;
  PdcDeviceContext SmallPdc={.PdcRangeCount=2,.PdcRegion={{1,481,1},{32,483,1}}};
  DirectPins[0].Integer=1; DirectPins[1].Integer=32;
  UINT16 SmallGsis[2]={513,515}; memcpy(BeforeRoutes,Routes,sizeof(Routes));
  assert(CrGpioPdcRoutesLoad(&Small,&SmallPdc,&BadDirect,SmallGsis,2,Routes)==CR_INVALID_PARAMETER);
  assert(memcmp(BeforeRoutes,Routes,sizeof(Routes))==0); // Different enable words, shared SPI word.
  SmallPdc.PdcRegion[1]=(PdcRange){2,550,1}; DirectPins[1].Integer=2; SmallGsis[1]=582;
  assert(CrGpioPdcRoutesLoad(&Small,&SmallPdc,&BadDirect,SmallGsis,2,Routes)==CR_INVALID_PARAMETER);
  assert(memcmp(BeforeRoutes,Routes,sizeof(Routes))==0); // Different SPI words, shared enable word.
  puts("Actual SM8450 GPIO ACPI: 214 pins, special-pad offsets, wrapped/flat tables, atomic rejection passed");
  puts("PDC virtual routes: GPIO92/63 and peripheral-direct pin1, distinct DIRQL RMW words passed");
  return 0;
}
