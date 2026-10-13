/** @file
  SM8450 DPU8.1 DMA0/CTL0/LM0 and DSI6G video control.
  Source: Linux dpu_8_1_sm8450.h, dpu_hw_{sspp,ctl,intf,lm}.c,
  dsi_host.c, Android sde_encoder_phys_vid.c, native MDSSCrDxe.
  SPDX-License-Identifier: GPL-2.0-only
**/
#include <Library/MdssCrLib.h>
#include <oskal/cr_string.h>
#define B(n) (1U << (n))
#define DMA 0x24000U
#define CTL 0x15000U
#define LM 0x44000U
#define DSI 4U
static CR_STATUS Completed(CR_STATUS S)
{ return S == CR_SUCCESS ? S : CR_ERROR(S) ? S : CR_NOT_READY; }

CR_STATUS CrMdssValidateMode(CONST CR_MDSS_MODE *M, UINTN Bytes)
{
  UINT64 H, V, Size;
  if (!M || M->Soc != CR_MDSS_SOC_SM8450 || M->Controller > 1 ||
      !M->Width || !M->Height || !M->HSync || !M->VSync ||
      !M->RefreshQ16 || M->Lanes < 1 || M->Lanes > 4 ||
      M->VirtualChannel > 3 || M->TrafficMode > 2 ||
      (M->VideoFlags & ~CR_MDSS_VIDEO_LP_MASK) ||
      M->InvertHsync > 1 || M->InvertVsync > 1 ||
      M->HsaHseAfterVsVe > 1 || M->DisableEot > 1)
    return CR_INVALID_PARAMETER;
  H = (UINT64)M->Width + M->HSync + M->HBackPorch + M->HFrontPorch;
  V = (UINT64)M->Height + M->VSync + M->VBackPorch + M->VFrontPorch;
  Size = (UINT64)M->Stride * M->Height;
  if (H > 0xFFFF || V > 0xFFFF || H * V > CR_MAX_UINT32 ||
      (UINT64)M->Width * 4 > M->Stride || (M->Stride & 3) ||
      M->Stride > 0xFFFF || Size > Bytes)
    return CR_BAD_BUFFER_SIZE;
  return CR_SUCCESS;
}

CR_STATUS CrMdssGetFetchStart(CONST CR_MDSS_MODE *M, UINT32 *Start)
{
  UINT64 H, V, Prefill, Sof, Lines;
  if (!Start || !M) return CR_INVALID_PARAMETER;
  *Start = 0;
  if (!M->Width || !M->Height || !M->HSync || !M->VSync || !M->RefreshQ16)
    return CR_INVALID_PARAMETER;
  H = (UINT64)M->Width + M->HSync + M->HBackPorch + M->HFrontPorch;
  V = (UINT64)M->Height + M->VSync + M->VBackPorch + M->VFrontPorch;
  if (H > 0xFFFF || V > 0xFFFF || H * V > CR_MAX_UINT32)
    return CR_BAD_BUFFER_SIZE;
  Prefill = 40;
  if (M->RefreshQ16 > (60U << 16))
    Prefill = (40ULL * M->RefreshQ16) / (60U << 16);
  Sof = (UINT64)M->VSync + M->VBackPorch;
  Lines = Prefill > Sof ? Prefill - Sof : 2;
  if (Lines < 2) Lines = 2;
  if (Lines > M->VFrontPorch) Lines = M->VFrontPorch;
  if (Lines >= 2) *Start = (UINT32)((V - Lines + 1) * H + 1);
  return CR_SUCCESS;
}

CR_STATUS CrMdssInit(CR_MDSS *D, CONST CR_MDSS_IO *Io,
                    CONST CR_MDSS_MODE *M, VOID *Cpu, UINT64 Pa, UINTN Bytes)
{
  CR_STATUS S;
  BOOLEAN Pipeline;
  if(!Io)return CR_INVALID_PARAMETER;
  Pipeline=Io->PipelineRevision||Io->StartPipeline||Io->StopPipeline;
  if(Pipeline&&(Io->PipelineRevision!=1||!Io->StartPipeline||!Io->StopPipeline))
    return CR_INVALID_PARAMETER;
  if (!D || !Io || !Cpu || !Pa || !Bytes ||
      Bytes > (UINTN)-1 - (UINTN)Cpu || Pa > CR_MAX_UINT64 - Bytes ||
      (!Pipeline&&(!Io->Read32 || !Io->Write32 || !Io->Fence || !Io->DelayUs ||
      !Io->Acquire || !Io->Release || !Io->PreparePanel || !Io->UnpreparePanel)) ||
      !Io->PublishCpuWrites ||
      ((Io->Read32Checked || Io->Write32Checked || Io->FenceChecked || Io->DelayUsChecked) &&
       (!Io->Read32Checked || !Io->Write32Checked || !Io->FenceChecked || !Io->DelayUsChecked)))
    return CR_INVALID_PARAMETER;
  S = CrMdssValidateMode(M, Bytes);
  if (CR_ERROR(S)) return S;
  cr_memset(D, 0, sizeof(*D));
  D->Io = *Io; D->Mode = *M; D->FrameBuffer = Cpu;
  D->CpuPhysical = Pa; D->BufferBytes = Bytes; D->Initialized = TRUE;
  return CR_SUCCESS;
}

CR_STATUS CrMdssTransferToPost(CR_MDSS *D)
{
  CR_STATUS S;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (!D->Active || D->CleanupPending || !D->ResourcesOwned ||
      !D->HostTouched || !D->PanelAttempted || !D->ScanoutTouched)
    return CR_NOT_READY;
  if (!D->Io.TransferToPost) return CR_UNSUPPORTED;
  S = Completed(D->Io.TransferToPost(D->Io.Context));
  if (CR_ERROR(S)) return S;
  /* The successful platform operation owns every lifetime from here.
     Never run local Unprepare/Release or write disable registers. */
  cr_memset(D, 0, sizeof(*D));
  return CR_SUCCESS;
}

static UINT32 R(CR_MDSS *D, CR_MDSS_WINDOW W, UINT32 O)
{
  UINT32 V = CR_MAX_UINT32;
  if (D->IoStatus != CR_SUCCESS) return V;
  if (!D->Io.Read32Checked) return D->Io.Read32(D->Io.Context, W, O);
  D->IoStatus = Completed(D->Io.Read32Checked(D->Io.Context, W, O, &V));
  return D->IoStatus == CR_SUCCESS ? V : CR_MAX_UINT32;
}
static VOID W(CR_MDSS *D, CR_MDSS_WINDOW Win, UINT32 O, UINT32 V)
{
  if (D->IoStatus != CR_SUCCESS) return;
  if (D->Io.Write32Checked)
    D->IoStatus = Completed(D->Io.Write32Checked(D->Io.Context, Win, O, V));
  else D->Io.Write32(D->Io.Context, Win, O, V);
}
static VOID Fence(CR_MDSS *D)
{
  if (D->IoStatus != CR_SUCCESS) return;
  if (D->Io.FenceChecked) D->IoStatus = Completed(D->Io.FenceChecked(D->Io.Context));
  else D->Io.Fence(D->Io.Context);
}
static VOID Delay(CR_MDSS *D, UINT32 Us)
{
  if (D->IoStatus != CR_SUCCESS) return;
  if (D->Io.DelayUsChecked) D->IoStatus = Completed(D->Io.DelayUsChecked(D->Io.Context, Us));
  else D->Io.DelayUs(D->Io.Context, Us);
}
static CR_STATUS WaitClear(CR_MDSS *D, CR_MDSS_WINDOW Win, UINT32 O,
                          UINT32 Mask, UINT32 Us)
{
  UINT32 Elapsed;
  for (Elapsed = 0; Elapsed < Us; Elapsed += 50) {
    UINT32 V = R(D, Win, O);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
    if (!(V & Mask)) return CR_SUCCESS;
    Delay(D, 50);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
  }
  UINT32 V = R(D, Win, O);
  return D->IoStatus != CR_SUCCESS ? D->IoStatus : (V & Mask) ? CR_TIMEOUT : CR_SUCCESS;
}

static VOID ProgramDsi(CR_MDSS *D)
{
  CONST CR_MDSS_MODE *M = &D->Mode;
  UINT32 H = M->Width + M->HSync + M->HBackPorch + M->HFrontPorch;
  UINT32 V = M->Height + M->VSync + M->VBackPorch + M->VFrontPorch;
  UINT32 Ha = M->HSync + M->HBackPorch, Va = M->VSync + M->VBackPorch;
  UINT32 Ctrl = B(8) | B(0) | (((1U << M->Lanes) - 1) << 4);
  D->HostTouched = TRUE;
  W(D, CrMdssDsi, DSI, R(D, CrMdssDsi, DSI) & ~B(0)); Fence(D);
  W(D, CrMdssDsi, DSI + 0x118, 0x23F); Fence(D);
  W(D, CrMdssDsi, DSI + 0x114, 1);
  Delay(D, 20000);
  W(D, CrMdssDsi, DSI + 0x114, 0); Fence(D);
  W(D, CrMdssDsi, DSI + 0x020, Ha | ((Ha + M->Width) << 16));
  W(D, CrMdssDsi, DSI + 0x024, Va | ((Va + M->Height) << 16));
  W(D, CrMdssDsi, DSI + 0x028, (H - 1) | ((V - 1) << 16));
  W(D, CrMdssDsi, DSI + 0x02C, M->HSync << 16);
  W(D, CrMdssDsi, DSI + 0x030, 0);
  W(D, CrMdssDsi, DSI + 0x034, M->VSync << 16);
  W(D, CrMdssDsi, DSI + 0x00C, M->VirtualChannel | (3U << 4) |
      (M->TrafficMode << 8) | (M->HsaHseAfterVsVe ? B(28) : 0) | M->VideoFlags);
  W(D, CrMdssDsi, DSI + 0x01C, 0);
  W(D, CrMdssDsi, DSI + 0x0C0, D->HostClockTiming);
  W(D, CrMdssDsi, DSI + 0x17C, 0);
  W(D, CrMdssDsi, DSI + 0x0AC, 0);
  W(D, CrMdssDsi, DSI + 0x0C8, M->DisableEot ? 0 : B(0));
  W(D, CrMdssDsi, DSI + 0x0A8,
    (R(D, CrMdssDsi, DSI + 0x0A8) & ~B(24)) | B(28));
  W(D, CrMdssDsi, DSI, Ctrl); Fence(D);
}

static VOID ProgramDpu(CR_MDSS *D, UINT32 Fetch)
{
  CONST CR_MDSS_MODE *M = &D->Mode;
  UINT32 I = M->Controller ? 0x36000U : 0x35000U;
  UINT32 H = M->Width + M->HSync + M->HBackPorch + M->HFrontPorch;
  UINT32 V = M->Height + M->VSync + M->VBackPorch + M->VFrontPorch;
  UINT32 Start = M->HSync + M->HBackPorch, Bit = M->Controller + 1;
  UINT32 J;
  W(D, CrMdssDpu, DMA + 0x014, (UINT32)D->DeviceAddress);
  W(D, CrMdssDpu, DMA + 0x018, 0);
  W(D, CrMdssDpu, DMA + 0x024, M->Stride);
  W(D, CrMdssDpu, DMA + 0x000, M->Width | (M->Height << 16));
  W(D, CrMdssDpu, DMA + 0x00C, M->Width | (M->Height << 16));
  W(D, CrMdssDpu, DMA + 0x008, 0); W(D, CrMdssDpu, DMA + 0x010, 0);
  W(D, CrMdssDpu, DMA + 0x034, 0x03020001);
  W(D, CrMdssDpu, DMA + 0x030, 0x000236FF);
  for (J = 0; J < 3; ++J) {
    W(D, CrMdssDpu, DMA + 0x100 + J * 0x10, 0);
    W(D, CrMdssDpu, DMA + 0x104 + J * 0x10, 0);
    W(D, CrMdssDpu, DMA + 0x108 + J * 0x10, M->Width | (M->Height << 16));
  }
  W(D, CrMdssDpu, DMA + 0x170, 0); W(D, CrMdssDpu, DMA + 0x038, B(31));
  W(D, CrMdssDpu, 0x2AC, R(D, CrMdssDpu, 0x2AC) | B(8));
  W(D, CrMdssDpu, CTL + 0x040, 0); W(D, CrMdssDpu, CTL + 0x070, 0);
  W(D, CrMdssDpu, CTL + 0x0A0, 0); W(D, CrMdssDpu, CTL, B(24) | B(18));
  W(D, CrMdssDpu, DMA + 0x060, 0x0003FFFF);
  W(D, CrMdssDpu, DMA + 0x064, 0x0000FE00);
  W(D, CrMdssDpu, DMA + 0x074, 0x22335777);
  W(D, CrMdssDpu, DMA + 0x078, 0x00112222);
  W(D, CrMdssDpu, DMA + 0x06C, 1);
  W(D, CrMdssDpu, CTL + 0x0F4, B(Bit));
  W(D, CrMdssDpu, CTL + 0x0FC, 1);
  W(D, CrMdssDpu, LM + 4, M->Width | (M->Height << 16));
  W(D, CrMdssDpu, LM, R(D, CrMdssDpu, LM) & ~B(31));
  W(D, CrMdssDpu, CTL + 0x014, 0xF0000000);
  W(D, CrMdssDpu, I + 0x008, (H << 16) | M->HSync);
  W(D, CrMdssDpu, I + 0x00C, V * H);
  W(D, CrMdssDpu, I + 0x014, M->VSync * H);
  W(D, CrMdssDpu, I + 0x03C, ((H - M->HFrontPorch - 1) << 16) | Start);
  W(D, CrMdssDpu, I + 0x01C, (M->VSync + M->VBackPorch) * H);
  W(D, CrMdssDpu, I + 0x024, (V - M->VFrontPorch) * H - 1);
  W(D, CrMdssDpu, I + 0x040, 0); W(D, CrMdssDpu, I + 0x044, M->BorderColor);
  W(D, CrMdssDpu, I + 0x048, 0xFF); W(D, CrMdssDpu, I + 0x04C, 0);
  W(D, CrMdssDpu, I + 0x050, M->InvertHsync | (M->InvertVsync << 1));
  W(D, CrMdssDpu, I + 0x090, 0x213F); W(D, CrMdssDpu, I + 0x0A8, 3);
  W(D, CrMdssDpu, I + 0x25C, 0); W(D, CrMdssDpu, I + 0x060, B(4));
  W(D, CrMdssDpu, I + 0x064, ((Start + M->Width - 1) << 16) | Start);
  W(D, CrMdssDpu, I + 0x068, 0);
  if (Fetch) W(D, CrMdssDpu, I + 0x170, Fetch);
  W(D, CrMdssDpu, I + 0x004, Fetch ? B(31) : 0);
  W(D, CrMdssDpu, CTL + 0x110, B(Bit));
  W(D, CrMdssDpu, CTL + 0x018, B(11) | B(6) | B(31) | B(17));
  W(D, CrMdssDpu, CTL + 0x01C, 1);
  D->ScanoutTouched = TRUE;
  W(D, CrMdssDpu, I, 1); Fence(D);
}

CR_STATUS CrMdssStart(CR_MDSS *D)
{
  CR_STATUS S;
  UINT32 Version, Fetch, Elapsed, Initial = 0;
  BOOLEAN HaveFrame = FALSE, Owned = FALSE;
  UINT32 I;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  D->IoStatus = CR_SUCCESS;
  if(D->Io.PipelineRevision) {
    if(D->CleanupPending)return CR_NOT_READY;
    Owned=D->ResourcesOwned;
    S=Completed(D->Io.StartPipeline(D->Io.Context,&D->Mode,D->CpuPhysical,
        D->BufferBytes,&D->DeviceAddress,&Owned));
    D->ResourcesOwned=Owned;
    if(CR_ERROR(S)) {D->Active=FALSE;D->CleanupPending=Owned;return S;}
    if(!Owned||!D->DeviceAddress||D->DeviceAddress>CR_MAX_UINT32||
       (UINT64)D->Mode.Stride*D->Mode.Height-1>CR_MAX_UINT32-D->DeviceAddress) {
      D->Active=FALSE;D->CleanupPending=Owned;return CR_BAD_BUFFER_SIZE;
    }
    D->HostTouched=D->PanelAttempted=D->ScanoutTouched=D->Active=TRUE;
    return CR_SUCCESS;
  }
  if (D->CleanupPending || D->ResourcesOwned || D->HostTouched) {
    if (!D->Active || D->CleanupPending) return CR_NOT_READY;
    if (D->Io.FenceChecked) Fence(D);
    if (D->IoStatus != CR_SUCCESS) { D->Active = FALSE; D->CleanupPending = TRUE; }
    return D->IoStatus;
  }
  S = CrMdssGetFetchStart(&D->Mode, &Fetch);
  if (CR_ERROR(S)) return S;
  S = Completed(D->Io.Acquire(D->Io.Context, &D->Mode, D->CpuPhysical,
                    D->BufferBytes, &D->DeviceAddress, &D->HostClockTiming, &Owned));
  D->ResourcesOwned = Owned;
  if (CR_ERROR(S)) { D->CleanupPending = Owned; return S; }
  if (!Owned || !D->DeviceAddress || D->DeviceAddress > CR_MAX_UINT32 ||
      (UINT64)D->Mode.Stride * D->Mode.Height - 1 >
          CR_MAX_UINT32 - D->DeviceAddress) {
    D->CleanupPending = Owned; return CR_BAD_BUFFER_SIZE;
  }
  Version = R(D, CrMdssDsi, 0x1F0);
  if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
  if (Version != 0 && Version != CR_MAX_UINT32) S = CR_UNSUPPORTED;
  else {
    Version = R(D, CrMdssDsi, DSI + 0x1F0);
    if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
    S = (!Version || Version == CR_MAX_UINT32) ? CR_NOT_READY : CR_SUCCESS;
  }
  if (CR_ERROR(S)) { D->CleanupPending = TRUE; return S; }
  ProgramDsi(D);
  if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
  D->PanelAttempted = TRUE;
  S = Completed(D->Io.PreparePanel(D->Io.Context, &D->Mode));
  if (CR_ERROR(S)) { D->CleanupPending = TRUE; return S; }
  W(D, CrMdssDsi, DSI, R(D, CrMdssDsi, DSI) | B(1)); Fence(D);
  if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
  D->Io.PublishCpuWrites(D->Io.Context, D->FrameBuffer,
                         (UINTN)D->Mode.Stride * D->Mode.Height);
  ProgramDpu(D, Fetch);
  if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
  I = D->Mode.Controller ? 0x36000U : 0x35000U;
  for (Elapsed = 0; Elapsed <= 250000; Elapsed += 50) {
    UINT32 Status = R(D, CrMdssDpu, I + 0x26C);
    if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
    if (Status & 1) {
      UINT32 Frame = R(D, CrMdssDpu, I + 0x0AC);
      if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
      if (!HaveFrame) { Initial = Frame; HaveFrame = TRUE; }
      else if (Frame != Initial && R(D, CrMdssDpu, CTL + 0x018) == 0) {
        if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
        D->Active = TRUE; return CR_SUCCESS;
      }
    } else HaveFrame = FALSE;
    if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
    if (Elapsed != 250000) Delay(D, 50);
    if (D->IoStatus != CR_SUCCESS) { D->CleanupPending = TRUE; return D->IoStatus; }
  }
  D->CleanupPending = TRUE; return CR_TIMEOUT;
}

CR_STATUS CrMdssStop(CR_MDSS *D)
{
  CR_STATUS S;
  UINT32 I;
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  D->IoStatus = CR_SUCCESS;
  D->Active = FALSE;
  if(D->Io.PipelineRevision) {
    if(!D->ResourcesOwned&&!D->CleanupPending)return CR_SUCCESS;
    BOOLEAN Owned=D->ResourcesOwned;
    D->CleanupPending=TRUE;
    S=Completed(D->Io.StopPipeline(D->Io.Context,&Owned));
    /* An error cannot retire a possibly committed pipeline lease. Only exact
       successful stop plus explicit not-owned clears the outer journal. */
    if(CR_ERROR(S))return S;
    if(Owned)return CR_NOT_READY;
    D->ResourcesOwned=D->HostTouched=D->PanelAttempted=D->ScanoutTouched=FALSE;
    D->CleanupPending=FALSE;D->DeviceAddress=0;return CR_SUCCESS;
  }
  I = D->Mode.Controller ? 0x36000U : 0x35000U;
  if (D->ScanoutTouched) {
    D->CleanupPending = TRUE;
    W(D, CrMdssDpu, I, 0); Fence(D);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
    S = WaitClear(D, CrMdssDpu, I + 0x26C, 1, 100000);
    if (CR_ERROR(S)) return S;
    W(D, CrMdssDpu, CTL + 0x030, 1); Fence(D);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
    S = WaitClear(D, CrMdssDpu, CTL + 0x030, 1, 2000);
    if (CR_ERROR(S)) return S;
    D->ScanoutTouched = FALSE;
  }
  if (D->HostTouched) {
    D->CleanupPending = TRUE;
    W(D, CrMdssDsi, DSI, R(D, CrMdssDsi, DSI) & ~B(1)); Fence(D);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
    /* Panel termination commands require the command host and PHY clocks. */
    if (D->PanelAttempted) {
      S = Completed(D->Io.UnpreparePanel(D->Io.Context, &D->Mode));
      if (CR_ERROR(S)) return S;
      D->PanelAttempted = FALSE;
    }
    W(D, CrMdssDsi, DSI, R(D, CrMdssDsi, DSI) & ~7U); Fence(D);
    W(D, CrMdssDsi, DSI + 0x114, 1);
    Delay(D, 20000);
    W(D, CrMdssDsi, DSI + 0x114, 0); Fence(D);
    UINT32 Ctrl = R(D, CrMdssDsi, DSI);
    if (D->IoStatus != CR_SUCCESS) return D->IoStatus;
    if (Ctrl & 7) return CR_DEVICE_ERROR;
    D->HostTouched = FALSE;
  }
  if (D->ResourcesOwned) {
    D->CleanupPending = TRUE;
    S = Completed(D->Io.Release(D->Io.Context));
    if (CR_ERROR(S)) return S;
    D->ResourcesOwned = FALSE; D->DeviceAddress = 0;
  }
  D->CleanupPending = FALSE;
  return CR_SUCCESS;
}

CR_STATUS CrMdssUninit(CR_MDSS *D)
{
  if (!D || !D->Initialized) return CR_INVALID_PARAMETER;
  if (D->ResourcesOwned || D->HostTouched || D->PanelAttempted ||
      D->ScanoutTouched || D->CleanupPending || D->Active) return CR_BUSY;
  cr_memset(D, 0, sizeof(*D)); return CR_SUCCESS;
}
