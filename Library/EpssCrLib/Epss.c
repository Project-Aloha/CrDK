/* EPSS register/LUT facts: Linux qcom-cpufreq-hw.c, sm8450.dtsi.
 * Independently implemented; no Linux source included. SPDX-License-Identifier: MIT */
#include <Library/EpssCrLib.h>
#include <oskal/cr_string.h>
STATIC CR_STATUS Exact(CR_STATUS S) {return S==CR_SUCCESS ? S : CR_ERROR(S) ? S : CR_NOT_READY;}
CR_STATUS CrEpssQualify(CONST CR_EPSS_DESCRIPTOR *D,UINT32 Count) {
  if (!D || Count!=CR_EPSS_DOMAINS) return CR_INVALID_PARAMETER;
  CONST UINT32 Masks[3]={0x0f,0x70,0x80};UINT32 Seen=0;
  for (UINT32 I=0;I<Count;++I) {
    UINT32 N=D[I].Domain;
    if (N>=3 || (Seen&(1U<<N)) || D[I].SoC!=0x8450 || D[I].Board!=1 ||
        D[I].Base!=0x17d91000ULL+(UINT64)N*0x1000 || D[I].Length!=CR_EPSS_WINDOW_SIZE || D[I].CpuMask!=Masks[N]) return CR_UNSUPPORTED;
    Seen|=1U<<N;
  }
  return Seen==7 ? CR_SUCCESS : CR_UNSUPPORTED;
}
STATIC CR_STATUS Read(CONST CR_EPSS_IO *Io,UINT32 D,UINT32 Offset,UINT32 *V) {
  CR_STATUS S=Exact(Io->Ready(Io->Context,FALSE));if (S!=CR_SUCCESS) return S;
  return Exact(Io->Read32(Io->Context,D,Offset,V));
}
STATIC CR_STATUS Decode(CONST CR_EPSS_CLOCKS *C,UINT32 Raw,UINT32 *Khz) {
  UINT64 Hz;
  if (Raw>>30) {
    UINT32 L=Raw&0xffU;if (!L || C->XoRateHz>CR_MAX_UINT64/L) return CR_COMPROMISED_DATA;
    Hz=C->XoRateHz*L;
  } else Hz=C->AlternateRateHz/2U;
  Hz/=1000U;if (!Hz || Hz>CR_MAX_UINT32) return CR_COMPROMISED_DATA;
  *Khz=(UINT32)Hz;return CR_SUCCESS;
}
CR_STATUS CrEpssInitialize(CR_EPSS *E,CONST CR_EPSS_IO *Io,CONST CR_EPSS_CLOCKS *C,CONST CR_EPSS_DESCRIPTOR *Descriptors,UINT32 Count) {
  if (!E || !Io || !Io->Context || !Io->Ready || !Io->Read32 || !Io->Write32 || !Io->Barrier || !C || E->Initialized) return CR_INVALID_PARAMETER;
  CR_STATUS S=CrEpssQualify(Descriptors,Count);if (S!=CR_SUCCESS) return S;
  if (!C->XoRateHz || !C->AlternateRateHz || C->AlternateRateHz%2U) return CR_INVALID_PARAMETER;
  CR_EPSS New;cr_memset(&New,0,sizeof(New));cr_memcpy(&New.Io,Io,sizeof(*Io));cr_memcpy(&New.Clocks,C,sizeof(*C));
  for (UINT32 N=0;N<CR_EPSS_DOMAINS;++N) {
    CR_EPSS_DOMAIN *D=&New.Domains[N];UINT32 Enabled;
    D->CpuMask=N==0 ? 0x0f : N==1 ? 0x70 : 0x80;D->MemberCount=N==0 ? 4 : N==1 ? 3 : 1;
    S=Read(Io,N,CR_EPSS_ENABLE,&Enabled);if (S!=CR_SUCCESS) return S;if (!(Enabled&1U)) return CR_NOT_READY;
    S=Read(Io,N,CR_EPSS_DCVS_CONTROL,&D->RawControl);if (S!=CR_SUCCESS) return S;D->PerCore=(D->RawControl&1U)!=0;
    UINT32 Previous=0;
    for (UINT32 I=0;I<CR_EPSS_LUT_ROWS;++I) {
      CR_EPSS_STATE *Row=&D->Rows[I];Row->HardwareIndex=I;
      S=Read(Io,N,CR_EPSS_FREQUENCY_LUT+I*4U,&Row->RawFrequency);if (S!=CR_SUCCESS) return S;
      S=Read(Io,N,CR_EPSS_VOLTAGE_LUT+I*4U,&Row->RawVoltage);if (S!=CR_SUCCESS) return S;
      S=Decode(C,Row->RawFrequency,&Row->FrequencyKhz);if (S!=CR_SUCCESS) return S;
      Row->VoltageUv=(Row->RawVoltage&0xfffU)*1000U;D->RowCount=I+1U;
      if (I && Previous==Row->FrequencyKhz) break;
      Previous=Row->FrequencyKhz;
      if (((Row->RawFrequency>>16)&7U)==1U) continue;
      if (!Row->VoltageUv) return CR_COMPROMISED_DATA;
      /* Sorting the portable presentation never renumbers the hardware LUT. */
      UINT32 At=D->StateCount;
      for (UINT32 J=0;J<D->StateCount;++J) {
        if (D->States[J].FrequencyKhz==Row->FrequencyKhz) return CR_COMPROMISED_DATA;
        if (D->States[J].FrequencyKhz>Row->FrequencyKhz) {At=J;break;}
      }
      for (UINT32 J=D->StateCount;J>At;--J) cr_memcpy(&D->States[J],&D->States[J-1],sizeof(*Row));
      cr_memcpy(&D->States[At],Row,sizeof(*Row));++D->StateCount;
    }
    if (!D->StateCount) return CR_UNSUPPORTED;
    /* A stable double-read excludes a torn initialization and an enabled/
     * per-core mode transition. Later IO retains exact selected-row checks. */
    S=Exact(Io->Barrier(Io->Context));if (S!=CR_SUCCESS) return S;
    for (UINT32 I=0;I<D->RowCount;++I) {
      UINT32 F,V;S=Read(Io,N,CR_EPSS_FREQUENCY_LUT+I*4U,&F);if (S!=CR_SUCCESS) return S;
      S=Read(Io,N,CR_EPSS_VOLTAGE_LUT+I*4U,&V);if (S!=CR_SUCCESS) return S;
      if (F!=D->Rows[I].RawFrequency || V!=D->Rows[I].RawVoltage) return CR_NOT_READY;
    }
    S=Read(Io,N,CR_EPSS_ENABLE,&Enabled);if (S!=CR_SUCCESS) return S;if (!(Enabled&1U)) return CR_NOT_READY;
    UINT32 Control;S=Read(Io,N,CR_EPSS_DCVS_CONTROL,&Control);if (S!=CR_SUCCESS) return S;
    if (Control!=D->RawControl) return CR_NOT_READY;
  }
  S=Exact(Io->Ready(Io->Context,TRUE));if (S!=CR_SUCCESS) return S;
  New.Initialized=TRUE;cr_memcpy(E,&New,sizeof(New));return CR_SUCCESS;
}
CR_STATUS CrEpssReady(CR_EPSS *E,UINT32 N,BOOLEAN Write) {
  if (!E || N>=CR_EPSS_DOMAINS) return CR_INVALID_PARAMETER;
  if (!E->Initialized) return CR_NOT_READY;
  CR_STATUS S=Exact(E->Io.Ready(E->Io.Context,Write));if (S!=CR_SUCCESS) return S;
  UINT32 Enable,Control;S=Read(&E->Io,N,CR_EPSS_ENABLE,&Enable);if (S!=CR_SUCCESS) return S;
  S=Read(&E->Io,N,CR_EPSS_DCVS_CONTROL,&Control);if (S!=CR_SUCCESS) return S;
  return (Enable&1U) && Control==E->Domains[N].RawControl ? CR_SUCCESS : CR_NOT_READY;
}
CR_STATUS CrEpssGetStates(CR_EPSS *E,UINT32 N,CR_EPSS_STATE *States,UINT32 Capacity,UINT32 *Count) {
  if (!States || !Count) return CR_INVALID_PARAMETER;
  CR_STATUS S=CrEpssReady(E,N,FALSE);if (S!=CR_SUCCESS) return S;
  CR_EPSS_DOMAIN *D=&E->Domains[N];if (Capacity<D->StateCount) return CR_BUFFER_TOO_SMALL;
  cr_memcpy(States,D->States,D->StateCount*sizeof(*States));*Count=D->StateCount;return CR_SUCCESS;
}
CR_STATUS CrEpssReadRequested(CR_EPSS *E,UINT32 N,CR_EPSS_REQUESTED *Out) {
  if (!Out) return CR_INVALID_PARAMETER;
  CR_STATUS S=CrEpssReady(E,N,FALSE);if (S!=CR_SUCCESS) return S;
  CR_EPSS_DOMAIN *D=&E->Domains[N];UINT32 Index;
  S=Read(&E->Io,N,CR_EPSS_PERFORMANCE_STATE,&Index);if (S!=CR_SUCCESS) return S;
  if (Index>=D->RowCount) return CR_COMPROMISED_DATA;
  if (D->PerCore) for (UINT32 I=1;I<D->MemberCount;++I) {
    UINT32 Other;S=Read(&E->Io,N,CR_EPSS_PERFORMANCE_STATE+I*4U,&Other);if (S!=CR_SUCCESS) return S;
    if (Other!=Index) return CR_NOT_READY;
  }
  CR_EPSS_REQUESTED R={Index,D->Rows[Index].FrequencyKhz,D->PerCore};cr_memcpy(Out,&R,sizeof(R));return CR_SUCCESS;
}
CR_STATUS CrEpssReadFrequency(CR_EPSS *E,UINT32 N,UINT64 *Hz) {
  if (!Hz) return CR_INVALID_PARAMETER;
  CR_STATUS S=CrEpssReady(E,N,FALSE);if (S!=CR_SUCCESS) return S;
  UINT32 State;S=Read(&E->Io,N,CR_EPSS_DOMAIN_STATE,&State);if (S!=CR_SUCCESS) return S;
  UINT32 L=State&0xffU;if (!L) return CR_NOT_READY;
  if (E->Clocks.XoRateHz>CR_MAX_UINT64/L) return CR_COMPROMISED_DATA;
  *Hz=E->Clocks.XoRateHz*L;return CR_SUCCESS;
}
CR_STATUS CrEpssSamplePressure(CR_EPSS *E,UINT32 N,CR_EPSS_PRESSURE *Out) {
  if (!Out) return CR_INVALID_PARAMETER;
  CR_EPSS_REQUESTED Requested;CR_EPSS_PRESSURE P;
  cr_memset(&P,0,sizeof(P));
  CR_STATUS S=CrEpssReadRequested(E,N,&Requested);if (S!=CR_SUCCESS) return S;
  S=CrEpssReadFrequency(E,N,&P.ObservedHz);if (S!=CR_SUCCESS) return S;
  CR_EPSS_DOMAIN *D=&E->Domains[N];UINT32 Floor=0,Lowest=CR_MAX_UINT32;
  /* Hardware rows may be unsorted; adjacent duplicate terminators do not
   * affect floor/ceiling. Linux accepts boost rows for hardware pressure. */
  for (UINT32 I=0;I<D->RowCount;++I) {
    UINT32 F=D->Rows[I].FrequencyKhz;
    if (F<Lowest) Lowest=F;
    if ((UINT64)F*1000U<=P.ObservedHz && F>Floor) Floor=F;
  }
  if (!D->RowCount || !Lowest || Lowest==CR_MAX_UINT32) return CR_COMPROMISED_DATA;
  P.NormalizedKhz=Floor ? Floor : Lowest;P.RequestedKhz=Requested.FrequencyKhz;
  P.Throttled=P.NormalizedKhz<P.RequestedKhz;
  cr_memcpy(Out,&P,sizeof(P));return CR_SUCCESS;
}
CR_STATUS CrEpssAcknowledgeLmh(CR_EPSS *E,UINT32 N) {
  CR_STATUS S=CrEpssReady(E,N,TRUE);if (S!=CR_SUCCESS) return S;
  CR_EPSS_DOMAIN *D=&E->Domains[N];D->AckPending=TRUE;D->Touched=TRUE;
  S=Exact(E->Io.Ready(E->Io.Context,TRUE));if (S!=CR_SUCCESS) goto Done;
  S=Exact(E->Io.Write32(E->Io.Context,N,CR_EPSS_INTERRUPT_CLEAR,CR_EPSS_GT_INTERRUPT_STATUS));if (S!=CR_SUCCESS) goto Done;
  S=Exact(E->Io.Barrier(E->Io.Context));if (S!=CR_SUCCESS) goto Done;
  S=CrEpssReady(E,N,TRUE);if (S!=CR_SUCCESS) goto Done;
  D->AckPending=FALSE;
Done:
  D->LastError=S;return S;
}
CR_STATUS CrEpssSet(CR_EPSS *E,UINT32 N,UINT32 Ordinal) {
  if (!E || N>=CR_EPSS_DOMAINS || !E->Initialized || Ordinal>=E->Domains[N].StateCount) return CR_INVALID_PARAMETER;
  CR_EPSS_DOMAIN *D=&E->Domains[N];CR_EPSS_STATE *Row=&D->States[Ordinal];UINT32 Index=Row->HardwareIndex;
  if (D->Pending && D->PendingIndex!=Index) return CR_BUSY;
  CR_STATUS S=CrEpssReady(E,N,TRUE);if (S!=CR_SUCCESS) {D->LastError=S;return S;}
  UINT32 F,V;S=Read(&E->Io,N,CR_EPSS_FREQUENCY_LUT+Index*4U,&F);if (S!=CR_SUCCESS) goto Done;
  S=Read(&E->Io,N,CR_EPSS_VOLTAGE_LUT+Index*4U,&V);if (S!=CR_SUCCESS) goto Done;
  if (F!=Row->RawFrequency || V!=Row->RawVoltage) {S=CR_NOT_READY;goto Done;}
  D->PendingIndex=Index;D->Pending=TRUE;D->Touched=TRUE;
  for (UINT32 I=0;I<(D->PerCore ? D->MemberCount : 1U);++I) {
    S=Exact(E->Io.Ready(E->Io.Context,TRUE));if (S!=CR_SUCCESS) goto Done;
    S=Exact(E->Io.Write32(E->Io.Context,N,CR_EPSS_PERFORMANCE_STATE+I*4U,Index));if (S!=CR_SUCCESS) goto Done;
  }
  S=Exact(E->Io.Barrier(E->Io.Context));if (S!=CR_SUCCESS) goto Done;
  for (UINT32 I=0;I<(D->PerCore ? D->MemberCount : 1U);++I) {
    UINT32 Current;S=Read(&E->Io,N,CR_EPSS_PERFORMANCE_STATE+I*4U,&Current);if (S!=CR_SUCCESS) goto Done;
    if (Current!=Index) {S=CR_NOT_READY;goto Done;}
  }
  D->Pending=FALSE;
Done:
  D->LastError=S;return S;
}
BOOLEAN CrEpssHasPending(CONST CR_EPSS *E) {
  if (!E || !E->Initialized) return FALSE;
  for (UINT32 I=0;I<CR_EPSS_DOMAINS;++I) if (E->Domains[I].Pending || E->Domains[I].AckPending) return TRUE;
  return FALSE;
}
