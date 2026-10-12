#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../Driver/MDSSCrDxe/MdpScanout.h"

VOID *EFIAPI SetMem (VOID *P, UINTN N, UINT8 V) { return memset(P,V,N); }

static UINTN intf, dsi;
static unsigned stage, reads, scenario, resets;
static BOOLEAN pixel_extension;
static UINT32 pixel_registers[0x180 / 4];
static unsigned pixel_writes;
static BOOLEAN routing;
static UINT32 route_registers[4];
static unsigned route_writes;
static BOOLEAN mixer;
static UINT32 mixer_mode, mixer_size;
static unsigned mixer_writes;
static BOOLEAN qos;
static UINT32 qos_registers[0x80 / 4];
static unsigned qos_writes;
static VOID *clear_base;
static UINTN clear_size;
static unsigned clear_calls, flush_calls;

VOID *EFIAPI ZeroMem (VOID *buffer, UINTN size)
{
  assert(buffer == clear_base && size == clear_size);
  ++clear_calls;
  return memset(buffer, 0, size);
}

VOID *EFIAPI WriteBackDataCacheRange (VOID *buffer, UINTN size)
{
  UINTN index;
  assert(buffer == clear_base && size == clear_size && clear_calls == flush_calls + 1);
  for (index = 0; index < size; ++index) assert(((UINT8 *)buffer)[index] == 0);
  ++flush_calls;
  return buffer;
}
#define CTL_RESET 0x0AE16030

UINT32 EFIAPI MmioRead32 (UINTN address)
{
  if (mixer) {
    assert(address == 0x0AE45000 && mixer_writes == 0);
    return mixer_mode;
  }
  if (address == intf + 0x26C) {
    assert(stage == 1);
    if (scenario == 1 || reads++ < 3) return 1;
    stage = 2;
    return 0;
  }
  if (address == CTL_RESET) {
    assert(stage == 3);
    if (scenario == 2) return 1;
    stage = 4;
    return 0;
  }
  assert(address == dsi);
  assert(stage == 4 || stage == 7);
  return stage == 4 || scenario == 3 ? 0x1f3 : 0x1f0;
}
UINT32 EFIAPI MmioWrite32 (UINTN address, UINT32 value)
{
  if (qos) {
    UINTN offset = address - 0x0AE25000;
    assert(offset == 0x60 || offset == 0x64 || offset == 0x74 ||
           offset == 0x78 || offset == 0x6c);
    if (offset == 0x6c) assert(qos_writes == 4);
    qos_registers[offset / 4] = value;
    ++qos_writes;
    return value;
  }
  if (mixer) {
    if (mixer_writes == 0) {
      assert(address == 0x0AE45004);
      mixer_size = value;
    } else {
    assert(mixer_writes == 1 && address == 0x0AE45000);
      mixer_mode = value;
    }
    ++mixer_writes;
    return value;
  }
  if (routing) {
    UINTN offset = address - 0x0AE16000;
    unsigned index;
    if (offset == 0x40) index = 1;
    else if (offset == 0x70) index = 2;
    else if (offset == 0xa0) index = 3;
    else { assert(offset == 0 && route_writes == 3); index = 0; }
    route_registers[index] = value;
    ++route_writes;
    return value;
  }
  if (pixel_extension) {
    UINTN offset = address - 0x0AE25000;
    assert(offset < sizeof(pixel_registers) && offset % 4 == 0);
    /* All component dimensions and solo mode must be ready before the
       final OP_MODE write activates software extension. */
    if (offset == 0x38) assert(pixel_writes == 10 && value == BIT31);
    pixel_registers[offset / 4] = value;
    ++pixel_writes;
    return value;
  }
  if (address == intf) { assert(stage == 0 && value == 0); stage = 1; }
  else if (address == CTL_RESET) { assert(stage == 2 && value == 1); stage = 3; }
  else if (address == dsi) { assert(stage == 4 && value == 0x1f0); stage = 5; }
  else {
    assert(address == dsi + 0x114);
    if (value) { assert(stage == 5); stage = 6; ++resets; }
    else { assert(stage == 6); stage = 7; }
  }
  return value;
}
VOID EFIAPI MemoryFence (VOID) {}
UINTN EFIAPI MicroSecondDelay (UINTN delay)
{
  assert(delay == 50 || (delay == 20000 && stage == 6));
  return delay;
}

static void test_video_fetch(void)
{
  EFI_MDP_DSI_TIMING timing = {
    .Width = 1920, .Height = 1080,
    .HFrontPorch = 88, .HBackPorch = 148, .HSync = 44,
    .VFrontPorch = 4, .VBackPorch = 36, .VSync = 5,
    .DsiRefreshRate = 60U << 16,
  };
  UINT32 fetch, front, back, hz;
  UINT64 active_end, frame_end;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 2472801U); /* HDK last VFP line, one pixel into the line. */

  /* Packaged Truly 1080x1920 video timing has just 2+2 start-of-frame
     lines. All 12 VFP lines are selected before applying the one-line delay. */
  timing = (EFI_MDP_DSI_TIMING) {
    .Width = 1080, .Height = 1920,
    .HFrontPorch = 120, .HBackPorch = 60, .HSync = 12,
    .VFrontPorch = 12, .VBackPorch = 2, .VSync = 2,
    .DsiRefreshRate = 60U << 16,
  };
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 2448601U);

  timing.Width = 1920; timing.Height = 1080;
  timing.HFrontPorch = 88; timing.HBackPorch = 148; timing.HSync = 44;
  timing.VFrontPorch = 100;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 2527801U);
  timing.DsiRefreshRate = 90U << 16;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 2483801U); /* Higher refresh requires earlier fetch. */
  timing.DsiRefreshRate = 120U << 16;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 2439801U);

  /* Exercise both sides of the prefill-budget boundary, including a
     one-line deficit. Every enabled trigger must lie in VFP, after the
     mandatory delay, and strictly before the next frame. */
  timing.VSync = 1;
  for (hz = 60; hz <= 120; hz += 60) {
    timing.DsiRefreshRate = hz << 16;
    for (back = 0; back < 90; ++back) {
      timing.VBackPorch = back;
      for (front = 0; front < 129; ++front) {
        timing.VFrontPorch = front;
        assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
        if (front < 2) { assert(fetch == 0); continue; }
        active_end = ((UINT64)timing.Height + back + 1) * 2200;
        frame_end = active_end + front * 2200;
        assert(fetch > active_end + 2200 && fetch < frame_end);
        assert(fetch % 2200 == 1);
      }
    }
  }
  timing.Width = timing.Height = timing.HSync = timing.VSync = 1;
  timing.HFrontPorch = 65533; timing.HBackPorch = 0;
  timing.VFrontPorch = 2; timing.VBackPorch = 65531;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_SUCCESS);
  assert(fetch == 4294770691U); /* Largest supported 16-bit totals. */
  timing.VFrontPorch = MAX_UINT32;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_BAD_BUFFER_SIZE && fetch == 0);
  timing.VFrontPorch = 2; timing.HFrontPorch = MAX_UINT32;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_BAD_BUFFER_SIZE && fetch == 0);
  timing.HFrontPorch = 65533; timing.DsiRefreshRate = 0;
  assert(MdpGetVideoFetchStart(&timing, &fetch) == EFI_INVALID_PARAMETER);
  assert(MdpGetVideoFetchStart(NULL, &fetch) == EFI_INVALID_PARAMETER);
  assert(MdpGetVideoFetchStart(&timing, NULL) == EFI_INVALID_PARAMETER);
  puts("MDP video prefetch: HDK/Truly XML timing, refresh scaling, porch boundaries and overflow PASS");
}

int main(void)
{
  UINT32 controller;
  UINT32 widths[] = { 1920, 1, 65535 };
  UINT32 heights[] = { 1080, 1, 65535 };
  unsigned mode, index, component;
  UINT8 framebuffer[66];
  test_video_fetch();
  memset(framebuffer, 0xa5, sizeof(framebuffer));
  clear_base = framebuffer + 1;
  clear_size = 48; /* two RGBX rows of five pixels with four bytes padding */
  assert(MdpClearFrameBuffer(clear_base, 64, 5, 2, 24) == EFI_SUCCESS);
  assert(clear_calls == 1 && flush_calls == 1 && framebuffer[0] == 0xa5);
  for (index = 49; index < sizeof(framebuffer); ++index) assert(framebuffer[index] == 0xa5);
  /* Re-selecting the current mode must clear newly drawn pixels too. */
  memset(clear_base, 0x5a, clear_size);
  assert(MdpClearFrameBuffer(clear_base, 64, 5, 2, 24) == EFI_SUCCESS);
  assert(clear_calls == 2 && flush_calls == 2);
  assert(MdpClearFrameBuffer(NULL, 64, 5, 2, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer(clear_base, 47, 5, 2, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer(clear_base, 64, 7, 2, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer(clear_base, 64, 0, 2, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer(clear_base, 64, 5, 0, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer(clear_base, 64, MAX_UINT32, 2, 24) == EFI_INVALID_PARAMETER);
  assert(MdpClearFrameBuffer((VOID *)(MAX_UINTN - 16), 64, 5, 2, 24) == EFI_INVALID_PARAMETER);
  assert(clear_calls == 2 && flush_calls == 2);
  puts("MDP framebuffer clear: padded rows, repeated mode, bounds, overflow, flush ordering PASS");
  qos = TRUE;
  for (index = 0; index < sizeof(qos_registers) / sizeof(qos_registers[0]); ++index)
    qos_registers[index] = 0xffffffff;
  MdpSetupDmaQos();
  assert(qos_writes == 5);
  assert(qos_registers[0x60 / 4] == 0x3ffff);
  assert(qos_registers[0x64 / 4] == 0xfe00);
  assert((((UINT64)qos_registers[0x78 / 4] << 32) |
          qos_registers[0x74 / 4]) == 0x0011222222335777ULL);
  assert(qos_registers[0x6c / 4] == 1);
  assert(qos_registers[0x68 / 4] == 0xffffffff); /* legacy four-level LUT untouched */
  qos = FALSE;
  puts("MDP DMA QoS: SM8450 linear LUTs, 64-bit CREQ, enable ordering PASS");
  mixer = TRUE;
  for (mode = 0; mode < 3; ++mode) {
    mixer_mode = mode == 0 ? 0 : 0xffffffff;
    mixer_writes = 0;
    MdpSetupSingleMixer(widths[mode], heights[mode]);
    assert(mixer_writes == 2);
    assert(mixer_size == (widths[mode] | (heights[mode] << 16)));
    assert(mixer_mode == (mode == 0 ? 0 : 0x7fffffff));
  }
  mixer = FALSE;
  puts("MDP single mixer: dimensions, inherited right-half cleared, other controls preserved PASS");
  routing = TRUE;
  for (index = 0; index < 4; ++index) route_registers[index] = 0xffffffff;
  MdpSetupDmaRouting();
  assert(route_writes == 4);
  /* Decode the DMA0 stage across both words: an inherited EXT bit 16
     would route it to stage 9 rather than the base-stage encoding 1. */
  assert((((route_registers[0] >> 18) & 7) |
          (((route_registers[1] >> 16) & 1) << 3)) == 1);
  assert(route_registers[0] == (BIT24 | (1U << 18)));
  assert(route_registers[1] == 0 && route_registers[2] == 0 && route_registers[3] == 0);
  routing = FALSE;
  puts("MDP DMA routing: poisoned extended stages cleared, sole DMA0 base route PASS");
  pixel_extension = TRUE;
  for (mode = 0; mode < 3; ++mode) {
    /* Poison inherited state: zero repeat/fetch and multirect writes are
       required even when a previous owner left a different pipe setup. */
    for (index = 0; index < sizeof(pixel_registers) / sizeof(pixel_registers[0]); ++index)
      pixel_registers[index] = 0xffffffff;
    pixel_writes = 0;
    MdpSetupDmaPixelExtension(widths[mode], heights[mode]);
    assert(pixel_writes == 11);
    for (component = 0; component < 3; ++component) {
      index = (0x100 + component * 0x10) / 4;
      assert(pixel_registers[index] == 0 && pixel_registers[index + 1] == 0);
      assert(pixel_registers[index + 2] == (widths[mode] | (heights[mode] << 16)));
    }
    assert(pixel_registers[0x170 / 4] == 0);
    assert(pixel_registers[0x38 / 4] == BIT31);
  }
  pixel_extension = FALSE;
  puts("MDP DMA pixel extension: RGB components, poisoned state, solo mode, 16-bit dimensions PASS");
  for (controller = 0; controller < 2; ++controller) {
    intf = controller ? 0x0AE37000 : 0x0AE36000;
    dsi = (controller ? 0x0AE96000 : 0x0AE94000) + 4;
    for (scenario = 0; scenario < 4; ++scenario) {
      EFI_STATUS status;
      stage = reads = resets = 0;
      status = MdpStopScanout(controller);
      if (scenario == 0) { assert(status == EFI_SUCCESS && stage == 7 && resets == 1); }
      if (scenario == 1) { assert(status == EFI_TIMEOUT && stage == 1 && resets == 0); }
      if (scenario == 2) { assert(status == EFI_TIMEOUT && stage == 3 && resets == 0); }
      if (scenario == 3) { assert(status == EFI_DEVICE_ERROR && stage == 7); }
    }
  }
  stage = 0;
  assert(MdpStopScanout(2) == EFI_INVALID_PARAMETER && stage == 0);
  puts("MDP scanout shutdown: both hosts, delayed latch, INTF/CTL timeout, DSI failure PASS");
  return 0;
}
