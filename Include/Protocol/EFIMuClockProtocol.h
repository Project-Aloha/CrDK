/** @file
  Qualcomm native clock protocol prefix used by the Crane adapters.

  The Waipio protocol is revision 1.9.  Only the stable slots through
  ResetClock are declared here; later revision-specific slots are deliberately
  left opaque.

  SPDX-License-Identifier: MIT
**/
#ifndef EFI_MU_CLOCK_PROTOCOL_H_
#define EFI_MU_CLOCK_PROTOCOL_H_

#include <Uefi.h>

#define EFI_CLOCK_PROTOCOL_REVISION  0x0000000000010009ULL
#define EFI_CLOCK_PROTOCOL_GUID \
  { 0x241afae6, 0x885f, 0x4f6c, \
    { 0xa7, 0xea, 0xc2, 0x8e, 0xab, 0x79, 0xc3, 0xe5 } }

extern EFI_GUID  gEfiClockProtocolGuid;

typedef struct _EFI_CLOCK_PROTOCOL EFI_CLOCK_PROTOCOL;

typedef enum {
  EfiClockFrequencyHzAtLeast  = 0,
  EfiClockFrequencyHzAtMost   = 1,
  EfiClockFrequencyHzClosest  = 2,
  EfiClockFrequencyHzExact    = 3,
  EfiClockFrequencyKhzAtLeast = 0x10,
  EfiClockFrequencyKhzAtMost  = 0x11,
  EfiClockFrequencyKhzClosest = 0x12,
  EfiClockFrequencyKhzExact   = 0x13,
  EfiClockFrequencyMhzAtLeast = 0x20,
  EfiClockFrequencyMhzAtMost  = 0x21,
  EfiClockFrequencyMhzClosest = 0x22,
  EfiClockFrequencyMhzExact   = 0x23
} EFI_CLOCK_FREQUENCY_TYPE;

typedef enum {
  EfiClockResetDeassert = 0,
  EfiClockResetAssert   = 1,
  EfiClockResetPulse    = 2
} EFI_CLOCK_RESET_TYPE;

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_GET_ID)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  CONST CHAR8        *ClockName,
  OUT UINTN              *ClockId
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_ENABLE)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               ClockId
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_DISABLE)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               ClockId
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_IS_ENABLED)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  UINTN               ClockId,
  OUT BOOLEAN            *IsEnabled
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_IS_ON)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  UINTN               ClockId,
  OUT BOOLEAN            *IsOn
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_SET_FREQ_HZ)(
  IN  EFI_CLOCK_PROTOCOL       *This,
  IN  UINTN                     ClockId,
  IN  UINT32                    Freq,
  IN  EFI_CLOCK_FREQUENCY_TYPE  Match,
  OUT UINT32                   *ResultFreq OPTIONAL
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_GET_FREQ_HZ)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  UINTN               ClockId,
  OUT UINT32             *FreqHz
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_CALC_FREQ_HZ)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  UINTN               ClockId,
  OUT UINT32             *FreqHz
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_SELECT_EXTERNAL_SOURCE)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               ClockId,
  IN UINT32              FreqHz,
  IN UINT32              Source,
  IN UINT32              Divider,
  IN UINT32              M,
  IN UINT32              N,
  IN UINT32              TwiceD
  );

typedef EFI_STATUS (EFIAPI *EFI_POWER_DOMAIN_GET_ID)(
  IN  EFI_CLOCK_PROTOCOL *This,
  IN  CONST CHAR8        *PowerDomainName,
  OUT UINTN              *PowerDomainId
  );

typedef EFI_STATUS (EFIAPI *EFI_POWER_DOMAIN_ENABLE)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               PowerDomainId
  );

typedef EFI_STATUS (EFIAPI *EFI_POWER_DOMAIN_DISABLE)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               PowerDomainId
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_LOW_POWER_MODE)(
  IN EFI_CLOCK_PROTOCOL *This
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_SET_DIVIDER)(
  IN EFI_CLOCK_PROTOCOL *This,
  IN UINTN               ClockId,
  IN UINT32              Divider
  );

typedef EFI_STATUS (EFIAPI *EFI_CLOCK_RESET)(
  IN EFI_CLOCK_PROTOCOL   *This,
  IN UINTN                 ClockId,
  IN EFI_CLOCK_RESET_TYPE  ResetType
  );

struct _EFI_CLOCK_PROTOCOL {
  UINT64                            Version;
  EFI_CLOCK_GET_ID                  GetClockID;
  EFI_CLOCK_ENABLE                  EnableClock;
  EFI_CLOCK_DISABLE                 DisableClock;
  EFI_CLOCK_IS_ENABLED              IsClockEnabled;
  EFI_CLOCK_IS_ON                   IsClockOn;
  EFI_CLOCK_SET_FREQ_HZ             SetClockFreqHz;
  EFI_CLOCK_GET_FREQ_HZ             GetClockFreqHz;
  EFI_CLOCK_CALC_FREQ_HZ            CalcClockFreqHz;
  EFI_CLOCK_SELECT_EXTERNAL_SOURCE  SelectExternalSource;
  EFI_POWER_DOMAIN_GET_ID           GetClockPowerDomainID;
  EFI_POWER_DOMAIN_ENABLE           EnableClockPowerDomain;
  EFI_POWER_DOMAIN_DISABLE          DisableClockPowerDomain;
  EFI_CLOCK_LOW_POWER_MODE          EnterLowPowerMode;
  EFI_CLOCK_LOW_POWER_MODE          ExitLowPowerMode;
  EFI_CLOCK_SET_DIVIDER             SetClockDivider;
  EFI_CLOCK_RESET                   ResetClock;
};

#endif
