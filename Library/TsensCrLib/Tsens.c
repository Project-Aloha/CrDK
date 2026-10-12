/* TSENS facts: Linux drivers/thermal/qcom/tsens{,-v2}.c and sm8450.dtsi.
 * No Linux code is included. SPDX-License-Identifier: MIT */
#include <Library/TsensCrLib.h>

CR_STATUS CrTsensQualify(CONST CR_TSENS_DESCRIPTOR *D,UINT32 Count) {
  if (!D || Count!=CR_TSENS_BANKS) return CR_INVALID_PARAMETER;
  CONST UINT64 Tm[2]={0xc263000ULL,0xc265000ULL},Srot[2]={0xc222000ULL,0xc223000ULL};
  UINT32 Seen=0;
  for (UINT32 I=0;I<Count;++I) {
    UINT32 B=D[I].Bank;
    if (B>=2 || (Seen&(1U<<B)) || D[I].SoC!=0x8450 || D[I].Board!=1 ||
        D[I].TmBase!=Tm[B] || D[I].SrotBase!=Srot[B] ||
        D[I].TmLength!=CR_TSENS_WINDOW_SIZE || D[I].SrotLength!=CR_TSENS_WINDOW_SIZE ||
        D[I].UpperLowerIrq!=538U+B || D[I].CriticalIrq!=540U+B)
      return CR_UNSUPPORTED;
    Seen|=1U<<B;
  }
  return Seen==3 ? CR_SUCCESS : CR_UNSUPPORTED;
}
CR_STATUS CrTsensInitialize(CR_TSENS *T,CONST CR_TSENS_IO *Io,CONST CR_TSENS_DESCRIPTOR *D,UINT32 Count) {
  if (!T || !Io || !Io->Read32 || !Io->Context || T->Initialized) return CR_INVALID_PARAMETER;
  CR_STATUS S=CrTsensQualify(D,Count);if (CR_ERROR(S)) return S;
  CR_TSENS New={0};New.Io=*Io;
  for (UINT32 B=0;B<CR_TSENS_BANKS;++B) {
    S=Io->Read32(Io->Context,B,CrTsensSrot,CR_TSENS_SROT_VERSION,&New.Version[B]);if (CR_ERROR(S)) return S;
    if ((New.Version[B]>>28)!=2) return CR_UNSUPPORTED;
    S=Io->Read32(Io->Context,B,CrTsensSrot,CR_TSENS_SROT_CONTROL,&New.Control[B]);if (CR_ERROR(S)) return S;
    /* TSENS_EN, deasserted SW_RESET, and explicitly calibrated TEMP format.
     * RPM/XBL remains the calibration owner; no invented slopes or offsets. */
    if (!(New.Control[B]&1U) || (New.Control[B]&2U) || !(New.Control[B]&(1U<<21))) return CR_NOT_READY;
    New.SensorMask[B]=(UINT16)((New.Control[B]>>3)&0xffffU);
    if (!New.SensorMask[B]) return CR_NOT_READY;
  }
  New.Initialized=TRUE;*T=New;return CR_SUCCESS;
}
CR_STATUS CrTsensRead(CR_TSENS *T,UINT32 Bank,UINT32 Sensor,CR_TSENS_SAMPLE *Out) {
  if (!T || !Out || Bank>=CR_TSENS_BANKS || Sensor>=CR_TSENS_SENSORS) return CR_INVALID_PARAMETER;
  if (!T->Initialized || !T->Io.Read32 || !T->Io.Context) return CR_NOT_READY;
  UINT32 Control;
  CR_STATUS S=T->Io.Read32(T->Io.Context,Bank,CrTsensSrot,CR_TSENS_SROT_CONTROL,&Control);if (CR_ERROR(S)) return S;
  if (!(Control&1U) || (Control&2U) || !(Control&(1U<<21)) || !(Control&(1U<<(3U+Sensor)))) return CR_NOT_READY;
  /* Three reads maximum; an asserted VALID accepts immediately. Otherwise the
   * first matching adjacent temperature pair is the documented stable sample.
   * Only temperature bits participate: IRQ status/VALID may change meanwhile. */
  UINT32 Status[3],Code[3],Selected=0;BOOLEAN Valid=FALSE,Fallback=FALSE;UINT8 Reads=0;
  for (UINT32 I=0;I<3;++I) {
    S=T->Io.Read32(T->Io.Context,Bank,CrTsensTm,CR_TSENS_TM_SENSOR_STATUS+Sensor*4U,&Status[I]);if (CR_ERROR(S)) return S;
    Code[I]=Status[I]&0xfffU;Reads=(UINT8)(I+1);
    if (Status[I]&CR_TSENS_VALID) {Selected=I;Valid=TRUE;break;}
  }
  if (!Valid) {
    if (Code[0]==Code[1]) Selected=1;
    else if (Code[1]==Code[2]) Selected=2;
    else return CR_NOT_READY;
    Fallback=TRUE;
  }
  INT32 DeciC=(INT32)Code[Selected];if (DeciC&0x800) DeciC-=0x1000;
  CR_TSENS_SAMPLE Sample={.MilliCelsius=DeciC*100,.RawStatus=Status[Selected],.HardwareVersion=T->Version[Bank],
    .SensorMask=(UINT16)((Control>>3)&0xffffU),.ReadCount=Reads,.ValidSample=Valid,.StableFallback=Fallback};
  *Out=Sample;return CR_SUCCESS;
}
VOID CrTsensClear(CR_TSENS *T) {if (T) *T=(CR_TSENS){0};}
