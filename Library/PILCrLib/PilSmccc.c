/* Exact PAS frame qualification and retained standard-call continuation. MIT */
#include <Library/PilSmcccLib.h>
#include <oskal/cr_mutex.h>
#include <oskal/cr_string.h>

STATIC CR_STATUS Check(CR_PIL_SMCCC *T) {
  if (CR_ERROR(CrMutexCheckLevel())) return CR_INVALID_PARAMETER;
  return T!=NULL && T->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC BOOLEAN Shutdown(UINT64 Fid) {return Fid==0x42000206U || Fid==0x02000206U;}
STATIC BOOLEAN FrameAllowed(CONST CR_PIL_SMCCC *T,CONST CR_PIL_SMC_ARGS *A) {
  UINT64 Fid=A->X[0];
  if (A->X[5]!=0 || A->X[7]!=0 || A->X[6]!=0) return FALSE;
  if (Fid==0xc2000601U || Fid==0x82000601U)
    return A->X[1]==1 && A->X[2]==0x02000601U && A->X[3]==0 && A->X[4]==0;
  if (Fid==0x42000601U || Fid==0x02000601U)
    return A->X[1]==1 && (A->X[2]==0x02000201U || A->X[2]==0x02000202U ||
      A->X[2]==0x02000205U || A->X[2]==0x02000206U || A->X[2]==0x02000207U) &&
      A->X[3]==0 && A->X[4]==0;
  if (A->X[2]!=T->Target->PasId || (Fid!=0x42000201U && Fid!=0x02000201U &&
      Fid!=0x42000202U && Fid!=0x02000202U && Fid!=0x42000205U && Fid!=0x02000205U &&
      Fid!=0x42000206U && Fid!=0x02000206U && Fid!=0x42000207U && Fid!=0x02000207U)) return FALSE;
  switch ((UINT32)Fid & 255U) {
  case 1: return A->X[1]==0x82 && A->X[3]!=0 && A->X[3]<=CR_MAX_UINT32 &&
                 (A->X[3]&4095U)==0 && A->X[4]==0;
  case 2: return A->X[1]==3 && A->X[3]==T->Target->MemoryBase && A->X[4]!=0 &&
                 A->X[4]<=T->Target->MemorySize;
  default: return A->X[1]==1 && A->X[3]==0 && A->X[4]==0;
  }
}
STATIC VOID Completed(CR_PIL_SMCCC *T,CONST CR_PIL_SMC_ARGS *A) {
  T->Interrupted=(INT32)A->X[0]==1;
  T->ResumeCookie=T->Interrupted ? A->X[6] : 0;
}
STATIC CR_STATUS Drain(CR_PIL_SMCCC *T) {
  for (UINT32 I=0;T->Interrupted && I<256;++I) {
    CR_PIL_SMC_ARGS A=T->Original;A.X[0]=1;A.X[6]=T->ResumeCookie;
    CrPilArm64Smccc(&A);Completed(T,&A);
  }
  return T->Interrupted ? CR_TIMEOUT : CR_SUCCESS;
}
CR_STATUS CrPilSmcccInitialize(CR_PIL_SMCCC *T,CONST CR_PIL_TARGET *Target) {
  if (CR_ERROR(CrMutexCheckLevel()) || T==NULL || Target==NULL || Target->PasId!=1 ||
      Target->MemorySize==0 || Target->MemoryBase==0 || (Target->MemoryBase&4095U)!=0 ||
      (UINT64)Target->MemoryBase+Target->MemorySize>0x100000000ULL) return CR_INVALID_PARAMETER;
  if (T->Initialized) return CR_ALREADY_STARTED;
  cr_memset(T,0,sizeof(*T));T->Target=Target;T->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrPilSmcccCall(VOID *Context,CR_PIL_SMC_ARGS *A) {
  CR_PIL_SMCCC *T=Context;CR_STATUS Status=Check(T);
  if (CR_ERROR(Status) || A==NULL || ((UINTN)A&7U)!=0) return CR_INVALID_PARAMETER;
  if (A->X[0]==1) {
    if (!T->Interrupted || A->X[6]!=T->ResumeCookie) return CR_INVALID_PARAMETER;
    for (UINT32 I=1;I<8;++I) if (I!=6 && A->X[I]!=T->Original.X[I]) return CR_INVALID_PARAMETER;
  } else {
    if (!FrameAllowed(T,A)) return CR_INVALID_PARAMETER;
    if (T->Interrupted) {
      if (!Shutdown(A->X[0])) return CR_NOT_READY;
      Status=Drain(T);if (CR_ERROR(Status)) return Status;
    }
    T->Original=*A;
  }
  CrPilArm64Smccc(A);Completed(T,A);return CR_SUCCESS;
}
CR_STATUS CrPilSmcccUninitialize(CR_PIL_SMCCC *T) {
  CR_STATUS Status=Check(T);if (CR_ERROR(Status)) return Status;
  if (T->Interrupted) return CR_NOT_READY;
  cr_memset(T,0,sizeof(*T));return CR_SUCCESS;
}
CR_STATUS CrPilSmcccProbe(CR_PIL_SMCCC *T) {
  CR_STATUS Status=Check(T);if (CR_ERROR(Status)) return Status;
  if (T->Qualified) return CR_SUCCESS;
  if (T->ProbeStep==0) {
    if (T->Interrupted) return CR_NOT_READY;
    if (CrPilArm64CurrentEl()!=1) return CR_UNSUPPORTED;
    T->ProbeStep=1;
  }
  STATIC CONST UINT32 Services[5]={0x02000201U,0x02000202U,0x02000205U,0x02000206U,0x02000207U};
  while (T->ProbeStep<=8) {
    CR_PIL_SMC_ARGS A={0};
    if (T->ProbeStep<=2) {A.X[0]=T->ProbeStep==1 ? 0xc2000601U : 0x82000601U;A.X[1]=1;A.X[2]=0x02000601U;}
    else if (T->ProbeStep<8) {A.X[0]=T->SmcBase|0x601U;A.X[1]=1;A.X[2]=Services[T->ProbeStep-3];}
    else {A.X[0]=T->SmcBase|0x207U;A.X[1]=1;A.X[2]=T->Target->PasId;}
    if (T->Interrupted) {
      /* Never treat another accepted operation as the pending probe. */
      if (cr_memcmp(&A,&T->Original,sizeof(A))!=0) return CR_NOT_READY;
      A=T->Original;A.X[0]=1;A.X[6]=T->ResumeCookie;
    }
    for (UINT32 I=0;I<256;++I) {
      Status=CrPilSmcccCall(T,&A);if (CR_ERROR(Status)) return Status;
      if (!T->Interrupted) break;
      A=T->Original;A.X[0]=1;A.X[6]=T->ResumeCookie;
    }
    if (T->Interrupted) return CR_TIMEOUT;
    BOOLEAN Supported=A.X[0]==0 && A.X[1]!=0;
    if (T->ProbeStep<=2) {
      if (Supported && A.X[1]==1) {T->SmcBase=T->ProbeStep==1 ? 0x42000000U : 0x02000000U;T->ProbeStep=3;}
      else if (T->ProbeStep==1) T->ProbeStep=2;
      else {T->ProbeStep=0;return CR_UNSUPPORTED;}
    } else {
      if (T->ProbeStep==8) {
        if (A.X[0]!=0) {T->ProbeStep=0;return CR_UNSUPPORTED;}
        T->PasSupportKnown=TRUE;T->PasSupportResult=A.X[1];
      } else if (!Supported) {T->ProbeStep=0;return CR_UNSUPPORTED;}
      ++T->ProbeStep;
    }
  }
  T->Qualified=TRUE;return CR_SUCCESS;
}
