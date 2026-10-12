/* Retained native bootstrap access; no direct clock or SMMU programming.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
#include <oskal/cr_mutex.h>

static CR_STATUS Exact(CR_STATUS S)
{ return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY; }

static CR_STATUS Valid(CR_MDSS_BOOT8250 *P)
{
  if(CrMutexCheckLevel()!=CR_SUCCESS || !P || !P->Initialized)
    return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}

CR_STATUS BootReady(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W)
{
  CR_STATUS S=Valid(P);if(S!=CR_SUCCESS)return S;
  if((UINT32)W>=CrMdssBootWindows)return CR_INVALID_PARAMETER;
  if(!P->AccessOwned || !P->Generation)return CR_NOT_READY;
  return Exact(P->Io.Ready(P->Io.Context,P->Generation,W));
}
static CR_STATUS Bounds(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O)
{
  CR_STATUS S=BootReady(P,W);if(S!=CR_SUCCESS)return S;
  return (O&3)||P->Io.Bytes[W]<4||O>P->Io.Bytes[W]-4?CR_BAD_BUFFER_SIZE:CR_SUCCESS;
}
CR_STATUS BootRead(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 *V)
{
  if(!V)return CR_INVALID_PARAMETER;
  CR_STATUS S=Bounds(P,W,O);if(S!=CR_SUCCESS)return S;
  return Exact(P->Io.Read32(P->Io.Context,W,O,V));
}
CR_STATUS BootWrite(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 V)
{
  CR_STATUS S=Bounds(P,W,O);if(S!=CR_SUCCESS)return S;
  if(!P->Admitted || !P->WritersDrained)return CR_NOT_READY;
  P->Touched=TRUE;
  return Exact(P->Io.Write32(P->Io.Context,W,O,V));
}
CR_STATUS BootFence(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W)
{
  CR_STATUS S=BootReady(P,W);
  return S==CR_SUCCESS?Exact(P->Io.Fence(P->Io.Context)):S;
}
CR_STATUS BootDelay(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 Us)
{
  CR_STATUS S=BootReady(P,W);
  return S==CR_SUCCESS?Exact(P->Io.DelayUs(P->Io.Context,Us)):S;
}
CR_STATUS BootChecked(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,
                      UINT32 V,UINT32 Mask)
{
  UINT32 Actual;CR_STATUS S=BootWrite(P,W,O,V);if(S!=CR_SUCCESS)return S;
  S=BootFence(P,W);if(S!=CR_SUCCESS)return S;
  S=BootRead(P,W,O,&Actual);
  return S!=CR_SUCCESS?S:((Actual^V)&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}
CR_STATUS BootUpdate(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,
                     UINT32 Mask,UINT32 V)
{
  UINT32 Actual;CR_STATUS S=BootRead(P,W,O,&Actual);if(S!=CR_SUCCESS)return S;
  return BootChecked(P,W,O,(Actual&~Mask)|(V&Mask),Mask);
}
CR_STATUS BootPoll(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,
                   UINT32 Mask,UINT32 V,UINT32 Us)
{
  if(!Us||Us>100000)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<Us;++I){
    UINT32 Actual;CR_STATUS S=BootRead(P,W,O,&Actual);if(S!=CR_SUCCESS)return S;
    if((Actual&Mask)==V)return CR_SUCCESS;
    S=BootDelay(P,W,1);if(S!=CR_SUCCESS)return S;
  }
  return CR_TIMEOUT;
}

CR_STATUS CrMdssBoot8250Init(CR_MDSS_BOOT8250 *P,CONST CR_MDSS_BOOT_IO *Io)
{
  static const UINT64 Bases[CrMdssBootWindows]={
    0xae00000,0xae01000,0xaeb0000,0xae94000,
    0xae94400,0xae96000,0xae96400,0xaeac000
  };
  static const UINTN Sizes[CrMdssBootWindows]={
    0x1000,0x83208,0x2008,0x400,0x7c0,0x400,0x7c0,0x214
  };
  if(CrMutexCheckLevel()!=CR_SUCCESS || !P || !Io)return CR_INVALID_PARAMETER;
  if(P->Initialized || P->AccessOwned || P->Touched)return CR_ALREADY_STARTED;
  if(!Io->Access||!Io->Ready||!Io->Read32||!Io->Write32||!Io->Fence||!Io->DelayUs||
     !Io->ObserveDma||!Io->DrainWriters||!Io->PanelResetOff||!Io->ExternalClocksOff||
     !Io->PanelResetReadyOff||!Io->ExternalClocksReadyOff)
    return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<CrMdssBootWindows;++I)
    if(Io->Physical[I]!=Bases[I]||Io->Bytes[I]<Sizes[I])return CR_UNSUPPORTED;
  CR_MDSS_BOOT_IO Saved=*Io; /* Includes the valid self-alias initialization. */
  cr_memset(P,0,sizeof(*P));P->Io=Saved;P->Initialized=TRUE;return CR_SUCCESS;
}

CR_STATUS CrMdssBoot8250Observe(CR_MDSS_BOOT8250 *P)
{
  CR_STATUS S=Valid(P);if(S!=CR_SUCCESS)return S;
  if(P->Touched||P->Retired)return CR_NOT_READY;
  P->Admitted=FALSE;
  if(!P->AccessOwned){
    S=Exact(P->Io.Access(P->Io.Context,TRUE,&P->AccessOwned,&P->Generation));
    if(S!=CR_SUCCESS)return P->LastStatus=S;
    if(!P->AccessOwned||!P->Generation)return P->LastStatus=CR_NOT_READY;
  }
  /* Preserve even an unsupported complete snapshot for diagnostics. Observe
   * clears its output first; a partial read is not admitted. */
  S=BootObserve(P,&P->Before);
  if(S==CR_SUCCESS)S=BootAdmit(&P->Before);
  P->Admitted=S==CR_SUCCESS;P->LastStatus=S;return S;
}

CR_STATUS CrMdssBoot8250Abort(CR_MDSS_BOOT8250 *P)
{
  CR_STATUS S=Valid(P);if(S!=CR_SUCCESS)return S;
  if(P->Touched||P->Retired||P->WritersDrained)return CR_BUSY;
  if(P->AccessOwned){
    /* A failed lease withdrawal may no longer grant MMIO; retry that retained
     * provider token without first demanding the obsolete access state. */
    S=Exact(P->Io.Access(P->Io.Context,FALSE,&P->AccessOwned,&P->Generation));
    if(S!=CR_SUCCESS)return P->LastStatus=S;
    if(P->AccessOwned)return P->LastStatus=CR_NOT_READY;
  }
  cr_memset(P,0,sizeof(*P));return CR_SUCCESS;
}
