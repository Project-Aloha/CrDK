/** @file
 *  SMMUv2/MMU-500 domains preserving firmware-owned streams and banks.
 *  SPDX-License-Identifier: MIT
 */
#include <Library/smmu.h>
#include <oskal/cr_debug.h>

#define SMR(n)      (0x800U + 4U * (n))
#define S2CR(n)     (0xC00U + 4U * (n))
#define VALID      0x80000000U
#define TRANS_MASK 0x00030000U
#define S2CR_PRIVCFG_SHIFT 24
#define S2CR_PRIVCFG_DEFAULT 0U
#define SCR0_VMID16EN  (1U << 31)
#define SCR0_BSU_MASK  (3U << 14)
#define SCR0_FB        (1U << 13)
#define SCR0_PTM       (1U << 12)
#define SCR0_VMIDPNE   (1U << 11)
#define SCR0_USFCFG    (1U << 10)
#define SCR0_GCFGFIE   (1U << 5)
#define SCR0_GCFGFRE   (1U << 4)
#define SCR0_EXIDENABLE (1U << 3)
#define SCR0_GFIE      (1U << 2)
#define SCR0_GFRE      (1U << 1)
#define SCR0_CLIENTPD  (1U << 0)
#define GR0_TLBIALLNSNH 0x68U
#define GR0_TLBGSYNC    0x70U
#define GR0_TLBGSTATUS  0x74U
#define GR0_GFSR        0x48U
#define CB_FSR          0x58U
#define TLBGSTATUS_ACTIVE 1U
#define PAGE_DESC  0x0060000000000F43ULL /* nG, UXN, PXN, AF, ISH, AP=RW EL1/EL0 */
/* Android io-pgtable-arm.c arm_lpae_prot_to_pte() uses SH_IS for IOMMU_CACHE.
 * Our existing PAGE_DESC already has that shareability. Each context uses
 * its own MAIR slot 0 (rather than Android's cache slot 1): the selected byte
 * determines whether this normal-memory mapping is NC or WB Read/Write-Allocate.
 */
STATIC UINT32 DomainMair0(CONST SmmuDomain *Domain) {
  return Domain->CacheCoherent ? 0xFFU : 0x44U;
}

/* ARM SMMUv2 TCR2 fields defined by the public arm-smmu driver. Qualcomm
 * implementations may return implementation/reserved bits (SM8450 returns
 * 0x60) in the same register; those bits are not software-owned. */
#define TCR2_PASIZE_MASK 0x0000000FU
#define TCR2_AS          0x00000010U
#define TCR2_SEP_MASK    0x00038000U
#define TCR2_CONFIG_MASK (TCR2_PASIZE_MASK | TCR2_AS | TCR2_SEP_MASK)

STATIC UINT32 Read(SmmuDevice *D, UINT64 Address) {
  return D->Io.Read32(D->Io.Cookie, Address);
}

STATIC VOID Write(SmmuDevice *D, UINT64 Address, UINT32 Value) {
  D->Io.Write32(D->Io.Cookie, Address, Value);
}

/* A global invalidation is safe only on the cold path, where every stream
 * match and context bank has already been proved unused.  Keep it separate
 * from the per-domain TLBI so an adopted firmware controller is never flushed
 * underneath another live client. */
STATIC CR_STATUS GlobalSync(SmmuDevice *D) {
  UINT32 Wait;

  Write(D, D->Base + GR0_TLBIALLNSNH, ~0U);
  Write(D, D->Base + GR0_TLBGSYNC, ~0U);
  for (Wait = 0; Wait < 10000; Wait++) {
    if ((Read(D, D->Base + GR0_TLBGSTATUS) & TLBGSTATUS_ACTIVE) == 0) {
      return CR_SUCCESS;
    }
    D->Io.DelayUs(D->Io.Cookie, 1);
  }
  D->SyncFailed = TRUE;
  log_err("SMMU cold global TLB sync timed out waited-us=%u", Wait);
  return CR_TIMEOUT;
}

STATIC VOID Zero(UINT64 *P, UINTN Count) {
  while (Count-- != 0) {
    *P++ = 0;
  }
}

STATIC UINT64 BankBase(SmmuDevice *D, UINT16 Bank) {
  return D->ContextBase + (UINT64)Bank * D->PageSize;
}

/* Keep address-aperture checks defined when a caller represents a full
 * 64-bit output address space.  Shifting a 64-bit literal by 64 is undefined
 * in C, and the external-IOMMU adapter deliberately uses AddressBits=64. */
STATIC BOOLEAN AddressRangeFits(UINT8 Bits, UINT64 Address, UINTN Pages) {
  UINT64 Bytes;
  UINT64 Limit;

  if (Pages == 0 || Pages > CR_MAX_UINT64 / SMMU_PAGE_SIZE) return FALSE;
  Bytes = (UINT64)Pages * SMMU_PAGE_SIZE;
  if (Bits >= 64) {
    return Address <= CR_MAX_UINT64 && (Bytes - 1U) <= CR_MAX_UINT64 - Address;
  }
  if (Bits == 0) return FALSE;
  Limit = (1ULL << Bits) - 1ULL;
  return Address <= Limit && (Bytes - 1U) <= Limit - Address;
}

STATIC BOOLEAN ShortDescriptor(UINT32 Cbar, UINT32 Cba2r, UINT32 Tcr) {
  return (Cbar & TRANS_MASK) != 0 && (Cba2r & 1U) == 0 &&
         (Tcr & (1U << 31)) == 0;
}

STATIC BOOLEAN BankUsesAsid(SmmuDevice *D, UINT16 Bank, UINT16 Asid) {
  UINT64 Cb = BankBase(D, Bank);
  UINT32 Cbar = Read(D, D->Base + D->PageSize + Bank * 4U);
  UINT32 Cba2r = Read(D, D->Base + D->PageSize + 0x800U + Bank * 4U);
  UINT32 Tcr = Read(D, Cb + 0x30);
  if ((Cbar & TRANS_MASK) == 0) return FALSE; /* Stage 2 uses a VMID. */
  if (ShortDescriptor(Cbar, Cba2r, Tcr))
    return (Read(D, Cb + 0x34) & 0xFFU) == Asid;
  return (Read(D, Cb + 0x24) >> 16) == Asid ||
         (Read(D, Cb + 0x2C) >> 16) == Asid;
}

STATIC BOOLEAN ContextBankInUse(SmmuDevice *D, UINT16 Bank) {
  UINT64 Cb = BankBase(D, Bank);

  /*
   * Linux arm_smmu_write_context_bank() disables unassigned banks by clearing
   * SCTLR, leaving TTBR/TCR/MAIR and CBAR/CBA2R intact. Those registers cannot
   * establish ownership. SmmuAttach separately protects every bank referenced
   * by a valid translating stream, even when its SCTLR.M is clear.
   */
  return (Read(D, Cb) & 1U) != 0;
}

/* Public Linux arm-smmu.h fault/status registers. These reads do not clear
 * faults: never acknowledge W1C state or submit maintenance for diagnostics. */
STATIC VOID LogFaultState(SmmuDomain *Domain) {
  SmmuDevice *D = Domain->Device;
  UINT64 Cb = Domain->BankBase;
  UINT32 Fsr = Read(D, Cb + 0x58);
  UINT32 FarLo = Read(D, Cb + 0x60), FarHi = Read(D, Cb + 0x64);
  UINT32 Syn0 = Read(D, Cb + 0x68), Syn1 = Read(D, Cb + 0x6C);
  log_err("SMMU snapshot SID=0x%x CB=%u SCTLR=0x%08x FSR=0x%08x",
          Domain->Sid, Domain->Bank, Read(D, Cb), Fsr);
  log_err("SMMU snapshot FAR=0x%08x%08x FSYNR0=0x%08x FSYNR1=0x%08x",
          FarHi, FarLo, Syn0, Syn1);
  log_err("SMMU snapshot CB=%u TLBSTATUS=0x%08x base=0x%llx",
          Domain->Bank, Read(D, Cb + 0x7F4), D->Base);
  log_err("SMMU snapshot SCR0=0x%08x GFSR=0x%08x",
          Read(D, D->Base), Read(D, D->Base + 0x48));
  log_err("SMMU snapshot GFSYNR0=0x%08x GFSYNR1=0x%08x GFSYNR2=0x%08x",
          Read(D, D->Base + 0x50), Read(D, D->Base + 0x54), Read(D, D->Base + 0x58));
}

STATIC BOOLEAN VerifyWrite(SmmuDomain *Domain, UINT64 Address, UINT32 Value) {
  UINT32 Actual;
  Write(Domain->Device, Address, Value);
  Actual = Read(Domain->Device, Address);
  if (Actual == Value) return TRUE;
  log_err("SMMU write rejected SID=0x%x CB=%u addr=0x%llx", Domain->Sid, Domain->Bank, Address);
  log_err("SMMU write expected=0x%08x actual=0x%08x; ownership retained", Value, Actual);
  LogFaultState(Domain);
  Domain->Failed = TRUE;
  return FALSE;
}

STATIC BOOLEAN VerifyWriteMasked(SmmuDomain *Domain, UINT64 Address,
                                 UINT32 Value, UINT32 Mask) {
  UINT32 Actual;
  Write(Domain->Device, Address, Value);
  Actual = Read(Domain->Device, Address);
  if ((Actual & Mask) == (Value & Mask)) {
    if (Actual != Value) {
      log_info("SMMU write retained implementation bits SID=0x%x CB=%u addr=0x%llx",
              Domain->Sid, Domain->Bank, Address);
      log_info("SMMU masked write accepted expected=0x%08x actual=0x%08x mask=0x%08x",
              Value, Actual, Mask);
    }
    return TRUE;
  }
  log_err("SMMU masked write rejected SID=0x%x CB=%u addr=0x%llx",
          Domain->Sid, Domain->Bank, Address);
  log_err("SMMU masked write expected=0x%08x actual=0x%08x mask=0x%08x; ownership retained",
          Value, Actual, Mask);
  LogFaultState(Domain);
  Domain->Failed = TRUE;
  return FALSE;
}

STATIC BOOLEAN VerifyWrite64(SmmuDomain *Domain, UINT64 Address, UINT64 Value) {
  SmmuDevice *D = Domain->Device;
  UINT32 Actual;
  D->Io.Write64(D->Io.Cookie, Address, Value);
  Actual = Read(D, Address);
  if (Actual == (UINT32)Value) {
    Actual = Read(D, Address + 4);
    if (Actual == (UINT32)(Value >> 32)) return TRUE;
    log_err("SMMU write64 rejected SID=0x%x CB=%u addr=0x%llx", Domain->Sid, Domain->Bank, Address);
    log_err("SMMU write64 expected=0x%llx actual-hi=0x%08x; ownership retained", Value, Actual);
  } else {
    log_err("SMMU write64 rejected SID=0x%x CB=%u addr=0x%llx", Domain->Sid, Domain->Bank, Address);
    log_err("SMMU write64 expected=0x%llx actual-lo=0x%08x; ownership retained", Value, Actual);
  }
  LogFaultState(Domain);
  Domain->Failed = TRUE;
  return FALSE;
}

STATIC BOOLEAN ExitRetained(CONST SmmuDomain *Domain) {
  return Domain != NULL && (Domain->ExitState != SmmuDomainExitNormal ||
    (Domain->Device != NULL && Domain->Bank < 256 &&
     Domain->Device->PhysicalExitBanks[Domain->Bank]));
}

STATIC CR_STATUS Sync(SmmuDomain *Domain) {
  SmmuDevice *D = Domain->Device;
  UINT32 Wait;

  if (D->SyncFailed || Domain->Failed) {
    log_err("SMMU sync rejected SID=0x%x CB=%u dev-failed=%u domain-failed=%u; no TLBI",
            Domain->Sid, Domain->Bank, D->SyncFailed, Domain->Failed);
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  Write(D, Domain->BankBase + 0x610, Domain->Asid); /* TLBIASID */
  Write(D, Domain->BankBase + 0x7F0, ~0U);         /* Qualcomm TLBSYNC dummy */
  for (Wait = 0; Wait < 10000; Wait++) {
    if ((Read(D, Domain->BankBase + 0x7F4) & 1U) == 0) {
      return CR_SUCCESS;
    }
    D->Io.DelayUs(D->Io.Cookie, 1);
  }
  D->SyncFailed = TRUE;
  Domain->Failed = TRUE;
  log_err("SMMU sync timeout SID=0x%x CB=%u ASID=%u waited-us=%u",
          Domain->Sid, Domain->Bank, Domain->Asid, Wait);
  log_err("SMMU controller poisoned; ownership retained, further TLB commands suppressed");
  LogFaultState(Domain);
  return CR_TIMEOUT;
}

CR_STATUS SmmuProbe(SmmuDevice *D, UINT64 Base, UINT64 Size,
                    CONST SmmuIoOps *Io) {
  STATIC CONST UINT8 Bits[] = {32, 36, 40, 42, 44, 48};
  UINT32 Id0, Id1, Id2, Shift, Pages, UbsEncoding, IasEncoding,
         OasEncoding, Control;
  UINT16 I;
  UINT32 Actual;

  log_info("SMMU probe begin base=0x%llx size=0x%llx device=%p io=%p", Base, Size, D, Io);
  if (D == NULL || Io == NULL || Io->Read32 == NULL || Io->Write32 == NULL ||
      Io->Write64 == NULL || Io->Publish == NULL || Io->DelayUs == NULL ||
      Base == 0 || (Base & 0xFFF) != 0 || Size < 0x10000 ||
      Base > ~0ULL - Size) {
    log_err("SMMU probe rejected: null/incomplete IO, unaligned/zero base, undersized/overflowing range");
    return CR_INVALID_PARAMETER;
  }
  if (D->ProbeState != SmmuProbeUninitialized) {
    log_err("SMMU reprobe rejected state=%u; controller and ownership retained",
            D->ProbeState);
    return CR_BUSY;
  }
  /* An attempted probe cannot serve as recovery, including after an ignored
   * cold-enable write. Only a fresh device after a real controller reset can
   * discard reservations or timeout state. Attach requires a successful probe. */
  D->ProbeState = SmmuProbeFailed;
  D->ActlrSettings = NULL;
  D->ActlrSettingCount = 0;
  D->SyncFailed = FALSE;
  for (I = 0; I < 256; I++) {
    D->ReservedBanks[I] = FALSE;
    D->PhysicalExitBanks[I] = FALSE;
    D->ReservedAsids[I] = FALSE;
  }
  for (I = 0; I < 128; I++) D->ReservedStreams[I] = FALSE;
  D->Io = *Io;
  Id0 = Read(D, Base + 0x20);
  Id1 = Read(D, Base + 0x24);
  Id2 = Read(D, Base + 0x28);
  /* SMS, stage 1, AArch64 4K. Extended stream IDs are deliberately excluded. */
  Control = Read(D, Base);
  log_info("SMMU probe registers ID0=0x%08x ID1=0x%08x ID2=0x%08x SCR0=0x%08x", Id0, Id1, Id2, Control);
  if ((Id0 & 0x48000000U) != 0x48000000U || (Id2 & 0x1000U) == 0 ||
      (Control & SCR0_EXIDENABLE) != 0) {
    log_err("SMMU probe unsupported: require SMS+S1+AArch64-4K and EXID disabled");
    return CR_UNSUPPORTED;
  }
  Shift = (Id1 & VALID) != 0 ? 16 : 12;
  Pages = 1U << (((Id1 >> 28) & 7U) + 1U);
  D->BankCount = (UINT16)(Id1 & 0xFF);
  D->Stage2BankCount = (UINT16)((Id1 >> 16) & 0xFF);
  /* The reservation array supports 128 groups. Truncating the hardware
   * count would hide live routes from bank/ASID ownership checks. */
  D->StreamCount = (UINT16)(Id0 & 0xFF);
  if (D->StreamCount > 128) {
    log_err("SMMU probe unsupported stream count=%u; no registers changed",
            D->StreamCount);
    return CR_UNSUPPORTED;
  }
  /* ID2 has independent UBS (stage-1 input/IOVA), IAS (stage-1
   * output/IPA), and OAS (table-walker/physical output) fields. */
  UbsEncoding = (Id2 >> 8) & 0xF;
  OasEncoding = (Id2 >> 4) & 0xF;
  IasEncoding = Id2 & 0xF;
  log_info("SMMU probe geometry shift=%u pages=%u CBs=%u S2-CBs=%u",
          Shift, Pages, D->BankCount, D->Stage2BankCount);
  log_info("SMMU probe geometry streams=%u UBS-encoding=%u IAS-encoding=%u OAS-encoding=%u required=0x%llx",
          D->StreamCount, UbsEncoding, IasEncoding, OasEncoding,
          (UINT64)Pages << (Shift + 1));
  if (Shift != 12 || UbsEncoding >= sizeof(Bits) || IasEncoding >= sizeof(Bits) ||
      OasEncoding >= sizeof(Bits) ||
      D->BankCount <= D->Stage2BankCount ||
      D->BankCount > Pages || D->StreamCount == 0 || Bits[UbsEncoding] < 32 ||
      ((UINT64)Pages << (Shift + 1)) > Size) {
    log_err("SMMU probe unsupported geometry: address encoding/bank range/stream count/aperture");
    return CR_UNSUPPORTED;
  }
  D->Base = Base;
  D->PageSize = 1U << Shift;
  D->ContextBase = Base + ((UINT64)Pages << Shift);
  D->InputAddressBits = Bits[UbsEncoding];
  D->InputAddressEncoding = (UINT8)UbsEncoding;
  /* With 4 KiB pages an input wider than 39 bits needs an L0 table. The
   * firmware IOVA window fits in 32 bits; never advertise a fourth level
   * while publishing our fixed L1/L2/L3 hierarchy. UBS is a maximum width. */
  D->TranslationAddressBits = D->InputAddressBits < SMMU_TRANSLATION_BITS_MAX
                               ? D->InputAddressBits
                               : SMMU_TRANSLATION_BITS_MAX;
  D->Stage1OutputAddressBits = Bits[IasEncoding];
  D->Stage1OutputAddressEncoding = (UINT8)IasEncoding;
  D->AddressBits = Bits[OasEncoding];
  D->AddressEncoding = (UINT8)OasEncoding;
  if ((Control & SCR0_CLIENTPD) != 0) {
    /* Only an entirely unused controller can be cold-enabled here. */
    for (I = 0; I < D->StreamCount; I++) {
      if ((Read(D, Base + SMR(I)) & VALID) != 0) {
        log_err("SMMU probe cold-enable blocked: valid stream=%u", I);
        return CR_BUSY;
      }
    }
    for (I = 0; I < D->BankCount; I++) {
      if (ContextBankInUse(D, I)) {
        log_err("SMMU probe cold-enable blocked: enabled CB=%u", I);
        return CR_BUSY;
      }
    }
    /* Follow Linux arm_smmu_device_reset(): invalidate before enabling the
     * client, synchronize the global TLB, report both context/global faults,
     * disable forced/barrier broadcasting, and fault unmatched streams. */
    /* Clear stale W1C fault state before enabling fault reporting.  A failed
     * clear is fatal: exposing a controller with an uncleared inherited fault
     * makes the first real DMA failure indistinguishable from old firmware
     * state. */
    Actual = Read(D, D->Base + GR0_GFSR);
    if (Actual != 0) {
      Write(D, D->Base + GR0_GFSR, Actual);
      if (Read(D, D->Base + GR0_GFSR) != 0) {
        log_err("SMMU probe cold-enable global fault clear rejected value=0x%08x", Actual);
        D->ProbeState = SmmuProbeFailed;
        return CR_DEVICE_ERROR;
      }
    }
    for (I = 0; I < D->BankCount; I++) {
      UINT64 Cb = BankBase(D, I);
      Actual = Read(D, Cb + CB_FSR);
      if (Actual == 0) continue;
      Write(D, Cb + CB_FSR, Actual);
      if (Read(D, Cb + CB_FSR) != 0) {
        log_err("SMMU probe cold-enable CB fault clear rejected CB=%u value=0x%08x", I, Actual);
        D->ProbeState = SmmuProbeFailed;
        return CR_DEVICE_ERROR;
      }
    }
    if (CR_ERROR(GlobalSync(D))) {
      D->ProbeState = SmmuProbeFailed;
      return CR_TIMEOUT;
    }
    Control &= ~(SCR0_CLIENTPD | SCR0_FB | SCR0_BSU_MASK);
    Control |= SCR0_PTM | SCR0_VMIDPNE | SCR0_USFCFG |
               SCR0_GCFGFIE | SCR0_GCFGFRE | SCR0_GFIE | SCR0_GFRE;
    log_info("SMMU probe cold-enable SCR0 expected=0x%08x", Control);
    Write(D, Base, Control);
    Actual = Read(D, Base);
    if (Actual != Control) {
      log_err("SMMU probe SCR0 write rejected address=0x%llx expected=0x%08x actual=0x%08x", Base, Control, Actual);
      return CR_DEVICE_ERROR;
    }
  }
  log_info("SMMU probe complete base=0x%llx CB-base=0x%llx", D->Base, D->ContextBase);
  log_info("SMMU probe page-size=0x%x input-bits=%u output-bits=%u walker-bits=%u",
          D->PageSize, D->InputAddressBits, D->Stage1OutputAddressBits,
          D->AddressBits);
  D->ProbeState = SmmuProbeReady;
  log_info("SMMU probe configured-input-bits=%u table-levels=3",
          D->TranslationAddressBits);
  return CR_SUCCESS;
}

STATIC CR_STATUS Attach(SmmuDevice *D, SmmuDomain *Domain, UINT16 Sid,
                        VOID *Tables, UINT64 TableAddress, BOOLEAN Coherent) {
  BOOLEAN Used[256] = {FALSE};
  UINT16 I, Bank, Stream, Asid;
  UINT32 Smr, S2cr, Mask, Expected, Actual, Actlr = 0;
  UINTN Setting;
  BOOLEAN HasActlr = FALSE;
  UINT64 *T = Tables;
  UINT64 Cb;
  CR_STATUS Status;

  log_info("SMMU attach begin SID=0x%x tables=%p table-PA=0x%llx", Sid, Tables, TableAddress);
  log_info("SMMU attach objects device=%p domain=%p", D, Domain);
  if (D != NULL) {
    D->LastAttachResource = SmmuAttachResourceNone;
  }
  if (D == NULL || Domain == NULL || Tables == NULL ||
      D->ProbeState != SmmuProbeReady || D->StreamCount == 0 ||
      (((UINTN)Tables | TableAddress) & 0xFFF) != 0 || Sid > 0x7FFF ||
      !AddressRangeFits (D->AddressBits, TableAddress, SMMU_TABLE_PAGES)) {
    log_err("SMMU attach rejected SID=0x%x: invalid object/stream count/SID/table alignment or PA range", Sid);
    return CR_INVALID_PARAMETER;
  }
  if (D->SyncFailed) {
    log_err("SMMU attach rejected SID=0x%x: controller sync previously failed", Sid);
    return CR_DEVICE_ERROR;
  }
  if (Domain->Active || Domain->Failed || ExitRetained(Domain)) {
    log_err("SMMU reattach rejected SID=0x%x active=%u failed=%u; ownership retained",
            Domain->Sid, Domain->Active, Domain->Failed);
    return CR_BUSY;
  }
  if (D->ActlrSettingCount != 0 && D->ActlrSettings == NULL) {
    log_err("SMMU attach rejected SID=0x%x: ACTLR table missing count=%llu", Sid, (UINT64)D->ActlrSettingCount);
    return CR_INVALID_PARAMETER;
  }
  /* The only documented SM8450 readback quirk is SCTLR.S1_ASIDPNE. Keep this
   * whitelist here so a target cannot silently waive M or fault-control bits. */
  if ((D->SctlrReadbackIgnoreMask & ~0x1000U) != 0) {
    log_err("SMMU attach rejected SID=0x%x: unsupported SCTLR readback mask=0x%08x",
            Sid, D->SctlrReadbackIgnoreMask);
    return CR_INVALID_PARAMETER;
  }
  for (Setting = 0; Setting < D->ActlrSettingCount; Setting++) {
    CONST SmmuActlrSetting *Entry = &D->ActlrSettings[Setting];
    if (((Sid ^ Entry->Sid) & ~(UINT32)Entry->Mask) != 0) continue;
    if (HasActlr && Actlr != Entry->Value) {
      log_err("SMMU ACTLR conflict SID=0x%x old=0x%08x new=0x%08x entry=%llu",
              Sid, Actlr, Entry->Value, (UINT64)Setting);
      return CR_INVALID_PARAMETER;
    }
    Actlr = Entry->Value;
    HasActlr = TRUE;
  }
  Stream = 0xFFFF;
  /* Do not disturb any valid stream, including a firmware bypass mapping.
   * Board handoff must release its matching entry before attaching here. */
  for (I = 0; I < D->StreamCount; I++) {
    Smr = Read(D, D->Base + SMR(I));
    S2cr = Read(D, D->Base + S2CR(I));
    if ((Smr & VALID) != 0) {
      Mask = (Smr >> 16) & 0x7FFF;
      if (((Sid ^ Smr) & ~Mask & 0xFFFF) == 0) {
        log_err("SMMU attach collision SID=0x%x stream=%u mask=0x%x", Sid, I, Mask);
        log_err("SMMU collision SMR=0x%08x S2CR=0x%08x; firmware route retained", Smr, S2cr);
        return CR_BUSY;
      }
      if ((S2cr & TRANS_MASK) == 0) {
        Used[S2cr & 0xFF] = TRUE;
      }
    } else if (Stream == 0xFFFF && !D->ReservedStreams[I]) {
      Stream = I;
    }
  }
  /* Keep the final context bank available for Qualcomm's bypass quirk. */
  for (Bank = D->Stage2BankCount; Bank + 1U < D->BankCount; Bank++) {
    if (!Used[Bank] && !D->ReservedBanks[Bank] && !ContextBankInUse(D, Bank)) {
      break;
    }
  }
  if (Stream == 0xFFFF) {
    D->LastAttachResource = SmmuAttachResourceStream;
    log_err("SMMU attach exhausted streams SID=0x%x count=%u", Sid, D->StreamCount);
    return CR_OUT_OF_RESOURCES;
  }
  if (Bank + 1U >= D->BankCount) {
    D->LastAttachResource = SmmuAttachResourceBank;
    log_err("SMMU attach exhausted banks SID=0x%x total=%u stage2=%u final-bypass=%u",
            Sid, D->BankCount, D->Stage2BankCount, D->BankCount - 1);
    log_err("SMMU excludes referenced, enabled and software-reserved banks");
    return CR_OUT_OF_RESOURCES;
  }
  /* Avoid ASID aliases with enabled or stream-bound firmware contexts. */
  for (Asid = 1; Asid < 256; Asid++) {
    if (D->ReservedAsids[Asid]) continue;
    for (I = 0; I < D->BankCount; I++) {
      if (I != Bank && (Used[I] || ContextBankInUse(D, I)) &&
          BankUsesAsid(D, I, Asid)) {
        break;
      }
    }
    if (I == D->BankCount) {
      break;
    }
  }
  if (Asid == 256) {
    D->LastAttachResource = SmmuAttachResourceAsid;
    log_err("SMMU attach exhausted ASIDs SID=0x%x candidate-CB=%u", Sid, Bank);
    return CR_OUT_OF_RESOURCES;
  }
  log_info("SMMU attach selected SID=0x%x stream=%u CB=%u ASID=%u", Sid, Stream, Bank, Asid);
  log_info("SMMU attach table-PA=0x%llx ACTLR-owned=%u ACTLR=0x%08x", TableAddress, HasActlr, Actlr);
  Domain->Device = D;
  Domain->Tables = T;
  Domain->TableAddress = TableAddress;
  Domain->Bank = Bank;
  Domain->Stream = Stream;
  Domain->Asid = Asid;
  Domain->Sid = Sid;
  Domain->Active = FALSE;
  Domain->Failed = FALSE;
  Domain->CacheCoherent = Coherent;
  Cb = Domain->BankBase = BankBase(D, Bank);
  Domain->OriginalSmr   = Read(D, D->Base + SMR(Stream));
  Domain->OriginalS2cr  = Read(D, D->Base + S2CR(Stream));
  Domain->OriginalSctlr = Read(D, Cb);
  Domain->OriginalCbar  = Read(D, D->Base + D->PageSize + Bank * 4U);
  Domain->OriginalCba2r = Read(D, D->Base + D->PageSize + 0x800U + Bank * 4U);
  Domain->OriginalTcr2  = Read(D, Cb + 0x10);
  Domain->OriginalTcr   = Read(D, Cb + 0x30);
  Domain->OriginalTtbr0 = Read(D, Cb + 0x20);
  Domain->OriginalTtbr1 = Read(D, Cb + 0x28);
  if (!ShortDescriptor(Domain->OriginalCbar, Domain->OriginalCba2r, Domain->OriginalTcr)) {
    Domain->OriginalTtbr0 |= (UINT64)Read(D, Cb + 0x24) << 32;
    Domain->OriginalTtbr1 |= (UINT64)Read(D, Cb + 0x2C) << 32;
  }
  Domain->OwnsActlr = HasActlr;
  Domain->OriginalActlr = HasActlr ? Read(D, Cb + 4) : 0;
  Domain->OriginalMair0 = Read(D, Cb + 0x38);
  Domain->OriginalMair1 = Read(D, Cb + 0x3C);
  log_info("SMMU attach saved SID=0x%x CB=%u SCTLR=0x%08x", Sid, Bank, Domain->OriginalSctlr);
  log_info("SMMU saved CBAR=0x%08x CBA2R=0x%08x ACTLR=0x%08x",
          Domain->OriginalCbar, Domain->OriginalCba2r, Domain->OriginalActlr);
  log_info("SMMU saved TCR2=0x%08x TCR=0x%08x", Domain->OriginalTcr2, Domain->OriginalTcr);
  log_info("SMMU saved TTBR0=0x%llx TTBR1=0x%llx", Domain->OriginalTtbr0, Domain->OriginalTtbr1);
  D->ReservedBanks[Bank] = TRUE;
  D->ReservedStreams[Stream] = TRUE;
  D->ReservedAsids[Asid] = TRUE;
  Zero(T, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE / sizeof(UINT64));
  T[0] = (TableAddress + SMMU_PAGE_SIZE) | 3;
  for (I = 0; I < 32; I++) {
    T[512 + (SMMU_IOVA_BASE >> 21) + I] =
        (TableAddress + (UINT64)(I + 2) * SMMU_PAGE_SIZE) | 3;
  }
  D->Io.Publish(D->Io.Cookie, T, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE);
  log_info("SMMU program SID=0x%x CB=%u base=0x%llx table-bytes=%u",
          Sid, Bank, Cb, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE);
  /* Public qsmmuv500_init_cb sets only DT-matched ACTLR values and
   * invalidates the context afterward. Our normal attach Sync follows this. */
  if (HasActlr && !VerifyWrite(Domain, Cb + 4, Actlr)) return CR_DEVICE_ERROR;
  /* Verify configuration before enabling the acquired bank. */
  if (!VerifyWrite(Domain, D->Base + D->PageSize + 0x800 + Bank * 4, 1) ||
      !VerifyWrite(Domain, D->Base + D->PageSize + Bank * 4, 0x1F300) ||
      /* Linux arm_smmu_init_context_bank() sets AS for AArch64 so the
       * high eight ASID bits in TTBR0/TTBR1 are architecturally active. */
      !VerifyWriteMasked(Domain, Cb + 0x10,
                         TCR2_AS | (7U << 15) | D->Stage1OutputAddressEncoding,
                         TCR2_CONFIG_MASK) ||
      !VerifyWrite(Domain, Cb + 0x30,
                   (1U << 23) | 0x3500U |
                   ((UINT32)(64U - D->TranslationAddressBits) & 0x3FU)) ||
      !VerifyWrite64(Domain, Cb + 0x20, TableAddress | ((UINT64)Asid << 48)) ||
      !VerifyWrite64(Domain, Cb + 0x28, (UINT64)Asid << 48) ||
      !VerifyWrite(Domain, Cb + 0x38, DomainMair0(Domain)) ||
      !VerifyWrite(Domain, Cb + 0x3C, 0)) return CR_DEVICE_ERROR;
  Write(D, Cb + 0x58, Read(D, Cb + 0x58)); /* clear stale W1C faults */
  /* No stall or IRQ: translation faults terminate the DMA transaction. */
  /* Most SMMUs require exact SCTLR readback. A target may document a narrow
   * implementation quirk, but architectural enable/fault/cache bits are
   * always checked by the complement mask. */
  if (!VerifyWriteMasked(Domain, Cb, 0x1027,
                         ~D->SctlrReadbackIgnoreMask)) return CR_DEVICE_ERROR;
  /* These values have just passed readback; do not add MMIO for success logs. */
  log_info("SMMU verified CB=%u CBAR=0x0001f300 CBA2R=0x00000001 SCTLR=0x%08x mask=0x%08x",
          Bank, 0x1027, ~D->SctlrReadbackIgnoreMask);
  log_info("SMMU verified TCR2=0x%08x mask=0x%08x TCR=0x%08x MAIR0=0x%08x MAIR1=0",
          TCR2_AS | (7U << 15) | D->Stage1OutputAddressEncoding,
          TCR2_CONFIG_MASK,
          (1U << 23) | 0x3500U |
          ((UINT32)(64U - D->TranslationAddressBits) & 0x3FU),
          DomainMair0(Domain));
  log_info("SMMU verified TTBR0=0x%llx TTBR1=0x%llx",
          TableAddress | ((UINT64)Asid << 48), (UINT64)Asid << 48);
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }
  /* Translation to an empty table avoids Qualcomm's ignored FAULT writes.
   * Keep PRIVCFG at Linux's S2CR_PRIVCFG_DEFAULT value (0): the stream's
   * transaction privilege is preserved instead of forcing every request to
   * the unprivileged path. The stage-1 AP=01 descriptors accept RW accesses
   * from either EL1/privileged or EL0/unprivileged transactions. */
  Expected = (S2CR_PRIVCFG_DEFAULT << S2CR_PRIVCFG_SHIFT) | Bank;
  Write(D, D->Base + S2CR(Stream), Expected);
  Actual = Read(D, D->Base + S2CR(Stream));
  if (Actual != Expected) {
    log_err("SMMU attach S2CR rejected SID=0x%x stream=%u addr=0x%llx", Sid, Stream, D->Base + S2CR(Stream));
    log_err("SMMU S2CR expected=0x%08x actual=0x%08x", Expected, Actual);
    LogFaultState(Domain);
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  log_info("SMMU verified S2CR stream=%u CB=%u type=translation privcfg=default value=0x%08x",
          Stream, Bank, Actual);
  Write(D, D->Base + SMR(Stream), VALID | Sid);
  Actual = Read(D, D->Base + SMR(Stream));
  if (Actual != (VALID | Sid)) {
    log_err("SMMU attach SMR rejected SID=0x%x stream=%u addr=0x%llx", Sid, Stream, D->Base + SMR(Stream));
    log_err("SMMU SMR expected=0x%08x actual=0x%08x", VALID | Sid, Actual);
    LogFaultState(Domain);
    Domain->Failed = TRUE;
    return CR_DEVICE_ERROR;
  }
  Domain->Active = TRUE;
  log_info("SMMU attach complete SID=0x%x stream=%u CB=%u ASID=%u", Sid, Stream, Bank, Asid);
  return CR_SUCCESS;
}

CR_STATUS SmmuAttach(SmmuDevice *D, SmmuDomain *Domain, UINT16 Sid,
                     VOID *Tables, UINT64 TableAddress) {
  return Attach(D, Domain, Sid, Tables, TableAddress, FALSE);
}

CR_STATUS SmmuAttachCoherent(SmmuDevice *D, SmmuDomain *Domain, UINT16 Sid,
                             VOID *Tables, UINT64 TableAddress) {
  return Attach(D, Domain, Sid, Tables, TableAddress, TRUE);
}

CR_STATUS SmmuSetMapping(SmmuDomain *Domain, UINT64 Iova, UINT64 Physical,
                         UINTN Pages, UINT32 Access) {
  UINT64 *Leaf, Desc;
  UINTN I;
  SmmuDevice *D;
  CR_STATUS Status;

  if (Domain == NULL || Domain->Device == NULL || !Domain->Active ||
      ExitRetained(Domain) || Domain->Failed || Domain->Device->SyncFailed) {
    log_err("SMMU map rejected: null/inactive/failed domain or controller; no mapping change");
    return CR_DEVICE_ERROR;
  }
  D = Domain->Device;
  if (Pages == 0 || Pages > SMMU_IOVA_SIZE / SMMU_PAGE_SIZE ||
      (Access & ~3U) != 0 || ((Iova | Physical) & 0xFFF) != 0 ||
      Iova < SMMU_IOVA_BASE || Iova - SMMU_IOVA_BASE >= SMMU_IOVA_SIZE ||
      Pages > (SMMU_IOVA_SIZE - (Iova - SMMU_IOVA_BASE)) / SMMU_PAGE_SIZE ||
      (Access != 0 &&
       !AddressRangeFits (D->Stage1OutputAddressBits, Physical, Pages))) {
    log_err("SMMU map rejected SID=0x%x CB=%u: pages/access/alignment/IOVA/PA bounds", Domain->Sid, Domain->Bank);
    return CR_INVALID_PARAMETER;
  }
  /* A coherent grant must not silently degrade into non-cacheable DMA after
   * a foreign register change. Reject before changing/publishing any leaf.
   * Revocation remains available so an owner can still block a drifted bank.
   */
  if (Access != 0 && Domain->CacheCoherent &&
      Read(D, Domain->BankBase + 0x38) != DomainMair0(Domain))
    return CR_DEVICE_ERROR;
  Leaf = Domain->Tables + 1024 + (Iova - SMMU_IOVA_BASE) / SMMU_PAGE_SIZE;
  /* Break before make, including permission changes. */
  Zero (Leaf, Pages);
  D->Io.Publish(D->Io.Cookie, Leaf, Pages * sizeof(*Leaf));
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    log_err("SMMU map break failed SID=0x%x CB=%u status=0x%llx IOVA=0x%llx pages=%llu",
            Domain->Sid, Domain->Bank, (UINT64)Status, Iova, (UINT64)Pages);
    log_err("SMMU map returns timeout; memory must remain pinned");
    return CR_TIMEOUT;
  }
  if (Access != 0) {
    Desc = PAGE_DESC | ((Access & SMMU_ACCESS_WRITE) == 0 ? 0x80 : 0);
    for (I = 0; I < Pages; I++) {
      Leaf[I] = (Physical + I * SMMU_PAGE_SIZE) | Desc;
    }
    D->Io.Publish(D->Io.Cookie, Leaf, Pages * sizeof(*Leaf));
    Status = Sync(Domain);
    if (CR_ERROR(Status))
      log_err("SMMU map make failed SID=0x%x CB=%u status=0x%llx IOVA=0x%llx PA=0x%llx",
              Domain->Sid, Domain->Bank, (UINT64)Status, Iova, Physical);
    return Status;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS QueryMappingEvidence(SmmuDomain *Domain, UINT64 Iova,
    UINT64 Physical, UINTN Pages, UINT32 Access, SmmuMappingEvidence *Evidence) {
  SmmuMappingEvidence E = {0};
  SmmuDevice *D;
  UINTN I, First;
  UINT64 Desc;
  UINT32 ExpectedActlr = 0;
  BOOLEAN HasActlr = FALSE;
  if (Evidence == NULL) return CR_INVALID_PARAMETER;
  *Evidence = E;
  if (Domain == NULL || Domain->Device == NULL || !Domain->Active ||
      ExitRetained(Domain) || Domain->Failed || Domain->Device->SyncFailed) return CR_NOT_READY;
  D = Domain->Device;
  if (D->ProbeState != SmmuProbeReady || D->Io.Read32 == NULL ||
      Domain->Tables == NULL || Domain->Bank >= D->BankCount ||
      Domain->Bank >= 256 || Domain->Stream >= D->StreamCount ||
      Domain->Stream >= 128 || Domain->Asid >= 256 ||
      !D->ReservedBanks[Domain->Bank] || !D->ReservedStreams[Domain->Stream] ||
      !D->ReservedAsids[Domain->Asid] ||
      Domain->BankBase != BankBase(D, Domain->Bank) ||
      (D->SctlrReadbackIgnoreMask & ~0x1000U) != 0)
    return CR_NOT_READY;
  if (Pages == 0 || Pages > SMMU_IOVA_SIZE / SMMU_PAGE_SIZE ||
      (Access != 0 && Access != SMMU_ACCESS_READ && Access != (SMMU_ACCESS_READ | SMMU_ACCESS_WRITE)) ||
      ((Iova | Physical | Domain->TableAddress) & (SMMU_PAGE_SIZE - 1)) != 0 ||
      Iova < SMMU_IOVA_BASE || Iova - SMMU_IOVA_BASE >= SMMU_IOVA_SIZE ||
      Pages > (SMMU_IOVA_SIZE - (Iova - SMMU_IOVA_BASE)) / SMMU_PAGE_SIZE ||
      (Access != 0 && !AddressRangeFits(D->Stage1OutputAddressBits, Physical, Pages)) ||
      !AddressRangeFits(D->AddressBits, Domain->TableAddress, SMMU_TABLE_PAGES))
    return CR_INVALID_PARAMETER;
  if (D->ActlrSettingCount != 0 && D->ActlrSettings == NULL)
    return CR_DEVICE_ERROR;
  for (I = 0; I < D->ActlrSettingCount; ++I) {
    CONST SmmuActlrSetting *Entry = &D->ActlrSettings[I];
    if (((Domain->Sid ^ Entry->Sid) & ~(UINT32)Entry->Mask) != 0) continue;
    if (HasActlr && ExpectedActlr != Entry->Value) return CR_DEVICE_ERROR;
    ExpectedActlr = Entry->Value;
    HasActlr = TRUE;
  }
  if (HasActlr != Domain->OwnsActlr) return CR_DEVICE_ERROR;
  E.Base = D->Base; E.BankBase = Domain->BankBase;
  E.Tables = Domain->TableAddress; E.TableBytes = SMMU_TABLE_PAGES * SMMU_PAGE_SIZE;
  E.Iova = Iova; E.Physical = Physical; E.Bytes = (UINT64)Pages * SMMU_PAGE_SIZE;
  E.Sid = Domain->Sid; E.Bank = Domain->Bank; E.Stream = Domain->Stream; E.Asid = Domain->Asid;
  E.SctlrIgnoreMask = D->SctlrReadbackIgnoreMask; E.Access = Access;
  E.Smr = Read(D, D->Base + SMR(Domain->Stream));
  E.S2cr = Read(D, D->Base + S2CR(Domain->Stream));
  E.Sctlr = Read(D, E.BankBase); E.Actlr = Read(D, E.BankBase + 4);
  E.Cbar = Read(D, D->Base + D->PageSize + Domain->Bank * 4);
  E.Cba2r = Read(D, D->Base + D->PageSize + 0x800 + Domain->Bank * 4);
  E.Tcr = Read(D, E.BankBase + 0x30); E.Tcr2 = Read(D, E.BankBase + 0x10);
  E.Ttbr0 = Read(D, E.BankBase + 0x20) | ((UINT64)Read(D, E.BankBase + 0x24) << 32);
  E.Ttbr1 = Read(D, E.BankBase + 0x28) | ((UINT64)Read(D, E.BankBase + 0x2C) << 32);
  E.Mair0 = Read(D, E.BankBase + 0x38); E.Mair1 = Read(D, E.BankBase + 0x3C);
  if ((Read(D, D->Base) & SCR0_CLIENTPD) != 0 ||
      (HasActlr && E.Actlr != ExpectedActlr) ||
      E.Smr != (VALID | Domain->Sid) || E.S2cr != Domain->Bank ||
      (E.Sctlr & ~E.SctlrIgnoreMask) != (0x1027U & ~E.SctlrIgnoreMask) ||
      E.Cbar != 0x1F300 || E.Cba2r != 1 ||
      E.Tcr != ((1U << 23) | 0x3500U | ((64U - D->TranslationAddressBits) & 0x3FU)) ||
      (E.Tcr2 & TCR2_CONFIG_MASK) != (TCR2_AS | (7U << 15) | D->Stage1OutputAddressEncoding) ||
      E.Ttbr0 != (E.Tables | ((UINT64)Domain->Asid << 48)) ||
      E.Ttbr1 != ((UINT64)Domain->Asid << 48) ||
      E.Mair0 != DomainMair0(Domain) || E.Mair1 != 0)
    return CR_DEVICE_ERROR;
  /* Do not infer a DMA translation from the software grant bitmap. Walk
     both parent levels and all leaf pages, including a partial last page. */
  if (Domain->Tables[0] != ((E.Tables + SMMU_PAGE_SIZE) | 3)) return CR_DEVICE_ERROR;
  First = (UINTN)((Iova - SMMU_IOVA_BASE) / SMMU_PAGE_SIZE);
  Desc = PAGE_DESC | ((Access & SMMU_ACCESS_WRITE) == 0 ? 0x80 : 0);
  for (I = 0; I < Pages; ++I) {
    UINTN LeafTable = (First + I) / 512;
    if (Domain->Tables[512 + (SMMU_IOVA_BASE >> 21) + LeafTable] !=
        ((E.Tables + (LeafTable + 2) * SMMU_PAGE_SIZE) | 3) ||
        Domain->Tables[1024 + First + I] != (Access == 0 ? 0 : ((Physical + I * SMMU_PAGE_SIZE) | Desc)))
      return CR_DEVICE_ERROR;
  }
  *Evidence = E;
  return CR_SUCCESS;
}

CR_STATUS SmmuQueryMappingEvidence(SmmuDomain *Domain, UINT64 Iova,
    UINT64 Physical, UINTN Pages, UINT32 Access, SmmuMappingEvidence *Evidence) {
  if (Access == 0) {
    if (Evidence != NULL) { SmmuMappingEvidence Empty = {0}; *Evidence = Empty; }
    return CR_INVALID_PARAMETER;
  }
  return QueryMappingEvidence(Domain, Iova, Physical, Pages, Access, Evidence);
}

CR_STATUS SmmuQueryRevokedMapping(SmmuDomain *Domain, UINT64 Iova,
    UINTN Pages, SmmuMappingEvidence *Evidence) {
  /* A zero software grant does not establish revocation. Retain the exact
     owned context checks and inspect every requested leaf after completed TLBI. */
  return QueryMappingEvidence(Domain, Iova, 0, Pages, 0, Evidence);
}

CR_STATUS SmmuBlock(SmmuDomain *Domain) {
  CR_STATUS Status;
  log_info("SMMU block begin domain=%p", Domain);
  if (Domain == NULL || Domain->Device == NULL || !Domain->Active || ExitRetained(Domain)) {
    log_err("SMMU block rejected: null/inactive domain");
    return CR_INVALID_PARAMETER;
  }
  if (Domain->Failed || Domain->Device->SyncFailed) {
    log_err("SMMU block rejected SID=0x%x CB=%u domain-failed=%u dev-failed=%u; no TLBI",
            Domain->Sid, Domain->Bank, Domain->Failed, Domain->Device->SyncFailed);
    return CR_DEVICE_ERROR;
  }
  log_info("SMMU block clear SID=0x%x CB=%u IOVA=0x%x bytes=0x%x",
          Domain->Sid, Domain->Bank, SMMU_IOVA_BASE, SMMU_IOVA_SIZE);
  Zero (Domain->Tables + 1024, (SMMU_TABLE_PAGES - 2U) * 512);
  Domain->Device->Io.Publish(Domain->Device->Io.Cookie,
      Domain->Tables + 1024, (SMMU_TABLE_PAGES - 2) * SMMU_PAGE_SIZE);
  Status = Sync(Domain);
  if (CR_ERROR(Status))
    log_err("SMMU block failed SID=0x%x CB=%u status=0x%llx", Domain->Sid, Domain->Bank, (UINT64)Status);
  return Status;
}

CR_STATUS SmmuDetach(SmmuDomain *Domain) {
  SmmuDevice *D;
  UINT64 Cb;
  UINT32 Actual;
  CR_STATUS Status;

  log_info("SMMU owned-domain detach begin domain=%p", Domain);
  if (Domain == NULL || !Domain->Active || Domain->Device == NULL || ExitRetained(Domain)) {
    log_err("SMMU owned-domain detach rejected: null/inactive domain or device");
    return CR_INVALID_PARAMETER;
  }
  if (Domain->Failed || Domain->Device->SyncFailed) {
    log_err("SMMU owned-domain detach rejected SID=0x%x CB=%u", Domain->Sid, Domain->Bank);
    log_err("SMMU detach domain-failed=%u dev-failed=%u; ownership retained", Domain->Failed, Domain->Device->SyncFailed);
    return CR_DEVICE_ERROR;
  }

  D  = Domain->Device;
  Cb = Domain->BankBase;

  log_info("SMMU owned-domain detach invalidate SID=0x%x stream=%u CB=%u", Domain->Sid, Domain->Stream, Domain->Bank);
  log_info("SMMU detach original SMR=0x%08x S2CR=0x%08x", Domain->OriginalSmr, Domain->OriginalS2cr);
  /* Break the stream mapping before touching the context-bank registers. */
  if (!VerifyWrite(Domain, D->Base + SMR(Domain->Stream),
                   Domain->OriginalSmr & ~VALID)) return CR_DEVICE_ERROR;
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }

  log_info("SMMU owned-domain detach disable CB=%u restore-ACTLR=%u", Domain->Bank, Domain->OwnsActlr);
  if (!VerifyWrite(Domain, Cb, 0)) return CR_DEVICE_ERROR;
  if (Domain->OwnsActlr && !VerifyWrite(Domain, Cb + 4, Domain->OriginalActlr))
    return CR_DEVICE_ERROR;
  Status = Sync(Domain);
  if (CR_ERROR(Status)) {
    return Status;
  }

  log_info("SMMU owned-domain detach restore CB=%u CBAR=0x%08x CBA2R=0x%08x",
          Domain->Bank, Domain->OriginalCbar, Domain->OriginalCba2r);
  log_info("SMMU detach TCR2=0x%08x TCR=0x%08x short-format=%u", Domain->OriginalTcr2,
          Domain->OriginalTcr, ShortDescriptor(Domain->OriginalCbar, Domain->OriginalCba2r, Domain->OriginalTcr));
  /* Restore format and TCR before TTBR: TCR2.AS controls ASID[15:8].
   * Public Linux arm_smmu_write_context_bank uses this same ordering. */
  if (!VerifyWrite(Domain, D->Base + D->PageSize + 0x800U + Domain->Bank * 4U,
                   Domain->OriginalCba2r) ||
      !VerifyWrite(Domain, D->Base + D->PageSize + Domain->Bank * 4U,
                   Domain->OriginalCbar) ||
      !VerifyWriteMasked(Domain, Cb + 0x10, Domain->OriginalTcr2,
                         TCR2_CONFIG_MASK) ||
      !VerifyWrite(Domain, Cb + 0x30, Domain->OriginalTcr)) return CR_DEVICE_ERROR;
  if (ShortDescriptor(Domain->OriginalCbar, Domain->OriginalCba2r, Domain->OriginalTcr)) {
    if (!VerifyWrite(Domain, Cb + 0x20, (UINT32)Domain->OriginalTtbr0) ||
        !VerifyWrite(Domain, Cb + 0x28, (UINT32)Domain->OriginalTtbr1)) return CR_DEVICE_ERROR;
  } else if (!VerifyWrite64(Domain, Cb + 0x20, Domain->OriginalTtbr0) ||
             !VerifyWrite64(Domain, Cb + 0x28, Domain->OriginalTtbr1)) return CR_DEVICE_ERROR;
  if (!VerifyWrite(Domain, Cb + 0x38, Domain->OriginalMair0) ||
      !VerifyWrite(Domain, Cb + 0x3C, Domain->OriginalMair1)) return CR_DEVICE_ERROR;

  Write(D, D->Base + S2CR(Domain->Stream), Domain->OriginalS2cr);
  Actual = Read(D, D->Base + S2CR(Domain->Stream));
  if (Actual != Domain->OriginalS2cr) {
    log_err("SMMU detach S2CR mismatch stream=%u expected=0x%08x actual=0x%08x",
            Domain->Stream, Domain->OriginalS2cr, Actual);
    log_err("SMMU detach S2CR fault-retry=%u", (Domain->OriginalS2cr & TRANS_MASK) == (2U << 16));
    /* Qualcomm can ignore FAULT writes and translate BYPASS writes to FAULT.
     * Try that documented encoding only for a failed FAULT restoration, on
     * our already-invalid stream; verify the complete saved value afterward. */
    if ((Domain->OriginalS2cr & TRANS_MASK) == (2U << 16))
      Write(D, D->Base + S2CR(Domain->Stream),
            (Domain->OriginalS2cr & ~TRANS_MASK) | (1U << 16));
    Actual = Read(D, D->Base + S2CR(Domain->Stream));
    if (Actual != Domain->OriginalS2cr) {
      log_err("SMMU detach S2CR rejected SID=0x%x addr=0x%llx", Domain->Sid, D->Base + S2CR(Domain->Stream));
      log_err("SMMU detach expected=0x%08x actual=0x%08x; ownership retained", Domain->OriginalS2cr, Actual);
      LogFaultState(Domain);
      Domain->Failed = TRUE;
      return CR_DEVICE_ERROR;
    }
  }
  if (!VerifyWrite(Domain, D->Base + SMR(Domain->Stream), Domain->OriginalSmr) ||
      !VerifyWrite(Domain, Cb, Domain->OriginalSctlr)) return CR_DEVICE_ERROR;
  /* All our translations were invalidated before changing the bank format.
   * Do not submit our ASID again through the restored firmware context. */
  D->ReservedBanks[Domain->Bank] = FALSE;
  D->ReservedStreams[Domain->Stream] = FALSE;
  D->ReservedAsids[Domain->Asid] = FALSE;
  Domain->Active = FALSE;
  log_info("SMMU owned-domain detach complete SID=0x%x stream=%u CB=%u; reservations released",
          Domain->Sid, Domain->Stream, Domain->Bank);
  return CR_SUCCESS;
}

/* Read-only exclusive route qualification. The caller owns the producer lock;
 * foreign firmware/OS writers must not run concurrently with this transaction. */
STATIC CR_STATUS PhysicalExitRoute(SmmuDomain *Domain, UINT32 Control,
                                   UINT32 ExpectedSctlr) {
  SmmuDevice *D = Domain->Device;
  UINT16 I;
  UINT32 Smr, S2cr, Mask;
  if (Read(D, D->Base) != Control ||
      (Control & (SCR0_CLIENTPD | SCR0_EXIDENABLE)) != 0 ||
      Read(D, Domain->BankBase) != ExpectedSctlr ||
      Read(D, D->Base + D->PageSize + Domain->Bank * 4U) != 0x1F300U ||
      Read(D, D->Base + D->PageSize + 0x800U + Domain->Bank * 4U) != 1U)
    return CR_DEVICE_ERROR;
  for (I = 0; I < D->StreamCount; ++I) {
    Smr = Read(D, D->Base + SMR(I));
    S2cr = Read(D, D->Base + S2CR(I));
    if (I == Domain->Stream) {
      if (Smr != (VALID | Domain->Sid) || S2cr != Domain->Bank)
        return CR_DEVICE_ERROR;
      continue;
    }
    if ((Smr & VALID) == 0) continue;
    Mask = (Smr >> 16) & 0x7FFFU;
    if (((Domain->Sid ^ Smr) & ~Mask & 0xFFFFU) == 0 ||
        ((S2cr & TRANS_MASK) == 0 && (S2cr & 0xFFU) == Domain->Bank))
      return CR_BUSY;
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS ReleaseToPhysicalAtExit(SmmuDomain *Domain, UINT32 Sctlr) {
  SmmuMappingEvidence E;
  SmmuDevice *D;
  CR_STATUS Status;
  UINT32 Control;
  UINTN I;
  if (Domain == NULL || Domain->Device == NULL) return CR_INVALID_PARAMETER;
  D = Domain->Device;
  if (ExitRetained(Domain) || !Domain->Active || Domain->Failed || D->SyncFailed)
    return CR_NOT_READY;
  if (D->ProbeState != SmmuProbeReady || D->PageSize != SMMU_PAGE_SIZE ||
      D->StreamCount == 0 || D->StreamCount > 128 || D->BankCount > 256 ||
      Domain->Bank < D->Stage2BankCount || Domain->Bank + 1U >= D->BankCount ||
      Domain->Sid > 0x7FFFU || D->Io.Write32 == NULL || D->Io.DelayUs == NULL)
    return CR_INVALID_PARAMETER;
  Status = SmmuQueryRevokedMapping(Domain, SMMU_IOVA_BASE,
                                 SMMU_IOVA_SIZE / SMMU_PAGE_SIZE, &E);
  if (Status != CR_SUCCESS) return Status;
  /* The full native domain has only one L1 route and 32 L2 routes. Reject
   * unexpected routes outside the usual grant window as well as live leaves. */
  for (I = 1; I < 512; ++I)
    if (Domain->Tables[I] != 0) return CR_DEVICE_ERROR;
  for (I = 0; I < 512; ++I)
    if ((I < SMMU_IOVA_BASE / 0x200000U ||
         I >= (SMMU_IOVA_BASE + SMMU_IOVA_SIZE) / 0x200000U) &&
        Domain->Tables[512 + I] != 0) return CR_DEVICE_ERROR;
  Control = Read(D, D->Base);
  Status = PhysicalExitRoute(Domain, Control, E.Sctlr);
  if (Status != CR_SUCCESS) return Status;

  /* Maintenance can commit before a timeout is reported. Mark the route
   * terminal before its first write; never retry, detach or free afterward. */
  Domain->ExitState = SmmuDomainExitPending;
  D->PhysicalExitBanks[Domain->Bank] = TRUE;
  Status = Sync(Domain);
  if (Status != CR_SUCCESS) return Status;
  Status = PhysicalExitRoute(Domain, Control, E.Sctlr);
  if (Status != CR_SUCCESS) {
    Domain->Failed = TRUE;
    return Status;
  }
  /* Linux qcom_smmu_cfg_probe()/qcom_smmu_write_s2cr() demonstrate physical
   * bypass through SCTLR.M=0 on a S1_TRANS_S2_BYPASS bank while S2CR remains
   * TRANS. Reuse only our already-qualified bank, never Linux's global probe
   * or a firmware-owned final bypass bank. CBAR and the stream stay intact. */
  if (!VerifyWrite(Domain, Domain->BankBase, Sctlr)) return CR_DEVICE_ERROR;
  Status = Sync(Domain);
  if (Status != CR_SUCCESS) return Status;
  Status = PhysicalExitRoute(Domain, Control, Sctlr);
  if (Status != CR_SUCCESS) {
    Domain->Failed = TRUE;
    return Status;
  }
  Domain->ExitState = SmmuDomainExitPhysical;
  log_info("SMMU physical exit SID=0x%x stream=%u CB=%u SCTLR=0x%08x; terminal ownership retained",
           Domain->Sid, Domain->Stream, Domain->Bank, Sctlr);
  return CR_SUCCESS;
}

CR_STATUS SmmuReleaseToPhysicalAtExit(SmmuDomain *Domain) {
  return ReleaseToPhysicalAtExit(Domain, 0);
}

CR_STATUS SmmuReleaseToCoherentPhysicalAtExit(SmmuDomain *Domain) {
  /* Qualcomm SM8450 Android arm-smmu.c (e93f69dc83e6),
   * arm_smmu_init_context_bank(): dma-coherent clients use WACFG=WA (2),
   * RACFG=RA (2), SHCFG=OSH (1), MTCFG=1 and MEMATTR=OISH_WB_CACHE (15).
   * The source explicitly restricts these overrides to bypass (SCTLR.M=0).
   * Do not enable translation, fault interrupts or unrelated control bits.
   * Both the immediate and final readback must match every bit, including
   * these attributes; a dropped override is terminal uncertainty.
   */
  return ReleaseToPhysicalAtExit(Domain,
      (2U << 26) | (2U << 24) | (1U << 22) | (1U << 20) | (15U << 16));
}
