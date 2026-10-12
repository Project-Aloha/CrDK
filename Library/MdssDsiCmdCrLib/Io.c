/* Exact native owner admission around bounded portable MMIO.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
CR_STATUS CmdReady(CR_MDSS_CMD *D, CR_MDSS_CMD_WINDOW W){
  if(!D||!D->Initialized||W>=CrMdssCmdWindows)return CR_INVALID_PARAMETER;
  return Exact(D->Io.Ready(D->Io.Context,W));
}
STATIC CR_STATUS Bounds(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O){
  CR_STATUS S=CmdReady(D,W);if(S!=CR_SUCCESS)return S;
  return (O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4?CR_BAD_BUFFER_SIZE:CR_SUCCESS;
}
CR_STATUS CmdRead(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 *V){
  if(!V)return CR_INVALID_PARAMETER;CR_STATUS S=Bounds(D,W,O);if(S!=CR_SUCCESS)return S;
  return Exact(D->Io.Read32(D->Io.Context,W,O,V));
}
CR_STATUS CmdWrite(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V){
  CR_STATUS S=Bounds(D,W,O);if(S!=CR_SUCCESS)return S;return Exact(D->Io.Write32(D->Io.Context,W,O,V));
}
CR_STATUS CmdFence(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W){
  CR_STATUS S=CmdReady(D,W);return S==CR_SUCCESS?Exact(D->Io.Fence(D->Io.Context)):S;
}
CR_STATUS CmdDelay(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 Us){
  /* Panel sleep-out/sleep-in require up to 120 ms. Native suppliers bound
   * each callback to 20 ms; keep the full interval and recheck the owner
   * between chunks, as the shared video-panel path does. */
  if(!Us||Us>120000)return CR_INVALID_PARAMETER;
  while(Us){UINT32 Chunk=Us>20000?20000:Us;
    CR_STATUS S=CmdReady(D,W);if(S!=CR_SUCCESS)return S;
    S=Exact(D->Io.DelayUs(D->Io.Context,Chunk));if(S!=CR_SUCCESS)return S;
    Us-=Chunk;
  }
  return CmdReady(D,W);
}
CR_STATUS CmdUpdate(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 Mask,UINT32 Value){
  UINT32 V;CR_STATUS S=CmdRead(D,W,O,&V);if(S!=CR_SUCCESS)return S;
  return CmdWrite(D,W,O,(V&~Mask)|(Value&Mask));
}
CR_STATUS CmdChecked(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 Value,UINT32 Mask){
  UINT32 V;CR_STATUS S=CmdWrite(D,W,O,Value);if(S!=CR_SUCCESS)return S;
  S=CmdFence(D,W);if(S!=CR_SUCCESS)return S;S=CmdRead(D,W,O,&V);
  return S!=CR_SUCCESS?S:((V^Value)&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}
CR_STATUS CmdPoll(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 Mask,UINT32 Value,UINT32 Us){
  if(!Us||Us>100000)return CR_INVALID_PARAMETER;
  for(UINT32 I=0;I<Us;++I){UINT32 V;CR_STATUS S=CmdRead(D,W,O,&V);if(S!=CR_SUCCESS)return S;
    if((V&Mask)==Value)return CR_SUCCESS;
    S=CmdDelay(D,W,1);if(S!=CR_SUCCESS)return S;}
  return CR_TIMEOUT;
}
CR_STATUS CmdDmaReady(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W){
  CR_STATUS S=CmdReady(D,W);if(S!=CR_SUCCESS)return S;
  if(W!=CrMdssCmdDpu&&W!=CrMdssCmdHost)return CR_INVALID_PARAMETER;
  CR_MDSS_CMD_MEMORY *M=W==CrMdssCmdDpu?&D->Frame:&D->Commands;
  return Exact(M->Ready(M->Context,W));
}
