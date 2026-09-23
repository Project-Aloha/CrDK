#include <Library/CrTargetPdcLib.h>

STATIC PdcDeviceContext PdcDevContext = {
    .BaseAddress   = 0xb220000,
    .Size          = 0x30000,
    .SPIConfigBase = 0x174000f0,
    .SPIConfigSize = 0x64,
    .PdcRangeCount = 6,
    .PdcRegion =
        {{.PdcPinBase = 0, .GicSpiBase = 480, .PdcCount = 12},
         {.PdcPinBase = 14, .GicSpiBase = 494, .PdcCount = 24},
         {.PdcPinBase = 40, .GicSpiBase = 520, .PdcCount = 54},
         {.PdcPinBase = 94, .GicSpiBase = 609, .PdcCount = 31},
         {.PdcPinBase = 125, .GicSpiBase = 63, .PdcCount = 1},
         {.PdcPinBase = 126, .GicSpiBase = 716, .PdcCount = 12}},
};

PdcDeviceContext *GetPdcDevContext() { return &PdcDevContext; }
