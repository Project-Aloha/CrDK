/* Shared bounded status-returning IO for the extended display modules.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssDsiCrLib.h>
#define XB(n) (1U<<(n))
static inline CR_STATUS XExact(CR_STATUS S)
{return S==CR_SUCCESS?S:CR_ERROR(S)?S:CR_NOT_READY;}
static inline CR_STATUS XReady(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W)
{return XExact(D->Io.Ready(D->Io.Context,W));}
static inline CR_STATUS XRead(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 *V)
{CR_STATUS S;if((O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 S=XReady(D,W);return S==CR_SUCCESS?XExact(D->Io.Read32(D->Io.Context,W,O,V)):S;}
static inline CR_STATUS XWrite(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 V)
{CR_STATUS S;if((O&3)||D->Io.Bytes[W]<4||O>D->Io.Bytes[W]-4)return CR_BAD_BUFFER_SIZE;
 S=XReady(D,W);return S==CR_SUCCESS?XExact(D->Io.Write32(D->Io.Context,W,O,V)):S;}
static inline CR_STATUS XFence(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W)
{CR_STATUS S=XReady(D,W);return S==CR_SUCCESS?XExact(D->Io.Fence(D->Io.Context)):S;}
static inline CR_STATUS XDelay(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 Us)
{CR_STATUS S=XReady(D,W);return S==CR_SUCCESS?XExact(D->Io.DelayUs(D->Io.Context,Us)):S;}
static inline CR_STATUS XPoll(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 M,UINT32 V,UINT32 Us)
{UINT32 I,A;CR_STATUS S;for(I=0;I<=Us;I+=10){S=XRead(D,W,O,&A);if(S!=CR_SUCCESS)return S;
 if((A&M)==V)return CR_SUCCESS;if(I==Us)break;S=XDelay(D,W,10);if(S!=CR_SUCCESS)return S;}return CR_TIMEOUT;}
static inline CR_STATUS XChecked(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 V,UINT32 Mask)
{CR_STATUS S=XWrite(D,W,O,V);if(S!=CR_SUCCESS)return S;S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 return XPoll(D,W,O,Mask,V&Mask,1000);}
static inline CR_STATUS XUpdate(CR_MDSS_DSI *D,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 Mask,UINT32 V)
{UINT32 A;CR_STATUS S=XRead(D,W,O,&A);if(S!=CR_SUCCESS)return S;
 return XChecked(D,W,O,(A&~Mask)|V,Mask);}
static inline CR_MDSS_EXT_WINDOW XPhy(UINT32 C){return (CR_MDSS_EXT_WINDOW)(CrMdssExtPhy0+C);}
static inline CR_MDSS_EXT_WINDOW XHost(UINT32 C){return (CR_MDSS_EXT_WINDOW)(CrMdssExtDsi0+C);}
