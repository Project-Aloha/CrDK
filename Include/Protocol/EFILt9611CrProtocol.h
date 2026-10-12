/** @file
  HDK8450 LT9611UXC video-mode handoff.

  MDSS passes the parsed panel timing to the bridge before enabling DSI
  scanout. The bridge's register sequence follows Linux lontium-lt9611uxc.c.
**/
#ifndef EFI_LT9611_CR_PROTOCOL_H_
#define EFI_LT9611_CR_PROTOCOL_H_

#include <Protocol/EFIMdpDsiProtocol.h>

#define EFI_LT9611_CR_PROTOCOL_GUID \
  { 0x4aa85bcd, 0xb1a0, 0x4df0, { 0x9f, 0x04, 0xc8, 0x91, 0xf4, 0x8f, 0x8b, 0xc1 } }

extern EFI_GUID gEfiLt9611CrProtocolGuid;

typedef struct _EFI_LT9611_CR_PROTOCOL EFI_LT9611_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_LT9611_CR_CONFIGURE_VIDEO)(
  IN EFI_LT9611_CR_PROTOCOL *This,
  IN CONST EFI_MDP_DSI_TIMING *Timing
  );

struct _EFI_LT9611_CR_PROTOCOL {
  EFI_LT9611_CR_CONFIGURE_VIDEO ConfigureVideo;
};

#endif
