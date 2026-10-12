/**
  Small MDP/DSI hand-off protocol used by the open HDK8450 display driver.

  The protocol deliberately models the Linux msm DSI video path.  It does not
  expose Qualcomm's private DisplayDxe ABI and is therefore usable by the
  framebuffer consumers in this tree.
**/
#ifndef EFI_MDP_DSI_PROTOCOL_H_
#define EFI_MDP_DSI_PROTOCOL_H_

#include <Uefi.h>

#define EFI_MDP_DSI_PROTOCOL_GUID \
  { 0x7c3e2f4a, 0x5c01, 0x4f16, { 0x9a, 0x57, 0x31, 0x8c, 0x72, 0x0d, 0x55, 0x90 } }

extern EFI_GUID gEfiMdpDsiProtocolGuid;

typedef struct _EFI_MDP_DSI_PROTOCOL EFI_MDP_DSI_PROTOCOL;

typedef struct {
  UINT32 Width;
  UINT32 Height;
  UINT32 HFrontPorch;
  UINT32 HBackPorch;
  UINT32 HSync;
  UINT32 VFrontPorch;
  UINT32 VBackPorch;
  UINT32 VSync;
  UINT32 DsiChannel;
  UINT32 DsiVirtualChannel;
  UINT32 DsiColorFormat;
  UINT32 DsiTrafficMode;
  UINT32 DsiLanes;
  UINT32 DsiRefreshRate;
  UINT32 DisplayResetGpio;
  /* Physical DSI controller from XML DSIControllerMapping (zero based).
     DSIChannelId identifies the display channel, not the host controller. */
  UINT32 DsiController;
  UINT32 DsiVideoFlags;
} EFI_MDP_DSI_TIMING;

#define MDP_DSI_VIDEO_BLLP_LP      BIT12
#define MDP_DSI_VIDEO_EOF_BLLP_LP  BIT15
#define MDP_DSI_VIDEO_HSA_LP       BIT16
#define MDP_DSI_VIDEO_HBP_LP       BIT20
#define MDP_DSI_VIDEO_HFP_LP       BIT24

typedef EFI_STATUS (EFIAPI *EFI_MDP_DSI_CONFIGURE_VIDEO)(
  IN EFI_MDP_DSI_PROTOCOL *This,
  IN CONST EFI_MDP_DSI_TIMING *Timing,
  IN EFI_PHYSICAL_ADDRESS FrameBuffer,
  IN UINT32 FrameBufferStride
  );

typedef EFI_STATUS (EFIAPI *EFI_MDP_DSI_GET_FRAMEBUFFER)(
  IN EFI_MDP_DSI_PROTOCOL *This,
  OUT EFI_PHYSICAL_ADDRESS *FrameBuffer,
  OUT UINT32 *Width,
  OUT UINT32 *Height,
  OUT UINT32 *Stride
  );

/*
 * Complete the bridge handoff after the MDSS resources and framebuffer have
 * been prepared.  MDSS publishes this protocol before the LT9611 bridge is
 * dispatched; LT9611 calls this method after installing its bridge protocol.
 * The callback performs the bridge configure, DSI host setup, scanout start,
 * and final GOP publication as one ordered operation.
 */
typedef EFI_STATUS (EFIAPI *EFI_MDP_DSI_BRIDGE_READY)(
  IN EFI_MDP_DSI_PROTOCOL *This
  );

struct _EFI_MDP_DSI_PROTOCOL {
  EFI_MDP_DSI_BRIDGE_READY   BridgeReady;
  EFI_MDP_DSI_CONFIGURE_VIDEO ConfigureVideo;
  EFI_MDP_DSI_GET_FRAMEBUFFER GetFramebuffer;
};

#endif
