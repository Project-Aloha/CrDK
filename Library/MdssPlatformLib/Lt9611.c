/* LT9611UXC banked MCU video setup, based on native Lt9611uxcCrDxe and
 * Linux drivers/gpu/drm/bridge/lontium-lt9611uxc.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdssPlatformInternal.h"
STATIC CR_STATUS Exact(CR_STATUS S) {return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
STATIC CR_STATUS Write(CR_MDSS_PLATFORM *P, UINT8 Reg, UINT8 Value) {
  return Exact(P->Io.I2cWrite(P->Io.Context,P,Reg,Value));
}
STATIC CR_STATUS Read(CR_MDSS_PLATFORM *P, UINT8 Reg, UINT8 *Value) {
  return Exact(P->Io.I2cRead(P->Io.Context,P,Reg,Value));
}
STATIC CR_STATUS Unlock(CR_MDSS_PLATFORM *P) {
  if (!P->McuLockPending) return CR_SUCCESS;
  CR_STATUS S=Write(P,0xff,0x80); if (CR_ERROR(S)) return S;
  /* Never write MCU control in an unknown page after a failed page select. */
  S=Write(P,0xee,0); if (CR_ERROR(S)) return S;
  P->McuLockPending=FALSE; P->Io.DelayUs(P->Io.Context,50000); return CR_SUCCESS;
}
STATIC CR_STATUS Lock(CR_MDSS_PLATFORM *P) {
  CR_STATUS S=Write(P,0xff,0x80); if (CR_ERROR(S)) return S;
  /* An I2C error can be reported after the device accepted this write. */
  P->McuLockPending=TRUE; return Write(P,0xee,1);
}
STATIC CR_STATUS End(CR_MDSS_PLATFORM *P, CR_STATUS Operation) {
  CR_STATUS S=Unlock(P); return CR_ERROR(Operation) ? Operation : S;
}
STATIC CR_STATUS LowNibble(CR_MDSS_PLATFORM *P, UINT8 Reg, UINT8 Value) {
  UINT8 Current; CR_STATUS S=Read(P,Reg,&Current); if (CR_ERROR(S)) return S;
  return Write(P,Reg,(Current&0xf0)|(Value&15));
}
STATIC CR_STATUS IdentityAndFirmware(CR_MDSS_PLATFORM *P) {
  UINT8 Chip[3]={0}, Version=0;
  CR_STATUS S=Lock(P); if (CR_ERROR(S)) return End(P,S);
  S=Write(P,0xff,0x81);
  for (UINT32 I=0; I<3 && !CR_ERROR(S); ++I) S=Read(P,(UINT8)I,&Chip[I]);
  if (!CR_ERROR(S)) S=Write(P,0xff,0xb0);
  if (!CR_ERROR(S)) S=Read(P,0x21,&Version);
  if (!CR_ERROR(S) && Version==0) S=CR_UNSUPPORTED;
  /* Upstream has no silicon revision whitelist; a nonzero MCU firmware
     version is required. The qualified board route supplies device identity. */
  return End(P,S);
}
STATIC CR_STATUS Video(CR_MDSS_PLATFORM *P) {
  CONST CR_MDSS_MODE *M=&P->Config.Mode;
  UINT32 H=M->Width+M->HSync+M->HBackPorch+M->HFrontPorch;
  UINT32 V=M->Height+M->VSync+M->VBackPorch+M->VFrontPorch;
  CONST UINT8 Values[]={ (UINT8)(V>>8),(UINT8)V,(UINT8)(M->Height>>8),(UINT8)M->Height,
    (UINT8)(H>>8),(UINT8)H,(UINT8)(M->Width>>8),(UINT8)M->Width,(UINT8)M->VSync };
  CR_STATUS S=Lock(P); if (CR_ERROR(S)) return End(P,S);
  S=Write(P,0xff,0xd0);
  for (UINT32 I=0; I<ARRAY_COUNT(Values) && !CR_ERROR(S); ++I) S=Write(P,(UINT8)(0x0d+I),Values[I]);
  if (!CR_ERROR(S)) S=LowNibble(P,0x16,(UINT8)(M->HSync>>8));
  if (!CR_ERROR(S)) S=Write(P,0x17,(UINT8)M->HSync);
  if (!CR_ERROR(S)) S=LowNibble(P,0x18,(UINT8)(M->VFrontPorch>>8));
  if (!CR_ERROR(S)) S=Write(P,0x19,(UINT8)M->VFrontPorch);
  if (!CR_ERROR(S)) S=LowNibble(P,0x1a,(UINT8)(M->HFrontPorch>>8));
  if (!CR_ERROR(S)) S=Write(P,0x1b,(UINT8)M->HFrontPorch);
  return End(P,S);
}
CR_STATUS CrMdssPlatformBridgePrepare(CR_MDSS_PLATFORM *P) {
  CONST UINT32 Pins[]={9,109}; CR_STATUS S;
  if (P->McuLockPending || P->ResetTouched || P->BridgePowerTouched[0] || P->BridgePowerTouched[1])
    return CR_NOT_READY;
  for (UINT32 I=0; I<ARRAY_COUNT(Pins); ++I) {
    P->BridgePowerTouched[I]=TRUE; S=Exact(P->Io.Gpio(P->Io.Context,P,Pins[I],TRUE));
    if (CR_ERROR(S)) return S;
  }
  P->ResetTouched=TRUE;
  S=Exact(P->Io.Gpio(P->Io.Context,P,107,TRUE)); if (CR_ERROR(S)) return S;
  P->Io.DelayUs(P->Io.Context,20000);
  S=Exact(P->Io.Gpio(P->Io.Context,P,107,FALSE)); if (CR_ERROR(S)) return S;
  P->Io.DelayUs(P->Io.Context,20000);
  S=Exact(P->Io.Gpio(P->Io.Context,P,107,TRUE)); if (CR_ERROR(S)) return S;
  P->Io.DelayUs(P->Io.Context,300000);
  S=IdentityAndFirmware(P); if (CR_ERROR(S)) return S;
  return Video(P);
}
CR_STATUS CrMdssPlatformBridgeUnprepare(CR_MDSS_PLATFORM *P) {
  CR_STATUS S=Unlock(P); if (CR_ERROR(S)) return S;
  if (P->ResetTouched) {
    S=Exact(P->Io.Gpio(P->Io.Context,P,107,FALSE)); if (CR_ERROR(S)) return S;
    P->ResetTouched=FALSE;
  }
  CONST UINT32 Pins[]={9,109};
  for (UINT32 I=ARRAY_COUNT(Pins); I-- > 0;) if (P->BridgePowerTouched[I]) {
    S=Exact(P->Io.Gpio(P->Io.Context,P,Pins[I],FALSE)); if (CR_ERROR(S)) return S;
    P->BridgePowerTouched[I]=FALSE;
  }
  return CR_SUCCESS;
}
