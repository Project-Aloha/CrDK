/**
  Portable PAS, AOSS QMP and SMP2P ADSP lifecycle.
  References: Linux and OnePlus SM8450 Android qcom_q6v5_pas.c,
  qcom_scm.c / qcom_scm-smc.c, smp2p.c, qcom_aoss.c and rpmhpd.c.
  SPDX-License-Identifier: MIT
**/
#include <Library/PILCrLib.h>
#include <oskal/cr_string.h>
#include <oskal/cr_debug.h>
#ifdef _KERNEL_MODE
#include "Pil.tmh"
#endif

#define SMP_MAGIC 0x504d5324U
#define SMP_SIZE 340U
#define SMP_READY (1U << 1)
#define SMP_FATAL (1U << 0)
#define SMP_STOP_ACK (1U << 3)
#define QMP_UP 0x0000ffffU
#define QMP_DOWN 0xffff0000U
#define SCM_SIP 0x02000000U
#define SCM_SMC64 (1U << 30)
#define SCM_FAST (1U << 31)
#define SCM_INFO_AVAILABLE 0x0601U
#define SCM_PAS_SUPPORTED 0x0207U

STATIC CR_STATUS InvokeSmc(CR_PIL_CONTEXT *Pil, CR_PIL_SMC_ARGS *Args)
{
  CR_STATUS Status;
  if (Args->X[0] != 1) Pil->SmcOriginal = *Args;
  Status = Pil->Io.Smc(Pil->Io.Context, Args);
  if (Pil->PasOwned && (Pil->SmcOriginal.X[0] == 0x42000206U ||
                       Pil->SmcOriginal.X[0] == 0x02000206U)) {
    Pil->Shutdown.Generation = Pil->StartGeneration;
    Pil->Shutdown.FunctionId = Pil->SmcOriginal.X[0];
    Pil->Shutdown.Attempted = TRUE;
    Pil->Shutdown.Returned = !CR_ERROR(Status);
    Pil->Shutdown.CallStatus = Status;
    Pil->Shutdown.Complete = FALSE;
    /* A callback failure does not establish that its register frame contains
       a monitor reply. Never report its untouched input as returned evidence. */
    Pil->Shutdown.X0 = CR_ERROR(Status) ? 0 : Args->X[0];
    Pil->Shutdown.X1 = CR_ERROR(Status) ? 0 : Args->X[1];
  }
  if (CR_ERROR(Status)) return Status;
  Pil->SmcInterrupted = (INT32)Args->X[0] == 1;
  Pil->SmcCookie = Pil->SmcInterrupted ? Args->X[6] : 0;
  return CR_SUCCESS;
}

STATIC CR_STATUS RetireSmc(CR_PIL_CONTEXT *Pil, BOOLEAN *ShutdownDone)
{
  CR_STATUS Status;
  CR_PIL_SMC_ARGS Args;
  UINT32 Index;
  *ShutdownDone = FALSE;
  for (Index = 0; Pil->SmcInterrupted && Index < 256; ++Index) {
    Args = Pil->SmcOriginal;
    Args.X[0] = 1; Args.X[6] = Pil->SmcCookie;
    Status = InvokeSmc(Pil, &Args);
    if (CR_ERROR(Status)) return Status;
    if (!Pil->SmcInterrupted && (INT32)Args.X[0] == 0 && Args.X[1] == 0 &&
        (Pil->SmcOriginal.X[0] == 0x42000206U || Pil->SmcOriginal.X[0] == 0x02000206U))
      *ShutdownDone = TRUE;
  }
  return Pil->SmcInterrupted ? CR_TIMEOUT : CR_SUCCESS;
}

STATIC CR_STATUS Smc(CR_PIL_CONTEXT *Pil, UINT32 Base, UINT32 Function, UINT32 ArgInfo,
                       UINTN A, UINTN B, UINTN C, UINTN *Result)
{
  CR_PIL_SMC_ARGS Args;
  CR_STATUS Status;
  UINTN Busy, Resume, Cookie, Call;
  INT32 Transport;
  *Result = 0;
  /* Only Stop may retire a timed-out continuation before beginning another
     secure transaction. Never overlap the pending call with a new PAS call. */
  if (Pil->SmcInterrupted) return CR_NOT_READY;
  for (Busy = 0; Busy < 21; ++Busy) {
    Cookie = 0;
    Call = Base | Function;
    for (Resume = 0; Resume < 256; ++Resume) {
      cr_memset(&Args, 0, sizeof(Args));
      Args.X[0] = Call; Args.X[1] = ArgInfo;
      Args.X[2] = A; Args.X[3] = B; Args.X[4] = C; Args.X[6] = Cookie;
      Status = InvokeSmc(Pil, &Args);
      if (CR_ERROR(Status)) return Status;
      Transport = (INT32)Args.X[0];
      if (Transport != 1) break;
      /* Qualcomm's interrupted standard call resumes with x0=1 and x6
         returned by the previous call; preserve the original arguments. */
      Call = 1; Cookie = Args.X[6];
    }
    if (Resume == 256) {
      log_err("PILCr: SCM fid=0x%x interrupted-call limit reached\n", Base | Function);
      return CR_TIMEOUT;
    }
    /* Include errors and capability replies: the transport and service
       result are distinct, and both are needed to diagnose secure firmware. */
    log_info("PILCr: SCM fid=0x%x info=0x%x x0=0x" CR_LOG_HEX64_FMT " x1=0x" CR_LOG_HEX64_FMT " x2=0x" CR_LOG_HEX64_FMT " x3=0x" CR_LOG_HEX64_FMT "\n",
            Base | Function, ArgInfo, (UINT64)Args.X[0], (UINT64)Args.X[1],
            (UINT64)Args.X[2], (UINT64)Args.X[3]);
    if (Transport == -12) { Pil->Io.DelayUs(Pil->Io.Context, 30000); continue; }
    if (Transport == -4) return CR_UNSUPPORTED;
    if (Transport == -2 || Transport == -3) return CR_INVALID_PARAMETER;
    if (Transport == -5) return CR_OUT_OF_RESOURCES;
    if (Transport != 0) {
      log_err("PILCr: SCM fn=0x%x transport=%d\n", Function, Transport);
      return CR_DEVICE_ERROR;
    }
    *Result = Args.X[1];
    return CR_SUCCESS;
  }
  log_err("PILCr: SCM fid=0x%x busy-call limit reached\n", Base | Function);
  return CR_TIMEOUT;
}

STATIC VOID PasSupportDiagnostic(CR_PIL_CONTEXT *Pil)
{
  CR_STATUS Status;
  UINTN Result;
  /* Both Linux and Android SCM helpers query availability with an owner/SVC/
     command ID, without the FAST or SMC64 flags. Their ADSP remoteproc/MDT
     boot paths do not require PAS_IS_SUPPORTED. Treat that legacy query as
     diagnostic; signed init_image/auth_and_reset remain authoritative. */
  Status = Smc(Pil, Pil->SmcBase, SCM_INFO_AVAILABLE, 1,
                SCM_SIP | SCM_PAS_SUPPORTED, 0, 0, &Result);
  if (CR_ERROR(Status) || Result == 0) {
    log_warn("PILCr: PAS support query unavailable: status=" CR_LOG_STATUS_FMT " result=0x" CR_LOG_HEX64_FMT "; trying signed PAS startup\n",
            Status, (UINT64)Result);
    return;
  }
  Status = Smc(Pil, Pil->SmcBase, SCM_PAS_SUPPORTED, 1,
                Pil->Target->PasId, 0, 0, &Result);
  log_info(
          "PILCr: PAS%u support query: status=" CR_LOG_STATUS_FMT " result=0x" CR_LOG_HEX64_FMT " supported=%u; signed PAS startup determines readiness\n",
          Pil->Target->PasId, Status, (UINT64)Result, !CR_ERROR(Status) && Result != 0);
}

STATIC CR_STATUS Pas(CR_PIL_CONTEXT *Pil, UINT32 Command, UINT32 Info,
                       UINTN B, UINTN C)
{
  CR_STATUS Status;
  UINTN Result = 0;
  Status = Smc(Pil, Pil->SmcBase, 0x0200U | Command, Info,
                Pil->Target->PasId, B, C, &Result);
  if (CR_ERROR(Status)) return Status;
  if (Result != 0) {
    log_err("PILCr: PAS%u cmd=%u result=0x" CR_LOG_HEX64_FMT "\n",
            Pil->Target->PasId, Command, (UINT64)Result);
    return Command == 1 || Command == 5 ? CR_PIL_SECURITY_VIOLATION : CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}

STATIC VOID Kick(CR_PIL_CONTEXT *Pil, UINT32 Client, UINT32 Signal)
{
  Pil->Io.Fence(Pil->Io.Context);
  Pil->Io.Write32(Pil->Io.Context, Pil->Target->IpccSend, (Client << 16) | Signal);
  /* Complete the local send invocation before a final ownership fence.
     SEND_ID is write-only: reading zero cannot prove remote consumption. */
  Pil->Io.Fence(Pil->Io.Context);
}

STATIC CR_STATUS QmpWait(CR_PIL_CONTEXT *Pil, UINT32 Offset, UINT32 Value)
{
  UINTN Count;
  UINT32 Observed = 0;
  for (Count = 0; Count < 1000; ++Count) {
    Observed = Pil->Io.Read32(Pil->Io.Context, Pil->Target->QmpBase + Offset);
    if (Observed == Value) return CR_SUCCESS;
    Pil->Io.DelayUs(Pil->Io.Context, 1000);
  }
  log_err("PILCr: QMP wait timeout offset=0x%x observed=0x%x expected=0x%x\n",
          Offset, Observed, Value);
  return CR_TIMEOUT;
}

STATIC BOOLEAN QmpEstablishedSnapshot(CR_PIL_CONTEXT *Pil, UINT32 *Offset,
                                      UINT32 *Observed, UINT32 *Expected)
{
  STATIC CONST UINT32 Fields[] = {0x0c, 0x10, 0x14, 0x18, 0x24, 0x28, 0x2c, 0x30};
  UINT32 Value, MailboxSize = 0, Base = Pil->Target->QmpBase;
  for (UINT32 Pass = 0; Pass < 2; ++Pass) {
    Value = Pil->Io.Read32(Pil->Io.Context, Base);
    if (Value != 0x4d41494cU) {
      *Offset = 0; *Observed = Value; *Expected = 0x4d41494cU; return FALSE;
    }
    Value = Pil->Io.Read32(Pil->Io.Context, Base + 4);
    if (Value != 1) { *Offset = 4; *Observed = Value; *Expected = 1; return FALSE; }
    Value = Pil->Io.Read32(Pil->Io.Context, Base + 0x38);
    if (Value != Pil->QmpOffset) {
      *Offset = 0x38; *Observed = Value; *Expected = Pil->QmpOffset; return FALSE;
    }
    Value = Pil->Io.Read32(Pil->Io.Context, Base + 0x34);
    if (Value < 68 || Value > Pil->Target->QmpSize - Pil->QmpOffset ||
        (Pass != 0 && Value != MailboxSize)) {
      *Offset = 0x34; *Observed = Value; *Expected = Pass == 0 ? 68 : MailboxSize; return FALSE;
    }
    MailboxSize = Value;
    for (UINT32 Index = 0; Index < sizeof(Fields) / sizeof(Fields[0]); ++Index) {
      Value = Pil->Io.Read32(Pil->Io.Context, Base + Fields[Index]);
      if (Value != QMP_UP) {
        *Offset = Fields[Index]; *Observed = Value; *Expected = QMP_UP; return FALSE;
      }
    }
    Value = Pil->Io.Read32(Pil->Io.Context, Base + Pil->QmpOffset);
    if (Value != 0) {
      *Offset = Pil->QmpOffset; *Observed = Value; *Expected = 0; return FALSE;
    }
    Pil->Io.Fence(Pil->Io.Context);
  }
  return TRUE;
}

STATIC CR_STATUS QmpEstablishedRetire(CR_PIL_CONTEXT *Pil)
{
  UINT32 Offset = 0, Observed = 0, Expected = 0;
  for (UINT32 Count = 0; Count < 1000; ++Count) {
    if (QmpEstablishedSnapshot(Pil, &Offset, &Observed, &Expected)) return CR_SUCCESS;
    Pil->Io.DelayUs(Pil->Io.Context, 1000);
  }
  log_err("PILCr: QMP producer retirement timeout offset=0x%x observed=0x%x expected=0x%x; shared transport retained\n",
          Offset, Observed, Expected);
  return CR_TIMEOUT;
}

STATIC CR_STATUS QmpClose(CR_PIL_CONTEXT *Pil)
{
  CR_STATUS Status;
  if (!Pil->QmpOpen) return CR_SUCCESS;
  /* Linux/Android PAS consumers release their load-state request, not the
     shared AOSS provider. Keep established transport UP after real message
     consumption and two coherent state/ACK snapshots. Dropping the link and
     channel together and requiring DOWN ACKs invents a PAS close contract. */
  Status = QmpWait(Pil, Pil->QmpOffset, 0);
  if (CR_ERROR(Status)) return Status;
  Pil->QmpClosing = TRUE;
  if (Pil->QmpEstablished || !Pil->QmpChanged) {
    Status = QmpEstablishedRetire(Pil);
    if (CR_ERROR(Status)) return Status;
  } else {
    /* Failed setup never established the consumer transport. Preserve the
       existing partial-effect rollback and demand actual DOWN ACKs; an AOP
       that does not acknowledge this remains uncertain/resident. This path
       must never manufacture a consumed request or a completed handshake. */
    Pil->Io.Write32(Pil->Io.Context, Pil->Target->QmpBase + 0x2c, QMP_DOWN);
    Pil->Io.Write32(Pil->Io.Context, Pil->Target->QmpBase + 0x24, QMP_DOWN);
    Kick(Pil, 0, 0);
    Status = QmpWait(Pil, 0x30, QMP_DOWN);
    if (CR_ERROR(Status)) return Status;
    Status = QmpWait(Pil, 0x28, QMP_DOWN);
    if (CR_ERROR(Status)) return Status;
  }
  Pil->QmpOpen = Pil->QmpChanged = Pil->QmpClosing = Pil->QmpEstablished = FALSE;
  return CR_SUCCESS;
}

STATIC CR_STATUS QmpOpen(CR_PIL_CONTEXT *Pil)
{
  UINT32 Base = Pil->Target->QmpBase, Size, Offset;
  CR_STATUS Status;
  if (Pil->QmpOpen) return Pil->QmpClosing ? CR_NOT_READY : CR_SUCCESS;
  if (Pil->Io.Read32(Pil->Io.Context, Base) != 0x4d41494cU || Pil->Io.Read32(Pil->Io.Context, Base + 4) != 1)
    return CR_PIL_INCOMPATIBLE_VERSION;
  Size = Pil->Io.Read32(Pil->Io.Context, Base + 0x34); Offset = Pil->Io.Read32(Pil->Io.Context, Base + 0x38);
  if ((Offset & 3U) != 0 || Offset < 0x3c || Size < 68 ||
      Offset > Pil->Target->QmpSize || Size > Pil->Target->QmpSize - Offset)
    return CR_COMPROMISED_DATA;
  Pil->QmpOffset = Offset;
  Pil->QmpChanged = Pil->Io.Read32(Pil->Io.Context, Base + 0x24) != QMP_UP ||
                    Pil->Io.Read32(Pil->Io.Context, Base + 0x2c) != QMP_UP;
  Pil->QmpOpen = TRUE;
  Pil->Io.Write32(Pil->Io.Context, Base + 0x10, Pil->Io.Read32(Pil->Io.Context, Base + 0x0c));
  Pil->Io.Write32(Pil->Io.Context, Base + 0x24, QMP_UP); Kick(Pil, 0, 0);
  Status = QmpWait(Pil, 0x28, QMP_UP);
  if (CR_ERROR(Status)) goto Fail;
  Pil->Io.Write32(Pil->Io.Context, Base + 0x2c, QMP_UP); Kick(Pil, 0, 0);
  Status = QmpWait(Pil, 0x14, QMP_UP);
  if (CR_ERROR(Status)) goto Fail;
  Pil->Io.Write32(Pil->Io.Context, Base + 0x18, QMP_UP); Kick(Pil, 0, 0);
  Status = QmpWait(Pil, 0x30, QMP_UP);
  if (!CR_ERROR(Status)) { Pil->QmpEstablished = TRUE; return Status; }
Fail:
  (VOID)QmpClose(Pil);
  return Status;
}

STATIC CR_STATUS LoadState(CR_PIL_CONTEXT *Pil, BOOLEAN On)
{
  CONST CHAR8 *Message = On ?
    "{class: image, res: load_state, name: adsp, val: on}" :
    "{class: image, res: load_state, name: adsp, val: off}";
  UINT32 Words[16];
  UINTN Index, Length = On ? sizeof("{class: image, res: load_state, name: adsp, val: on}") - 1 :
                               sizeof("{class: image, res: load_state, name: adsp, val: off}") - 1;
  CR_STATUS Status;
  Status = QmpOpen(Pil);
  if (CR_ERROR(Status)) return Status;
  Status = QmpWait(Pil, Pil->QmpOffset, 0);
  if (CR_ERROR(Status)) return Status;
  cr_memset(Words, 0, sizeof(Words)); cr_memcpy(Words, Message, Length);
  /* AOSS message RAM requires aligned 32-bit accesses, including padding. */
  for (Index = 0; Index < 16; ++Index)
    Pil->Io.Write32(Pil->Io.Context, Pil->Target->QmpBase + Pil->QmpOffset + 4 + Index * 4, Words[Index]);
  if (On) Pil->LoadState = TRUE; // A timed-out request may already be applied.
  Pil->Io.Write32(Pil->Io.Context, Pil->Target->QmpBase + Pil->QmpOffset, sizeof(Words));
  (VOID)Pil->Io.Read32(Pil->Io.Context, Pil->Target->QmpBase + Pil->QmpOffset);
  Kick(Pil, 0, 0);
  Status = QmpWait(Pil, Pil->QmpOffset, 0);
  if (!CR_ERROR(Status)) Pil->LoadState = On;
  return Status;
}

STATIC CR_STATUS RemoteState(CR_PIL_CONTEXT *Pil, UINT32 *State)
{
  VOID *Address;
  UINT32 Size, Word, Count, Index;
  volatile UINT32 *In;
  CHAR8 Name[16];
  CR_STATUS Status;
  *State = 0;
  Status = Pil->Io.SmemGet(Pil->Io.Context, Pil->Target->SmemHost,
                              Pil->Target->SmemIn, &Size, &Address);
  if (Status == CR_NOT_FOUND || Status == CR_NOT_READY) return CR_NOT_READY;
  if (CR_ERROR(Status)) return Status;
  if (Address == NULL || ((UINTN)Address & 3U) != 0 || Size < 20)
    return CR_COMPROMISED_DATA;
  In = Address; Pil->Io.Fence(Pil->Io.Context);
  Word = In[1];
  if ((Word & 255U) == 0) return CR_NOT_READY;
  if (In[0] != SMP_MAGIC || (Word & 255U) > 2 || In[2] != Pil->Target->SmemHost)
    return CR_COMPROMISED_DATA;
  Count = In[3] >> 16;
  if ((In[3] & 0xffffU) > 16 || Count > (In[3] & 0xffffU) ||
      Size < 20 + Count * 20) return CR_COMPROMISED_DATA;
  if (Pil->SmpOut != NULL) {
    /* Negotiate version/features; acknowledge restart completion if enabled. */
    if (Pil->SmpOut[1] != (Word & 0x1ffU)) {
      Pil->SmpOut[1] = Word & 0x1ffU;
      Kick(Pil, Pil->Target->IpccClient, 2);
    }
    if ((Word & (1U << 8)) != 0) {
      UINT32 Flags = Pil->SmpOut[4];
      UINT32 Ack = (In[4] & (1U << 0)) != 0 ? (1U << 1) : 0;
      if ((Flags & (1U << 1)) != Ack) {
        Pil->SmpOut[4] = (Flags & ~(1U << 1)) | Ack;
        Kick(Pil, Pil->Target->IpccClient, 2);
      }
    }
  }
  for (Index = 0; Index < Count; ++Index) {
    cr_memcpy(Name, (CONST VOID *)((UINTN)Address + 20 + Index * 20), sizeof(Name));
    if (cr_memcmp(Name, "slave-kernel\0\0\0\0", sizeof(Name)) == 0) {
      *State = In[9 + Index * 5];
      Pil->Io.Fence(Pil->Io.Context); return CR_SUCCESS;
    }
  }
  return CR_NOT_READY;
}

STATIC CR_STATUS SmpPrepare(CR_PIL_CONTEXT *Pil)
{
  CR_STATUS Status;
  VOID *Address;
  UINT32 Size, State;
  volatile UINT32 *Out;
  Status = RemoteState(Pil, &State);
  if (!CR_ERROR(Status) && (State & SMP_FATAL) != 0) return CR_DEVICE_ERROR;
  if (!CR_ERROR(Status) && (State & SMP_READY) != 0) return CR_ALREADY_STARTED;
  if (CR_ERROR(Status) && Status != CR_NOT_READY) return Status;
  Status = Pil->Io.SmemAllocate(Pil->Io.Context, Pil->Target->SmemHost,
                                  Pil->Target->SmemOut, SMP_SIZE, &Address);
  if (CR_ERROR(Status) && Status != CR_ALREADY_STARTED) return Status;
  Status = Pil->Io.SmemGet(Pil->Io.Context, Pil->Target->SmemHost,
                              Pil->Target->SmemOut, &Size, &Address);
  if (CR_ERROR(Status)) return Status;
  if (Address == NULL || Size < SMP_SIZE || ((UINTN)Address & 3U) != 0)
    return CR_COMPROMISED_DATA;
  Out = Address;
  /* No live ADSP readiness was observed. Establish the APSS-owned item,
     publishing its version only after the header and stop entry are valid. */
  cr_memset(Address, 0, SMP_SIZE);
  Out[0] = SMP_MAGIC; Out[2] = Pil->Target->SmemHost << 16;
  Out[3] = 16 | (1U << 16);
  cr_memcpy((VOID *)((UINTN)Address + 20), "master-kernel", 13);
  Pil->Io.Fence(Pil->Io.Context); Out[1] = 2 | (1U << 8);
  Pil->SmpOut = Out;
  Kick(Pil, Pil->Target->IpccClient, 2);
  return CR_SUCCESS;
}

STATIC CR_STATUS DescribeVotes(CR_PIL_CONTEXT *Pil)
{
  CONST CHAR8 *Names[3] = {"lmx.lvl", "lcx.lvl", "xo.lvl"};
  UINT8 Aux[64];
  UINT32 Index, Length, Corner, Level, Previous;
  CR_STATUS Status;
  for (Index = 0; Index < 3; ++Index) {
    Status = Pil->Io.CmdAddress(Pil->Io.Context, Names[Index],
                                               &Pil->VoteAddress[Index]);
    if (CR_ERROR(Status)) return Status;
    if (Pil->VoteAddress[Index] == 0) return CR_COMPROMISED_DATA;
    if (Index == 2) { Pil->VoteValue[Index] = 3; continue; }
    Length = sizeof(Aux);
    Status = Pil->Io.CmdAux(Pil->Io.Context, Names[Index], Aux, &Length);
    if (CR_ERROR(Status)) return Status;
    if (Length < 4 || Length > sizeof(Aux) || (Length & 1U) != 0 ||
        Aux[0] != 0 || Aux[1] != 0) return CR_COMPROMISED_DATA;
    Previous = 0; Pil->VoteValue[Index] = 0;
    for (Corner = 1; Corner < Length / 2; ++Corner) {
      Level = Aux[Corner * 2] | ((UINT32)Aux[Corner * 2 + 1] << 8);
      if (Level == 0) break; // CmdDB ARC level tables can be zero padded.
      if (Level < Previous) return CR_COMPROMISED_DATA;
      Previous = Level; Pil->VoteValue[Index] = Corner;
    }
    if (Pil->VoteValue[Index] == 0) return CR_COMPROMISED_DATA;
    log_info("PILCr: " CR_LOG_CHAR8_STR_FMT " addr=0x%x proxy corner=%u level=%u\n",
            Names[Index], Pil->VoteAddress[Index], Pil->VoteValue[Index], Previous);
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS Vote(CR_PIL_CONTEXT *Pil, UINT32 Index, UINT32 Value)
{
  return Pil->Io.ProxyWrite(Pil->Io.Context, Pil->VoteAddress[Index], Value);
}

CR_STATUS CrPilCoreInitialize(CR_PIL_CONTEXT *Pil, CONST CR_PIL_TARGET *Target,
                              CONST CR_PIL_IO *Io)
{
  if (Pil == NULL || Target == NULL || Io == NULL || Io->Smc == NULL ||
      Io->Read32 == NULL || Io->Write32 == NULL || Io->Fence == NULL ||
      Io->DelayUs == NULL || Io->SmemGet == NULL || Io->SmemAllocate == NULL ||
      Io->CmdAddress == NULL || Io->CmdAux == NULL || Io->ProxyWrite == NULL ||
      Io->CryptoAcquire == NULL || Io->CryptoSet == NULL || Io->CryptoRelease == NULL ||
      Target->InterconnectProvider == NULL || Target->PasId != 1 || Target->SmemHost != 2 ||
      Target->QmpBase == 0 || (Target->QmpBase & 3U) != 0 || Target->QmpSize < 0x80 ||
      (UINT64)Target->QmpBase + Target->QmpSize > 0x100000000ULL ||
      Target->IpccSend == 0 || (Target->IpccSend & 3U) != 0)
    return CR_INVALID_PARAMETER;
  cr_memset(Pil, 0, sizeof(*Pil));
  Pil->Target = Target; Pil->Io = *Io; Pil->Stage = "idle";
  Pil->Shutdown.CallStatus = CR_NOT_READY;
  return CR_SUCCESS;
}

CR_STATUS CrPilStartPhysical(CR_PIL_CONTEXT *Pil, CONST VOID *Header, UINTN Size,
                      CONST CR_PIL_IMAGE *Image, VOID *Metadata,
                      UINT64 MetadataPhysical, VOID *Carveout, CR_PIL_READ_SEGMENT Read, VOID *FileContext)
{
  CR_STATUS Status;
  UINT32 Index, State;
  UINTN Result = 0;
  CR_PIL_IMAGE Checked;
  if (Pil == NULL || Pil->Target == NULL || Metadata == NULL || Carveout == NULL ||
      Image == NULL || ((UINTN)Metadata & 4095U) != 0 ||
      (MetadataPhysical & 4095U) != 0 || MetadataPhysical > CR_MAX_UINT32)
    return CR_INVALID_PARAMETER;
  if (Pil->PasOwned || Pil->Votes != 0 || Pil->LoadState || Pil->CryptoPath != 0 ||
      Pil->SmcInterrupted || Pil->QmpOpen || Pil->QmpClosing)
    return CR_ALREADY_STARTED;
  Pil->Stage = "ELF validation";
  Status = CrPilInspect(Header, Size, Pil->Target, &Checked);
  if (CR_ERROR(Status)) return Status;
  if (cr_memcmp(Image, &Checked, sizeof(Checked)) != 0) return CR_COMPROMISED_DATA;
  if (MetadataPhysical + Image->HeaderSize + Image->HashSize > 0x100000000ULL)
    return CR_INVALID_PARAMETER;
  if (Pil->StartGeneration == CR_MAX_UINT64) return CR_OUT_OF_RESOURCES;
  ++Pil->StartGeneration;
  cr_memset(&Pil->Shutdown, 0, sizeof(Pil->Shutdown));
  Pil->NativeShutdownGeneration = 0;
  Pil->NativeShutdownEl = 0;
  Pil->Shutdown.Generation = Pil->StartGeneration;
  Pil->Shutdown.CallStatus = CR_NOT_READY;
  Pil->Stage = "SMP2P preparation";
  Status = SmpPrepare(Pil); if (CR_ERROR(Status)) return Status;
  Pil->Stage = "SCM convention";
  Pil->SmcBase = SCM_SIP | SCM_SMC64;
  Status = Smc(Pil, Pil->SmcBase | SCM_FAST, SCM_INFO_AVAILABLE, 1,
                SCM_SIP | SCM_INFO_AVAILABLE, 0, 0, &Result);
  if (CR_ERROR(Status) || Result != 1) {
    Status = Smc(Pil, SCM_SIP | SCM_FAST, SCM_INFO_AVAILABLE, 1,
                  SCM_SIP | SCM_INFO_AVAILABLE, 0, 0, &Result);
    if (CR_ERROR(Status) || Result != 1) return CR_UNSUPPORTED;
    Pil->SmcBase = SCM_SIP;
  }
  log_info("PILCr: SCM convention=SMC%u standard PAS calls\n",
          (Pil->SmcBase & SCM_SMC64) != 0 ? 64 : 32);
  Pil->Stage = "PAS support diagnostic";
  PasSupportDiagnostic(Pil);
  if (Pil->SmcInterrupted) return CR_NOT_READY;
  Pil->Stage = "crypto DDR path";
  Status = Pil->Io.CryptoAcquire(Pil->Io.Context,
             Pil->Target->InterconnectProvider, Pil->Target->CryptoSource,
             Pil->Target->CryptoDestination, &Pil->CryptoPath);
  if (CR_ERROR(Status)) return Status;
  Status = Pil->Io.CryptoSet(Pil->Io.Context, Pil->CryptoPath,
                                          0, CR_MAX_UINT32);
  if (CR_ERROR(Status)) return Status;
  Pil->Stage = "CmdDB proxy votes";
  Status = DescribeVotes(Pil); if (CR_ERROR(Status)) return Status;
  Pil->Stage = "AOSS load_state on";
  Status = LoadState(Pil, TRUE); if (CR_ERROR(Status)) return Status;
  Pil->Stage = "LCX/LMX/XO proxy votes";
  for (Index = 0; Index < 3; ++Index) {
    Pil->Votes |= 1U << Index; // Retain possibly applied votes on timeout.
    Status = Vote(Pil, Index, Pil->VoteValue[Index]);
    if (CR_ERROR(Status)) return Status;
  }
  Pil->Stage = "PAS init_image";
  Pil->Io.Fence(Pil->Io.Context);
  Pil->PasOwned = TRUE; // A transport failure can leave secure state allocated.
  Status = Pas(Pil, 1, 2 | (2U << 6), (UINTN)MetadataPhysical, 0);
  if (CR_ERROR(Status)) return Status;
  if (Image->Relocatable) {
    Pil->Stage = "PAS mem_setup";
    Status = Pas(Pil, 2, 3, Pil->Target->MemoryBase, Image->Span);
    if (CR_ERROR(Status)) return Status;
  }
  Pil->Stage = "firmware segments";
  for (Index = 0; Index < Image->Count; ++Index) {
    CONST CR_PIL_SEGMENT *Segment = &Image->Segments[Index];
    UINT8 *Destination = (UINT8 *)Carveout + Segment->Destination;
    if (!Segment->Load) continue;
    if (Segment->FileSize != 0) {
      if (Image->Split) {
        if (Read == NULL) return CR_NOT_FOUND;
        Status = Read(FileContext, Index, Destination, Segment->FileSize);
        if (CR_ERROR(Status)) return Status;
      } else {
        cr_memcpy(Destination, (CONST UINT8 *)Header + Segment->Offset, Segment->FileSize);
      }
    }
    cr_memset(Destination + Segment->FileSize, 0, Segment->MemorySize - Segment->FileSize);
  }
  Pil->Io.Fence(Pil->Io.Context);
  Pil->Stage = "PAS auth_and_reset";
  Status = Pas(Pil, 5, 1, 0, 0); if (CR_ERROR(Status)) return Status;
  Pil->Running = TRUE;
  Pil->Stage = "ADSP ready";
  for (Index = 0; Index < 5000; ++Index) {
    Status = RemoteState(Pil, &State);
    if (CR_ERROR(Status) && Status != CR_NOT_READY) return Status;
    if (!CR_ERROR(Status) && (State & SMP_FATAL) != 0) return CR_DEVICE_ERROR;
    if (!CR_ERROR(Status) && (State & SMP_READY) != 0) {
      Pil->Stage = "running";
      log_info("PILCr: ADSP PAS1 authenticated and ready; PIL owns shutdown\n");
      return CR_SUCCESS;
    }
    Pil->Io.DelayUs(Pil->Io.Context, 1000);
  }
  return CR_TIMEOUT;
}

STATIC VOID FinishShutdown(CR_PIL_CONTEXT *Pil)
{
  if (Pil->PasOwned && Pil->Shutdown.Generation != 0 &&
      Pil->Shutdown.Generation == Pil->StartGeneration &&
      Pil->Shutdown.Attempted && Pil->Shutdown.Returned &&
      !CR_ERROR(Pil->Shutdown.CallStatus) &&
      Pil->Shutdown.X0 == 0 && Pil->Shutdown.X1 == 0 && !Pil->SmcInterrupted)
    Pil->Shutdown.Complete = TRUE;
  Pil->PasOwned = Pil->Running = FALSE;
  if (Pil->SmpOut != NULL) {
    Pil->SmpOut[9] &= ~(1U << 0);
    Kick(Pil, Pil->Target->IpccClient, 2);
  }
}

CR_STATUS CrPilStop(CR_PIL_CONTEXT *Pil)
{
  CR_STATUS Status;
  UINT32 Index, State;
  BOOLEAN ShutdownDone;
  if (Pil == NULL || Pil->Target == NULL) return CR_INVALID_PARAMETER;
  Status = RetireSmc(Pil, &ShutdownDone);
  if (CR_ERROR(Status)) {
    log_err("PILCr: secure continuation retirement failed " CR_LOG_STATUS_FMT "; retaining ownership and prerequisites\n", Status);
    return Status;
  }
  /* Retired shutdown success already relinquished secure ownership. Firmware
     need not accept a second shutdown for an image that is now off. */
  if (ShutdownDone) FinishShutdown(Pil);
  if (Pil->PasOwned) {
    if (Pil->Running && Pil->SmpOut != NULL) {
      Pil->SmpOut[9] |= (1U << 0);
      Kick(Pil, Pil->Target->IpccClient, 2);
      for (Index = 0; Index < 1000; ++Index) {
        Status = RemoteState(Pil, &State);
        if (!CR_ERROR(Status) && (State & SMP_STOP_ACK) != 0) break;
        Pil->Io.DelayUs(Pil->Io.Context, 1000);
      }
      if (Index == 1000) log_warn("PILCr: no ADSP stop ack; issuing PAS shutdown\n");
    }
    Status = Pas(Pil, 6, 1, 0, 0);
    if (CR_ERROR(Status)) {
      log_err("PILCr: PAS shutdown failed " CR_LOG_STATUS_FMT "; retaining ownership and votes\n", Status);
      return Status;
    }
    FinishShutdown(Pil);
  }
  if (Pil->LoadState) {
    Status = LoadState(Pil, FALSE);
    if (CR_ERROR(Status)) return Status;
  }
  for (Index = 3; Index-- > 0;) {
    if ((Pil->Votes & (1U << Index)) == 0) continue;
    Status = Vote(Pil, Index, 0);
    if (CR_ERROR(Status)) return Status;
    Pil->Votes &= ~(1U << Index);
  }
  Status = QmpClose(Pil);
  if (CR_ERROR(Status)) return Status;
  if (Pil->CryptoPath != 0) {
    Status = Pil->Io.CryptoRelease(Pil->Io.Context, Pil->CryptoPath);
    if (CR_ERROR(Status)) return Status;
    Pil->CryptoPath = 0;
  }
  return CR_SUCCESS;
}

CR_STATUS CrPilGetShutdownEvidence(CONST CR_PIL_CONTEXT *Pil,
                                   CR_PIL_SHUTDOWN_EVIDENCE *Evidence)
{
  if (Pil == NULL || Pil->Target == NULL || Evidence == NULL)
    return CR_INVALID_PARAMETER;
  *Evidence = Pil->Shutdown;
  return CR_SUCCESS;
}

CR_STATUS CrPilStart(CR_PIL_CONTEXT *Pil, CONST VOID *Header, UINTN Size,
                      CONST CR_PIL_IMAGE *Image, VOID *Metadata,
                      VOID *Carveout, CR_PIL_READ_SEGMENT Read, VOID *FileContext)
{
  return CrPilStartPhysical(Pil, Header, Size, Image, Metadata,
                            (UINT64)(UINTN)Metadata, Carveout, Read, FileContext);
}
CR_STATUS CrPilCheckReady(CR_PIL_CONTEXT *Pil)
{
  UINT32 State;
  if (Pil == NULL || Pil->Target == NULL) return CR_INVALID_PARAMETER;
  if (Pil->SmcInterrupted) return CR_NOT_READY;
  CR_STATUS Status = RemoteState(Pil, &State);
  if (CR_ERROR(Status)) return Status;
  if ((State & SMP_FATAL) != 0) return CR_DEVICE_ERROR;
  return (State & SMP_READY) != 0 ? CR_SUCCESS : CR_NOT_READY;
}
