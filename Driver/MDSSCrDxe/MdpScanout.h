/** @file Scanout setup/shutdown while display resources remain powered. */
#ifndef MDP_SCANOUT_H_
#define MDP_SCANOUT_H_
#include <Uefi.h>
#include <Protocol/EFIMdpDsiProtocol.h>

/** Calculate Waipio video prefetch in vertical front porch. Zero disables
    programmable fetch when no safe line remains after the hardware delay.
    Validate before programming the DPU or starting the DSI video engine. */
EFI_STATUS MdpGetVideoFetchStart (
  IN CONST EFI_MDP_DSI_TIMING *Timing, OUT UINT32 *FetchStart
  );

/** Require enabled timing, a new frame, and consumed CTL flush before GOP.
    This proves interface progress only, not physical panel output. */
EFI_STATUS MdpWaitScanoutProgress (IN UINT32 Controller);

/** Stop the owned DPU/DSI path. On failure the caller must retain DMA mappings
    and powered resources, since a timing-disable write alone is not an ack. */
EFI_STATUS MdpStopScanout (IN UINT32 Controller);

/** Route only DMA0 RECT0 to LM0's base stage, clearing extended routes. */
VOID MdpSetupDmaRouting (VOID);

/** Configure LM0 dimensions and explicitly select a single/left mixer. */
VOID MdpSetupSingleMixer (IN UINT32 Width, IN UINT32 Height);

/** Configure SM8450 DMA0 QoS for realtime linear RGB scanout. */
VOID MdpSetupDmaQos (VOID);

/** Clear the bounded BGRX framebuffer to black and publish it to scanout. */
EFI_STATUS MdpClearFrameBuffer (
  IN VOID *FrameBuffer, IN UINTN BufferSize,
  IN UINT32 Width, IN UINT32 Height, IN UINT32 Stride
  );

/** Configure DMA0's unscaled, single-rectangle RGB pixel extension before
    flushing the pipe. Width and Height must fit the 16-bit source fields. */
VOID MdpSetupDmaPixelExtension (IN UINT32 Width, IN UINT32 Height);
#endif
